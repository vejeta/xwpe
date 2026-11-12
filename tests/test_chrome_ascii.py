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
from wpe_driver import WpeSession, MACOS

SEED = "int main(void){\n  return 0;\n}\n"

# A UTF-8 locale that actually exists on the host: macOS ships en_US.UTF-8 but
# not the glibc-ism C.UTF-8, so naming C.UTF-8 there leaves ncurses in a non-UTF-8
# locale and the Unicode chrome degrades to ASCII -- a locale-availability
# artefact, not a regression. Pick the one present on each platform.
UTF8_LOCALE = "en_US.UTF-8" if MACOS else "C.UTF-8"

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
        # The title-bar border must reach the right edge: under a non-UTF-8
        # locale ncurses compresses the horizontal line with the REP control
        # (CSI Pn b), which the driver's pyte screen honours.  A short row here
        # would mean REP was dropped and the border only looked truncated.
        assert len(row.rstrip()) >= len(row) - 1, \
            "title-bar border should span the full width (REP), row=%r" % row


def test_utf8_console_keeps_unicode_buttons():
    """In a UTF-8 locale the title bar keeps the real Unicode boxes (no regression)."""
    with WpeSession("/tmp", SEED, filename="t_chrome.c",
                    env_extra={"LC_ALL": UTF8_LOCALE, "LANG": UTF8_LOCALE}) as w:
        row = _title_row(w.display())
        assert w.alive(), "wpe died in the UTF-8 locale"
        assert UNICODE_BUTTONS & set(row), \
            "UTF-8 title bar should keep the Unicode close/zoom boxes, row=%r" % row


# Text the terminal cannot show is a mark, never a blank: in the C locale an
# accented letter, a CJK character and an emoji each show as one '?'.
TEXT = "café 日 \U0001F600"
C_ENV = {"LC_ALL": "C", "LANG": "C", "LC_CTYPE": "C"}
U_ENV = {"LC_ALL": UTF8_LOCALE, "LANG": UTF8_LOCALE}
END = "\033[F"


def _row_of(rows, needle):
    """The screen row holding @needle and the column it starts at."""
    for y, r in enumerate(rows):
        if needle in r:
            return y, r.index(needle)
    raise AssertionError("%r not on screen:\n%s" % (needle, "\n".join(rows)))


# @scenario SC-edit-model-52
def test_c_locale_window_shows_a_mark_for_each_glyph(tmp_path):
    with WpeSession(str(tmp_path), TEXT + "\n", filename="t.txt",
                    env_extra=C_ENV) as w:
        rows = w.display()
        y, x = _row_of(rows, "caf?")
        assert rows[y][x:x + 8] == "caf? ? ?", rows[y]
        w.key(END, delay=0.3)
        w.display()
        assert (w.screen.cursor.y, w.screen.cursor.x) == (y, x + 8)


# @scenario SC-edit-model-52
def test_utf8_locale_window_still_shows_the_text(tmp_path):
    with WpeSession(str(tmp_path), TEXT + "\n", filename="t.txt",
                    env_extra=U_ENV) as w:
        rows = w.display()
        y, x = _row_of(rows, "café 日")
        assert "\U0001F600" in rows[y], rows[y]
        w.key(END, delay=0.3)
        w.display()
        assert (w.screen.cursor.y, w.screen.cursor.x) == (y, x + 10)


# @scenario SC-edit-model-52
def test_c_locale_dialog_field_shows_a_mark_for_each_glyph(tmp_path):
    with WpeSession(str(tmp_path), TEXT + "\nfoo\n", filename="t.txt",
                    env_extra=C_ENV) as w:
        w.key("\033[H", "\x0b", "b", END, "\x0b", "k", delay=0.3)
        w.key("\x03", delay=0.5)
        w.key("\033[B", "\033[H", delay=0.3)
        w.menu("\033s", "r")
        w.key("foo")
        w.key("\033n")
        w.key("\x16", delay=0.5)
        rows = [r for r in w.display() if "New Text:" in r]
        assert rows and "caf? ? ?" in rows[0], rows
