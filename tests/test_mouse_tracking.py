"""Mouse-tracking regression test for wpe (terminal mode).

A sub-process run (F9 compile) goes through e_t_sys_ini() -> e_endwin(),
which disables xterm motion tracking (CSI ?1002h -> ?1002l). The symmetric
e_t_sys_end() must re-enable it, or window drag/resize silently stops working
after F9 (clicks still arrive via the basic 1000 mode, but no motion events).

This guards that wpe re-emits CSI ?1002h after returning from F9.

Run: tests/.venv/bin/python -m pytest -v tests/test_mouse_tracking.py
"""

import os
import pty
import re
import select
import subprocess
import time

WPE_BIN = os.path.join(os.path.dirname(__file__), '..', 'wpe')
ENABLE_MOTION = b'\x1b[?1002h'
ALT_SCREEN_ENTER = b'\x1b[?1049h'
KEY_F9 = b'\033[20~'


def _read_raw(workdir, pre_keys_wait, action, action_wait):
    """Run wpe, capture raw output; return bytes emitted after `action`."""
    master_fd, slave_fd = pty.openpty()
    env = os.environ.copy()
    env.update(TERM='xterm-256color', COLUMNS='80', LINES='30',
               LC_ALL='en_US.UTF-8', HOME=workdir)
    proc = subprocess.Popen(
        [WPE_BIN, 'main.c'],
        stdin=slave_fd, stdout=slave_fd, stderr=slave_fd,
        cwd=workdir, env=env, preexec_fn=os.setsid)
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
        try:
            os.killpg(os.getpgid(proc.pid), 9)
        except Exception:
            pass
        os.close(master_fd)
        proc.wait()


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
