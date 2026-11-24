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

Every process of the suite (the X server, the compositor, each editor, each
xdotool call) is started and ended by the owner in tests/gui_owner.py, as in
the X11 suite: the X server takes a free display number by itself, the
compositor's socket lives in a folder the owner made, an editor gets a
whitelisted environment and only this suite's display and socket, each wait
is for an event, and an editor a failed row leaves behind is ended and
reported as an error of that row.  A row asks for the display by
display_name(), never by a constant.
"""
import contextlib
import os
import pty
import sys
import time

import pytest

TESTS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if TESTS_DIR not in sys.path:
    sys.path.insert(0, TESTS_DIR)

import gui_owner as owner_api  # noqa: E402
import host_tools  # noqa: E402
import procinfo  # noqa: E402
from gui_owner import gui_owner, wait_for  # noqa: E402,F401  (the fixture)
from gui_owner import PreviousRowLeft  # noqa: E402,F401  (rows import it from here)

REQUIRED_BINS = ["Xvfb", "xdotool", "weston"]
# The tools the layer cannot use, each with its reason (the machine has none,
# the tool farm does not give it, it does not run).
_MISSING = host_tools.layer_unmet("Wayland GUI layer", REQUIRED_BINS)
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
    if host_tools.layer_reason("weston") is not None:
        return False
    for pat in ("/usr/lib/*/libweston-*/" + module,   # libweston-15 multiarch dir
                "/usr/lib/libweston-*/" + module,
                "/usr/lib/*/weston/" + module,         # Debian multiarch weston dir
                "/usr/lib/weston/" + module):
        if _glob.glob(pat):
            return True
    return False


if host_tools.layer_reason("weston") is None:
    if not _weston_has("x11-backend.so"):
        _MISSING.append("weston x11-backend.so")
    if not _weston_has("kiosk-shell.so"):
        _MISSING.append("weston kiosk-shell.so")


def pytest_collection_modifyitems(config, items):
    if not _MISSING:
        return
    # A job that installed one of the missing tools has a broken image: the
    # layer fails the session instead of skipping whole and reading as green.
    host_tools.fail_the_session_on_expected("Wayland GUI layer", _MISSING)
    here = os.path.dirname(os.path.abspath(__file__))
    skip = pytest.mark.skip(
        reason="Wayland GUI test dependencies missing: " + ", ".join(_MISSING))
    for item in items:
        if str(getattr(item, "fspath", "")).startswith(here):
            item.add_marker(skip)


HERE = os.path.dirname(os.path.abspath(__file__))
XWPE_BIN = os.environ.get("XWPE_BIN") or os.path.normpath(os.path.join(HERE, "..", "..", "xwpe"))
# The size of the X server's screen and of weston's output window.
SCREEN = owner_api.SCREEN
WL_WIDTH, WL_HEIGHT = int(owner_api.WL_WIDTH), int(owner_api.WL_HEIGHT)

# Deadlines, in seconds before XWPE_TEST_WAIT_SCALE, of the events the
# fixtures wait for.  Each is how long a loaded host may need, not how long
# the event takes: a wait ends the moment its event happens.
WINDOW_S = 20.0
TOOL_S = 30.0
DUMP_STOP_S = 10.0

import hashlib

# A loaded CI runner is slower; stretch every settle without slowing local runs.
_WAIT_SCALE = owner_api.wait_scale()
_real_sleep = time.sleep


def _sleep(seconds):
    _real_sleep(seconds * _WAIT_SCALE)


# The compositor of this run (a WaylandDisplay of the owner) and the id of the
# row that last started an editor: served by the xserver fixture once the
# servers are up, and by serve_display() alone.
_SERVED = {"display": None, "row": None}


@contextlib.contextmanager
def serve_display(compositor):
    """Serve @compositor to the rows that ask for it, and put the one served
    before it back when the caller is done.

    A restore and not a clear: the suite's compositor is served by a session
    fixture, and a module whose rows end their own weston serves a compositor
    of their own from a fixture of the same name.  That fixture shadows the
    session's one without running it again, so a row of that module that
    cleared the record left every later row of the session asking a display
    nobody serves."""
    before = _SERVED["display"]
    _SERVED["display"] = compositor
    try:
        yield compositor
    finally:
        _SERVED["display"] = before


def _served_display():
    """The compositor this run started, or an error naming what is missing."""
    served = _SERVED["display"]
    if served is None:
        raise RuntimeError("no Wayland display yet: ask for it from a row "
                           "that uses the xserver or xwpe fixture")
    return served


def display_name():
    """The X display this suite's weston runs on: the same accessor the X11
    suite has, so a module both suites run reads its display one way.  The
    X server picks its own number, so there is no constant to import."""
    return _served_display().x_display.name


def xtool_env():
    """The environment for a short tool a row runs: the whitelist, pointed at
    this run's X display and compositor.  Never a copy of the machine's."""
    return owner_api.tool_env(_served_display())


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


def _run_tool(argv):
    """Run a short tool (xdotool, xmodmap) against this run's display, bounded,
    on the owner's ledger; return its CompletedProcess.  Its standard output
    is the CompletedProcess's; its errors go to the row's log, which the
    owner keeps in the row's folder."""
    return owner_api.run_tool(argv, TOOL_S, display=_served_display())


def _xdo(*args):
    """One xdotool call against this run's display."""
    _run_tool(["xdotool", *args])


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
    out = _run_tool(["xmodmap", "-pke"]).stdout.decode()
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


# The same driver helper as the X11 suite: only a failed assertion is a known
# divergence, a crash or an owner failure fails the run.
from wpe_driver import incoherence  # noqa: E402,F401  (rows import it from here)


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


class WaylandSession(procinfo.ChildAsker):
    """A native-Wayland xwpe instance under headless weston, with the same
    helper API as the X11 suite's XwpeSession (key/click/type/menu/save/
    screenshot/saved_text), so test bodies can be shared.

    ``running(name)`` finds a child of the editor by the program it is (a
    list of pids; ``owned.running()`` is the owner's bool for whether a
    process is alive).  ``exec_path`` is the PATH the editor was started
    with."""

    def __init__(self, proc, env, wid, dump, owned=None):
        self.proc = proc
        self.env = env
        if env is not None:
            self.exec_path = env.get("PATH")
        self.wid = wid          # the weston X11 window (xdotool target)
        self.dump = dump        # XWPE_WL_DUMP path (continuously refreshed PPM)
        self.owned = owned      # the owner's record of the editor

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

    def _key(self, k):
        _xdo("key", "--window", self.wid, "--clearmodifiers", _to_keycodes(k))

    def focus(self):
        _xdo("windowfocus", self.wid)
        _xdo("windowactivate", self.wid)
        _sleep(0.3)

    def key(self, *keys, delay=0.5):
        for k in keys:
            self._note_children()
            self._key(k)
            wait_frame_quiet(self.dump, floor=delay)  # wait for the paint to settle
        return self

    def type(self, text, delay=0.4):
        self._note_children()
        _xdo("type", "--window", self.wid, "--clearmodifiers", text)
        wait_frame_quiet(self.dump, floor=delay)
        return self

    def click(self, px, py, delay=0.5, button=1, hold=None):
        """Click at (px,py) with `button` (1 left, 2 middle, 3 right).  `hold`
        names a modifier key (an xdotool key name such as "alt") kept down
        for the click, so a test can drive Alt-click."""
        self._note_children()
        _xdo("mousemove", "--window", self.wid, str(px), str(py))
        _sleep(0.15)
        _xdo("mousemove", "--window", self.wid, str(px + 1), str(py))  # real motion
        _sleep(0.1)
        if hold:
            _xdo("keydown", hold)
            _sleep(0.1)
        _xdo("click", str(button))
        if hold:
            _xdo("keyup", hold)
        wait_frame_quiet(self.dump, floor=delay)
        return self

    def drag(self, x1, y1, x2, y2, steps=12, delay=0.6):
        """Press button 1 at (x1,y1), move to (x2,y2) over `steps`, release.

        Drives the real Wayland pointer path (wl_pointer button PRESS ->
        motion* -> RELEASE) so the scrollbar-thumb drag (e_scroll_drag_v/h)
        runs end to end -- the mirror of the X11 suite's drag()."""
        self._note_children()
        _xdo("mousemove", "--window", self.wid, str(x1), str(y1))
        _sleep(0.15)
        _xdo("mousedown", "1")
        _sleep(0.1)
        for i in range(1, steps + 1):
            ix = x1 + (x2 - x1) * i // steps
            iy = y1 + (y2 - y1) * i // steps
            _xdo("mousemove", "--window", self.wid, str(ix), str(iy))
            _sleep(0.03)
        _sleep(0.1)
        _xdo("mouseup", "1")
        wait_frame_quiet(self.dump, floor=delay)
        return self

    def wheel(self, direction, count=3, px=512, py=300, delay=0.4):
        """Scroll the wheel `count` steps at (px,py). button 4 = up, 5 = down."""
        self._note_children()
        _xdo("mousemove", "--window", self.wid, str(px), str(py))
        _sleep(0.1)
        btn = "4" if direction == "up" else "5"
        for _ in range(count):
            _xdo("click", btn)
            _sleep(0.05)
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

    def save(self, delay=0.6, settle=4.0):
        """File -> Save (Alt-F, s), then wait until the file is really rewritten.

        The Wayland input path is asynchronous: under load the Alt-F/s keystrokes
        and the disk write they trigger can land a little after the fixed key
        delays.  Poll the file mtime so a caller that reads saved_text() right
        after never observes the pre-save file -- the read-after-save race that
        made the scrollbar-drag tests flaky in the full suite.  Every caller
        saves a modified buffer, so the write always lands and this returns as
        soon as it does (the timeout is only a backstop)."""
        try:
            before = os.path.getmtime(self.srcfile)
        except OSError:
            before = -1.0
        self.key("alt+f", delay=delay)
        self.key("s", delay=delay)
        deadline = time.time() + settle
        while time.time() < deadline:
            try:
                if os.path.getmtime(self.srcfile) != before:
                    break
            except OSError:
                pass
            time.sleep(0.05)
        return self

    def saved_text(self):
        with open(self.srcfile) as fh:
            return fh.read()

    def screenshot(self):
        """The current wl_shm frame xwpe last painted, as a Pillow RGB image."""
        return _load_ppm(self.dump)


@pytest.fixture(scope="session")
def xserver(gui_owner, tmp_path_factory, request):
    """Start Xvfb + weston (x11-backend) once for the whole test session.

    Both are started by the owner: the X server on a display number it
    picked itself, weston on that display with a socket in a folder the
    owner made.  The owner ends them, the compositor first, when the
    session ends, so a failed start leaves nothing running and nothing of
    another run, or of the desktop, is touched.  The compositor's reasons
    (kiosk-shell, the x11 backend) are in owner_api.start_compositor."""
    log_dir = str(tmp_path_factory.mktemp("wayland-session"))
    x_display = owner_api.start_x_server(gui_owner, request, log_dir, SCREEN)
    compositor = owner_api.start_compositor(gui_owner, request, x_display,
                                            log_dir, WL_WIDTH, WL_HEIGHT)
    with serve_display(compositor):
        yield compositor


def pytest_configure(config):
    """Refuse --reruns: a row that fails under load must fail."""
    owner_api.reject_reruns(config)


def _weston_window():
    """The id of weston's X window (the xdotool target), or None before it
    exists.  Under the kiosk shell it is the one window of the output."""
    ids = _run_tool(["xdotool", "search", "--class", "weston"]).stdout.decode().split()
    return ids[-1] if ids else None


def _require_clean_compositor(owner):
    """Check no editor of the row before is still running when a row starts
    its own, and name that row when one is, instead of letting this row
    talk to the old surface by accident."""
    running = owner.running("xwpe")
    if running:
        raise PreviousRowLeft("%s left an editor running: %s"
                              % (_SERVED["row"] or "an earlier row",
                                 "; ".join(o.describe() for o in running)))


def launch_editor(owner, workdir, request, args, extra_env=None,
                  detached_from_terminal=False, terminal_fd=None):
    """Start the native-Wayland xwpe with `args` on this run's compositor,
    in `workdir`, which is also its HOME, and return the owner's record.

    Its end is registered with the row before anything waits on it, so a row
    that fails to start it, or never sees its first frame, still ends it.
    The row's id is kept so the next row can name it if it leaves an editor
    behind.  The owner sets the frame dump and the window size."""
    _require_clean_compositor(owner)
    _SERVED["row"] = request.node.nodeid
    return owner_api.Editor.start(
        owner, XWPE_BIN, args, "wayland", _SERVED["display"], str(workdir),
        str(workdir), extra_env=extra_env, label=request.node.nodeid,
        request=request,
        detached_from_terminal=detached_from_terminal,
        stdio=owner_api.LOG if terminal_fd is None else terminal_fd)


def _window_size(wid):
    """(width, height) of weston's X window as xdotool reports it, or None."""
    out = _run_tool(["xdotool", "getwindowgeometry", "--shell", wid])
    values = dict(line.split("=", 1) for line in out.stdout.decode().split()
                  if "=" in line)
    if "WIDTH" not in values or "HEIGHT" not in values:
        return None
    return int(values["WIDTH"]), int(values["HEIGHT"])


def _force_output_size():
    """Give weston's output window the size every editor of the suite is
    started for, and wait until the server reports it.

    A resize row leaves the output at another size.  An editor commits a
    surface of WL_WIDTH x WL_HEIGHT, and a fullscreen surface larger than
    the output makes the compositor disconnect it with a protocol error, so
    the next row would get an editor that dies at once.  The fixture owns
    the output's state, not each resize row (as the X11 fixture does for its
    window)."""
    wid = wait_for(_weston_window, "weston's window to exist", WINDOW_S)
    _xdo("windowsize", wid, str(WL_WIDTH), str(WL_HEIGHT))
    wait_for(lambda: _window_size(wid) == (WL_WIDTH, WL_HEIGHT),
             "weston's window to be %dx%d" % (WL_WIDTH, WL_HEIGHT), WINDOW_S,
             state_fn=lambda: _window_size(wid))


def _dump_has_stopped(dump):
    """An event function: true once the frame dump is the same as it was at
    the previous poll.  It observes only the dump file: that the ended
    editor writes no more frames.  It does not observe the compositor
    dropping the surface."""
    last = [_frame_digest(dump)]

    def stopped():
        now = _frame_digest(dump)
        same = now == last[0]
        last[0] = now
        return same
    return stopped


def _wait_dump_stopped(dump):
    """Wait, after a row's editor has ended, for its frame dump to stop
    changing, so the row's last frame is the file the next row finds gone or
    rewritten, never one still being written."""
    wait_for(_dump_has_stopped(dump), "the ended editor's frame dump to stop "
             "changing", DUMP_STOP_S)


@pytest.fixture
def xwpe(xserver, gui_owner, tmp_path, request):
    """Launch a fresh NATIVE-WAYLAND xwpe editing a small C file; tear down after.

    The editor is started by the owner with its end registered before the
    first wait, so a row that fails to start never leaves it running, and the
    compositor must hold no editor of an earlier row when it starts."""
    assert os.path.exists(XWPE_BIN), "xwpe binary not found at %s (set XWPE_BIN)" % XWPE_BIN
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
    # The launcher pins everything else: HOME is the row's folder, the
    # backend is wayland on this suite's compositor only (never the user's
    # session), the font size, the eager-server opt-out and the 1024x768
    # window size the X11 suite also forces (so centered dialogs land at the
    # same window fraction and coordinate-based pixel scans stay calibrated)
    # are set, and the frame dump is the row's frame.ppm.
    dump = str(tmp_path / "frame.ppm")
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
    # XWPE_TEST_HELP: the help file F1 opens, which the editor finds in its
    # HOME (the row's folder) through XWPE_LIB.  XWPE_TEST_OPTIONS is the
    # xwperc that goes with it, so a long page row stays one row.
    help_text = param.pop("XWPE_TEST_HELP", None)
    options = param.pop("XWPE_TEST_OPTIONS", None)
    if help_text is not None:
        (tmp_path / "help.xwpe").write_text(help_text)
        (tmp_path / "xwperc").write_text(options or "[General]\n")
        param["XWPE_LIB"] = str(tmp_path)
    # XWPE_TEST_DETACHED: start the editor as a desktop launcher would, a
    # session leader with no controlling terminal (present means detached).
    detached = param.pop("XWPE_TEST_DETACHED", None) is not None
    # XWPE_TEST_PTY: the editor's standard streams are a pty the test reads,
    # as when xwpe is started from a terminal; every byte it writes to that
    # terminal is then visible (session.pty_master).
    on_pty = param.pop("XWPE_TEST_PTY", False)
    pty_master = pty_slave = None
    if on_pty:
        pty_master, pty_slave = pty.openpty()
        request.addfinalizer(lambda: os.close(pty_master))
    # Registered before the editor, so it runs after the owner has ended it
    # (and so fully reaped it): the last frame is complete before the row
    # is over.
    request.addfinalizer(lambda: _wait_dump_stopped(dump))
    _force_output_size()
    try:
        owned = launch_editor(gui_owner, tmp_path, request, [str(src)], param,
                              detached_from_terminal=detached,
                              terminal_fd=pty_slave)
    finally:
        if pty_slave is not None:
            os.close(pty_slave)
    # Wait for the weston X window (the xdotool target) and the first frame
    # dump, then for the first paint to settle.
    wid = wait_for(lambda: _weston_window() if os.path.exists(dump) else None,
                   "weston's window and the first frame of the editor",
                   WINDOW_S, state_fn=owned.log_text)
    wait_frame_quiet(dump, floor=0.1)
    session = WaylandSession(owned.proc, owned.env, wid, dump, owned)
    session.srcfile = str(src)
    session.pty_master = pty_master
    session.focus()
    yield session
