"""A row that ends the editor with a key leaves the display usable for the
next row, whether the editor had a terminal of its own or not.

Two rows, in this order and in one session: the first starts xwpe with its
standard streams on a pty (the owner's terminal_fd) and ends it with Alt-X,
which leaves the chord's last keys to be sent to a window that is gone; the
second starts a plain editor and needs its window.  Before the display's
keyboard was cleared between rows, the second editor quit at once and its
window never appeared.
"""
import pytest

PTY = {"XWPE_TEST_PTY": True}


# @scenario SC-term-11
@pytest.mark.parametrize("xwpe", [PTY], indirect=True)
def test_a_first_editor_with_a_terminal_is_ended_by_alt_x(xwpe):
    xwpe.key("alt+x", delay=1.0)
    xwpe.proc.wait(timeout=10)
    assert xwpe.proc.returncode == 0


# @scenario SC-term-11
def test_b_the_next_plain_editor_still_gets_its_window(xwpe):
    assert xwpe.proc.poll() is None, "the editor after the Alt-X row quit at once"
