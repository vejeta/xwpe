"""The dropdown engine under the Alt-G menu: a repeated prefix, bar letters, the divider.

The Alt-G dropdown hangs from no bar menu, so three things the top File/Edit
menus do must not happen here: the Alt form of a row letter is not a shortcut
(the rows advertise "Alt-G <letter>", a prefix and a letter), Left and Right
have no neighbouring menu to move to, and the blank divider between the
actions and the settings is never a row the highlight can rest on.  A main
menu's own Alt letter, on the other hand, opens that menu from the dropdown
just as it does from the editor.  Each test asserts what the user sees: the
screen, the cell colours, and the XWPE_AI_TRACE log.  The mock backend
answers every turn, so nothing here reaches a network.
"""
import pytest
from wpe_driver import WpeSession, ALT, assistant_build

pytestmark = pytest.mark.skipif(not assistant_build(),
                                reason="wpe built without --enable-ai")

ENV = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_MOCK_REPLY": "ok"}
DOWN = "\033[B"
UP = "\033[A"
END = "\033[F"
ALT_F = "\033f"
ALT_E = "\033e"
DROPDOWN_MARK = "Ask (chat)"
AGENT_POPUP = "AI agent task"
DIVIDER_MARK = "-----"
PERMISSIONS_ROW = "Permissions:"
BUILD_ROW = "Build & fix"
# Rows from Ask down to the divider: Ask, Edit, Complete, Multi-file, Agent,
# Build & fix are six selectable rows, so six Down presses would land on the
# divider if it were selectable.
DOWNS_TO_REACH_DIVIDER = 6


def _env(tmp_path):
    return dict(ENV, XWPE_AI_TRACE=str(tmp_path / "t.trace"))


def _trace(tmp_path):
    path = tmp_path / "t.trace"
    if path.exists():
        return path.read_text()
    return ""


def _screen(disp):
    return "\n".join(disp)


def _find(disp, needle):
    """(1-based column, 1-based row) of ``needle`` on the screen, or None."""
    for r, line in enumerate(disp):
        i = line.find(needle)
        if i >= 0:
            return i + 1, r + 1
    return None


def _cell(s, y, x):
    """Colour attributes of one cell: what a highlight changes."""
    c = s.screen.buffer[y][x]
    return (c.fg, c.bg, c.reverse, c.bold)


def _row_cell(s, disp, needle):
    """The attributes of the first cell of the row that shows ``needle``."""
    col, row = _find(disp, needle)
    return _cell(s, row - 1, col - 1)


def _open_dropdown(s, delay=0.6):
    s.key(ALT.AI, delay=delay)
    assert DROPDOWN_MARK in _screen(s.display()), \
        "Alt-G did not open the dropdown:\n" + _screen(s.display())


# @scenario SC-ai-menu-14
def test_a_second_alt_g_leaves_the_one_dropdown_as_it_is(tmp_path):
    """Alt-G with the dropdown open runs no row and opens no second box: the
    trace holds one open line, the dropdown is still up with Ask highlighted,
    and one Esc then closes it."""
    with WpeSession(str(tmp_path), "hello\n", filename="notes.txt", env_extra=_env(tmp_path)) as s:
        _open_dropdown(s)
        first = s.display()
        lit = _row_cell(s, first, DROPDOWN_MARK)
        s.key(ALT.AI, delay=0.6)
        second = s.display()
        ask_after = _row_cell(s, second, DROPDOWN_MARK) if _find(second, DROPDOWN_MARK) else None
        trace = _trace(tmp_path)
        s.key("\033", delay=0.4)
        closed = s.display()
    assert AGENT_POPUP not in _screen(second), \
        "the second Alt-G ran the Agent row (its letter is G):\n" + _screen(second)
    assert DROPDOWN_MARK in _screen(second), \
        "the dropdown is gone after the second Alt-G:\n" + _screen(second)
    assert trace.count("menu open") == 1, "the dropdown was opened twice (trace):\n" + trace
    assert ask_after == lit, "the highlight left Ask after the second Alt-G: %r -> %r" % (lit, ask_after)
    assert DROPDOWN_MARK not in _screen(closed), "one Esc did not close the dropdown:\n" + _screen(closed)


# @scenario SC-ai-menu-15
def test_a_main_menu_alt_letter_opens_that_menu_from_the_dropdown(tmp_path):
    """Alt-F from the dropdown closes it and opens the File menu; Alt-E opens
    the Edit menu."""
    with WpeSession(str(tmp_path), "hello\n", filename="notes.txt", env_extra=_env(tmp_path)) as s:
        _open_dropdown(s)
        s.key(ALT_F, delay=0.6)
        file_menu = _screen(s.display())
        s.key("\033", delay=0.4)
        _open_dropdown(s)
        s.key(ALT_E, delay=0.6)
        edit_menu = _screen(s.display())
        s.key("\033", delay=0.4)
    assert DROPDOWN_MARK not in file_menu, "the dropdown stayed open after Alt-F:\n" + file_menu
    assert "File-Manager" in file_menu, "Alt-F from the dropdown did not open the File menu:\n" + file_menu
    assert DROPDOWN_MARK not in edit_menu, "the dropdown stayed open after Alt-E:\n" + edit_menu
    assert "Paste" in edit_menu, "Alt-E from the dropdown did not open the Edit menu:\n" + edit_menu


# @scenario SC-ai-menu-12
def test_the_divider_is_never_highlighted(tmp_path):
    """Down from Ask six times lands on Permissions, not on the divider; Up from
    Permissions lands on Build & fix; End then Up three times lands on Build &
    fix again."""
    with WpeSession(str(tmp_path), "hello\n", filename="notes.txt", env_extra=_env(tmp_path)) as s:
        _open_dropdown(s, delay=0.8)
        disp = s.display()
        lit = _row_cell(s, disp, DROPDOWN_MARK)
        unlit = _row_cell(s, disp, DIVIDER_MARK)
        for _ in range(DOWNS_TO_REACH_DIVIDER):
            s.key(DOWN, delay=0.25)
        after_downs = (_row_cell(s, disp, DIVIDER_MARK), _row_cell(s, disp, PERMISSIONS_ROW))
        s.key(UP, delay=0.3)
        after_up = (_row_cell(s, disp, DIVIDER_MARK), _row_cell(s, disp, BUILD_ROW))
        s.key(END, delay=0.3)
        for _ in range(3):
            s.key(UP, delay=0.25)
        after_end_ups = (_row_cell(s, disp, DIVIDER_MARK), _row_cell(s, disp, BUILD_ROW))
        s.key("\033", delay=0.4)
    assert lit != unlit, "the highlight is not visible: Ask %r, divider %r" % (lit, unlit)
    assert after_downs[0] != lit and after_downs[1] == lit, \
        "six Down did not skip the divider onto Permissions: divider %r, Permissions %r" % after_downs
    assert after_up[0] != lit and after_up[1] == lit, \
        "Up from Permissions did not skip the divider onto Build & fix: divider %r, Build %r" % after_up
    assert after_end_ups[0] != lit and after_end_ups[1] == lit, \
        "End then three Up did not land on Build & fix: divider %r, Build %r" % after_end_ups
