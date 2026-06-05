"""Window-menu operation tests for wpe (terminal mode).

Drives the Window menu (Alt-W) commands that test_window_views.py does not
already cover (it owns Tile/Cascade/switch, which guard a double-free):

  - Zoom     (Alt-W Z): maximise the active window over the others
  - Next     (Alt-W X): cycle which window is active
  - Close    (Alt-W C): close the active window
  - List All (Alt-W L): open the "Windows" chooser dialog

Three files are opened (a.c, b.c, c.c -> three stacked edit windows) and we
assert on the visible window title bars after each command.

Run: tests/.venv/bin/python -m pytest -v tests/test_window_menu.py
"""

import os
import pty
import select
import subprocess
import time

import pyte

from test_utf8_border import SafeScreen

WPE_BIN = os.path.join(os.path.dirname(__file__), '..', 'wpe')

ALT_W = '\033w'
WIN_ZOOM = 'z'
WIN_NEXT = 'x'        # "Next" menu hotkey is X (e_ed_next)
WIN_CLOSE = 'c'
WIN_LIST_ALL = 'l'
KEY_ESC = '\033'
COLS, ROWS = 80, 30


def run_windows(workdir, keys, wait=1.3, key_delay=0.5, settle=0.6):
    """Open a.c/b.c/c.c, send keys, return (SafeScreen, exit_code)."""
    for name in ('a.c', 'b.c', 'c.c'):
        with open(os.path.join(workdir, name), 'w') as fh:
            fh.write('int %s;\n' % name[0])

    screen = SafeScreen(COLS, ROWS)
    stream = pyte.Stream(screen)
    master_fd, slave_fd = pty.openpty()

    env = os.environ.copy()
    env['TERM'] = 'xterm-256color'
    env['COLUMNS'] = str(COLS)
    env['LINES'] = str(ROWS)
    env['LC_ALL'] = 'en_US.UTF-8'
    env['HOME'] = workdir

    proc = subprocess.Popen(
        [WPE_BIN, 'a.c', 'b.c', 'c.c'],
        stdin=slave_fd, stdout=slave_fd, stderr=slave_fd,
        cwd=workdir, env=env, preexec_fn=os.setsid)
    os.close(slave_fd)

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
                stream.feed(data.decode('utf-8', 'replace'))

    try:
        drain(wait)
        for key in keys:
            os.write(master_fd, key.encode())
            drain(key_delay)
        drain(settle)
        exit_code = proc.poll()
    finally:
        try:
            os.killpg(os.getpgid(proc.pid), 9)
        except Exception:
            pass
        os.close(master_fd)
        proc.wait()
    return screen, exit_code


def visible_titles(screen):
    """Set of source-file names whose window title bar is currently shown."""
    found = set()
    for y in range(ROWS):
        for name in ('a.c', 'b.c', 'c.c'):
            if (' %s ' % name) in screen.display[y]:
                found.add(name)
    return found


def test_zoom_maximises_active_window(tmp_path):
    screen, code = run_windows(str(tmp_path), [ALT_W, WIN_ZOOM])
    assert code is None, 'wpe died during Zoom (exit=%r)' % code
    # The active window (c.c, opened last) fills the screen and covers the
    # other two title bars.
    titles = visible_titles(screen)
    assert titles == {'c.c'}, \
        'Zoom should leave only the active window visible, saw %s' % titles


def test_next_changes_active_window(tmp_path):
    # c.c is active on startup (opened last) and shows its content "int c;".
    # Window > Next cycles to the next window (wraps to a.c), which then
    # becomes active and shows "int a;".  Only the active window's body is
    # visible in the cascade, so this is a reliable "active changed" check.
    screen, code = run_windows(str(tmp_path), [ALT_W, WIN_NEXT])
    assert code is None, 'wpe died during Next (exit=%r)' % code
    blob = '\n'.join(screen.display)
    assert 'int a;' in blob, \
        'Next should make a.c active (its body "int a;" should show)'


def test_close_closes_active_window(tmp_path):
    screen, code = run_windows(str(tmp_path), [ALT_W, WIN_CLOSE])
    assert code is None, 'wpe died during Close (exit=%r)' % code
    titles = visible_titles(screen)
    assert 'c.c' not in titles, 'Close should remove the active window (c.c)'
    assert {'a.c', 'b.c'} <= titles, \
        'Close should keep the other windows, saw %s' % titles


def test_list_all_opens_window_chooser(tmp_path):
    screen, code = run_windows(str(tmp_path), [ALT_W, WIN_LIST_ALL])
    assert code is None, 'wpe died during List All (exit=%r)' % code
    blob = '\n'.join(screen.display)
    assert 'Windows' in blob, 'List All should open the "Windows" chooser'
