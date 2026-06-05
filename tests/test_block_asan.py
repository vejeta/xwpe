"""Memory-safety guards for block operations, run under AddressSanitizer.

Why this file is separate from test_block.py: a heap-buffer-overflow (like
the Ctrl-K V block-move bug, an original Kruse-era off-by-one in
e_ins_nchar / e_move_block) is undefined behaviour, NOT a deterministic
crash.  On the normal build the process usually survives with corrupted
memory, so a pyte "stays alive" assertion passes and the bug is invisible.
It only becomes a hard, reproducible failure under AddressSanitizer.

So these tests drive block Copy/Move with pyte and assert that the ASan
build produces NO report -- pyte supplies the input, ASan watches the heap.
They require the sanitizer binary `we-asan`; build it with:

    make clean
    make CFLAGS="-fsanitize=address -g -O1 -fno-omit-frame-pointer" \\
         LDFLAGS="-fsanitize=address"
    cp we we-asan && make clean && make

If `we-asan` is absent the tests skip.  See HACKING.md ("Debugging crashes
and memory bugs") and the v1.6.3 autopkgtest plan (tasks #111/#112) for
running the whole suite under a sanitizer in CI.

Run: tests/.venv/bin/python -m pytest -v tests/test_block_asan.py
"""

import glob
import os
import pty
import select
import subprocess
import time

import pyte
import pytest

from test_utf8_border import SafeScreen

ASAN_BIN = os.path.join(os.path.dirname(__file__), '..', 'we-asan')

CK = '\x0b'          # Ctrl-K prefix
DOWN = '\033[B'
HOME = '\033[H'
ALT_B = '\033b'
B_MARK_WHOLE = 'o'
B_MOVE = 'm'
B_COPY = 'c'
COLS, ROWS = 80, 30

pytestmark = pytest.mark.skipif(
    not os.path.exists(ASAN_BIN),
    reason='we-asan (AddressSanitizer build) not present; see module docstring')


def run_asan(workdir, seed, keys, wait=1.3, key_delay=0.4, settle=0.8):
    """Run we-asan, send keys, return (asan_report_text_or_None, exit_code).

    ASan writes its report to <workdir>/asan.<pid> (log_path) so the TUI
    clobbering stderr does not lose it.
    """
    with open(os.path.join(workdir, 't.c'), 'w') as fh:
        fh.write(seed)
    log_prefix = os.path.join(workdir, 'asan')

    screen = SafeScreen(COLS, ROWS)
    stream = pyte.Stream(screen)
    master_fd, slave_fd = pty.openpty()

    env = os.environ.copy()
    env['TERM'] = 'xterm-256color'
    env['COLUMNS'] = str(COLS)
    env['LINES'] = str(ROWS)
    env['LC_ALL'] = 'en_US.UTF-8'
    env['HOME'] = workdir
    env['ASAN_OPTIONS'] = (
        'detect_leaks=0:abort_on_error=1:log_path=%s' % log_prefix)

    proc = subprocess.Popen(
        [ASAN_BIN, 't.c'],
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

    reports = glob.glob(log_prefix + '.*')
    text = None
    if reports:
        with open(reports[0]) as fh:
            text = fh.read()
    return text, exit_code


def assert_no_asan(report, action):
    assert report is None, \
        '%s triggered AddressSanitizer:\n%s' % (action, report[:1500])


def test_block_move_multiline_no_overflow(tmp_path):
    # The original crash: mark a multi-line block (^K B ... ^K K) and move it
    # (^K V).  e_move_block's multi-line path is where the line-buffer
    # overflow lived.
    report, _ = run_asan(
        str(tmp_path), 'AAA\nBBB\nCCC\nDDD\nEEE\n',
        [HOME, CK, 'b', DOWN, DOWN, CK, 'k', DOWN, DOWN, CK, 'v'])
    assert_no_asan(report, 'Ctrl-K multiline mark + move')


def test_block_move_markwhole_no_overflow(tmp_path):
    report, _ = run_asan(
        str(tmp_path), 'AAA\nBBB\nCCC\n',
        [ALT_B, B_MARK_WHOLE, DOWN, DOWN, ALT_B, B_MOVE])
    assert_no_asan(report, 'Mark Whole + Move')


def test_block_copy_markwhole_no_overflow(tmp_path):
    report, _ = run_asan(
        str(tmp_path), 'AAA\nBBB\nCCC\n',
        [ALT_B, B_MARK_WHOLE, DOWN, DOWN, ALT_B, B_COPY])
    assert_no_asan(report, 'Mark Whole + Copy')
