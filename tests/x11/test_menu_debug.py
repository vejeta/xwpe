"""Debug-menu (Alt-D) tests for xwpe (X11/Xft).

Full gdb debug sessions (Trace/Step/breakpoints/watches, Ctrl-G ...) are
covered by the wpe debug tests.  Here we cover what the graphical key reader
decides on its own: while gdb runs a program that waits for input, Debug >
Quit from the menu ends gdb and typed text (a character above Latin-1
included) reaches the program.  Toggle Breakpoint (F5), which needs no
running program, marks the current line, detected as a screenshot change.
"""
import os
import sys
import time

import pytest

from conftest import changed_pixels, _sleep

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import procinfo  # noqa: E402  (the tests/ directory is added just above)

# A program that waits for a line on stdin and writes what it read to a file,
# so the test sees what reached the program from outside the editor.
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
BREAK_LINE = 14                    # return 0;  after the line is read
RUN_TIMEOUT_S = 40


def _gdb(xwpe):
    """The live gdb among the editor's descendants: under X11 the debugger
    is started through a helper process (we_sys), not by the editor itself."""
    found = []
    todo = [xwpe.proc.pid]
    while todo:
        for pid in procinfo._child_pids(todo.pop()):
            todo.append(pid)
            if procinfo._live_and_named(pid, "gdb"):
                found.append(pid)
    return found


def _wait(predicate, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        _sleep(0.3)
    return predicate()


def _run_until_blocked(xwpe):
    """Breakpoint after the read, Ctrl-G R; return once the program has
    started and waits on its stdin, which is a running (not stopped) session."""
    for _ in range(BREAK_LINE - 1):
        xwpe.key("Down", delay=0.1)
    xwpe.key("ctrl+g", "b")
    xwpe.key("ctrl+g", "r")
    started = os.path.join(os.path.dirname(xwpe.srcfile), "started.txt")
    assert _wait(lambda: os.path.exists(started), RUN_TIMEOUT_S), \
        "the program should build, start under gdb and run"
    assert _gdb(xwpe), "gdb should be running"
    _sleep(1.0)


# Wayland cannot drive a debug session headless here (the editor cannot
# create /dev/tty under the compositor test harness, old binary included), so
# this is the graphical twin of the terminal test and the Wayland row stays a
# manual check in WAYLAND-TESTING.txt.
@pytest.mark.skipif(not __import__("shutil").which("gdb")
                    or not __import__("shutil").which("gcc"),
                    reason="no gdb or gcc")
@pytest.mark.parametrize("xwpe", [{"XWPE_TEST_SOURCE": BLOCKED_PROG}],
                         indirect=True)
def test_debug_quit_from_the_menu_while_the_program_runs(xwpe):
    """Alt-D, Q while the program runs ends gdb (the menu key was typed into
    the program's console, so gdb ran on)."""
    _run_until_blocked(xwpe)
    xwpe.menu("d", "q")
    assert _wait(lambda: not _gdb(xwpe), 10), \
        "Debug > Quit should end gdb while the program runs"
    assert xwpe.proc.poll() is None, "xwpe died ending the session"


@pytest.mark.skipif(not __import__("shutil").which("gdb")
                    or not __import__("shutil").which("gcc"),
                    reason="no gdb or gcc")
@pytest.mark.parametrize("xwpe", [{"XWPE_TEST_SOURCE": BLOCKED_PROG}],
                         indirect=True)
def test_text_typed_while_the_program_runs_reaches_the_program(xwpe):
    """The line typed and Enter go to the program, which writes it to a file.
    The g with a breve (U+011F) has the same number as Alt-D's key code, so
    it reaches the program only because the key reader marks it as a typed
    character."""
    _run_until_blocked(xwpe)
    xwpe.type("hiğ")
    xwpe.key("Return")
    got = os.path.join(os.path.dirname(xwpe.srcfile), "got.txt")
    assert _wait(lambda: os.path.exists(got), 10), \
        "the program should have read the typed line"
    with open(got, encoding="utf-8") as fh:
        assert fh.read().strip() == "hiğ"


def test_toggle_breakpoint_marks_the_line(xwpe):
    """Debug -> Toggle Breakpoint (F5) paints a breakpoint marker on the line."""
    xwpe.key("Down"); xwpe.key("Down")   # move onto an interior code line
    before = xwpe.screenshot()
    xwpe.key("F5")                       # Debug -> Toggle Breakpoint
    time.sleep(0.3)
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died toggling a breakpoint"
    assert changed_pixels(before, after) > 200, \
        "Toggle Breakpoint should mark the line (screen did not change)"


def test_toggle_breakpoint_via_ctrl_g_b(xwpe):
    """^G B (the build-independent debugger prefix) toggles the breakpoint.

    The advertised "F5" accelerator is the Window Zoom key in the default key
    style, so ^G B is the reliable accelerator to assert here -- and it goes
    through the X11 keyboard decode (we_xterm.c) independently of ncurses."""
    xwpe.key("Down"); xwpe.key("Down")   # move onto an interior code line
    before = xwpe.screenshot()
    xwpe.key("ctrl+g", "b")              # debugger prefix + Toggle Breakpoint
    time.sleep(0.3)
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died on ^G B"
    assert changed_pixels(before, after) > 200, \
        "^G B should mark the breakpoint line (screen did not change)"
