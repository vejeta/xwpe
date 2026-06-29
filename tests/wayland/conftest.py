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
import sys
import time

import pytest

REQUIRED_BINS = ["Xvfb", "xdotool", "weston"]
_MISSING = [b for b in REQUIRED_BINS if shutil.which(b) is None]
try:
    import PIL  # noqa: F401
except ImportError:
    _MISSING.append("python3-pil (Pillow)")
# weston needs its x11-backend (to get input from Xvfb) and kiosk-shell (to
# fullscreen the app at (0,0) for exact click coords).  If either module is
# absent the compositor can't give the suite what it needs -> skip cleanly.
import glob as _glob


def _weston_has(module):
    """True if weston ships `module` in a standard (non-recursive) location.
    Bounded globs only -- a recursive /usr/lib/** walk is pathologically slow."""
    if not shutil.which("weston"):
        return False
    for pat in ("/usr/lib/*/libweston-*/" + module,   # libweston-15 multiarch dir
                "/usr/lib/libweston-*/" + module,
                "/usr/lib/*/weston/" + module,         # Debian multiarch weston dir
                "/usr/lib/weston/" + module):
        if _glob.glob(pat):
            return True
    return False


if shutil.which("weston"):
    if not _weston_has("x11-backend.so"):
        _MISSING.append("weston x11-backend.so")
    if not _weston_has("kiosk-shell.so"):
        _MISSING.append("weston kiosk-shell.so")


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

import hashlib

# A loaded CI runner is slower; stretch every settle without slowing local runs.
_WAIT_SCALE = float(os.environ.get("XWPE_TEST_WAIT_SCALE", "1") or 1)
_real_sleep = time.sleep


def _sleep(seconds):
    _real_sleep(seconds * _WAIT_SCALE)


def _frame_digest(path):
    """Content hash of the current XWPE_WL_DUMP frame, or None if unreadable."""
    try:
        with open(path, "rb") as fh:
            return hashlib.md5(fh.read()).digest()
    except OSError:
        return None


def wait_frame_quiet(dump, floor, quiet=0.3, cap=3.0):
    """Adaptive settle: let xwpe begin reacting (the `floor`), then return once
    the painted frame has stopped changing for `quiet` seconds, capped at `cap`.

    This replaces a fixed post-key sleep.  On a CPU-starved CI runner xwpe paints
    late, so a flat wait samples the screen before the paint ("0 px" / "dialog
    did not appear") or fires the next key while the editor is mid-update (which
    can drive it into an unexpected state); waiting for the frame to settle makes
    the harness adapt to however long the paint actually takes, while a fast
    machine still returns promptly.  All durations scale with WAIT_SCALE."""
    _real_sleep(floor * _WAIT_SCALE)
    deadline = time.monotonic() + cap * _WAIT_SCALE
    quiet_need = quiet * _WAIT_SCALE
    last = _frame_digest(dump)
    last_change = time.monotonic()
    while time.monotonic() < deadline:
        _real_sleep(0.1 * _WAIT_SCALE)
        sig = _frame_digest(dump)
        if sig != last:
            last, last_change = sig, time.monotonic()
        elif time.monotonic() - last_change >= quiet_need:
            return


# Optional diagnostics dir: when set, capture weston and per-test xwpe stderr so
# a CI failure can be explained (e.g. a real crash vs a slow-runner mistime).
_LOGDIR = os.environ.get("XWPE_WL_LOGDIR")


def _logfile(name):
    if not _LOGDIR:
        return subprocess.DEVNULL
    os.makedirs(_LOGDIR, exist_ok=True)
    return open(os.path.join(_LOGDIR, name), "ab")


def _spawn(cmd, stderr=subprocess.DEVNULL, **kw):
    return subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=stderr, **kw)


def _xdo(*args, env=None):
    subprocess.run(["xdotool", *args], env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


_FKEY_KEYCODE = {}


def _fkey_keycodes():
    """Map F1..F12 -> the live X keycode by parsing `xmodmap -pke`.

    `xdotool key F3` resolves the keysym to a keycode PLUS the modifiers to reach
    its level; on xkeyboard-config >= 2.46 that resolver (xdotool #491) adds a
    phantom Alt, so xwpe sees Alt+F3 (AF3), a different key -- the bare
    accelerator looks dead.  Even under weston's x11-backend the injection goes
    through the Xvfb keymap, so the bug applies; injecting the raw keycode skips
    the resolver and lands the true function key.  Cached after first lookup."""
    if _FKEY_KEYCODE:
        return _FKEY_KEYCODE
    out = subprocess.run(["xmodmap", "-pke"],
                         env={**os.environ, "DISPLAY": DISPLAY},
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout.decode()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[0] == "keycode" and parts[2] == "=":
            sym = parts[3]
            if len(sym) >= 2 and sym[0] in "Ff" and sym[1:].isdigit():
                _FKEY_KEYCODE.setdefault(sym.upper(), parts[1])
    return _FKEY_KEYCODE


def _to_keycodes(keystroke):
    """Rewrite an xdotool keystroke so any function-key token is its keycode
    (bare 'F3' or combos like 'ctrl+F9'); other tokens pass through."""
    fk = _fkey_keycodes()
    return "+".join(fk.get(tok.upper(), tok) for tok in keystroke.split("+"))


def changed_pixels(img_a, img_b, thresh=40):
    """Number of pixels that differ by more than `thresh` (0..255 grey)."""
    from PIL import ImageChops
    diff = ImageChops.difference(img_a, img_b).convert("L")
    return diff.point(lambda p: 255 if p > thresh else 0).histogram()[255]


def incoherence(reason):
    """Mark a test that asserts behaviour the editor does not currently honour
    (a backend divergence found during coverage).  Recorded as a non-strict
    xfail, matching the X11 suite's helper of the same name."""
    return pytest.mark.xfail(reason="INCOHERENCE: " + reason, strict=False)


def run_x11_module(caller_name, modname):
    """Execute the body of tests/x11/<modname>.py inside the CALLER module's
    namespace, compiled under the CALLER's filename so pytest collects its
    test_* functions HERE -- bound to this directory's native-Wayland `xwpe`
    fixture.  Zero duplication: the X11 module is the single source of truth, so
    the two suites can never drift; only the fixture (Xlib vs wl_surface)
    differs.  (Importing the X11 module instead fails: pytest keys collection
    off the function's code filename, which would still be the x11 file.)  The
    x11 dir is never put on sys.path, so the body's `from conftest import ...`
    resolves to THIS conftest at runtime."""
    caller = sys.modules[caller_name]
    x11_path = os.path.normpath(
        os.path.join(os.path.dirname(__file__), "..", "x11", modname + ".py"))
    with open(x11_path) as fh:
        src = fh.read()
    code = compile(src, caller.__file__, "exec")
    exec(code, caller.__dict__)


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
        _xdo("key", "--window", self.wid, "--clearmodifiers", _to_keycodes(k),
             env={**os.environ, "DISPLAY": DISPLAY})

    def focus(self):
        _xdo("windowfocus", self.wid, env={**os.environ, "DISPLAY": DISPLAY})
        _xdo("windowactivate", self.wid, env={**os.environ, "DISPLAY": DISPLAY})
        _sleep(0.3)

    def key(self, *keys, delay=0.5):
        for k in keys:
            self._key(DISPLAY, k)
            wait_frame_quiet(self.dump, floor=delay)  # wait for the paint to settle
        return self

    def type(self, text, delay=0.4):
        _xdo("type", "--window", self.wid, "--clearmodifiers", text,
             env={**os.environ, "DISPLAY": DISPLAY})
        wait_frame_quiet(self.dump, floor=delay)
        return self

    def click(self, px, py, delay=0.5):
        d = {**os.environ, "DISPLAY": DISPLAY}
        _xdo("mousemove", "--window", self.wid, str(px), str(py), env=d)
        _sleep(0.15)
        _xdo("mousemove", "--window", self.wid, str(px + 1), str(py), env=d)  # real motion
        _sleep(0.1)
        _xdo("click", "1", env=d)
        wait_frame_quiet(self.dump, floor=delay)
        return self

    def menu(self, alt_letter, item, delay=0.5):
        """Open a top-level menu (Alt-<letter>) and pick an item by its key."""
        self.key("alt+" + alt_letter, delay=delay)
        self.key(item, delay=delay)
        return self

    def esc_menu(self, menu_key, item, delay=0.5):
        """Open the leftmost '#' (System) menu and pick an item by its letter.

        The '#' menu has no Alt-<letter> hotkey, so a GUI user clicks its title
        at the far left of the menu bar; do the same (kiosk-shell puts the
        surface at the window origin, so this pixel maps to the '#' cell), then
        press the item's letter.  (menu_key kept for call-site readability.)"""
        self.click(15, 8, delay=delay)      # the '#' title, far left of the bar
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
        env={**os.environ, "DISPLAY": DISPLAY, "XDG_RUNTIME_DIR": runtime},
        stderr=_logfile("weston.log"))
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
def xwpe(wlserver, tmp_path, request):
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
    proc = _spawn([XWPE_BIN, str(src)], env=env, cwd=str(tmp_path),
                  stderr=_logfile("xwpe-%s.log" % request.node.name))
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
    # Fully reap this xwpe AND let weston process its surface-destroy before the
    # next test spawns a client.  Otherwise, under load, the dying client's
    # surface can still hold the seat focus when the next xwpe starts, so its
    # keystrokes land on the wrong (departing) surface -- the cross-test flake
    # that makes an unrelated later test "die".
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except Exception:
        proc.kill()
        try:
            proc.wait(timeout=5)
        except Exception:
            pass
    _sleep(0.4)
