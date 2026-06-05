"""Terminal (ncurses) scrollbar glyph regression test for wpe.

The vertical scrollbar is drawn from the sp_chr[] ACS table (we_term.c).  It
used ACS_S9 (a thin scan line) for the track and ACS_DIAMOND for the thumb.
The `linux` console terminfo does not map those to good glyphs, so on a real
Linux VT the track fell back to a column of 's'/'_' and the thumb to a poor
diamond -- "the scrollbars look worst" on the console.

Fixed to ACS_CKBOARD (the CP437 shaded board) for the track and ACS_BLOCK
(solid block) for the thumb -- both are in the linux acsc (mapped to CP437
0xB1 and 0xDB) and in every VGA/console font, so the scrollbar reads as a
shaded gutter with a solid slider, matching the X11/Xft look.

Note: pyte is a minimal VT that shows the raw acsc letter, not the alt-charset
glyph, so this guards the REGRESSION (track must not be the old 's'/'_' scan
line; thumb must not be the old diamond) rather than the final pixels, which
only a real console/terminal renders.
"""
import time

from wpe_driver import WpeSession

# CKBOARD/shade family (acsc 'a' in pyte, or the Unicode shades on a UTF-8 VT)
TRACK_OK = set("a░▒▓")
# BLOCK family (acsc '0', or U+2588/U+25AE block forms)
THUMB_OK = set("0█▀▄▮▭")
# the broken old look
BROKEN_TRACK = set("s_")
BROKEN_THUMB = set("`+◆♦")
ARROWS = set("^v↑↓▲▼")


def _scrollbar_column(rows):
    """The rightmost column over the editor body (the vertical scrollbar)."""
    return [rows[i][-1] for i in range(2, 19) if len(rows[i]) > 1]


def test_scrollbar_track_and_thumb_glyphs():
    seed = "".join("line %d\n" % i for i in range(1, 60))
    with WpeSession("/tmp", seed, filename="t_scroll.c") as w:
        time.sleep(0.5)
        col = _scrollbar_column(w.display())
        body = [c for c in col if c not in ARROWS and c != " "]
        assert body, "no scrollbar glyphs found in the right column: %r" % col

        track = [c for c in body if c not in THUMB_OK]
        # Regression guards: the broken S9/diamond look must be gone.
        assert not (set(track) & BROKEN_TRACK), \
            "scrollbar track regressed to the ACS_S9 's'/'_' look: %r" % col
        assert not (set(body) & BROKEN_THUMB), \
            "scrollbar thumb regressed to the ACS_DIAMOND look: %r" % col
        # And the track must be the shaded board (CKBOARD/shade family).
        assert track and set(track) <= TRACK_OK, \
            "scrollbar track should be the shaded board (CKBOARD), got: %r" % col
        # A solid-block thumb must be present somewhere on the bar.
        assert set(body) & THUMB_OK, \
            "scrollbar thumb should be a solid block (ACS_BLOCK), got: %r" % col
