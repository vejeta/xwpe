"""Max-Columns + cut/paste crash regression (#87, a Payne-era bug).

The 1993 code kept lines in fixed buffers sized from the Max Columns setting;
changing Max Columns and then cutting/pasting lines longer than the new limit
could overrun those buffers and crash. The SCREENCELL migration (1.6.0)
replaced the fixed buffers; this guards that the crash stays gone: set a tiny
Max Columns and hammer the line with block copy, clipboard paste and typing
past the limit -- xwpe must stay alive.

Run: tests/.venv/bin/python -m pytest -v tests/test_maxcol_paste.py
"""

import os
import pty
import select
import subprocess
import time

import pyte

from test_utf8_border import SafeScreen

WPE_BIN = os.path.join(os.path.dirname(__file__), '..', 'wpe')
COLS, ROWS = 80, 30

# Editor block keys (Old-Style / WordStar, the default).
CK_BEGIN = '\x0bb'     # Ctrl-K B  mark block begin
CK_END = '\x0bk'       # Ctrl-K K  mark block end
CK_COPY = '\x0bc'      # Ctrl-K C  copy block to cursor
CLIP_COPY = '\x03'     # Ctrl-C    copy to clipboard
CLIP_PASTE = '\x16'    # Ctrl-V    paste clipboard
DOWN = '\033[B'
SET_MAXCOL_8 = ['\033o', 'e', '\033m', '\x7f\x7f\x7f', '8', '\r']


def test_small_maxcolumns_paste_does_not_crash(tmp_path):
    (tmp_path / 'big.c').write_text('x' * 70 + '\n' + 'second line\n')

    screen = SafeScreen(COLS, ROWS)
    stream = pyte.Stream(screen)
    master_fd, slave_fd = pty.openpty()
    env = os.environ.copy()
    env.update(TERM='xterm-256color', COLUMNS=str(COLS), LINES=str(ROWS),
               LC_ALL='en_US.UTF-8', HOME=str(tmp_path))
    proc = subprocess.Popen(
        [WPE_BIN, 'big.c'], stdin=slave_fd, stdout=slave_fd, stderr=slave_fd,
        cwd=str(tmp_path), env=env, preexec_fn=os.setsid)
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

    def send(keys, t=0.3):
        for k in keys:
            os.write(master_fd, k.encode())
            drain(t)

    try:
        drain(1.5)
        send(SET_MAXCOL_8, 0.4)            # Max Columns -> 8
        # Mark the long first line (+ next) as a block and copy it repeatedly.
        send([CK_BEGIN, DOWN, CK_END])
        send([CK_COPY] * 5)
        # Clipboard copy then paste many times -> a line far over Max Columns.
        send([CLIP_COPY])
        send([CLIP_PASTE] * 6)
        # Type past the limit on a fresh line.
        send(['\r', 'y' * 40], 0.5)
        drain(0.4)

        assert proc.poll() is None, \
            'xwpe crashed during Max-Columns + cut/paste stress (#87)'
        # ...and it is still a live editor, not a frozen/garbage screen.
        assert any('File' in line and 'Edit' in line
                   for line in screen.display), \
            'editor menu bar gone after the stress -- xwpe is not responsive'
    finally:
        try:
            os.killpg(os.getpgid(proc.pid), 9)
        except Exception:
            pass
        os.close(master_fd)
        proc.wait()
