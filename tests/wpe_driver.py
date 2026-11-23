"""Shared pyte driver for the per-menu-section wpe (ncurses) test suite.

Each menu section gets a `tests/test_menu_<section>.py` that drives wpe
through pyte, performs menu operations, saves, and asserts on the file
written to disk -- ground truth (the on-screen render can be captured
mid-repaint; the saved file cannot).

Usage:
    from wpe_driver import WpeSession, ALT, incoherence

    def test_xxx(tmp_path):
        with WpeSession(str(tmp_path), "AAA\\nBBB\\n") as w:
            w.menu(ALT.BLOCK, "o")     # Block -> Mark WhOle
            w.menu(ALT.EDIT, "c")      # Edit  -> Copy
            w.save()
            assert w.alive(), "wpe died"
            assert w.text().count("AAA") == 2

Incoherence flagging:
    @incoherence("Undo does not revert an XBuffer paste")
    def test_yyy(tmp_path): ...
    -> recorded as xfail; `pytest -rxX` lists every INCOHERENCE for a later
       manual review (turns green/XPASS when the behaviour is fixed).
"""
import base64
import collections
import os
import pty
import re
import select
import shutil
import time

import pytest
import pyte

import build_config
import gui_owner
import procinfo
from test_utf8_border import SafeScreen

# How many of the last lines of the editor's trace a failure message shows.
TRACE_TAIL_LINES = 20


def ui_trace_tail(path):
    """The last TRACE_TAIL_LINES lines of the trace file at @path, as text
    for a failure message; says so when there is no trace to read."""
    if not path:
        return "ui trace: the session writes none (XWPE_UI_TRACE is not set)"
    try:
        with open(path, errors="replace") as fh:
            lines = fh.read().splitlines()
    except OSError as exc:
        return "ui trace: %s cannot be read (%s)" % (path, exc.strerror)
    return "ui trace (last %d lines of %s):\n%s" % (
        TRACE_TAIL_LINES, path, "\n".join(lines[-TRACE_TAIL_LINES:]))


def sweep_cuts(one, last_cut, whole="whole"):
    """Run @one(n) for the cuts n = 1, 2, ... up to @last_cut, one after
    another, and stop after the first cut whose outcome is @whole.
    Return: {cut: outcome}.

    A fault sweep cuts the editor's memory at the n-th allocation of one
    action, for each n in turn.  What it proves: the action has a first
    allocation, a last allocation, and nothing in between that it can fail
    on halfway.  Every cut before the last allocation must leave the text
    as it was (the caller asserts that on the returned outcomes), and the
    first cut after it must let the action through whole, so the first
    "whole" is the action's own allocation count and nothing past it needs
    a run.

    The cuts run on the calling thread, never from a pool: the process
    owner (gui_owner) refuses a launch off the main thread, because the
    death signal of an editor follows the thread that forked it and a pool
    thread that ends would take the editor down with it."""
    outcomes = {}
    for cut in range(1, last_cut + 1):
        outcomes[cut] = one(cut)
        if outcomes[cut] == whole:
            break
    assert list(outcomes) == list(range(1, len(outcomes) + 1)), \
        "a sweep must run the cuts 1..n in order, got %s" % list(outcomes)
    return outcomes


WPE_BIN = os.environ.get("WPE_BIN") or os.path.join(os.path.dirname(__file__), "..", "wpe")

# The data files xwpe loads at runtime (the in-app help, syntax_def) live under
# $XWPE_LIB when set, else the compiled-in install prefix. Point it at the
# checkout so the in-tree help.xwpe_eng is found via the editor's "_eng"
# fallback: the help/manual scenarios then pass from a plain build tree on any
# OS, instead of silently depending on a system-installed copy that happens to
# exist on the Linux dev box but not on a fresh macOS runner.
XWPE_LIB_DIR = os.path.dirname(os.path.abspath(WPE_BIN))

# A session on one of these files runs its program under a build server the
# editor does not own (scala-cli's Bloop): close() ends that program, found
# by the JVM it runs as and this session's folder.
SERVER_PROGRAM_SUFFIXES = (".scala", ".sc")
# The files of the claude login that real_login links into a row's HOME.
LOGIN_NAMES = (".claude", ".claude.json")
SERVER_TRACE_NAME = ".ui-trace"  # the editor's trace in the session's folder
SERVER_JDK_KEY = "scala jdk"     # the trace line naming the java it chose
SERVER_PROGRAM_GONE_S = 10      # the server reaps a killed program well within this
SERVER_PROGRAM_POLL_S = 0.1

# A loaded CI runner is 3-5x slower than a dev box, so the fixed wait/timeout
# budgets that pass in-tree flake under autopkgtest. XWPE_TEST_WAIT_SCALE
# stretches every wait without slowing the local run (default 1.0; the Debian
# autopkgtest sets it to 3). All waits funnel through _drain(), so scaling there
# scales the startup wait AND every per-key delay.  The variable is read by
# gui_owner.wait_scale(), the one reader of it (tests/check-test-waits.sh).
WAIT_SCALE = gui_owner.wait_scale()

# Waiting for the screen to settle instead of sleeping the whole delay.  With
# XWPE_TEST_SETTLE_MS set (milliseconds), a drain returns once the editor has
# written something and then stayed silent for that long; the delay a test
# passes stays the ceiling.  A key that makes the editor write nothing still
# waits the whole delay, so a test that waits on purpose for something late
# (a mock reply, a timer) keeps its wait unless the editor paints in between.
# Unset or 0: the old behaviour, every drain sleeps its full delay.
SETTLE_S = float(os.environ.get("XWPE_TEST_SETTLE_MS", "0") or 0) / 1000.0
DRAIN_POLL_S = 0.1
COLS, ROWS = 80, 30

# The box the editor shows before a language server that runs the folder's
# build code starts, and its Run it key (see WpeSession answer_server_start).
SERVER_START_HEADING = "Opening this file starts its language server:"
SERVER_START_RUN_IT = "\033r"

# Pasting more than a pty holds.  The text goes in chunks, each written only
# when the editor's terminal takes it, so a blocked editor blocks the paste
# instead of the test.  The paste is "stalled" once the editor has taken
# nothing for PASTE_STALL_S: the editor is waiting on something (a program
# that is not reading); the caller is told, once, so it can unblock it.
PASTE_CHUNK = 256
PASTE_STALL_S = 2.0
PASTE_TIMEOUT_S = 120
PASTE_POLL_S = 0.05
# Lines of a paste, and their length with the Enter that ends each.  Twice
# what a pty buffers on Linux and the BSDs (about 20 KB), so a key the
# editor drops instead of waiting for the program cannot hide.
PASTE_LINES = 800
PASTE_LINE_TEXT = "a" * 59
PASTE_BYTES_PER_LINE = len(PASTE_LINE_TEXT) + 1
PASTE_END_LINE = "END"


def paste_text():
    """The text a paste row types: PASTE_LINES lines then PASTE_END_LINE,
    each ended by Enter as a terminal sends it."""
    return (PASTE_LINE_TEXT + "\r") * PASTE_LINES + PASTE_END_LINE + "\r"


def paste_end_text():
    """The line that ends the count, typed once more after the paste.
    When the editor dropped keys it may have dropped the first one, or the
    Enter before it, so this one starts with its own Enter; the program
    then prints what it got instead of waiting for ever.  When nothing was
    dropped the program has ended and these keys go to the editor,
    changing no count."""
    return "\r" + PASTE_END_LINE + "\r"


# How often the end line is typed again while the count has not appeared.
PASTE_END_RETRY_S = 5


def end_paste_until(session, counted, timeout):
    """Type paste_end_text() until ``counted()`` holds or ``timeout``
    seconds (scaled) run out; return ``counted()``.  An editor that drops
    keys may drop each end line too, while the program is still busy with
    what came before it; one that drops nothing is done after the first.
    Extra end lines follow the first in the program's input, so they never
    change its count."""
    deadline = time.time() + timeout * WAIT_SCALE
    while time.time() < deadline:
        if counted():
            return True
        session.paste(paste_end_text(), lambda: None)
        session._drain(PASTE_END_RETRY_S)
    return counted()


def late_reader_c_program(go_path):
    """A C program that prints READY, reads nothing until ``go_path``
    exists, then counts the bytes of every line up to PASTE_END_LINE and
    prints COUNT:<n>.  Keys typed for it before it reads must wait in the
    pty (or in the editor), never be dropped.  The marker is printed with
    a format so the source the editor shows never contains it."""
    return (
        "#include <stdio.h>\n"
        "#include <string.h>\n"
        "#include <unistd.h>\n"
        "int main(void)\n"
        "{\n"
        "  char line[256];\n"
        "  long count = 0;\n"
        "  printf(\"RE%%sDY\\n\", \"A\");\n"
        "  fflush(stdout);\n"
        "  while (access(\"%s\", F_OK) != 0)\n"
        "    usleep(50000);\n"
        "  while (fgets(line, sizeof line, stdin)) {\n"
        "    if (strcmp(line, \"%s\\n\") == 0) break;\n"
        "    count += (long)strlen(line);\n"
        "  }\n"
        "  printf(\"CO%%sNT:%%ld\\n\", \"U\", count);\n"
        "  return 0;\n"
        "}\n" % (go_path, PASTE_END_LINE))

# How the editor learns its screen size, and what the owner does for it
# (gui_owner.Terminal.start; the one place a test starts the editor):
#
# * ncurses reads the pty size with TIOCGWINSZ on stdout, both at initscr()
#   and again on every SIGWINCH (resizeterm -> KEY_RESIZE).  A fresh pty is
#   0x0, so the owner sets the size on the master BEFORE spawning; the slave
#   shares it.
# * The kernel raises SIGWINCH from TIOCSWINSZ only towards the foreground
#   process group of the pty's CONTROLLING terminal, so the owner makes the
#   pty the editor's controlling terminal (TIOCSCTTY); resize() below then
#   needs no signal of its own.
# * LINES / COLUMNS in the environment override the pty size inside ncurses
#   for the initial size AND every relayout: with them set, a resize
#   re-lays-out to the OLD size.  The owner never passes them, whatever a
#   row's env_extra holds.
# The ioctl numbers of the terminal differ by architecture, which is why the
# owner uses termios.TIOCSWINSZ and not a constant.


def server_program(workdir, ui_trace=None, path=None):
    """The Program the build server's JVM runs as for the session in
    @workdir, as the editor chose it, or None when the editor has not said.

    The editor picks the JDK the Scala tools run on (it leaves a too-new
    default aside), so a test that must find or end the program's JVM reads
    that choice from the trace line the editor writes, never repeating the
    rule.  @ui_trace is the trace the session was given, else the one the
    driver gave a Scala session in its folder."""
    if ui_trace is None:
        ui_trace = os.path.join(workdir, SERVER_TRACE_NAME)
    return procinfo.program_from_trace(ui_trace, SERVER_JDK_KEY, path)


def session_env_extra(env_extra):
    """The variables a session passes to its editor on top of the owner's
    allow-list: the data files of this checkout (not a system-installed copy
    that happens to exist on the dev box), then the row's own ``env_extra``,
    which wins.  A value of None removes the variable (a row of the first-run
    question removes XWPE_KEYS; a locale row removes LC_ALL)."""
    extra = {"XWPE_LIB": XWPE_LIB_DIR}
    extra.update(env_extra or {})
    return extra


class _RepStream(pyte.Stream):
    """A pyte Stream that also handles REP (CSI Pn b), SU (CSI Pn S) and
    SD (CSI Pn T).

    ncurses emits REP to compress long identical runs (window borders) on a
    non-UTF-8 terminal; without it pyte drops the repeats and a full-width
    border looks short.  It emits SU/SD inside a scrolling region when every
    line of a window moved by the same count (the AI pane after a few lines
    were appended above its pinned input row, or paged with PgUp) and then
    rewrites only the rows that are new; without them the rows that should
    have scrolled keep their old text.  Extending the class-level ``csi`` map
    is the version-stable way to add a control: the parser binds its CSI
    dispatcher from ``csi`` when the Stream is constructed, so no private
    re-init is needed.  (Older pyte exposed ``_initialize_parser`` for this;
    newer pyte -- the one in Debian -- removed it, which broke the previous
    approach.)
    """
    csi = {**pyte.Stream.csi, "b": "repeat_last",
           "S": "scroll_up", "T": "scroll_down"}

    # The editor asks the terminal for unambiguous key reports and gives them
    # back with private-marked sequences that end in u or m: "CSI > 4 ; 2 m",
    # "CSI > 5 u", "CSI < u", "CSI ? u".  pyte skips a '>' marker and runs
    # the rest (the first as SGR 4;2, underline), and takes '<' for a final
    # byte and draws the 'u'; a real terminal does neither.  They are
    # consumed here, before pyte sees them.  The private modes that do mean
    # something to a screen (CSI ? 25 l, CSI ? 2004 h) end in h or l and are
    # left alone.  A sequence cut by the end of a read is kept for the next.
    _KEY_PROTOCOL_SEQ = re.compile(r"\x1b\[[<=>?][0-9;:]*[um]")
    _CUT_SEQ_AT_END = re.compile(r"\x1b(\[[<=>?0-9;:]*)?\Z")
    _held = ""

    def feed(self, chars):
        chars = self._held + chars
        self._held = ""
        cut = self._CUT_SEQ_AT_END.search(chars)
        if cut:
            self._held = chars[cut.start():]
            chars = chars[:cut.start()]
        super().feed(self._KEY_PROTOCOL_SEQ.sub("", chars))


class ALT:
    """Alt-<key> sequences that open each top-level menu."""
    SYSTEM = "\033#"
    FILE = "\033f"
    EDIT = "\033e"
    SEARCH = "\033s"
    BLOCK = "\033b"
    RUN = "\033r"
    DEBUG = "\033d"
    PROJECT = "\033p"
    OPTIONS = "\033o"
    WINDOW = "\033w"
    HELP = "\033h"
    AI = "\033g"        # Alt-G: the AI assistant prefix (not a menu)


# The start of the reason of an incoherence: the text the harness reads
# (tests/harness/harness.py, KNOWN_XFAIL_PREFIX) to tell a recorded
# incoherence from any other xfail.
INCOHERENCE_PREFIX = gui_owner.INCOHERENCE_PREFIX


def incoherence(reason):
    """Mark a test that asserts behaviour we believe SHOULD hold but which
    currently does not (an incoherence found during coverage).  Recorded as
    xfail so CI stays green; `pytest -rxX` prints the review queue.

    Only a failed assertion is the known incoherence.  A crash, a timeout
    or a refusal inside the row is a different failure and must fail the
    run, not hide behind the marker, so the xfail names AssertionError."""
    return pytest.mark.xfail(reason=INCOHERENCE_PREFIX + reason, strict=False,
                             raises=AssertionError)


def assistant_build(bin_path=None):
    """True when the binary under test has the assistant compiled in.

    The assistant is off by default at configure time, so a distribution build
    usually does not have it, and a test that drives it must skip rather than
    fail there.  The answer is the binary's own (`--version` says
    "enable-ai=yes"), read through build_config, the one reader of what a
    build contains.  A binary that cannot say counts as "no assistant", as it
    always did here, so that collecting a tree with no binary still works.

    New tests use `pytestmark = requires("enable-ai")` (build_config), which
    skips by name and resolves when the row runs; this function stays for the
    callers that ask at import time.
    """
    try:
        return build_config.built("enable-ai", bin_path or WPE_BIN)
    except build_config.BuildUnknown:
        return False


def wait_deadline(tmo):
    """The clock time a wait of ``tmo`` seconds ends at, stretched by the
    wait scale (XWPE_TEST_WAIT_SCALE).

    A row that waits for a trace line or a file with a loop of its own takes
    its end from here, so its wait is stretched like the mock reply it waits
    for: a reply delayed by 5000 * scale ms is not waited for 10 s flat.  A
    caller passes the seconds it needs on an idle machine and never
    multiplies them itself."""
    return time.time() + gui_owner.scaled(tmo)


def wait_screen(session, needle, tmo=12):
    """Poll the screen for ``needle`` until ``tmo`` seconds run out.

    ``tmo`` is the time the wait takes on an idle machine; the owner stretches
    it by the wait scale (wait_deadline), so a caller never passes a time it
    has already multiplied.

    One turn of this loop costs 0.4 s, because `display(drain=0)` reads the
    screen as it stands instead of draining for another 0.6 s first.  That
    matters for anything that is on screen only while work is in flight: a
    waiting line lives exactly as long as the backend takes, and a loop that
    costs a full second per turn cannot look until it has gone.  A box that
    stays up is found either way.

    Fourteen test files grew their own copy of this, ten of them with the
    coarse grid, and one of those missed a 1.5 s spinner for a whole release.
    Every test uses this one; a file that needs another needle shape (the
    pane's words, a trace line) asks wait_until with its own reading.
    """
    def needle_is_shown():
        return needle in "\n".join(session.display(drain=0))
    return wait_until(session, needle_is_shown, tmo, what="%r on the screen" % needle)


# One look at the screen: how long wait_until reads before it asks again.
# Not scaled: the scale stretches the ceiling of a wait, never the grain.
TURN_S = 0.4


def wait_until(session, ready, tmo=12, what="the awaited state"):
    """Drain the editor, one turn of 0.4 s at a time, until ``ready()`` is
    true; False when ``tmo`` seconds (stretched by the wait scale) run out.

    For a row that waits for something other than a needle on the screen: the
    pane's words across wrapped rows, a line of the trace, a file on disk.
    The row writes only what "ready" means; how long it waits is decided
    here, with wait_screen's."""
    end = wait_deadline(tmo)
    gui_owner.ROW_CLOCK.note_wait(what)
    while time.time() < end:
        gui_owner.ROW_CLOCK.check()
        session._read_for(TURN_S, SETTLE_S * WAIT_SCALE)
        if ready():
            return True
    return False


def row_with(session, needle):
    """The first screen row containing ``needle``, or "" if none does.  For
    reading one widget's row (a field, a radio line) without pinning where
    the dialog landed."""
    return next((r for r in session.display() if needle in r), "")


def title_pos(session, title):
    """(row, col) of ``title`` on the screen, or None.  A dialog's title sits
    on its top border, so this is where the box IS; compare two readings to
    assert a move, a re-centre or that a box stayed put."""
    for i, row in enumerate(session.display()):
        c = row.find(title)
        if c >= 0:
            return (i, c)
    return None


def checkbox_mark(session, label):
    """The "[ ]" / "[X]" glyph drawn in front of a checkbox ``label``.

    Reads the mark, not the whole row, so the comparison survives the row
    shifting (a moved or re-centred dialog) and still catches a toggle."""
    row = row_with(session, label)
    assert label in row, "no %r checkbox on screen:\n%s" % (label, "\n".join(session.display()))
    at = row.index(label)
    return row[at - 4:at - 1]


STATE_DIR_MODE = 0o700      # the editor keeps its state folder the user's alone
STATE_FILE_MODE = 0o600     # and each store in it


def write_user_store(path, text):
    """Write one of the editor's stores (start-commands, built-outputs) in
    the user's state folder, as the editor itself writes it: the folder and
    the file the user's alone, whatever the umask.  The editor never believes
    a store that group or others could write, so a test that wrote it with
    the umask (0664 under Debian's 002) would test a refused store instead
    of the answer it planted."""
    folder = os.path.dirname(str(path))
    os.makedirs(folder, exist_ok=True)
    os.chmod(folder, STATE_DIR_MODE)
    with open(str(path), "w") as fh:
        fh.write(text)
    os.chmod(str(path), STATE_FILE_MODE)


# -- reading the AI pane ------------------------------------------------------
#
# The pane is an ordinary framed window whose transcript is word-wrapped to the
# pane's present width before every paint (ai_pane_reflow in we_ai_ui.c).  A
# test that asserts a phrase the pane wrote must therefore read the pane's
# LOGICAL lines, not screen rows: once Messages shares the bottom strip the
# pane is half as wide and a line written at 76 columns spans two rows.
#
# The frame as pyte sees it: ncurses draws the borders in the alternate
# character set, so the horizontal line is "q", the top corners "l" / "k" and
# the bottom corners "m" / "j".
FRAME_HLINE_RUN = "qqq"
FRAME_TOP_LEFT = "l"
FRAME_TOP_RIGHT = "k"
FRAME_BOTTOM_LEFT = "m"

# The debugger's own background colours, named here once.  Four test files
# used to carry a private copy of the mark search and three of them matched any
# greenish background instead of this exact colour, which can paint a green
# where the debugger never stopped.
CURRENT_LINE_BG = "brightgreen"
BREAKPOINT_BG = "red"
# A breakpoint the debugger has not confirmed is drawn as the outline of the
# red bar: the bar's red as the text colour, on the editor's own background.
UNCONFIRMED_BREAKPOINT_FG = "red"

PANE_NAME = "AI"
# ai_pane_width() wraps one column short of the interior (e.x - a.x - 2: the
# caret's column after the input text) and never wider than AI_PANE_MAX_COLS.
PANE_CARET_COLUMNS = 1
PANE_WRAP_MAX_COLS = 80
# How far back pane_text_with_history pages by default, and where a click
# lands to focus the pane (second interior column of the first transcript row).
PANE_HISTORY_PAGES = 4
PANE_KEPT_ROWS = 1              # rows a PgUp keeps of the page below (AI_PAGE_KEEP_ROWS)
PANE_TOP_CONFIRMATIONS = 2      # PgUp returning the same page this often is the top
PANE_CLICK_COLUMN = 2
PANE_CLICK_ROW = 2
KEY_PGUP = "\033[5~"


def title_row(disp, name):
    """The screen row of window ``name``'s title bar, or -1 when it is not
    open.  The name must sit on a row of the frame's horizontal line, so a
    mention of the name inside a file or the pane's own text does not pass
    for the window."""
    for y, r in enumerate(disp):
        if " %s " % name in r and FRAME_HLINE_RUN in r:
            return y
    return -1


def pane_frame(disp, name=PANE_NAME):
    """(title row, left border column, right border column) of the pane
    ``name``; fails with the screen when the pane is not open.  The columns
    are found from the corners nearest the title, so the pane is read
    correctly whether it spans the terminal or shares the strip with
    Messages."""
    y = title_row(disp, name)
    assert y >= 0, "the %s pane is not open:\n%s" % (name, "\n".join(disp))
    title = disp[y]
    at = title.index(" %s " % name)
    x0 = title.rfind(FRAME_TOP_LEFT, 0, at)
    x1 = title.index(FRAME_TOP_RIGHT, at)
    return y, x0, x1


def pane_frame_drawn(disp, name=PANE_NAME):
    """True once the pane ``name`` is open and its title row is drawn to the
    right corner.  A row that reads the pane (pane_frame, pane_rows,
    pane_text_with_history) right after a wait for some text in it can catch
    the frame half painted, with no right corner yet; waiting for this first
    reads it whole."""
    y = title_row(disp, name)
    if y < 0:
        return False
    return FRAME_TOP_RIGHT in disp[y][disp[y].index(" %s " % name):]


def pane_rows(disp, name=PANE_NAME):
    """The interior text of every row of the pane, top to bottom, down to
    its bottom border.  Trailing blanks are KEPT: every row is exactly as
    wide as the pane's interior, which is how pane_logical_lines learns the
    width the wrapper cut at.  rstrip() a row to compare it as text."""
    y, x0, x1 = pane_frame(disp, name)
    rows = []
    for r in disp[y + 1:]:
        if r[x0:x0 + 1] == FRAME_BOTTOM_LEFT:
            break
        rows.append(r[x0 + 1:x1])
    return rows


def pane_wrap_width(rows):
    """The columns the pane wrapped its text to, from the interior width of
    its rows (see PANE_CARET_COLUMNS / PANE_WRAP_MAX_COLS)."""
    if not rows:
        return 0
    width = len(rows[0]) - PANE_CARET_COLUMNS
    if width > PANE_WRAP_MAX_COLS:
        width = PANE_WRAP_MAX_COLS
    return width


def first_word(row):
    """The first word of a row, "" for a blank one."""
    words = row.split()
    if words:
        return words[0]
    return ""


def row_continues(row, next_row, width):
    """Does ``next_row`` carry on the line ``row`` began, by the wrapper's
    rule (ai_wrap_cut in we_ai_ui.c)?

    The wrapper cuts a row "up to and including the last space that fits
    (the space stays on the row, so the rows joined again read exactly as
    written), or a hard cut at the width inside a word longer than the
    row".  The break space cannot be told from the row's padding on the
    screen (the fill has the same attributes), so the same rule is read the
    way the user reads it: the row is full to the wrap width (a hanging
    break space or a hard cut), or the next row's first word would not have
    fitted after this row's last one -- which is exactly why the wrapper
    broke there.  A line that merely ends long, followed by a line opening
    with a long word, reads as one line too; that only ever merges two
    neighbours, it never loses or reorders text, so a phrase searched with
    ``in`` and an order of lines both hold."""
    text = row.rstrip()
    following = first_word(next_row)
    if not text or not following:
        return False
    if len(text) >= width:
        return True
    return len(text) + 1 + len(following) > width


def row_is_hard_cut(row, width):
    """Was ``row`` cut inside one word longer than the pane (no space the
    wrapper could break after)?  Its continuation then glues on without a
    space, as the wrapper took none away."""
    text = row.rstrip()
    return len(text) >= width and " " not in text.strip()


def pane_logical_lines(rows):
    """The rows of the pane joined back into the lines the pane wrote, each
    normalised to single spaces so a phrase can be searched with ``in``
    whatever width the pane had when it was painted.  Rows come from
    pane_rows (trailing blanks kept); a blank row is an empty line."""
    width = pane_wrap_width(rows)
    lines = []
    current = ""
    open_line = False
    hard = False
    for i, row in enumerate(rows):
        text = row.rstrip()
        if open_line and hard:
            current = current + text
        elif open_line:
            current = current + " " + text
        else:
            current = text
        next_row = ""
        if i + 1 < len(rows):
            next_row = rows[i + 1]
        open_line = row_continues(row, next_row, width)
        hard = open_line and row_is_hard_cut(row, width)
        if not open_line:
            lines.append(" ".join(current.split()))
    return lines


def box_text(disp, name):
    """The text of the box titled ``name``: the rows inside its frame, each
    with its blanks squeezed, joined by single spaces.  A phrase the box
    wrapped over two rows ("Save" at the end of one, "As" at the start of the
    next) is then found whole, wherever the wrap fell, which is what a long
    path above the sentence moves.  Fails with the screen when no box has that
    title."""
    y = title_row(disp, name)
    assert y >= 0, "no box titled %s:\n%s" % (name, "\n".join(disp))
    title = disp[y]
    at = title.index(" %s " % name)
    x0 = title.rfind(FRAME_TOP_LEFT, 0, at)
    x1 = title.index(FRAME_TOP_RIGHT, at)
    lines = []
    for row in disp[y + 1:]:
        if row[x0:x0 + 1] == FRAME_BOTTOM_LEFT:
            break
        lines.append(" ".join(row[x0 + 1:x1].split()))
    return " ".join(line for line in lines if line)


def click_into_pane(session, name=PANE_NAME):
    """Give the pane the focus with a mouse click (SGR 1006 press and
    release, 1-based cells) on its first transcript row, so PgUp/PgDn reach
    it.  The focus stays in the pane afterwards: a test that goes on to use
    its file window presses Esc, which leaves the pane the way the user
    does."""
    disp = session.display(drain=0)
    y, x0, _ = pane_frame(disp, name)
    col = x0 + PANE_CLICK_COLUMN
    row = y + PANE_CLICK_ROW
    session.key("\033[<0;%d;%dM" % (col, row), delay=0.2)
    session.key("\033[<0;%d;%dm" % (col, row), delay=0.3)


def missing_from_pane(rows, needles):
    """The needles (words or phrases) no logical line of ``rows`` holds."""
    text = "\n".join(pane_logical_lines(rows))
    return [n for n in needles if n not in text]


def pane_overlap(page, seen):
    """How many rows the bottom of ``page`` (an older view of the transcript)
    shares with the top of ``seen``: the longest k, 1 to len(page) - 1, with
    page[-k:] == seen[:k] and not all blank.  The pane keeps AI_PAGE_KEEP_ROWS
    rows of the page below when PgUp moves a page (we_ai_ui.c), and a PgUp
    stopped by the top of the transcript moves less, so the rows two reads
    share are read off the rows and not assumed.  0 when they share none.

    The pane's own contract is tried first: when the last row of ``page`` is
    the first row of ``seen`` the overlap is that one row, not more.  A long
    path of one repeated letter makes several identical rows, and the longest
    match would take rows of the path for rows read twice.

    What the rows cannot settle: a page that was stopped by the top of the
    transcript and moved less than a page can share more rows than one with
    the page below, and when its last row also equals the first row below, the
    one row is taken and the rest are read twice (a path of identical rows
    across the top edge).  Only the pane's scroll position would tell the two
    apart, and the pane does not show it."""
    if page and seen and page[-1] == seen[0] and seen[0].strip():
        return PANE_KEPT_ROWS
    for k in range(min(len(page) - 1, len(seen)), 0, -1):
        if page[-k:] == seen[:k] and any(row.strip() for row in seen[:k]):
            return k
    return 0


def rows_above(page, seen):
    """``seen`` with the rows of the older ``page`` put above it, the rows
    they share counted once.  A long wrapped line that crosses a page edge
    is then read back as it was written, not with a row of it twice."""
    return page[:len(page) - pane_overlap(page, seen)] + seen


def needs_older_rows(rows, needle_words):
    """Must the pane be read further back for ``needle_words``?  Yes when a
    needle is missing, and also when a needle lies only in the logical line
    that begins on the first row read: that row may be the middle of a
    longer line whose start is above the view, and the phrase the row asserts
    would be cut there."""
    lines = pane_logical_lines(rows)
    text = "\n".join(lines)
    below_the_first = "\n".join(lines[1:])
    first = lines[0] if lines else ""
    for needle in needle_words:
        if needle not in text:
            return True
        if needle in first and needle not in below_the_first:
            return True
    return False


def pane_rows_with_history(session, needle_words, pages=PANE_HISTORY_PAGES,
                           name=PANE_NAME):
    """Every pane row now on screen plus, when needles are missing from
    them (or lie only on the first row's line, see needs_older_rows), the
    rows PgUp brings back into the view, up to ``pages`` pages: the way the
    transcript is read once it is longer than the pane.  Each older page is
    merged by the rows it shares with what was read (rows_above), and paging
    stops at the top of the transcript, where PgUp returns the page it
    returned before.  The needles are the phrases the row asserts.  The pane
    is clicked into first; see click_into_pane for the focus after."""
    rows = pane_rows(session.display(), name)
    if not needs_older_rows(rows, needle_words):
        return rows
    click_into_pane(session, name)
    seen = list(rows)
    last_page = list(rows)
    for _ in range(pages):
        session.key(KEY_PGUP, delay=0.4)
        page = pane_rows(session.display(), name)
        rereads = 1
        while page == last_page and rereads < PANE_TOP_CONFIRMATIONS:
            # The top of the transcript, or a repaint still to come on a
            # loaded machine: the screen is read again, without another PgUp
            # (which would move a second page and lose the one between), and
            # the top is believed when it still shows the page it showed.
            page = pane_rows(session.display(), name)
            rereads += 1
        if page == last_page:
            break
        seen = rows_above(page, seen)
        last_page = page
        if not needs_older_rows(seen, needle_words):
            break
    return seen


def pane_text_with_history(session, needle_words, pages=PANE_HISTORY_PAGES,
                           name=PANE_NAME):
    """The pane's logical lines now visible plus the pages PgUp brings back
    while any of ``needle_words`` is still missing (see
    pane_rows_with_history), joined by pane_logical_lines: a phrase the pane
    wrote is found whether the pane wrapped it over two rows or it scrolled
    above the view.

    ``needle_words`` must be every phrase the row asserts on the result, each
    whole: a needle shorter than the asserted phrase can be found while the
    rest of the phrase is still above the view.  tests/check-test-paths.sh
    (rule P1) fails a row whose asserted phrase is not one of its needles."""
    rows = pane_rows_with_history(session, needle_words, pages, name)
    return pane_logical_lines(rows)


# The title row of a window, as pyte sees it: the corner "l", a run of the
# frame's horizontal line "q", the title between spaces (centred, or cut at
# its head with "..." when it does not fit: we_wind.c e_std_rahmen), the run
# again, the window number, and the zoom and close buttons, then "qk".
WINDOW_BUTTONS = "□ ✕"      # the zoom and close buttons of a window
WINDOW_BUTTONS_FROM_NUMBER = 3        # cells from the window number to them
TITLE_CUT_MARK = "..."
FRAME_FILL = "q"
ASCII_LAST = 0x7F
FRAME_CORNER_CHARS = "kjlmnqtuvwx"    # what may stand just left of a corner


class WindowTitle(collections.namedtuple("WindowTitle", "text elided")):
    """The title of the active window: ``text`` as the editor wrote it and
    ``elided`` when the editor cut it (it ends with TITLE_CUT_MARK), so a row
    that expects a whole name says that the editor, not the row, lost it."""
    __slots__ = ()


def window_buttons_on(row):
    """Does this screen row carry the zoom and close buttons of a window?
    True on the title row of the active window and of no other."""
    return WINDOW_BUTTONS in row


def _title_corner_at(row, buttons_at, after_frame_only):
    """The column of the first "l" before ``buttons_at`` that opens a run of
    the frame's line followed by the title, and (when ``after_frame_only``)
    that is at the row's start or follows a frame character; -1 when none."""
    for x in range(buttons_at):
        if row[x] != "l":
            continue
        if after_frame_only and x > 0 and row[x - 1] not in FRAME_CORNER_CHARS:
            continue
        rest = row[x + 1:buttons_at]
        run = len(rest) - len(rest.lstrip(FRAME_FILL))
        if 0 < run < len(rest):
            return x
    return -1


def _title_corner(row, buttons_at):
    """The column of the left corner of the window whose buttons start at
    ``buttons_at``: a window beside another has the other's edge (a scroll
    arrow, not a frame letter) before its corner, so the strict reading is
    tried first and the loose one second.  A letter "l" of a file name follows
    a space, not a frame letter, so the strict reading skips it."""
    corner = _title_corner_at(row, buttons_at, True)
    if corner < 0:
        corner = _title_corner_at(row, buttons_at, False)
    return corner


def _without_kind_mark(after_corner):
    """The row after the corner without the glyph that marks a window that is
    not a plain file (the padlock of a locked file, the wrench of a tool
    window such as Messages): it stands in the second cell, a wide character
    between the frame's line, and is not part of the title."""
    if after_corner.startswith(FRAME_FILL):
        rest = after_corner[len(FRAME_FILL):]
        if rest and ord(rest[0]) > ASCII_LAST:
            return rest[1:]
    return after_corner


def active_window_title(disp):
    """The WindowTitle of the active window on screen ``disp`` (the rows of
    session.display()), read by column: the corner of the row that carries
    the buttons, the frame's run after it, the title, the run and the window
    number before the buttons.  Never by a pattern on the frame letters: a
    file name with a "q" in it, or a long title the editor cut, has no
    pattern.  Fails with the screen when no window is active."""
    for row in disp:
        at = row.find(WINDOW_BUTTONS)
        if at < 0:
            continue
        corner = _title_corner(row, at)
        if corner < 0:
            continue
        number_at = at - WINDOW_BUTTONS_FROM_NUMBER
        middle = _without_kind_mark(row[corner + 1:number_at]).lstrip(FRAME_FILL)
        text = middle.rstrip(FRAME_FILL).strip()
        return WindowTitle(text, text.endswith(TITLE_CUT_MARK))
    raise AssertionError("no active window on screen:\n" + "\n".join(disp))


# A compiler's row names the file by its whole path: the screen has to hold
# that path and what the compiler says after it.  cols_for gives the width.
COMPILER_LINE_TAIL = 80    # ":3:13: error: 'undef_q' undeclared (first use ..."
SCREEN_ROW_MARGIN = 6      # frame, scrollbar and the cells the editor keeps


def cols_for(*texts, tail=0, at_least=COLS):
    """The screen width for a row that waits for ``texts`` on one screen
    row: ``at_least`` (COLS by default) when they fit, else the longest of
    them plus the ``tail`` the row adds after it (COMPILER_LINE_TAIL for a
    compiler's row) and the frame's margin.  The width follows the path the
    row waits for, not a constant that held while the test's folder was
    short."""
    longest = max([len(text) for text in texts] or [0])
    return max(at_least, longest + tail + SCREEN_ROW_MARGIN)


class WpeSession(procinfo.ChildAsker):
    """A live wpe instance under a pyte-backed pty, with helpers to send
    keys, drive menus, save, and read back the file or the screen.

    ``running(name)`` is the one way a row finds a child of the editor by
    the program it is (a list of pids); it is not ``owned.running()``, the
    owner's bool for whether a process is alive.  ``exec_path`` is the PATH
    the editor was started with and ``asked`` the Program the last
    ``running(name)`` looked for, which ``process_report()`` prints.

    ``real_login=True`` is for the one kind of test that talks to a real
    `claude` CLI: it links the machine's ``~/.claude`` and ``~/.claude.json``
    into the session's HOME so that CLI finds its login.  Off by default,
    because the session's HOME is also the editor's workspace: with the link
    in place every process a test starts can read and write the user's
    credentials, settings and transcripts.  A test with a stub CLI never needs
    it."""

    @staticmethod
    def _refuse_account_login(login_home):
        """Refuse to link a login file that is, by its realpath, in this
        account's own home folder, unless the row is a live file's with the
        live gate open.  It is decided per file (``.claude``,
        ``.claude.json``) and not by the folder HOME names: a HOME in a
        scratch folder whose ``.claude`` is a link into the account's home
        still reaches the user's real login.  A row whose logins are files
        of its own (a fake login in its scratch) is testing the link, not
        the engine, and passes."""
        for name in LOGIN_NAMES:
            src = os.path.join(login_home, name)
            if os.path.lexists(src) and gui_owner.is_account_data(src) \
                    and not gui_owner.current().row_is_live():
                raise gui_owner.OwnerRefused(
                    "real_login=True would link %s, which is this account's "
                    "own login: allowed only to a row of a live file with "
                    "XWPE_TEST_LIVE=1 in the environment of the test run"
                    % src)

    @staticmethod
    def _link_real_login(real_home, workdir):
        """Link the machine's claude login into ``workdir`` (see real_login)."""
        for name in LOGIN_NAMES:
            src = os.path.join(real_home, name)
            dst = os.path.join(workdir, name)
            if os.path.exists(src) and not os.path.exists(dst):
                try:
                    os.symlink(src, dst)
                except OSError:
                    pass

    def __init__(self, workdir, seed, filename="t.c", cols=COLS, rows=ROWS,
                 wait=1.3, env_extra=None, real_login=False,
                 stdin_is_terminal=True, answer_server_start=False,
                 args=(), new_profile=False, inherit=(), expect_crash=None):
        """``args`` are command-line options put before the file name.
        ``expect_crash`` is the signal a row that crashes its editor on
        purpose names (``signal.SIGSEGV``): the editor starts with no core
        file allowed and the row fails unless it died of exactly that
        signal.  It must be given here, at start, so the limit is in place.
        A row that does not give it fails when its editor dies of a crash
        signal.
        ``inherit`` names the variables of the machine this row's editor may
        have on top of the owner's allow-list (an API key for a live row);
        everything else of the machine's environment stays out.
        ``new_profile=True`` leaves the session's HOME with nothing in it
        (the driver otherwise drops a syntax_def under ~/.xwpe, which makes
        it an existing profile), as the rows of the first-run question need.

        ``stdin_is_terminal=False`` feeds the editor's keys through a
        pipe while its screen stays on the pty, as under a desktop
        launcher: the editor's input is then not a terminal, so it has no
        terminal of its own to name.

        ``answer_server_start=True`` is for the rows that test what a real
        language server does (rust-analyzer, Metals): such a server runs the
        folder's build code, so the editor asks once per folder before it
        starts it, and these rows answer that box with Run it, as the user
        would once.  The box itself is tested by test_startplan_run.py."""
        self.answer_server_start = answer_server_start
        self._fds_closed = False
        self.workdir = workdir
        self.path = os.path.join(workdir, filename)
        # A bytes seed is written as it is (a row about line endings or a
        # NUL must say which bytes the editor opens); a str is text, as ever.
        if isinstance(seed, bytes):
            with open(self.path, "wb") as fh:
                fh.write(seed)
        else:
            with open(self.path, "w") as fh:
                fh.write(seed)
        self.screen = SafeScreen(cols, rows)
        self.stream = _RepStream(self.screen)   # Stream + REP (CSI Pn b) handler
        self.raw = bytearray()                  # every byte wpe emits, pre-parse
                                                # (pyte swallows OSC 52, so the
                                                # clipboard test scans this)
        self.master_fd, slave_fd = pty.openpty()
        # The claude CLI backend reads its credentials from $HOME/.claude/ +
        # $HOME/.claude.json, so under HOME=workdir a real login is invisible
        # and a turn against the real CLI comes back "Not logged in".  Only a
        # test that asks for it (real_login) gets the machine's ones linked in;
        # if they do not exist (CI, no login) nothing is linked and that test
        # self-skips.  Every other test keeps a HOME with nothing of the
        # user's in it.  ~/.xwpe stays isolated either way.
        if real_login:
            login_home = os.path.expanduser("~")
            self._refuse_account_login(login_home)
            self._link_real_login(login_home, workdir)
        # Make the editor under test use THIS repo's syntax_def, not whatever
        # stale copy is installed on the machine: it reads $HOME/.xwpe/syntax_def
        # first and we set HOME=workdir, so drop the build-tree copy there.
        _synt = os.path.join(os.path.dirname(os.path.abspath(WPE_BIN)), "syntax_def")
        if os.path.exists(_synt) and not new_profile:
            _xd = os.path.join(workdir, ".xwpe")
            os.makedirs(_xd, exist_ok=True)
            shutil.copyfile(_synt, os.path.join(_xd, "syntax_def"))
        extra_env = session_env_extra(env_extra)
        if filename.endswith(SERVER_PROGRAM_SUFFIXES):
            # The editor says which java it chose in its trace; a session on
            # a Scala file always has one, unless the row names its own.
            extra_env.setdefault("XWPE_UI_TRACE",
                                 os.path.join(workdir, SERVER_TRACE_NAME))
        owner = gui_owner.current()
        self.input_fd = self.master_fd
        stdin_arg = None            # the pty: the editor's input is a terminal
        if not stdin_is_terminal:
            stdin_arg, self.input_fd = os.pipe()
        # The owner starts the editor: the pty is sized before the start and
        # is its controlling terminal, the environment is the owner's
        # allow-list (no display, no key of the machine's) and the row's own
        # extra_env, and the editor is ended, and what it left found, when
        # the session closes or the row ends.
        self.owned = gui_owner.Terminal.start(
            owner, WPE_BIN, list(args) + [filename], workdir, workdir,
            self.master_fd, slave_fd, (cols, rows), stdin_fd=stdin_arg,
            extra_env=extra_env, inherit=inherit,
            label="%s %s" % (os.path.basename(workdir), filename),
            on_output=self._take_output, expect_crash=expect_crash)
        self.proc = self.owned.proc
        # The trace the editor was given (the row's, else the machine's);
        # None: there is no trace to read.
        self.ui_trace = self.owned.env.get("XWPE_UI_TRACE")
        # The PATH the editor's execvp searches (None: the row removed it),
        # which running(name) looks a program up on.
        self.exec_path = self.owned.env.get("PATH")
        os.close(slave_fd)
        if not stdin_is_terminal:
            os.close(stdin_arg)
        self._drain(wait)

    def _take_output(self, data):
        """Keep what the editor wrote: the raw bytes and the screen.  The
        session's own reads and the owner's, while the editor is ended, both
        come here, so nothing the editor says on its way out is lost."""
        self.raw.extend(data)
        self.stream.feed(data.decode("utf-8", "replace"))

    def _answer_server_box(self):
        """Run it, when the language server's start box is up and this
        session was made to answer it (see answer_server_start).  Called
        from _drain as the screen changes, so a key sent with a long delay
        (a server's cold start) is answered at once, not after the delay."""
        if not getattr(self, "answer_server_start", False):
            return
        if SERVER_START_HEADING not in "\n".join(self.screen.display):
            self._server_box_answered = False
            return
        if not getattr(self, "_server_box_answered", False):
            self._server_box_answered = True     # once per box, until it closes
            os.write(self.input_fd, SERVER_START_RUN_IT.encode())

    # -- context manager -----------------------------------------------------
    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # -- internals -----------------------------------------------------------
    def _drain(self, timeout):
        """Read what the editor writes for up to ``timeout`` seconds (scaled).

        With SETTLE_S set, return early once output has arrived and the
        editor has then been silent for SETTLE_S: the screen has settled.
        """
        # A slower machine pauses longer between two paints, so the quiet
        # window stretches with the same factor as every other wait.
        self._read_for(timeout * WAIT_SCALE, SETTLE_S * WAIT_SCALE)

    def _read_for(self, seconds, settle):
        """Read what the editor writes for @seconds, as they are (no scale),
        returning early once output has arrived and @settle seconds of quiet
        have followed.

        The scale is for ceilings.  The grain of looking (TURN_S in
        wait_until) is read through here unscaled, so that a state shown for
        a second is not missed between two looks on a slow machine."""
        deadline = time.time() + seconds
        last_output = None
        while time.time() < deadline:
            gui_owner.ROW_CLOCK.check()
            poll = DRAIN_POLL_S
            if settle and last_output is not None:
                quiet_left = last_output + settle - time.time()
                if quiet_left <= 0:
                    return
                poll = min(poll, quiet_left)
            r, _, _ = select.select([self.master_fd], [], [], poll)
            if r:
                try:
                    data = os.read(self.master_fd, 65536)
                except OSError:
                    break
                if not data:
                    break
                self.raw.extend(data)
                self.stream.feed(data.decode("utf-8", "replace"))
                last_output = time.time()
                self._answer_server_box()

    def osc52_payload(self):
        """Decode the most recent OSC 52 "set clipboard" sequence wpe emitted
        (ESC ] 52 ; c ; <base64> BEL), or None. Used to assert that a plain
        Copy pushed the selection to the OS clipboard."""
        raw = bytes(self.raw)
        marker = b"\033]52;c;"
        i = raw.rfind(marker)
        if i < 0:
            return None
        j = raw.find(b"\a", i)
        if j < 0:
            return None
        try:
            return base64.b64decode(raw[i + len(marker):j]).decode("utf-8", "replace")
        except Exception:
            return None

    # -- input ---------------------------------------------------------------
    def key(self, *keys, delay=0.45):
        """Send raw key strings, draining wpe output after each. Every key is
        written to the pty as given: no keyboard layer lies between the
        harness and the editor, on any system."""
        for k in keys:
            os.write(self.input_fd, k.encode())
            self._drain(delay)
        return self

    def paste(self, text, when_stalled):
        """Type ``text`` as fast as the editor takes it, draining its
        output meanwhile.  ``when_stalled()`` is called once, when the
        editor has taken nothing for PASTE_STALL_S or the whole text is
        written, whichever comes first.  Returns the bytes written."""
        data = text.encode()
        done = 0
        told = False
        last_progress = time.time()
        deadline = time.time() + PASTE_TIMEOUT_S * WAIT_SCALE
        os.set_blocking(self.input_fd, False)
        try:
            while done < len(data) and time.time() < deadline:
                _, writable, _ = select.select([], [self.input_fd], [],
                                               PASTE_POLL_S)
                if writable:
                    try:
                        done += os.write(self.input_fd,
                                         data[done:done + PASTE_CHUNK])
                        last_progress = time.time()
                    except BlockingIOError:
                        pass
                if not told and time.time() - last_progress > PASTE_STALL_S:
                    when_stalled()
                    told = True
                self._drain(PASTE_POLL_S)
        finally:
            os.set_blocking(self.input_fd, True)
        if not told:
            when_stalled()
        return done

    def resize(self, cols, rows, delay=0.6):
        """Resize the terminal: set the pty window size (the kernel sends
        SIGWINCH to wpe, which is its controlling terminal) and the pyte
        screen to match, then drain the repaint.  This is the one sanctioned
        way to drive the ncurses KEY_RESIZE -> resizeterm ->
        e_relayout_windows path, the terminal peer of an X11 ConfigureNotify /
        a Wayland xdg configure; do not kill(SIGWINCH) on top of it, a second
        KEY_RESIZE can double the relayout and hide ordering bugs."""
        gui_owner.set_pty_size(self.master_fd, cols, rows)
        try:
            self.screen.resize(rows, cols)
        except Exception:
            pass
        self._drain(delay)
        return self

    def menu(self, alt_seq, item, delay=0.45):
        """Open a top-level menu (ALT.<X>) and pick an item by its letter."""
        return self.key(alt_seq, item, delay=delay)

    def save(self):
        """File -> Save."""
        return self.key(ALT.FILE, "s", delay=0.5)

    # -- output --------------------------------------------------------------
    def text(self):
        """The file as written to disk (call save() first)."""
        with open(self.path) as fh:
            return fh.read()

    def disk(self):
        """The file's bytes as written to disk (call save() first).  text()
        reads in text mode and turns CR LF into LF, so a test about line
        endings reads the bytes with this."""
        with open(self.path, "rb") as fh:
            return fh.read()

    def display(self, drain=0.6):
        """The current pyte screen as a list of row strings.

        Drains pending pty bytes for up to ``drain`` seconds first so a caller
        that did `time.sleep(N)` after a key (instead of `key()`'s built-in
        drain) still sees output that arrived during the sleep.  Without this
        the slower-to-compile/run macOS pipeline left the run's "XQZ42",
        "End output" and "Return-Code" lines stuck in the kernel pty buffer
        and the display showed the pre-run snapshot."""
        if drain > 0:
            self._drain(drain)
        return self.screen.display

    def cell_bg(self, y, x):
        """Background colour name of the cell at row y, col x (pyte parses the
        SGR colour the terminal emitted, e.g. 'red').  'default' if unstyled."""
        return self.screen.buffer[y][x].bg

    def row_has_bg(self, y, bg):
        """True if any cell on row y has background colour `bg` -- used to find
        an inline mark without pinning the exact column."""
        row = self.screen.buffer[y]
        return any(row[x].bg == bg for x in range(self.screen.columns))

    def source_rows(self, drain=0.0):
        """Screen rows of the topmost source window's text area: between its
        top border (row 1) and the first bottom border below it, so Messages
        and Watches are left out."""
        disp = self.display(drain=drain)
        last = len(disp) - 1
        for y in range(2, len(disp)):
            if disp[y].startswith(FRAME_BOTTOM_LEFT):
                last = y
                break
        return range(2, last)

    def rows_with_bg(self, bg, drain=0.0):
        """Screen rows of the source window carrying background colour `bg`.

        The border columns are skipped: they hold the box drawing, not text."""
        rows = []
        for y in self.source_rows(drain=drain):
            row = self.screen.buffer[y]
            if any(row[x].bg == bg for x in range(1, self.screen.columns - 1)):
                rows.append(y)
        return rows

    def rows_with_fg(self, fg, drain=0.0):
        """Screen rows of the source window carrying text of foreground colour
        `fg`, as rows_with_bg does for a background.

        The border columns are skipped: they hold the box drawing, not text."""
        rows = []
        for y in self.source_rows(drain=drain):
            row = self.screen.buffer[y]
            if any(row[x].fg == fg and row[x].data.strip()
                   for x in range(1, self.screen.columns - 1)):
                rows.append(y)
        return rows

    def unconfirmed_breakpoint_lines(self, fg=UNCONFIRMED_BREAKPOINT_FG,
                                     drain=0.0):
        """The source lines drawn as an unconfirmed breakpoint: the bar's red
        (or `fg`, when the colour file sets another) as the text colour, on
        the editor's background rather than on red."""
        lines = []
        for y in self.rows_with_fg(fg, drain=drain):
            row = self.screen.buffer[y]
            if not any(row[x].bg == BREAKPOINT_BG
                       for x in range(1, self.screen.columns - 1)):
                lines.append(y - 1)
        return lines

    def mark_line(self, drain=0.0):
        """The source line carrying the debugger's current-line mark, or None.

        This is where a stop is visible to the user.  The status row cannot
        answer that on its own, because it also shows the line the cursor was
        already on before Run."""
        rows = self.rows_with_bg(CURRENT_LINE_BG, drain=drain)
        if not rows:
            return None
        return rows[0] - 1

    def breakpoint_lines(self, drain=0.0):
        """The source lines the margin marks as breakpoints."""
        return [y - 1 for y in self.rows_with_bg(BREAKPOINT_BG, drain=drain)]

    def alive(self):
        """True while wpe is still running healthily."""
        return self.proc.poll() is None

    def ended_within(self, tmo, what="the editor to end"):
        """Wait for the editor to end, up to @tmo seconds (stretched by the
        wait scale), and return its exit code.  A row that asserts the editor
        closed after a key reads this and not alive(): one read right after
        the key sees the editor still on its way out on a loaded machine.
        The failure names what was waited for and the scale."""
        end = wait_deadline(tmo)
        while self.proc.poll() is None:
            if time.time() >= end:
                raise gui_owner.OwnerTimeout(
                    "waited %.1f s (scale %.1f) for %s; the editor is still running"
                    % (tmo * WAIT_SCALE, WAIT_SCALE, what))
            self._read_for(TURN_S, SETTLE_S * WAIT_SCALE)
        return self.proc.returncode

    def process_report(self):
        """What to add to the message of a check that looked for a child of
        the editor and did not find it: the processes below the editor as
        the helpers and the system's table describe them, and the last lines
        of the editor's own trace (when the session writes one, the file
        XWPE_UI_TRACE names), where a program that could not be started says
        which step failed and why."""
        return "%s\n%s" % (procinfo.describe_children(self.proc.pid,
                                                       self.asked),
                           ui_trace_tail(self.ui_trace))

    def exit_code(self):
        return self.proc.poll()

    def close(self):
        """End the editor through the owner and return its exit status as it
        was before (None while it ran).

        The owner ends it (SIGTERM, the editor's own ladder, SIGKILL), reading
        its terminal meanwhile, then finds and ends what it left.  What it
        left errors the row when the row ends, not here: close() runs in the
        exit of a ``with`` block, where an error would replace the failure
        the row itself had."""
        code = self.proc.poll()
        self.owned.end_for_row()
        self._close_terminal_fds()
        self._end_programs_left_under_a_server()
        return code

    def _close_terminal_fds(self):
        """Close the terminal's master side and the keys' pipe, once: a
        second close() must not close a number the process has since been
        given for something else."""
        if self._fds_closed:
            return
        self._fds_closed = True
        for fd in {self.master_fd, self.input_fd}:
            try:
                os.close(fd)
            except OSError:
                pass

    def server_program(self):
        """The Program the build server's JVM runs as for this session, as
        the editor chose it (None before it has said)."""
        path = self.exec_path or procinfo.EXECVP_DEFAULT_PATH
        return server_program(self.workdir, self.ui_trace, path)

    def _end_programs_left_under_a_server(self):
        """End the program a shared build server ran for this session.

        A Scala session's program is started by the build server (Bloop),
        which other editors share and which is outside the editor's
        process group, so the group kill above never reaches it; and a
        program the editor was killed beside, held at a breakpoint, stays
        suspended for as long as the server runs.  Only a JVM whose
        working directory is this session's own folder is ended, never the
        server and never another session's program."""
        if not self.path.endswith(SERVER_PROGRAM_SUFFIXES):
            return
        if not procinfo.has_procfs():
            return
        jvm = self.server_program()
        if jvm is None:
            return                  # the editor never chose a JDK: no program
        for pid in procinfo.processes_running(jvm, self.workdir):
            try:
                os.kill(pid, 9)
            except ProcessLookupError:
                pass
        deadline = time.time() + SERVER_PROGRAM_GONE_S
        while (procinfo.processes_running(jvm, self.workdir)
               and time.time() < deadline):
            time.sleep(SERVER_PROGRAM_POLL_S)


# The row clock (gui_owner.ROW_CLOCK) never cuts a row while its editor is
# being started or ended: a cut half way would leave the editor to the sweep
# instead of ending it whole.
gui_owner.ROW_CLOCK.defer_in(WpeSession.__init__.__code__)
gui_owner.ROW_CLOCK.defer_in(WpeSession.close.__code__)
