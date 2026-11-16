"""Replace, Change All and Grep on a Latin-1 file under a UTF-8 locale.

A byte that is no UTF-8 is one character of its own to the search owner
(we_rowfind.c), on every system: `.` takes it, a bracket takes it, and a
replacement writes exactly the bytes of the hit and no others.  The rows
assert on the file on disk, never on the screen: a hit that covered half a
character or skipped a stray byte shows as wrong bytes in the saved file.

An overlong form or a surrogate is one such character too, and the same one,
only where the system's engine reads UTF-8 (configure: WPE_HAS_REGEX_MULTIBYTE);
the row that needs it skips by name where the engine reads bytes.
"""
import pytest

pytest.importorskip("pyte")

from edit_model.oracle import EditSession, F2, alt  # noqa: E402
from wpe_driver import ALT, WpeSession  # noqa: E402
import procinfo  # noqa: E402

ALT_R = alt("r")     # Regular expression
ALT_B = alt("b")     # Backward
ALT_N = alt("n")     # New Text of the Replace dialog
ALT_P = alt("p")     # Prompt on Replace
ALT_A = alt("a")     # Change All
ALT_C = alt("c")     # Case sensitive
ALT_W = alt("w")     # Whole words only
ENTER = "\r"
NO_ENTRY = "No entry in this list"

needs_utf8_engine = pytest.mark.skipif(
    procinfo.build_has("WPE_HAS_REGEX_MULTIBYTE") is False,
    reason="this system's regex engine reads bytes (configure: 'regex reads "
           "UTF-8: no'), so a bad-scalar sequence is not one character here")


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


# ---- Replace and Change All write exactly the hit's bytes ----

# @scenario SC-edit-model-56
@pytest.mark.parametrize("seed,text,opts,want", [
    (b"ay\xe9z\n", "y.", [ALT_R], b"aXz\n"),
    (b"a\xe9zb\n", ".z", [ALT_R, ALT_B], b"aXb\n"),
    (b"a\xe9b\n", "a.b", [ALT_R], b"X\n"),
    (b"a\xe9a\n", "[^a]", [ALT_R], b"aXa\n"),
    (b"\xe9abc\n", "^.", [ALT_R], b"Xabc\n"),
    (b"\xc3\x89\xff\n", "é", [], b"X\xff\n"),
    (b"foo\xe9 foo\n", "foo", [ALT_W], b"foo\xe9 X\n"),
], ids=["y-dot", "dot-z-backward", "a-dot-b", "not-a", "caret-dot",
        "plain-case-off", "whole-word"])
def test_change_all_writes_exactly_the_hit(tmp_path, seed, text, opts, want):
    with EditSession(str(tmp_path), seed) as s:
        assert _change_all(s, text, "X", *opts) == want


# @scenario SC-edit-model-56
@needs_utf8_engine
def test_change_all_takes_every_kind_of_glyph_as_one_character(tmp_path):
    """A stray byte, an overlong form, a surrogate and another stray byte
    are four characters: `a....b` covers them all."""
    seed = b"a\xe9\xc0\x80\xed\xa0\x80\xffb\n"
    with EditSession(str(tmp_path), seed) as s:
        assert _change_all(s, "a....b", "X", ALT_R) == b"X\n"


# @scenario SC-edit-model-56
def test_replace_with_prompt_writes_exactly_the_hit(tmp_path):
    with EditSession(str(tmp_path), b"ay\xe9z\n") as s:
        assert _change_all(s, "y.", "X", ALT_R, prompt=True) == b"aXz\n"


# ---- Grep lists a Latin-1 file by the same rules ----

def _grep_lists(tmp_path, seed, text, *opts):
    """File > Grep for @text: whether the list held the file."""
    with WpeSession(str(tmp_path), seed, filename="t.c", wait=2.0) as w:
        w.key(ALT.FILE, "g", delay=1.2)
        w.key(text, delay=0.8)
        for o in opts:
            w.key(o, delay=0.3)
        w.key(ENTER, delay=1.5)
        w.key(ENTER, delay=1.5)
        assert w.alive(), "Grep ended the editor"
        return NO_ENTRY not in "\n".join(w.display())


# @scenario SC-edit-model-56
@pytest.mark.parametrize("seed,text,opts,listed", [
    (b"\xe9a\n", "a", [], True),
    (b"\xe9a\n", "zzq", [], False),
    (b"\xe9\n", ".", ["\033r"], True),
    (b"a\xe9b\n", "a.b", ["\033r"], True),
    (b"a\xe9\n", "[^a]", ["\033r"], True),
    (b"\xe9\xff\n", "x", [], False),
], ids=["plain-beside", "absent", "dot-takes-stray", "a-dot-b", "not-a", "stray-only-absent"])
def test_grep_of_a_latin1_file(tmp_path, seed, text, opts, listed):
    assert _grep_lists(tmp_path, seed, text, *opts) == listed
