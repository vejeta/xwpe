"""Graphical key decoding: the same keys as the terminal and Wayland.

The three displays share one key table (we_keymap.c).  These rows drive the
graphical reader with real key events: a chord the Debug menu shows acts, and
a key the terminal gives a meaning gives it here too.  The Wayland twin runs
this body under its own fixture (tests/wayland/test_keymap.py); the key
decoder is also covered by the compositor-free selftest
(tests/test_wayland_keysym.py).
"""
import os
import shutil
import sys
import time

import pytest

from conftest import changed_pixels, _sleep

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import procinfo  # noqa: E402  (the tests/ directory is added just above)

# A program that waits for a line on stdin, so the session is running.
BLOCKED_PROG = (
    "#include <stdio.h>\n"
    "int main(void)\n"
    "{\n"
    "  char line[100];\n"
    "  FILE *o;\n"
    "  o = fopen(\"started.txt\", \"w\");\n"
    "  fputs(\"1\", o);\n"
    "  fclose(o);\n"
    "  if (fgets(line, sizeof line, stdin)) {\n"
    "    o = fopen(\"got.txt\", \"w\");\n"
    "    fputs(line, o);\n"
    "    fclose(o);\n"
    "  }\n"
    "  return 0;\n"
    "}\n"
)
BREAK_LINE = 14
RUN_TIMEOUT_S = 40
BREAKPOINT_PIXELS = 200


def _wait(predicate, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        _sleep(0.3)
    return predicate()


def _gdb(xwpe):
    """The live gdb among the editor's descendants."""
    found = []
    todo = [xwpe.proc.pid]
    while todo:
        for pid in procinfo._child_pids(todo.pop()):
            todo.append(pid)
            if procinfo._live_and_named(pid, "gdb"):
                found.append(pid)
    return found


# @scenario SC-debug-26
def test_ctrl_f8_marks_the_breakpoint_line(xwpe):
    """The chord the Debug menu shows for Toggle Breakpoint (^F8) marks the
    line under the cursor, as the terminal's ^F8 does."""
    xwpe.key("Down"); xwpe.key("Down")
    before = xwpe.screenshot()
    xwpe.key("ctrl+F8")
    _sleep(0.3)
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died on ^F8"
    assert changed_pixels(before, after) > BREAKPOINT_PIXELS, \
        "^F8 should mark the breakpoint line (screen did not change)"


# @scenario SC-debug-62
def test_alt_right_moves_by_a_word_as_it_does_in_the_terminal(xwpe):
    """The terminal reads ESC then Right as Alt-Right and moves a word; the
    graphical reader had no Alt-Right and the cursor stayed.  It is one key
    table now: Alt-Right takes the cursor off the first column."""
    xwpe.key("alt+Right")
    xwpe.type("X")
    xwpe.save()
    first = xwpe.saved_text().splitlines()[0]
    assert "X" in first and not first.startswith("X"), \
        "Alt-Right should move the cursor a word right, the first line is %r" % first


WORDS_SOURCE = "abcdef ghi\nsecond\n"


# @scenario SC-debug-62
@pytest.mark.parametrize("xwpe", [{"XWPE_TEST_SOURCE": WORDS_SOURCE}],
                         indirect=True)
def test_shift_right_marks_a_block_as_it_does_in_the_terminal(xwpe):
    """Shift-Right twice marks the first two characters and Ctrl-K Y
    deletes them, as in the terminal: "abcdef" is "cdef" on disk."""
    xwpe.key("shift+Right", "shift+Right")
    xwpe.key("ctrl+k", "y")
    xwpe.save()
    first = xwpe.saved_text().splitlines()[0]
    assert first == "cdef ghi", \
        "Shift-Right twice then Ctrl-K Y should delete 'ab', line 1 is %r" % first


# @scenario SC-debug-62
@pytest.mark.parametrize("xwpe", [{"XWPE_TEST_SOURCE": WORDS_SOURCE}],
                         indirect=True)
def test_ctrl_right_moves_by_a_word_as_it_does_in_the_terminal(xwpe):
    """Ctrl-Right goes to the next word, as in the terminal: an X typed
    after it lands before "ghi"."""
    xwpe.key("ctrl+Right")
    xwpe.type("X")
    xwpe.save()
    first = xwpe.saved_text().splitlines()[0]
    assert first == "abcdef Xghi", \
        "Ctrl-Right should move the cursor a word right, line 1 is %r" % first


LINES_SOURCE = "a\nb\nc\n"


# @scenario SC-keymap-13
@pytest.mark.parametrize("xwpe", [{"XWPE_TEST_SOURCE": LINES_SOURCE}],
                         indirect=True)
@pytest.mark.parametrize("chord", ["ctrl+y", "ctrl+shift+y"])
def test_ctrl_shift_letter_does_what_ctrl_letter_does(xwpe, chord):
    """Ctrl-Y deletes a line.  Ctrl-Shift-Y is the same key in the Borland
    key set, and on every display (the terminal's twin is in
    tests/test_keymap_terminal.py): the first line is gone on disk."""
    xwpe.key(chord)
    xwpe.save()
    assert xwpe.saved_text() == "b\nc\n", \
        "%s should delete the first line, the file is %r" \
        % (chord, xwpe.saved_text())


# @scenario SC-keymap-14
def test_ctrl_h_is_backspace_and_alt_letter_is_not_text(xwpe):
    """Ctrl-H is Backspace (its control byte); Alt-F opens the File menu and
    types nothing.  Both go through the chord owner now."""
    xwpe.type("ab")
    xwpe.key("ctrl+h")
    xwpe.save()
    first = xwpe.saved_text().splitlines()[0]
    assert first.startswith("a") and not first.startswith("ab"), \
        "Ctrl-H should delete the b just typed, line 1 is %r" % first


# @scenario SC-debug-62
def test_f11_does_not_quit_the_editor(xwpe):
    """F11 sends the same symbol as the Sun Stop key, which the graphical
    reader took as Quit: a PC keyboard's F11 closed the editor on X11 and did
    nothing in the terminal.  It does nothing on every display now; the file
    is still open and takes typing."""
    xwpe.key("F11", delay=0.8)
    assert xwpe.proc.poll() is None, "F11 should not quit the editor"
    xwpe.type("Z")
    xwpe.save()
    assert xwpe.saved_text().startswith("Z"), \
        "the editor should still take typing after F11"


# @scenario SC-debug-26
@pytest.mark.skipif(not shutil.which("gdb") or not shutil.which("gcc"),
                    reason="no gdb or gcc")
@pytest.mark.parametrize("xwpe", [{"XWPE_TEST_SOURCE": BLOCKED_PROG}],
                         indirect=True)
def test_ctrl_f2_asks_to_quit_the_debugger_and_yes_ends_it(xwpe):
    """Graphical twin of the terminal row: the ^F2 Quit chord the Debug menu shows asks
    whether to quit while the program runs, and Yes ends gdb."""
    for _ in range(BREAK_LINE - 1):
        xwpe.key("Down", delay=0.1)
    xwpe.key("ctrl+g", "b")
    xwpe.key("ctrl+g", "r")
    started = os.path.join(os.path.dirname(xwpe.srcfile), "started.txt")
    assert _wait(lambda: os.path.exists(started), RUN_TIMEOUT_S), \
        "the program should build, start under gdb and run"
    assert _gdb(xwpe), "gdb should be running"
    _sleep(1.0)
    before = xwpe.screenshot()
    xwpe.key("ctrl+F2", delay=0.8)
    assert changed_pixels(before, xwpe.screenshot()) > BREAKPOINT_PIXELS, \
        "^F2 should open the Quit Debugger box (screen did not change)"
    xwpe.key("y", delay=0.6)
    assert _wait(lambda: not _gdb(xwpe), 10), \
        "answering Yes to ^F2 should end gdb"
    assert xwpe.proc.poll() is None, "xwpe died ending the session"
