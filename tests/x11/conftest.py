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
import tempfile
import time

import pytest

# The window manager and the XWD-to-PNG converter are platform-dependent:
# matchbox + convert (ImageMagick 6) on Debian/Ubuntu CI, twm + magick +
# xwdtopnm on macOS (XQuartz ships no matchbox; IM7 dropped the legacy `convert`
# binary and its XWD codec).  Probe what's installed and pick the first chain
# that works; the suite skips with a precise reason if nothing matches.
_WM_BIN = next((b for b in ("matchbox-window-manager", "twm")
                if shutil.which(b)), None)
# Prefer `magick` when both are present: ImageMagick 7 keeps a `convert` shim
# but its XWD decode delegate is gone, so `convert xwd:-` errors out -- only
# `magick` (via xwdtopnm) decodes XWD on IM7.  IM6 ships `convert` only and
# decodes XWD directly.
_CONV_BIN = next((b for b in ("magick", "convert") if shutil.which(b)), None)
_XWDTOPNM = shutil.which("xwdtopnm")  # needed only when convert is absent

REQUIRED_BINS = ["Xvfb", "xdotool", "xwd"]
_MISSING = [b for b in REQUIRED_BINS if shutil.which(b) is None]
if _WM_BIN is None:
    _MISSING.append("matchbox-window-manager|twm")
if _CONV_BIN is None:
    _MISSING.append("convert|magick")
# IM7's `magick` cannot read xwd directly; needs xwdtopnm as a bridge.
if _CONV_BIN == "magick" and _XWDTOPNM is None:
    _MISSING.append("xwdtopnm (netpbm)")

# A module-level `pytestmark` set in a conftest does NOT propagate to the
# sibling test modules, so an earlier skipif here never actually engaged: the
# suite ran and crashed on the first missing tool (e.g. xwdtopnm on an
# ImageMagick-7 box without netpbm). `collect_ignore_glob` IS honoured from a
# conftest, so when a required tool is absent skip collecting this directory's
# tests entirely -- with the reason on stderr -- instead of letting them fail.
if _MISSING:
    import sys
    print("SKIP xwpe X11 GUI tests: missing " + ", ".join(_MISSING),
          file=sys.stderr)
    collect_ignore_glob = ["test_*.py"]

HERE = os.path.dirname(os.path.abspath(__file__))
XWPE_BIN = os.environ.get("XWPE_BIN") or os.path.normpath(os.path.join(HERE, "..", "..", "xwpe"))
DISPLAY = os.environ.get("XWPE_TEST_DISPLAY", ":88")
SCREEN = "1024x768x24"
SCREEN_W, SCREEN_H = (int(v) for v in SCREEN.split("x")[:2])
WINDOW_NAME = "Programming Environment"
# WM_CLASS res_name set by WeXterm.c::WpeXInit -- used to locate the window via
# xdotool's --classname (--name is broken in some xdotool builds; see _launch).
WINDOW_CLASS = "xwpe"


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


def _find_xwpe_window(timeout=10.0, step=0.25):
    """Wait up to `timeout` seconds for an xwpe window to appear; return its id.

    Uses xdotool's --classname (XGetClassHint) rather than --name (a regex over
    WM_NAME): the Homebrew xdotool's name regex returns nothing even for
    `--name .` while xprop confirms WM_NAME is set, whereas --classname is
    reliable across builds.  Returns None on timeout."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        out = subprocess.run(["xdotool", "search", "--classname", WINDOW_CLASS],
                             env={**os.environ, "DISPLAY": DISPLAY},
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        ids = out.stdout.decode().split()
        if ids:
            return ids[0]
        _real_sleep(step)
    return None


def _force_window_geometry(win, x=0, y=0, w=SCREEN_W, h=SCREEN_H):
    """Pin `win` at (x,y) with size (w,h) so screen-relative pixel scans match.

    matchbox (Debian/CI) honours xwpe's geometry hint and places it at +0+0
    full-screen; twm (the macOS fallback) uses RandomPlacement, leaving the
    window at roughly +50+50 a few pixels smaller -- which throws off every
    scrollbar/dialog coordinate scan and the bottom-strip status-bar diff.
    Forcing the geometry via xdotool makes both WMs equivalent for the suite."""
    _xdo("windowmove", win, str(x), str(y))
    _xdo("windowsize", win, str(w), str(h))
    _sleep(0.3)


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
        if _CONV_BIN == "convert":
            png = subprocess.run(["convert", "xwd:-", "png:-"],
                                 input=xwd.stdout, stdout=subprocess.PIPE,
                                 stderr=subprocess.DEVNULL).stdout
        else:
            # IM7 magick lost the xwd decode delegate, so route through netpbm.
            pnm = subprocess.run(["xwdtopnm"], input=xwd.stdout,
                                 stdout=subprocess.PIPE,
                                 stderr=subprocess.DEVNULL).stdout
            png = subprocess.run(["magick", "ppm:-", "png:-"], input=pnm,
                                 stdout=subprocess.PIPE,
                                 stderr=subprocess.DEVNULL).stdout
        return Image.open(io.BytesIO(png)).convert("RGB")


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
    if _WM_BIN == "matchbox-window-manager":
        # Use an empty kbdconfig: matchbox's default grabs <Alt>n/p/c/d/m, <Alt>Tab
        # etc. at the root, which are xwpe's own dialog/menu hotkeys -- the WM would
        # swallow them and fake an "Alt-hotkey broken" divergence.  See the file.
        kbd = os.path.join(HERE, "matchbox-kbdconfig")
        wm = _spawn([_WM_BIN, "-use_titlebar", "no", "-kbdconfig", kbd],
                    env={**os.environ, "DISPLAY": DISPLAY})
    else:
        # twm (the macOS fallback): a default install pops a startup menu and
        # grabs the X server for interactive window placement -- that locks out
        # xdotool and xwd.  RandomPlacement + NoGrabServer disables both;
        # NoTitle keeps the geometry the test suite expects.  twm has no Alt-
        # <letter> bindings in this minimal config, so xwpe's menu hotkeys
        # reach the application unmodified.
        twmrc = os.path.join(tempfile.gettempdir(), "xwpe-test-twmrc")
        with open(twmrc, "w") as fh:
            fh.write("NoTitle\nRandomPlacement\nNoGrabServer\n")
        wm = _spawn([_WM_BIN, "-f", twmrc],
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
    # Force xwpe.altMask: mod1.  The macOS build of xwpe (WeXterm.c:38) defaults
    # the Alt modifier mask to Mod4 (Cmd on real XQuartz), but xdotool under
    # Xvfb -- which is what the test harness uses on every platform -- delivers
    # the Alt_L keysym on Mod1, so Alt+<letter> menu accelerators never reach
    # the menu code without this override.  xwpe reads ~/.Xdefaults via
    # WpeXDefaults() when RESOURCE_MANAGER is unset, and the fixture already
    # sets HOME to tmp_path below, so a per-session resources file is enough.
    (tmp_path / ".Xdefaults").write_text("xwpe.altMask: mod1\n")
    # XWPE_LSP_NO_EAGER: these are chrome/dialog/menu tests that compare
    # full-screen pixel deltas.  If a language server (clangd for the .c file)
    # is installed on the box, its eager start pops an asynchronous "Messages"
    # window ("Starting language server... ready.") at some point after the
    # baseline screenshot -- ~17k changed pixels that make a closed dialog look
    # un-closed and add a phantom scrollbar'd window to the overlap tests.  None
    # of the X11 tests exercise LSP, so opt out of the eager boot for a
    # deterministic screen (the opt-out the editor already exposes for exactly
    # this -- see e_lsp_open_eager).
    # XWPE_FONT_SIZE: pin the Xft point size so the cell grid is a known size.
    # xwpe now honours the desktop monospace-font-name size (or this override);
    # without pinning, the coordinate-based pixel scans below would shift with
    # whatever monospace size the test host happens to have configured (the
    # GNOME schema default is "Monospace 11", not the 10 these tests assume).
    proc = _spawn([XWPE_BIN, str(src)],
                  env={**os.environ, "DISPLAY": DISPLAY, "HOME": str(tmp_path),
                       "XWPE_LSP_NO_EAGER": "1", "XWPE_FONT_SIZE": "10"},
                  cwd=str(tmp_path))
    win = _find_xwpe_window()
    assert win, "xwpe window did not appear"
    _force_window_geometry(win)
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
