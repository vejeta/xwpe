"""The commands tests/test_job_cut.py runs under tests/harness/job_cut.py.

Each mode is a small program that behaves the way a run behaves at its cut:
it takes the interrupt and says so, it ignores it, it dies by itself, it
leaves processes in groups of their own.  The parent names the files they
write their marks in on the command line.

  inner_job_cut.py exit N
  inner_job_cut.py die-by-kill
  inner_job_cut.py dispositions
  inner_job_cut.py take-interrupt READY GOT
  inner_job_cut.py ignore-interrupt READY
  inner_job_cut.py family PIDS            (a child and a grandchild, each in
                                           a group of its own)
  inner_job_cut.py controller READY GOT CHILD_GOT
                                          (a controller and a worker in the
                                           controller's group, as pytest-xdist)
  inner_job_cut.py wait-forever PIDS      (the grandchild of a family)
  inner_job_cut.py burst PIDS             (a run that forks 28 children, each
                                           in a session of its own, while it
                                           is being killed)
  inner_job_cut.py kill-bound SECONDS     (the tool's kill on a process no
                                           signal reaches)

The file is in tests/ and its name does not begin with test_, so a plain run
of tests/ never collects it.
"""
import os
import signal
import subprocess
import sys

HARNESS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "harness")
# The burst of a run that keeps forking while it is killed: how many
# children, and the gap between two of them (the whole burst lasts a little
# longer than the wait for the stack dump that precedes the kill).
BURST_SIZE = 28
BURST_STEP_S = 0.08
# How long the controller gives its worker to end after the interrupt before
# it ends the worker itself; the interrupt reaches the worker only when the
# tool sent it to the whole group.
WORKER_GRACE_S = 1
# The file the last of a family writes beside the pids file once it is up.
FAMILY_UP_SUFFIX = ".up"


def note(path, text):
    """Append a line to @path."""
    with open(path, "a", encoding="utf-8") as fh:
        fh.write("%s\n" % text)


def wait_forever():
    """Block until a signal ends the process; an interrupt raises."""
    while True:
        signal.pause()


def stay_through_the_stack_dump():
    """Ignore SIGUSR1, the signal the cut sends to ask a stuck run for its
    stacks: a process of a run that is really stuck does not end on it, so
    only the kill can."""
    signal.signal(signal.SIGUSR1, signal.SIG_IGN)


def spawn_alone(mode, *words):
    """Start this file in @mode in a session of its own."""
    return subprocess.Popen([sys.executable, __file__, mode] + list(words),
                            start_new_session=True)


def disposition_name(handler):
    """One word for what a process does with a signal: `ignored`, `default`
    (the system's) or `interrupt` (Python's own, which raises
    KeyboardInterrupt)."""
    if handler == signal.SIG_IGN:
        return "ignored"
    if handler == signal.SIG_DFL:
        return "default"
    if handler == signal.default_int_handler:
        return "interrupt"
    return "other"


def mode_dispositions():
    """Print what the process has for the signals a cut run may have
    inherited ignored."""
    for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP, signal.SIGQUIT):
        print("%s=%s" % (number.name, disposition_name(signal.getsignal(number))))
    print("blocked=%s" % sorted(signal.pthread_sigmask(signal.SIG_BLOCK, [])))
    return 0


def mode_take_interrupt(ready, got):
    """Say the run is up, wait, and say the interrupt arrived."""
    note(ready, os.getpid())
    try:
        wait_forever()
    except KeyboardInterrupt:
        note(got, os.getpid())
    return 0


def mode_ignore_interrupt(ready):
    """Say the run is up and keep running through the interrupt."""
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    note(ready, os.getpid())
    wait_forever()


def mode_family(pids):
    """Leave a child in a group of its own and a grandchild of it in another,
    the way an owner starts an editor and what the editor starts."""
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    stay_through_the_stack_dump()
    note(pids, os.getpid())
    spawn_alone("family-child", pids)
    wait_forever()


def mode_family_child(pids):
    """The child of the family: records itself, starts the grandchild."""
    stay_through_the_stack_dump()
    note(pids, os.getpid())
    spawn_alone("wait-forever", pids)
    wait_forever()


def mode_wait_forever(pids):
    """The grandchild: records itself, then the mark that the whole family is
    up (the parent's wait), and waits for a signal."""
    stay_through_the_stack_dump()
    note(pids, os.getpid())
    note(pids + FAMILY_UP_SUFFIX, "up")
    wait_forever()


def mode_burst(pids):
    """A run that keeps forking while it is cut: it ignores the interrupt, and
    on the stack-dump signal that comes just before the kill it starts
    BURST_SIZE children, one every BURST_STEP_S seconds, each in a session of
    its own.  The burst is still going when the kill is made, so some of its
    children are born after the kill has listed what to end."""
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    signal.pthread_sigmask(signal.SIG_BLOCK, [signal.SIGUSR1, signal.SIGUSR2])
    note(pids, os.getpid())
    note(pids + FAMILY_UP_SUFFIX, "up")
    signal.sigwait([signal.SIGUSR1])
    for _ in range(BURST_SIZE):
        fork_a_burst_child(pids)
        signal.sigtimedwait([signal.SIGUSR2], BURST_STEP_S)
    wait_forever()


def fork_a_burst_child(pids):
    """One child of the burst: alone in its session, it records itself and
    waits for a signal."""
    child = os.fork()
    if child != 0:
        return
    signal.pthread_sigmask(signal.SIG_UNBLOCK, [signal.SIGUSR1])
    stay_through_the_stack_dump()
    os.setsid()
    note(pids, os.getpid())
    wait_forever()


def mode_kill_bound(seconds):
    """Run the tool's own kill on a run that cannot be ended, and say how long
    it took and what it named: a child that the listing names but no signal
    reaches stands for a process that is reparented after the listing or
    does not die.  The kill must give up after its bound, never wait for it."""
    sys.path.insert(0, HARNESS)
    import job_cut
    job_cut.KILL_BOUND_S = float(seconds)
    job = job_cut.Job(["none"], signal.SIGINT)
    job.wake_read, wake_write = os.pipe()
    os.set_blocking(job.wake_read, False)
    os.set_blocking(wake_write, False)
    signal.set_wakeup_fd(wake_write, warn_on_full_buffer=False)
    signal.signal(signal.SIGCHLD, lambda _n, _f: None)
    stubborn = subprocess.Popen(
        [sys.executable, "-c", "import signal\nwhile True: signal.pause()"],
        start_new_session=True)
    job.proc = stubborn
    job_cut.procinfo.descendants = lambda _pid: [stubborn.pid]
    real_kill, real_killpg = os.kill, os.killpg
    os.kill = os.killpg = lambda *_args: None
    started = job_cut.deadline_in(0)
    try:
        job_cut.kill_the_group(job)
    finally:
        os.kill, os.killpg = real_kill, real_killpg
    print("elapsed=%.1f unkilled=%s" % (-job_cut.seconds_until(started),
                                        job.unkilled == [stubborn.pid]), flush=True)
    stubborn.kill()
    stubborn.wait()
    return 0


def mode_worker(got):
    """A worker of the controller: it says it is up, then notes the interrupt
    if it gets one."""
    try:
        print("up", flush=True)
        wait_forever()
    except KeyboardInterrupt:
        note(got, os.getpid())
    return 0


def mode_controller(ready, got, worker_got):
    """A controller with a worker in its own group: it ends the worker itself
    after the interrupt, as pytest-xdist does.  It is ready once the worker
    has said it is up, so the interrupt never meets a worker that is still
    starting."""
    worker = subprocess.Popen([sys.executable, __file__, "worker", worker_got],
                              stdout=subprocess.PIPE)
    worker.stdout.readline()
    note(ready, os.getpid())
    try:
        wait_forever()
    except KeyboardInterrupt:
        note(got, os.getpid())
    try:
        worker.wait(WORKER_GRACE_S)
    except subprocess.TimeoutExpired:
        worker.kill()
        worker.wait()
    return 0


def run(argv):
    """Dispatch on the mode in argv[0]; returns the exit status."""
    mode, words = argv[0], argv[1:]
    if mode == "exit":
        return int(words[0])
    if mode == "die-by-kill":
        os.kill(os.getpid(), signal.SIGKILL)
    if mode == "dispositions":
        return mode_dispositions()
    if mode == "take-interrupt":
        return mode_take_interrupt(*words)
    if mode == "ignore-interrupt":
        return mode_ignore_interrupt(*words)
    if mode == "family":
        return mode_family(*words)
    if mode == "family-child":
        return mode_family_child(*words)
    if mode == "wait-forever":
        return mode_wait_forever(*words)
    if mode == "burst":
        return mode_burst(*words)
    if mode == "kill-bound":
        return mode_kill_bound(*words)
    if mode == "worker":
        return mode_worker(*words)
    if mode == "controller":
        return mode_controller(*words)
    raise SystemExit("unknown mode %s" % mode)


if __name__ == "__main__":
    sys.exit(run(sys.argv[1:]))
