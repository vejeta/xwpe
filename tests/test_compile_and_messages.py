"""Compile, the Messages window and the error walk, as every user meets them.

F9 builds the file in the active window, the compiler's own lines land in
Messages, and the editor puts the cursor on the line a diagnostic names.
These tests pin what already behaves: they drive the real compilers, and
assert on the editor's status row (the line the cursor is on), on the
Messages window as painted, and on the build products on disk.

Each test that needs a compiler skips itself with a message naming the
binary when it is not installed.
"""
import os
import re

import pytest

from wpe_driver import WpeSession, tool_usable, wait_screen

F9 = "\033[20~"            # Make: compile and link
CTRL_F9 = "\033[20;5~"     # Run
F6 = "\033[17~"            # next window
ALT_T = "\033t"            # next error
ALT_V = "\033v"            # previous error
ALT_O = "\033o"            # Options menu
ALT_C = "\033c"            # in a dialog: the Compiler field
UP = "\033[A"
ENTER = "\r"
ESC = "\033"

# A compile that finds nothing to say still needs the compiler to start;
# the slow toolchains (JVM, go, rustc) need longer.  Every wait is scaled
# by XWPE_TEST_WAIT_SCALE inside the driver.
BUILD_WAIT = 4.0
SLOW_BUILD_WAIT = 15.0
FIND_TIMEOUT = 20
SLOW_FIND_TIMEOUT = 60

# The active window shows "line:column" on its bottom border; an inactive
# window shows a plain border.
STATUS_RE = re.compile(r"^m+q[A-Z][* ]q\s+(\d+):(\d+)\s+q")
ACTIVE_MARK = "\u25a1 \u2715"      # the zoom and close buttons of the active window


# Wide enough that a compiler line naming a file under pytest's temporary
# directory is painted whole, so the needles a test waits for are on screen.
WIDE = 200


def session(workdir, src, filename):
    """A wide editor session on ``filename`` holding ``src``."""
    return WpeSession(str(workdir), src, filename=filename, cols=WIDE)


def need(tool, *probe):
    """Skip the test, naming the binary, when ``tool`` is not usable."""
    if not tool_usable(tool, *probe):
        pytest.skip("no %s" % tool)


def active_position(session):
    """(line, column) the status row of the active window shows."""
    for row in session.display():
        m = STATUS_RE.match(row)
        if m:
            return int(m.group(1)), int(m.group(2))
    raise AssertionError("no status row on screen:\n" + "\n".join(session.display()))


def active_title(session):
    """The title of the active window: the framed name on the row that
    carries the zoom and close buttons."""
    for row in session.display():
        if ACTIVE_MARK in row:
            m = re.search(r"q ([^q]+?) q", row)
            if m:
                return m.group(1)
    raise AssertionError("no active window on screen:\n" + "\n".join(session.display()))


def screen_text(session):
    return "\n".join(session.display())


def leave_error_line(session, lines=10):
    """Move the cursor of the source window up and away from the error, so a
    following jump is visible as a change of line."""
    session.key(*([UP] * lines), delay=0.2)


ONE_ERROR_C = ("#include <stdio.h>\n"
               "\n"
               "int main(void)\n"
               "{\n"
               "    int x = 1;\n"
               "    printf(\"XQ%d\\n\", x)\n"     # line 6: missing ';'
               "    return 0;\n"
               "}\n")

TWO_WARNINGS_C = ("#include <stdio.h>\n"
                  "\n"
                  "int main(void)\n"
                  "{\n"
                  "    int a = 1 / 0;\n"            # line 5: warning
                  "    int b = 2 / 0;\n"            # line 6: warning
                  "    printf(\"%d\\n\", a + b);\n"
                  "    return 0;\n"
                  "}\n")


# @scenario SC-compile-02
def test_a_fixed_error_rebuilds_and_messages_says_success(tmp_path):
    """The user adds the missing semicolon where F9 left the cursor and
    presses F9 again: Messages reports Success for the compile and for the
    link, and the object and the program exist."""
    need("gcc")
    with session(tmp_path, ONE_ERROR_C, "t.c") as w:
        w.key(F9, delay=BUILD_WAIT)
        assert wait_screen(w, "error", tmo=FIND_TIMEOUT), screen_text(w)
        line, _ = active_position(w)
        assert line == 6, "F9 did not leave the cursor on the error line:\n" + screen_text(w)
        # The jump leaves the cursor at the end of the line, after the
        # closing parenthesis, which is where the semicolon goes.
        w.key(";", delay=0.3)
        w.key(F9, delay=BUILD_WAIT)
        assert wait_screen(w, "Success", tmo=FIND_TIMEOUT), screen_text(w)
        screen = screen_text(w)
        assert w.alive()
    assert screen.count("Success") >= 2, \
        "Messages should say Success for the compile and the link:\n" + screen
    assert "error:" not in screen, "an error is still listed:\n" + screen
    assert "printf(\"XQ%d\\n\", x);" in (tmp_path / "t.c").read_text(), \
        "the fix was not saved before the build"
    assert (tmp_path / "t.o").exists() and (tmp_path / "t.e").exists(), \
        sorted(os.listdir(tmp_path))


# @scenario SC-compile-03
def test_ctrl_f9_runs_the_program_and_messages_shows_its_output(tmp_path):
    """Ctrl-F9 builds and runs: Messages frames the program's output and
    ends with its return code; the source window is untouched."""
    need("gcc")
    src = ("#include <stdio.h>\n"
           "int main(void){int n = 6 * 7; printf(\"RUN%dOK\\n\", n); return 3;}\n")
    with session(tmp_path, src, "t.c") as w:
        w.key(CTRL_F9, delay=BUILD_WAIT)
        assert wait_screen(w, "Return-Code", tmo=FIND_TIMEOUT), screen_text(w)
        screen = screen_text(w)
        assert w.alive()
    assert "RUN42OK" in screen, "the program's computed output is missing:\n" + screen
    assert "Return-Code: 3" in screen, screen
    assert "--- End output" in screen, screen
    assert (tmp_path / "t.c").read_text() == src


# @scenario SC-compile-04
def test_a_warning_only_build_succeeds_and_alt_t_walks_the_warnings(tmp_path):
    """Warnings do not fail the build: Messages shows the warning rows and
    Success, the program is linked, and Alt-T then steps to each warning's
    line in order."""
    need("gcc")
    with session(tmp_path, TWO_WARNINGS_C, "t.c") as w:
        w.key(F9, delay=BUILD_WAIT)
        assert wait_screen(w, "Success", tmo=FIND_TIMEOUT), screen_text(w)
        assert (tmp_path / "t.e").exists(), "a warning-only build did not link"
        w.key(ALT_T, delay=1.0)
        first, _ = active_position(w)
        w.key(ALT_T, delay=1.0)
        second, _ = active_position(w)
        w.key(ALT_V, delay=1.0)
        back, _ = active_position(w)
        assert w.alive()
    assert (first, second, back) == (5, 6, 5), \
        "Alt-T / Alt-V did not walk the warnings 5 -> 6 -> 5: %r" % ((first, second, back),)


# @scenario SC-compile-07
def test_a_missing_compiler_is_named_and_nothing_hangs(tmp_path):
    """The C compiler points at a binary that does not exist: F9 shows the
    command it tried, names the binary in the error row, and the editor
    goes on taking keys."""
    (tmp_path / "xwperc").write_text("[Language C]\nCompiler : nosuch_cc_for_test\n")
    with session(tmp_path, "int main(void){return 0;}\n", "t.c") as w:
        w.key(F9, delay=BUILD_WAIT)
        assert wait_screen(w, "Error at Command", tmo=FIND_TIMEOUT), screen_text(w)
        row = next(r for r in w.display() if "Error at Command" in r)
        assert "nosuch_cc_for_test" in row, row
        w.key(F6, delay=0.5)
        w.key("Z", delay=0.5)
        w.save()
        assert w.alive()
    assert (tmp_path / "t.c").read_text().startswith("Z"), \
        "the editor stopped taking keys after the failed start"


def build(w, slow=False):
    """Press F9 and give the compiler time to start; the slow toolchains
    (a JVM, go, rustc) get longer."""
    if slow:
        w.key(F9, delay=SLOW_BUILD_WAIT)
    else:
        w.key(F9, delay=BUILD_WAIT)


def assert_enter_jumps(w, needle, filename, line, slow=False):
    """After F9 has shown ``needle`` in Messages, the user moves the source
    cursor away, goes to Messages with F6 and presses Enter: the cursor of
    ``filename`` lands on ``line``."""
    timeout = FIND_TIMEOUT
    if slow:
        timeout = SLOW_FIND_TIMEOUT
    assert wait_screen(w, needle, tmo=timeout), screen_text(w)
    if active_title(w) != "Messages":
        leave_error_line(w)
        w.key(F6, delay=0.8)
    assert active_title(w) == "Messages", screen_text(w)
    w.key(ENTER, delay=1.5)
    assert active_title(w) == filename, \
        "Enter in Messages did not go to %s:\n%s" % (filename, screen_text(w))
    got, _ = active_position(w)
    assert got == line, "Enter went to line %d, not %d:\n%s" % (got, line, screen_text(w))
    assert w.alive()


# @scenario SC-compile-10
def test_cpp_error_row_jumps_to_its_line(tmp_path):
    need("g++")
    src = "int main()\n{\n    int x = 1\n    return x;\n}\n"      # error on line 4
    with session(tmp_path, src, "t.cpp") as w:
        build(w)
        assert_enter_jumps(w, "error:", "t.cpp", 4)


# @scenario SC-compile-11
def test_fortran_error_row_jumps_to_its_line(tmp_path):
    need("gfortran")
    src = ("program t\n  implicit none\n  integer :: x\n"
           "  x = y + 1\n  print *, x\nend program t\n")           # line 4
    with session(tmp_path, src, "t.f90") as w:
        build(w)
        assert_enter_jumps(w, "IMPLICIT type", "t.f90", 4)


# @scenario SC-compile-14
def test_java_error_row_jumps_to_its_line(tmp_path):
    need("javac", "-version")
    src = ("public class T {\n  public static void main(String[] a) {\n"
           "    int x = undefinedThing;\n  }\n}\n")                  # line 3
    with session(tmp_path, src, "T.java") as w:
        build(w, slow=True)
        assert_enter_jumps(w, "cannot find symbol", "T.java", 3, slow=True)


# @scenario SC-compile-16
def test_rust_error_row_jumps_to_its_line(tmp_path):
    need("rustc")
    src = "fn main() {\n    let _x: i32 = \"a\";\n}\n"               # line 2
    with session(tmp_path, src, "t.rs") as w:
        build(w, slow=True)
        assert_enter_jumps(w, "mismatched types", "t.rs", 2, slow=True)


# @scenario SC-compile-17
def test_go_error_row_jumps_to_its_line(tmp_path):
    need("go", "version")
    src = "package main\n\nfunc main() {\n\tx := undefinedThing\n\t_ = x\n}\n"   # line 4
    with session(tmp_path, src, "t.go") as w:
        build(w, slow=True)
        assert_enter_jumps(w, "undefined: undefinedThing", "t.go", 4, slow=True)


# @scenario SC-compile-18
def test_python_syntax_error_row_jumps_to_its_line(tmp_path):
    need("python3")
    src = "x = 1\n\ndef f(:\n    pass\n"                              # line 3
    with session(tmp_path, src, "t.py") as w:
        build(w)
        assert_enter_jumps(w, "SyntaxError", "t.py", 3)


COBOL_ONE_ERROR = ("       IDENTIFICATION DIVISION.\n"
                   "       PROGRAM-ID. T.\n"
                   "       PROCEDURE DIVISION.\n"
                   "           DISPLAY \"HI\"\n"
                   "           MOVE 1 TO UNDEFVAR.\n"          # line 5
                   "           STOP RUN.\n")


# @scenario SC-compile-19
def test_cobol_error_row_jumps_to_its_line(tmp_path):
    need("cobc")
    with session(tmp_path, COBOL_ONE_ERROR, "t.cob") as w:
        build(w)
        assert_enter_jumps(w, "is not defined", "t.cob", 5)


def open_c_compiler_options(w):
    """Options > Compiler, then the C entry of the language list."""
    w.key(ALT_O, delay=0.6)
    w.key("c", delay=0.8)
    w.key("C", delay=0.8)
    assert "Compiler-Options" in screen_text(w), screen_text(w)


# @scenario SC-compile-21
def test_compiler_options_ok_is_used_by_the_next_f9_and_cancel_is_not(tmp_path):
    """At 80x24 the Compiler-Options dialog fits the screen; a compiler typed
    and accepted with Enter is what the next F9 runs; a compiler typed and
    abandoned with Esc changes nothing."""
    need("gcc")
    work_ok = tmp_path / "ok"
    work_cancel = tmp_path / "cancel"
    work_ok.mkdir()
    work_cancel.mkdir()
    src = "int main(void){return 0;}\n"

    with WpeSession(str(work_ok), src, filename="t.c", cols=80, rows=24) as w:
        open_c_compiler_options(w)
        rows = w.display()
        title = next(i for i, r in enumerate(rows) if "Compiler-Options" in r)
        buttons = next(i for i, r in enumerate(rows) if "Ok" in r and "Cancel" in r)
        # The buttons, then the dialog's bottom border, then the hint row.
        assert title >= 1 and buttons <= len(rows) - 3, \
            "the dialog does not fit 80x24:\n" + screen_text(w)
        w.key(ALT_C, delay=0.5)
        w.key("nosuch_cc_ok", delay=0.5)
        w.key(ENTER, delay=0.8)
        w.key(F9, delay=BUILD_WAIT)
        assert wait_screen(w, "Error at Command", tmo=FIND_TIMEOUT), screen_text(w)
        assert "nosuch_cc_ok -c" in screen_text(w), screen_text(w)

    with WpeSession(str(work_cancel), src, filename="t.c", cols=80, rows=24) as w:
        open_c_compiler_options(w)
        w.key(ALT_C, delay=0.5)
        w.key("nosuch_cc_cancel", delay=0.5)
        w.key(ESC, delay=1.0)
        w.key(F9, delay=BUILD_WAIT)
        assert wait_screen(w, "Success", tmo=FIND_TIMEOUT), screen_text(w)
        assert "nosuch_cc_cancel" not in screen_text(w), screen_text(w)
    assert (work_cancel / "t.e").exists(), "the default compiler did not build after Cancel"


# @scenario SC-compile-23
def test_f9_saves_an_unsaved_buffer_before_compiling(tmp_path):
    """The user edits and presses F9 without saving: the edit is written to
    disk first and the build compiles it (the error it introduces shows)."""
    need("gcc")
    with session(tmp_path, "int main(void){return 0;}\n", "t.c") as w:
        w.key("int bad = ;", ENTER, delay=0.2)
        w.key(F9, delay=BUILD_WAIT)
        assert wait_screen(w, "error", tmo=FIND_TIMEOUT), screen_text(w)
        screen = screen_text(w)
        assert w.alive()
    assert (tmp_path / "t.c").read_text().startswith("int bad = ;"), \
        "the unsaved edit was not written before the compile"
    assert "expected expression" in screen, \
        "the compiler did not see the edited text:\n" + screen
