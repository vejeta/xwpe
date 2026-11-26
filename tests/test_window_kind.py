"""Which window a key belongs to, when the window on top holds no text.

One field of a window carries four different things: the text of a file, the
File Manager's directory lists, a list window's rows (the project, the
window list, the search results) or a dropdown's.  Nothing but convention
stops a command from reading one as another, and two paths let that happen
from a key:

* a language-server command reads the word at the cursor before it asks what
  the window holds, so Alt-Q U and Alt-Q N -- and the Modern style's own F2,
  F12 and Ctrl-Space -- read a list's corner numbers as a row pointer;
* a build that ends while the text loop waits for a key puts the window the
  user was in back on top, and the loop goes on with the payload it took
  before the wait, so the next key is handled as text whatever is on top.

The rows drive the real editor (wpe, terminal mode) and read the screen, the
files on disk and the editor's trace.  The same rows run against the
sanitizer build, where the second path is a report instead of a write
through a list's corner numbers taken for a row pointer.

What the user gets once these rows are green: a command that works on text,
asked of a window that has none, says "<Action> needs a text window" on the
status row and changes nothing; a key pressed after a build is handled
by the window the user is looking at; and a file named like one of the
editor's own windows (Messages, Watches, Stack, Help, Buffer, AI, Metals
Doctor, a manual page) is the user's file, opened, typed in and saved as any
other, while the editor's own window of that name is a window beside it (the
rows from K23 on, which read the user's file back from the disk).

Run: tests/.venv/bin/python -m pytest -v tests/test_window_kind.py
"""
import os
import re
import subprocess
import sys

import pytest

from host_tools import require
from wpe_driver import (ALT, FRAME_BOTTOM_LEFT, WPE_BIN, WpeSession,
                        pane_frame, wait_screen, wait_until)

F2 = "\033OQ"                # Modern: Rename
F3 = "\033OR"                # the File Manager
F9 = "\033[20~"              # Make: compile and link
F12 = "\033[24~"             # Modern: Go to definition
CTRL_F9 = "\033[20;5~"       # Run
ALT_F9 = "\033[20;3~"        # Compile
CTRL_G = "\x07"              # the debugger prefix
CTRL_O = "\x0f"              # Modern: the File Manager
CTRL_SPACE = "\x1b[27;5;32~" # Modern: Complete
ESC = "\033"
ENTER = "\r"

ALT_0 = "\0330"              # the window list
ALT_1 = "\0331"              # bring window 1 to the front
ALT_2 = "\0332"              # bring window 2 to the front
ALT_A = "\033a"              # Run > Execute make
ALT_C = "\033c"              # Compile
ALT_F = "\033f"              # the File menu
ALT_L = "\033l"              # Install
ALT_M = "\033m"              # Make
ALT_P = "\033p"              # the Project menu
ALT_Q = "\033q"              # the language-server prefix
ALT_U = "\033u"              # Run

MAIN_C = "main.c"
PROJECT_TITLE = "Project: demo.prj"
WINDOW_LIST_TITLE = "Windows"
SOURCE = "int main(void){return 0;}\n"
PROJECT = "CMP=\tgcc\nEXENAME=\tdemo\nFILES=\tmain.c\n"
# The text of a file the user named after one of the editor's own panes.
USER_NOTES = "the user's own notes\n"
# A source big enough that its compile is still running when the next key is
# pressed, so the build's end lands while a box the key opened owns the
# screen.  It carries a main() so the build succeeds: only a build that
# succeeded gives the keys back to the window the user was in, which is the
# state these rows judge.
SLOW_SOURCE_FUNCTIONS = 40000
SLOW_SOURCE = ("int main(void){return 0;}\n" +
               "".join("int f%d(void){return %d;}\n" % (i, i)
                       for i in range(SLOW_SOURCE_FUNCTIONS)))

# A make that names an error in another file after a pause: a build long
# enough to end while the editor waits for a key, or while a box is up.
SLOW_ERROR_MAKEFILE = (
    "all:\n"
    "\t@echo BUILD_START\n"
    "\t@sleep 4\n"
    "\t@echo \"b.c:3:5: error: STUB_ERROR\" >&2\n"
    "\t@false\n"
)
BUILD_SRC = "int main(void)\n{\n    undef_x = 1;\n    return 0;\n}\n"
BUILD_START = "BUILD_START"
DEFERRED_BUILD = "paint deferred the build's end"
REPLAYED_BUILD = "paint replay the build's end"
# What the screen's owner writes when a box's own key read saw the window on
# top change.  A box reads at paint depth one or more, where the owner keeps
# a build's end behind it, so the line is never written and a row holds that.
BOX_SAW_TOP_CHANGE = "a box's key read saw the window on top change"
# The file a slow successful make leaves behind, so a row waits for the
# build's end instead of guessing how long this machine takes.
BUILD_DONE_FILE = "BUILT"
# A make that takes its time and then succeeds: the build's end lands while
# whatever the user opened next is still on the screen, and the keys go back
# to the window he was in.  It leaves a file behind, so a row can wait for
# the end instead of guessing how long the machine takes.
SLOW_OK_MAKEFILE = (
    "all:\n"
    "\t@echo BUILD_START\n"
    "\t@sleep 6\n"
    "\t@echo built\n"
    "\t@touch " + BUILD_DONE_FILE + "\n"
)
TRACE = "ui.trace"

OPEN_WAIT_S = 12
BUILD_WAIT_S = 20
KEY_WAIT_S = 10

NEEDS_TEXT = "needs a text window"

# The seven keys that build from the window on top and leave Messages behind.
BUILD_KEYS = [
    pytest.param(F9, id="F9-make"),
    pytest.param(ALT_M, id="Alt-M-make"),
    pytest.param(ALT_C, id="Alt-C-compile"),
    pytest.param(ALT_F9, id="Alt-F9-compile"),
    pytest.param(ALT_L, id="Alt-L-install"),
    pytest.param(ALT_U, id="Alt-U-run"),
    pytest.param(CTRL_F9, id="Ctrl-F9-run"),
]


def screen_text(w):
    return "\n".join(w.display(drain=0))


# What each kind of window paints while it is on top.  A list window's frame
# carries no button row, so its own captions and buttons are the marker;
# these are the three sets the sweep of this class verified a window by.
PROJECT_MARKERS = (PROJECT_TITLE, "Add", "Delete")
FILE_MANAGER_MARKERS = ("File-Manager", "DirTree:")
WINDOW_LIST_MARKERS = (WINDOW_LIST_TITLE, "Show")


def markers_on_top(w, markers):
    """Every one of @markers is painted, so the window that owns them is up."""
    text = screen_text(w)
    return all(m in text for m in markers)


def file_manager_on_top(w):
    """The File Manager is a fixed-size dialog: its title and its directory
    tree's caption are on screen exactly while it is up."""
    return markers_on_top(w, FILE_MANAGER_MARKERS)


def project_window_on_top(w):
    """The Project window's title and the two buttons only it has."""
    return markers_on_top(w, PROJECT_MARKERS)


def window_list_on_top(w):
    """The window list's title and its Show button."""
    return markers_on_top(w, WINDOW_LIST_MARKERS)


def trace_text(path):
    try:
        with open(path) as fh:
            return fh.read()
    except OSError:
        return ""


def needs_text(action):
    """The one sentence a command that works on text says when the window on
    top holds none."""
    return "%s %s" % (action, NEEDS_TEXT)


def refusal_on_the_status_row(w, action):
    """The sentence is on the bottom row, where the editor says a refusal.

    A box would carry the same words inside a frame the user has to dismiss,
    drawn over the very window the command was refused for, so the row the
    sentence lands on is what tells the two apart."""
    return needs_text(action) in w.display(drain=0)[-1]


def project_folder(tmp_path):
    """A folder with main.c and a demo.prj that lists it."""
    (tmp_path / MAIN_C).write_text(SOURCE)
    (tmp_path / "demo.prj").write_text(PROJECT)
    return tmp_path


def session_in(workdir, filename=MAIN_C, seed=SOURCE, **kw):
    return WpeSession(str(workdir), seed, filename=filename, **kw)


def open_file_manager(w):
    """F3: the File Manager comes to the front."""
    w.key(F3, delay=0.6)
    assert wait_until(w, lambda: file_manager_on_top(w), tmo=OPEN_WAIT_S,
                      what="the File Manager on top"), screen_text(w)


def open_file_manager_modern(w):
    """Ctrl-O: the File Manager comes to the front under the Modern style.
    F3 is Search again there, so the Borland spelling opens no window at all
    and only leaves a "String NOT found" box behind."""
    w.key(CTRL_O, delay=0.6)
    assert wait_until(w, lambda: file_manager_on_top(w), tmo=OPEN_WAIT_S,
                      what="the File Manager on top"), screen_text(w)


def open_project_window(w):
    """Alt-P p, Enter (focus Files), Enter (open demo.prj): the Project
    window comes to the front."""
    w.key(ALT_P, "p", delay=0.8)
    w.key(ENTER, ENTER, delay=1.0)
    assert wait_until(w, lambda: project_window_on_top(w),
                      tmo=OPEN_WAIT_S, what="the project window on top"), \
        screen_text(w)


def open_window_list(w):
    """Alt-0: the window list comes to the front."""
    w.key(ALT_0, delay=0.8)
    assert wait_until(w, lambda: window_list_on_top(w), tmo=OPEN_WAIT_S,
                      what="the window list on top"), screen_text(w)


def build_from_project_window(w, key):
    """Press a build key in the Project window and wait for the build's end
    to put the Project window back on top, while the text loop is still
    waiting for the next key.  This is the stale state the rows below judge:
    the loop took the window's payload before the wait and the top window
    changed during it."""
    w.key(key, delay=0.6)
    assert wait_until(w, lambda: project_window_on_top(w),
                      tmo=BUILD_WAIT_S,
                      what="the project window back on top after the build"), \
        screen_text(w)


def cell_of(w, needle):
    """(row, column), 1-based as a terminal mouse report counts, of the first
    cell of @needle on screen; None when it is not there."""
    for y, row in enumerate(w.display(drain=0)):
        x = row.find(needle)
        if x >= 0:
            return y + 1, x + 1
    return None


def click(w, needle):
    """Press and release the left button on @needle (SGR 1006, 1-based)."""
    at = cell_of(w, needle)
    assert at is not None, "nothing on screen to click at %r:\n%s" % (
        needle, screen_text(w))
    row, col = at
    w.key("\033[<0;%d;%dM" % (col, row), delay=0.2)
    w.key("\033[<0;%d;%dm" % (col, row), delay=0.5)


def crash_files(state):
    """The crash reports the editor's own handler wrote in this session."""
    folder = os.path.join(state, "xwpe")
    if not os.path.isdir(folder):
        return []
    return sorted(n for n in os.listdir(folder)
                  if re.match(r"crash-\d+\.txt$", n))


def state_folder(tmp_path):
    return str(tmp_path / "state")


def project_files_unchanged(workdir, why):
    """Neither the project file nor its source gained a key's letter."""
    assert (workdir / "demo.prj").read_text() == PROJECT, \
        "the .prj changed: %s" % why
    assert (workdir / MAIN_C).read_text() == SOURCE, \
        "main.c changed: %s" % why


# --- K1 to K4: a language-server command with a window that holds no text ---

# K1
def test_highlight_uses_under_the_file_manager_says_it_needs_a_text_window(tmp_path):
    """Alt-Q U with the File Manager on top: the sentence, the File Manager
    still on top, the editor alive and the file behind it untouched.  On the
    base this reads a list's corner numbers as a row pointer and the editor
    dies of SIGSEGV."""
    with session_in(tmp_path) as w:
        open_file_manager(w)
        w.key(ALT_Q, "u", delay=0.6)
        assert wait_screen(w, needs_text("Highlight uses"), tmo=KEY_WAIT_S), \
            screen_text(w)
        assert refusal_on_the_status_row(w, "Highlight uses"), screen_text(w)
        assert file_manager_on_top(w), screen_text(w)
        assert w.alive(), "the editor died on Alt-Q U under the File Manager"
    assert (tmp_path / MAIN_C).read_text() == SOURCE


# K2
def test_rename_under_the_file_manager_says_it_needs_a_text_window(tmp_path):
    """Alt-Q N with the File Manager on top: the sentence, nothing renamed."""
    with session_in(tmp_path) as w:
        open_file_manager(w)
        w.key(ALT_Q, "n", delay=0.6)
        assert wait_screen(w, needs_text("Rename"), tmo=KEY_WAIT_S), \
            screen_text(w)
        assert refusal_on_the_status_row(w, "Rename"), screen_text(w)
        assert file_manager_on_top(w), screen_text(w)
        assert w.alive(), "the editor died on Alt-Q N under the File Manager"
    assert (tmp_path / MAIN_C).read_text() == SOURCE


# K3
def test_highlight_uses_under_the_project_window_says_it_needs_a_text_window(tmp_path):
    """Alt-Q U with the Project window on top: the sentence, the project list
    intact."""
    project_folder(tmp_path)
    with session_in(tmp_path) as w:
        open_project_window(w)
        w.key(ALT_Q, "u", delay=0.6)
        assert wait_screen(w, needs_text("Highlight uses"), tmo=KEY_WAIT_S), \
            screen_text(w)
        assert refusal_on_the_status_row(w, "Highlight uses"), screen_text(w)
        assert project_window_on_top(w), screen_text(w)
        assert w.alive(), "the editor died on Alt-Q U under the Project window"
    project_files_unchanged(tmp_path, "Alt-Q U renamed or inserted")


# K4
def test_rename_under_the_project_window_says_it_needs_a_text_window(tmp_path):
    """Alt-Q N with the Project window on top: the sentence, nothing renamed."""
    project_folder(tmp_path)
    with session_in(tmp_path) as w:
        open_project_window(w)
        w.key(ALT_Q, "n", delay=0.6)
        assert wait_screen(w, needs_text("Rename"), tmo=KEY_WAIT_S), \
            screen_text(w)
        assert refusal_on_the_status_row(w, "Rename"), screen_text(w)
        assert project_window_on_top(w), screen_text(w)
        assert w.alive(), "the editor died on Alt-Q N under the Project window"
    project_files_unchanged(tmp_path, "Alt-Q N renamed or inserted")


# --- K5 to K11: a key read after a build put another window on top --------

@pytest.mark.parametrize("key", BUILD_KEYS)
def test_a_letter_after_a_build_from_the_project_window_stays_out_of_the_text(
        tmp_path, key):
    """A build started in the Project window ends and puts that window back
    on top while the text loop waits for a key.  The letter that follows
    belongs to the window the user now sees: the project list is intact, the
    .prj and the source are unchanged, and the editor lives.  On the base the
    loop still holds the payload it took before the wait and writes the
    letter through a list's corner numbers -- a report under the sanitizer,
    a silent write without one."""
    require("gcc")
    project_folder(tmp_path)
    with session_in(tmp_path) as w:
        open_project_window(w)
        build_from_project_window(w, key)
        w.key("x", delay=0.6)
        assert wait_until(w, lambda: project_window_on_top(w), tmo=KEY_WAIT_S,
                          what="the project window still on top after the key"), \
            screen_text(w)
        assert w.alive(), "the editor died on the key after the build"
    project_files_unchanged(tmp_path, "the letter landed in a file")


# K12
def test_esc_after_a_build_from_the_project_window_closes_it_as_a_list(tmp_path):
    """Esc after the build is the Project window's own Esc: the window
    closes and the editor lives.  On the base the same stale payload makes
    Esc a change to the text, a report under the sanitizer."""
    require("gcc")
    project_folder(tmp_path)
    with session_in(tmp_path) as w:
        open_project_window(w)
        build_from_project_window(w, F9)
        w.key(ESC, delay=0.8)
        assert w.alive(), "the editor died on Esc after the build"
        assert wait_until(w, lambda: not project_window_on_top(w),
                          tmo=KEY_WAIT_S,
                          what="the project window closed by its own Esc"), \
            screen_text(w)
    assert (tmp_path / MAIN_C).read_text() == SOURCE


# K13
def test_a_click_in_the_project_list_after_a_build_leaves_the_editor_alive(tmp_path):
    """A click in the project list after the build reaches the mouse handler
    through the same stale payload.  A click is where the two displays
    differ, so this row has a twin on each.  The editor's own handler runs
    before the sanitizer can say anything, so the row reads the crash report
    the editor writes instead."""
    require("gcc")
    project_folder(tmp_path)
    state = state_folder(tmp_path)
    with session_in(tmp_path, env_extra={"XDG_STATE_HOME": state}) as w:
        open_project_window(w)
        build_from_project_window(w, F9)
        click(w, "Add")
        assert w.alive(), "the editor died on the click after the build"
        assert crash_files(state) == [], \
            "the editor wrote a crash report: %s" % crash_files(state)
    project_files_unchanged(tmp_path, "the click changed a file")


# --- K14, K16, K17: guards, green on the base ----------------------------

# K14
def test_the_window_list_keeps_its_own_keys_and_never_reaches_a_text_command(tmp_path):
    """The window list reads Alt-Q as one of its own box keys, so no
    language-server command runs and the editor lives.  A guard: the row says
    the same before and after the owner lands."""
    with session_in(tmp_path) as w:
        open_window_list(w)
        w.key(ALT_Q, "u", delay=0.6)
        w.key(ALT_Q, "n", delay=0.6)
        assert w.alive(), "the editor died on Alt-Q under the window list"
        assert window_list_on_top(w), screen_text(w)
    assert (tmp_path / MAIN_C).read_text() == SOURCE


F4 = "\033OS"                # Search: Find, a dialog
FIND_TITLE = " Find q"       # the dialog's title in its frame, as the pty paints it
HELP_KEY = "\033OP"          # F1


def find_dialog_top(w):
    """Open the Find dialog over the window on top, read the row its frame's
    title is on, and close it."""
    w.key(F4, delay=0.8)
    rows = w.display()
    top = next((y for y, row in enumerate(rows) if FIND_TITLE in row), None)
    assert top is not None, "no Find dialog:\n%s" % "\n".join(rows)
    w.key(ESC, delay=0.6)
    return top


def test_a_dialog_over_a_locked_file_and_over_help_sits_on_that_window(tmp_path):
    """A dialog is centred on the window under it, except over a docked tool
    strip (Messages, Watches, Stack, the assistant), where it is centred on
    the screen.  A file locked against writing and the Help viewer are
    read-only like those strips, and are ordinary windows: the dialog opens
    on them where it opens on a file the user can write.  Red when the strip
    is told from the window by the read-only flag, which both carry."""
    locked = tmp_path / "locked.c"
    locked.write_text("int x;\n")
    locked.chmod(0o444)
    with session_in(tmp_path, args=(str(locked),)) as w:
        # The writable file was opened last, so it is on top.
        over_a_writable_file = find_dialog_top(w)
        w.key(ALT_1, delay=0.6)
        over_a_locked_file = find_dialog_top(w)
        w.key(HELP_KEY, delay=1.0)
        over_the_help_viewer = find_dialog_top(w)
        assert w.alive(), "the editor died opening a dialog"
    assert over_a_locked_file == over_a_writable_file, \
        "a dialog over a locked file sits on row %d, over a writable one on %d" % (
            over_a_locked_file, over_a_writable_file)
    assert over_the_help_viewer == over_a_writable_file, \
        "a dialog over the Help viewer sits on row %d, over a writable file on %d" % (
            over_the_help_viewer, over_a_writable_file)


def click_at(w, row, col):
    """Press and release the left button at a 1-based cell (SGR 1006)."""
    w.key("\033[<0;%d;%dM" % (col, row), delay=0.2)
    w.key("\033[<0;%d;%dm" % (col, row), delay=0.5)


# The Windows list answers a click on the two rows above its Show button as
# the project lists' Add and Edit, though it paints neither: the mouse reads a
# row as a button by its place.  These rows say what such a click does.
HIDDEN_ROWS_ABOVE_SHOW = (4, 2)


def test_the_hidden_add_and_edit_rows_of_the_window_list_open_no_window(tmp_path):
    """A click on the cell where the project lists keep Add or Edit does not
    open the window under the cursor: the Windows list only shows results,
    so only its Show button opens one.  Red when the Windows list is taken
    for a list that keeps a blank row to add at (the sealed project lists),
    because then those two cells act as Add and Edit."""
    with session_in(tmp_path) as w:
        open_window_list(w)
        show = cell_of(w, "Show")
        assert show is not None, screen_text(w)
        show_row, show_col = show
        w.key("\033[B", delay=0.4)       # Down: the row of main.c, not the list itself
        for above in HIDDEN_ROWS_ABOVE_SHOW:
            click_at(w, show_row - above, show_col)
            assert w.alive(), "the editor died on a click above Show"
            assert window_list_on_top(w), \
                "a click %d rows above Show left the window list:\n%s" % (
                    above, screen_text(w))
        click(w, "Show")
        assert wait_until(w, lambda: not window_list_on_top(w),
                          tmo=OPEN_WAIT_S,
                          what="the window under the list in front"), \
            screen_text(w)
    assert (tmp_path / MAIN_C).read_text() == SOURCE


def slow_build_folder(tmp_path):
    """A folder whose make names an error after a pause, with a source to
    build from."""
    (tmp_path / "Makefile").write_text(SLOW_ERROR_MAKEFILE)
    return tmp_path


def build_under_a_box(w, trace, window, opener):
    """Start the slow make, put @window on top before it ends, and wait for
    the trace to say the build's end is waiting for that box."""
    w.key(ALT_A, delay=0.8)
    assert wait_screen(w, BUILD_START, tmo=BUILD_WAIT_S), screen_text(w)
    opener(w)
    assert wait_until(w, lambda: DEFERRED_BUILD in trace_text(trace),
                      tmo=BUILD_WAIT_S,
                      what="the build's end kept behind the %s" % window), \
        "%s\ntrace:\n%s" % (screen_text(w), trace_text(trace))


# K16
def test_a_build_that_ends_under_the_file_manager_waits_and_replays_in_the_text_loop(
        tmp_path):
    """The File Manager loop never returns to the dispatcher between keys, so
    it cannot notice a build: the build's end waits for the box, and the
    replay that shows it runs in the text loop after Esc, where the entry
    check can see what is on top.  A guard, green on the base."""
    require("make")
    trace = tmp_path / TRACE
    slow_build_folder(tmp_path)
    with session_in(tmp_path, filename="b.c", seed=BUILD_SRC,
                    env_extra={"XWPE_UI_TRACE": str(trace)}) as w:
        build_under_a_box(w, str(trace), "File Manager", open_file_manager)
        w.key("x", delay=0.6)
        assert file_manager_on_top(w), \
            "a key moved the File Manager:\n%s" % screen_text(w)
        assert w.alive(), "the editor died on a key under the File Manager"
        w.key(ESC, delay=0.8)
        assert wait_until(w, lambda: REPLAYED_BUILD in trace_text(trace),
                          tmo=BUILD_WAIT_S,
                          what="the build's end shown once the File Manager closed"), \
            "%s\ntrace:\n%s" % (screen_text(w), trace_text(trace))
        assert w.alive(), "the editor died closing the File Manager"


# K17
def test_a_build_that_ends_under_the_window_list_waits_and_replays_in_the_text_loop(
        tmp_path):
    """The window list's twin of the row above."""
    require("make")
    trace = tmp_path / TRACE
    slow_build_folder(tmp_path)
    with session_in(tmp_path, filename="b.c", seed=BUILD_SRC,
                    env_extra={"XWPE_UI_TRACE": str(trace)}) as w:
        build_under_a_box(w, str(trace), "window list", open_window_list)
        w.key("x", delay=0.6)
        assert window_list_on_top(w), screen_text(w)
        assert w.alive(), "the editor died on a key under the window list"
        w.key(ESC, delay=0.8)
        assert wait_until(w, lambda: REPLAYED_BUILD in trace_text(trace),
                          tmo=BUILD_WAIT_S,
                          what="the build's end shown once the window list closed"), \
            "%s\ntrace:\n%s" % (screen_text(w), trace_text(trace))
        assert w.alive(), "the editor died closing the window list"


# --- K15: the language-server menu with a window that holds no text -------

# K15
def test_the_language_server_menu_under_the_file_manager_says_it_needs_a_text_window(
        tmp_path):
    """Alt-Q ? with the File Manager on top opens no menu: the sentence names
    the menu and the File Manager stays on top.  On the base a box says
    "unsupported file type", which is a guess from the window's title and not
    an answer about the window."""
    with session_in(tmp_path) as w:
        open_file_manager(w)
        w.key(ALT_Q, "?", delay=0.6)
        assert wait_screen(w, needs_text("Language server menu"), tmo=KEY_WAIT_S), \
            screen_text(w)
        assert refusal_on_the_status_row(w, "Language server menu"), \
            screen_text(w)
        assert file_manager_on_top(w), screen_text(w)
        assert w.alive(), "the editor died on Alt-Q ? under the File Manager"


# --- K18B: a file the user named like a pane ----------------------------

# K18B
def test_a_file_named_messages_is_the_users_file_and_not_the_pane(tmp_path):
    """`wpe ./Messages main.c` opens the file the user named as a file of his
    own: writable, not the build's list of errors, and left alone by a build.
    On the base the window is recognised by its title, so it opens locked
    read-only, a build parses the compiler's lines into the user's own text,
    and the build after that closes his window to put the pane in its place."""
    require("gcc")
    (tmp_path / "Messages").write_text(USER_NOTES)
    with session_in(tmp_path, filename=MAIN_C, seed=SOURCE,
                    args=("./Messages",)) as w:
        w.key(ALT_1, delay=0.8)          # the user's own window to the front
        assert wait_screen(w, "Messages", tmo=OPEN_WAIT_S), screen_text(w)
        w.key("X", delay=0.5)            # a window the user may change takes it
        w.save()
        assert w.alive(), "the editor died with a file named Messages open"
        # A build of the source, not of the notes: the pane the editor opens
        # for its output must not take the user's window for its own by the
        # title it happens to carry, and must not parse into it.
        w.key(ALT_2, delay=0.8)          # the source file back to the front
        folder_before = set(os.listdir(str(tmp_path)))
        w.key(F9, delay=0.6)
        assert wait_until(w,
                          lambda: set(os.listdir(str(tmp_path))) != folder_before,
                          tmo=BUILD_WAIT_S,
                          what="the build of main.c to leave its output"), \
            screen_text(w)
        w.key(ALT_1, delay=0.8)          # the user's own window, if it survived
        assert wait_screen(w, USER_NOTES.strip(), tmo=KEY_WAIT_S), \
            "the build closed the user's file named Messages, or parsed its " \
            "output into it:\n%s" % screen_text(w)
        assert w.alive(), "the editor died building with a file named Messages"
    assert (tmp_path / "Messages").read_text() == "X" + USER_NOTES, \
        "the user's file named Messages was opened read-only, or taken for " \
        "the build's own pane"


# The names the editor gives its own windows.  A file the user opens under
# one of them is the user's file.
PANE_NAMES = [
    pytest.param("Watches", id="Watches"),
    pytest.param("Stack", id="Stack"),
    pytest.param("Help", id="Help"),
    pytest.param("Buffer", id="Buffer"),
    pytest.param("AI", id="AI"),
    pytest.param("Messages", id="Messages"),
]


# K18C
@pytest.mark.parametrize("name", PANE_NAMES)
def test_a_file_named_like_a_pane_is_edited_and_saved_as_a_file(tmp_path, name):
    """`wpe ./<name> main.c`: the file the user named after one of the
    editor's own windows takes a letter and saves it, whatever the window
    would have been.  A guard on every comparison that used to ask the title."""
    (tmp_path / name).write_text(USER_NOTES)
    with session_in(tmp_path, filename=MAIN_C, seed=SOURCE,
                    args=("./" + name,)) as w:
        w.key(ALT_1, delay=0.8)
        assert wait_screen(w, name, tmo=OPEN_WAIT_S), screen_text(w)
        w.key("X", delay=0.5)
        w.save()
        assert w.alive(), "the editor died with a file named %s open" % name
    assert (tmp_path / name).read_text() == "X" + USER_NOTES, \
        "the user's file named %s was taken for the editor's own window" % name


# K18D
@pytest.mark.parametrize("name", [
    pytest.param("Watches", id="Watches"),
    pytest.param("Stack", id="Stack"),
    pytest.param("Messages", id="Messages"),
])
def test_enter_in_a_read_only_file_named_like_a_pane_does_not_act_as_the_pane(
        tmp_path, name):
    """Enter in a locked window of the debugger or of the build acts on the
    row under the cursor: the Watches pane asks for the expression, Messages
    jumps to the error.  A read-only file of the user's that carries the same
    name is locked too, and Enter there must do nothing at all: no box, no
    new window, the file as it was.  Red while the title decides."""
    path = tmp_path / name
    path.write_text(USER_NOTES)
    path.chmod(0o444)
    with session_in(tmp_path, filename=MAIN_C, seed=SOURCE,
                    args=("./" + name,)) as w:
        w.key(ALT_1, delay=0.8)
        assert wait_screen(w, USER_NOTES.strip(), tmo=OPEN_WAIT_S), \
            screen_text(w)
        before = screen_text(w)
        w.key(ENTER, delay=0.8)
        assert w.alive(), "the editor died on Enter in a file named %s" % name
        after = screen_text(w)
        assert after == before, \
            "Enter in the user's read-only file named %s acted as the pane:\n" \
            "%s\n--- before:\n%s" % (name, after, before)
    assert path.read_text() == USER_NOTES


# --- K20: the replay is a second path to the same stale read ------------

# K20
def test_a_build_that_ends_behind_a_menu_is_not_read_as_the_project_windows_text(
        tmp_path):
    """The Project window is on top, a slow build runs, and the File menu is
    open when it ends: the screen's owner keeps the build's end behind the
    menu, and the text loop replays it after Esc.  The replay puts the
    Project window on top, and the entry check has already run for this key,
    so the letter that follows is read through the window the replay left --
    the second path to the same stale read.

    The row waits for the two trace lines that say the build's end was kept
    and then shown, so what it judges is the replay and not a build that
    happened to end before the menu opened."""
    require("gcc")
    trace = tmp_path / TRACE
    (tmp_path / "demo.prj").write_text(PROJECT)
    with session_in(tmp_path, filename=MAIN_C, seed=SLOW_SOURCE,
                    env_extra={"XWPE_UI_TRACE": str(trace)}) as w:
        open_project_window(w)
        w.key(F9, delay=0.6)             # a compile long enough to end behind a box
        w.key(ALT_F, delay=0.4)          # the File menu takes the screen
        assert wait_until(w, lambda: DEFERRED_BUILD in trace_text(trace),
                          tmo=BUILD_WAIT_S,
                          what="the build's end kept behind the File menu"), \
            "%s\ntrace:\n%s" % (screen_text(w), trace_text(trace))
        w.key(ESC, delay=0.6)            # the menu gives the screen back
        assert wait_until(w, lambda: REPLAYED_BUILD in trace_text(trace),
                          tmo=BUILD_WAIT_S,
                          what="the build's end shown once the menu closed"), \
            "%s\ntrace:\n%s" % (screen_text(w), trace_text(trace))
        w.key("x", delay=0.8)
        assert w.alive(), "the editor died on the key after the replay"
        assert project_window_on_top(w), screen_text(w)
    assert (tmp_path / MAIN_C).read_text() == SLOW_SOURCE, \
        "the replayed build's end wrote a letter into the project list"


# --- K21: a guard on the debugger's own raise --------------------------

# K21
def test_run_from_the_project_window_raises_its_source_before_it_builds(tmp_path):
    """Ctrl-G R in the Project window: the debugger raises the source file
    first, so the build it starts has a text window on top.  A guard, green
    on the base, kept because the window on top after a successful build may
    be the Project window."""
    require("gcc", "gdb")
    project_folder(tmp_path)
    with session_in(tmp_path) as w:
        open_project_window(w)
        w.key(CTRL_G, "r", delay=1.5)
        assert w.alive(), "the editor died on Ctrl-G R in the Project window"
        assert wait_until(w, lambda: not project_window_on_top(w),
                          tmo=BUILD_WAIT_S,
                          what="the debugger's source window raised over the "
                               "project list"), screen_text(w)


# --- K22: the Modern style's own keys for the same commands ------------

MODERN = {"XWPE_KEYS": "modern"}
MODERN_KEYS = [
    pytest.param(F2, "Rename", id="F2-rename"),
    pytest.param(F12, "Go to definition", id="F12-definition"),
    pytest.param(CTRL_SPACE, "Complete", id="Ctrl-Space-complete"),
]
MODERN_WINDOWS = [
    # how to put the window on top, how to see it is still there, what to call
    # it, whether it needs a project folder, and whether the key reaches the
    # language server at all from there
    pytest.param(open_file_manager_modern, file_manager_on_top, "File Manager",
                 False, False, id="file-manager"),
    pytest.param(open_project_window, project_window_on_top, "Project window",
                 True, True, id="project"),
]


@pytest.mark.parametrize("opener,on_top,name,wants_project,reaches_lsp",
                         MODERN_WINDOWS)
@pytest.mark.parametrize("key,action", MODERN_KEYS)
def test_a_modern_single_key_under_a_window_without_text_says_why(
        tmp_path, key, action, opener, on_top, name, wants_project, reaches_lsp):
    """The Modern style binds Rename, Go to definition and Complete to keys of
    their own beside the Alt-Q letters, and both ways in end in the same
    runner.  With the Project window on top each says its own sentence and
    changes nothing.  The File Manager reads these three as keys of its own
    dialog and never reaches the runner, so there the guard is what matters:
    the dialog stays up, no file behind it changes, and the editor lives."""
    if wants_project:
        project_folder(tmp_path)
    with session_in(tmp_path, env_extra=dict(MODERN)) as w:
        opener(w)
        w.key(key, delay=0.8)
        if reaches_lsp:
            assert wait_screen(w, needs_text(action), tmo=KEY_WAIT_S), \
                screen_text(w)
            assert refusal_on_the_status_row(w, action), screen_text(w)
        assert on_top(w), screen_text(w)
        assert w.alive(), "the editor died on %s under the %s" % (key, name)
    if wants_project:
        project_files_unchanged(tmp_path, "%s changed a file" % action)
    else:
        assert (tmp_path / MAIN_C).read_text() == SOURCE


# --- a typed character that shares a notice's number --------------------

TYPED_TOP_CHANGED = "\u07d8"     # U+07D8: the number of the top-window notice
TYPED_RESIZE = "\u07d2"          # U+07D2: the number of the new-size notice


def test_a_typed_character_that_shares_a_notices_number_is_the_character(
        tmp_path):
    """The editor's own codes live in the same numeric range as Unicode, so
    U+07D2 is the number a display reports a new size with and U+07D8 the
    number it reports a window that changed under a wait with.  A character
    the user typed is that character: it goes into his text, and the notice
    that shares its number is not read out of it.

    On the base U+07D2 was taken for a new size and thrown away, so the
    letter never arrived.  U+07D8 arrived then, and is swallowed by a wait
    that tells the two apart by the number alone, which is why the reader
    also says whether what it handed over was a typed character."""
    with session_in(tmp_path) as w:
        w.key(TYPED_TOP_CHANGED, delay=0.6)
        w.key(TYPED_RESIZE, delay=0.6)
        assert w.alive(), "the editor died on the two characters"
        w.save()
    raw = (tmp_path / MAIN_C).read_bytes()
    assert TYPED_TOP_CHANGED.encode() in raw, \
        "a typed U+07D8 was read as the top-window notice: %r" % raw
    assert TYPED_RESIZE.encode() in raw, \
        "a typed U+07D2 was read as a new size: %r" % raw


# --- the screen's owner and a box's own key read ------------------------

def test_a_box_reading_its_keys_never_sees_the_window_on_top_change(tmp_path):
    """A box reads its keys with the screen's owner holding a build's end
    back, so no display tells a box that the window on top changed: across
    the box's own key read the count stands still, and a code that reached a
    box anyway would be thrown away rather than answered by a window the user
    is not looking at.

    The File Manager is up and waiting for its next key when the slow build
    ends, which is the moment the invariant covers.  The owner writes the
    line this row looks for when the count does move, and its own unit test
    holds that it writes it and throws the code away; here the row is the
    guard on the running editor, green on the base, which had no owner to
    ask and kept the build's end behind the box by the same depth rule."""
    require("make")
    trace = tmp_path / TRACE
    slow_build_folder(tmp_path)
    with session_in(tmp_path, filename="b.c", seed=BUILD_SRC,
                    env_extra={"XWPE_UI_TRACE": str(trace)}) as w:
        w.key(ALT_A, delay=0.8)
        assert wait_screen(w, BUILD_START, tmo=BUILD_WAIT_S), screen_text(w)
        open_file_manager(w)
        # The box reads on: its next read is the one the build's end lands in.
        w.key("x", delay=0.2)
        assert wait_until(w, lambda: DEFERRED_BUILD in trace_text(trace),
                          tmo=BUILD_WAIT_S,
                          what="the build's end kept behind the File Manager"), \
            "%s\ntrace:\n%s" % (screen_text(w), trace_text(trace))
        assert BOX_SAW_TOP_CHANGE not in trace_text(trace), \
            "a box's key read saw the window on top change:\n%s" % \
            trace_text(trace)
        assert file_manager_on_top(w), screen_text(w)
        w.key(ESC, delay=0.8)
        assert wait_until(w, lambda: REPLAYED_BUILD in trace_text(trace),
                          tmo=BUILD_WAIT_S,
                          what="the build's end shown once the File Manager "
                               "closed"), \
            "%s\ntrace:\n%s" % (screen_text(w), trace_text(trace))
        assert BOX_SAW_TOP_CHANGE not in trace_text(trace), \
            "a box's key read saw the window on top change:\n%s" % \
            trace_text(trace)
        assert w.alive(), "the editor died on a key under the File Manager"


# --- the composer's nested text loop ------------------------------------

COMPOSER_TITLE_MARK = "Esc = done"
COMPOSER_TEXT = "hello"


def assistant_in_the_build():
    """The editor under test was built with the assistant in it."""
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


@pytest.mark.skipif(not assistant_in_the_build(),
                    reason="wpe built without the assistant")
def test_a_build_that_ends_while_the_composer_is_open_returns_to_the_composer(
        tmp_path):
    """The assistant's multi-line composer is a window of its own with the
    editor's own key loop running inside it, so it is the one caller of the
    text loop that is not the dispatcher's.  A build that ends while the user
    writes gives the keys back to the window he was in, and that window is
    the composer: it stays open, holding what he typed, and a key he presses
    afterwards is the composer's.

    A guard, green on the base: the composer is a text window, and the build
    finds the window to return to by what it is."""
    require("make")
    (tmp_path / "Makefile").write_text(SLOW_OK_MAKEFILE)
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": "ok"}
    with session_in(tmp_path, filename="b.c", seed=BUILD_SRC,
                    env_extra=env) as w:
        w.key(ALT_A, delay=0.8)
        assert wait_screen(w, BUILD_START, tmo=BUILD_WAIT_S), screen_text(w)
        w.key(ALT.AI, delay=0.6)         # the assistant's prefix
        w.key("g", delay=1.0)            # Ask: the prompt box
        w.key(COMPOSER_TEXT, delay=0.5)
        w.key("\033m", delay=1.0)        # Multi-line: the composer window
        assert wait_screen(w, COMPOSER_TITLE_MARK, tmo=OPEN_WAIT_S), \
            screen_text(w)
        assert wait_until(w,
                          lambda: (tmp_path / BUILD_DONE_FILE).exists(),
                          tmo=BUILD_WAIT_S,
                          what="the slow make to finish under the composer"), \
            screen_text(w)
        w.key("z", delay=0.8)            # a key the composer's own loop reads
        assert w.alive(), "the editor died when a build ended under the composer"
        assert wait_screen(w, COMPOSER_TITLE_MARK, tmo=KEY_WAIT_S), \
            "the build's end took the keys away from the composer:\n%s" % \
            screen_text(w)
        assert COMPOSER_TEXT + "z" in screen_text(w), \
            "the key after the build did not reach the composer:\n%s" % \
            screen_text(w)


# --- K23 to K34: the window the editor opens for itself, and a user's file ---
#
# One request names its kind and one function finds or makes the window.  A
# file the user names like one of the editor's own windows is the user's file
# at every entry (the command line, File > Open, File > New and Save As), and
# a pane the editor opens never takes a user's file for itself.  Every row
# below drives the pane through its own command and reads the user's file
# back from the disk, because what is lost when the two are mixed is the
# file's text.

ALT_H = "\033h"                  # the Help menu
CTRL_F1 = "\033[11;5~"           # Help > Topic Search
CTRL_INS = "\033[2;5~"           # copy the marked block
SHIFT_INS = "\033[2;2~"          # paste
SHIFT_RIGHT = "\033[1;2C"        # mark a character
TOPIC_SEARCH = "t"               # Help > Topic Search
BUILD_ERROR_WORD = "1 error"     # the count Messages ends a failed build of BUILD_SRC with
BUILD_FAILED_WORD = "build failed"  # the line the base appends to a window it took for Messages
HELP_PAGE_MARK = "Programming Environment"
MAN_PAGE_MARK = "PRINTF"
REPLY_MARK = "PANEREPLY"
DEBUG_SOURCE = (
    "#include <stdio.h>\n"
    "int main(void)\n"
    "{\n"
    "  int x = 3;\n"
    "  int y = x * x;\n"
    "  printf(\"%d\\n\", y);\n"
    "  return 0;\n"
    "}\n"
)
DEBUG_BREAK_LINE = 4             # int x = 3;
DEBUG_START_WAIT_S = 30
# The user's file is open under p.c, so p.c opens one row down and right of
# it, and the driver's mark_line (which counts rows from the top border of a
# single window) reports the stopped line one row further down.
CASCADE_ROWS_BELOW = 1
DEBUG_STEP_WAIT_S = 10
UNDEFINED_WATCH = "xyz"
STACK_FRAME_MARK = "#0"          # the innermost frame, as gdb prints it
GDB_INTERFACE_ENV = "XWPE_GDB_INTERFACE"   # which of gdb's two interfaces the debugger speaks
GDB_INTERFACE = "cli"
DOWN_KEY = "\033OB"
CTRL_G_PREFIX = "\x07"


def users_file(tmp_path, name, text=USER_NOTES):
    """A file of the user's, named like one of the editor's own windows."""
    (tmp_path / name).write_text(text)
    return tmp_path / name


def users_window_takes_a_letter(w):
    """The user's own file is window 1: it comes to the front, takes a letter
    and File > Save writes it.  The disk is what the rows read after."""
    w.key(ALT_1, delay=0.8)
    w.key("X", delay=0.5)
    w.save()
    w.key(ALT.FILE, "l", delay=0.8)      # File > Save aLl: any other window changed too


def other_file_is_unchanged(tmp_path, name, text):
    """The file the pane's own command ran from keeps its text on the disk
    after Save All: a pane that wrote into it, in place of its own window,
    would have changed it."""
    assert (tmp_path / name).read_text() == text, \
        "the pane wrote into %s instead of its own window" % name


def wait_for_a_failed_build(w):
    """Wait until the build of BUILD_SRC has said why it failed."""
    assert wait_until(
        w, lambda: BUILD_ERROR_WORD in screen_text(w) or
        BUILD_FAILED_WORD in screen_text(w),
        tmo=BUILD_WAIT_S, what="the build to say it failed"), screen_text(w)


# K23
def test_a_build_leaves_a_file_named_messages_to_its_user(tmp_path):
    """`wpe ./Messages main.c` with a main.c that does not build: F9 puts the
    compiler's lines in the pane the editor opens, and the user's own file
    named Messages takes a letter and saves it without them.  On the base the
    pane request takes the user's window for the pane, the build line is
    appended to it and Save writes it."""
    require("gcc")
    notes = users_file(tmp_path, "Messages")
    with session_in(tmp_path, filename=MAIN_C, seed=BUILD_SRC,
                    args=("./Messages",)) as w:
        w.key(F9, delay=0.6)
        wait_for_a_failed_build(w)
        users_window_takes_a_letter(w)
        assert w.alive(), "the editor died building with a file named Messages"
    assert notes.read_text() == "X" + USER_NOTES, \
        "the build wrote into the user's file named Messages"
    other_file_is_unchanged(tmp_path, MAIN_C, BUILD_SRC)


# K24
@pytest.mark.skipif(not assistant_in_the_build(),
                    reason="wpe built without the assistant")
def test_a_chat_turn_leaves_a_file_named_ai_to_its_user(tmp_path):
    """`wpe ./AI main.c`, one chat turn on the mock backend: the transcript
    goes to the assistant's own pane, and the user's file named AI stays
    writable and keeps its text.  On the base the pane request takes the
    user's window, locks it as the pane and writes the transcript into it."""
    notes = users_file(tmp_path, "AI")
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": REPLY_MARK}
    with session_in(tmp_path, args=("./AI",), env_extra=env) as w:
        w.key(ALT.AI, "a", delay=0.8)
        w.key("hi", delay=0.4)
        w.key(ENTER, delay=1.2)
        assert wait_screen(w, REPLY_MARK, tmo=OPEN_WAIT_S), screen_text(w)
        w.key(ESC, delay=0.6)
        users_window_takes_a_letter(w)
        assert w.alive(), "the editor died on a chat turn with a file named AI"
    assert notes.read_text() == "X" + USER_NOTES, \
        "the transcript was written into, or the save was refused for, the " \
        "user's file named AI"
    other_file_is_unchanged(tmp_path, MAIN_C, SOURCE)


# A language server that, once it has the file, pushes the Doctor report the
# way Metals does (metals/executeClientCommand), so the editor writes it into
# a window named "Metals Doctor".
FAKE_DOCTOR_SERVER = r'''#!%(python)s -I
import json, sys
inp, out = sys.stdin.buffer, sys.stdout.buffer

def send(obj):
    body = json.dumps(obj).encode()
    out.write(b"Content-Length: %%d\r\n\r\n" %% len(body) + body)
    out.flush()

def read():
    head = b""
    while b"\r\n\r\n" not in head:
        ch = inp.read(1)
        if not ch:
            return None
        head += ch
    for line in head.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            return json.loads(inp.read(int(line.split(b":")[1])))
    return None

while True:
    msg = read()
    if msg is None:
        break
    method = msg.get("method")
    if method == "exit":
        break
    if "id" in msg and method is not None:
        result = {"capabilities": {}} if method == "initialize" else None
        send({"jsonrpc": "2.0", "id": msg["id"], "result": result})
    elif method == "textDocument/didOpen":
        send({"jsonrpc": "2.0", "method": "metals/executeClientCommand",
              "params": {"command": "metals-doctor-run",
                         "arguments": ["<h1>%(report)s</h1><p>fine</p>"]}})
        with open(%(sent)r, "w") as fh:
            fh.write("sent")
'''
DOCTOR_SENT = "doctor.sent"       # written by the server once it pushed the report
DOCTOR_SETTLE_S = 2.0             # the editor paints what it was pushed
DOCTOR_TITLE = "Metals Doctor"
DOCTOR_REPORT = "DOCTORREPORT"
CLANGD_NAME = "clangd"


def install_doctor_server(tmp_path):
    """A clangd on the PATH of the editor that pushes the Doctor report."""
    bindir = tmp_path / "bin"
    bindir.mkdir()
    server = bindir / CLANGD_NAME
    server.write_text(FAKE_DOCTOR_SERVER % {
        "python": sys.executable, "report": DOCTOR_REPORT,
        "sent": str(tmp_path / DOCTOR_SENT)})
    server.chmod(0o755)
    return {"PATH": "%s:%s" % (bindir, os.environ.get("PATH", ""))}


# K25
def test_a_pushed_report_leaves_a_file_named_metals_doctor_to_its_user(tmp_path):
    """`wpe "./Metals Doctor" a.c`, the language server pushes its Doctor
    report: the report goes to a window of its own, and the user's file of
    that name keeps its text and takes a letter.  On the base the report is
    written into the user's window (locked as a view) and Save writes it."""
    notes = users_file(tmp_path, DOCTOR_TITLE)
    env = install_doctor_server(tmp_path)
    with session_in(tmp_path, filename="a.c", seed=SOURCE,
                    args=("./" + DOCTOR_TITLE,), env_extra=env) as w:
        w.key(ALT_Q, "e", delay=2.0)
        assert wait_until(w, lambda: (tmp_path / DOCTOR_SENT).exists(),
                          tmo=OPEN_WAIT_S, what="the server to push its report"), \
            screen_text(w)
        w.key(ESC, delay=DOCTOR_SETTLE_S)
        users_window_takes_a_letter(w)
        assert w.alive(), "the editor died on the pushed report"
        titled = windows_titled(window_list_rows(w), DOCTOR_TITLE)
        assert len(titled) == 2, \
            "the report is not a window of its own:\n%s" % screen_text(w)
    assert notes.read_text() == "X" + USER_NOTES, \
        "the report was written into the user's file named Metals Doctor"
    other_file_is_unchanged(tmp_path, "a.c", SOURCE)


def debug_session(tmp_path, name):
    """`wpe ./<name> p.c` with the debugger stopped at line DEBUG_BREAK_LINE
    of p.c; returns the session after the stop."""
    require("gcc", "gdb")
    users_file(tmp_path, name)
    return session_in(tmp_path, filename="p.c", seed=DEBUG_SOURCE,
                      args=("./" + name,), wait=1.5,
                      env_extra={GDB_INTERFACE_ENV: GDB_INTERFACE})


def stop_at_the_breakpoint(w):
    """A breakpoint on DEBUG_BREAK_LINE, Ctrl-G R, and the stop's mark."""
    for _ in range(DEBUG_BREAK_LINE - 1):
        w.key(DOWN_KEY, delay=0.1)
    w.key(CTRL_G_PREFIX, "b", delay=0.4)
    w.key(CTRL_G_PREFIX, "r", delay=1.0)
    stop_row = DEBUG_BREAK_LINE + CASCADE_ROWS_BELOW
    assert wait_until(w, lambda: w.mark_line() == stop_row,
                      tmo=DEBUG_START_WAIT_S,
                      what="the stop mark on line %d" % DEBUG_BREAK_LINE), \
        screen_text(w)


# K26C
def test_a_stop_paints_its_mark_with_a_file_of_another_name_open(tmp_path):
    """The control of K26 and K27: the same session with a file called notes
    open before p.c, stopped at the breakpoint.  The stop's mark is painted
    on the stopped line."""
    with debug_session(tmp_path, "notes") as w:
        stop_at_the_breakpoint(w)
        assert w.alive(), "the editor died at the stop"


# K26
def test_a_watch_leaves_a_file_named_watches_to_its_user(tmp_path):
    """`wpe ./Watches p.c`, debugged to a stop, a watch on a name the program
    has not: the Watches pane the editor opens shows it, and the user's file
    named Watches keeps its text and takes a letter.  On the base the watch
    is written into the user's window and Save writes it."""
    with debug_session(tmp_path, "Watches") as w:
        stop_at_the_breakpoint(w)
        w.key(CTRL_G_PREFIX, "w", delay=0.8)
        w.key(UNDEFINED_WATCH, delay=0.4)
        w.key(ENTER, delay=0.8)
        assert wait_until(w, lambda: UNDEFINED_WATCH + ": " in screen_text(w),
                          tmo=DEBUG_STEP_WAIT_S,
                          what="the watch in the Watches pane"), screen_text(w)
        users_window_takes_a_letter(w)
    assert (tmp_path / "Watches").read_text() == "X" + USER_NOTES, \
        "the watch was written into the user's file named Watches"
    other_file_is_unchanged(tmp_path, "p.c", DEBUG_SOURCE)


# K27
def test_the_stack_leaves_a_file_named_stack_to_its_user(tmp_path):
    """`wpe ./Stack p.c`, debugged to a stop (the mark is painted on the
    stopped line), Ctrl-G K: the call stack is shown in the pane the editor
    opens and the user's file named Stack keeps its text.  On the base the
    frames are written into the user's window."""
    with debug_session(tmp_path, "Stack") as w:
        stop_at_the_breakpoint(w)
        w.key(CTRL_G_PREFIX, "k", delay=1.0)
        assert wait_until(w, lambda: STACK_FRAME_MARK in screen_text(w),
                          tmo=DEBUG_STEP_WAIT_S,
                          what="the frames in the Stack pane"), screen_text(w)
        users_window_takes_a_letter(w)
    assert (tmp_path / "Stack").read_text() == "X" + USER_NOTES, \
        "the call stack was written into the user's file named Stack"
    other_file_is_unchanged(tmp_path, "p.c", DEBUG_SOURCE)


# K28
def test_help_leaves_a_file_named_help_to_its_user(tmp_path):
    """`wpe ./Help main.c`, F1: the viewer opens in a window of its own with
    the help text, and the user's file named Help keeps its text.  On the base
    the help text replaces the user's text in the user's window."""
    notes = users_file(tmp_path, "Help")
    with session_in(tmp_path, args=("./Help",)) as w:
        w.key(HELP_KEY, delay=1.0)
        assert wait_screen(w, HELP_PAGE_MARK, tmo=OPEN_WAIT_S), screen_text(w)
        w.key(ESC, delay=0.8)
        users_window_takes_a_letter(w)
        assert w.alive(), "the editor died on F1 with a file named Help open"
    assert notes.read_text() == "X" + USER_NOTES, \
        "the help text replaced the user's file named Help"
    other_file_is_unchanged(tmp_path, MAIN_C, SOURCE)


# K29
def test_a_file_named_buffer_is_edited_and_the_clipboard_is_separate(tmp_path):
    """`wpe ./Buffer ./Buffer main.c`: the user's file named Buffer opens as
    window 1 and, named twice, is not opened twice; a block copied in it is
    pasted in it, and Save writes it.  On the base the open finds the
    clipboard (slot 0, titled Buffer) and the user's file never opens, so
    window 1 is main.c."""
    notes = users_file(tmp_path, "Buffer")
    with session_in(tmp_path, args=("./Buffer", "./Buffer")) as w:
        w.key(ALT_1, delay=0.8)
        assert wait_screen(w, USER_NOTES.strip(), tmo=OPEN_WAIT_S), \
            screen_text(w)
        w.key(SHIFT_RIGHT, SHIFT_RIGHT, delay=0.3)
        w.key(CTRL_INS, delay=0.4)
        w.key(SHIFT_INS, delay=0.5)
        w.save()
        assert w.alive(), "the editor died with a file named Buffer open"
        rows = window_list_rows(w)
        assert len(windows_titled(rows, "Buffer")) == 1, "\n".join(rows)
    assert notes.read_text() == USER_NOTES[:2] + USER_NOTES, \
        "the copied block was not pasted into the user's file named Buffer"
    assert (tmp_path / MAIN_C).read_text() == SOURCE


# K30
def test_help_on_a_word_that_is_an_open_file_shows_the_manual_page(tmp_path):
    """A file named printf is open and Topic Search is asked for the word
    printf: the manual page opens in a view of its own and the file is not
    raised in its place.  On the base the page is found by its title and the
    user's file answers."""
    require("man")
    with session_in(tmp_path, filename="printf", seed="printf\n", wait=2.0) as w:
        w.key(ALT_H, TOPIC_SEARCH, delay=6.0)
        assert wait_until(w, lambda: MAN_PAGE_MARK in screen_text(w),
                          tmo=OPEN_WAIT_S, what="the manual page"), \
            screen_text(w)
        assert w.alive(), "the editor died on Topic Search"
        rows = window_list_rows(w)
        assert len(windows_titled(rows, "printf")) == 2, "\n".join(rows)
    assert (tmp_path / "printf").read_text() == "printf\n"


# The screen a CI runner showed for K30: two windows titled printf and the
# window list open over the manual page, whose own NAME row reads
# "printf  - Prix" two columns left of the list's frame.  That man-db puts two
# spaces after the name is what made a count over the whole screen read the
# page's row as an entry of the list.
CI_WINDOW_LIST_SCREEN = (
    "  #   File  Edit  Search  Block   Run   Debug  Project  Options  Window   Help  ",
    "lqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq printf qqqqqqqqqqqqqqqqqqqqqqqqqqqqq1qqqqqqk",
    "xlqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq printf qqqqqqqqqqqqqqqqqqqqqqqqqqqq2qqqqqqk",
    "xxPRINTF(1)           lqqqqqqqqqqqq Windows qqqqqq3qqqqqqk              PRINTF(x",
    "xx                    x                                  x                     x",
    "xxNAME                x                                  x                     x",
    "xx       printf  - Prix   Windows           ^            xring and proceeding ax",
    "xx       guments.     x   printf            \u25ae            x                     x",
    "xx                    x   printf            a            x                     x",
    "xxSYNOPSIS            x                     a            x                     x",
    "xx       printf [--helx                     a            x                     x",
    "xx                    x                     a            x                     x",
    "xxDESCRIPTION         x                     a            x                     x",
    "xx       Print output x                     a            x                     x",
    "xx                    x                     a            x                     x",
    "xxOPTIONS             x                     a            x                     x",
    "xx       --help Print x                     a            x                     x",
    "xx                    x                     a   Show     x                     x",
    "xx       --version    x                     a            x                     x",
    "xx              Print x                     v   Save     x                     x",
    "mmqqqqqqqqqqqqqqqqqqqqx  <\u25aeaaaaaaaaaaaaaaaa>             xqqqqqqqqqqqqqqqqqqqqqj",
    "                      mqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqj                      ",
    "                                                                                ",
    "                                                                                ",
    "                                                                                ",
    "                                                                                ",
    "                                                                                ",
    "                                                                                ",
    "                                                                                ",
    " F1 Help  Add  Delete  ^W Close W.  Alt-X Quit                                  ",
)


# K30 on the screen a CI runner showed
def test_the_window_list_counts_only_the_entries_inside_its_frame():
    """The manual page behind the window list names printf on its own NAME
    row, so a count over the whole screen reads three rows titled printf
    where the list holds two."""
    rows = list(CI_WINDOW_LIST_SCREEN)
    inside = window_list_box_rows(rows)
    assert len(windows_titled(inside, "printf")) == 2, "\n".join(inside)


def window_list_rows(w):
    """The rows inside the window list's frame while the list is open; the
    list is opened with Alt-0, read and closed again."""
    open_window_list(w)
    rows = window_list_box_rows(w.display(drain=0))
    w.key(ESC, delay=0.6)
    return rows


def window_list_box_rows(rows):
    """The rows of @rows that are inside the window list's frame, cut to the
    columns between its borders, from its title row down to the frame row
    that closes it.

    The list is a window over the others, so the whole screen also holds what
    they show: a manual page behind the list that names the same title as one
    of its entries would be counted as an entry of the list."""
    y, x0, x1 = pane_frame(rows, WINDOW_LIST_TITLE)
    inside = []
    for row in rows[y + 1:]:
        if row[x0:x0 + 1] == FRAME_BOTTOM_LEFT:
            break
        inside.append(row[x0 + 1:x1])
    return inside


def windows_titled(rows, title):
    """The rows of the window list that name @title: inside the frame a row
    of the list is the title alone, set off from the frame and the scroll bar
    by blanks."""
    return [r for r in rows if re.match(r" {2,}%s {2,}" % re.escape(title), r)]


# K33
def test_help_on_the_same_word_twice_gives_one_page_window(tmp_path):
    """Topic Search on printf, back to the source, Topic Search again: the
    second ask raises the page that is open, so the window list names it
    once.  A guard on the find the manual page asks at its entry."""
    require("man")
    with session_in(tmp_path, filename="p.c", seed="printf\n", wait=2.0) as w:
        w.key(ALT_H, TOPIC_SEARCH, delay=6.0)
        assert wait_until(w, lambda: MAN_PAGE_MARK in screen_text(w),
                          tmo=OPEN_WAIT_S, what="the manual page"), \
            screen_text(w)
        w.key(ALT_1, delay=0.8)
        w.key(ALT_H, TOPIC_SEARCH, delay=3.0)
        assert w.alive(), "the editor died on the second Topic Search"
        rows = window_list_rows(w)
        assert len(windows_titled(rows, "printf")) == 1, "\n".join(rows)


# K31
def test_a_file_given_by_a_bare_pane_name_is_the_users_file(tmp_path):
    """`wpe Messages main.c`, without the ./ of K18B: the file named Messages
    opens as a file, takes a letter and saves it, and a build that fails puts
    its lines in the pane, a second window.  On the base the bare name is the
    pane: it opens empty and locked and the file is never shown."""
    require("gcc")
    notes = users_file(tmp_path, "Messages")
    with session_in(tmp_path, filename=MAIN_C, seed=BUILD_SRC,
                    args=("Messages",)) as w:
        users_window_takes_a_letter(w)
        w.key(ALT_2, delay=0.8)
        w.key(F9, delay=0.6)
        wait_for_a_failed_build(w)
        w.key(ALT.FILE, "l", delay=0.8)
        assert w.alive(), "the editor died building with a file named Messages"
    assert notes.read_text() == "X" + USER_NOTES, \
        "the bare name Messages was taken for the pane"
    other_file_is_unchanged(tmp_path, MAIN_C, BUILD_SRC)


# K31, second way in
def test_a_file_opened_by_the_pane_s_name_after_a_build_is_the_users_file(tmp_path):
    """The build has failed, so the Messages pane is open; File > Open of the
    user's file named Messages then opens that file as a window of its own
    (it takes a letter and saves it) and does not raise the pane in its
    place.  On a find that lets the pane answer a file's request, the pane
    is raised and the file is never shown."""
    require("gcc")
    notes = users_file(tmp_path, "Messages")
    with session_in(tmp_path, filename=MAIN_C, seed=BUILD_SRC) as w:
        w.key(F9, delay=0.6)
        wait_for_a_failed_build(w)
        w.key(F3, delay=1.0)
        w.key("Messages", delay=0.4)
        w.key(ENTER, delay=1.2)
        assert wait_screen(w, USER_NOTES.strip(), tmo=OPEN_WAIT_S), screen_text(w)
        w.key("X", delay=0.5)
        w.save()
        w.key(ALT.FILE, "l", delay=0.8)
        assert w.alive(), "the editor died opening a file named Messages"
    assert notes.read_text() == "X" + USER_NOTES, \
        "File > Open of Messages raised the pane or wrote into it"
    other_file_is_unchanged(tmp_path, MAIN_C, BUILD_SRC)


# K32
def test_a_file_saved_as_messages_stays_the_users_file_through_a_build(tmp_path):
    """File > New, a line typed, Save As Messages, then a failed build of
    main.c and Save All: the file is the user's and the build wrote nothing
    into it.  On the base the build finds the renamed window by its title and
    appends its line to it."""
    require("gcc")
    with session_in(tmp_path, filename=MAIN_C, seed=BUILD_SRC) as w:
        w.key(ALT.FILE, "n", delay=1.0)
        w.key("mine", delay=0.4)
        w.key(ALT.FILE, "a", delay=1.0)
        w.key("Messages", delay=0.4)
        w.key("\033s", delay=1.0)
        written = (tmp_path / "Messages").read_text()
        assert written.startswith("mine"), written
        w.key(ALT_1, delay=0.8)
        w.key(F9, delay=0.6)
        wait_for_a_failed_build(w)
        w.key(ALT.FILE, "l", delay=1.0)
        assert w.alive(), "the editor died building with a file saved as Messages"
    assert (tmp_path / "Messages").read_text() == written, \
        "the build wrote into the file the user saved as Messages"


# K34
@pytest.mark.skipif(not assistant_in_the_build(),
                    reason="wpe built without the assistant")
def test_a_second_composer_opened_in_the_first_is_a_window_of_its_own(tmp_path):
    """The multi-line composer is open and Alt-G g asks for another: the
    second is its own window, Esc and N close it and the first is still open
    with its text.  On the base the request finds the open composer by its
    title, the second loop runs on the same window and closes it, and the
    first loop goes on with a window that is gone."""
    require("make")
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": "ok"}
    with session_in(tmp_path, filename="b.c", seed=BUILD_SRC,
                    env_extra=env) as w:
        w.key(ALT.AI, "g", delay=1.0)
        w.key(COMPOSER_TEXT, delay=0.5)
        w.key("\033m", delay=1.0)
        assert wait_screen(w, COMPOSER_TITLE_MARK, tmo=OPEN_WAIT_S), \
            screen_text(w)
        w.key(ALT.AI, "g", delay=1.0)
        w.key("second", delay=0.5)
        w.key("\033m", delay=1.0)
        w.key(ESC, delay=0.8)
        w.key("n", delay=0.8)
        assert w.alive(), "the editor died closing a second composer"
        assert wait_screen(w, COMPOSER_TITLE_MARK, tmo=KEY_WAIT_S), \
            "the first composer is gone:\n%s" % screen_text(w)
        assert COMPOSER_TEXT in screen_text(w), screen_text(w)
