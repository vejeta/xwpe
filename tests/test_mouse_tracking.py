"""Mouse-tracking regression test for wpe (terminal mode).

A sub-process run (F9 compile) goes through e_t_sys_ini() -> e_t_terminal_hand_over(),
which disables xterm motion tracking (CSI ?1002h -> ?1002l). The symmetric
e_t_sys_end() must re-enable it, or window drag/resize silently stops working
after F9 (clicks still arrive via the basic 1000 mode, but no motion events).

This guards that wpe re-emits CSI ?1002h after returning from F9.

The second row guards the other half of the terminal's mouse state: that a
click ends when its release has been read, even when the two reports reach
the editor together.

Run: tests/.venv/bin/python -m pytest -v tests/test_mouse_tracking.py
"""

import os
import pty
import re
import select
import time

import gui_owner
from wpe_driver import WpeSession, wait_screen

WPE_BIN = os.environ.get('WPE_BIN') or os.path.join(os.path.dirname(__file__), '..', 'wpe')
ENABLE_MOTION = b'\x1b[?1002h'
ALT_SCREEN_ENTER = b'\x1b[?1049h'
KEY_F9 = b'\033[20~'


def _read_raw(workdir, pre_keys_wait, action, action_wait):
    """Run wpe, capture raw output; return bytes emitted after `action`."""
    master_fd, slave_fd = pty.openpty()
    owned = gui_owner.Terminal.start(
        gui_owner.current(), WPE_BIN, ['main.c'], workdir, workdir,
        master_fd, slave_fd, (80, 30))
    os.close(slave_fd)

    buf = bytearray()

    def drain(timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            r, _, _ = select.select([master_fd], [], [], 0.1)
            if r:
                try:
                    data = os.read(master_fd, 65536)
                except OSError:
                    break
                if not data:
                    break
                buf.extend(data)

    try:
        drain(pre_keys_wait)
        mark = len(buf)
        os.write(master_fd, action)
        drain(action_wait)
        return bytes(buf[mark:])
    finally:
        owned.end_for_row()
        os.close(master_fd)


def test_motion_tracking_reenabled_after_f9(tmp_path):
    (tmp_path / 'main.c').write_text('int main(void){return 0;}\n')
    post_f9 = _read_raw(str(tmp_path), 1.5, KEY_F9, 3.0)

    # Motion tracking must be re-enabled after the compile.
    assert ENABLE_MOTION in post_f9, (
        'wpe must re-emit CSI ?1002h after an F9 compile, else window '
        'drag/resize stops working (e_t_sys_end mouse re-enable regression)')

    # ...AND it must be the LAST mouse-mode change: some terminals reset the
    # mouse mode when ncurses re-enters the alternate screen (CSI ?1049h), so
    # if a ?1049h follows the final ?1002h the tracking is silently cancelled.
    last_motion = post_f9.rfind(ENABLE_MOTION)
    later_alt_screen = post_f9.find(ALT_SCREEN_ENTER, last_motion + 1)
    assert later_alt_screen == -1, (
        'CSI ?1049h (alt-screen enter) appears after the last CSI ?1002h; the '
        'screen switch cancels motion tracking. e_t_rearm_mouse must refresh '
        'ncurses (flush 1049h) BEFORE re-enabling the mouse.')


# The click of the second row: which text row it lands on and which column of
# that row, so the place the editor must name is known before the screen says
# where the row stands.
CLICK_LINE = 10
CLICK_COLUMN = 5
CLICK_ROW_TEXT = 'line %d' % CLICK_LINE
CLICK_SEED = ''.join('line %d\n' % n for n in range(1, 21))


def _cell_of_text_column(rows, text, column):
    """The 1-based screen cell of a text column, on the row that shows
    ``text``.  The window frame is one cell in from the left, so the cell of
    a text column is the column's own number plus one."""
    y = next(i for i, row in enumerate(rows) if text in row)
    return rows[y].index(text) + column, y + 1


def test_a_click_whose_two_reports_arrive_together_still_ends(tmp_path):
    """A press and its release that reach the editor in one read are one
    click, not a button that stays down: the cursor goes on the cell clicked,
    the status row names the place, and the editor goes on taking keys.
    Waiting for a release it had read already left it doing none of the
    three."""
    with WpeSession(str(tmp_path), CLICK_SEED, filename='t.txt') as w:
        col, row = _cell_of_text_column(w.display(), CLICK_ROW_TEXT, CLICK_COLUMN)
        os.write(w.input_fd,
                 ('\x1b[<0;%d;%dM\x1b[<0;%d;%dm' % (col, row, col, row)).encode())
        assert wait_screen(w, '%d:%d' % (CLICK_LINE, CLICK_COLUMN), tmo=8), (
            'a click that arrived in one read left the editor where it '
            'was:\n%s' % '\n'.join(w.display()))
        assert w.alive(), 'the editor died on a click that arrived in one read'
