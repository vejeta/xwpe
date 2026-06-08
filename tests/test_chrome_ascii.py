"""Non-UTF-8 console chrome fallback for wpe (we_term.c).

The window title bar carries a close box (U+2715) and a zoom/restore box
(U+25A1 / U+25A3), written into the SCREENCELL buffer as raw codepoints by the
shared chrome code.  On a terminal whose locale is NOT UTF-8 (the C locale's
ANSI_X3.4-1968, a serial console, ISO-8859-*), wcwidth() reports those glyphs
unprintable, so the emit loop used to blank them -- the buttons silently
vanished and the user had no visible close/zoom affordance.

Fix (we_term.c): detect the locale's CODESET once (e_t_detect_unicode) and, on
a non-UTF-8 terminal, substitute an ASCII stand-in (e_t_chrome_ascii: 'x'
close, '^' zoom, 'v' restore) before the wcwidth() guard.  Box-drawing and the
scrollbar are unaffected -- they use ncurses ACS, which ncurses downgrades to
the VT100/ASCII line set on its own.

pyte decodes the pty byte stream, so in the C locale it sees the literal ASCII
stand-ins and in UTF-8 it sees the real codepoints; both are asserted here.
"""
from wpe_driver import WpeSession

SEED = "int main(void){\n  return 0;\n}\n"

# The Unicode chrome buttons that must NOT survive on a non-UTF-8 terminal.
UNICODE_BUTTONS = set("✕□▣◻")


def _title_row(rows):
    """The window title bar -- row 1 (row 0 is the menu bar).  It holds the
    filename plus the zoom and close boxes; the filename here (t_chrome.c) has
    no 'x' or '^', so those glyphs identify the buttons unambiguously."""
    return rows[1]


def test_non_utf8_console_shows_ascii_buttons():
    """In the C locale the close/zoom boxes degrade to ASCII 'x'/'^', not blanks."""
    with WpeSession("/tmp", SEED, filename="t_chrome.c",
                    env_extra={"LC_ALL": "C", "LANG": "C",
                               "LC_CTYPE": "C"}) as w:
        row = _title_row(w.display())
        assert w.alive(), "wpe died in the C locale"
        assert "x" in row, \
            "close box should fall back to ASCII 'x' in C locale, row=%r" % row
        assert "^" in row, \
            "zoom box should fall back to ASCII '^' in C locale, row=%r" % row
        assert not (UNICODE_BUTTONS & set(row)), \
            "no Unicode chrome should survive on a non-UTF-8 terminal, row=%r" % row


def test_utf8_console_keeps_unicode_buttons():
    """In a UTF-8 locale the title bar keeps the real Unicode boxes (no regression)."""
    with WpeSession("/tmp", SEED, filename="t_chrome.c",
                    env_extra={"LC_ALL": "C.UTF-8", "LANG": "C.UTF-8"}) as w:
        row = _title_row(w.display())
        assert w.alive(), "wpe died in the UTF-8 locale"
        assert UNICODE_BUTTONS & set(row), \
            "UTF-8 title bar should keep the Unicode close/zoom boxes, row=%r" % row
