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
import time

import pytest

from wpe_driver import WAIT_SCALE, WpeSession, tool_usable, wait_screen

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


# ---------------------------------------------------------------------------
# What a build prints, how it runs, and how its rows are walked.
# ---------------------------------------------------------------------------

ALT_A = "\033a"            # Run > Execute Make
PGUP = "\033[5~"
CTRL_PGDN = "\033[6;5~"    # end of the buffer
FLOOD_LINES = 1000
FLOOD_ROW_RE = re.compile(r"FLOODLINE (\d+)\b")


def scaled(seconds):
    """A wait budget stretched like every other wait in the driver."""
    return seconds * WAIT_SCALE


def flood_numbers(w):
    """The FLOODLINE numbers painted on screen, top row first."""
    found = []
    for row in w.display():
        m = FLOOD_ROW_RE.search(row)
        if m:
            found.append(int(m.group(1)))
    return found


# @scenario SC-compile-09
def test_every_line_of_a_large_build_output_reaches_messages_in_order(tmp_path):
    """A make that prints a thousand numbered lines in one burst: Messages
    holds every one of them, in the order printed.  The pump used to keep a
    partial line with its first byte overwritten and drop the next read."""
    makefile = "all:\n\t@seq -f 'FLOODLINE %%g' 1 %d\n\t@echo FLOOD_END\n" % FLOOD_LINES
    (tmp_path / "Makefile").write_text(makefile)
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    filename="t.c", cols=WIDE, rows=30) as w:
        w.key(ALT_A, delay=BUILD_WAIT)
        if active_title(w) != "Messages":
            w.key(F6, delay=0.8)
        w.key(CTRL_PGDN, delay=0.8)
        assert wait_screen(w, "FLOOD_END", tmo=scaled(FIND_TIMEOUT)), screen_text(w)
        last_line, _ = active_position(w)
        tail = flood_numbers(w)
        for _ in range(60):
            w.key(PGUP, delay=0.05)
        w.display()
        middle = flood_numbers(w)
        assert w.alive()
    # "make", the thousand lines and FLOOD_END come before the closing rows.
    assert last_line >= FLOOD_LINES + 2, \
        "Messages holds %d lines for %d printed" % (last_line, FLOOD_LINES + 2)
    assert tail and tail[-1] == FLOOD_LINES, "the last line is missing: %r" % tail
    for seen in (tail, middle):
        assert seen == list(range(seen[0], seen[0] + len(seen))), \
            "the lines are not consecutive: %r" % seen
    assert middle and middle[0] < tail[0], "PgUp did not reach earlier lines: %r" % middle


# @scenario SC-compile-12
def test_pascal_error_line_reaches_messages_and_enter_jumps(tmp_path):
    """fpc writes in 256-byte pieces; under a long directory the first piece
    ends inside the error line.  The whole line still reaches Messages, it
    is made a row, and the build's jump to the first error lands on the
    line it names."""
    need("fpc", "-iV")
    src = "program t;\nbegin\n  y := 1;\nend.\n"                  # line 3
    with session(tmp_path, src, "t.pas") as w:
        w.key(F9, delay=BUILD_WAIT)
        assert wait_screen(w, "Identifier not found", tmo=scaled(FIND_TIMEOUT)), \
            "the fpc error line was dropped:\n" + screen_text(w)
        assert active_title(w) == "t.pas", screen_text(w)
        line, _ = active_position(w)
        assert w.alive()
    assert line == 3, "the jump went to line %d, not 3" % line


# A make that takes a while and then produces its program.
SLOW_BUILD_SECONDS = 6
SLOW_MAKEFILE = ("all:\n\t@echo BUILD_START\n\t@sleep %s\n\t@echo BUILD_DONE\n"
                 "\t@touch prog\n")
WAITING_LINE = "building ..."
COUNT_ROW_RE = re.compile(r"\b(\d+) errors?, (\d+) warnings?\b")


def slow_build_dir(tmp_path):
    """A directory whose Makefile sleeps (scaled) before it makes ``prog``."""
    seconds = "%g" % scaled(SLOW_BUILD_SECONDS)
    (tmp_path / "Makefile").write_text(SLOW_MAKEFILE % seconds)
    return tmp_path


# @scenario SC-compile-08
def test_a_long_build_keeps_the_editor_and_esc_cancels_it(tmp_path):
    """While make sleeps, Messages shows the waiting line, the editor takes
    keys (F6 moves to the source window at once), and keys typed into
    Messages are not replayed into the file when the build ends.  Esc ends
    the build: "build cancelled", well before the make would have finished,
    and no program is produced."""
    src = "int main(void){return 0;}\n"
    slow_build_dir(tmp_path)
    with WpeSession(str(tmp_path), src, filename="t.c", cols=WIDE) as w:
        start = time.time()
        w.key(ALT_A, delay=0.8)
        assert wait_screen(w, WAITING_LINE, tmo=scaled(FIND_TIMEOUT)), \
            "no waiting line while the build runs:\n" + screen_text(w)
        assert active_title(w) == "Messages", screen_text(w)
        w.key("zz", delay=0.3)                  # read-only Messages: ignored
        w.key(F6, delay=0.5)
        answered = active_title(w)
        w.key(ESC, delay=0.5)
        assert wait_screen(w, "build cancelled", tmo=scaled(FIND_TIMEOUT)), \
            "Esc did not cancel the build:\n" + screen_text(w)
        elapsed = time.time() - start
        w.key(F6, delay=0.5)
        if active_title(w) != "t.c":
            w.key(F6, delay=0.5)
        w.save()
        assert w.alive()
    assert answered == "t.c", "F6 during the build was not answered: %r" % answered
    assert elapsed < scaled(SLOW_BUILD_SECONDS), \
        "the build ran to its end (%.1fs) instead of stopping on Esc" % elapsed
    assert (tmp_path / "t.c").read_text() == src, \
        "keys typed during the build reached the file: %r" % (tmp_path / "t.c").read_text()
    assert not (tmp_path / "prog").exists(), "a cancelled build produced its program"


# @scenario SC-compile-08
def test_a_long_build_left_alone_ends_with_the_count_row(tmp_path):
    """The same build, left alone, runs to its end: its last output, the
    program on disk, and the row that counts errors and warnings."""
    slow_build_dir(tmp_path)
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    filename="t.c", cols=WIDE) as w:
        w.key(ALT_A, delay=0.8)
        assert wait_screen(w, WAITING_LINE, tmo=scaled(FIND_TIMEOUT)), screen_text(w)
        assert wait_screen(w, "BUILD_DONE",
                           tmo=scaled(SLOW_BUILD_SECONDS + FIND_TIMEOUT)), screen_text(w)
        assert wait_screen(w, "0 errors, 0 warnings", tmo=scaled(FIND_TIMEOUT)), \
            "no count row after the build:\n" + screen_text(w)
        screen = screen_text(w)
        assert w.alive()
    assert WAITING_LINE not in screen, "the waiting line outlived the build:\n" + screen
    assert (tmp_path / "prog").exists(), "the build did not produce its program"


# The mark a warning row of Messages starts with.
WARNING_MARK = "⚠ "

ERROR_AT_COLUMN_5_C = ("int main(void)\n"
                       "{\n"
                       "    undef_x = 1;\n"              # 3:5
                       "    return 0;\n"
                       "}\n")


def messages_rows(w):
    """The text rows inside the Messages frame, top first."""
    rows = w.display()
    top = next(i for i, r in enumerate(rows) if " Messages " in r)
    found = []
    for r in rows[top + 1:]:
        if not r.startswith("x"):
            break
        found.append(r.rstrip()[1:-1].rstrip())     # between the borders
    return found


# Tall enough that Messages shows a whole small build at once.
TALL = 60


def tall_session(workdir, src, filename):
    """A wide and tall editor session on ``filename`` holding ``src``."""
    return WpeSession(str(workdir), src, filename=filename, cols=WIDE, rows=TALL)


def last_messages_row(w):
    """The last row of Messages that holds text."""
    filled = [r for r in messages_rows(w) if r.strip()]
    assert filled, "Messages is empty:\n" + screen_text(w)
    return filled[-1].strip()


# @scenario SC-compile-01
def test_one_error_lands_on_its_column_and_messages_counts_it(tmp_path):
    """gcc names 3:5; F9 puts the cursor on line 3, column 5, and the last
    row of Messages says "1 error, 0 warnings"."""
    need("gcc")
    with tall_session(tmp_path, ERROR_AT_COLUMN_5_C, "t.c") as w:
        w.key(F9, delay=BUILD_WAIT)
        assert wait_screen(w, "1 error, 0 warnings", tmo=scaled(FIND_TIMEOUT)), \
            "no count row after one error:\n" + screen_text(w)
        assert active_title(w) == "t.c", screen_text(w)
        position = active_position(w)
        last = last_messages_row(w)
        assert w.alive()
    assert position == (3, 5), "gcc says 3:5, the cursor is at %d:%d" % position
    assert last == "1 error, 0 warnings", "the count is not the last row: %r" % last


# @scenario SC-compile-04
def test_a_warning_only_build_counts_and_marks_its_warnings(tmp_path):
    """Two warnings and no error: the build succeeds, the last row says
    "0 errors, 2 warnings", and each warning row starts with the warning
    mark."""
    need("gcc")
    with tall_session(tmp_path, TWO_WARNINGS_C, "t.c") as w:
        w.key(F9, delay=BUILD_WAIT)
        assert wait_screen(w, "0 errors, 2 warnings", tmo=scaled(FIND_TIMEOUT)), \
            "no count row after a warning-only build:\n" + screen_text(w)
        rows = messages_rows(w)
        last = last_messages_row(w)
        assert w.alive()
    warning_rows = [r for r in rows if "warning:" in r]
    assert len(warning_rows) == 2, "Messages rows: %r" % rows
    for r in warning_rows:
        assert r.startswith(WARNING_MARK), "a warning row is not marked: %r" % r
    assert last == "0 errors, 2 warnings", "the count is not the last row: %r" % last
    assert (tmp_path / "t.e").exists(), "a warning-only build did not link"


# @scenario SC-compile-04
def test_a_clean_build_counts_nothing(tmp_path):
    """A build with nothing to say ends with "0 errors, 0 warnings"."""
    need("gcc")
    with session(tmp_path, "int main(void){return 0;}\n", "t.c") as w:
        w.key(F9, delay=BUILD_WAIT)
        assert wait_screen(w, "0 errors, 0 warnings", tmo=scaled(FIND_TIMEOUT)), \
            "no count row after a clean build:\n" + screen_text(w)
        assert w.alive()
    assert (tmp_path / "t.e").exists()


# a.c has an error on line 1, includes h.h on line 2 (an error on its line
# 3) and has another error on line 5: gcc prints a.c:1, then the include
# chain "In file included from a.c:2:" and h.h:3, then a.c:5.
INTERLEAVED_H = "/* helper header */\n\nstatic int hf(void) { return undef_h; }\n"
INTERLEAVED_A = ("int x = undef_a1;\n"
                 "#include \"h.h\"\n"
                 "int main(void)\n"
                 "{\n"
                 "    return hf() + undef_a5;\n"
                 "}\n")


def where(w):
    """(window title, line) of the active window."""
    return active_title(w), active_position(w)[0]


# @scenario SC-compile-05
def test_alt_t_walks_two_files_in_the_order_printed(tmp_path):
    """F9 lands on a.c:1; Alt-T then visits h.h:3 and a.c:5, in the order
    gcc printed them, and Alt-V steps back to h.h:3.  The include line
    a.c:2 is never a stop of its own."""
    need("gcc")
    (tmp_path / "h.h").write_text(INTERLEAVED_H)
    with session(tmp_path, INTERLEAVED_A, "a.c") as w:
        w.key(F9, delay=BUILD_WAIT)
        assert wait_screen(w, "undef_a5", tmo=scaled(FIND_TIMEOUT)), screen_text(w)
        walk = [where(w)]
        w.key(ALT_T, delay=1.0)
        walk.append(where(w))
        w.key(ALT_T, delay=1.0)
        walk.append(where(w))
        w.key(ALT_V, delay=1.0)
        back = where(w)
        assert w.alive()
    assert walk == [("a.c", 1), ("h.h", 3), ("a.c", 5)], \
        "the walk did not follow the order printed: %r" % walk
    assert back == ("h.h", 3), "Alt-V did not step back to h.h:3: %r" % (back,)
    assert ("a.c", 2) not in walk + [back], "the include line was a stop: %r" % walk


def sgr_click(col, row):
    """A left press and its release at 1-based screen (col, row), SGR 1006,
    as the terminal sends them: two reports."""
    return ("\x1b[<0;%d;%dM" % (col, row), "\x1b[<0;%d;%dm" % (col, row))


# The pause between the reports of a double click: well inside the editor's
# double-click interval, as a hand makes it.
DOUBLE_CLICK_GAP = 0.03


# Clicks land inside the Messages frame, clear of its border.
CLICK_COL = 10


def screen_row_with(w, needle):
    """The 1-based screen row that shows ``needle``."""
    rows = w.display()
    return next(i for i, r in enumerate(rows) if needle in r) + 1


def built_with_warnings(w):
    """F9 on TWO_WARNINGS_C: a success, the source window back on top."""
    w.key(F9, delay=BUILD_WAIT)
    assert wait_screen(w, "0 errors, 2 warnings", tmo=scaled(FIND_TIMEOUT)), screen_text(w)
    assert active_title(w) == "t.c", screen_text(w)


# @scenario SC-compile-06
def test_esc_in_messages_returns_to_the_source_at_the_line_it_left(tmp_path):
    """The user leaves the cursor on line 3 of t.c, goes to Messages with F6
    and presses Esc: t.c is active again, the cursor still on line 3."""
    need("gcc")
    with tall_session(tmp_path, TWO_WARNINGS_C, "t.c") as w:
        built_with_warnings(w)
        w.key(UP, UP, UP, delay=0.3)
        left = active_position(w)
        w.key(F6, delay=0.8)
        assert active_title(w) == "Messages", screen_text(w)
        w.key(ESC, delay=1.0)
        title = active_title(w)
        back = active_position(w)
        assert w.alive()
    assert title == "t.c", "Esc left the user in Messages"
    assert back == left, "Esc came back to %r, the cursor was at %r" % (back, left)


# @scenario SC-compile-06
def test_double_click_on_a_messages_row_jumps_to_it(tmp_path):
    """Two clicks in quick succession on the row of the warning at 6:15,
    from the source window: the cursor lands on line 6 of t.c."""
    need("gcc")
    with tall_session(tmp_path, TWO_WARNINGS_C, "t.c") as w:
        built_with_warnings(w)
        y = screen_row_with(w, "6:15: warning")
        w.key(*(sgr_click(CLICK_COL, y) + sgr_click(CLICK_COL, y)),
              delay=DOUBLE_CLICK_GAP)
        w.display(drain=1.0)
        title = active_title(w)
        line, _ = active_position(w)
        assert w.alive()
    assert title == "t.c", "the double click did not leave Messages"
    assert line == 6, "the double click went to line %d, not 6" % line


# @scenario SC-compile-06
def test_one_click_on_a_messages_row_only_moves_its_cursor(tmp_path):
    """In Messages, one click on the row of the warning at 6:15 stays in
    Messages, and so does a second click on it well after the first; Enter
    then goes to line 6, so the click had put the Messages cursor there."""
    need("gcc")
    with tall_session(tmp_path, TWO_WARNINGS_C, "t.c") as w:
        built_with_warnings(w)
        w.key(F6, delay=0.8)
        assert active_title(w) == "Messages", screen_text(w)
        y = screen_row_with(w, "6:15: warning")
        w.key(*sgr_click(CLICK_COL, y), delay=0.75)
        first = active_title(w)
        w.key(*sgr_click(CLICK_COL, y), delay=0.5)
        second = active_title(w)
        w.key(ENTER, delay=1.0)
        title = active_title(w)
        line, _ = active_position(w)
        assert w.alive()
    assert first == "Messages", "one click left Messages for %r" % first
    assert second == "Messages", "two separate clicks jumped like a double click"
    assert (title, line) == ("t.c", 6), \
        "the click did not put the Messages cursor on the row: Enter went to %s:%d" % (title, line)

