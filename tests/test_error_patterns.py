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
import pytest

from wpe_driver import WpeSession

F9 = "\033[20~"
BUILD_WAIT = 5.0
SANITIZER = "AddressSanitizer"
COMPILER_ROW = "t.lua:1: syntax error near ="
NO_ROWS = "0 errors, 0 warnings"
ONE_ROW = "1 error, 0 warnings"


def run_f9_with_pattern(tmp_path, pattern_line):
    """F9 on a Lua file whose language has ``pattern_line`` as its pattern.

    Returns the text of every sanitizer log the editor wrote and the screen
    after the build.
    """
    fake = tmp_path / "fake-lua"
    fake.write_text("#!/bin/sh\necho 't.lua:1: syntax error near ='\nexit 1\n")
    fake.chmod(0o755)
    cfg = tmp_path / ".config" / "xwpe"
    cfg.mkdir(parents=True)
    (cfg / "xwperc").write_text(
        "[Language/Lua]\nFileExtension : .lua\nCompiler : %s\n"
        "CompilerOptions : -p\nCompilerSwitch : 1\nKey : L\nX : 20\n%s"
        % (fake, pattern_line))
    env = {"ASAN_OPTIONS": "detect_leaks=0:log_path=%s" % (tmp_path / "asan.log")}
    with WpeSession(str(tmp_path), "x = = 1\n", filename="t.lua", cols=200,
                    env_extra=env) as w:
        w.key(F9, delay=BUILD_WAIT)
        screen = "\n".join(w.display())
    logs = [p for p in tmp_path.iterdir() if p.name.startswith("asan.log")]
    return "".join(p.read_text() for p in logs), screen


# @scenario SC-error-pattern-01
def test_empty_message_string_does_not_overflow(tmp_path):
    """A language with no MessageString: F9 recognises no error rows."""
    text, screen = run_f9_with_pattern(tmp_path, "")
    assert SANITIZER not in text, text
    assert COMPILER_ROW in screen, screen
    assert NO_ROWS in screen, screen


# @scenario SC-error-pattern-02
def test_pattern_cut_inside_a_test_does_not_overflow(tmp_path):
    """A pattern that ends inside ${?n:text}: its row is still a string."""
    text, screen = run_f9_with_pattern(
        tmp_path, "MessageString : ${FILE}:${?5\n")
    assert SANITIZER not in text, text
    assert COMPILER_ROW in screen, screen


# @scenario SC-error-pattern-03
def test_one_row_pattern_does_not_overflow(tmp_path):
    """A one-row pattern leaves rows 2 to 4 empty and they are never read."""
    text, screen = run_f9_with_pattern(
        tmp_path, "MessageString : ${FILE}:${LINE}:*\n")
    assert SANITIZER not in text, text
    assert ONE_ROW in screen, screen
