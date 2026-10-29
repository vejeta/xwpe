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
import subprocess
import time

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
    """Start wpe on t.c with ``keyboard`` as stdin and a pty as its screen.
    Returns (process, screen master fd)."""
    with open(os.path.join(workdir, "t.c"), "w") as fh:
        fh.write("")
    master, slave = pty.openpty()
    env = os.environ.copy()
    env.update(TERM="xterm-256color", LC_ALL="en_US.UTF-8", HOME=workdir,
               XDG_CONFIG_HOME="", XDG_STATE_HOME="", XWPE_LSP_NO_EAGER="1")
    env.setdefault("XWPE_LIB", XWPE_LIB_DIR)
    proc = subprocess.Popen([WPE_BIN, "t.c"], stdin=keyboard, stdout=slave,
                            stderr=slave, cwd=workdir, env=env)
    os.close(slave)
    return proc, master


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


def wait_for_exit(proc, master, seconds):
    """The exit code, or None when the process is still running."""
    deadline = time.time() + seconds
    while time.time() < deadline:
        code = proc.poll()
        if code is not None:
            return code
        drain_screen(master, 0.1)
    return None


def stop(proc, master):
    """Remove a process this test started and is sure is its own."""
    if proc.poll() is None:
        proc.kill()
        proc.wait()
    os.close(master)


# @scenario SC-input-eof-01
def test_wpe_exits_when_its_stdin_is_at_end_of_file(tmp_path):
    proc, master = start_wpe(str(tmp_path), subprocess.DEVNULL)
    try:
        code = wait_for_exit(proc, master, EXIT_DEADLINE_S)
        assert code is not None, "wpe still running %ds after its input ended" % EXIT_DEADLINE_S
        assert code != 0
    finally:
        stop(proc, master)


# @scenario SC-input-eof-02
def test_typed_work_is_kept_when_the_input_ends_after_it(tmp_path):
    read_end, write_end = os.pipe()
    proc, master = start_wpe(str(tmp_path), read_end)
    os.close(read_end)
    try:
        drain_screen(master, 1.5 * WAIT_SCALE)
        os.write(write_end, TYPED.encode())
        drain_screen(master, TYPING_SETTLE_S)
        os.close(write_end)
        write_end = None
        code = wait_for_exit(proc, master, EXIT_DEADLINE_S)
        assert code is not None, "wpe still running after its input ended"
        saved = tmp_path / "t.c.ESV"
        assert saved.exists(), sorted(p.name for p in tmp_path.iterdir())
        assert TYPED in saved.read_text()
    finally:
        if write_end is not None:
            os.close(write_end)
        stop(proc, master)


# @scenario SC-input-eof-03
def test_an_idle_wpe_with_a_live_keyboard_uses_no_cpu(tmp_path):
    """The check for the end of input must not mistake a quiet keyboard for
    a closed one: a pipe nobody writes to, and nobody has closed, leaves the
    editor running and idle."""
    read_end, write_end = os.pipe()
    proc, master = start_wpe(str(tmp_path), read_end)
    os.close(read_end)
    try:
        drain_screen(master, 1.5 * WAIT_SCALE)
        assert proc.poll() is None
        before = cpu_seconds(proc.pid)
        drain_screen(master, IDLE_WATCH_S)
        assert proc.poll() is None, "wpe left although its keyboard is open"
        used = cpu_seconds(proc.pid) - before
        assert used < IDLE_CPU_BUDGET_S, "idle editor used %.2fs of CPU in %.1fs" % (used, IDLE_WATCH_S)
    finally:
        os.close(write_end)
        stop(proc, master)


def cpu_seconds(pid):
    """User plus system CPU time of ``pid``, read from ps(1) so the row
    also runs on macOS and the BSDs, which have no Linux /proc."""
    out = subprocess.run(["ps", "-o", "time=", "-p", str(pid)],
                         capture_output=True, text=True).stdout.strip()
    return _ps_time_seconds(out)


def _ps_time_seconds(text):
    """Seconds in a ps(1) TIME field: [[dd-]hh:]mm:ss[.frac]."""
    days = 0
    if "-" in text:
        day_text, text = text.split("-", 1)
        days = int(day_text)
    seconds = 0.0
    for part in text.split(":"):
        seconds = seconds * 60 + float(part)
    return days * 86400 + seconds
