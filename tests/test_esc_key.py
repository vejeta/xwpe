"""Esc-key regression test for wpe (terminal mode).

#141: a lone Esc had to be pressed several times because e_t_getch always
blocked on a second read after Esc (to detect Alt-<key> combos). The second
read is now time-limited (ESC_ALT_DELAY_MS): if no byte follows, Esc registers
on the first press. Alt-<key> combos (ESC+key sent together) must still work.

Run: tests/.venv/bin/python -m pytest -v tests/test_esc_key.py
"""

import os
import pty
import select
import subprocess
import time

import pyte
import pytest

from test_utf8_border import SafeScreen
from wpe_driver import MACOS

WPE_BIN = os.environ.get('WPE_BIN') or os.path.join(os.path.dirname(__file__), '..', 'wpe')
COLS, ROWS = 80, 30


def _run(workdir, keys):
    screen = SafeScreen(COLS, ROWS)
    stream = pyte.Stream(screen)
    master_fd, slave_fd = pty.openpty()
    env = os.environ.copy()
    env.update(TERM='xterm-256color', COLUMNS=str(COLS), LINES=str(ROWS),
               LC_ALL='en_US.UTF-8', HOME=workdir)
    proc = subprocess.Popen(
        [WPE_BIN, 'e.c'], stdin=slave_fd, stdout=slave_fd, stderr=slave_fd,
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
        drain(1.5)
        for k in keys:
            os.write(master_fd, k.encode())
            drain(0.5)
        drain(0.4)
        return screen
    finally:
        try:
            os.killpg(os.getpgid(proc.pid), 9)
        except Exception:
            pass
        os.close(master_fd)
        proc.wait()


def _menu_open(screen):
    # "Save As" is a File-menu dropdown item, only visible while it is open.
    return any('Save As' in line for line in screen.display)


@pytest.fixture
def workdir(tmp_path):
    (tmp_path / 'e.c').write_text('int main(void){return 0;}\n')
    return str(tmp_path)


def test_alt_key_still_opens_menu(workdir):
    screen = _run(workdir, ['\033f'])           # Alt-F (ESC+f together)
    assert _menu_open(screen), 'Alt-F must still open the File menu'


def test_lone_esc_closes_menu_in_one_press(workdir):
    screen = _run(workdir, ['\033f', '\033'])    # open menu, then a single ESC
    assert not _menu_open(screen), \
        'a single Esc must close the menu (no second press / no blocking)'


def test_lone_esc_in_editor_is_a_noop(workdir):
    """Borland/Turbo Vision faithful: a bare Esc in the editor (nothing open)
    does nothing -- F10 opens the menu, not Esc.  So the editor stays active and
    text typed right after still lands in the buffer (and the Esc itself is not
    inserted).  Before this, Esc shared F10's case and jumped into the menu bar,
    so the following keystrokes were swallowed."""
    screen = _run(workdir, ['\033', 'Z', 'Z', 'Z'])   # bare Esc, then type
    body = "\n".join(screen.display)
    assert not _menu_open(screen), 'a bare Esc must NOT enter the menu'
    assert 'ZZZ' in body, \
        'after a bare Esc the editor must still accept input (Esc was not a ' \
        'no-op, or it entered menu mode and ate the keys):\n' + body


def test_f10_still_enters_the_menu(workdir):
    """The other half of the Borland model: F10 DOES enter the menu bar.  Proven
    by contrast with the Esc no-op -- keys typed after F10 are consumed by menu
    mode (not inserted), whereas after Esc they land in the buffer.

    macOS reserves F10 at the OS layer, so there the menu bar is entered with
    Alt-<letter> (the supported path); an open menu likewise swallows the
    following non-hotkey keys, so the same contrast holds."""
    enter_menu = '\033f' if MACOS else '\033[21~'   # Alt-F on macOS, else F10
    screen = _run(workdir, [enter_menu, 'Z', 'Z', 'Z'])
    assert 'ZZZ' not in "\n".join(screen.display), \
        'Entering the menu must swallow the following keys, but they were typed ' \
        'into the buffer -- menu entry no longer works'
