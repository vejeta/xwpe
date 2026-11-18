"""A word of a syntax_def longer than the editor can hold is refused, in a box.

The editor read every word of the user's ~/.xwpe/syntax_def (extensions,
keywords, operators) with a scan that put no limit on the word into a
4096-byte buffer, so a word of 5000 bytes wrote past it.  The file is the
user's own, but it is also what a copied dotfiles folder or a shared home
brings in, and the editor must not die at start on it.

Each row starts the editor with such a file and asserts that it is alive, that
no sanitizer report is on the screen, and that the box names the problem.
The row is red on a plain build only when the damage is loud; under the
sanitizer build (WPE_BIN=$PWD/wpe-asan, ASAN_OPTIONS=detect_leaks=0:
abort_on_error=1) it is red on the unfixed tree for the stack overflow.

A word of 4000 bytes, which fits, is the control: it is read, so no box.
"""
import os
import re

import pytest

from wpe_driver import WpeSession

SANITIZER = "AddressSanitizer"
TOO_LONG_WORD = 5000       # past the 4096 bytes of the editor's word buffer
FITTING_WORD = 4000        # inside it
WORD_BOX = "word of the personal syntax_def is too long"
READ_BOX = "Error reading syntax_def"
ENTER = "\r"
COLS = 100
WIDE_COLS = 200            # room for a test folder's whole path in the box
ASAN_ENV = {"ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1"}
# A language the editor reads whole: .zz files, one keyword.  The tail is the
# six characters and three numbers every entry ends with.
GOOD_LANGUAGE = ".zz\n1 begin\n0\n~ /* */ NULL NULL \"'#\\\\  -1 -1 1000\n"
KEYWORD = "begin"
PLAIN = "x end"            # its first cell is the plain identifier x


def _raw(w):
    return bytes(w.raw).decode("utf-8", "replace")


def _screen_text(w):
    return "\n".join(w.display())


def _start_with(tmp_path, syntax_text, name, seed="int x;\n", filename="t.c",
                cols=COLS):
    """The editor started with @syntax_text as the personal syntax_def."""
    home = tmp_path / name
    (home / ".xwpe").mkdir(parents=True)
    (home / ".xwpe" / "syntax_def").write_text(syntax_text)
    return WpeSession(str(home), seed, filename=filename, wait=2.0,
                      cols=cols, new_profile=True, env_extra=dict(ASAN_ENV))


def _fg_at(w, text):
    """The foreground pyte holds for the first cell of @text, or None."""
    for y, row in enumerate(w.screen.display):
        x = row.find(text)
        if x >= 0:
            return w.screen.buffer[y][x].fg
    return None


@pytest.mark.parametrize("where", ["first", "keyword", "operator"])
def test_a_word_longer_than_the_buffer_is_refused(tmp_path, where):
    big = "a" * TOO_LONG_WORD
    text = {
        "first": big + " 0 0 NULL NULL NULL NULL\n",
        "keyword": "1 .c 1 " + big + " 0 NULL NULL NULL\n",
        "operator": "1 .c 0 1 " + big + " NULL NULL NULL\n",
    }[where]
    with _start_with(tmp_path, text, where) as w:
        assert w.alive(), "the editor died reading a long word (%s)" % where
        assert SANITIZER not in _raw(w), \
            "sanitizer report (%s):\n%s" % (where, _raw(w)[-1500:])
        assert WORD_BOX in _screen_text(w), \
            "no box names the long word (%s):\n%s" % (where, _screen_text(w))
        w.key(ENTER, delay=0.8)
        assert w.alive(), "the editor died after the box (%s)" % where


def test_a_word_that_fits_is_read(tmp_path):
    text = "1 .c 1 " + "a" * FITTING_WORD + " 0 NULL NULL NULL\n"
    with _start_with(tmp_path, text, "fits") as w:
        assert w.alive()
        assert SANITIZER not in _raw(w)
        assert WORD_BOX not in _screen_text(w)


def _box_row(w):
    """The screen row that holds the too-long box's sentence, or None."""
    for row in w.display():
        if WORD_BOX in row:
            return row
    return None


def test_the_box_names_the_file_within_its_width(tmp_path):
    """The folder is deep enough that its path cannot fit a 100 column box:
    the front of the path is left out ("..."), the end that tells which file
    it is stays, and the whole box is on the screen."""
    deep = "d" * 120
    text = "a" * TOO_LONG_WORD + " 0 0 NULL NULL NULL NULL\n"
    with _start_with(tmp_path, text, deep) as w:
        assert w.alive()
        row = _box_row(w)
        assert row is not None, "no box:\n%s" % _screen_text(w)
        assert "..." in row, "a path that cannot fit is not shortened: %r" % row
        assert re.search(r"\.xwpe/syntax_def\s*\S?\s*$", row), \
            "the file's own end, then the box's edge, is not shown: %r" % row
        assert len(row) <= COLS


def test_the_box_names_a_path_that_fits_whole(tmp_path):
    text = "a" * TOO_LONG_WORD + " 0 0 NULL NULL NULL NULL\n"
    with _start_with(tmp_path, text, "h", cols=WIDE_COLS) as w:
        row = _box_row(w)
        assert row is not None, "no box:\n%s" % _screen_text(w)
        assert str(tmp_path / "h" / ".xwpe" / "syntax_def") in row, \
            "the box does not name the file: %r" % row
        assert "..." not in row


def test_the_languages_read_before_a_long_word_stay(tmp_path):
    """A good language, then one with a word too long: the box is shown, and
    the first language still colours its files.  Red if the parser ever keeps
    nothing of a file it could not read to the end."""
    big = "a" * TOO_LONG_WORD
    text = (GOOD_LANGUAGE + "\n.yy\n1 " + big +
            "\n0\n~ /* */ NULL NULL NULL \"'#\\\\  -1 -1 1000\n")
    with _start_with(tmp_path, text, "stay", seed="begin x end\n",
                     filename="t.zz") as w:
        assert w.alive()
        assert WORD_BOX in _screen_text(w), \
            "no box for the second language:\n%s" % _screen_text(w)
        w.key(ENTER, delay=1.0)
        assert KEYWORD in _screen_text(w), "the file did not open"
        keyword = _fg_at(w, KEYWORD)
        plain = _fg_at(w, PLAIN)
        assert keyword != plain, \
            "'%s' is not coloured (%s): the language read before the long " \
            "word was dropped" % (KEYWORD, keyword)


def test_a_file_of_good_languages_colours(tmp_path):
    """The control of the row above: the same first language alone."""
    with _start_with(tmp_path, GOOD_LANGUAGE, "alone", seed="begin x end\n",
                     filename="t.zz") as w:
        assert w.alive()
        assert WORD_BOX not in _screen_text(w)
        assert _fg_at(w, KEYWORD) != _fg_at(w, PLAIN), \
            "the control language does not colour its keyword"
