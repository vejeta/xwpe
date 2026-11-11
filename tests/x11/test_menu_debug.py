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


# The graphical twin of the terminal test; the Wayland twin runs this body
# too, and WAYLAND-TESTING.txt keeps the same check on a real desktop.
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


# A program that writes a counter to a file ten times a second.  The file
# changes while the program runs and stops changing while gdb holds it, which
# is how the test sees a pause from outside the editor.
TICK_PROG = (
    "#include <stdio.h>\n"
    "#include <unistd.h>\n"
    "int main(void)\n"
    "{\n"
    "  int n;\n"
    "  for (n = 0; ; n++)\n"
    "  {\n"
    "    FILE *o = fopen(\"tick.txt\", \"w\");\n"
    "    fprintf(o, \"%d\", n);\n"
    "    fclose(o);\n"
    "    usleep(100000);\n"
    "  }\n"
    "  return 0;\n"
    "}\n"
)
TICK_SETTLE_S = 1.5       # long enough for a dozen ticks, or for none
TICK_PAUSE_WAIT_S = 10


def _tick(xwpe):
    """The counter the program last wrote, or None before it wrote one."""
    path = os.path.join(os.path.dirname(xwpe.srcfile), "tick.txt")
    try:
        with open(path) as fh:
            text = fh.read().strip()
    except OSError:
        return None
    return int(text) if text.isdigit() else None


def _ticks_move(xwpe):
    """True when the counter changes within a moment: the program runs."""
    before = _tick(xwpe)
    _sleep(TICK_SETTLE_S)
    after = _tick(xwpe)
    return before is not None and after is not None and after != before


@pytest.mark.skipif(not __import__("shutil").which("gdb")
                    or not __import__("shutil").which("gcc"),
                    reason="no gdb or gcc")
@pytest.mark.parametrize("xwpe", [{"XWPE_TEST_SOURCE": TICK_PROG}],
                         indirect=True)
@pytest.mark.parametrize("keystroke", ["ctrl+Pause", "Break"])
def test_ctrl_break_pauses(xwpe, keystroke):
    """Ctrl-Break (the key a PC keyboard sends as Break) stops the program
    gdb runs: its counter stops changing, and Ctrl-G R makes it run again.
    The graphical key reader must hand the editor the key as Ctrl-Break,
    which no menu or terminal test reaches.

    The Wayland twin runs this body; its key decoder is also covered by the
    keysym selftest (tests/test_wayland_keysym.py)."""
    xwpe.key("ctrl+g", "r", delay=2.0)          # to the start of main
    xwpe.key("ctrl+g", "r", delay=1.0)          # run on
    assert _wait(lambda: _ticks_move(xwpe), RUN_TIMEOUT_S), \
        "the program should run under gdb and write its counter"
    xwpe.key(keystroke, delay=1.0)
    assert _wait(lambda: not _ticks_move(xwpe), TICK_PAUSE_WAIT_S), \
        "%s should stop the program: its counter still changes" % keystroke
    xwpe.key("ctrl+g", "r", delay=1.0)
    assert _wait(lambda: _ticks_move(xwpe), RUN_TIMEOUT_S), \
        "Ctrl-G R should run the program on from the pause"
    assert xwpe.proc.poll() is None, "xwpe died pausing the program"


# A Java program that prints, then sleeps; the breakpoint is on the print.
JDB_SLEEP_PROG = (
    "public class P {\n"
    "    public static void main(String[] a) throws Exception {\n"
    "        System.out.println(\"ready\");\n"
    "        Thread.sleep(600000);\n"
    "    }\n"
    "}\n"
)
JDB_SLEEP_LINE = 3
JDB_START_TIMEOUT_S = 60
JDB_SETTLE_S = 3.0


@pytest.mark.skipif(not __import__("shutil").which("jdb")
                    or not __import__("shutil").which("javac"),
                    reason="no jdb or javac")
@pytest.mark.parametrize("xwpe", [{"XWPE_TEST_SOURCE": JDB_SLEEP_PROG,
                                   "XWPE_TEST_FILE": "P.java"}],
                         indirect=True)
def test_debug_quit_under_jdb_leaves_no_java_process(xwpe):
    """Alt-D, Q while the Java program sleeps ends the whole tree under the
    editor, the VM the editor started included: the graphical editor starts
    the debugger through a helper script, one process further down.

    An end-to-end guard only: here jdb also ends by itself once the script
    is killed (its input reaches its end), so this row passes with a plain
    kill of the script as well.  The VM is ended by its PID, as in the
    terminal editor."""
    for _ in range(JDB_SLEEP_LINE - 1):
        xwpe.key("Down", delay=0.1)
    xwpe.key("ctrl+g", "b")
    xwpe.key("ctrl+g", "r")
    assert _wait(lambda: len(procinfo.descendants(xwpe.proc.pid)) >= 3,
                 JDB_START_TIMEOUT_S), \
        "jdb and its VM should be running: %s" \
        % procinfo.descendants(xwpe.proc.pid)
    _sleep(JDB_SETTLE_S)
    xwpe.key("ctrl+g", "r")
    _sleep(JDB_SETTLE_S)
    tree = procinfo.descendants(xwpe.proc.pid)
    xwpe.menu("d", "q")
    assert _wait(lambda: procinfo.all_ended(tree), 10), \
        "no process of the session should be left (alive: %s)" \
        % [p for p in tree if not procinfo.all_ended([p])]
    assert xwpe.proc.poll() is None, "xwpe died ending the session"


# A Java program that sleeps, prints a block of text, then sleeps again.
JDB_PRINT_BLOCK = "#" * 60
JDB_PRINT_PROG = (
    "public class P {\n"
    "    public static void main(String[] a) throws Exception {\n"
    "        int x = 1;\n"
    "        Thread.sleep(4000);\n"
    "        for (int i = 0; i < 6; i++)\n"
    "            System.out.println(\"%s\");\n"
    "        Thread.sleep(600000);\n"
    "    }\n"
    "}\n" % JDB_PRINT_BLOCK
)
JDB_PRINT_LINE = 3
JDB_PRINT_SETTLE_S = 5.0      # the first stop is reached and painted
JDB_PRINT_BEFORE_S = 1.0      # the program sleeps, nothing printed yet
JDB_PRINT_AFTER_S = 6.0       # the sleep ended and the block was printed
JDB_PRINT_PIXELS = 3000       # six rows of '#' change far more than this


@pytest.mark.skipif(not __import__("shutil").which("jdb")
                    or not __import__("shutil").which("javac"),
                    reason="no jdb or javac")
@pytest.mark.parametrize("xwpe", [{"XWPE_TEST_SOURCE": JDB_PRINT_PROG,
                                   "XWPE_TEST_FILE": "P.java"}],
                         indirect=True)
def test_jdb_program_output_appears_in_messages(xwpe):
    """While the Java program runs, what it prints is painted in Messages
    as it prints: between two screenshots taken while it runs, only its
    output changes the screen."""
    for _ in range(JDB_PRINT_LINE - 1):
        xwpe.key("Down", delay=0.1)
    xwpe.key("ctrl+g", "b")
    xwpe.key("ctrl+g", "r")
    assert _wait(lambda: len(procinfo.descendants(xwpe.proc.pid)) >= 3,
                 JDB_START_TIMEOUT_S), \
        "jdb and its VM should be running: %s" \
        % procinfo.descendants(xwpe.proc.pid)
    _sleep(JDB_PRINT_SETTLE_S)
    xwpe.key("ctrl+g", "r")
    _sleep(JDB_PRINT_BEFORE_S)
    before = xwpe.screenshot()
    _sleep(JDB_PRINT_AFTER_S)
    after = xwpe.screenshot()
    assert changed_pixels(before, after) > JDB_PRINT_PIXELS, \
        "the program's output should be painted in Messages"
    xwpe.menu("d", "q")
    assert xwpe.proc.poll() is None, "xwpe died ending the session"


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
