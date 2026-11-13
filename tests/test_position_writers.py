"""Positions that come from outside the editor's own keys stand on characters.

A compiler names a column, a click names a screen cell, a bracket match and a
reformatter name a byte, and each of them used to write the cursor straight
into the window.  On a row with an accented letter that put the cursor between
the two bytes of the letter, and the next key (a typed letter, Delete) cut it
in two.  Every one of these writers now goes through the buffer's owner, which
brings the position back onto a character.

Each row names the writer and the row of text it was given, then types a
letter at the cursor and reads the file back: a cursor inside a character
leaves bytes that are no UTF-8.
"""
import os
import select
import shutil
import subprocess
import time

import pytest

pytest.importorskip("pyte")

from edit_model import selfcheck  # noqa: E402
from edit_model.oracle import DOWN, HOME, RIGHT, EditSession, alt, ctrl  # noqa: E402
from wpe_driver import ALT, WPE_BIN, WpeSession, wait_screen  # noqa: E402

F9 = "\033[20~"
F6 = "\033[17~"           # next window: Messages, after a build
WIDE = 300                # wide enough for gcc's row after a long temporary path
NOTHING = "\x00"        # Ctrl-Space: a key that changes nothing, to get a dump
CO = ctrl("o")
POSITION_TRACES = ("position off a glyph", "position inside")


def _on_glyph(win, y, x):
    return 0 <= y < len(win.lines) and selfcheck._on_boundary(win.lines[y][1], x)


def _assert_no_position_trace(s):
    bad = [t for t in s.trace_since_last() if any(p in t for p in POSITION_TRACES)]
    assert bad == [], "the editor traced a position off a glyph: %s" % bad


def _need_gcc():
    if shutil.which("gcc") is None:
        pytest.skip("no gcc")


# The line a compiler blames, as gcc counts it: the third `é` ends at screen
# column 19, so the token that follows is at column 21 and byte 23.
STRING_THEN_TOKEN_C = ("int main(void)\n"
                       "{\n"
                       "    char *s = \"ééé\" undef_q;\n"
                       "    return 0;\n"
                       "}\n")


# @scenario SC-edit-model-53
def test_the_compiler_column_after_accented_letters_lands_on_the_token(tmp_path):
    """gcc counts a screen column, so on a row with three `é` before the
    blamed token the column is three less than the byte.  F9 puts the cursor on
    the token; a letter typed there goes before it and the file stays UTF-8."""
    _need_gcc()
    with WpeSession(str(tmp_path), STRING_THEN_TOKEN_C, filename="t.c", cols=WIDE,
                    rows=60) as w:
        w.key(F9, delay=4.0)
        assert wait_screen(w, "1 error", tmo=20), "\n".join(w.display())
        w.key("Z", delay=0.5)
        w.save()
        with open(w.path, "rb") as fh:
            data = fh.read()
    text = data.decode("utf-8")
    assert text.splitlines()[2] == '    char *s = "ééé" Zundef_q;', text


def _messages_row_of(s, row_mark, needle):
    """(screen row, screen column), 1-based, of the last `needle` on the first
    screen row that shows `row_mark`."""
    for i, row in enumerate(s.display()):
        if row_mark in row and needle in row:
            return i + 1, row.rfind(needle) + 1
    raise AssertionError("no %r on screen:\n%s" % (row_mark, "\n".join(s.display())))


def _click(s, col, row):
    """A left press and release at the 1-based screen cell (col, row), and the
    dump the editor writes once it has read both.  A mouse report is not
    counted as the keys it is made of, so the session takes the editor's own
    count after it."""
    for tail in ("M", "m"):
        os.write(s.master_fd, ("\x1b[<0;%d;%d%s" % (col, row, tail)).encode())
    time.sleep(1.5)
    while True:
        r, _, _ = select.select([s.dump_fd], [], [], 0.5)
        if not r:
            break
        s.reader.feed(os.read(s.dump_fd, 65536))
    if s.reader.dumps:
        s.keys_sent = s.reader.dumps[-1].seq
    s.reader.dumps = []
    return s.send([NOTHING])


# @scenario SC-edit-model-53
def test_a_click_in_messages_after_quotes_lands_on_the_character_clicked(tmp_path):
    """gcc's rows in Messages carry typographic quotes of three bytes each.  A
    click on the `u` of `undef_q` puts the Messages cursor on that letter, not
    between the bytes of a quote before it."""
    _need_gcc()
    with EditSession(str(tmp_path), STRING_THEN_TOKEN_C, cols=WIDE, rows=60) as s:
        s.send([F9])
        time.sleep(5.0)
        s.send([F6])
        row, col = _messages_row_of(s, "error: expected", "undef_q")
        d = _click(s, col, row)
        messages = [w for w in d.windows.values() if w.name == "Messages"]
        assert messages, "no Messages window: " + d.raw[:300]
        m = messages[0]
        assert m.pos == -1, "Messages cursor off a place: %r" % (m.cursor,)
        line = m.lines[m.y][1]
        assert line[m.x:m.x + 7] == b"undef_q", "cursor %r on %r" % (m.cursor, line)
        _assert_no_position_trace(s)


# @scenario SC-edit-model-53
def test_bracket_match_over_accented_letters_lands_on_the_bracket(tmp_path):
    """Ctrl-O K from a bracket goes to its partner; the partner of the `(` on
    a row that starts with accented letters is the `)` byte, a whole place."""
    with EditSession(str(tmp_path), "éé(éé) x\n") as s:
        s.send([RIGHT, RIGHT])
        w = s.send([CO, "k"]).win
        assert w.cursor == (0, 9), "cursor %r" % (w.cursor,)
        assert _on_glyph(w, w.y, w.x)
        _assert_no_position_trace(s)


BEAUTIFY_C = "int main(void)\n{\nx = 1; /* ééé */\n}\n"


# @scenario SC-edit-model-53
def test_the_reformatter_puts_the_cursor_back_on_a_character(tmp_path):
    """The reformatter indents `x = 1;` by three bytes, so the byte column the
    cursor had falls inside an `é` of the comment.  The cursor goes back on the
    letter's first byte, and the editor never held it between the two."""
    with EditSession(str(tmp_path), BEAUTIFY_C) as s:
        s.send([DOWN, DOWN, HOME] + [RIGHT] * 12)
        before = s.send([NOTHING]).win
        assert before.cursor == (2, 14), "cursor %r" % (before.cursor,)
        s.trace_since_last()
        w = s.send([CO, "b", alt("o")]).win
        assert w.lines[2][1].startswith(b"   x = 1;"), w.lines[2]
        assert (w.cursor, w.pos) == ((2, 13), -1), "cursor %r pos %d" % (w.cursor, w.pos)
        assert _on_glyph(w, w.y, w.x)
        _assert_no_position_trace(s)


AI_ORIG = "éé\nabc\n"
AI_REPLY = "€x\nabc\n"


def _ai_built():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


# @scenario SC-edit-model-53
@pytest.mark.skipif(not _ai_built(), reason="wpe built without --enable-ai")
def test_an_assistant_edit_that_covers_the_cursor_leaves_it_on_a_character(tmp_path):
    """The cursor stands after the first `é` (byte 2) when the assistant's
    reply makes bytes 0 to 2 one `€`: a letter typed next goes before the `€`,
    not inside it."""
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_MOCK_REPLY": AI_REPLY,
           "XWPE_AI_TRACE": str(tmp_path / "e.trace")}
    with WpeSession(str(tmp_path), AI_ORIG, env_extra=env, filename="t.txt") as s:
        s._drain(0.6)
        s.key("\033[C", delay=0.3)
        s.key(ALT.AI)
        s.key("e")
        s._drain(0.5)
        s.key("change the first line")
        s.key("\r", delay=0.8)
        assert wait_screen(s, "Proposed change", tmo=12), "\n".join(s.display())
        s.key("y", delay=0.6)
        s._drain(0.8)
        s.key("Z", delay=0.4)
        s.save()
        with open(s.path, "rb") as fh:
            data = fh.read()
    assert data.decode("utf-8") == "Z€x\nabc\n", data
