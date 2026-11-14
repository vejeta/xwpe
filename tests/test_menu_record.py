"""The state channel names the dropdown that is open (the M record).

A display with no text screen cannot be read the way a terminal is, so the
editor's test dump says what the open dropdown holds: the bar entry it hangs
from, how many rows it has and the text of each.  These rows read that record
and what the Block menu's Mark Word row does, in the programming editor.

Every row reads the editor's own state, never the screen.
"""
import pytest

from edit_model.oracle import EditSession

SEED = "hello world\nsecond line\nthird\n"
FRESH = 0x98            # a fresh profile's Options2
MODERN = 0x0001
SELECTION = 0x0400
ALT_BLOCK, ALT_WINDOW, ALT_EDIT, ALT_FILE = "\033b", "\033w", "\033e", "\033f"
ESC = "\033"

# The rows every menu of the programming bar has on a terminal, with the
# debugger built in; the bar entry is its place in that bar.
MENU_ROWS = [("\033#", 0, 6), (ALT_FILE, 1, 11), (ALT_EDIT, 2, 7), ("\033s", 3, 4),
             (ALT_BLOCK, 4, 15), ("\033r", 5, 12), ("\033d", 6, 16),
             ("\033p", 7, 6), ("\033o", 8, 9), (ALT_WINDOW, 9, 11), ("\033h", 10, 8)]


def _session(tmp_path, options2):
    return EditSession(str(tmp_path), SEED, filename="t.c",
                       xwperc="[General]\nOptions2 : %d\n" % options2)


# @scenario SC-menus-20
@pytest.mark.parametrize("alt,bar,rows", MENU_ROWS,
                         ids=["system", "file", "edit", "search", "block", "run",
                              "debug", "project", "options", "window", "help"])
def test_the_open_dropdown_names_its_rows(tmp_path, alt, bar, rows):
    with _session(tmp_path, FRESH) as s:
        d = s.send([alt])
        assert d.ctx == "menu"
        assert d.menu is not None, d.raw
        assert d.menu[0] == bar
        assert len(d.menu[1]) == rows, d.menu[1]


# @scenario SC-menus-21
def test_a_closed_menu_names_nothing(tmp_path):
    with _session(tmp_path, FRESH) as s:
        s.send([ALT_EDIT])
        d = s.send([ESC, ESC])
        assert d.menu is None, d.raw


# @scenario SC-menus-22
def test_the_window_menu_of_a_terminal_has_user_screen(tmp_path):
    with _session(tmp_path, FRESH) as s:
        d = s.send([ALT_WINDOW])
        assert any(r.startswith("User Screen") for r in d.menu[1]), d.menu[1]


# @scenario SC-menus-23
@pytest.mark.parametrize("options2,shown", [(FRESH, False), (FRESH | SELECTION, True),
                                            (FRESH | MODERN, True),
                                            (FRESH | MODERN | SELECTION, True)],
                         ids=["borland-anchor", "borland-selection", "modern-anchor",
                              "modern-selection"])
def test_mark_word_is_in_the_block_menu_where_the_mode_has_it(tmp_path, options2, shown):
    with _session(tmp_path, options2) as s:
        d = s.send([ALT_BLOCK])
        rows = d.menu[1]
        assert len(rows) == (16 if shown else 15), rows
        assert any(r.startswith("MarK Word") for r in rows) == shown, rows


# @scenario SC-menus-24
@pytest.mark.parametrize("options2", [FRESH | SELECTION, FRESH | MODERN],
                         ids=["borland-selection", "modern-anchor"])
def test_alt_b_k_marks_the_word_under_the_cursor(tmp_path, options2):
    with _session(tmp_path, options2) as s:
        s.send([ALT_BLOCK])
        d = s.send(["k"])
        assert d.ctx == "edit"
        assert (d.win.mb, d.win.me) == ((0, 0), (0, 5)), (d.win.mb, d.win.me)


# @scenario SC-menus-25
def test_alt_b_k_does_nothing_where_the_menu_has_no_mark_word(tmp_path):
    """Positive control: Borland keys with anchor blocks have no such row, so
    the letter does not mark the word."""
    with _session(tmp_path, FRESH) as s:
        s.send([ALT_BLOCK])
        d = s.send(["k"])
        assert (d.win.mb, d.win.me) != ((0, 0), (0, 5)), (d.win.mb, d.win.me)


CTRL_X, CTRL_U, CTRL_K = "\x18", "\x15", "\x0b"


# @scenario SC-menus-27
@pytest.mark.parametrize("keys", [[CTRL_X], ["\033e", "d"], ["\033e", "t"]],
                         ids=["ctrl-x", "edit-delete", "edit-cut"])
def test_a_refusal_is_said_once(tmp_path, keys):
    """Cut says Nothing is marked once, not twice: the copy step under it is
    silent and the delete says the line.  The state channel counts the lines
    said since the dump before."""
    with _session(tmp_path, FRESH) as s:
        d = s.send(keys)
        assert d.says == 1, d.raw


# @scenario SC-menus-28
def test_a_cut_with_a_block_and_the_undo_of_a_deletion_say_nothing(tmp_path):
    """Positive controls: the cut of a marked block says no line, and the
    Undo that replays a block deletion calls the silent primitive."""
    with _session(tmp_path, FRESH) as s:
        s.send([CTRL_K, "b"])
        s.send(["\033[C"] * 5)
        s.send([CTRL_K, "k"])
        d = s.send([CTRL_X])
        assert d.says == 0, d.raw
        d = s.send([CTRL_U])
        assert d.says == 0, d.raw
        assert d.win.lines[0][1] == b"hello world"


CTRL_G = "\x07"


# @scenario SC-menus-29
@pytest.mark.parametrize("keys", [["\033d", "q"], ["\033d", "i"], ["\033d", "e"],
                                  ["\033d", "d"], ["\033d", "a"], ["\033d", "k"],
                                  [CTRL_G, "q"], [CTRL_G, "k"]],
                         ids=["debug-quit", "debug-interrupt", "debug-edit-watch",
                              "debug-delete-watch", "debug-remove-watches", "debug-show-stack",
                              "ctrl-g-quit", "ctrl-g-show-stack"])
def test_no_debug_session_is_said_once(tmp_path, keys):
    """A debugger command that needs a session says No debug session once,
    from the menu and from the Ctrl-G letters alike: the status row alone
    cannot tell one line from the same line said twice, the count can."""
    with _session(tmp_path, FRESH) as s:
        d = s.send(keys)
        assert d.ctx == "edit", d.raw
        assert d.says == 1, d.raw


# @scenario SC-menus-26
def test_alt_b_o_marks_the_whole_text(tmp_path):
    """Positive control kept from before the table: Mark WhOle is still Alt-B O."""
    with _session(tmp_path, FRESH) as s:
        s.send([ALT_BLOCK])
        d = s.send(["o"])
        assert d.win.mb == (0, 0)
        assert d.win.me[0] >= 2, d.win.me
