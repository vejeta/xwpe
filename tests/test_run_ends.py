"""Run (Ctrl-F9) on the children's owner: the program's end, Ctrl-C and its
typed keys.

The program Run starts is a child of the editor's like the build and the
servers.  Its end is reported once, from the loop, with its own exit status
however many other processes ended at the same time; Ctrl-C reaches every
process it started; and what the user types for it is queued on the loop,
so a program that stopped reading its terminal never freezes the editor.

Each program is compiled by the editor itself (Ctrl-F9 builds, then runs)
from a C source; the ones that start helpers write their PIDs, so every
check is on an exact process.
"""
import os
import time

from wpe_driver import WpeSession, WAIT_SCALE

CTRL_F9 = "\033[20;5~"            # Run
CTRL_C = "\x03"
POLL_S = 0.2
RUN_END_S = 40                    # compile, link, run and report
STOP_S = 15
GONE_S = 8
EXIT_CODE = 5
SIGINT_CODE = 130                 # 128 + SIGINT, as a shell reports it
BURST = 20                        # processes ending while the program runs
RETURN_LINE = "Return-Code: %d"
READY = "READYTOSTOP"
TYPED_BYTES = 400000              # far more than a raw terminal queues
CHUNK = 512
TYPE_S = 20                       # the longest the typing may take

# Starts twenty short processes that end during the run, waits for them and
# exits with a code of its own.
SRC_BURST = r'''
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
int main(void)
{
    int i;
    for (i = 0; i < %(burst)d; i++)
        if (fork() == 0) { usleep(20000 + 3000 * i); _exit(0); }
    while (wait(NULL) > 0)
        ;
    printf("done\n");
    return %(code)d;
}
''' % {"burst": BURST, "code": EXIT_CODE}

# Starts a second process in its own right (a later stage of a pipeline),
# writes its PID, and waits for a signal.
SRC_PIPELINE = r'''
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(void)
{
    pid_t later = fork();
    FILE *fp;
    if (later == 0) { execlp("sleep", "sleep", "300", (char *)0); _exit(127); }
    fp = fopen("later.pid", "w");
    fprintf(fp, "%%d", (int)later);
    fclose(fp);
    printf("%(ready)s\n");
    fflush(stdout);
    for (;;)
        pause();
}
''' % {"ready": READY}

# Puts its terminal in raw mode and never reads it again.
SRC_DEAF = r'''
#include <stdio.h>
#include <unistd.h>
#include <termios.h>
int main(void)
{
    struct termios t;
    tcgetattr(0, &t);
    cfmakeraw(&t);
    tcsetattr(0, TCSANOW, &t);
    printf("%(ready)s\r\n");
    fflush(stdout);
    for (;;)
        pause();
}
''' % {"ready": READY}


def _screen(w):
    return "\n".join(w.display(drain=0))


def _wait_for(w, predicate, timeout):
    deadline = time.time() + timeout * WAIT_SCALE
    while time.time() < deadline:
        w._drain(POLL_S)
        if predicate():
            return True
    return predicate()


def _pid_alive(pid):
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def _read_pid(path):
    try:
        with open(path) as fh:
            return int(fh.read().strip() or 0)
    except (OSError, ValueError):
        return 0


def _type(w, data, timeout):
    """Write @data to the editor's terminal without ever blocking the test:
    an editor that stopped reading its keys leaves the write refused.
    Return True when every byte went in within @timeout seconds."""
    deadline = time.time() + timeout * WAIT_SCALE
    while data and time.time() < deadline:
        try:
            sent = os.write(w.master_fd, data[:CHUNK])
            data = data[sent:]
        except BlockingIOError:
            pass
        w._drain(0.02)
    return not data


# @scenario SC-debug-83
def test_run_with_a_pty_reports_the_exit_status_when_the_reaper_got_there_first(tmp_path):
    """Ctrl-F9 on a program that starts twenty processes ending during the
    run and exits with code 5: Messages reports Return-Code: 5, though the
    editor's child reaper takes many ends in the same moments."""
    with WpeSession(str(tmp_path), SRC_BURST, filename="t.c", wait=1.5) as w:
        w.key(CTRL_F9, delay=0.5)
        assert _wait_for(w, lambda: RETURN_LINE % EXIT_CODE in _screen(w),
                         RUN_END_S), \
            "the program's own exit code should be reported:\n%s" % _screen(w)
        assert w.alive()


# @scenario SC-debug-84
def test_ctrl_c_during_run_ends_the_whole_pipeline(tmp_path):
    """Ctrl-C while the program and a process it started are running: both
    are gone, and Messages reports the program's end by the signal."""
    later = 0
    try:
        with WpeSession(str(tmp_path), SRC_PIPELINE, filename="t.c",
                        wait=1.5) as w:
            w.key(CTRL_F9, delay=0.5)
            assert _wait_for(w, lambda: READY in _screen(w), RUN_END_S), \
                _screen(w)
            later = _read_pid(str(tmp_path / "later.pid"))
            assert later > 0, "the program did not write the pid"
            assert _pid_alive(later)
            w.key(CTRL_C, delay=0.1)
            assert _wait_for(w, lambda: RETURN_LINE % SIGINT_CODE in
                             _screen(w), STOP_S), _screen(w)
            assert _wait_for(w, lambda: not _pid_alive(later), GONE_S), \
                "the later stage of the pipeline outlived Ctrl-C"
            assert w.alive()
    finally:
        if later > 0 and _pid_alive(later):
            os.kill(later, 9)


# @scenario SC-debug-85
def test_typing_goes_on_when_the_running_program_stops_reading(tmp_path):
    """A program that put its terminal in raw mode and never reads it, with
    far more typed for it than the terminal queues: Ctrl-C still stops it,
    because the editor never waits for the program to read."""
    with WpeSession(str(tmp_path), SRC_DEAF, filename="t.c", wait=1.5) as w:
        w.key(CTRL_F9, delay=0.5)
        assert _wait_for(w, lambda: READY in _screen(w), RUN_END_S), \
            _screen(w)
        os.set_blocking(w.master_fd, False)
        _type(w, b"a" * TYPED_BYTES, TYPE_S)
        assert _type(w, CTRL_C.encode(), TYPE_S), \
            "the editor stopped taking keys while the program was deaf"
        assert _wait_for(w, lambda: RETURN_LINE % SIGINT_CODE in _screen(w),
                         STOP_S), \
            "Ctrl-C did not reach the editor while the program was deaf:\n%s" \
            % _screen(w)
        assert w.alive()
