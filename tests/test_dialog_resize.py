"""
Dialog resize tests for wpe.

Verifies that dialogs survive terminal resize (SIGWINCH) without
crashing or corrupting the heap.  Uses TIOCSWINSZ on the pty master
to trigger resize events while a dialog is open.

Run: tests/.venv/bin/python -m pytest -v tests/test_dialog_resize.py

Root cause of the original crash: e_pr_char (macro) wrote to
schirm[] without bounds checking.  After REALLOC to smaller
dimensions, old coordinates overwrote heap metadata.
"""

import fcntl
import os
import pty
import select
import signal
import struct
import subprocess
import tempfile
import time

import pyte
import pytest

from test_utf8_border import SafeScreen

WPE_BIN = os.environ.get('WPE_BIN') or os.path.join(os.path.dirname(__file__), '..', 'wpe')

KEY_ESC = '\033\r'
KEY_CTRL_Q = '\x11'
KEY_ALT_G = '\033g'
KEY_ALT_S = '\033s'
KEY_ALT_O = '\033o'
KEY_DOWN = '\033[B'
KEY_RETURN = '\r'


def set_pty_size(fd, cols, rows):
    """Change pty dimensions via TIOCSWINSZ (sends SIGWINCH to slave)."""
    TIOCSWINSZ = 0x5414
    winsize = struct.pack('HHHH', rows, cols, 0, 0)
    fcntl.ioctl(fd, TIOCSWINSZ, winsize)


def drain(master_fd, stream, timeout):
    """Read and feed pty output to pyte stream."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        r, _, _ = select.select([master_fd], [], [], 0.05)
        if r:
            try:
                data = os.read(master_fd, 65536)
                if data:
                    stream.feed(data.decode('utf-8', errors='replace'))
            except OSError:
                break


def extract_lines(screen, rows, cols):
    """Extract visible text lines from pyte screen."""
    lines = []
    for row in range(rows):
        line = ''
        for col in range(cols):
            char = screen.buffer[row][col]
            line += char.data if char.data else ' '
        lines.append(line.rstrip())
    return lines


def screen_contains(lines, needle):
    """Check if any screen line contains the given text."""
    return any(needle in line for line in lines)


class WpeSession:
    """Manages a wpe process on a pty with resize support."""

    def __init__(self, filename, cols=80, rows=30):
        self.cols = cols
        self.rows = rows
        self.screen = SafeScreen(cols, rows)
        self.stream = pyte.Stream(self.screen)

        self.master_fd, slave_fd = pty.openpty()
        set_pty_size(self.master_fd, cols, rows)

        self.workdir = tempfile.mkdtemp(prefix='xwpe_resize_')
        filepath = os.path.join(self.workdir, filename)
        with open(filepath, 'w') as f:
            f.write('#include <stdio.h>\nint main() { return 0; }\n')

        env = os.environ.copy()
        env['TERM'] = 'xterm-256color'
        env['COLUMNS'] = str(cols)
        env['LINES'] = str(rows)
        env['LC_ALL'] = 'en_US.UTF-8'
        env['HOME'] = self.workdir

        self.proc = subprocess.Popen(
            [WPE_BIN, filename],
            stdin=slave_fd,
            stdout=slave_fd,
            stderr=slave_fd,
            cwd=self.workdir,
            env=env,
            preexec_fn=os.setsid
        )
        os.close(slave_fd)

    def drain(self, timeout=1.0):
        drain(self.master_fd, self.stream, timeout)

    def send(self, key, delay=0.3):
        os.write(self.master_fd, key.encode('utf-8'))
        self.drain(delay)

    def resize(self, cols, rows):
        self.cols = cols
        self.rows = rows
        set_pty_size(self.master_fd, cols, rows)
        self.screen = SafeScreen(cols, rows)
        self.stream = pyte.Stream(self.screen)
        self.drain(0.8)

    def lines(self):
        return extract_lines(self.screen, self.rows, self.cols)

    def alive(self):
        return self.proc.poll() is None

    def close(self):
        try:
            os.killpg(os.getpgid(self.proc.pid), signal.SIGTERM)
        except (ProcessLookupError, OSError):
            pass
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(os.getpgid(self.proc.pid), signal.SIGKILL)
            except (ProcessLookupError, OSError):
                pass
            self.proc.wait(timeout=3)
        os.close(self.master_fd)

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


class TestDialogResizeSurvival:
    """Verify dialogs survive resize without crashing."""

    def test_goto_line_resize_shrink(self):
        """Open Goto Line dialog, shrink terminal, verify no crash."""
        with WpeSession('test.c', cols=80, rows=30) as s:
            s.drain(1.5)
            assert s.alive(), "wpe should start"

            s.send(KEY_ALT_G, delay=0.5)

            s.resize(40, 15)
            assert s.alive(), "wpe must survive shrink with dialog open"

            s.send(KEY_ESC)
            assert s.alive(), "wpe must survive after closing dialog"

    def test_goto_line_resize_grow(self):
        """Open Goto Line in small terminal, grow, verify no crash."""
        with WpeSession('test.c', cols=40, rows=15) as s:
            s.drain(1.5)
            s.send(KEY_ALT_G)

            s.resize(120, 40)
            assert s.alive(), "wpe must survive grow with dialog open"

            s.send(KEY_ESC)
            assert s.alive()

    def test_goto_line_resize_multiple(self):
        """Resize multiple times rapidly with dialog open."""
        with WpeSession('test.c', cols=80, rows=30) as s:
            s.drain(1.5)
            s.send(KEY_ALT_G)

            for cols, rows in [(40, 15), (100, 40), (30, 10), (80, 30)]:
                s.resize(cols, rows)
                assert s.alive(), \
                    f"wpe crashed at resize to {cols}x{rows}"

            s.send(KEY_ESC)
            assert s.alive()

    def test_search_dialog_resize(self):
        """Open Search dialog, resize, verify survival."""
        with WpeSession('test.c', cols=80, rows=30) as s:
            s.drain(1.5)
            s.send(KEY_ALT_S)
            s.drain(0.3)
            s.send('f')
            s.drain(0.3)

            lines_before = s.lines()
            assert screen_contains(lines_before, 'Search') or \
                screen_contains(lines_before, 'Find'), \
                "Search dialog should appear"

            s.resize(40, 15)
            assert s.alive(), "wpe must survive resize with Search dialog"

            s.send(KEY_ESC)
            assert s.alive()

    def test_error_popup_resize(self):
        """Trigger error popup (compile nonexistent), resize, verify."""
        with WpeSession('nonexistent.c', cols=80, rows=30) as s:
            s.drain(1.5)
            assert s.alive()

            s.resize(40, 15)
            assert s.alive(), "wpe must survive resize"

    def test_resize_then_dialog(self):
        """Resize first, then open dialog in new dimensions."""
        with WpeSession('test.c', cols=80, rows=30) as s:
            s.drain(1.5)

            s.resize(40, 15)
            assert s.alive()

            s.send(KEY_ALT_G, delay=0.5)
            assert s.alive(), "dialog must open after resize"

            s.send(KEY_ESC)
            assert s.alive()

    def test_aggressive_shrink(self):
        """Shrink to minimum viable size with dialog open."""
        with WpeSession('test.c', cols=80, rows=30) as s:
            s.drain(1.5)
            s.send(KEY_ALT_G)

            s.resize(20, 8)
            assert s.alive(), \
                "wpe must survive aggressive shrink (20x8)"

            s.send(KEY_ESC)
            assert s.alive()

    def test_no_crash_editor_after_resize(self):
        """After resize cycle, editor must still accept input."""
        with WpeSession('test.c', cols=80, rows=30) as s:
            s.drain(1.5)

            s.send(KEY_ALT_G, delay=0.5)
            s.resize(40, 15)
            s.send(KEY_ESC)

            s.resize(80, 30)

            for ch in 'hello':
                s.send(ch, delay=0.1)
            s.drain(0.5)
            assert s.alive(), "editor must accept input after resize"


class TestEditorResizeNoCrash:
    """Verify basic editor resize without dialogs."""

    def test_plain_resize_shrink(self):
        """Shrink terminal without any dialog."""
        with WpeSession('test.c', cols=80, rows=30) as s:
            s.drain(1.5)
            s.resize(40, 15)
            assert s.alive()

    def test_plain_resize_grow(self):
        """Grow terminal without any dialog."""
        with WpeSession('test.c', cols=40, rows=15) as s:
            s.drain(1.5)
            s.resize(120, 40)
            assert s.alive()

    def test_rapid_resize_burst(self):
        """Send many resize events quickly (resize storm)."""
        with WpeSession('test.c', cols=80, rows=30) as s:
            s.drain(1.5)
            for cols, rows in [
                (60, 20), (40, 15), (30, 10), (50, 20),
                (80, 30), (100, 40), (40, 12), (80, 30)
            ]:
                s.resize(cols, rows)
            assert s.alive(), "wpe must survive resize storm"
