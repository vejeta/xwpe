#!/usr/bin/env python3
"""job_cut.py -- run a command and cut it after its minutes, keeping what it did.

  python3 tests/harness/job_cut.py --after 100m [--grace 120] [--signal INT|KILL]
          [--report OUT.json] -- COMMAND...

The one owner of "a job that ran out of minutes".  A job that is cut has to
end, keep what it finished (pytest writes its junit when it is interrupted, never
when it is killed) and say how it ended.  The program that does this was the
`timeout` of the image, whose grace differs from system to system: on the
runner it ended its grace at once, so the interrupted run never got its
seconds and left no junit.  This tool does the same job the same way on every
system, with the standard library only, so the build job can run it before any
virtual environment exists.

What a run gets:

- J1  COMMAND starts with SIGINT, SIGTERM and SIGHUP at their defaults and an
  empty signal mask, in a process group of its own, whatever this tool
  inherited.  A parent that ignored an interrupt (a command started with `&`
  does) cannot take the cut away from it.
- J2  The cut is sent to COMMAND's pid only.  Under pytest-xdist that is the
  controller; it leaves its loop, writes the junit and ends its workers
  itself, so no row in flight is written as crashed and no worker is cloned.
- J3  COMMAND has `--grace` seconds after the cut.  If it has not ended, the
  report names what did not (J5), and then every descendant of this tool is
  killed.
- J4  The exit status is the same on every system: COMMAND's own when it ended
  before the deadline (128 plus the signal when a signal ended it), 124 for a
  cut that ended inside the grace, 137 for a cut that was killed, 125 when this
  tool failed, 126 and 127 as `env` has them.
- J5  A killed run is named: the pid, name, state and the ignored, blocked and
  caught signal sets of every process of the run are written to the report,
  and SIGUSR1 goes to the run so a process that registered a stack dump
  (tests/conftest.py) writes every thread's stack to its log.  Where /proc
  is missing the sets are reported as not readable.
- J6  SIGTERM, SIGINT or SIGHUP sent to this tool (a cancelled job, a step
  limit) is a cut with the same grace.  Best effort: the runner escalates a
  cancel within seconds.
- J7  No sleep and no poll: one `select` on the wakeup pipe of the signals and
  on the FIFO named by XWPE_JOB_CUT_NOW, with the time left as its timeout.
  A byte written to that FIFO is the deadline arriving now (the rows' event;
  a job never writes it).  COMMAND does not inherit the variable.
- J8  A failure of this tool kills the run before it exits 125, and COMMAND
  has PR_SET_PDEATHSIG(SIGKILL) on Linux, so a tool that is itself killed
  does not leave a run with no deadline.  The limit: that covers COMMAND
  only.  When this tool is killed with SIGKILL, COMMAND dies, but a process
  COMMAND started in its own group, and one that left it with setsid, are not
  ended; they are reparented to the user's subreaper or to init and run on.
  The tool is killed only by the runner's own cancel, which sends SIGTERM
  first (J6), so the limit is a SIGKILL of the tool itself.

Descendants: on Linux this tool is a child subreaper, so a descendant that
left COMMAND's process group (the editors and display servers of a test are
each in a group of their own) and was orphaned comes back to it; the kill
walks the process table for every descendant and repeats until none is left.
Without a subreaper (macOS, the BSDs) the walk alone is made, from the table
as it was before the first kill.

The report (--report, written on every end) is JSON: cut, killed, cut_by,
deadline_s, elapsed_s, exit, command_exit, command, and, when the run was
killed, "stuck": one entry per process, and, when a process was still there
after the kill's bound (KILL_BOUND_S), "unkilled": its pids.

Exit 2 with the usage for a duration that is not `<n>s`, `<n>m` or a bare
number of seconds, or is zero for --after: a run is never started with no
deadline.
"""
import argparse
import ctypes
import json
import os
import re
import select
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
try:
    import procinfo  # stdlib only; the one reader of the process table
finally:
    sys.path.pop(0)

CUT_EXIT = 124
KILL_EXIT = 137
TOOL_FAILED_EXIT = 125
CANNOT_RUN_EXIT = 126
NOT_FOUND_EXIT = 127
USAGE_EXIT = 2
SIGNAL_EXIT_BASE = 128

DEFAULT_GRACE_S = 120
# The wait for a run to answer the stack-dump signal before it is killed.
STACK_DUMP_WAIT_S = 2
# The most seconds the kill takes: rounds of "find every descendant, kill it,
# reap" repeat until none is left or this passes, and what is left is named in
# the report.  A run that keeps forking while it is killed ends well inside it
# (each round ends a generation); nothing in the kill waits without a bound.
KILL_BOUND_S = 30
# The most the kill waits for a SIGCHLD between two rounds.
KILL_ROUND_WAIT_S = 1
NOW_FIFO_VARIABLE = "XWPE_JOB_CUT_NOW"
PR_SET_PDEATHSIG = 1
PR_SET_CHILD_SUBREAPER = 36
DURATION_PATTERN = re.compile(r"^([0-9]+)([sm]?)$")
SECONDS_PER_MINUTE = 60
READ_CHUNK = 256

# What ended a wait.
ENDED, DEADLINE, NOW, SIGNALLED = "ended", "deadline", "now", "signalled"
# The signals that mean "cut now" when they reach this tool (J6).
CUT_SIGNALS = (signal.SIGTERM, signal.SIGINT, signal.SIGHUP)
# What COMMAND gets back at its default, whatever this tool inherited (J1).
DEFAULT_SIGNALS = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP,
                   signal.SIGQUIT, signal.SIGCHLD, signal.SIGPIPE)
SIGNAL_SETS = ("SigBlk", "SigIgn", "SigCgt")
PROC_STATUS = "/proc/%d/status"
SETS_UNREADABLE = "signal sets: not readable here"


class Job:
    """What one run of a command is: the process, the pipe its signals come
    through, the FIFO of the rows and when it started."""

    def __init__(self, argv, cut_signal):
        self.argv = argv
        self.cut_signal = cut_signal
        self.proc = None
        self.wake_read = -1
        self.fifo = -1
        self.started = time.monotonic()
        self.cut_by = None
        self.stuck = None
        self.unkilled = []


def parse_duration(text, allow_zero=False):
    """Seconds of a duration written `<n>s`, `<n>m` or `<n>`; ValueError for
    anything else (a unit of hours, a fraction, a sign, nothing at all).

    A job that is given no usable deadline is refused instead of run with
    none, so a mistyped limit never turns into a run that is never cut."""
    found = DURATION_PATTERN.match(text or "")
    if not found:
        raise ValueError("%r is not a duration: write <n>s, <n>m or <n>" % text)
    seconds = int(found.group(1))
    if found.group(2) == "m":
        seconds *= SECONDS_PER_MINUTE
    if seconds == 0 and not allow_zero:
        raise ValueError("a duration of zero is no deadline")
    return seconds


def set_death_signal():
    """Ask the kernel to kill this process when its parent ends (Linux).

    Used by COMMAND, so a tool that was itself killed leaves no run with no
    deadline.  False where the call does not exist."""
    if not sys.platform.startswith("linux"):
        return False
    libc = ctypes.CDLL(None, use_errno=True)
    return libc.prctl(PR_SET_PDEATHSIG, signal.SIGKILL, 0, 0, 0) == 0


def become_subreaper():
    """Make this tool the one an orphaned descendant of the run comes back
    to (Linux), so the kill finds it and reaps it.  False elsewhere."""
    if not sys.platform.startswith("linux"):
        return False
    libc = ctypes.CDLL(None, use_errno=True)
    return libc.prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) == 0


def prepare_the_run(tool_pid):
    """The step COMMAND takes between fork and exec: the default signals, an
    empty mask (J1) and the death signal (J8).  Returns the function Popen
    runs there."""
    def setup():
        for number in DEFAULT_SIGNALS:
            signal.signal(number, signal.SIG_DFL)
        signal.pthread_sigmask(signal.SIG_SETMASK, [])
        set_death_signal()
        if os.getppid() != tool_pid:
            os._exit(KILL_EXIT)
    return setup


def environment_of_the_run():
    """This tool's environment less the variable of the rows' FIFO, so a
    nested run does not take the deadline of an outer one."""
    env = dict(os.environ)
    env.pop(NOW_FIFO_VARIABLE, None)
    return env


def noop(_number, _frame):
    """The handler that makes Python write a signal to the wakeup pipe."""


def watch_signals(job):
    """Open the pipe the signals arrive through, and the FIFO of the rows."""
    job.wake_read, wake_write = os.pipe()
    os.set_blocking(job.wake_read, False)
    os.set_blocking(wake_write, False)
    signal.set_wakeup_fd(wake_write, warn_on_full_buffer=False)
    for number in CUT_SIGNALS + (signal.SIGCHLD,):
        signal.signal(number, noop)
    named = os.environ.get(NOW_FIFO_VARIABLE)
    if named:
        job.fifo = os.open(named, os.O_RDWR | os.O_NONBLOCK)


def start_run(job):
    """Start COMMAND in a process group of its own (J1, J8).

    Returns an exit status when it cannot be started (126 or 127), else None."""
    watch_signals(job)
    become_subreaper()
    try:
        job.proc = subprocess.Popen(
            job.argv, env=environment_of_the_run(), process_group=0,
            preexec_fn=prepare_the_run(os.getpid()))
    except FileNotFoundError as err:
        sys.stderr.write("job_cut: %s: %s\n" % (job.argv[0], err.strerror))
        return NOT_FOUND_EXIT
    except PermissionError as err:
        sys.stderr.write("job_cut: %s: %s\n" % (job.argv[0], err.strerror))
        return CANNOT_RUN_EXIT
    return None


def drain(fd):
    """The bytes waiting on a non-blocking descriptor."""
    data = b""
    while True:
        try:
            chunk = os.read(fd, READ_CHUNK)
        except BlockingIOError:
            return data
        if not chunk:
            return data
        data += chunk


def wait_for_end_or_deadline(job, seconds, watch_cut=True):
    """Wait up to @seconds for the run to end; returns ENDED, DEADLINE, NOW
    (a byte on the rows' FIFO) or SIGNALLED (a termination signal to this
    tool).  With watch_cut false, only the end and the deadline count: the
    grace is waited out once the cut has been made.

    One select on the wakeup pipe and the FIFO with the time left as its
    timeout (J7): the end of COMMAND is a SIGCHLD, so nothing is polled."""
    end = deadline_in(seconds)
    while True:
        if job.proc.poll() is not None:
            return ENDED
        left = seconds_until(end)
        if left <= 0:
            return DEADLINE
        now_asked, numbers = wait_on_the_pipes(job, left)
        if now_asked and watch_cut:
            return NOW
        if watch_cut and any(number in CUT_SIGNALS for number in numbers):
            return SIGNALLED


def deadline_in(seconds):
    """The time, on the monotonic clock, @seconds from now."""
    return time.monotonic() + seconds


def seconds_until(end):
    """The seconds left until @end on the monotonic clock (negative when it
    has passed)."""
    return end - time.monotonic()


def wait_on_the_pipes(job, seconds):
    """The one wait of this tool (J7): at most @seconds on the wakeup pipe and
    the rows' FIFO.  Returns (a byte came on the FIFO, the signal numbers that
    came on the wakeup pipe); both are drained."""
    watched = [job.wake_read] + ([job.fifo] if job.fifo >= 0 else [])
    ready, _w, _x = select.select(watched, [], [], seconds)
    now_asked = job.fifo in ready
    if now_asked:
        drain(job.fifo)
    numbers = drain(job.wake_read) if job.wake_read in ready else b""
    return now_asked, numbers


def cut_the_run(job, how):
    """Send the cut to COMMAND's pid and to nothing else (J2).

    @how says what ended the wait, kept for the report."""
    job.cut_by = how
    try:
        os.kill(job.proc.pid, job.cut_signal)
    except ProcessLookupError:
        pass


def read_signal_sets(pid):
    """{"name", "state", "SigBlk", "SigIgn", "SigCgt"} of one process from
    /proc, or None where /proc is missing or the process is gone (SP5: the
    process table module reads no signal sets, so they are read here)."""
    try:
        with open(PROC_STATUS % pid, encoding="utf-8", errors="replace") as fh:
            lines = fh.read().splitlines()
    except OSError:
        return None
    found = {}
    for line in lines:
        key, _sep, value = line.partition(":")
        if key in SIGNAL_SETS or key in ("Name", "State"):
            found[key] = value.strip()
    return {"name": found.get("Name"), "state": found.get("State"),
            **{key: found.get(key) for key in SIGNAL_SETS}}


def run_pids(job):
    """COMMAND's pid, while it has not ended, and every descendant of this
    tool that has not ended."""
    found = procinfo.descendants(os.getpid())
    if job.proc.pid not in found and procinfo.run_state(job.proc.pid) not in (
            procinfo.GONE, procinfo.ZOMBIE):
        found.insert(0, job.proc.pid)
    return found


def report_the_stuck_run(job):
    """Name what did not end, before it is killed (J5).

    Every process of the run gets an entry with its signal sets; SIGUSR1 then
    goes to each, and the run has STACK_DUMP_WAIT_S seconds to write the
    stacks its processes registered a dump of.  The entries go to the report
    and a line each to the log."""
    entries = []
    for pid in run_pids(job):
        sets = read_signal_sets(pid)
        entry = {"pid": pid}
        entry.update(sets if sets else {"note": SETS_UNREADABLE})
        entries.append(entry)
        sys.stderr.write("job_cut: still running: %s\n" % json.dumps(entry))
    for entry in entries:
        try:
            os.kill(entry["pid"], signal.SIGUSR1)
        except OSError:
            pass
    wait_for_end_or_deadline(job, STACK_DUMP_WAIT_S, watch_cut=False)
    job.stuck = entries


def reap_children():
    """Reap every child of this tool that has ended; the orphans a subreaper
    was given are among them."""
    while True:
        try:
            pid, _status = os.waitpid(-1, os.WNOHANG)
        except ChildProcessError:
            return
        if pid == 0:
            return


def kill_the_group(job):
    """End the whole run: its process group, then every descendant of this
    tool found in the process table, again and again until none is left, and
    reap them (SP4).

    A group kill alone does not reach what an owner started in a group of its
    own, and a grandchild of those has no death signal.  A run that forks
    while it is killed (a descendant that was reparented after the listing)
    is found by the next round; the whole kill ends after KILL_BOUND_S at
    most, so the tool never waits for a process that cannot be ended, and the
    pids that were still there are kept for the report (job.unkilled)."""
    end = deadline_in(KILL_BOUND_S)
    while True:
        left = procinfo.descendants(os.getpid())
        try:
            os.killpg(job.proc.pid, signal.SIGKILL)
        except OSError:
            pass
        for pid in left:
            try:
                os.kill(pid, signal.SIGKILL)
            except OSError:
                pass
        reap_children()
        if not left:
            return
        remaining = seconds_until(end)
        if remaining <= 0:
            job.unkilled = left
            sys.stderr.write("job_cut: still there after the kill: %s\n" % left)
            return
        wait_on_the_pipes(job, min(remaining, KILL_ROUND_WAIT_S))


def exit_status_of(returncode):
    """COMMAND's status as a shell exit code: its own, or 128 plus the signal
    that ended it."""
    if returncode < 0:
        return SIGNAL_EXIT_BASE - returncode
    return returncode


def write_report(path, job, deadline_s, status):
    """Write how the run ended (SP3): what the gate reads to tell a cut from
    a command that died of a signal of its own."""
    if not path:
        return
    killed = job.stuck is not None
    report = {"cut": job.cut_by is not None, "killed": killed,
              "cut_by": job.cut_by, "deadline_s": deadline_s,
              "elapsed_s": round(time.monotonic() - job.started, 2),
              "exit": status,
              "command_exit": job.proc.returncode if job.proc else None,
              "command": job.argv}
    if killed:
        report["stuck"] = job.stuck
    if job.unkilled:
        report["unkilled"] = job.unkilled
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(report, fh, indent=1, sort_keys=True)
        fh.write("\n")


def run_and_cut(job, deadline_s, grace_s):
    """Run COMMAND until it ends or is cut; returns the exit status (J4)."""
    failed = start_run(job)
    if failed is not None:
        return failed
    first = wait_for_end_or_deadline(job, deadline_s)
    if first == ENDED:
        return exit_status_of(job.proc.returncode)
    cut_the_run(job, first)
    if wait_for_end_or_deadline(job, grace_s, watch_cut=False) == ENDED:
        return CUT_EXIT
    report_the_stuck_run(job)
    return KILL_EXIT


def cut_signal_of(name):
    """The signal a cut sends: INT (pytest writes its junit) or KILL."""
    return signal.SIGKILL if name == "KILL" else signal.SIGINT


def run_the_job(options, argv):
    """The whole of one run: start, wait, cut, kill what is left, report.

    A failure of this tool ends the run before it exits 125 (J8)."""
    job = Job(argv, cut_signal_of(options.signal))
    status = TOOL_FAILED_EXIT
    try:
        status = run_and_cut(job, options.after, options.grace)
    except BaseException as err:
        sys.stderr.write("job_cut: failed: %r\n" % (err,))
        status = TOOL_FAILED_EXIT
    finally:
        if job.proc is not None:
            kill_the_group(job)
    try:
        write_report(options.report, job, options.after, status)
    except OSError as err:
        sys.stderr.write("job_cut: cannot write the report: %s\n" % err)
        status = TOOL_FAILED_EXIT
    return status


def duration_argument(text):
    """argparse type of --after (a positive duration)."""
    try:
        return parse_duration(text)
    except ValueError as err:
        raise argparse.ArgumentTypeError(str(err))


def grace_argument(text):
    """argparse type of --grace (a duration, zero allowed)."""
    try:
        return parse_duration(text, allow_zero=True)
    except ValueError as err:
        raise argparse.ArgumentTypeError(str(err))


def split_command(argv):
    """(the tool's own words, COMMAND) around the first `--`."""
    if "--" not in argv:
        return argv, []
    index = argv.index("--")
    return argv[:index], argv[index + 1:]


def main(argv):
    """The command line of the tool; see the header."""
    parser = argparse.ArgumentParser(
        prog="job_cut.py", usage="job_cut.py --after DURATION [--grace S] "
        "[--signal INT|KILL] [--report FILE] -- COMMAND...")
    parser.add_argument("--after", type=duration_argument, required=True)
    parser.add_argument("--grace", type=grace_argument, default=DEFAULT_GRACE_S)
    parser.add_argument("--signal", choices=("INT", "KILL"), default="INT")
    parser.add_argument("--report")
    own, command = split_command(argv)
    options = parser.parse_args(own)
    if not command:
        parser.error("no COMMAND after --")
    return run_the_job(options, command)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
