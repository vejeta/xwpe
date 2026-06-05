"""Pytest fixtures for the xwpe (X11/Xft) GUI test-suite.

These tests are the X11 counterpart to the pyte tests that cover wpe
(ncurses).  pyte can only emulate a VT100 text terminal, so it cannot
exercise xwpe's real Xft rendering or its X11 keyboard/mouse path.  We
therefore drive a real xwpe under a headless X server and assert on
screenshots.

Pipeline
--------
  Xvfb  : headless X server (a real X display, no hardware)
  matchbox-window-manager : a tiny WM.  REQUIRED -- without a window
        manager xwpe's X11 size handling oscillates into an
        unrecoverable resize feedback loop under bare Xvfb (xwpe never
        calls XResizeWindow itself; a WM owns the geometry).
  xdotool : synthetic keyboard/mouse via XTEST (real input events)
  xwd | convert (ImageMagick) : capture the root window to PNG
  Pillow  : load the PNG and assert on pixels

If any binary is missing the whole module skips with a clear reason.
Set XWPE_BIN to point at the xwpe binary (defaults to ../../xwpe).
"""
import os
import shutil
import subprocess
import time

import pytest

REQUIRED_BINS = ["Xvfb", "matchbox-window-manager", "xdotool", "xwd", "convert"]
_MISSING = [b for b in REQUIRED_BINS if shutil.which(b) is None]

pytestmark = pytest.mark.skipif(
    bool(_MISSING),
    reason="X11 GUI test dependencies missing: " + ", ".join(_MISSING),
)

HERE = os.path.dirname(os.path.abspath(__file__))
XWPE_BIN = os.environ.get("XWPE_BIN") or os.path.normpath(os.path.join(HERE, "..", "..", "xwpe"))
DISPLAY = os.environ.get("XWPE_TEST_DISPLAY", ":88")
SCREEN = "1024x768x24"
WINDOW_NAME = "Programming Environment"


def _spawn(cmd, **kw):
    return subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, **kw)


def _xdo(*args):
    subprocess.run(["xdotool", *args], env={**os.environ, "DISPLAY": DISPLAY},
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


class XwpeSession:
    """A running xwpe instance under the headless display, with helpers
    to send input and screenshot the result."""

    def __init__(self, proc, win):
        self.proc = proc
        self.win = win

    def focus(self):
        _xdo("windowfocus", self.win)
        time.sleep(0.3)

    def key(self, *keys, delay=0.5):
        for k in keys:
            _xdo("key", "--window", self.win, "--clearmodifiers", k)
            time.sleep(delay)

    def click(self, px, py, delay=0.5):
        _xdo("mousemove", "--window", self.win, str(px), str(py))
        time.sleep(0.2)
        _xdo("click", "1")
        time.sleep(delay)

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
        """The edited file as written to disk (call save() first)."""
        with open(self.srcfile) as fh:
            return fh.read()

    def screenshot(self):
        """Return the root window as a Pillow Image (RGB)."""
        from PIL import Image
        import io
        xwd = subprocess.run(["xwd", "-root", "-silent"],
                             env={**os.environ, "DISPLAY": DISPLAY},
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        png = subprocess.run(["convert", "xwd:-", "png:-"],
                            input=xwd.stdout, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL)
        return Image.open(io.BytesIO(png.stdout)).convert("RGB")


@pytest.fixture(scope="session")
def xserver():
    """Start Xvfb + matchbox once for the whole test session."""
    lock = "/tmp/.X%s-lock" % DISPLAY.lstrip(":")
    if os.path.exists(lock):
        try:
            os.unlink(lock)
        except OSError:
            pass
    xvfb = _spawn(["Xvfb", DISPLAY, "-screen", "0", SCREEN])
    time.sleep(2.0)
    wm = _spawn(["matchbox-window-manager", "-use_titlebar", "no"],
                env={**os.environ, "DISPLAY": DISPLAY})
    time.sleep(1.5)
    yield DISPLAY
    wm.terminate()
    xvfb.terminate()


@pytest.fixture
def xwpe(xserver, tmp_path):
    """Launch a fresh xwpe editing a small C file; tear it down after."""
    assert os.path.exists(XWPE_BIN), "xwpe binary not found at %s (set XWPE_BIN)" % XWPE_BIN
    src = tmp_path / "t.c"
    # Several lines so the block-marking tests have room to mark and re-mark.
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
    proc = _spawn([XWPE_BIN, str(src)],
                  env={**os.environ, "DISPLAY": DISPLAY, "HOME": str(tmp_path)},
                  cwd=str(tmp_path))
    # Wait for the window to appear.
    win = None
    for _ in range(40):
        out = subprocess.run(["xdotool", "search", "--name", WINDOW_NAME],
                             env={**os.environ, "DISPLAY": DISPLAY},
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        ids = out.stdout.decode().split()
        if ids:
            win = ids[0]
            break
        time.sleep(0.25)
    assert win, "xwpe window did not appear"
    time.sleep(1.0)
    session = XwpeSession(proc, win)
    session.srcfile = str(src)
    session.focus()
    yield session
    try:
        proc.terminate()
        proc.wait(timeout=5)
    except Exception:
        proc.kill()
