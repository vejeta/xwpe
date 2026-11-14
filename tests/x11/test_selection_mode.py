"""X11 GUI tests: the selection block mode and Keep Final newline (xwpe).

Options > Editor holds the boxes "SelEction blocK" (Alt-E) and "Keep Final
newline" (Alt-F).  With the first ticked, Shift and a cursor key marks a
selection and a typed character replaces it; with the second ticked, Save
writes the file's last line break as it was read.

The bodies run the same keys as the terminal rows of tests/test_selection_mode.py,
through the X key path (Shift as a modifier on the keysym, Alt-letter for the
dialog's hotkeys) and read the file Save wrote.
"""
import pytest

pytest.importorskip("PIL")  # python3-pil; skip cleanly if absent


def _tick_in_editor_options(xwpe, *hotkeys):
    """Options > Editor, the hotkeys given (each one box), Ok."""
    xwpe.key("alt+o", delay=0.7)
    xwpe.key("e", delay=1.0)
    for hk in hotkeys:
        xwpe.key("alt+" + hk, delay=0.6)
    xwpe.key("Return", delay=0.8)


# @scenario SC-selection-13
def test_typing_over_a_shift_selection_replaces_it(xwpe):
    """t.c starts `int main(){`: Shift-Right three times marks `int`, `Z`
    takes its place, and Save writes it."""
    _tick_in_editor_options(xwpe, "e")
    for _ in range(3):
        xwpe.key("shift+Right", delay=0.3)
    xwpe.key("z", delay=0.4)
    xwpe.save()
    text = xwpe.saved_text()
    assert text.startswith("z main(){"), text


# @scenario SC-selection-13
def test_a_plain_cursor_key_ends_the_selection_so_typing_inserts(xwpe):
    _tick_in_editor_options(xwpe, "e")
    for _ in range(3):
        xwpe.key("shift+Right", delay=0.3)
    xwpe.key("Right", delay=0.3)
    xwpe.key("z", delay=0.4)
    xwpe.save()
    assert xwpe.saved_text().startswith("int zmain(){"), xwpe.saved_text()


# @scenario SC-selection-21
def test_a_block_ctrl_k_b_started_stays_open_over_plain_cursor_keys(xwpe):
    """Ctrl-K B, three plain Right keys, Ctrl-K K marks `int`, which `z`
    replaces: the plain keys did not end the block."""
    _tick_in_editor_options(xwpe, "e")
    xwpe.key("ctrl+k", "b", delay=0.5)
    for _ in range(3):
        xwpe.key("Right", delay=0.3)
    xwpe.key("ctrl+k", "k", delay=0.5)
    xwpe.key("z", delay=0.4)
    xwpe.save()
    assert xwpe.saved_text().startswith("z main(){"), xwpe.saved_text()


# @scenario SC-selection-22
def test_paste_over_a_selection_replaces_it_and_leaves_the_text_unselected(xwpe):
    """`int` is copied, then `in` selected and pasted over: `intt main(){`,
    and the next typed `z` goes after the pasted text."""
    _tick_in_editor_options(xwpe, "e")
    for _ in range(3):
        xwpe.key("shift+Right", delay=0.3)
    xwpe.key("ctrl+c", delay=0.5)
    xwpe.key("Home", delay=0.3)
    for _ in range(2):
        xwpe.key("shift+Right", delay=0.3)
    xwpe.key("ctrl+v", delay=0.6)
    xwpe.key("z", delay=0.4)
    xwpe.save()
    assert xwpe.saved_text().startswith("intzt main(){"), xwpe.saved_text()


# @scenario SC-selection-14
@pytest.mark.parametrize("xwpe", [{"XWPE_TEST_SOURCE": "a\nb"}], indirect=True)
def test_keep_final_newline_saves_a_file_without_one_as_it_was(xwpe):
    _tick_in_editor_options(xwpe, "f")
    xwpe.save()
    assert xwpe.saved_text() == "a\nb"
