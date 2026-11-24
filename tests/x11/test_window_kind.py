"""Which window a key belongs to, in the xwpe/X11 window.

Twin of the terminal rows of tests/test_window_kind.py, for the four of them
that reach the display's own key wait: a build ends while the editor waits
for a key and puts a window without text on top, and the next key belongs to
the window the user is looking at, not to the payload the text loop took
before it waited.  The X11 wait is a different piece of code from the
terminal's (we_xterm.c, not we_term.c), so the notice that says "what is on
top changed" has to be delivered here too.

The rows assert what a user can see from outside a graphical editor: the
editor is alive, it wrote no crash report, and neither the project file nor
the source gained a key's letter.  The screen is pixels here, so the window
that is on top is told by the change a key makes to it and by what the build
left on disk, not by reading a title.

Red on the base: with no notice from the display, the text loop keeps the
window it fetched before the wait and reads the project list's corner
numbers as a row pointer -- a wild write, and a crash the editor's own
handler reports.

Run: XWPE_BIN=$PWD/xwpe tests/.venv/bin/python -m pytest -q tests/x11/test_window_kind.py
"""
import os
import re
import time

import pytest

from conftest import changed_pixels
from host_tools import needs

# Every row builds the project with the host's compiler.
pytestmark = needs("gcc")

PROJECT_NAME = "demo.prj"
EXE_NAME = "demo"
# The project's own build: compile the file the fixture opened and link it.
PROJECT = "CMP=\tgcc\nEXENAME=\t%s\nFILES=\tt.c\n" % EXE_NAME
# A source big enough that its compile is still running when the File menu
# opens, so the build's end lands while that box owns the screen.  It carries
# a main() so the build succeeds: only a build that succeeded gives the keys
# back to the window the user was in.
SLOW_FUNCTIONS = 40000
SLOW_SOURCE = ("int main(void){return 0;}\n" +
               "".join("int f%d(void){return %d;}\n" % (i, i)
                       for i in range(SLOW_FUNCTIONS)))
# The menu the K20 row opens over the build, and the key that closes it.
SLOW_BUILD = {"XWPE_TEST_SOURCE": SLOW_SOURCE}

BUILD_WAIT_S = 60.0
SETTLE_S = 1.5
CRASH_NAME = re.compile(r"^(xwpe-)?crash-\d+\.txt$")
# The file the fixture opens, spelled out so a row can hold it unchanged.
DEFAULT_SOURCE = (
    "int main(){\n"
    "  int a = 1;\n"
    "  int b = 2;\n"
    "  int c = 3;\n"
    "  int d = 4;\n"
    "  int e = 5;\n"
    "  int f = 6;\n"
    "  return 0;\n"
    "}\n")


def workdir(xwpe):
    """The folder the editor runs in, which is also its HOME."""
    return os.path.dirname(xwpe.srcfile)


def crash_reports(xwpe):
    """The crash reports the editor's own handler wrote in this session.

    The editor writes one under its state folder ($XDG_STATE_HOME/xwpe, or
    ~/.local/state/xwpe) and, with no usable state folder, one in /tmp under
    its own name.  HOME is this row's folder, so both are looked for there;
    a report anywhere in it means the editor died of a signal it caught."""
    found = []
    wd = workdir(xwpe)
    for base, _dirs, files in os.walk(wd):
        for name in files:
            if CRASH_NAME.match(name):
                found.append(os.path.relpath(os.path.join(base, name), wd))
    return sorted(found)


def wait_for(predicate, what, timeout=BUILD_WAIT_S, proc=None):
    """Poll @predicate until it holds, the editor dies, or @timeout passes."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc is not None:
            assert proc.poll() is None, "xwpe died waiting for %s" % what
        if predicate():
            return True
        time.sleep(0.25)
    return False


def folder(xwpe):
    return set(os.listdir(workdir(xwpe)))


def open_project_window(xwpe):
    """Alt-P p, Return (focus Files), Return (open the project): the Project
    window comes to the front."""
    before = xwpe.screenshot()
    xwpe.key("alt+p", delay=0.8)
    xwpe.key("p", delay=1.0)
    xwpe.key("Return", delay=1.0)
    xwpe.key("Return", delay=1.5)
    assert changed_pixels(before, xwpe.screenshot()) > 2000, \
        "the Project window did not open"


def build_from_project_window(xwpe):
    """F9 in the Project window, then wait for the build to leave its output
    and for the editor to settle with the Project window back on top."""
    wd = workdir(xwpe)
    before = folder(xwpe)
    xwpe.key("F9", delay=1.0)
    assert wait_for(lambda: set(os.listdir(wd)) != before,
                    "the build to leave its output", proc=xwpe.proc), \
        "the build of the project left nothing in %s" % sorted(before)
    assert wait_for(lambda: os.path.exists(os.path.join(wd, EXE_NAME)),
                    "the build to link %s" % EXE_NAME, proc=xwpe.proc), \
        "the build did not link %s: %s" % (EXE_NAME, os.listdir(wd))
    time.sleep(SETTLE_S)


def sources_unchanged(xwpe, why, source=None):
    wd = workdir(xwpe)
    with open(os.path.join(wd, PROJECT_NAME)) as fh:
        assert fh.read() == PROJECT, "the project file changed: %s" % why
    with open(xwpe.srcfile) as fh:
        assert fh.read() == (source if source is not None else DEFAULT_SOURCE), \
            "the source changed: %s" % why


def project_folder(xwpe):
    """Write the project file beside the source the fixture opened."""
    with open(os.path.join(workdir(xwpe), PROJECT_NAME), "w") as fh:
        fh.write(PROJECT)


# --- K5: a letter after the build ---------------------------------------

def test_x11_a_letter_after_a_build_from_the_project_window_stays_out_of_the_text(
        xwpe):
    """A build started in the Project window ends and puts that window back
    on top while the editor waits for a key.  The letter that follows belongs
    to the window the user now sees: the editor lives, it wrote no crash
    report, and neither the project file nor the source gained the letter."""
    project_folder(xwpe)
    open_project_window(xwpe)
    build_from_project_window(xwpe)
    xwpe.key("x", delay=1.0)
    assert xwpe.proc.poll() is None, "xwpe died on the key after the build"
    assert crash_reports(xwpe) == [], \
        "xwpe wrote a crash report: %s" % crash_reports(xwpe)
    sources_unchanged(xwpe, "the letter landed in a file")


# --- K12: Esc after the build -------------------------------------------

def test_x11_esc_after_a_build_from_the_project_window_leaves_the_editor_alive(
        xwpe):
    """Esc after the build is the Project window's own Esc, not a change to a
    text nobody is looking at: the editor lives and wrote no crash report."""
    project_folder(xwpe)
    open_project_window(xwpe)
    build_from_project_window(xwpe)
    xwpe.key("Escape", delay=1.2)
    assert xwpe.proc.poll() is None, "xwpe died on Esc after the build"
    assert crash_reports(xwpe) == [], \
        "xwpe wrote a crash report: %s" % crash_reports(xwpe)
    sources_unchanged(xwpe, "Esc changed a file")


# --- K13: a click in the project list after the build --------------------

# The Project window is a dialog of fixed size: on the suite's 1024x768 screen
# its list of files spans the upper-left quarter, so a click at this fraction
# of the screen lands on a row of the list.  The centre of the screen is the
# text window behind the dialog, where a click reads a text payload and says
# nothing about the pun.
LIST_CLICK = (0.27, 0.31)


def test_x11_a_click_in_the_project_list_after_a_build_leaves_the_editor_alive(
        xwpe):
    """A click in the project list after the build reaches the mouse handler
    through the window the text loop holds: on the base that is the payload it
    fetched before the wait, so e_mouse_cursor reads a list's rows as a file's
    text and dies of SIGSEGV.  The editor's own handler runs before a
    sanitizer can say anything, so the crash report it writes is the evidence,
    here as on the terminal.  A click at the centre of the screen lands on the
    text window behind the dialog and proves nothing, which is why this row
    clicks the list itself."""
    project_folder(xwpe)
    open_project_window(xwpe)
    build_from_project_window(xwpe)
    img = xwpe.screenshot()
    wide, high = img.size
    xwpe.click(int(wide * LIST_CLICK[0]), int(high * LIST_CLICK[1]), delay=1.0)
    assert xwpe.proc.poll() is None, "xwpe died on the click after the build"
    assert crash_reports(xwpe) == [], \
        "xwpe wrote a crash report: %s" % crash_reports(xwpe)
    sources_unchanged(xwpe, "the click changed a file")


# --- K20: the build ends behind a menu, and the text loop replays it -----

@pytest.mark.parametrize("xwpe", [SLOW_BUILD], indirect=True)
def test_x11_a_build_that_ends_behind_a_menu_is_not_read_as_the_project_windows_text(
        xwpe):
    """The Project window is on top, a slow build runs, and the File menu is
    open when it ends: the screen's owner keeps the build's end behind the
    menu and the text loop replays it after Escape.  The replay puts the
    Project window on top, and the entry check has already run for this key,
    so the letter that follows is read through the window the replay left.
    This is the second path to the same stale read, and it runs after the
    display's notice, so a row on the notice alone does not cover it."""
    project_folder(xwpe)
    open_project_window(xwpe)
    wd = workdir(xwpe)
    before = folder(xwpe)
    xwpe.key("F9", delay=1.0)             # a compile long enough to end behind a box
    xwpe.key("alt+f", delay=1.0)          # the File menu takes the screen
    assert wait_for(lambda: set(os.listdir(wd)) != before,
                    "the slow build to leave its output", proc=xwpe.proc), \
        "the build left nothing in %s" % sorted(before)
    xwpe.key("Escape", delay=SETTLE_S)    # the menu gives the screen back: replayed
    xwpe.key("x", delay=1.0)
    assert xwpe.proc.poll() is None, "xwpe died on the key after the replay"
    assert crash_reports(xwpe) == [], \
        "xwpe wrote a crash report: %s" % crash_reports(xwpe)
    sources_unchanged(xwpe, "the replayed build's end wrote a letter",
                      source=SLOW_SOURCE)
