"""Borland "User Screen" (Alt-F5) regression tests for wpe (terminal mode).

Alt-F5 leaves the editor and replays the running program's OWN full screen --
the raw bytes it wrote to its terminal -- then waits for a keypress before
returning to the editor (e_t_user_screen, we_term.c).  Unlike the line-oriented
Messages panel this can show a program that PAINTS (cursor positioning, ANSI
colour); here we assert the simpler, deterministic case: after a Ctrl-F9 Run the
User Screen shows the program's COMPUTED output and a return prompt, and a key
returns to the editor.

This is part of the #158 console-input coverage (Ctrl-F9 Run and the Alt-F5
shortcut).  Esc-U / Alt-U (the Run accelerator) and Ctrl-F9 are covered by
tests/test_menu_run.py; this file locks in the Alt-F5 User Screen.

Run: tests/.venv/bin/python -m pytest -v tests/test_user_screen.py
Requires: gcc.
"""
import os
import pty
import select
import subprocess
import time

import pyte

from test_utf8_border import SafeScreen

WPE_BIN = os.environ.get('WPE_BIN') or os.path.join(os.path.dirname(__file__), '..', 'wpe')

CTRL_F9 = '\033[20;5~'    # kf33 -> Run
ALT_F5 = '\033\033[15~'  # Esc + F5 -> Alt-F5 (User Screen) via the Esc meta prefix

# 6*7 = 42 is computed at runtime, so "USERSCR42" exists only in the program's
# OUTPUT -- never in the source the editor shows -- proving the program ran and
# the User Screen replayed its real output.
PROG = ('#include <stdio.h>\n'
        'int main(void){ printf("USERSCR%d\\n", 6 * 7); return 0; }\n')
RAN = "USERSCR42"
PROMPT = "press any key to return"


def _run_keys(workdir, keys, settle=0.6):
    """Seed t.c with PROG, run wpe, send keys, return (screen_text, alive)."""
    with open(os.path.join(workdir, 't.c'), 'w') as fh:
        fh.write(PROG)
    screen = SafeScreen(80, 30)
    stream = pyte.Stream(screen)
    master_fd, slave_fd = pty.openpty()
    env = os.environ.copy()
    env.update(TERM='xterm-256color', COLUMNS='80', LINES='30',
               LC_ALL='en_US.UTF-8', HOME=workdir)
    proc = subprocess.Popen([WPE_BIN, 't.c'], stdin=slave_fd, stdout=slave_fd,
                            stderr=slave_fd, cwd=workdir, env=env,
                            preexec_fn=os.setsid)
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
        drain(1.3)
        for key in keys:
            os.write(master_fd, key.encode())
            drain(0.8)
        drain(settle)
        text = "\n".join(screen.display)
        alive = proc.poll() is None
    finally:
        try:
            os.killpg(os.getpgid(proc.pid), 9)
        except Exception:
            pass
        os.close(master_fd)
        proc.wait()
    return text, alive


def test_alt_f5_shows_program_user_screen(tmp_path):
    """Ctrl-F9 Run then Alt-F5 replays the program's computed output full-screen
    with a return prompt."""
    text, alive = _run_keys(str(tmp_path), [CTRL_F9, ALT_F5])
    assert alive, "wpe died on Alt-F5 (User Screen):\n%s" % text
    assert RAN in text, \
        "Alt-F5 should replay the program's computed output, got:\n%s" % text
    assert PROMPT in text.lower(), \
        "Alt-F5 User Screen should show a 'press any key to return' prompt:\n%s" % text


def test_alt_f5_key_returns_to_editor(tmp_path):
    """A keypress on the User Screen returns to the editor (prompt gone, the
    menu bar / source is back)."""
    text, alive = _run_keys(str(tmp_path), [CTRL_F9, ALT_F5, '\n'])
    assert alive, "wpe died returning from the User Screen:\n%s" % text
    assert PROMPT not in text.lower(), \
        "after a key the User Screen prompt should be gone, got:\n%s" % text
    assert ("File" in text and "Edit" in text) or "main" in text, \
        "a key should return to the editor (menu bar / source), got:\n%s" % text
