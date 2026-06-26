"""Run-menu (Alt-R) keyboard-shortcut coverage for wpe (terminal mode).

Run items and their advertised accelerators (we_menue.c):
  Compile  Alt-F9 / Alt-C   Make  F9 / Alt-M   Run  ^F9 / Alt-U
  Next Error  Alt-F8 / Alt-T   Previous Error  Alt-F7 / Alt-V

This is the #159 shortcut path -- and the Run menu is exactly where the X11/
terminal mapping bugs lived: the menu item hotkeys are R/M/C, but the advertised
accelerators are Alt-U/Alt-M/Alt-C, which dispatch through e_prog_switch (a
DIFFERENT path from the menu).  Alt-U "Run" was a no-op until that fallback was
wired up.  We verify by RUNNING a program whose output is computed at runtime,
so the marker exists only if the program actually compiled, linked and ran.

Run: tests/.venv/bin/python -m pytest -v tests/test_menu_run.py
Requires: gcc.
"""
import os
import pty
import select
import subprocess
import time

import pyte

from test_utf8_border import SafeScreen
from wpe_driver import macos_key_route

WPE_BIN = os.environ.get('WPE_BIN') or os.path.join(os.path.dirname(__file__), '..', 'wpe')

CTRL_F9 = '\033[20;5~'    # kf33 -> Run
ALT_U = '\033u'          # advertised Run accelerator (hotkey is 'R', not 'U')
F9 = '\033[20~'          # kf9 -> Make
ALT_M = '\033m'          # advertised Make accelerator

# 6*7 = 42 is computed at runtime, so "XQZ42" is in the program OUTPUT only,
# never in the source the editor shows.
PROG = ('#include <stdio.h>\n'
        'int main(void){ printf("XQZ%d\\n", 6 * 7); return 0; }\n')
RAN = "XQZ42"


def _session(workdir, fname='t.c'):
    with open(os.path.join(workdir, fname), 'w') as fh:
        fh.write(PROG)
    screen = SafeScreen(80, 30)
    stream = pyte.Stream(screen)
    master_fd, slave = pty.openpty()
    env = os.environ.copy()
    env.update(TERM='xterm-256color', COLUMNS='80', LINES='30',
               LC_ALL='en_US.UTF-8', HOME=workdir)
    proc = subprocess.Popen([WPE_BIN, fname], stdin=slave, stdout=slave,
                            stderr=slave, cwd=workdir, env=env,
                            preexec_fn=os.setsid)
    os.close(slave)
    return proc, master_fd, screen, stream


def _drain(master_fd, stream, timeout):
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


def _run_keys(workdir, keys, settle=3.0):
    proc, fd, screen, stream = _session(workdir)
    try:
        _drain(fd, stream, 1.0)
        for k in keys:
            for stroke in macos_key_route(k):   # F9 -> Run->Make on macOS; identity on Linux
                os.write(fd, stroke.encode())
                _drain(fd, stream, 0.5)
        _drain(fd, stream, settle)
        ran = any(RAN in line for line in screen.display)
        alive = proc.poll() is None
        return ran, alive, screen
    finally:
        try:
            os.killpg(os.getpgid(proc.pid), 9)
        except Exception:
            pass
        os.close(fd)
        proc.wait()


def test_run_via_ctrl_f9(tmp_path):
    """Ctrl-F9 compiles, links and runs -- the computed output appears."""
    ran, alive, screen = _run_keys(str(tmp_path), [CTRL_F9])
    assert alive, "wpe died on Ctrl-F9"
    assert ran, "Ctrl-F9 did not run the program:\n%s" % "\n".join(screen.display)


def test_run_via_alt_u(tmp_path):
    """Alt-U (advertised Run accelerator, hotkey != 'U') runs from the editor
    via the e_prog_switch fallback -- no menu opened."""
    ran, alive, screen = _run_keys(str(tmp_path), [ALT_U])
    assert alive, "wpe died on Alt-U"
    assert ran, "Alt-U did not run the program:\n%s" % "\n".join(screen.display)


def test_make_via_f9_builds_executable(tmp_path):
    """F9 (Make) compiles and links -- the executable appears on disk."""
    proc, fd, screen, stream = _session(str(tmp_path))
    try:
        _drain(fd, stream, 1.0)
        for stroke in macos_key_route(F9):   # F9 -> Run->Make on macOS; identity on Linux
            os.write(fd, stroke.encode())
            _drain(fd, stream, 4.0)
        assert proc.poll() is None, "wpe died on F9 (Make)"
        # xwpe builds "<basename>.e" next to the source (gcc -o ./t.e ./t.o).
        exe = os.path.join(str(tmp_path), 't.e')
        built = os.path.exists(exe) and os.access(exe, os.X_OK)
        assert built, "F9 (Make) should build the executable 't.e', dir=%s" % \
            os.listdir(str(tmp_path))
    finally:
        try:
            os.killpg(os.getpgid(proc.pid), 9)
        except Exception:
            pass
        os.close(fd)
        proc.wait()
