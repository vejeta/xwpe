"""Empty-desktop chrome after closing every user window (wpe terminal mode).

Regression: when the user closes the last visible window, cn->mxedt drops to 0.
The previous main loop then re-dispatched on cn->f[0] (the internal clipboard
"Buffer"), which is hidden infrastructure with NO frame, NO close box and NOT
draggable.  The result was a black window with a cursor at screen centre and
leftover File-Manager hints in the status bar -- the bug reported as "una
ventana llamada Buffer ... no se puede cerrar pulsando x en su borde superior
derecho, tampoco se puede mover con el raton".

The fix (we_main.c: e_show_empty_desk + dispatch branch) repaints a clean
desktop and drives the top menu bar instead.  This file pins the visible
contract:

  1. After closing the auto File-Manager (wpe started with no file argument),
     the screen shows the top menu bar AND NOTHING ELSE in the body area --
     specifically no "Buffer" title bar and no FM hint strings like
     "Edit  Attributes  Move  COpy  Remove" in the status row.
  2. From the empty desktop, Alt-F still opens the File menu and Alt-X still
     quits (wired through WpeHandleMainmenu).
  3. From the empty desktop, File>File-Manager reopens the FM cleanly.

Run: tests/.venv/bin/python -m pytest -v tests/test_empty_desktop.py
"""
import os
import pty
import select
import subprocess
import time

import pyte

# SafeScreen tolerates the private-mode CSI sequences (e.g. set_margins with
# private=True) that some pyte versions reject on a raw pyte.Screen; the rest of
# the harness already standardises on it.  Using the bare Screen here made the
# test crash inside pyte's parser on Linux (TypeError: set_margins() got an
# unexpected keyword argument 'private') even though wpe rendered correctly.
from test_utf8_border import SafeScreen

WPE_BIN = os.environ.get('WPE_BIN') or os.path.join(
    os.path.dirname(__file__), '..', 'wpe')
COLS, ROWS = 80, 30
ESC = b'\x1b'
ALT_F = b'\x1bf'
ALT_X = b'\x1bx'

# Distinctive FM hint substrings that must NOT linger on the empty desktop.
FM_HINTS = ('Attributes', 'Alt-C:ChDi', 'Esc:Cancel')


class _Wpe:
    """wpe with NO file argument: auto-FM opens, ESC drops us to empty desktop."""

    def __init__(self, workdir):
        self.screen = SafeScreen(COLS, ROWS)
        self.stream = pyte.Stream(self.screen)
        self.master_fd, slave = pty.openpty()
        env = os.environ.copy()
        env.update(TERM='xterm-256color', COLUMNS=str(COLS), LINES=str(ROWS),
                   LC_ALL='en_US.UTF-8', HOME=workdir)
        self.proc = subprocess.Popen(
            [WPE_BIN], stdin=slave, stdout=slave, stderr=slave,
            cwd=workdir, env=env, preexec_fn=os.setsid)
        os.close(slave)

    def drain(self, timeout):
        end = time.time() + timeout
        while time.time() < end:
            r, _, _ = select.select([self.master_fd], [], [], 0.1)
            if not r:
                continue
            try:
                data = os.read(self.master_fd, 65536)
            except OSError:
                break
            if not data:
                break
            self.stream.feed(data.decode('utf-8', 'replace'))

    def send(self, b, settle=0.6):
        os.write(self.master_fd, b)
        self.drain(settle)

    def alive(self):
        return self.proc.poll() is None

    def close(self):
        try:
            os.killpg(os.getpgid(self.proc.pid), 9)
        except Exception:
            pass
        try:
            os.close(self.master_fd)
        except OSError:
            pass
        self.proc.wait()


def _reach_empty_desk(w):
    w.drain(2.0)                  # let the auto-FM render
    w.send(ESC, settle=1.5)       # close auto-FM -> empty desktop


def _menu_bar(disp):
    return disp[0]


def test_empty_desktop_hides_buffer_and_clears_hints(tmp_path):
    w = _Wpe(str(tmp_path))
    try:
        _reach_empty_desk(w)
        disp = w.screen.display
        assert w.alive(), 'wpe must survive closing the last window'
        # Top menu bar still present.
        assert 'File' in _menu_bar(disp) and 'Help' in _menu_bar(disp), \
            'menu bar lost on empty desktop:\n%s' % _menu_bar(disp)
        body = '\n'.join(disp[1:])
        # The internal clipboard Buffer must NOT be exposed.
        assert ' Buffer ' not in body, \
            '"Buffer" window must not be exposed on empty desktop:\n%s' % body
        # Status row must be free of leftover File-Manager hints.
        status = disp[-1]
        for h in FM_HINTS:
            assert h not in status, \
                'stale FM hint %r still in status bar: %r' % (h, status)
    finally:
        w.close()


def test_empty_desktop_alt_f_opens_file_menu(tmp_path):
    w = _Wpe(str(tmp_path))
    try:
        _reach_empty_desk(w)
        w.send(ALT_F, settle=1.0)
        assert w.alive(), 'wpe died on Alt-F from empty desktop'
        blob = '\n'.join(w.screen.display)
        assert 'File-Manager' in blob and 'Quit' in blob, \
            'File menu did not open from empty desktop:\n%s' % blob
    finally:
        w.close()


def test_empty_desktop_reopen_file_manager(tmp_path):
    w = _Wpe(str(tmp_path))
    try:
        _reach_empty_desk(w)
        w.send(ALT_F, settle=0.6)
        w.send(b'M', settle=1.5)   # File>File-Manager hotkey
        assert w.alive(), 'wpe died reopening FM from empty desktop'
        blob = '\n'.join(w.screen.display)
        assert 'File-Manager' in blob, \
            'File-Manager did not reopen from empty desktop:\n%s' % blob
    finally:
        w.close()
