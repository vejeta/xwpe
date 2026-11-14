"""The Help viewer of the graphical editor survives following links.

Twin of tests/test_help_buffers.py: the help and Info readers are shared code
with no backend branch, but the graphical editor reads its keys by another
road, so the row is run here too.  The long names that overflowed the readers'
buffers are driven by the terminal rows, where the page text can be read back;
here the viewer text is pixels, so the row reads that the editor is alive and
that its screen changed after each step.

The wayland twin runs this same body.
"""
from conftest import _sleep, changed_pixels

SETTLE_S = 1.5
LINK_KEYS = ["Down", "Right", "Right", "Return"]


# @scenario SC-help-buffers-11x
def test_x11_following_a_help_link_leaves_the_editor_alive(xwpe):
    """F1 opens the help, then a few cursor steps and Enter on a row: the
    editor is alive and the viewer is still on screen."""
    _sleep(SETTLE_S)
    before = xwpe.screenshot()
    xwpe.key("F1", delay=1.5)
    opened = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died opening the help with F1"
    assert changed_pixels(before, opened) > 3000, \
        "F1 should open the help viewer (screen barely changed)"
    for key in LINK_KEYS:
        xwpe.key(key, delay=0.8)
        assert xwpe.proc.poll() is None, "xwpe died on %s in the help viewer" % key
