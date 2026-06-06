"""Block-operation regression tests for wpe (terminal mode).

The Block menu (Alt-B) exposes the classic WordStar/Borland block commands
(^K B/K/C/V/Y/X/L/I/U ...).  These tests drive the deterministic ones --
those that need no cursor choreography -- through "Mark Whole" (Alt-B O,
^K X) and then assert on the file written to disk, which is ground truth
(the on-screen render can be captured mid-repaint, the saved file cannot).

Covered: Delete (^K Y), Move to Right / indent (^K I), Move to Left /
unindent (^K U), and an indent/unindent round trip.

Run: tests/.venv/bin/python -m pytest -v tests/test_block.py
"""

import os
import pty
import select
import subprocess
import time

import pyte

from test_utf8_border import SafeScreen

WPE_BIN = os.environ.get('WPE_BIN') or os.path.join(os.path.dirname(__file__), '..', 'wpe')

ALT_B = '\033b'          # open the Block menu
ALT_F = '\033f'          # open the File menu
MENU_MARK_WHOLE = 'o'    # Block -> Mark WhOle
MENU_DELETE = 'd'        # Block -> Delete
MENU_MOVE_RIGHT = 'i'    # Block -> Move to RIght (indent)
MENU_MOVE_LEFT = 't'     # Block -> Move to LefT (unindent)
MENU_FILE_SAVE = 's'     # File -> Save
COLS, ROWS = 80, 30


def run_block(workdir, seed, ops, wait=1.3, key_delay=0.45, settle=0.6):
    """Seed t.c, send menu ops, File->Save, and return the saved file text.

    Returns (file_text, exit_code).  exit_code is None while wpe is still
    running (healthy); a negative value means it was killed by a signal.
    """
    path = os.path.join(workdir, 't.c')
    with open(path, 'w') as fh:
        fh.write(seed)

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
        [WPE_BIN, 't.c'],
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
        for key in ops:
            os.write(master_fd, key.encode())
            drain(key_delay)
        for key in (ALT_F, MENU_FILE_SAVE):     # File -> Save
            os.write(master_fd, key.encode())
            drain(0.5)
        drain(settle)
        exit_code = proc.poll()
    finally:
        try:
            os.killpg(os.getpgid(proc.pid), 9)
        except Exception:
            pass
        os.close(master_fd)
        proc.wait()

    with open(path) as fh:
        return fh.read(), exit_code


def test_mark_whole_delete_empties_buffer(tmp_path):
    text, code = run_block(
        str(tmp_path), 'AAA\nBBB\nCCC\n',
        [ALT_B, MENU_MARK_WHOLE, ALT_B, MENU_DELETE])
    assert code is None, 'wpe died during Block Delete (exit=%r)' % code
    assert text.strip() == '', \
        'Mark Whole + Delete should empty the buffer, got %r' % text


def test_mark_whole_indent_adds_leading_space(tmp_path):
    text, code = run_block(
        str(tmp_path), 'AAA\nBBB\n',
        [ALT_B, MENU_MARK_WHOLE, ALT_B, MENU_MOVE_RIGHT])
    assert code is None, 'wpe died during Block Move-Right (exit=%r)' % code
    lines = text.splitlines()
    assert lines and all(ln.startswith(' ') and ln.strip() for ln in lines), \
        'Move to Right should indent every block line, got %r' % text


def test_mark_whole_unindent_removes_leading_space(tmp_path):
    text, code = run_block(
        str(tmp_path), '    AAA\n    BBB\n',
        [ALT_B, MENU_MARK_WHOLE, ALT_B, MENU_MOVE_LEFT])
    assert code is None, 'wpe died during Block Move-Left (exit=%r)' % code
    lines = text.splitlines()
    assert lines and all(not ln.startswith(' ') for ln in lines), \
        'Move to Left should unindent every block line, got %r' % text


def test_indent_then_unindent_round_trip(tmp_path):
    text, code = run_block(
        str(tmp_path), 'AAA\nBBB\n',
        [ALT_B, MENU_MARK_WHOLE, ALT_B, MENU_MOVE_RIGHT,
         ALT_B, MENU_MARK_WHOLE, ALT_B, MENU_MOVE_LEFT])
    assert code is None, 'wpe died during indent round trip (exit=%r)' % code
    assert text.strip() == 'AAA\nBBB'.strip() and 'AAA' in text and 'BBB' in text, \
        'indent then unindent should restore the original text, got %r' % text


# --- shortcut path (#159): the WordStar ^K block chords ------------------
# The same Block operations as above, driven by their advertised ^K two-key
# chords instead of the Alt-B menu.  The chord is a SEPARATE decode (the ^K
# prefix state machine), so an op can work from the menu while its chord is
# dead -- exactly the class of bug #159 hunts.  Same saved-file assertions.

CK = '\x0b'                       # Ctrl-K, the WordStar block prefix
CHORD_MARK_WHOLE = (CK, 'x')      # ^K X  -> Mark WhOle
CHORD_DELETE = (CK, 'y')          # ^K Y  -> Delete
CHORD_INDENT = (CK, 'i')          # ^K I  -> Move to RIght (indent)
CHORD_UNINDENT = (CK, 'u')        # ^K U  -> Move to LefT (unindent)


def test_chord_mark_whole_delete_empties_buffer(tmp_path):
    """^K X (Mark Whole) then ^K Y (Delete) empties the buffer."""
    text, code = run_block(
        str(tmp_path), 'AAA\nBBB\nCCC\n',
        [*CHORD_MARK_WHOLE, *CHORD_DELETE])
    assert code is None, 'wpe died during ^K Y Delete (exit=%r)' % code
    assert text.strip() == '', \
        '^K X + ^K Y should empty the buffer, got %r' % text


def test_chord_indent_adds_leading_space(tmp_path):
    """^K X (Mark Whole) then ^K I indents every block line."""
    text, code = run_block(
        str(tmp_path), 'AAA\nBBB\n',
        [*CHORD_MARK_WHOLE, *CHORD_INDENT])
    assert code is None, 'wpe died during ^K I indent (exit=%r)' % code
    lines = text.splitlines()
    assert lines and all(ln.startswith(' ') and ln.strip() for ln in lines), \
        '^K I should indent every block line, got %r' % text


def test_chord_unindent_removes_leading_space(tmp_path):
    """^K X (Mark Whole) then ^K U unindents every block line."""
    text, code = run_block(
        str(tmp_path), '    AAA\n    BBB\n',
        [*CHORD_MARK_WHOLE, *CHORD_UNINDENT])
    assert code is None, 'wpe died during ^K U unindent (exit=%r)' % code
    lines = text.splitlines()
    assert lines and all(not ln.startswith(' ') for ln in lines), \
        '^K U should unindent every block line, got %r' % text
