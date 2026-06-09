"""Algol 68 syntax highlighting in wpe (ncurses), for BOTH dialects.

syntax_def gives .a68/.alg a case-INSENSITIVE keyword set, so the bold words
highlight whether a file uses ga68's modern lowercase stropping (begin/end) or
a68g's classic UPPER stropping (BEGIN/END).  Verified by colour: a reserved word
must render in a different foreground colour than an ordinary identifier.

pyte records each cell's SGR colour in screen.buffer[y][x].fg, so we compare the
colour of a keyword cell against a plain identifier cell on screen.
"""
import time

import pytest

from wpe_driver import WpeSession

# Same program in each stropping regime.  "result" is an ordinary identifier
# (not a keyword), so its colour is the baseline the keywords must differ from.
LOWER = (
    "begin\n"
    "   int result := 42;\n"
    "   puts (\"done'n\")\n"
    "end\n"
)
UPPER = (
    "BEGIN\n"
    "   INT result := 42;\n"
    "   print((result, newline))\n"
    "END\n"
)


def _word_fg(w, word):
    """Foreground colour of the first cell of WORD on screen, or None."""
    rows = w.display()
    for y, text in enumerate(rows):
        idx = text.find(word)
        if idx >= 0:
            return w.screen.buffer[y][idx].fg
    return None


def _check_highlight(tmp_path, src, keyword, fname):
    with WpeSession(str(tmp_path), src, filename=fname, wait=2.0) as w:
        time.sleep(0.5)
        assert w.alive(), "wpe died opening %s" % fname
        kw_fg = _word_fg(w, keyword)
        id_fg = _word_fg(w, "result")
        assert kw_fg is not None, "%r not found on screen" % keyword
        assert id_fg is not None, "identifier 'result' not found on screen"
        assert kw_fg != id_fg, \
            "keyword %r (fg=%s) should be highlighted differently from the " \
            "identifier 'result' (fg=%s)" % (keyword, kw_fg, id_fg)


def test_highlight_ga68_lowercase(tmp_path):
    """Modern (ga68) stropping: lowercase 'begin' highlights."""
    _check_highlight(tmp_path, LOWER, "begin", "hi_lower.a68")


def test_highlight_a68g_uppercase(tmp_path):
    """Classic (a68g) stropping: UPPER 'BEGIN' highlights (case-insensitive)."""
    _check_highlight(tmp_path, UPPER, "BEGIN", "hi_upper.a68")
