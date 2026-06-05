"""Double-free regression guards for the window backing-store (PIC views).

History: fixing the 24-year "debugging leaks memory" leak made e_close_view
always free a window's off-screen SCREENCELL view and made e_firstl hand the
existing view to e_change_pic (so it gets closed before a fresh one opens).
That exposed a latent double-free in every place that bulk-released the views
and then repainted the window tree WITHOUT clearing cn->f[i]->pic:

  - e_switch_window  (Alt-<n> / F6 window switch, and the F9 project path)
  - e_ed_cascade     (Window > Cascade, Shift-F5)
  - e_ed_tile        (Window > Tile, Shift-F4)

Each repaint (e_firstl -> e_change_pic -> e_close_view) freed the same 19 KB
block a second time and aborted the process with
"free(): invalid pointer -- Aborted (core dumped)".

All three now route through e_free_view(), which always NULLs the pointer.
These tests reproduce each user action and assert the process is still alive
afterwards (a double-free shows up as a negative exit code = killed by
SIGABRT). They double as the permanent regression net for that helper.

Run: tests/.venv/bin/python -m pytest -v tests/test_window_views.py
"""

import os
import pty
import select
import subprocess
import time

import pyte
import pytest

from test_utf8_border import SafeScreen

WPE_BIN = os.path.join(os.path.dirname(__file__), '..', 'wpe')

KEY_ALT_P = '\033p'
KEY_ALT_W = '\033w'
KEY_RETURN = '\r'
KEY_F9 = '\033[20~'
MENU_TILE = 't'          # Window menu item hotkey (Tile)
MENU_CASCADE = 'a'       # Window menu item hotkey (cAscade)
COLS, ROWS = 80, 30


def run_wpe(workdir, files, keys, wait=1.5, key_delay=0.6, settle=0.8):
    """Run wpe on `files`, send `keys`, return (SafeScreen, exit_code).

    exit_code is the child's poll() value AFTER the key sequence: None means
    still running (healthy), a negative value means killed by a signal -- a
    double-free aborts with SIGABRT (-6).
    """
    screen = SafeScreen(COLS, ROWS)
    stream = pyte.Stream(screen)
    master_fd, slave_fd = pty.openpty()

    env = os.environ.copy()
    env['TERM'] = 'xterm-256color'
    env['COLUMNS'] = str(COLS)
    env['LINES'] = str(ROWS)
    env['LC_ALL'] = 'en_US.UTF-8'
    env['HOME'] = workdir          # isolate from the user's ~/.xwpe config

    proc = subprocess.Popen(
        [WPE_BIN] + list(files),
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
        for k in keys:
            os.write(master_fd, k.encode())
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


@pytest.fixture
def three_files(tmp_path):
    """A workdir with three editable C files (-> three edit windows)."""
    for name in ('a.c', 'b.c', 'c.c'):
        (tmp_path / name).write_text('int %s;\n' % name[0])
    return str(tmp_path), ['a.c', 'b.c', 'c.c']


@pytest.fixture
def project_dir(tmp_path):
    """A workdir with a demo.prj so we can open a project and press F9."""
    (tmp_path / 'main.c').write_text('int main(void){return 0;}\n')
    (tmp_path / 'demo.prj').write_text(
        'CMP=\tgcc\nCMPFLAGS=\t-g\nEXENAME=\tdemo\nFILES=\tmain.c\n')
    return str(tmp_path)


def assert_alive(exit_code, action):
    assert exit_code is None, \
        '%s aborted the process (exit=%r); expected it to stay alive ' \
        '(double-free regression in the PIC view release)' % (action, exit_code)


def test_open_project_then_f9_no_crash(project_dir):
    # Alt-P P, Enter, Enter -> open demo.prj; F9 -> make. This is the path
    # (e_c_project -> e_switch_window -> repaint) that first aborted.
    _, code = run_wpe(
        project_dir, ['main.c'],
        [KEY_ALT_P, 'p', KEY_RETURN, KEY_RETURN, KEY_F9])
    assert_alive(code, 'open project + F9')


def test_tile_no_crash(three_files):
    workdir, files = three_files
    _, code = run_wpe(workdir, files, [KEY_ALT_W, MENU_TILE])
    assert_alive(code, 'Window > Tile')


def test_cascade_no_crash(three_files):
    workdir, files = three_files
    _, code = run_wpe(workdir, files, [KEY_ALT_W, MENU_CASCADE])
    assert_alive(code, 'Window > Cascade')


def test_tile_cascade_repeated_no_crash(three_files):
    workdir, files = three_files
    _, code = run_wpe(
        workdir, files,
        [KEY_ALT_W, MENU_TILE, KEY_ALT_W, MENU_CASCADE, KEY_ALT_W, MENU_TILE])
    assert_alive(code, 'repeated Tile/Cascade/Tile')


def test_tile_actually_relayouts(three_files):
    # Confirms the menu action really reaches e_ed_tile (not a no-op): after
    # Tile, the three windows share the screen, so more than one window title
    # bar is visible at once.
    workdir, files = three_files
    screen, code = run_wpe(workdir, files, [KEY_ALT_W, MENU_TILE])
    assert_alive(code, 'Window > Tile')
    visible_titles = sum(
        1 for y in range(ROWS)
        for name in ('a.c', 'b.c', 'c.c')
        if (' %s ' % name) in screen.display[y])
    assert visible_titles >= 2, \
        'Tile should show multiple window title bars at once, saw %d' \
        % visible_titles
