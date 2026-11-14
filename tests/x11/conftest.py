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

Every process of the suite (the X server, the window manager, each editor,
a helper client) is started and ended by the owner in tests/gui_owner.py:
the X server takes a free display number by itself (Xvfb -displayfd), so
two runs at once never share a display; an editor gets a whitelisted
environment and only this suite's display; each wait is for an event, not
for a number of seconds; and what an editor leaves running is found, ended
and reported as an error of its row.  A row asks for the display by
display_name(), never by a constant.
"""
import os
import pty
import select
import shutil
import subprocess
import sys
import time

import pytest

TESTS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if TESTS_DIR not in sys.path:
    sys.path.insert(0, TESTS_DIR)

import gui_owner as owner_api  # noqa: E402
from gui_owner import gui_owner, wait_for  # noqa: E402,F401  (the fixture)

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
# ImageMagick-7 box without netpbm). Mark every X11 test in this directory as
# skipped via a collection hook instead. This yields a clean "skipped"
# (pytest exit 0), not a crash and not "no tests collected" (exit 5, which a
# plain `collect_ignore` would give for `pytest tests/x11`), so the suite stays
# green when the GUI stack is absent (e.g. on a text-only CI runner).
def pytest_collection_modifyitems(config, items):
    if not _MISSING:
        return
    here = os.path.dirname(os.path.abspath(__file__))
    skip = pytest.mark.skip(
        reason="X11 GUI test dependencies missing: " + ", ".join(_MISSING))
    for item in items:
        if str(getattr(item, "fspath", "")).startswith(here):
            item.add_marker(skip)

HERE = os.path.dirname(os.path.abspath(__file__))
XWPE_BIN = os.environ.get("XWPE_BIN") or os.path.normpath(os.path.join(HERE, "..", "..", "xwpe"))
SCREEN = "1024x768x24"
SCREEN_W, SCREEN_H = (int(v) for v in SCREEN.split("x")[:2])
WINDOW_NAME = "Programming Environment"
# WM_CLASS res_name set by WeXterm.c::WpeXInit -- used to locate the window via
# xdotool's --classname (--name is broken in some xdotool builds; see _launch).
WINDOW_CLASS = "xwpe"

# Deadlines, in seconds before XWPE_TEST_WAIT_SCALE, of the events the
# fixtures wait for.  Each is how long a loaded host may need, not how long
# the event takes: a wait ends the moment its event happens.
SERVER_START_S = 20.0
WM_START_S = 20.0
WINDOW_S = 10.0
GEOMETRY_S = 10.0
WINDOW_GONE_S = 10.0
# twm announces nothing when it is ready (it sets no window manager
# property), so the macOS fallback gets one fixed, scaled settle.
TWM_SETTLE_S = 1.5
# The line xprop prints for a root property that is set.
WM_CHECK_PROPERTY = "_NET_SUPPORTING_WM_CHECK"
WM_CHECK_SET = "window id"
# How many bytes the server's display-number pipe is read in.
DISPLAY_LINE_MAX = 64


# A loaded CI runner is 3-5x slower than a dev box, so the fixed settle times
# between a synthetic keystroke and the screenshot flake ("screen barely
# changed" -- the dialog had not rendered yet). XWPE_TEST_WAIT_SCALE stretches
# every settle without slowing the local run (default 1.0; the Debian
# autopkgtest sets 3). _sleep() replaces _sleep() throughout this module.
_WAIT_SCALE = owner_api.wait_scale()
_real_sleep = time.sleep


def _sleep(seconds):
    _real_sleep(seconds * _WAIT_SCALE)


# The X display of this run: set by the xserver fixture once the server has
# said which number it took, cleared when the session ends.
_SERVED = {"display": None, "row": None}


def display_name():
    """The name of the display this run's X server serves (":N").

    A row that talks to the server itself (a query, a screenshot, a helper
    client) asks for it here, when it needs it: the server picks its own
    number, so there is no constant to import."""
    served = _SERVED["display"]
    if served is None:
        raise RuntimeError("no X display yet: ask for it from a row that "
                           "uses the xserver or xwpe fixture")
    return served.name


def xtool_env():
    """The environment for a short X tool (xdotool, xwd, xclip) a row runs:
    the machine's, pointed at this run's display."""
    return {**os.environ, "DISPLAY": display_name()}


def _xdo(*args):
    subprocess.run(["xdotool", *args], env=xtool_env(),
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def _window_ids():
    """The ids of the xwpe windows on the display now (empty when none).

    Uses xdotool's --classname (XGetClassHint) rather than --name (a regex over
    WM_NAME): the Homebrew xdotool's name regex returns nothing even for
    `--name .` while xprop confirms WM_NAME is set, whereas --classname is
    reliable across builds."""
    out = subprocess.run(["xdotool", "search", "--classname", WINDOW_CLASS],
                         env=xtool_env(), stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL)
    return out.stdout.decode().split()


def _find_xwpe_window(timeout=WINDOW_S):
    """Wait for an xwpe window to appear and return its id, or None when
    none came within `timeout` seconds (before the wait scale)."""
    try:
        return wait_for(lambda: (_window_ids() or [None])[0],
                        "an xwpe window to appear on %s" % display_name(),
                        timeout)
    except owner_api.OwnerTimeout:
        return None


def _window_size(win):
    """(width, height) of `win` as xdotool reports it, or None."""
    out = subprocess.run(["xdotool", "getwindowgeometry", "--shell", win],
                         env=xtool_env(), stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL)
    values = dict(line.split("=", 1) for line in out.stdout.decode().split()
                  if "=" in line)
    if "WIDTH" not in values or "HEIGHT" not in values:
        return None
    return int(values["WIDTH"]), int(values["HEIGHT"])


def _force_window_geometry(win, x=0, y=0, w=SCREEN_W, h=SCREEN_H):
    """Pin `win` at (x,y) with size (w,h) so screen-relative pixel scans match.

    matchbox (Debian/CI) honours xwpe's geometry hint and places it at +0+0
    full-screen; twm (the macOS fallback) uses RandomPlacement, leaving the
    window at roughly +50+50 a few pixels smaller -- which throws off every
    scrollbar/dialog coordinate scan and the bottom-strip status-bar diff.
    Forcing the geometry via xdotool makes both WMs equivalent for the suite.
    It returns once the server reports the size."""
    _xdo("windowmove", win, str(x), str(y))
    _xdo("windowsize", win, str(w), str(h))
    wait_for(lambda: _window_size(win) == (w, h),
             "the window manager to give %s the size %dx%d" % (win, w, h),
             GEOMETRY_S, state_fn=lambda: _window_size(win))


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
    out = subprocess.run(["xmodmap", "-pke"], env=xtool_env(),
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

    def __init__(self, proc, win, owned=None):
        self.proc = proc
        self.win = win
        self.owned = owned

    def _note_children(self):
        """Tell the owner what the editor has started so far.

        Called before each input, which is what makes the editor start a
        program or a language server: if the editor crashes later, the
        owner still knows its children, also those it moved to a session
        of their own, and ends them with the row."""
        if self.owned is not None:
            self.owned.reap_zombies()
            self.owned.refresh_tree()

    def reap_zombies(self):
        """Collect the editor's orphaned programs that have ended, so a
        check that a program is gone does not see its zombie."""
        if self.owned is not None:
            self.owned.reap_zombies()

    def focus(self):
        _xdo("windowfocus", self.win)
        _sleep(0.3)

    def key(self, *keys, delay=0.5):
        for k in keys:
            self._note_children()
            keystroke = _to_keycodes(k)
            _KEYS_SENT.update(keystroke.split("+"))
            _xdo("key", "--window", self.win, "--clearmodifiers", keystroke)
            _sleep(delay)

    def click(self, px, py, delay=0.5, button=1, hold=None):
        """Click at (px,py) with `button` (1 left, 2 middle, 3 right).  `hold`
        names a modifier key (an xdotool key name such as "alt") kept down
        for the click, so a test can drive Alt-click."""
        self._note_children()
        _xdo("mousemove", "--window", self.win, str(px), str(py))
        _sleep(0.2)
        if hold:
            _xdo("keydown", hold)
            _sleep(0.1)
        _xdo("click", str(button))
        if hold:
            _xdo("keyup", hold)
        _sleep(delay)

    def drag(self, x1, y1, x2, y2, steps=12, delay=0.6):
        """Press button 1 at (x1,y1), move to (x2,y2) in steps, release.
        Drives the real X11 drag path (ButtonPress -> MotionNotify* ->
        ButtonRelease) -- e.g. a scrollbar-thumb drag."""
        self._note_children()
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
        self._note_children()
        if px is not None:
            _xdo("mousemove", "--window", self.win, str(px), str(py))
            _sleep(0.1)
        for _ in range(count):
            _xdo("click", button)
            _sleep(0.1)
        _sleep(delay)

    def type(self, text, delay=0.4):
        """Type a literal string (into a focused text field) via XTEST."""
        self._note_children()
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
        xwd = subprocess.run(["xwd", "-root", "-silent"], env=xtool_env(),
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        return xwd_to_image(xwd.stdout)


def xwd_to_image(xwd_bytes):
    """Decode an xwd capture into a Pillow Image (RGB).

    The capture carries the colormap of the window it was taken from, so on
    a palette display the pixels decode to the colours that colormap holds,
    whichever colormap the screen happens to have installed."""
    from PIL import Image
    import io
    if _CONV_BIN == "convert":
        png = subprocess.run(["convert", "xwd:-", "png:-"],
                             input=xwd_bytes, stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL).stdout
    else:
        # IM7 magick lost the xwd decode delegate, so route through netpbm.
        pnm = subprocess.run(["xwdtopnm"], input=xwd_bytes,
                             stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL).stdout
        png = subprocess.run(["magick", "ppm:-", "png:-"], input=pnm,
                             stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL).stdout
    return Image.open(io.BytesIO(png)).convert("RGB")


def _display_number(fd, chunks):
    """The display number the server wrote to fd, once its line is whole."""
    if not select.select([fd], [], [], 0)[0]:
        return None
    data = os.read(fd, DISPLAY_LINE_MAX)
    chunks.append(data)
    text = b"".join(chunks)
    if not data or not text.endswith(b"\n"):
        return None
    return int(text.strip())


def _start_xvfb(owner, request, log_dir):
    """Start Xvfb on the display number it picks; return the owner's
    record of that display.

    The server writes its number to the pipe once it is listening, so the
    read is the ready event: no fixed number, no lock file looked at or
    removed, and two runs at once never meet on one display."""
    read_fd, write_fd = os.pipe()
    try:
        server = owner.start_service(
            ["Xvfb", "-displayfd", str(write_fd), "-screen", "0", SCREEN,
             "-nolisten", "tcp"],
            "Xvfb of the X11 suite", log_dir, log_dir, kind="server",
            request=request, pass_fds=(write_fd,))
    finally:
        os.close(write_fd)
    chunks = []
    try:
        number = wait_for(lambda: _display_number(read_fd, chunks),
                          "Xvfb to print its display number", SERVER_START_S,
                          state_fn=server.log_text)
    finally:
        os.close(read_fd)
    return owner.register_x_display(":%d" % number, server)


def _wm_argv(log_dir):
    """The window manager's command line for this platform."""
    if _WM_BIN == "matchbox-window-manager":
        # Use an empty kbdconfig: matchbox's default grabs <Alt>n/p/c/d/m, <Alt>Tab
        # etc. at the root, which are xwpe's own dialog/menu hotkeys -- the WM would
        # swallow them and fake an "Alt-hotkey broken" divergence.  See the file.
        kbd = os.path.join(HERE, "matchbox-kbdconfig")
        return [_WM_BIN, "-use_titlebar", "no", "-kbdconfig", kbd]
    # twm (the macOS fallback): a default install pops a startup menu and
    # grabs the X server for interactive window placement -- that locks out
    # xdotool and xwd.  RandomPlacement + NoGrabServer disables both;
    # NoTitle keeps the geometry the test suite expects.  twm has no Alt-
    # <letter> bindings in this minimal config, so xwpe's menu hotkeys
    # reach the application unmodified.
    twmrc = os.path.join(log_dir, "twmrc")
    with open(twmrc, "w") as fh:
        fh.write("NoTitle\nRandomPlacement\nNoGrabServer\n")
    return [_WM_BIN, "-f", twmrc]


def _wm_is_ready():
    """True once a window manager has announced itself on the root window."""
    out = subprocess.run(["xprop", "-root", WM_CHECK_PROPERTY],
                         env=xtool_env(), stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL).stdout.decode()
    return WM_CHECK_SET in out


def _wait_wm_ready(wm):
    """Wait until the window manager is managing the display.

    matchbox sets _NET_SUPPORTING_WM_CHECK on the root window when it is
    ready.  twm sets nothing, so it gets one scaled settle."""
    if _WM_BIN == "twm":
        _real_sleep(owner_api.scaled(TWM_SETTLE_S))
        return
    wait_for(_wm_is_ready, "the window manager to announce itself on %s"
             % display_name(), WM_START_S, state_fn=wm.log_text)


def _start_window_manager(owner, request, display, log_dir):
    """Start the window manager on the owner's display and wait for it."""
    wm = owner.start_service(_wm_argv(log_dir), "window manager of the X11 "
                             "suite", log_dir, log_dir, kind="wm",
                             extra_env={"DISPLAY": display.name},
                             request=request)
    _wait_wm_ready(wm)
    return wm


@pytest.fixture(scope="session")
def xserver(gui_owner, tmp_path_factory, request):
    """Start Xvfb + the window manager once for the whole test session.

    Both are started by the owner, on a display number the server picked,
    and ended by it when the session ends, so a failed start leaves nothing
    running and nothing of another run is touched."""
    log_dir = str(tmp_path_factory.mktemp("x11-session"))
    display = _start_xvfb(gui_owner, request, log_dir)
    _SERVED["display"] = display
    try:
        _start_window_manager(gui_owner, request, display, log_dir)
        yield display.name
    finally:
        _SERVED["display"] = None


def pytest_configure(config):
    """Refuse --reruns: a row that fails under load must fail."""
    owner_api.reject_reruns(config)


class PreviousRowLeft(AssertionError):
    """A row started while the one before it had left something behind."""


def _running_editors(owner):
    """The editors the owner still has running."""
    return [owned for owned in owner.ledger.entries() if owned.kind == "xwpe"]


def _previous_row():
    return _SERVED["row"] or "an earlier row"


# The keys a synthetic keystroke may leave held.  A row that ends the editor
# with a key (Alt-X) has the chord's release sent to a window that is gone,
# so the server keeps Alt and x down: the next editor then gets Alt-x by
# auto-repeat and quits at once (its window never appears), and a later Alt-x
# is ignored because x is already down.
HELD_KEY_NAMES = ("Alt_L", "Alt_R", "Control_L", "Control_R", "Shift_L",
                  "Shift_R", "Meta_L", "Meta_R", "Super_L", "Super_R")
# Every key name or code a row has sent since the last release.
_KEYS_SENT = set()


def release_held_keys():
    """Release every modifier and every key a row sent that a synthetic
    keystroke may have left held on the display, so the next editor starts
    from a keyboard with nothing down."""
    names = list(HELD_KEY_NAMES) + sorted(_KEYS_SENT)
    _KEYS_SENT.clear()
    _xdo("keyup", *names)


def _require_clean_display(owner):
    """Check the display holds no editor before a row starts its own.

    A leftover window or editor of the row before is named, with that
    row's id, instead of letting this row find the old window by accident."""
    release_held_keys()
    running = _running_editors(owner)
    if running:
        raise PreviousRowLeft("%s left an editor running: %s"
                              % (_previous_row(),
                                 "; ".join(o.describe() for o in running)))
    try:
        wait_for(lambda: not _window_ids(),
                 "the xwpe window of the row before to leave %s" % display_name(),
                 WINDOW_GONE_S, state_fn=_window_ids)
    except owner_api.OwnerTimeout as error:
        raise PreviousRowLeft("%s left an xwpe window on the display: %s"
                              % (_previous_row(), error)) from error


def _require_windows_gone():
    """Wait, after a row's editor has ended, for its window to leave."""
    release_held_keys()
    wait_for(lambda: not _window_ids(),
             "the ended editor's window to leave %s" % display_name(),
             WINDOW_GONE_S, state_fn=_window_ids)


def launch_editor(owner, workdir, request, args, extra_env=None,
                  detached_from_terminal=False, terminal_fd=None):
    """Start xwpe with `args` on the suite's display, in `workdir`, which is
    also its HOME, and return the owner's record of it.

    Its end is registered with the row before anything waits on it, so a
    row that fails to start it, or never sees its window, still ends it.
    The row's id is kept so the next row can name it if it leaves a
    window behind."""
    _require_clean_display(owner)
    _SERVED["row"] = request.node.nodeid
    return owner_api.Editor.start(
        owner, XWPE_BIN, args, "x11", _SERVED["display"], str(workdir),
        str(workdir), extra_env=extra_env, label=request.node.nodeid,
        request=request, dump=False,
        detached_from_terminal=detached_from_terminal,
        terminal_fd=terminal_fd)


def launch_client(owner, workdir, request, argv):
    """Start a helper client (xlogo, a probe) on the suite's display; the
    row's end ends it, and anything it leaves is reported."""
    return owner_api.Client.start(
        owner, argv, _SERVED["display"], str(workdir), str(workdir),
        label=request.node.nodeid, request=request)


@pytest.fixture
def xwpe(xserver, gui_owner, tmp_path, request):
    """Launch a fresh xwpe editing a small C file; tear it down after.

    The editor is started by the owner, with its end registered before the
    first wait, so a row that fails to start never leaves it running; the
    display must be clean when the row starts and the ended editor's window
    must be gone when it ends."""
    assert os.path.exists(XWPE_BIN), "xwpe binary not found at %s (set XWPE_BIN)" % XWPE_BIN
    # Registered first, so it runs last: after the editor has been ended.
    request.addfinalizer(_require_windows_gone)
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
    # The launcher pins everything else: HOME is the row's folder, the
    # backend is x11 on this suite's display only (never the user's Wayland
    # session), the font size and the eager-server opt-out are set.
    param = dict(getattr(request, "param", None) or {})
    # XWPE_TEST_SOURCE: a test that needs its own program in t.c, not env.
    source = param.pop("XWPE_TEST_SOURCE", None)
    if source is not None:
        src.write_text(source)
    # XWPE_TEST_FILE: the name the program is edited under, for a language
    # whose debugger is chosen by it (P.java for jdb).
    name = param.pop("XWPE_TEST_FILE", None)
    if name is not None:
        src = tmp_path / name
        src.write_text(source or "")
    # XWPE_TEST_DETACHED: start the editor as a desktop launcher would, a
    # session leader with no controlling terminal.
    detached = param.pop("XWPE_TEST_DETACHED", None) is not None
    # XWPE_TEST_PTY: the editor's standard streams are a pty the test reads,
    # as when xwpe is started from a terminal; every byte it writes to that
    # terminal is then visible (session.pty_master).
    on_pty = param.pop("XWPE_TEST_PTY", False)
    pty_master = pty_slave = None
    if on_pty:
        pty_master, pty_slave = pty.openpty()
        request.addfinalizer(lambda: os.close(pty_master))
    try:
        owned = launch_editor(gui_owner, tmp_path, request, [str(src)], param,
                              detached_from_terminal=detached,
                              terminal_fd=pty_slave)
    finally:
        if pty_slave is not None:
            os.close(pty_slave)
    win = _find_xwpe_window()
    assert win, "xwpe window did not appear"
    _force_window_geometry(win)
    _sleep(1.0)
    session = XwpeSession(owned.proc, win, owned)
    session.srcfile = str(src)
    session.pty_master = pty_master
    session.focus()
    yield session
