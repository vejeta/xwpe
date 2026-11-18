"""The terminal editor when its keyboard input ends.

`wpe file </dev/null` used to read "no key" again and again with no pause and
use a whole CPU until someone killed it.  Like nano and vim, the editor now
treats the end of its input as the end of the session: it restores the
terminal, writes every open buffer to its .ESV file (which `wpe -r` brings
back) and exits.

These tests start wpe with a pty for its screen and something else for its
keyboard, so the keyboard can end while the screen stays alive.
"""
import os
import pty
import time

import gui_owner
import procinfo
from wpe_driver import WPE_BIN, WAIT_SCALE, XWPE_LIB_DIR

# The editor must be gone this long after its input ended.
EXIT_DEADLINE_S = 10 * WAIT_SCALE
# Seconds of CPU the editor may use while it waits for input that never
# comes.  A healthy idle editor uses a few hundredths; the spin used one
# per second.
IDLE_CPU_BUDGET_S = 0.5
IDLE_WATCH_S = 2.0
# How long typed keys get to reach the buffer before the input is closed.
TYPING_SETTLE_S = 2.0 * WAIT_SCALE
TYPED = "hello eof"


def start_wpe(workdir, keyboard):
    """Start wpe on t.c with the descriptor ``keyboard`` as stdin and a pty
    as its screen.  The owner starts it, so it is ended with the row.
    Returns (owned process, screen master fd)."""
    with open(os.path.join(workdir, "t.c"), "w") as fh:
        fh.write("")
    master, slave = pty.openpty()
    owned = gui_owner.Terminal.start(
        gui_owner.current(), WPE_BIN, ["t.c"], workdir, workdir, master, slave,
        stdin_fd=keyboard, extra_env={"XWPE_LIB": XWPE_LIB_DIR})
    os.close(slave)
    return owned, master


def drain_screen(master, seconds):
    """Read and discard the screen output so the editor never blocks on it."""
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            os.set_blocking(master, False)
            os.read(master, 65536)
        except (BlockingIOError, OSError):
            pass
        time.sleep(0.05)


def wait_for_exit(owned, master, seconds):
    """The exit code, or None when the process is still running."""
    deadline = time.time() + seconds
    while time.time() < deadline:
        code = owned.proc.poll()
        if code is not None:
            return code
        drain_screen(master, 0.1)
    return None


def stop(owned, master):
    """End a process this test started (the owner ends what is still
    running, and what it left) and close its screen."""
    owned.end_for_row()
    os.close(master)


# @scenario SC-input-eof-01
def test_wpe_exits_when_its_stdin_is_at_end_of_file(tmp_path):
    keyboard = os.open(os.devnull, os.O_RDONLY)
    try:
        owned, master = start_wpe(str(tmp_path), keyboard)
    finally:
        os.close(keyboard)
    try:
        code = wait_for_exit(owned, master, EXIT_DEADLINE_S)
        assert code is not None, "wpe still running %ds after its input ended" % EXIT_DEADLINE_S
        assert code != 0
    finally:
        stop(owned, master)


# @scenario SC-input-eof-02
def test_typed_work_is_kept_when_the_input_ends_after_it(tmp_path):
    read_end, write_end = os.pipe()
    owned, master = start_wpe(str(tmp_path), read_end)
    os.close(read_end)
    try:
        drain_screen(master, 1.5 * WAIT_SCALE)
        os.write(write_end, TYPED.encode())
        drain_screen(master, TYPING_SETTLE_S)
        os.close(write_end)
        write_end = None
        code = wait_for_exit(owned, master, EXIT_DEADLINE_S)
        assert code is not None, "wpe still running after its input ended"
        saved = tmp_path / "t.c.ESV"
        assert saved.exists(), sorted(p.name for p in tmp_path.iterdir())
        assert TYPED in saved.read_text()
    finally:
        if write_end is not None:
            os.close(write_end)
        stop(owned, master)


# @scenario SC-input-eof-03
def test_an_idle_wpe_with_a_live_keyboard_uses_no_cpu(tmp_path):
    """The check for the end of input must not mistake a quiet keyboard for
    a closed one: a pipe nobody writes to, and nobody has closed, leaves the
    editor running and idle."""
    read_end, write_end = os.pipe()
    owned, master = start_wpe(str(tmp_path), read_end)
    proc = owned.proc
    os.close(read_end)
    try:
        drain_screen(master, 1.5 * WAIT_SCALE)
        assert proc.poll() is None
        before = procinfo.cpu_seconds(proc.pid)
        drain_screen(master, IDLE_WATCH_S)
        assert proc.poll() is None, "wpe left although its keyboard is open"
        used = procinfo.cpu_seconds(proc.pid) - before
        assert used < IDLE_CPU_BUDGET_S, "idle editor used %.2fs of CPU in %.1fs" % (used, IDLE_WATCH_S)
    finally:
        os.close(write_end)
        stop(owned, master)
