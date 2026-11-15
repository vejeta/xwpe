"""The size the Wayland surface commits fits the configure it answers.

The compositor closes an editor whose frame does not fit its configure: weston
requires a maximized buffer to be the configured size and a fullscreen one to be
no larger.  The frame owner (we_wlframe.c) is the only code that sizes the
buffer, acknowledges a configure and commits; these rows hold it to the rule.

test_fullscreen_start_on_a_small_output
    The editor asks for 1024x768 and the compositor's output is 1000x750 (a
    fullscreen bound).  On the old code the first commit was 1024x768 and weston
    disconnected the editor (xdg_wm_base error 4); no window of the bound's size
    ever appeared.  No typing is needed: the first frame says it.

test_shrink_sweep_commits_within_bounds
    A deep shrink and grow of the output while the pointer moves over the window
    (the trigger of the old resize races), under CPU load.  The oracle is
    libwayland's own wire log (WAYLAND_DEBUG=1 on the editor), not the editor's
    trace: it is parsed for the invariants the protocol and weston enforce.  The
    race is load dependent: on the old code it shows as a wire violation (a
    commit that does not fit the configure just acknowledged) on every run in
    which a resize reaches the editor, and as weston's protocol error when the
    old buffer is the larger one.  Run it with XWPE_TEST_LOAD=8 for a harder
    host; it is under a load of at least DEFAULT_LOAD busy processes anyway.

The sweep row also holds the frame trace to no refused present (the frame
owner's private queue keeps a configure from arriving inside a relayout) and
the wire log to the 30x6-cell minimum size announced.

test_subcell_configure_is_answered_without_input, test_sigterm_after_a_subcell_configure_saves_the_work
    A weston of the row's own over a larger X screen, the output a pixel
    larger than it was, the pointer left alone: the configure reaches the
    editor alone.  It must be acked within a deadline, and SIGTERM after it
    must still end the editor with its work in the .ESV file.  The pump that
    reads the connection must not wait inside libwayland's dispatch loop for
    an event of the default queue.

The libwayland log text is pinned to libwayland-client 1.26 (the wire lines
"-> iface#id.request(args)" and "iface#id.event(args)").  The kiosk shell
fullscreens the one client, so a configure with a size is a fullscreen bound:
a buffer must be exactly that size (the owner's rule for MAXIMIZED and
FULLSCREEN).
"""
import os
import re
import signal
import subprocess
import time

import pytest

import gui_owner as owner_api
from conftest import XWPE_BIN, WINDOW_S, _load_ppm, _xdo, wait_for

# The compositor's output in the small-output row.
OUTPUT_W, OUTPUT_H = 1000, 750
# CPU hogs the sweep runs under at least.
DEFAULT_LOAD = 4

SRC = ("int main(){\n"
       "  return 0;\n"
       "}\n")


@pytest.fixture
def small_compositor(gui_owner, xserver, tmp_path, request):
    """A headless weston of this row, with an output of OUTPUT_W x OUTPUT_H and
    the kiosk shell (one fullscreen client).  It is the row's own, never the
    session's; the owner ends it with the row.  The headless backend needs no
    X server for the output, and software rendering keeps the load fixture
    from starving it."""
    runtime = gui_owner.make_runtime_dir()
    socket = "wlh-%d" % os.getpid()
    compositor = gui_owner.start_service(
        ["weston", "--backend=headless-backend.so", "--shell=kiosk-shell.so",
         "--renderer=pixman", "--width=%d" % OUTPUT_W, "--height=%d" % OUTPUT_H,
         "--socket=" + socket, "--idle-time=0"],
        "headless weston of the row", str(tmp_path), str(tmp_path),
        kind="compositor", request=request, runtime_dir=runtime)
    path = os.path.join(runtime, socket)
    wait_for(lambda: os.path.exists(path), "weston's socket %s to appear" % path,
             owner_api.COMPOSITOR_START_S, state_fn=compositor.log_text)
    return gui_owner.register_wayland(runtime, socket, compositor, xserver.x_display)


def _trace_lines(path):
    """The lines of the frame trace, or [] when the editor wrote none."""
    try:
        with open(path) as fh:
            return fh.read().splitlines()
    except OSError:
        return []


def test_fullscreen_start_on_a_small_output(small_compositor, gui_owner, tmp_path,
                                            request):
    src = tmp_path / "t.c"
    src.write_text(SRC)
    dump = str(tmp_path / "frame.ppm")
    trace = str(tmp_path / "frame.trace")
    owned = owner_api.Editor.start(
        gui_owner, XWPE_BIN, [str(src)], "wayland", small_compositor,
        str(tmp_path), str(tmp_path), extra_env={"XWPE_WL_FRAME_TRACE": trace},
        label=request.node.nodeid, request=request)
    wait_for(lambda: os.path.exists(dump) or owned.proc.poll() is not None,
             "the editor's first frame", WINDOW_S, state_fn=owned.log_text)
    assert owned.proc.poll() is None, "the editor ended at start-up"
    frame = _load_ppm(dump)
    assert frame.size == (OUTPUT_W, OUTPUT_H), \
        "the first frame is %dx%d, the output is %dx%d" % (
            frame.size[0], frame.size[1], OUTPUT_W, OUTPUT_H)
    lines = _trace_lines(trace)
    commits = [ln for ln in lines if ln.startswith("commit ")]
    assert commits, "the frame trace holds no commit: %r" % lines
    for ln in commits:
        assert ("buffer=%dx%d bound=%dx%d" % (OUTPUT_W, OUTPUT_H, OUTPUT_W, OUTPUT_H)) in ln, \
            "a commit that does not fit the bound: %s" % ln
    assert not [ln for ln in lines if ln.startswith("withheld")], lines


# ---------------------------------------------------------------------------
# The wire log

SURFACE_RE = re.compile(
    r"-> xdg_wm_base#\d+\.get_xdg_surface\(new id xdg_surface#(\d+), wl_surface#(\d+)\)")
# An event line: a timestamp, then the queue's name when it has one.
EVENT_PREFIX = r"^\[[0-9:. ]+\]\s+(?:\{[^}]*\}\s+)?"
TOPLEVEL_CONFIGURE_RE = re.compile(
    EVENT_PREFIX + r"xdg_toplevel#\d+\.configure\((-?\d+), (-?\d+), array\[\d+\]\)")
SURFACE_CONFIGURE_RE = re.compile(
    EVENT_PREFIX + r"xdg_surface#(\d+)\.configure\((\d+)\)")
ACK_RE = re.compile(
    r"-> xdg_surface#(\d+)\.ack_configure\((\d+)\)")
BUFFER_RE = re.compile(
    r"-> wl_shm_pool#\d+\.create_buffer\(new id wl_buffer#(\d+), \d+, (\d+), (\d+), \d+, \d+\)")
ATTACH_RE = re.compile(r"-> wl_surface#(\d+)\.attach\(wl_buffer#(\d+), -?\d+, -?\d+\)")
COMMIT_RE = re.compile(r"-> wl_surface#(\d+)\.commit\(\)")


def wire_problems(text):
    """The violations of the surface protocol in a WAYLAND_DEBUG=1 log.

    Invariants (the compositor is in the kiosk shell, so a configure with a
    size is a fullscreen bound):
    - every ack_configure carries the serial of the newest configure the
      editor had received when it sent it;
    - every commit after an acknowledgement has a buffer of exactly the size
      the acknowledged configure offered (a configure of 0x0 leaves the size
      to the editor and is not held to one).
    Returns a list of text lines; empty when none was violated."""
    problems = []
    xdg_surface = main_surface = None
    pending = (0, 0)
    configures = {}           # serial -> (w, h)
    newest = None
    acked_bound = None
    buffers = {}              # wl_buffer id -> (w, h)
    attached = None
    for line in text.splitlines():
        m = SURFACE_RE.search(line)
        if m:
            xdg_surface, main_surface = m.group(1), m.group(2)
            continue
        m = TOPLEVEL_CONFIGURE_RE.match(line)
        if m:
            pending = (int(m.group(1)), int(m.group(2)))
            continue
        m = SURFACE_CONFIGURE_RE.match(line)
        if m and m.group(1) == xdg_surface:
            serial = int(m.group(2))
            configures[serial] = pending
            newest = serial
            continue
        m = ACK_RE.search(line)
        if m and m.group(1) == xdg_surface:
            serial = int(m.group(2))
            if serial != newest:
                problems.append("ack of serial %d, the newest configure is %s: %s"
                                % (serial, newest, line))
            acked_bound = configures.get(serial)
            continue
        m = BUFFER_RE.search(line)
        if m:
            buffers[m.group(1)] = (int(m.group(2)), int(m.group(3)))
            continue
        m = ATTACH_RE.search(line)
        if m and m.group(1) == main_surface:
            attached = buffers.get(m.group(2))
            continue
        m = COMMIT_RE.search(line)
        if m and m.group(1) == main_surface and attached and acked_bound:
            if acked_bound[0] > 0 and acked_bound[1] > 0 and attached != acked_bound:
                problems.append("commit of a %dx%d buffer after the ack of a %dx%d "
                                "configure: %s" % (attached + acked_bound + (line,)))
    return problems


MIN_SIZE_RE = re.compile(r"-> xdg_toplevel#\d+\.set_min_size\((\d+), (\d+)\)")
APPLY_RE = re.compile(r"^apply serial=\d+ buffer=(\d+)x(\d+) grid=(\d+)x(\d+)$")
FRAME_TRACE = "frame.trace"   # relative: the editor runs in the row's folder
WIRE_AND_TRACE = {"WAYLAND_DEBUG": "1", "XWPE_WL_FRAME_TRACE": FRAME_TRACE}


def min_size_problems(log, trace_lines):
    """What is wrong with the window's announced minimum size.

    The editor must tell the compositor its smallest window, 30x6 cells (as
    X11's PMinSize does for a window manager).  The cell is read off the
    editor's own first apply line: the grid times the cell fits the buffer."""
    m = MIN_SIZE_RE.search(log)
    if not m:
        return ["no xdg_toplevel.set_min_size on the wire"]
    min_w, min_h = int(m.group(1)), int(m.group(2))
    applied = [APPLY_RE.match(ln) for ln in trace_lines]
    applied = [a for a in applied if a]
    if not applied:
        return ["the frame trace holds no apply line"]
    buf_w, buf_h, cols, rows = [int(v) for v in applied[0].groups()]
    cell_w, cell_h = min_w // 30, min_h // 6
    problems = []
    if min_w != 30 * cell_w or min_h != 6 * cell_h:
        problems.append("min size %dx%d is not 30x6 cells" % (min_w, min_h))
    if not (cols * cell_w <= buf_w < (cols + 1) * cell_w and
            rows * cell_h <= buf_h < (rows + 1) * cell_h):
        problems.append("min size %dx%d does not match a %dx%d cell grid in a "
                        "%dx%d buffer" % (min_w, min_h, cols, rows, buf_w, buf_h))
    return problems


@pytest.mark.parametrize("xwpe", [WIRE_AND_TRACE], indirect=True)
def test_shrink_sweep_commits_within_bounds(xwpe, gui_owner, tmp_path):
    hogs = gui_owner.start_hogs(max(owner_api.load_count(), DEFAULT_LOAD),
                                str(tmp_path), "load")
    try:
        xwpe.type("int main(){ return 0; }")
        for rep in range(2):
            for i, w in enumerate(range(1000, 400, -20)):
                _xdo("windowsize", xwpe.wid, str(w), str(int(w * 0.72)))
                _xdo("mousemove", "--window", xwpe.wid,
                     str(20 + (i % 30) * 8), str(40 + (i % 10) * 8))
                time.sleep(0.02)
            for i, w in enumerate(range(400, 1000, 20)):
                _xdo("windowsize", xwpe.wid, str(w), str(int(w * 0.72)))
                _xdo("mousemove", "--window", xwpe.wid,
                     str(30 + (i % 25) * 8), str(50 + (i % 8) * 8))
                time.sleep(0.02)
            assert xwpe.proc.poll() is None, "the editor ended in the sweep (rep %d)" % rep
        time.sleep(1.0)
        assert xwpe.proc.poll() is None, "the editor ended after the sweep"
    finally:
        for hog in hogs:
            hog.end()
    log = xwpe.owned.log_text()
    assert "xdg_surface#" in log, "no wire log: WAYLAND_DEBUG was not honoured"
    problems = wire_problems(log)
    assert not problems, "%d wire violation(s), first: %s" % (len(problems), problems[:3])
    trace = _trace_lines(os.path.join(os.path.dirname(xwpe.srcfile), FRAME_TRACE))
    withheld = [ln for ln in trace if ln.startswith("withheld")]
    assert not withheld, \
        "%d present(s) refused while a configure waited, first: %s" % (len(withheld), withheld[:3])
    problems = min_size_problems(log, trace)
    assert not problems, problems


# ---------------------------------------------------------------------------
# The pump never waits inside libwayland

# How long the editor may take to answer a configure that comes alone and
# changes only a few pixels, before the row says it is not being served.
SUBCELL_ACK_S = 1.5
# The output starts at ROOMY_W x ROOMY_H, a size the editor did not ask for,
# and grows by a pixel: the grid and the cells stay the same.
ROOMY_W, ROOMY_H = 1000, 750
STEP_W, STEP_H = 1001, 751
# The editor's own size, below the output's.
OWN_W, OWN_H = "720", "540"
# The X screen under this row's weston is larger than the output, so the
# output can grow without being cut and the pointer stays inside it.
X_SCREEN = "1600x1200x24"
EXIT_DEADLINE_S = 10
# The wire log is quiet when it has not grown for this long.
LOG_QUIET_S = 0.4


def wait_quiet_log(log):
    """Wait until the editor's wire log has stopped growing, so what follows
    is not mixed with the events of what came before."""
    state = {"size": -1, "since": time.monotonic()}

    def quiet():
        size = len(log())
        if size != state["size"]:
            state["size"], state["since"] = size, time.monotonic()
            return False
        return time.monotonic() - state["since"] >= LOG_QUIET_S * owner_api.wait_scale()
    wait_for(quiet, "the wire log to be quiet", 10.0)


def acked_serials(log):
    """The serials of every ack_configure on the wire, in order."""
    return [int(m.group(2)) for m in ACK_RE.finditer(log)]


class RoomyEditor:
    """An editor on a weston of the row whose output is smaller than the X
    screen under it, so the X pointer can be put outside the output and a
    resize then reaches the editor as a configure alone, with no pointer
    enter or leave to wake its pump."""

    def __init__(self, owned, compositor, wid, srcfile):
        self.owned = owned
        self.proc = owned.proc
        self.compositor = compositor
        self.wid = wid
        self.srcfile = srcfile

    def xdo(self, *args):
        owner_api.run_tool(["xdotool"] + list(args), 30, display=self.compositor)

    def log(self):
        return self.owned.log_text()

    def park_for_a_lone_configure(self):
        """Put the pointer inside the output, let the log go quiet, and return
        the highest serial acked so far."""
        wait_quiet_log(self.log)
        return max(acked_serials(self.log()), default=0)

    def grow_by_a_few_pixels(self):
        self.xdo("windowsize", self.wid, str(STEP_W), str(STEP_H))


@pytest.fixture
def roomy_editor(gui_owner, tmp_path, request):
    """Xvfb larger than the output, weston on it, and the editor (wire log on)."""
    x_display = owner_api.start_x_server(gui_owner, request, str(tmp_path), X_SCREEN)
    compositor = owner_api.start_compositor(gui_owner, request, x_display,
                                            str(tmp_path), ROOMY_W, ROOMY_H)
    src = tmp_path / "t.c"
    src.write_text(SRC)
    dump = str(tmp_path / "frame.ppm")
    owned = owner_api.Editor.start(
        gui_owner, XWPE_BIN, [str(src)], "wayland", compositor, str(tmp_path),
        str(tmp_path), extra_env={"WAYLAND_DEBUG": "1", "XWPE_WL_WIDTH": OWN_W,
                                  "XWPE_WL_HEIGHT": OWN_H},
        label=request.node.nodeid, request=request)
    wait_for(lambda: os.path.exists(dump), "the editor's first frame", WINDOW_S,
             state_fn=owned.log_text)
    editor = RoomyEditor(owned, compositor, None, str(src))
    out = owner_api.run_tool(["xdotool", "search", "--class", "weston"], 30,
                             display=compositor).stdout.decode().split()
    editor.wid = out[-1]
    editor.xdo("windowfocus", editor.wid)
    wait_quiet_log(editor.log)
    return editor


def test_subcell_configure_is_answered_without_input(roomy_editor):
    """A configure that comes alone, for a size a few pixels larger, is acked
    with nobody touching the pointer or the keyboard.

    The configure waits on the frame owner's queue, so the pump that reads the
    connection must read once and dispatch, not call libwayland's dispatch
    loop: that polls again until an event for the default queue arrives, and
    the editor sat in it with the window unanswered and the end request never
    served."""
    editor = roomy_editor
    before = editor.park_for_a_lone_configure()
    editor.grow_by_a_few_pixels()
    wait_for(lambda: max(acked_serials(editor.log()), default=0) > before,
             "the ack of the larger size on the wire", SUBCELL_ACK_S,
             state_fn=lambda: "acked before %d, now %r" % (before, acked_serials(editor.log())[-3:]))
    assert editor.proc.poll() is None


def test_sigterm_after_a_subcell_configure_saves_the_work(roomy_editor):
    """The end request is served after such a configure: SIGTERM ends the
    editor and its unsaved window is in the .ESV file."""
    editor = roomy_editor
    typed = "qqqq"
    editor.xdo("type", "--window", editor.wid, "--clearmodifiers", typed)
    wait_quiet_log(editor.log)
    editor.park_for_a_lone_configure()
    editor.grow_by_a_few_pixels()
    os.kill(editor.proc.pid, signal.SIGTERM)
    try:
        editor.proc.wait(timeout=EXIT_DEADLINE_S * owner_api.wait_scale())
    except subprocess.TimeoutExpired:
        pass
    assert editor.proc.poll() is not None, "the editor is still running after SIGTERM"
    saved = editor.srcfile + ".ESV"
    assert os.path.exists(saved), sorted(os.listdir(os.path.dirname(saved)))
    with open(saved) as fh:
        assert typed in fh.read()
