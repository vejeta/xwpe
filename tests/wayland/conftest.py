"""Pytest fixtures for the xwpe NATIVE WAYLAND GUI test-suite.

This is the Wayland counterpart to tests/x11/.  The X11 suite proves the Xlib
backend; this one proves the native wl_surface backend (we_wayland.c) -- the
same SCREENCELL grid and Cairo renderer, but driven over wl_keyboard/wl_pointer
instead of Xlib, so a regression in the Wayland input/render path is caught.

Pipeline
--------
  Xvfb                    : headless X server (a real X display, no hardware)
  weston (x11-backend)    : a real Wayland compositor whose seat gets its
                            keyboard/pointer FROM the X server -- so xdotool's
                            XTEST events reach the wl client as wl_keyboard /
                            wl_pointer events.  (weston's headless backend has
                            NO input devices, hence the X11 backend on Xvfb.)
  xdotool                 : synthetic keyboard/mouse (same as the X11 suite)
  XWPE_WL_DUMP            : xwpe mirrors every painted frame to a PPM, giving an
                            exact, race-free screenshot of the wl_shm buffer with
                            no compositor screenshooter (weston-x11 renders via
                            GL, which `xwd -root` cannot read).
  Pillow                  : load the PPM and assert on pixels

Most tests assert on the file written to disk (ground truth); a few assert on
the dumped pixels.  If any binary is missing the whole module skips cleanly.
Set XWPE_BIN to point at the xwpe binary (defaults to ../../xwpe).
"""
import os
import shutil
import subprocess
import time

import pytest

REQUIRED_BINS = ["Xvfb", "xdotool", "weston"]
_MISSING = [b for b in REQUIRED_BINS if shutil.which(b) is None]
try:
    import PIL  # noqa: F401
except ImportError:
    _MISSING.append("python3-pil (Pillow)")
# weston needs its x11-backend module; if absent the compositor cannot start.
_WESTON_X11 = next(
    (p for p in (
        "/usr/lib/x86_64-linux-gnu/libweston-15/x11-backend.so",
        "/usr/lib/weston/x11-backend.so",
    ) if os.path.exists(p)),
    None)
if shutil.which("weston") and _WESTON_X11 is None:
    # Probe more generally before giving up.
    import glob
    if not glob.glob("/usr/lib/**/libweston-*/x11-backend.so", recursive=True):
        _MISSING.append("weston x11-backend.so")


def pytest_collection_modifyitems(config, items):
    if not _MISSING:
        return
    here = os.path.dirname(os.path.abspath(__file__))
    skip = pytest.mark.skip(
        reason="Wayland GUI test dependencies missing: " + ", ".join(_MISSING))
    for item in items:
        if str(getattr(item, "fspath", "")).startswith(here):
            item.add_marker(skip)


HERE = os.path.dirname(os.path.abspath(__file__))
XWPE_BIN = os.environ.get("XWPE_BIN") or os.path.normpath(os.path.join(HERE, "..", "..", "xwpe"))
TREE = os.path.normpath(os.path.join(HERE, "..", ".."))
DISPLAY = os.environ.get("XWPE_TEST_WL_DISPLAY", ":91")
WL_SOCKET = "wl-xwpe-test"
SCREEN = "1024x768x24"

# A loaded CI runner is slower; stretch every settle without slowing local runs.
_WAIT_SCALE = float(os.environ.get("XWPE_TEST_WAIT_SCALE", "1") or 1)
_real_sleep = time.sleep


def _sleep(seconds):
    _real_sleep(seconds * _WAIT_SCALE)


def _spawn(cmd, **kw):
    return subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, **kw)


def _xdo(*args, env=None):
    subprocess.run(["xdotool", *args], env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def changed_pixels(img_a, img_b, thresh=40):
    """Number of pixels that differ by more than `thresh` (0..255 grey)."""
    from PIL import ImageChops
    diff = ImageChops.difference(img_a, img_b).convert("L")
    return diff.point(lambda p: 255 if p > thresh else 0).histogram()[255]


def _load_ppm(path):
    """Load a binary PPM (what xwpe writes via XWPE_WL_DUMP) as a Pillow RGB."""
    from PIL import Image
    with open(path, "rb") as fh:
        data = fh.read()
    assert data[:2] == b"P6", "not a P6 PPM"
    i, nums, tok = 2, [], b""
    while len(nums) < 3:
        c = data[i:i + 1]
        i += 1
        if c.isspace():
            if tok:
                nums.append(int(tok))
                tok = b""
        else:
            tok += c
    w, h, _ = nums
    return Image.frombytes("RGB", (w, h), data[i:])


class WaylandSession:
    """A native-Wayland xwpe instance under headless weston, with the same
    helper API as the X11 suite's XwpeSession (key/click/type/menu/save/
    screenshot/saved_text), so test bodies can be shared."""

    def __init__(self, proc, env, wid, dump):
        self.proc = proc
        self.env = env
        self.wid = wid          # the weston X11 window (xdotool target)
        self.dump = dump        # XWPE_WL_DUMP path (continuously refreshed PPM)

    def _key(self, env_display, k):
        _xdo("key", "--window", self.wid, "--clearmodifiers", k,
             env={**os.environ, "DISPLAY": DISPLAY})

    def focus(self):
        _xdo("windowfocus", self.wid, env={**os.environ, "DISPLAY": DISPLAY})
        _xdo("windowactivate", self.wid, env={**os.environ, "DISPLAY": DISPLAY})
        _sleep(0.3)

    def key(self, *keys, delay=0.5):
        for k in keys:
            self._key(DISPLAY, k)
            _sleep(delay)
        return self

    def type(self, text, delay=0.4):
        _xdo("type", "--window", self.wid, "--clearmodifiers", text,
             env={**os.environ, "DISPLAY": DISPLAY})
        _sleep(delay)
        return self

    def click(self, px, py, delay=0.5):
        d = {**os.environ, "DISPLAY": DISPLAY}
        _xdo("mousemove", "--window", self.wid, str(px), str(py), env=d)
        _sleep(0.15)
        _xdo("mousemove", "--window", self.wid, str(px + 1), str(py), env=d)  # real motion
        _sleep(0.1)
        _xdo("click", "1", env=d)
        _sleep(delay)
        return self

    def menu(self, alt_letter, item, delay=0.5):
        """Open a top-level menu (Alt-<letter>) and pick an item by its key."""
        self.key("alt+" + alt_letter, delay=delay)
        self.key(item, delay=delay)
        return self

    def save(self, delay=0.6):
        """File -> Save (Alt-F, s)."""
        self.key("alt+f", delay=delay)
        self.key("s", delay=delay)
        return self

    def saved_text(self):
        with open(self.srcfile) as fh:
            return fh.read()

    def screenshot(self):
        """The current wl_shm frame xwpe last painted, as a Pillow RGB image."""
        return _load_ppm(self.dump)


@pytest.fixture(scope="session")
def wlserver():
    """Start Xvfb + weston (x11-backend) once for the whole test session."""
    lock = "/tmp/.X%s-lock" % DISPLAY.lstrip(":")
    if os.path.exists(lock):
        try:
            os.unlink(lock)
        except OSError:
            pass
    xvfb = _spawn(["Xvfb", DISPLAY, "-screen", "0", SCREEN])
    _sleep(2.0)
    runtime = os.environ.get("XDG_RUNTIME_DIR") or "/run/user/%d" % os.getuid()
    sockpath = os.path.join(runtime, WL_SOCKET)
    for p in (sockpath, sockpath + ".lock"):
        try:
            os.unlink(p)
        except OSError:
            pass
    # kiosk-shell fullscreens the single client at the output's (0,0) with no
    # panel, so the xwpe surface origin == the weston window origin: an xdotool
    # click relative to that window lands on the matching surface cell.  (The
    # default desktop-shell centres/offsets the surface, breaking click-coord
    # tests.)  Keyboard focus works under kiosk once the fk_w_mouse startup spin
    # is fixed -- the earlier "kiosk breaks input" was that bug, not the shell.
    weston = _spawn(
        ["weston", "--backend=x11-backend.so", "--shell=kiosk-shell.so",
         "--width=1024", "--height=768", "--socket=" + WL_SOCKET, "--idle-time=0"],
        env={**os.environ, "DISPLAY": DISPLAY, "XDG_RUNTIME_DIR": runtime})
    # Wait for the wl socket to appear.
    for _ in range(60):
        if os.path.exists(sockpath):
            break
        _real_sleep(0.2)
    assert os.path.exists(sockpath), "weston wl socket did not appear"
    _sleep(1.0)
    yield runtime
    weston.terminate()
    xvfb.terminate()


@pytest.fixture
def xwpe(wlserver, tmp_path):
    """Launch a fresh NATIVE-WAYLAND xwpe editing a small C file; tear down after."""
    assert os.path.exists(XWPE_BIN), "xwpe binary not found at %s (set XWPE_BIN)" % XWPE_BIN
    runtime = wlserver
    src = tmp_path / "t.c"
    src.write_text(
        "int main(){\n"
        "  int a = 1;\n"
        "  int b = 2;\n"
        "  int c = 3;\n"
        "  int d = 4;\n"
        "  int e = 5;\n"
        "  int f = 6;\n"
        "  return 0;\n"
        "}\n")
    dump = str(tmp_path / "frame.ppm")
    env = {**os.environ,
           "WAYLAND_DISPLAY": WL_SOCKET,
           "XDG_RUNTIME_DIR": runtime,
           "XWPE_BACKEND": "wayland",
           "XWPE_LIB": TREE,
           "XWPE_LSP_NO_EAGER": "1",
           "XWPE_FONT_SIZE": "10",
           # Match the X11 suite's forced 1024x768 geometry so the centered
           # dialogs land at the same window fraction (coordinate-based pixel
           # scans -- e.g. the popup close [X] -- are calibrated for it).
           "XWPE_WL_WIDTH": "1024",
           "XWPE_WL_HEIGHT": "768",
           "XWPE_WL_DUMP": dump,
           "HOME": str(tmp_path)}
    proc = _spawn([XWPE_BIN, str(src)], env=env, cwd=str(tmp_path))
    # Wait for the weston X window (the xdotool target) and the first frame dump.
    wid = None
    for _ in range(60):
        out = subprocess.run(["xdotool", "search", "--class", "weston"],
                             env={**os.environ, "DISPLAY": DISPLAY},
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        ids = out.stdout.decode().split()
        if ids and os.path.exists(dump):
            wid = ids[-1]
            break
        _real_sleep(0.25)
    assert wid, "weston window / first xwpe frame did not appear"
    _sleep(1.0)
    session = WaylandSession(proc, env, wid, dump)
    session.srcfile = str(src)
    session.focus()
    yield session
    try:
        proc.terminate()
        proc.wait(timeout=5)
    except Exception:
        proc.kill()
