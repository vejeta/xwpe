"""File-Manager type-ahead: typing a long name into a panel is safe.

Typing in the Files or DirTree panel jumps the selection to the first name
that starts with what was typed (e_file_window, we_fl_fkt.c).  The typed text
lives in a buffer on the stack; it used to hold 17 characters and the 18th
and later ones were written past its end, which AddressSanitizer reports as a
stack-buffer-overflow and a normal build turns into a corrupted stack.

The rows open the File-Manager on a scratch folder holding one file with a
40 character name and one with a short name, type the name in the panel and
check that the panel lands on the file and that the editor is still alive.
One row types into the DirTree panel, and one types more characters than the
buffer holds, so the key that would fill its last byte is ignored rather than
written.  Under a sanitizer build (launched as wpe-asan) the long rows are red
on the old code, and the last row is red when the full-buffer check is off by
one.

Run: WPE_BIN=$PWD/wpe python3 -m pytest -q tests/test_file_manager_typeahead.py
"""
import os

from wpe_driver import WpeSession

F3 = "\033OR"                 # opens the File-Manager (xterm-256color kf3)
TAB = "\t"
# Both names sort after the editor's own file t.c, so the selection only
# reaches them by type-ahead; the long one sorts last.
LONG_NAME = "zzzz0123456789abcdefghij0123456789qrst"
SHORT_NAME = "zeta.txt"
TYPED_LONG = 30               # characters typed of the long name
TYPED_SHORT = 3
HOME_KEY = "\033[H"           # the DirTree panel opens on its last row
TABS_TO_DIRTREE = 3           # three Tabs after F3 focus the DirTree panel
# More than the 4096-byte type-ahead buffer holds; sent in chunks so the
# row takes seconds, not minutes.
TYPED_PAST_BUFFER = 4200
CHUNK = 64


def _make_folder(workdir):
    for name in (LONG_NAME, SHORT_NAME):
        with open(os.path.join(workdir, name), "w") as fh:
            fh.write("x\n")


def _panel_cell(s, name):
    """(row, col) of the first character of name in the Files panel, or None."""
    rows = s.screen.display
    first = next((y for y, line in enumerate(rows) if "Files:" in line), None)
    if first is None:
        return None
    for y in range(first + 1, len(rows)):
        x = rows[y].find(name[:12])
        if x >= 0:
            return (y, x)
    return None


def _selected(s, name):
    """True when the panel row of name is drawn with the selection colour.

    The selection colour is the one the other file rows do not have: the
    row of t.c, which is never the target here, gives the plain colour."""
    here = _panel_cell(s, name)
    plain = _panel_cell(s, "t.c")
    assert here is not None and plain is not None, "\n".join(s.screen.display)
    return s.cell_bg(*here) != s.cell_bg(plain[0], here[1])


def _open_files_panel(s):
    s.key(F3)
    s.key(TAB)


def _type(s, text):
    for ch in text:
        s.key(ch, delay=0.05)


# @scenario SC-fm-typeahead-01
def test_long_name_typed_in_files_panel_lands_on_it(tmp_path):
    """Typing 30 characters of a 40 character name selects it, editor alive."""
    _make_folder(str(tmp_path))
    with WpeSession(str(tmp_path), "int main;\n") as s:
        _open_files_panel(s)
        _type(s, LONG_NAME[:TYPED_LONG])
        assert s.alive(), "wpe died while typing into the Files panel"
        disp = "\n".join(s.display())
        assert "File-Manager" in disp, disp
        assert _selected(s, LONG_NAME), disp


# @scenario SC-fm-typeahead-02
def test_short_name_control(tmp_path):
    """A short name behaves as before: typed, the panel lands on it."""
    _make_folder(str(tmp_path))
    with WpeSession(str(tmp_path), "int main;\n") as s:
        _open_files_panel(s)
        _type(s, SHORT_NAME[:TYPED_SHORT])
        assert s.alive()
        disp = "\n".join(s.display())
        assert "File-Manager" in disp, disp
        assert _selected(s, SHORT_NAME), disp
        assert not _selected(s, LONG_NAME), disp


def _open_dirtree_panel(s):
    s.key(F3)
    for _ in range(TABS_TO_DIRTREE):
        s.key(TAB)
    s.key(HOME_KEY)


# @scenario SC-fm-typeahead-03
def test_long_text_typed_in_dirtree_panel_keeps_editor_alive(tmp_path):
    """The DirTree panel shares the type-ahead: 30 typed characters are safe."""
    _make_folder(str(tmp_path))
    with WpeSession(str(tmp_path), "int main;\n") as s:
        _open_dirtree_panel(s)
        _type(s, LONG_NAME[:TYPED_LONG])
        assert s.alive(), "wpe died while typing into the DirTree panel"
        disp = "\n".join(s.display())
        assert "File-Manager" in disp, disp


# @scenario SC-fm-typeahead-04
def test_typing_past_the_buffer_keeps_editor_alive(tmp_path):
    """Keys typed after the type-ahead buffer is full are ignored, not written."""
    _make_folder(str(tmp_path))
    text = (LONG_NAME * (TYPED_PAST_BUFFER // len(LONG_NAME) + 1))[:TYPED_PAST_BUFFER]
    with WpeSession(str(tmp_path), "int main;\n") as s:
        _open_files_panel(s)
        for i in range(0, len(text), CHUNK):
            s.key(text[i:i + CHUNK], delay=0.15)
        assert s.alive(), "wpe died once the type-ahead buffer was full"
        disp = "\n".join(s.display())
        assert "File-Manager" in disp, disp
