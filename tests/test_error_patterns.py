"""A language's error-message pattern never lets F9 read memory it did not fill.

A language set up in the option file with a compiler but no MessageString
has an empty error pattern, and a pattern can end in the middle of a
${?n:text} test.  Both used to leave the pattern's row buffer unterminated,
so the first F9 that reported errors read past it.  The plain build
survives that by luck; the sanitizer build reports a heap-buffer-overflow.

Run under the sanitizer build with
WPE_BIN=$PWD/wpe-asan ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 (the
binary must carry a wpe-prefixed name).  The rows set ASAN_OPTIONS
themselves to write the report beside the test, so they are red on the
sanitizer build.  On both builds each row also checks that the compiler's
output reached Messages and what F9 counted, so a row cannot pass because
the build never ran.
"""
import time

import pytest

from wpe_driver import WpeSession, wait_screen

F9 = "\033[20~"
BUILD_WAIT = 5.0
# The word the count line of a finished build carries ("1 error, 0 warnings").
COUNT_WORD = " warnings"
COUNT_WAIT = 25.0
SANITIZER = "AddressSanitizer"
COMPILER_ROW = "t.lua:1: syntax error near ="
NO_ROWS = "0 errors, 0 warnings"
ONE_ROW = "1 error, 0 warnings"


def build_row_number(n):
    """The row the numbered fake compiler prints on its n-th run."""
    return "build number %d" % n


def wait_build_end(w, marker, tmo):
    """Wait until ``marker`` is on screen with a count line somewhere below
    it: the build that printed ``marker`` has ended.  A timeout, never a
    sleep, ends a build that does not."""
    end = time.time() + tmo
    while time.time() < end:
        w._drain(0.4)
        rows = w.display(drain=0)
        for i, row in enumerate(rows):
            if marker in row and any(COUNT_WORD in r for r in rows[i + 1:]):
                return True
    return False


def run_f9_with_pattern(tmp_path, pattern_line, output=(COMPILER_ROW,),
                        count_wait=0.0, compiler_switch=1, builds=1):
    """F9 on a Lua file whose language has ``pattern_line`` as its pattern.

    ``output`` is what the fake compiler prints, one row per item.  With a
    ``count_wait`` the test waits up to that many seconds for the count line
    (a build that never ends then fails by timeout, never by a sleep).
    ``compiler_switch`` is the language's CompilerSwitch: 1 for the style
    that uses ``pattern_line``, 0 for the built-in GNU pattern.  With
    ``builds`` of 2 the fake compiler also prints "build number N" and F9 is
    pressed twice in the one session; the screen is read after the last.

    Returns the text of every sanitizer log the editor wrote and the screen
    after the build.
    """
    fake = tmp_path / "fake-lua"
    rows = "".join("printf '%%s\\n' '%s'\n" % row for row in output)
    counter = ""
    if builds > 1:
        counter = ("n=$(cat %s/count 2>/dev/null || echo 0); n=$((n+1)); "
                   "echo $n > %s/count; printf 'build number %%s\\n' $n\n"
                   % (tmp_path, tmp_path))
    fake.write_text("#!/bin/sh\n%s%sexit 1\n" % (rows, counter))
    fake.chmod(0o755)
    cfg = tmp_path / ".config" / "xwpe"
    cfg.mkdir(parents=True)
    (cfg / "xwperc").write_text(
        "[Language/Lua]\nFileExtension : .lua\nCompiler : %s\n"
        "CompilerOptions : -p\nCompilerSwitch : %d\nKey : L\nX : 20\n%s"
        % (fake, compiler_switch, pattern_line))
    env = {"ASAN_OPTIONS": "detect_leaks=0:log_path=%s" % (tmp_path / "asan.log")}
    with WpeSession(str(tmp_path), "x = = 1\n", filename="t.lua", cols=200,
                    env_extra=env) as w:
        w.key(F9, delay=BUILD_WAIT)
        for n in range(1, builds):
            assert wait_build_end(w, build_row_number(n), count_wait), n
            w.key(F9, delay=BUILD_WAIT)
        if builds > 1:
            assert wait_build_end(w, build_row_number(builds), count_wait)
        elif count_wait:
            wait_screen(w, COUNT_WORD, tmo=count_wait)
        screen = "\n".join(w.display())
    logs = [p for p in tmp_path.iterdir() if p.name.startswith("asan.log")]
    return "".join(p.read_text() for p in logs), screen


def row_after(screen, needle):
    """The screen row below the first one holding ``needle``."""
    rows = screen.split("\n")
    for i, row in enumerate(rows):
        if needle in row:
            return rows[i + 1]
    raise AssertionError("%r not on screen:\n%s" % (needle, screen))


# @scenario SC-error-pattern-01
def test_empty_message_string_does_not_overflow(tmp_path):
    """A language with no MessageString: F9 recognises no error rows."""
    text, screen = run_f9_with_pattern(tmp_path, "")
    assert SANITIZER not in text, text
    assert COMPILER_ROW in screen, screen
    assert NO_ROWS in screen, screen
    assert "MessageString of Lua not used: empty pattern" in screen, screen
    assert screen.count("MessageString of Lua") == 1, screen


# @scenario SC-error-pattern-02
def test_pattern_cut_inside_a_test_does_not_overflow(tmp_path):
    """A pattern that ends inside ${?n:text}: its row is still a string,
    and Messages says which part of the pattern was cut."""
    text, screen = run_f9_with_pattern(
        tmp_path, "MessageString : ${FILE}:${?5\n")
    assert SANITIZER not in text, text
    assert COMPILER_ROW in screen, screen
    assert "MessageString of Lua not used: unclosed ${? near" in screen, screen


# @scenario SC-error-pattern-04
def test_warning_test_without_its_end_is_refused_with_a_reason(tmp_path):
    """A pattern that ends inside ${?*:text} used to be read one byte past
    its end; F9 now ends with its count and the row that names the cut."""
    text, screen = run_f9_with_pattern(
        tmp_path, "MessageString : ${FILE}:${?*:warn\n",
        count_wait=COUNT_WAIT)
    assert SANITIZER not in text, text
    assert COMPILER_ROW in screen, screen
    assert "MessageString of Lua not used: unclosed ${? near" in screen, screen
    assert NO_ROWS in row_after(screen, "not used: unclosed"), screen


# @scenario SC-error-pattern-05
def test_pattern_ending_in_a_backslash_is_refused_with_a_reason(tmp_path):
    """A row of the pattern that ends in a backslash used to be read past
    its end.  The option file writes the backslash twice, which the option
    reader turns into one."""
    text, screen = run_f9_with_pattern(
        tmp_path, "MessageString : ${FILE}:${LINE}:x\\\\\n",
        count_wait=COUNT_WAIT)
    assert SANITIZER not in text, text
    assert COMPILER_ROW in screen, screen
    assert "MessageString of Lua not used: " in screen, screen
    assert NO_ROWS in screen, screen


# @scenario SC-error-pattern-08
def test_row_too_costly_for_the_pattern_is_named(tmp_path):
    """A row that costs the pattern more steps than the budget is left
    unrecognised and the build says which Messages row it was, once."""
    long_row = "a" * 400
    text, screen = run_f9_with_pattern(
        tmp_path, "MessageString : *a*a*a*a*b:${FILE}:${LINE}\n",
        output=(long_row,), count_wait=COUNT_WAIT)
    assert SANITIZER not in text, text
    assert "MessageString of Lua: pattern too costly for row" in screen, screen
    assert screen.count("MessageString of Lua") == 1, screen
    assert NO_ROWS in row_after(screen, "pattern too costly"), screen


# @scenario SC-error-pattern-09
def test_gnu_style_names_the_built_in_pattern(tmp_path):
    """With the GNU style the pattern in use is the built-in one, whatever
    MessageString the language holds, and the row says so and says how to
    use one's own."""
    text, screen = run_f9_with_pattern(
        tmp_path, "MessageString : ${FILE}:${LINE}:*\n",
        output=("-I/a:" * 12000,), count_wait=COUNT_WAIT,
        compiler_switch=0)
    assert SANITIZER not in text, text
    assert "the built-in GNU pattern (switch the style to Other to use " \
        "your own): pattern too costly for row" in screen, screen
    assert "MessageString of Lua" not in screen, screen
    assert COUNT_WORD in row_after(screen, "pattern too costly"), screen


# @scenario SC-error-pattern-10
def test_pattern_row_is_said_again_by_the_next_build(tmp_path):
    """Two builds in one session each end with the pattern row: the
    second build is a new build, not a repeat of the first."""
    text, screen = run_f9_with_pattern(
        tmp_path, "MessageString : ${FILE}:${?*:warn\n",
        count_wait=COUNT_WAIT, builds=2)
    assert SANITIZER not in text, text
    assert build_row_number(2) in screen, screen
    below = screen.split(build_row_number(2), 1)[1]
    assert "MessageString of Lua not used: unclosed ${? near" in below, screen


# @scenario SC-error-pattern-11
def test_unknown_variable_is_noted_and_still_read(tmp_path):
    """A pattern with an unknown variable is used, with the variable read
    as '*', and the build says so once."""
    text, screen = run_f9_with_pattern(
        tmp_path, "MessageString : ${FILE}:${LINE}: ${CMPTXT}\n",
        output=("t.lua:1: syntax error",), count_wait=COUNT_WAIT)
    assert SANITIZER not in text, text
    assert 'MessageString of Lua: unknown variable "CMPTXT" read as *' \
        in screen, screen
    assert screen.count("MessageString of Lua") == 1, screen
    assert ONE_ROW in row_after(screen, "read as *"), screen


# @scenario SC-error-pattern-03
def test_one_row_pattern_does_not_overflow(tmp_path):
    """A one-row pattern leaves rows 2 to 4 empty and they are never read."""
    text, screen = run_f9_with_pattern(
        tmp_path, "MessageString : ${FILE}:${LINE}:*\n")
    assert SANITIZER not in text, text
    assert ONE_ROW in screen, screen


# @scenario SC-error-pattern-06
def test_two_row_pattern_whose_first_row_matches_the_last_row_ends(tmp_path):
    """F9 ends with its count line when a two-row pattern's first row is
    the last row of Messages.  The message has no second row, so the walk
    used to give the row back past where it began and F9 stayed on
    "building ..." for good."""
    text, screen = run_f9_with_pattern(
        tmp_path, "MessageString : *${FILE}\\n${LINE}\n",
        count_wait=COUNT_WAIT)
    assert SANITIZER not in text, text
    assert COMPILER_ROW in screen, screen
    assert COUNT_WORD in screen, screen


# @scenario SC-error-pattern-07
def test_rows_that_end_in_a_backslash_stay_inside_messages(tmp_path):
    """A build whose last rows end in a backslash is read to its end: the
    rows that go on with a row stop at the last row of Messages, where the
    walk used to read past the buffer."""
    text, screen = run_f9_with_pattern(
        tmp_path, "MessageString : ${FILE}:${LINE}:*\n",
        output=("t.lua:1: x\\", "a\\", "b\\"), count_wait=COUNT_WAIT)
    assert SANITIZER not in text, text
    assert ONE_ROW in screen, screen
