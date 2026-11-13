"""Boxes have one layout path (terminal rows; the owner is we_layout.c).

What the user sees when the screen changes size while something holds the
screen: the editor comes back whole.  A command that takes the screen
(Ctrl-F9 on a shell script, which waits for Return) saves what was behind
it; if the terminal is resized meanwhile, that saved picture describes a
screen that is gone, and putting it back drew the old status row in the
middle of the new screen (grown) or an old text row on the last row
(shrunk).  The owner stamps every saved view with the screen's size and
puts back only a current one.

Rows: tests/x11/test_box_layout.py and tests/wayland/test_box_layout.py
are the graphical twins; the placement rules are in test_layout (make
check).
"""
import pytest

from wpe_driver import WpeSession

RUN_CTRL_F9 = "\x1b[20;5~"
STRIKE = "Strike <Return> to continue"
GROWN = (100, 40)
SHRUNK = (60, 20)


def _screen_after_strike_return(tmp_path, start, resized):
    """The display after Ctrl-F9 on a script, a resize to `resized` while the
    wait is up (None for none), and Return.  The session starts at `start`."""
    with WpeSession(str(tmp_path), "echo HITMARK\n", filename="h.sh",
                    cols=start[0], rows=start[1]) as w:
        w.key(RUN_CTRL_F9, delay=1.5)
        assert STRIKE in "\n".join(w.display()), "the wait did not appear"
        if resized is not None:
            w.resize(resized[0], resized[1])
        w.key("\r", delay=1.0)
        return list(w.display())


def _fresh(tmp_path, size):
    return _screen_after_strike_return(tmp_path / "fresh", size, None)


# @scenario SC-box-06
@pytest.mark.parametrize("name,size", [("grow", GROWN), ("shrink", SHRUNK)])
def test_strike_return_ends_with_the_status_line_on_the_last_row(
        tmp_path, name, size):
    """Resizing during the wait must not bring the old screen back: the last
    row, the status line, is what a session started at that size shows, and
    no row of the old size's status line is left in the middle."""
    (tmp_path / "fresh").mkdir()
    fresh = _fresh(tmp_path, size)
    after = _screen_after_strike_return(tmp_path, (80, 30), size)
    assert after[-1].rstrip() == fresh[-1].rstrip(), \
        "the status row after %s is %r, a fresh session at that size shows %r" % (
            name, after[-1].rstrip(), fresh[-1].rstrip())
    status = fresh[-1].strip()
    stray = [i for i, row in enumerate(after[:-1]) if status and status in row]
    assert not stray, \
        "the old status line was put back on row(s) %s:\n%s" % (
            stray, "\n".join(after))
