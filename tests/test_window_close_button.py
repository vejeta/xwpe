"""Title-bar close-box (mouse) for wpe (terminal mode).

Regression: the window close box returned AF3 in the default (non-CUA) key
style.  When Alt-F3 was dropped as the close shortcut (the WM/kernel grab it),
the mouse close box silently stopped closing windows -- e_edt_mouse still
handed back AF3, which no longer maps to anything.  It now returns CtrlW.

We drive the REAL mouse path: locate the close glyph (U+2715 '✕') that
e_draw_window_buttons paints in the title bar, click that exact cell, and
assert the window closed (its title and body are gone).  ncurses (mousemask
ALL_MOUSE_EVENTS, getmouse; kmous = ESC [ < -> SGR-1006) decodes the click we
feed the pty.  The press and release must be DELIVERED SEPARATELY (a real click
has a non-zero duration): a coalesced press+release in one write does not
register as a button click.

Run: tests/.venv/bin/python -m pytest -v tests/test_window_close_button.py
"""
import os
import pty
import select
import subprocess
import time

import pyte

from test_utf8_border import SafeScreen

WPE_BIN = os.environ.get('WPE_BIN') or os.path.join(os.path.dirname(__file__), '..', 'wpe')
CLOSE_GLYPH = '✕'          # the title-bar close box drawn by e_draw_window_buttons
MAX_GLYPH = '□'           # the maximize box (two cells left of close)
COLS, ROWS = 80, 30


def _sgr_press(col, row):
    """SGR-1006 left button press at 1-based (col,row)."""
    return '\033[<0;%d;%dM' % (col, row)


def _sgr_release(col, row):
    """SGR-1006 left button release at 1-based (col,row)."""
    return '\033[<0;%d;%dm' % (col, row)


class _Wpe:
    """Minimal interactive pty session: open one file, screenshot, click."""

    def __init__(self, workdir, fname='hello.c', body='int hello;\n'):
        with open(os.path.join(workdir, fname), 'w') as fh:
            fh.write(body)
        self.screen = SafeScreen(COLS, ROWS)
        self.stream = pyte.Stream(self.screen)
        self.master_fd, slave = pty.openpty()
        env = os.environ.copy()
        env.update(TERM='xterm-256color', COLUMNS=str(COLS), LINES=str(ROWS),
                   LC_ALL='en_US.UTF-8', HOME=workdir)
        self.proc = subprocess.Popen(
            [WPE_BIN, fname], stdin=slave, stdout=slave, stderr=slave,
            cwd=workdir, env=env, preexec_fn=os.setsid)
        os.close(slave)

    def drain(self, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            r, _, _ = select.select([self.master_fd], [], [], 0.1)
            if r:
                try:
                    data = os.read(self.master_fd, 65536)
                except OSError:
                    break
                if not data:
                    break
                self.stream.feed(data.decode('utf-8', 'replace'))

    def send(self, b):
        os.write(self.master_fd, b if isinstance(b, bytes) else b.encode())

    def click(self, col, row):
        """A real left click at 1-based (col,row): press, brief hold, release."""
        self.send(_sgr_press(col, row))
        self.drain(0.2)
        self.send(_sgr_release(col, row))
        self.drain(0.6)

    def find_glyph(self, glyph):
        """Return 1-based (col,row) of the first cell holding glyph, or None."""
        for y, line in enumerate(self.screen.display):
            x = line.find(glyph)
            if x >= 0:
                return (x + 1, y + 1)
        return None

    def alive(self):
        return self.proc.poll() is None

    def close(self):
        try:
            os.killpg(os.getpgid(self.proc.pid), 9)
        except Exception:
            pass
        os.close(self.master_fd)
        self.proc.wait()


def test_close_box_click_closes_window(tmp_path):
    w = _Wpe(str(tmp_path))
    try:
        w.drain(1.3)
        assert any('hello.c' in ln for ln in w.screen.display), \
            "window title did not appear:\n%s" % '\n'.join(w.screen.display)
        spot = w.find_glyph(CLOSE_GLYPH)
        assert spot, "close glyph not found in title bar:\n%s" % '\n'.join(w.screen.display)
        w.click(*spot)
        assert w.alive(), "wpe died clicking the close box"
        disp = '\n'.join(w.screen.display)
        assert 'hello.c' not in disp and 'hello' not in disp, \
            "close box should have closed the window, but it is still shown:\n%s" % disp
    finally:
        w.close()


ZOOM_GLYPH = '▣'          # maximize box turns from □ into ▣ (0x25A3) when zoomed


def test_maximize_box_click_zooms_window(tmp_path):
    """The maximize box (two cells left of close) toggles zoom: its glyph turns
    from □ into ▣, and the window stays open."""
    w = _Wpe(str(tmp_path))
    try:
        w.drain(1.3)
        spot = w.find_glyph(MAX_GLYPH)
        assert spot, "maximize glyph not found in title bar:\n%s" % '\n'.join(w.screen.display)
        w.click(*spot)
        assert w.alive(), "wpe died clicking the maximize box"
        disp = '\n'.join(w.screen.display)
        assert ZOOM_GLYPH in disp, \
            "maximize box should zoom the window (glyph -> ▣):\n%s" % disp
        assert 'hello.c' in disp, "zoom must not close the window"
    finally:
        w.close()
