"""A child's end is an event: the build's.

The editor starts a build's commands through the children's owner
(we_child.c), which learns of each end from the SIGCHLD handler and reports
it from the loop with the child's own wait status; nobody asks waitpid, and
nobody waits.  A compiler that starts twenty short processes which end with
it, and fails with exit status 3 printing nothing the error parser knows, is
reported as failed with that exit status.

A stand-in `gcc` on PATH plays the compiler.
"""
import os
import stat
import time

from wpe_driver import WpeSession, WAIT_SCALE

ALT_C = "\033c"                   # Compile
POLL_S = 0.2
BUILD_END_S = 20
BUILD_EXIT = 3
BUILD_BURST = 20                  # processes ending with the compiler
BUILD_FAILED_LINE = "the build failed (exit status %d)" % BUILD_EXIT
SEED_C = "int main(void){return 0;}\n"

# A compiler that starts twenty processes which end together with it, closes
# its output, and fails without a line the error parser would count.
FAKE_GCC = """#!/bin/sh
i=0
while [ $i -lt %(burst)d ]; do
  ( exit 0 ) &
  i=$((i + 1))
done
wait
exec >/dev/null 2>&1
exit %(code)d
""" % {"burst": BUILD_BURST, "code": BUILD_EXIT}


ESC = "\033"
CANCELLED_LINE = "build cancelled"
MEMBER_READY = "member started"
MEMBER_LIVES_S = 20               # far longer than the grace
CANCEL_WITHIN_S = 5.0             # the 2 s grace plus the loop's slack

# A compiler that ends at SIGTERM, with a child in its group that ignores
# SIGTERM and holds the build's output open (its pid in a file).
FAKE_GCC_MEMBER_DEAF = """#!/bin/sh
( trap '' TERM; exec sleep %(lives)d ) &
echo $! > "$MEMBER_PID"
echo %(ready)s
wait
""" % {"lives": MEMBER_LIVES_S, "ready": MEMBER_READY}


def _write_executable(path, text):
    with open(path, "w") as fh:
        fh.write(text)
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)


def _screen(w):
    return "\n".join(w.display(drain=0))


def _wait_for(w, predicate, timeout):
    deadline = time.time() + timeout * WAIT_SCALE
    while time.time() < deadline:
        w._drain(POLL_S)
        if predicate():
            return True
    return predicate()


# @scenario SC-debug-72
def test_build_end_is_reported_when_many_children_end_at_once(tmp_path):
    """Alt-C with a compiler that starts twenty processes ending with it and
    fails with exit status 3, printing nothing: Messages says the build
    failed with exit status 3, never "Success"."""
    bindir = tmp_path / "bin"
    bindir.mkdir()
    _write_executable(str(bindir / "gcc"), FAKE_GCC)
    env = {"PATH": "%s:%s" % (bindir, os.environ.get("PATH", ""))}
    with WpeSession(str(tmp_path), SEED_C, filename="t.c", wait=1.5,
                    env_extra=env) as w:
        w.key(ALT_C, delay=0.5)
        assert _wait_for(w, lambda: BUILD_FAILED_LINE in _screen(w),
                         BUILD_END_S), \
            "the build's own exit status should be reported:\n%s" % _screen(w)
        assert "Success" not in _screen(w), _screen(w)


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


# @scenario SC-debug-72
def test_esc_ends_a_build_whose_compiler_child_ignores_sigterm(tmp_path):
    """Esc on a build whose compiler ends at SIGTERM while a process it
    started ignores it and keeps the output open: that process is killed
    when the grace is over, and Messages says "build cancelled" within
    seconds, not when the process would have ended by itself."""
    bindir = tmp_path / "bin"
    bindir.mkdir()
    _write_executable(str(bindir / "gcc"), FAKE_GCC_MEMBER_DEAF)
    pidfile = str(tmp_path / "member.pid")
    env = {"PATH": "%s:%s" % (bindir, os.environ.get("PATH", "")),
           "MEMBER_PID": pidfile}
    member = 0
    try:
        with WpeSession(str(tmp_path), SEED_C, filename="t.c", wait=1.5,
                        env_extra=env) as w:
            w.key(ALT_C, delay=0.5)
            assert _wait_for(w, lambda: MEMBER_READY in _screen(w),
                             BUILD_END_S), _screen(w)
            member = _read_pid(pidfile)
            assert member > 0, "the stand-in compiler did not write its pid"
            start = time.time()
            w.key(ESC, delay=0.1)
            assert _wait_for(w, lambda: CANCELLED_LINE in _screen(w),
                             MEMBER_LIVES_S), _screen(w)
            took = time.time() - start
            gone = not _pid_alive(member)
            assert w.alive()
        assert took < CANCEL_WITHIN_S * WAIT_SCALE, \
            "the build was cancelled only after %.1fs" % took
        assert gone, "the compiler's child survived the cancel"
    finally:
        if member > 0 and _pid_alive(member):
            os.kill(member, 9)
