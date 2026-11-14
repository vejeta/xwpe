"""An error pattern in an option file is read and saved exactly as written.

A language's MessageString holds line breaks and backslashes, so the option
file stores it escaped.  The reading side used to look past the end of the
value (a start-up read of a folder's ./xwperc, which a cloned project
carries, went over the line buffer), and the writing side used to write one
byte past its buffer for a value made only of escapes, the empty one
included, after the save had already cut the file to nothing: one empty
MessageString line and the next quit left a zero-byte option file.

Run under the sanitizer build with
WPE_BIN=$PWD/wpe-asan ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 (the
binary must carry a wpe-prefixed name).  The rows set ASAN_OPTIONS
themselves to write the report beside the test.  Every row also asserts on a
file on disk or on the editor still drawing, so a row cannot pass because
the editor never ran.
"""
import glob
import os

from wpe_driver import WpeSession

SEED = "int main(void){return 0;}\n"
SEED_ON_SCREEN = "int main"
SANITIZER = "AddressSanitizer"
ALT_X = "\033x"
ALT_O = "\033o"
ALT_M = "\033m"
END = "\033[F"
BACKSPACE = "\x7f"
FIELD_WIDTH = 128
HEADER = "[General]\nAutosave : 1\n"
LANGUAGE = "[Language/C]\nCompiler : gcc\n"
SAVED_LANGUAGE = "[Language/C]"
SAVED_COMPILER = "Compiler : gcc"
MESSAGE_KEY = "MessageString"
FILE_LINE = "${FILE}:${LINE}:"


def _start_folder_rc(tmp_path, rc_text):
    """Write ./xwperc for a session started in tmp_path; return its path and
    the environment that makes the sanitizer write its report beside it."""
    path = os.path.join(str(tmp_path), "xwperc")
    with open(path, "w", newline="") as fh:
        fh.write(rc_text)
    log = os.path.join(str(tmp_path), "asan")
    return path, {"ASAN_OPTIONS": "detect_leaks=0:abort_on_error=0:log_path=" + log}


def _sanitizer_report(tmp_path):
    """The text of every sanitizer log the editor wrote in tmp_path."""
    text = ""
    for name in glob.glob(os.path.join(str(tmp_path), "asan*")):
        with open(name) as fh:
            text += fh.read()
    return text


def _message_row(path):
    """The saved MessageString line of the [Language/C] section, without its
    line end (every language has one; the rows seed only C's)."""
    section = None
    with open(path, "rb") as fh:
        for row in fh.read().split(b"\n"):
            if row.startswith(b"["):
                section = row
            elif (section == SAVED_LANGUAGE.encode()
                  and row.startswith(MESSAGE_KEY.encode())):
                return row
    return None


def _run_and_quit(tmp_path, rc_text):
    """Start on t.c with ./xwperc = rc_text, quit with Alt-X (the Autosave
    writes the option file); return the file's path and the sanitizer text."""
    path, env = _start_folder_rc(tmp_path, rc_text)
    with WpeSession(str(tmp_path), SEED, env_extra=env) as w:
        w.key(ALT_X)
    return path, _sanitizer_report(tmp_path)


def _assert_file_kept(path, report):
    """The save did not cut the option file and the editor made no report."""
    assert SANITIZER not in report, report
    with open(path, "rb") as fh:
        saved = fh.read()
    assert saved, "the option file was left at 0 bytes"
    assert SAVED_LANGUAGE.encode() in saved, saved
    assert SAVED_COMPILER.encode() in saved, saved
    return saved


# @scenario SC-option-codec-02
def test_pattern_ending_in_a_backslash_is_saved_as_written(tmp_path):
    """A 400-byte option line before it must leave no bytes in the pattern."""
    rc = (HEADER + LANGUAGE + "CompilerOptions : " + "Z" * 400 + "\n"
          + MESSAGE_KEY + " : " + FILE_LINE + "\\n\\n\\")
    path, report = _run_and_quit(tmp_path, rc)
    assert SANITIZER not in report, report
    assert _message_row(path) == (MESSAGE_KEY + " : " + FILE_LINE + "\\n\\n\\\\").encode()


# @scenario SC-option-codec-02a
def test_long_pattern_with_line_breaks_is_read_in_the_folder_file(tmp_path):
    """A 254-byte MessageString with three line breaks, read at start."""
    value = FILE_LINE + "\\n\\n\\n"
    value += " " * (254 - len(MESSAGE_KEY + " : ") - len(value))
    path, env = _start_folder_rc(
        tmp_path, HEADER + LANGUAGE + MESSAGE_KEY + " : " + value + "\n")
    with WpeSession(str(tmp_path), SEED, env_extra=env) as w:
        assert w.alive()
        assert SEED_ON_SCREEN in "\n".join(w.display())
    assert SANITIZER not in _sanitizer_report(tmp_path)


# @scenario SC-option-codec-02b
def test_hand_written_carriage_return_is_read_and_saved_raw(tmp_path):
    """Backslash r in the file is a carriage return, saved as the byte."""
    rc = HEADER + LANGUAGE + MESSAGE_KEY + " : a\\rb\n"
    path, report = _run_and_quit(tmp_path, rc)
    assert SANITIZER not in report, report
    assert _message_row(path) == (MESSAGE_KEY + " : a\rb").encode()


# @scenario SC-option-codec-03
def test_empty_pattern_does_not_wipe_the_option_file(tmp_path):
    path, report = _run_and_quit(
        tmp_path, HEADER + LANGUAGE + MESSAGE_KEY + " :\n")
    _assert_file_kept(path, report)
    assert _message_row(path) in (b"MessageString :", b"MessageString : ")


# @scenario SC-option-codec-04
def test_all_escape_pattern_does_not_wipe_the_option_file(tmp_path):
    path, report = _run_and_quit(
        tmp_path, HEADER + LANGUAGE + MESSAGE_KEY + " : \\n\n")
    _assert_file_kept(path, report)
    assert _message_row(path) == (MESSAGE_KEY + " : \\n").encode()


# @scenario SC-option-codec-05
def test_compiler_dialog_opens_on_an_empty_pattern(tmp_path):
    path, env = _start_folder_rc(
        tmp_path, HEADER + LANGUAGE + MESSAGE_KEY + " :\n")
    with WpeSession(str(tmp_path), SEED, env_extra=env) as w:
        w.key(ALT_O)
        w.key("c")
        w.key("c")
        screen = "\n".join(w.display())
        assert w.alive()
        assert "Message" in screen, screen
    assert SANITIZER not in _sanitizer_report(tmp_path)


# @scenario SC-option-codec-05a
def test_typed_pattern_with_line_breaks_fills_the_field_and_is_taken(tmp_path):
    """A full 128-byte Message-String field ending in two line breaks."""
    path, env = _start_folder_rc(tmp_path, HEADER + LANGUAGE)
    with WpeSession(str(tmp_path), SEED, env_extra=env) as w:
        w.key(ALT_O)
        w.key("c")
        w.key("c")
        w.key(ALT_M)
        w.key(END)
        w.key(*([BACKSPACE] * FIELD_WIDTH), delay=0.02)
        w.key(*(["a"] * (FIELD_WIDTH - 4)), delay=0.02)
        w.key("\\", "n", "\\", "n", delay=0.1)
        w.key("\r", delay=1.5)
        screen = "\n".join(w.display())
        assert w.alive()
        assert SEED_ON_SCREEN in screen, screen
    assert SANITIZER not in _sanitizer_report(tmp_path)
