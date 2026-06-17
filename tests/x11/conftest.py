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


# A loaded CI runner is 3-5x slower than a dev box, so the fixed settle times
# between a synthetic keystroke and the screenshot flake ("screen barely
# changed" -- the dialog had not rendered yet). XWPE_TEST_WAIT_SCALE stretches
# every settle without slowing the local run (default 1.0; the Debian
# autopkgtest sets 3). _sleep() replaces _sleep() throughout this module.
_WAIT_SCALE = float(os.environ.get("XWPE_TEST_WAIT_SCALE", "1") or 1)
_real_sleep = time.sleep


def _sleep(seconds):
    _real_sleep(seconds * _WAIT_SCALE)


def _xdo(*args):
    subprocess.run(["xdotool", *args], env={**os.environ, "DISPLAY": DISPLAY},
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


_FKEY_KEYCODE = {}


def _fkey_keycodes():
    """Map F1..F12 -> the live X keycode by parsing `xmodmap -pke`.

    WHY by keycode and not by name: xdotool resolves a keysym (e.g. "F3") to a
    keycode PLUS the modifiers needed to reach the level that keysym sits on.
    On xkeyboard-config >= 2.46 that resolver is buggy (xdotool issue #491) and
    emits the F-key with a phantom Alt modifier -- so `xdotool key F3` actually
    delivers Alt+F3.  xwpe decodes Alt+F3 as AF3, a DIFFERENT key from F3, so
    the bare accelerator looks dead.  Letters are unaffected (explicit level-1
    mapping).  Injecting the raw keycode skips the resolver entirely and lands
    the true function key.  Cached after the first lookup."""
    if _FKEY_KEYCODE:
        return _FKEY_KEYCODE
    out = subprocess.run(["xmodmap", "-pke"],
                         env={**os.environ, "DISPLAY": DISPLAY},
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout.decode()
    for line in out.splitlines():
        parts = line.split()
        # "keycode  69 = F3 F3 F3 F3 F3 F3 XF86Switch_VT_3"
        if len(parts) >= 4 and parts[0] == "keycode" and parts[2] == "=":
            sym = parts[3]
            if len(sym) >= 2 and sym[0] in "Ff" and sym[1:].isdigit():
                _FKEY_KEYCODE.setdefault(sym.upper(), parts[1])
    return _FKEY_KEYCODE


def _to_keycodes(keystroke):
    """Rewrite an xdotool keystroke so any function-key token is the keycode.

    Handles bare ("F3") and modifier combos ("ctrl+F9", "shift+F3"): each "+"-
    separated token that names a function key is swapped for its keycode, other
    tokens (ctrl/alt/shift/letters) pass through untouched.  See
    _fkey_keycodes() for why."""
    fk = _fkey_keycodes()
    return "+".join(fk.get(tok.upper(), tok) for tok in keystroke.split("+"))


def incoherence(reason):
    """Mark an X11 test that asserts behaviour we expect but which xwpe does
    not currently honour (a wpe-vs-xwpe divergence found during coverage).
    Recorded as xfail; `pytest -rxX` lists every INCOHERENCE for review."""
    return pytest.mark.xfail(reason="INCOHERENCE: " + reason, strict=False)


def changed_pixels(img_a, img_b, thresh=40):
    """Number of pixels that differ by more than `thresh` (0..255 grey).
    Used to assert that an action opened/changed a window on screen."""
    from PIL import ImageChops
    diff = ImageChops.difference(img_a, img_b).convert("L")
    return diff.point(lambda p: 255 if p > thresh else 0).histogram()[255]


class XwpeSession:
    """A running xwpe instance under the headless display, with helpers
    to send input and screenshot the result."""

    def __init__(self, proc, win):
        self.proc = proc
        self.win = win

    def focus(self):
        _xdo("windowfocus", self.win)
        _sleep(0.3)

    def key(self, *keys, delay=0.5):
        for k in keys:
            _xdo("key", "--window", self.win, "--clearmodifiers", _to_keycodes(k))
            _sleep(delay)

    def click(self, px, py, delay=0.5):
        _xdo("mousemove", "--window", self.win, str(px), str(py))
        _sleep(0.2)
        _xdo("click", "1")
        _sleep(delay)

    def drag(self, x1, y1, x2, y2, steps=12, delay=0.6):
        """Press button 1 at (x1,y1), move to (x2,y2) in steps, release.
        Drives the real X11 drag path (ButtonPress -> MotionNotify* ->
        ButtonRelease) -- e.g. a scrollbar-thumb drag."""
        _xdo("mousemove", "--window", self.win, str(x1), str(y1))
        _sleep(0.15)
        _xdo("mousedown", "1")
        _sleep(0.1)
        for i in range(1, steps + 1):
            ix = x1 + (x2 - x1) * i // steps
            iy = y1 + (y2 - y1) * i // steps
            _xdo("mousemove", "--window", self.win, str(ix), str(iy))
            _sleep(0.03)
        _sleep(0.1)
        _xdo("mouseup", "1")
        _sleep(delay)

    def wheel(self, direction, count=3, px=None, py=None, delay=0.4):
        """Spin the mouse wheel (button 4 = up, 5 = down) `count` times, over
        (px,py) if given.  Drives WPE_SCROLL_UP/DOWN."""
        button = "4" if direction == "up" else "5"
        if px is not None:
            _xdo("mousemove", "--window", self.win, str(px), str(py))
            _sleep(0.1)
        for _ in range(count):
            _xdo("click", button)
            _sleep(0.1)
        _sleep(delay)

    def type(self, text, delay=0.4):
        """Type a literal string (into a focused text field) via XTEST."""
        _xdo("type", "--window", self.win, "--clearmodifiers", text)
        _sleep(delay)
        return self

    def menu(self, alt_letter, item, delay=0.5):
        """Open a top-level menu (Alt-<letter>) and pick an item by its key."""
        self.key("alt+" + alt_letter, delay=delay)
        self.key(item, delay=delay)
        return self

    def esc_menu(self, menu_key, item, delay=0.5):
        """Open the leftmost '#' (System) menu and pick an item by its letter.

        The '#' menu has no Alt-<letter> hotkey.  In a real terminal it is
        reached with xwpe's ESC/Alt meta-prefix (Alt-# == the bytes ESC '#'),
        but under X11 there is no such prefix: ESC is a standalone keysym and
        the following '#' is simply typed into the buffer, while a synthetic
        Alt-# is unreliable (xdotool composes '#' as Shift+3, so the modifier
        state is Alt+Shift, not the bare Alt the menu code matches).  Open it
        the way a GUI user does instead -- click its title at the far left of
        the menu bar -- then press the item's letter.  (menu_key is kept for
        call-site readability; the '#' menu is the only caller.)"""
        self.click(15, 8, delay=delay)      # the '#' title, far left of the bar
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
    _sleep(2.0)
    # Use an empty kbdconfig: matchbox's default grabs <Alt>n/p/c/d/m, <Alt>Tab
    # etc. at the root, which are xwpe's own dialog/menu hotkeys -- the WM would
    # swallow them and fake an "Alt-hotkey broken" divergence.  See the file.
    kbd = os.path.join(HERE, "matchbox-kbdconfig")
    wm = _spawn(["matchbox-window-manager", "-use_titlebar", "no",
                 "-kbdconfig", kbd],
                env={**os.environ, "DISPLAY": DISPLAY})
    _sleep(1.5)
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
    # XWPE_LSP_NO_EAGER: these are chrome/dialog/menu tests that compare
    # full-screen pixel deltas.  If a language server (clangd for the .c file)
    # is installed on the box, its eager start pops an asynchronous "Messages"
    # window ("Starting language server... ready.") at some point after the
    # baseline screenshot -- ~17k changed pixels that make a closed dialog look
    # un-closed and add a phantom scrollbar'd window to the overlap tests.  None
    # of the X11 tests exercise LSP, so opt out of the eager boot for a
    # deterministic screen (the opt-out the editor already exposes for exactly
    # this -- see e_lsp_open_eager).
    proc = _spawn([XWPE_BIN, str(src)],
                  env={**os.environ, "DISPLAY": DISPLAY, "HOME": str(tmp_path),
                       "XWPE_LSP_NO_EAGER": "1"},
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
        _sleep(0.25)
    assert win, "xwpe window did not appear"
    _sleep(1.0)
    session = XwpeSession(proc, win)
    session.srcfile = str(src)
    session.focus()
    yield session
    try:
        proc.terminate()
        proc.wait(timeout=5)
    except Exception:
        proc.kill()
