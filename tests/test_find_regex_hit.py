"""A regular expression hit is the text the expression matched.

Search > Find with "Regular expression" on puts the cursor behind the text
that matched (in front of it searching backward), with or without a group in
the expression.  The hit used to run from where the search began to the end
of the row, so `a` on `abc` left the cursor at the end of the row, and `^`
and `$` never matched.  Search Again, Replace and Change All use the same hit.
"""
import pytest

pytest.importorskip("pyte")

from edit_model.oracle import END, ENTER, F2, RIGHT, EditSession, alt, ctrl  # noqa: E402

ALT_R = alt("r")     # the Regular expression option of the Find dialog
ALT_B = alt("b")     # Backward
ALT_N = alt("n")     # New Text of the Replace dialog
ALT_P = alt("p")     # Prompt on Replace
ALT_A = alt("a")     # Change All
ALT_C = alt("c")     # Case sensitive


def _find(s, pattern, *extra):
    return s.send([ctrl("o"), "f"] + list(pattern) + [ALT_R] + list(extra) + [ENTER]).win


# @scenario SC-edit-model-53
@pytest.mark.parametrize("seed,keys,pattern,cursor", [
    ("abc\n", [], "a", (0, 1)),
    ("abbc\n", [], "b+", (0, 3)),
    ("xabcd\n", [], "a(b)c", (0, 4)),
    ("abab\n", [RIGHT], "a", (0, 3)),
    ("abc\nxyz\n", [RIGHT], "^x", (1, 1)),
    ("abc\nxyz\n", [], "c$", (0, 3)),
    ("abab\n", [], "b*", (0, 0)),
], ids=["a", "b+", "group", "from-cursor", "bol", "eol", "empty-then-text"])
def test_forward_hit_is_what_matched(tmp_path, seed, keys, pattern, cursor):
    with EditSession(str(tmp_path), seed) as s:
        if keys:
            s.send(keys)
        w = _find(s, pattern)
        assert w.cursor == cursor, "cursor %s" % (w.cursor,)


# @scenario SC-edit-model-53
def test_an_empty_hit_is_a_place_and_search_again_goes_past_it(tmp_path):
    """`b*` on "abab" matches empty at the cursor: that place is the hit, and
    Search Again goes on to the `b` behind it."""
    with EditSession(str(tmp_path), "abab\n") as s:
        w = _find(s, "b*")
        assert w.cursor == (0, 0), "cursor %s" % (w.cursor,)
        w = s.send([ctrl("l")]).win
        assert w.cursor == (0, 2), "cursor %s" % (w.cursor,)


# @scenario SC-edit-model-53
def test_backward_hit_is_the_last_one_before_the_cursor(tmp_path):
    with EditSession(str(tmp_path), "abab\n") as s:
        s.send([END])
        w = _find(s, "b", ALT_B)
        assert w.cursor == (0, 3), "cursor %s" % (w.cursor,)


# @scenario SC-edit-model-53
def test_search_again_goes_on_to_the_next_regular_expression_hit(tmp_path):
    with EditSession(str(tmp_path), "abab\n") as s:
        w = _find(s, "a")
        assert w.cursor == (0, 1), "cursor %s" % (w.cursor,)
        w = s.send([ctrl("l")]).win
        assert w.cursor == (0, 3), "cursor %s" % (w.cursor,)


# @scenario SC-edit-model-53
def test_replace_with_a_regular_expression_changes_only_the_match(tmp_path):
    with EditSession(str(tmp_path), "abc\n") as s:
        s.send([alt("s"), "r", "b", ALT_R, ALT_N, "Z", ALT_P, ALT_A, ENTER, F2])
        assert s.disk() == b"aZc\n", s.disk()


# @scenario SC-edit-model-53
def test_change_all_with_a_regular_expression_changes_every_match(tmp_path):
    with EditSession(str(tmp_path), "abbc\nbb\n") as s:
        s.send([alt("s"), "r", "b", "+", ALT_R, ALT_N, "Z", ALT_P, ALT_A, ENTER, F2])
        assert s.disk() == b"aZc\nZ\n", s.disk()


# @scenario SC-edit-model-53
def test_case_sensitive_dollar_matches_at_the_end_of_the_row(tmp_path):
    """The row's newline is not text: `c$` with Case sensitive on finds the
    `c` that ends the row, as it does with the option off."""
    with EditSession(str(tmp_path), "abc\nxyz\n") as s:
        w = _find(s, "c$", ALT_C)
        assert w.cursor == (0, 3), "cursor %s" % (w.cursor,)


def _change_all(s, pattern, new, *extra):
    s.send([alt("s"), "r"] + list(pattern) + [ALT_R] + list(extra) + [ALT_N] + list(new)
           + [ALT_P, ALT_A, ENTER])
    s.send([F2])
    return s.disk()


# @scenario SC-edit-model-53
@pytest.mark.parametrize("seed,pattern,new,extra,disk", [
    ("abc\nxyz\n", "^", "", [], b"abc\nxyz\n"),
    ("abc\n", "x*", "", [], b"abc\n"),
    ("abc\n", "x*", "Z", [], b"ZaZbZcZ\n"),
    ("abc\nxyz\n", "$", "Z", [ALT_B], b"abcZ\nxyzZ\n"),
    ("ab\nab\n", "^a", "Z", [], b"Zb\nZb\n"),
], ids=["bol-to-nothing", "empty-to-nothing", "empty-to-text", "eol-backward",
        "anchored"])
def test_change_all_never_stands_still_on_an_empty_hit(tmp_path, seed, pattern, new, extra,
                                                      disk):
    """An expression that can match empty text: Change All ends (it used to
    run for ever), and each place it matches empty is written to once."""
    with EditSession(str(tmp_path), seed) as s:
        if ALT_B in extra:
            s.send(["\033[B", END])
        assert _change_all(s, pattern, new, *extra) == disk


# @scenario SC-edit-model-53
@pytest.mark.parametrize("pattern,extra,first,again", [
    ("^", [], (1, 0), (1, 0)),
    ("q*", [], (0, 1), (0, 2)),
    ("$", [ALT_B], (0, 1), (0, 1)),
], ids=["bol", "empty", "eol-backward"])
def test_search_again_never_stands_still_on_an_empty_hit(tmp_path, pattern, extra, first,
                                                        again):
    """Search Again after an expression that matches only empty text: the
    first search finds the place, Search Again goes to the next place, and
    where there is none the cursor stays where it was."""
    with EditSession(str(tmp_path), "abc\nxyz\n") as s:
        s.send([RIGHT])
        w = _find(s, pattern, *extra)
        assert w.cursor == first, "cursor %s" % (w.cursor,)
        w = s.send([ctrl("l")]).win
        assert w.cursor == again, "cursor %s" % (w.cursor,)
