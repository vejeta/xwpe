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
import fcntl
import os
import pty
import select
import struct
import termios
import shutil
import subprocess
import sys
import time

import pytest
import pyte

from test_utf8_border import SafeScreen

WPE_BIN = os.environ.get("WPE_BIN") or os.path.join(os.path.dirname(__file__), "..", "wpe")

# The data files xwpe loads at runtime (the in-app help, syntax_def) live under
# $XWPE_LIB when set, else the compiled-in install prefix. Point it at the
# checkout so the in-tree help.xwpe_eng is found via the editor's "_eng"
# fallback: the help/manual scenarios then pass from a plain build tree on any
# OS, instead of silently depending on a system-installed copy that happens to
# exist on the Linux dev box but not on a fresh macOS runner.
XWPE_LIB_DIR = os.path.dirname(os.path.abspath(WPE_BIN))

# macOS reserves the F1-F12 keys at the OS / terminal layer, so they never reach
# wpe -- macOS users drive those actions from the menu instead, which is the
# supported path there. Map each advertised function-key accelerator the tests
# send to its menu route, applied ONLY on macOS so every Linux scenario stays
# byte-for-byte unchanged. (The letters match we_menue.c in programming mode.)
MACOS = sys.platform == "darwin" or os.environ.get("XWPE_TEST_OS") == "macos"

_MACOS_KEY_ROUTE = {
    "\033OP": ["\033h", "e"],    # F1 -> Help -> Editor (manual viewer)
    "\033OQ": ["\033f", "s"],    # F2 -> File -> Save
    "\033OR": ["\033f", "m"],    # F3 -> File -> File-Manager
    "\033OS": ["\033s", "f"],    # F4 -> Search -> Find
    "\033[20~": ["\033r", "m"],  # F9 -> Run -> Make
    "\x0c": ["\033s", "s"],      # ^L -> Search -> Search again
}


def macos_key_route(k):
    """The keystrokes to actually send for one logical key. Identity on Linux;
    on macOS a dead function-key accelerator becomes its equivalent menu route."""
    if MACOS and k in _MACOS_KEY_ROUTE:
        return _MACOS_KEY_ROUTE[k]
    return [k]

# A loaded CI runner is 3-5x slower than a dev box, so the fixed wait/timeout
# budgets that pass in-tree flake under autopkgtest. XWPE_TEST_WAIT_SCALE
# stretches every wait without slowing the local run (default 1.0; the Debian
# autopkgtest sets it to 3). All waits funnel through _drain(), so scaling there
# scales the startup wait AND every per-key delay.
WAIT_SCALE = float(os.environ.get("XWPE_TEST_WAIT_SCALE", "1") or 1)

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

# How the editor learns its screen size, and why the harness must not get in
# the way:
#
# * ncurses reads the pty size with TIOCGWINSZ on stdout, both at initscr()
#   and again on every SIGWINCH (resizeterm -> KEY_RESIZE).  A fresh pty is
#   0x0, so the driver sets the size on the master BEFORE spawning; the slave
#   shares it.
# * The kernel raises SIGWINCH from TIOCSWINSZ only towards the foreground
#   process group of the pty's CONTROLLING terminal.  A child that merely
#   setsid()s has no controlling terminal and never hears about a resize, so
#   the child adopts the pty with TIOCSCTTY right after setsid().
# * LINES / COLUMNS in the environment override the pty size inside ncurses
#   (use_env(TRUE) is the default), for the initial size AND every relayout:
#   with them set, a resize re-lays-out to the OLD size.  They are never
#   exported to the child; any inherited copy is dropped.
PTY_WINSIZE_FORMAT = "HHHH"             # struct winsize: rows, cols, xpixel, ypixel
SIZE_ENV_VARS = ("LINES", "COLUMNS")
# In the forked child fd 0 is already the pty slave.  The number, not
# sys.stdin.fileno(): under pytest sys.stdin is a capture stub whose fileno()
# raises, and an exception in the pre-exec step aborts the spawn.
CHILD_TTY_FD = 0


def set_pty_size(master_fd, cols, rows):
    """Set the window size of the pty behind ``master_fd`` (TIOCSWINSZ).

    Use ``termios.TIOCSWINSZ`` rather than a hardcoded number: the terminal
    ioctl numbers differ by architecture (ppc64el, mips, alpha, sparc use the
    BSD-style encoding) and a wrong constant fails with ENOTTY there.
    Once the pty is the child's controlling terminal this ioctl alone makes
    the kernel send SIGWINCH to the child; no explicit signal is needed."""
    winsize = struct.pack(PTY_WINSIZE_FORMAT, rows, cols, 0, 0)
    fcntl.ioctl(master_fd, termios.TIOCSWINSZ, winsize)


def adopt_pty_as_controlling_tty():
    """Pre-exec step for the child: start a new session, then make the pty on
    stdin its controlling terminal (TIOCSCTTY) so TIOCSWINSZ on the master
    reaches it as SIGWINCH and ncurses turns it into KEY_RESIZE.

    Runs in the forked child between the fd setup (stdin is already the pty
    slave) and the exec.  TIOCSCTTY exists on Linux, the BSDs and macOS; the
    guard only keeps the harness importable elsewhere."""
    os.setsid()
    if hasattr(termios, "TIOCSCTTY"):
        fcntl.ioctl(CHILD_TTY_FD, termios.TIOCSCTTY, 0)


def strip_size_env(env):
    """Remove LINES / COLUMNS from ``env`` so ncurses sizes itself from the
    pty (see SIZE_ENV_VARS above); returns ``env`` for chaining."""
    for name in SIZE_ENV_VARS:
        env.pop(name, None)
    return env


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


def incoherence(reason):
    """Mark a test that asserts behaviour we believe SHOULD hold but which
    currently does not (an incoherence found during coverage).  Recorded as
    xfail so CI stays green; `pytest -rxX` prints the review queue."""
    return pytest.mark.xfail(reason="INCOHERENCE: " + reason, strict=False)


def assistant_build(bin_path=None):
    """True when the binary under test has the assistant compiled in.

    The assistant is off by default at configure time, so a distribution build
    usually does not have it, and a test that drives it must skip rather than
    fail there.  The marker is a sentence from the assistant's own prompt,
    which no other part of the editor contains: measured on a build configured
    without it, the string is absent while strings a test might pick instead,
    such as a menu label, are still there because they sit in conditional
    blocks of files that always compile.

    Use it as `pytestmark = pytest.mark.skipif(not assistant_build(),
    reason="wpe built without --enable-ai")` whenever a test opens the
    assistant's pane, its menu or its page in Options.
    """
    try:
        out = subprocess.run(["strings", os.path.abspath(bin_path or WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


def wait_screen(session, needle, tmo=12):
    """Poll the screen for ``needle`` until ``tmo`` seconds run out.

    One turn of this loop costs 0.4 s, because `display(drain=0)` reads the
    screen as it stands instead of draining for another 0.6 s first.  That
    matters for anything that is on screen only while work is in flight: a
    waiting line lives exactly as long as the backend takes, and a loop that
    costs a full second per turn cannot look until it has gone.  A box that
    stays up is found either way.

    Fourteen test files grew their own copy of this, ten of them with the
    coarse grid, and one of those missed a 1.5 s spinner for a whole release.
    New tests use this one; a copy is only kept where its file needs a
    different needle shape.
    """
    end = time.time() + tmo
    while time.time() < end:
        session._drain(0.4)
        if needle in "\n".join(session.display(drain=0)):
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


def tool_usable(name, *args):
    """True only if `name` is on PATH AND actually runs (exit 0).

    shutil.which() finds the executable file, but a rustup / asdf / mise shim
    can sit on PATH and still fail at *runtime* -- no default toolchain set, a
    rust-toolchain.toml pinning a toolchain that is not installed, a component
    (e.g. rust-analyzer) not added.  A which()-only skip guard then lets the
    test RUN and FAIL with a confusing toolchain error instead of self-skipping.
    Probing `name --version` (or the given args) and checking the exit status is
    what distinguishes "installed and usable" from "a shim that errors", so a
    misconfigured toolchain skips with a precise reason -- matching the rest of
    the suite, which self-skips when a tool is absent."""
    path = shutil.which(name)
    if path is None:
        return False
    probe = list(args) if args else ["--version"]
    try:
        return subprocess.run(
            [path, *probe],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            timeout=30,
        ).returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


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
PANE_NAME = "AI"
# ai_pane_width() wraps one column short of the interior (e.x - a.x - 2: the
# caret's column after the input text) and never wider than AI_PANE_MAX_COLS.
PANE_CARET_COLUMNS = 1
PANE_WRAP_MAX_COLS = 80
# How far back pane_text_with_history pages by default, and where a click
# lands to focus the pane (second interior column of the first transcript row).
PANE_HISTORY_PAGES = 4
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


def pane_rows_with_history(session, needle_words, pages=PANE_HISTORY_PAGES,
                           name=PANE_NAME):
    """Every pane row now on screen plus, when needles are missing from
    them, the rows PgUp brings back into the view, up to ``pages`` pages:
    the way the transcript is read once it is longer than the pane.  The
    pane is clicked into first; see click_into_pane for the focus after."""
    rows = pane_rows(session.display(), name)
    if not missing_from_pane(rows, needle_words):
        return rows
    click_into_pane(session, name)
    seen = list(rows)
    for _ in range(pages):
        session.key(KEY_PGUP, delay=0.4)
        page = pane_rows(session.display(), name)
        seen = page + seen
        if not missing_from_pane(seen, needle_words):
            break
    return seen


def pane_text_with_history(session, needle_words, pages=PANE_HISTORY_PAGES,
                           name=PANE_NAME):
    """The pane's logical lines now visible plus the pages PgUp brings back
    while any of ``needle_words`` is still missing (see
    pane_rows_with_history), joined by pane_logical_lines: a phrase the pane
    wrote is found whether the pane wrapped it over two rows or it scrolled
    above the view."""
    rows = pane_rows_with_history(session, needle_words, pages, name)
    return pane_logical_lines(rows)


class WpeSession:
    """A live wpe instance under a pyte-backed pty, with helpers to send
    keys, drive menus, save, and read back the file or the screen."""

    def __init__(self, workdir, seed, filename="t.c", cols=COLS, rows=ROWS,
                 wait=1.3, env_extra=None):
        self.workdir = workdir
        self.path = os.path.join(workdir, filename)
        with open(self.path, "w") as fh:
            fh.write(seed)
        self.screen = SafeScreen(cols, rows)
        self.stream = _RepStream(self.screen)   # Stream + REP (CSI Pn b) handler
        self.raw = bytearray()                  # every byte wpe emits, pre-parse
                                                # (pyte swallows OSC 52, so the
                                                # clipboard test scans this)
        self.master_fd, slave_fd = pty.openpty()
        set_pty_size(self.master_fd, cols, rows)    # the child inherits it
        env = strip_size_env(os.environ.copy())
        env.update(TERM="xterm-256color", LC_ALL="en_US.UTF-8", HOME=workdir)
        # The editor reads $XDG_CONFIG_HOME and $XDG_STATE_HOME before $HOME,
        # so a value inherited from the machine (GitHub's runner exports
        # XDG_CONFIG_HOME) would make every session share one xwperc, one
        # key file and one session store, and tests would fail on what an
        # earlier test saved.  An empty value is "unset" to the editor, so
        # both fall back under this session's HOME; a test that passes its
        # own value in env_extra still wins.
        env.update(XDG_CONFIG_HOME="", XDG_STATE_HOME="")
        # Load help/data from the checkout (hermetic), not a system-installed
        # copy; an explicit caller-set XWPE_LIB still wins.
        env.setdefault("XWPE_LIB", XWPE_LIB_DIR)
        # HOME=workdir gives xwpe a clean ~/.xwpe, but it also HIDES per-HOME
        # toolchain config: the rustup `rustc` / `cargo` / `rust-analyzer` shims
        # resolve the default toolchain from $HOME/.rustup, so under workdir they
        # die with "rustup could not choose a version of rustc ... one wasn't
        # configured" and every Rust compile/debug/LSP test breaks -- even though
        # the toolchain is installed and works in a normal shell. Point the
        # toolchain managers back at the real home (only if the user has not set
        # them) so the shims keep resolving under the test HOME.
        _real_home = os.path.expanduser("~")
        env.setdefault("RUSTUP_HOME", os.path.join(_real_home, ".rustup"))
        env.setdefault("CARGO_HOME", os.path.join(_real_home, ".cargo"))
        # Same problem for the claude CLI backend: it reads its credentials from
        # $HOME/.claude/ + $HOME/.claude.json, so under HOME=workdir a real login
        # is invisible and every claudecli turn comes back "Not logged in".  Link
        # the real ones into the test HOME (read-only use) so a claudecli-live
        # test can authenticate; if they do not exist (CI, no login) nothing is
        # linked and that test simply self-skips.  ~/.xwpe stays isolated.
        for _c in (".claude", ".claude.json"):
            _src = os.path.join(_real_home, _c)
            _dst = os.path.join(workdir, _c)
            if os.path.exists(_src) and not os.path.exists(_dst):
                try:
                    os.symlink(_src, _dst)
                except OSError:
                    pass
        # The menu/file/etc. tests do not exercise the LSP, but opening t.c
        # on a box with clangd installed (e.g. Homebrew on macOS) pops up a
        # "Starting language server..." Messages window that overlays the
        # editor and breaks tests that screenshot the chrome (Help viewer,
        # window operations, ...).  Opt out of the eager-on-open boot here;
        # the LSP-specific tests use their own driver and override this.
        env.setdefault("XWPE_LSP_NO_EAGER", "1")
        # Tests that exercise locale-dependent behaviour (e.g. the non-UTF-8
        # chrome fallback) pass env_extra to override LC_ALL/LANG; applied last
        # so the override wins over the UTF-8 default above.
        if env_extra:
            env.update(strip_size_env(dict(env_extra)))
        # Make the editor under test use THIS repo's syntax_def, not whatever
        # stale copy is installed on the machine: it reads $HOME/.xwpe/syntax_def
        # first and we set HOME=workdir, so drop the build-tree copy there.
        _synt = os.path.join(os.path.dirname(os.path.abspath(WPE_BIN)), "syntax_def")
        if os.path.exists(_synt):
            _xd = os.path.join(workdir, ".xwpe")
            os.makedirs(_xd, exist_ok=True)
            shutil.copyfile(_synt, os.path.join(_xd, "syntax_def"))
        self.proc = subprocess.Popen(
            [WPE_BIN, filename], stdin=slave_fd, stdout=slave_fd,
            stderr=slave_fd, cwd=workdir, env=env,
            preexec_fn=adopt_pty_as_controlling_tty)
        os.close(slave_fd)
        self._drain(wait)

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
        deadline = time.time() + timeout * WAIT_SCALE
        # A slower machine pauses longer between two paints, so the quiet
        # window stretches with the same factor as every other wait.
        settle = SETTLE_S * WAIT_SCALE
        last_output = None
        while time.time() < deadline:
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
        """Send raw key strings, draining wpe output after each. On macOS a
        function-key accelerator is replaced by its menu route (see
        macos_key_route); on Linux every key is sent verbatim."""
        for k in keys:
            for stroke in macos_key_route(k):
                os.write(self.master_fd, stroke.encode())
                self._drain(delay)
        return self

    def resize(self, cols, rows, delay=0.6):
        """Resize the terminal: set the pty window size (the kernel sends
        SIGWINCH to wpe, which is its controlling terminal) and the pyte
        screen to match, then drain the repaint.  This is the one sanctioned
        way to drive the ncurses KEY_RESIZE -> resizeterm ->
        e_relayout_windows path, the terminal peer of an X11 ConfigureNotify /
        a Wayland xdg configure; do not kill(SIGWINCH) on top of it, a second
        KEY_RESIZE can double the relayout and hide ordering bugs."""
        set_pty_size(self.master_fd, cols, rows)
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

    def alive(self):
        """True while wpe is still running healthily."""
        return self.proc.poll() is None

    def exit_code(self):
        return self.proc.poll()

    def close(self):
        code = self.proc.poll()
        try:
            os.killpg(os.getpgid(self.proc.pid), 9)
        except Exception:
            pass
        try:
            os.close(self.master_fd)
        except Exception:
            pass
        self.proc.wait()
        return code
