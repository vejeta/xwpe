"""Enter in a Grep or Find result window that lists nothing no longer ends the
graphical editor.

Twin of tests/test_empty_list_enter.py: the list is shared code, but the
graphical editor reads its keys by another road, so the row is run here too.
The row reads only that the editor is alive, because the note that says "No
entry in this list" is drawn as pixels, not cells.

The wayland twin runs this same body.
"""
import pytest

from conftest import _sleep

SETTLE_S = 1.5
NO_MATCH = "zzqnomatchzzq"


def _alive(xwpe, what):
    assert xwpe.proc.poll() is None, "xwpe died: " + what


# @scenario SC-empty-list-entry-01x
@pytest.mark.parametrize("item,name", [("g", "Grep"), ("f", "Find")])
def test_x11_enter_in_an_empty_result_window_leaves_the_editor_alive(xwpe, item, name):
    """File > Grep (or Find) for text that is nowhere, then Enter in the list."""
    _sleep(SETTLE_S)
    xwpe.menu("f", item, delay=1.2)
    xwpe.type(NO_MATCH, delay=0.8)
    xwpe.key("Return", delay=2.0)
    _alive(xwpe, "File > %s listed no match" % name)
    xwpe.key("Return", delay=1.5)
    _alive(xwpe, "Enter in an empty %s window" % name)
    xwpe.key("Return", delay=1.0)             # the note
    _alive(xwpe, "dismissing the note of the empty %s window" % name)
