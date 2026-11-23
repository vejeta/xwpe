"""F9 puts the cursor where a compiler's quoted text puts it, never off the row.

Some compilers do not give a column but quote the text the error is at.  A
language set up with ${COLUMN=AFTER} in its MessageString has the cursor land
by that text on the source row.  When the row does not hold the text the
search looks for, the search finds nothing and the cursor lands at the start
of the row plus the length of the quote, never somewhere the row does not
have.

Each row sets up a Lua language in the option file with a fake compiler that
prints one error row, presses F9, and reads the column the status row shows.

Run: WPE_BIN=$PWD/wpe tests/.venv/bin/python -m pytest -q tests/test_error_search_text.py
"""
import re

from wpe_driver import WpeSession, wait_screen

F9 = "\033[20~"
BUILD_WAIT = 5.0
COUNT_WORD = " warnings"
COUNT_WAIT = 25.0
SOURCE = "x = = 1\n"
STATUS_RE = re.compile(r"^m+q[A-Z][* ]q\s+(\d+):(\d+)\s+q")
PATTERN = "MessageString : ${FILE}:${LINE}:* near ${COLUMN=AFTER}\n"


def _status_position(session):
    """(line, column) the status row of the active window shows."""
    for row in session.display():
        found = STATUS_RE.match(row)
        if found:
            return int(found.group(1)), int(found.group(2))
    raise AssertionError("no status row on screen:\n" + "\n".join(session.display()))


def _column_after_f9(tmp_path, quoted):
    """The line and column the cursor is on after F9, for a compiler that
    reports an error on line 1 "near" @quoted."""
    fake = tmp_path / "fake-lua"
    fake.write_text("#!/bin/sh\nprintf '%%s\\n' 't.lua:1: syntax error near %s'\nexit 1\n" % quoted)
    fake.chmod(0o755)
    cfg = tmp_path / ".config" / "xwpe"
    cfg.mkdir(parents=True)
    (cfg / "xwperc").write_text(
        "[Language/Lua]\nFileExtension : .lua\nCompiler : %s\n"
        "CompilerOptions : -p\nCompilerSwitch : 1\nKey : L\nX : 20\n%s" % (fake, PATTERN))
    with WpeSession(str(tmp_path), SOURCE, filename="t.lua", cols=200) as w:
        w.key(F9, delay=BUILD_WAIT)
        assert wait_screen(w, COUNT_WORD, tmo=COUNT_WAIT), "\n".join(w.display())
        return _status_position(w)


# @scenario SC-row-readers-11
def test_a_quote_the_row_does_not_hold_lands_from_the_start_of_the_row(tmp_path):
    # The quote as the compiler printed it ("zzzzz" and the end of its line,
    # six bytes) is not on the source row: the search finds nothing and the
    # cursor goes six bytes in from the start of the row, column 7.
    assert _column_after_f9(tmp_path, "zzzzz") == (1, 7)


# @scenario SC-row-readers-12
def test_a_short_quote_the_row_does_not_hold_lands_from_the_start_too(tmp_path):
    # "=" is on the row, but the quote the search looks for ends with the
    # compiler's line end, which the row does not hold: two bytes in, column 3.
    assert _column_after_f9(tmp_path, "=") == (1, 3)
