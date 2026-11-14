"""One owner finds an occurrence on a row: whole characters, inside the scope,
on the line's own rows, and a zero-width hit is a position found once.

Find, Search Again, Replace, Change All and Grep ask one owner (we_rowfind.c)
where the Find text occurs on a row.  With or without "Regular expression",
the same rules hold: "Case sensitive" off is the engine's own folding (so
`\\w` stays `\\w`), a block is a range a hit must lie in (a `\\b` at its edge
sees the text beside it), `^` and `$` are the ends of a line and not of a row
the line wrapped into, a word includes `_` and the letters beyond ASCII, a
hit is whole characters in every locale, an empty Find text finds nothing, a
Find text that is no expression says why, and an expression that matches empty
text (`^`, `$`, `x*`) is a position: the cursor goes there, Replace writes
there, and no place is found twice.
"""
import time

import pytest

pytest.importorskip("pyte")

from edit_model.oracle import (DOWN, END, ENTER, F2, RIGHT, SHIFT_RIGHT, UP,  # noqa: E402
                               EditSession, alt, ctrl)
from wpe_driver import ALT, WpeSession  # noqa: E402

ALT_R = alt("r")     # Regular expression
ALT_B = alt("b")     # Backward
ALT_N = alt("n")     # New Text of the Replace dialog
ALT_P = alt("p")     # Prompt on Replace
ALT_A = alt("a")     # Change All
ALT_C = alt("c")     # Case sensitive
ALT_W = alt("w")     # Whole words only
ALT_S = alt("s")     # Selected Text
ALT_E = alt("e")     # Entire Scope
NOT_FOUND = "NOT found"
NARROW_RC = "[General]\nMaxColumn : 40\n"
EIGHT_BIT = "en_US.iso885915"


def _find(s, text, *extra):
    """Search > Find for @text with the dialog's @extra keys."""
    return s.send([ctrl("o"), "f"] + list(text) + list(extra) + [ENTER]).win


def _change_all(s, text, new, *extra, prompt=False):
    """Search > Replace, Change All of @text by @new; the file as saved."""
    keys = [alt("s"), "r"] + list(text) + list(extra) + [ALT_N] + list(new)
    if not prompt:
        keys.append(ALT_P)
    s.send(keys + [ALT_A])
    if prompt:
        while "Replace this occurrence" in "\n".join(s.display()):
            s.send(["Y"])
    s.send([ENTER, F2])
    return s.disk()


def _screen(s):
    return "\n".join(s.display())


# ---- what the engine sees: the whole row, the case of the text ----

# @scenario SC-edit-model-56
def test_w_survives_a_case_insensitive_search(tmp_path):
    """`\\w+` with Case sensitive off is still a word: it used to turn into
    `\\W+` and find the space."""
    with EditSession(str(tmp_path), "ab cd\n") as s:
        w = _find(s, "\\w+", ALT_R)
        assert w.cursor == (0, 2), "cursor %s" % (w.cursor,)


# @scenario SC-edit-model-56
def test_a_word_boundary_sees_the_text_before_where_the_search_starts(tmp_path):
    """The cursor is in the middle of "xab": `\\ba` has no word start at it."""
    with EditSession(str(tmp_path), "xab\n") as s:
        s.send([RIGHT])
        w = _find(s, "\\ba", ALT_R, ALT_C)
        assert w.cursor == (0, 1), "cursor %s" % (w.cursor,)
        assert NOT_FOUND in _screen(s)


# @scenario SC-edit-model-56
@pytest.mark.parametrize("pattern,cursor", [("b\\b", (0, 0)), ("b", (0, 2)), ("c", (0, 0))],
                         ids=["boundary-past-the-block", "control-in", "control-out"])
def test_the_block_is_a_range_not_a_cut_of_the_row(tmp_path, pattern, cursor):
    """The block holds "ab" of "abc": `b\\b` does not match at its end (the
    `c` is beside it); `b` is in the block, `c` is outside."""
    with EditSession(str(tmp_path), "abc\n") as s:
        s.send([SHIFT_RIGHT, SHIFT_RIGHT])
        w = _find(s, pattern, ALT_R, ALT_C, ALT_S, ALT_E)
        assert w.cursor == cursor, "cursor %s" % (w.cursor,)


# @scenario SC-edit-model-56
@pytest.mark.parametrize("plain", [False, True], ids=["regular", "plain"])
def test_a_case_insensitive_search_folds_letters_beyond_ascii(tmp_path, plain):
    with EditSession(str(tmp_path), "xÉ\n") as s:
        extra = [] if plain else [ALT_R]
        w = _find(s, "é", *extra)
        assert w.cursor == (0, 3), "cursor %s" % (w.cursor,)


# ---- zero-width hits are positions ----

# @scenario SC-edit-model-56
def test_a_line_start_is_a_place_to_find(tmp_path):
    with EditSession(str(tmp_path), "abc\nxyz\n") as s:
        s.send([RIGHT])
        w = _find(s, "^", ALT_R)
        assert w.cursor == (1, 0), "cursor %s" % (w.cursor,)


# @scenario SC-edit-model-56
def test_search_again_goes_from_line_start_to_line_start(tmp_path):
    with EditSession(str(tmp_path), "abc\nxyz\nqrs\n") as s:
        s.send([RIGHT])
        w = _find(s, "^", ALT_R)
        assert w.cursor == (1, 0), "cursor %s" % (w.cursor,)
        w = s.send([ctrl("l")]).win
        assert w.cursor == (2, 0), "cursor %s" % (w.cursor,)


# @scenario SC-edit-model-56
def test_an_empty_hit_is_found_once_per_place_and_the_cursor_may_come_back(tmp_path):
    """Find `^` at the start of the text finds it there; Search Again moves
    on; back at that place, Search Again finds it there once more."""
    with EditSession(str(tmp_path), "abc\nxyz\n") as s:
        w = _find(s, "^", ALT_R)
        assert w.cursor == (0, 0), "cursor %s" % (w.cursor,)
        steps = [(ctrl("l"), (1, 0)), (UP, (0, 0)), (ctrl("l"), (0, 0)), (ctrl("l"), (1, 0))]
        for key, cursor in steps:
            w = s.send([key]).win
            assert w.cursor == cursor, "after %r: cursor %s" % (key, w.cursor)


# @scenario SC-edit-model-56
def test_a_cursor_that_left_an_empty_hit_finds_it_again_when_it_comes_back(tmp_path):
    """Find `^` stays at the start of the text.  The user moves down and
    back up, and Search Again finds the line start there once more, as a
    first search would, not the next one."""
    with EditSession(str(tmp_path), "abc\nxyz\n") as s:
        w = _find(s, "^", ALT_R)
        assert w.cursor == (0, 0), "cursor %s" % (w.cursor,)
        s.send([DOWN])
        s.send([UP])
        w = s.send([ctrl("l")]).win
        assert w.cursor == (0, 0), "cursor %s" % (w.cursor,)


# @scenario SC-edit-model-56
def test_a_find_confirmed_again_in_its_dialog_starts_afresh(tmp_path):
    """Find `^` stays at the start of the text.  The Find dialog opened again
    and confirmed with the same text is a new search, so it finds that place
    again; only Search Again goes on to the next line start."""
    with EditSession(str(tmp_path), "abc\nxyz\n") as s:
        w = _find(s, "^", ALT_R)
        assert w.cursor == (0, 0), "cursor %s" % (w.cursor,)
        w = s.send([ctrl("o"), "f", ENTER]).win
        assert w.cursor == (0, 0), "cursor %s" % (w.cursor,)
        w = s.send([ctrl("l")]).win
        assert w.cursor == (1, 0), "cursor %s" % (w.cursor,)


# @scenario SC-edit-model-56
def test_a_change_all_after_a_find_starts_at_the_cursor(tmp_path):
    """Find `^` stays at the start of the text; a Change All from the
    Replace dialog is a new search and writes at that line start too.  The
    dialog keeps "Regular expression" checked from the Find."""
    with EditSession(str(tmp_path), "ab\ncd\n") as s:
        w = _find(s, "^", ALT_R)
        assert w.cursor == (0, 0), "cursor %s" % (w.cursor,)
        assert _change_all(s, "^", "// ") == b"// ab\n// cd\n"


# @scenario SC-edit-model-56
@pytest.mark.parametrize("seed,pattern,new,disk", [
    ("ab\ncd\n", "^", "// ", b"// ab\n// cd\n"),
    ("ab\ncd\n", "$", "Z", b"abZ\ncdZ\n"),
    ("ab\ncd\n", "x*", "Z", b"ZaZbZ\nZcZdZ\n"),
    ("ab\n\ncd\n", "^", "Z", b"Zab\nZ\nZcd\n"),
    ("ab\n\ncd\n", "$", "Z", b"abZ\nZ\ncdZ\n"),
], ids=["bol", "eol", "empty", "bol-with-an-empty-row", "eol-with-an-empty-row"])
def test_change_all_writes_at_every_place_an_expression_matches_empty(tmp_path, seed, pattern,
                                                                     new, disk):
    with EditSession(str(tmp_path), seed) as s:
        assert _change_all(s, pattern, new, ALT_R) == disk


# @scenario SC-edit-model-56
@pytest.mark.parametrize("pattern,disk", [("^a", b"ab\n"), ("^.", b"ab\n")],
                         ids=["anchored-letter", "anchored-dot"])
def test_change_all_takes_a_line_start_once(tmp_path, pattern, disk):
    """`^a` by nothing on "aab" takes the first `a` only, as sed and Vim do:
    the line starts once, even when the replacement leaves the cursor at
    column 0."""
    with EditSession(str(tmp_path), "aab\n") as s:
        assert _change_all(s, pattern, "", ALT_R) == disk


# @scenario SC-edit-model-56
def test_change_all_backward_writes_at_every_line_start(tmp_path):
    with EditSession(str(tmp_path), "ab\ncd\n") as s:
        s.send([DOWN, END])
        assert _change_all(s, "^", "Z", ALT_R, ALT_B) == b"Zab\nZcd\n"


# @scenario SC-edit-model-56
@pytest.mark.parametrize("seed,pattern", [("ab\ncd\n", "^"), ("ab\ncd", "$")],
                         ids=["bol", "eol-of-a-last-row-without-a-line-break"])
def test_an_empty_hit_replaced_by_nothing_changes_nothing(tmp_path, seed, pattern):
    """No text is touched, so the file is not modified and has no undo."""
    with EditSession(str(tmp_path), seed) as s:
        before = s.last.win
        s.send([alt("s"), "r"] + list(pattern) + [ALT_R, ALT_N, ALT_P, ALT_A, ENTER])
        w = s.last.win
        assert w.lines == before.lines, w.lines
        assert (w.save, w.ud) == (before.save, before.ud), (w.save, w.ud)


# @scenario SC-edit-model-56
@pytest.mark.parametrize("prompt", [False, True], ids=["whole", "with-prompt"])
def test_one_undo_takes_back_a_change_all_of_an_empty_hit(tmp_path, prompt):
    with EditSession(str(tmp_path), "ab\ncd\n") as s:
        assert _change_all(s, "^", "// ", ALT_R, prompt=prompt) == b"// ab\n// cd\n"
        w = s.send([ctrl("u")]).win
        assert w.lines == [("H", b"ab"), ("H", b"cd")], w.lines


# @scenario SC-edit-model-56
def test_change_all_reads_the_text_it_has_made(tmp_path):
    """Change All looks on the rows as the replacements leave them, as
    Replace does one at a time: after the first `a` is gone the next one
    starts the word, so `\\<a` takes all three."""
    with EditSession(str(tmp_path), "aaa b\n") as s:
        assert _change_all(s, "\\<a", "", ALT_R, ALT_C) == b" b\n"


# ---- a row is not a line ----

WRAPPED = "a " + "y" * 39 + " b\n"


# @scenario SC-edit-model-56
@pytest.mark.parametrize("pattern,cursor", [
    ("^y", (0, 0)), ("y $", (0, 0)), ("^a", (0, 1)), ("b$", (2, 1)),
], ids=["no-start-at-a-wrap", "no-end-at-a-wrap", "control-start", "control-end"])
def test_a_wrapped_row_has_no_line_start_or_end_inside_the_line(tmp_path, pattern, cursor):
    with EditSession(str(tmp_path), WRAPPED, xwperc=NARROW_RC) as s:
        assert [e for e, _ in s.last.win.lines] == ["S", "S", "H"], s.last.win.lines
        w = _find(s, pattern, ALT_R)
        assert w.cursor == cursor, "cursor %s" % (w.cursor,)


def _each_line(lines, make):
    return "".join(make(line) + "\n" for line in lines).encode()


# @scenario SC-edit-model-56
@pytest.mark.parametrize("pattern,new,make", [
    ("^", "// ", lambda line: "// " + line),
    ("$", "Z", lambda line: line + "Z"),
    ("x*", "Z", lambda line: "Z" + "Z".join(line) + "Z"),
], ids=["bol", "eol", "empty"])
def test_change_all_on_wrapped_lines_writes_at_the_line_ends_only(tmp_path, pattern, new, make):
    lines = [WRAPPED.rstrip("\n"), "cd"]
    with EditSession(str(tmp_path), "\n".join(lines) + "\n", xwperc=NARROW_RC) as s:
        assert _change_all(s, pattern, new, ALT_R) == _each_line(lines, make)


# @scenario SC-edit-model-56
def test_change_all_of_a_wrap_width_of_text_goes_in_once_per_line(tmp_path):
    lines = [WRAPPED.rstrip("\n"), "cd"]
    new = "Q" * 40
    with EditSession(str(tmp_path), "\n".join(lines) + "\n", xwperc=NARROW_RC) as s:
        assert _change_all(s, "^", new, ALT_R) == _each_line(lines, lambda line: new + line)


# ---- a word, a character, an empty text, a bad expression ----

# @scenario SC-edit-model-56
@pytest.mark.parametrize("seed,text,cursor", [
    ("a_foo foo\n", "foo", (0, 9)),
    ("café\n", "caf", (0, 0)),
    ("caf é\n", "caf", (0, 3)),
], ids=["underscore", "accented-letter-after", "accented-letter-apart"])
def test_whole_words_takes_underscore_and_letters_beyond_ascii_as_word_letters(
        tmp_path, seed, text, cursor):
    with EditSession(str(tmp_path), seed) as s:
        w = _find(s, text, ALT_W)
        assert w.cursor == cursor, "cursor %s" % (w.cursor,)


# @scenario SC-edit-model-56
@pytest.mark.parametrize("locale", ["C", EIGHT_BIT, "en_US.UTF-8"])
def test_a_dot_takes_a_whole_character_in_every_locale(tmp_path, locale):
    with EditSession(str(tmp_path), "éa\n", env_extra={"LC_ALL": locale}) as s:
        assert _change_all(s, ".", "Z", ALT_R, ALT_C) == b"ZZ\n"


# @scenario SC-edit-model-56
@pytest.mark.parametrize("locale", ["C", EIGHT_BIT, "en_US.UTF-8"])
def test_a_set_never_matches_a_part_of_another_character(tmp_path, locale):
    """`[é]` is the two bytes C3 A9; the bytes of "Ã" are C3 83.  No
    byte of one character is a hit on the other."""
    with EditSession(str(tmp_path), "Ã\n", env_extra={"LC_ALL": locale}) as s:
        assert _change_all(s, "[é]", "Z", ALT_R, ALT_C) == b"\xc3\x83\n"


# @scenario SC-edit-model-56
@pytest.mark.parametrize("locale", ["C", EIGHT_BIT])
def test_case_folding_does_not_read_utf8_bytes_as_latin1(tmp_path, locale):
    """U+3A40 is E3 A9 80: its middle byte is the second byte of `é`, and
    a Latin-1 fold of the first byte makes the two look alike."""
    with EditSession(str(tmp_path), "㩀\n", env_extra={"LC_ALL": locale}) as s:
        w = _find(s, "é")
        assert w.cursor == (0, 0), "cursor %s" % (w.cursor,)
        assert NOT_FOUND in _screen(s)


# @scenario SC-edit-model-56
def test_a_latin1_byte_in_a_row_does_not_hide_the_text_beside_it(tmp_path):
    with EditSession(str(tmp_path), b"caf\xe9 a\n") as s:
        w = _find(s, "a")
        assert w.cursor == (0, 2), "cursor %s" % (w.cursor,)


# @scenario SC-edit-model-56
def test_a_latin1_byte_taken_into_the_find_text_is_found(tmp_path):
    """The byte is selected and Find takes it as its text."""
    with EditSession(str(tmp_path), b"caf\xe9 a\n") as s:
        s.send([RIGHT, RIGHT, RIGHT, SHIFT_RIGHT])
        w = s.send([ctrl("o"), "f", ALT_E, ENTER]).win
        assert w.cursor == (0, 4), "cursor %s" % (w.cursor,)


# @scenario SC-edit-model-56
def test_an_empty_find_text_finds_nothing(tmp_path):
    with EditSession(str(tmp_path), "abc\n") as s:
        s.send([RIGHT])
        w = s.send([ctrl("o"), "f", ENTER]).win
        assert w.cursor == (0, 1), "cursor %s" % (w.cursor,)
        assert NOT_FOUND in _screen(s)


# @scenario SC-edit-model-56
def test_change_all_of_an_empty_find_text_ends(tmp_path):
    """It used to run for ever: the editor stopped answering."""
    with EditSession(str(tmp_path), "abc\n") as s:
        s.send([alt("s"), "r", ALT_N, "Z", ALT_P, ALT_A])
        assert NOT_FOUND in _screen(s)
        s.send([ENTER, F2])
        assert s.alive()
        assert s.disk() == b"abc\n"


# @scenario SC-edit-model-56
def test_a_text_that_is_no_expression_says_why(tmp_path):
    with EditSession(str(tmp_path), "abc\n") as s:
        _find(s, "(", ALT_R)
        screen = _screen(s)
        assert "Message" in screen and NOT_FOUND not in screen, screen


# ---- Grep ----

NO_ENTRY = "No entry in this list"


def _grep(tmp_path, seed, text, *opts):
    """File > Grep for @text with the dialog's @opts; the screen after Enter
    on the list, and whether the list held the file (Enter on it opens it,
    on nothing says so)."""
    with WpeSession(str(tmp_path), seed, filename="t.c", wait=2.0) as w:
        w.key(ALT.FILE, "g", delay=1.2)
        w.key(text, delay=0.8)
        for o in opts:
            w.key(o, delay=0.3)
        w.key("\r", delay=1.5)
        w.key("\r", delay=1.5)
        screen = "\n".join(w.display())
        assert w.alive(), "Grep ended the editor"
        return screen, NO_ENTRY not in screen


# @scenario SC-edit-model-56
@pytest.mark.parametrize("seed,text,opts,listed", [
    ("abcd\n", "\\w+", ["\033r"], True),
    ("abcd\n", "", [], False),
    ("foox foo\n", "foo", ["\033w"], True),
    ("foox foo\n", "zzq", [], False),
], ids=["case-insensitive-w", "empty-text", "later-whole-word", "control-absent"])
def test_grep_lists_a_file_by_the_rules_of_find(tmp_path, seed, text, opts, listed):
    screen, found = _grep(tmp_path, seed, text, *opts)
    assert found == listed, screen


# @scenario SC-edit-model-56
def test_grep_of_a_text_that_is_no_expression_says_why(tmp_path):
    with WpeSession(str(tmp_path), "ab\n", filename="t.c", wait=2.0) as w:
        w.key(ALT.FILE, "g", delay=1.2)
        w.key("(", delay=0.8)
        w.key("\033r", delay=0.3)
        w.key("\r", delay=1.5)
        screen = "\n".join(w.display())
        assert "Message" in screen, screen


# ---- speed ----

# @scenario SC-edit-model-56
def test_change_all_over_a_hundred_thousand_rows_ends_soon(tmp_path):
    """The search is a few percent of the command: Change All "foo" by "bar"
    over 100000 rows of 80 bytes takes seconds, not minutes."""
    rows = ["x" * 36 + " foo " + "y" * 39 for _ in range(100000)]
    seed = "\n".join(rows) + "\n"
    with EditSession(str(tmp_path), seed) as s:
        start = time.monotonic()
        s.send([alt("s"), "r", "f", "o", "o", ALT_N, "b", "a", "r", ALT_P, ALT_A], timeout=120)
        elapsed = time.monotonic() - start
        s.send([ENTER, F2], timeout=120)
        assert s.disk().count(b"bar") == 100000
        assert elapsed < 60, "Change All took %.1f s" % elapsed
