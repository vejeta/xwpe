"""One bottom shortcut bar, on the last row, after a dialog or menu closes.

When the focused window's bottom border sits on the row just above the
shortcut bar (the docked AI pane always does; a file window dragged down to
that row as well), closing any dialog or menu used to paint the bar a second
time: " F1 Help ... F4 Search  A" one row up, from column 22, over the
window's status row, and "lt-G AI  Alt-X Quit" at the start of the last row.
The next key healed it.  The terminal refresh recorded a line wrap after a
cursor move away from a completed row, so the bar's first cell went out
without a cursor move and its text landed where the previous cell had left
the cursor.  The refresh now records the cursor from the cell it just wrote.
"""
import os
import subprocess

import pytest

from wpe_driver import WpeSession, ALT, WPE_BIN

SEED = "int main(void){return 0;}\n"
ESC = "\033"
ENTER = "\r"
HELP = "F1 Help"
# The 30-row test screen: the bar is on the last row, the window's bottom
# border sits on the row above it.
LAST_ROW = 29
BORDER_ROW = 28
# A bottom border row starts with the frame's lower-left corner glyph.
CORNER = "m"
AI_ENV = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
          "XWPE_AI_MOCK_REPLY": "r"}
# SGR mouse: press on the file window's bottom-right corner (row 20, column
# 79), drag it down to row 28, release.
DRAG_PRESS = "\033[<0;80;21M"
DRAG_MOVE = "\033[<32;80;29M"
DRAG_RELEASE = "\033[<0;80;29m"


def _ai_build():
    """True when the binary was built with --enable-ai (the pane exists)."""
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


# Both cases here open the assistant's page of Options, so the guard belongs to
# the file: it used to sit on the second test only, and the first then failed
# on a build without the assistant instead of skipping.
pytestmark = pytest.mark.skipif(not _ai_build(),
                                reason="wpe built without --enable-ai")


def dump(s):
    """The screen with row numbers, for an assertion message."""
    rows = s.display(drain=0)
    return "\n".join("%2d|%s|" % (i, r) for i, r in enumerate(rows))


def help_rows(s):
    """The rows that hold the shortcut bar's first entry."""
    rows = s.display(drain=0)
    return [i for i, r in enumerate(rows) if HELP in r]


def border_row(s):
    """The focused window's bottom border row, asserted to be where the
    tests need it: right above the shortcut bar."""
    rows = s.display(drain=0)
    assert rows[BORDER_ROW].startswith(CORNER), \
        "the window's bottom border is not on row %d\n%s" % (BORDER_ROW, dump(s))
    return rows[BORDER_ROW]


def assert_one_bar(s, border_before, label):
    """Exactly one row holds the shortcut bar and it is the last row; the
    row above it is still the window's bottom border as it was before."""
    rows = s.display(drain=0)
    found = help_rows(s)
    assert found == [LAST_ROW], "%s: %r on rows %r, want [%d]\n%s" % (
        label, HELP, found, LAST_ROW, dump(s))
    assert rows[BORDER_ROW] == border_before, \
        "%s: row %d changed\n before: %r\n after:  %r" % (
            label, BORDER_ROW, border_before, rows[BORDER_ROW])


def arm_pane(s):
    """Alt-G a: open the chat pane, docked under the file and focused."""
    s.key(ALT.AI)
    s.key("a", delay=0.6)


def open_options_ai(s):
    """Options > AI: the settings dialog must be on screen."""
    s.key(ALT.OPTIONS)
    s.key("i", delay=0.8)
    assert "AI settings" in "\n".join(s.display(drain=0)), dump(s)


def close_with(s, key):
    """Send the key that closes the dialog or menu and let the repaint land."""
    s.key(key, delay=0.8)
    s.display(drain=0.8)


def drag_file_window_to_the_bottom(s):
    """Drag the file window's bottom-right corner down so that its bottom
    border is on the row above the shortcut bar, the docked pane's geometry
    without the pane."""
    s.key(DRAG_PRESS, delay=0.2)
    s.key(DRAG_MOVE, delay=0.2)
    s.key(DRAG_RELEASE, delay=0.5)


def test_file_window_at_the_bottom_keeps_one_bar_after_options_dialog(tmp_path):
    with WpeSession(str(tmp_path), SEED) as s:
        drag_file_window_to_the_bottom(s)
        border = border_row(s)
        open_options_ai(s)
        close_with(s, ESC)
        assert_one_bar(s, border, "Options > AI closed with Esc")
        assert s.alive(), "wpe died"


# @scenario SC-ai-chat-45
# @scenario SC-ai-menu-49
def test_pane_keeps_one_bar_after_dialogs_and_menus(tmp_path):
    with WpeSession(str(tmp_path), SEED, env_extra=AI_ENV) as s:
        arm_pane(s)
        border = border_row(s)

        open_options_ai(s)
        close_with(s, ESC)
        assert_one_bar(s, border, "Options > AI closed with Esc")

        open_options_ai(s)
        close_with(s, ENTER)
        assert_one_bar(s, border, "Options > AI closed with Ok")

        s.key(ALT.WINDOW, delay=0.5)
        close_with(s, ESC)
        assert_one_bar(s, border, "Window menu closed with Esc")
        assert s.alive(), "wpe died"
