"""Debugger watchdog: xwpe must never busy-loop on a dead or runaway debugger.

A debugger backend can die abnormally mid-session (a68g, for instance, can fall
into an abend->io_write_string recursion that floods its output until it
SIGSEGVs).  Before the watchdog, xwpe's synchronous read loops would spin on
that flood -- a tight CPU loop that hangs the editor (observed: a session stuck
at ~100% CPU for many minutes).  The design rule is to never leave xwpe in a
busy loop.

This test stands a FAKE debugger on PATH that floods its output forever and
never prints a prompt, drives xwpe into a debug session, and asserts that xwpe
stays responsive (low CPU / not in the run state) instead of spinning.  No real
debugger is needed, so it always runs.
"""
import os
import signal
import subprocess
import time

from wpe_driver import WpeSession

A68 = (
    "PROC factorial = (INT n) INT:\n"
    "  IF n <= 1 THEN 1\n"
    "  ELSE n * factorial(n - 1)\n"
    "  FI;\n"
    "\n"
    "INT result := factorial(5);\n"
    'print(("factorial(5) = ", result, newline))\n'
)
CTRL_G = "\x07"
DOWN = "\033OB"


def _cpu_ticks(pid):
    """utime+stime in clock ticks for pid, or -1 if gone.

    Linux uses /proc/<pid>/stat; macOS (no procfs) falls back to ps(1)'s
    cumulative CPU time, converted to centi-seconds to roughly match the
    ratio the caller compares against (< 20 ticks per wall-second)."""
    try:
        return int(open("/proc/%d/stat" % pid).read().split()[13]) + \
               int(open("/proc/%d/stat" % pid).read().split()[14])
    except Exception:
        pass
    try:
        out = subprocess.check_output(["ps", "-p", str(pid), "-o", "time="],
                                      text=True).strip()
        parts = out.replace("-", ":").split(":")
        if len(parts) == 4:
            h, m, sf = int(parts[1]), int(parts[2]), float(parts[3])
            h += int(parts[0]) * 24
        elif len(parts) == 3:
            h, m, sf = int(parts[0]), int(parts[1]), float(parts[2])
        else:
            h, m, sf = 0, int(parts[0]), float(parts[1])
        return int(((h * 3600 + m * 60) * 100) + sf * 100)
    except Exception:
        return -1


def _run_state(pid):
    """Single-letter run state (R/S/D/Z/T) for pid, or 'gone'."""
    try:
        return open("/proc/%d/stat" % pid).read().split()[2]
    except Exception:
        pass
    try:
        out = subprocess.check_output(["ps", "-p", str(pid), "-o", "state="],
                                      text=True).strip()
        return out[:1] if out else "gone"
    except Exception:
        return "gone"


FAKE_PIDS = "fake-a68g.pids"
ALT_X = "\033x"                 # quit the editor
QUIT_TIMEOUT_S = 10


def _flooding_a68g(tmp_path):
    """Put a fake a68g first on PATH that records its PID, prints a short
    banner, then floods its output forever with no prompt: the shape of a
    runaway abend.  Auto-selected because the file is .a68.  Return the
    environment that puts it on PATH."""
    bindir = tmp_path / "bin"
    bindir.mkdir()
    fake = bindir / "a68g"
    fake.write_text(
        "#!/bin/sh\n"
        "echo $$ >> %s\n"
        'echo "Algol 68 Genie (fake flood)"\n'
        'while : ; do echo "abend: runaway error $RANDOM"; done\n'
        % (tmp_path / FAKE_PIDS)
    )
    fake.chmod(0o755)
    return {"PATH": str(bindir) + ":" + os.environ.get("PATH", "")}


def _live_fakes(tmp_path):
    """PIDs of the fake a68g runs of this test that are still running (and
    still that fake: the command line names this test's directory)."""
    try:
        pids = [int(p) for p in (tmp_path / FAKE_PIDS).read_text().split()]
    except OSError:
        return []
    live = []
    for pid in pids:
        try:
            cmdline = open("/proc/%d/cmdline" % pid, "rb").read()
        except OSError:
            continue
        if str(tmp_path).encode() in cmdline and _run_state(pid) != "Z":
            live.append(pid)
    return live


def _end_fakes(tmp_path):
    """End every fake a68g this test started that is still running, by its
    exact PID: the editor is killed with its process group at the end of a
    session, but a build runs in a group of its own."""
    for pid in _live_fakes(tmp_path):
        try:
            os.kill(pid, 9)
        except OSError:
            pass


# @scenario SC-debug-15
def test_wpe_survives_a_flooding_debugger(tmp_path):
    """A debugger that floods output forever must not make xwpe busy-loop."""
    env_extra = _flooding_a68g(tmp_path)
    try:
        _drive_the_flood(tmp_path, env_extra)
    finally:
        _end_fakes(tmp_path)


# @scenario SC-debug-15
def test_quitting_the_editor_ends_a_running_build(tmp_path):
    """Alt-X while the fake a68g's check still floods: the editor quits and
    the check, which runs in a process group of its own, is gone too
    instead of being left spinning with no editor to show it."""
    env_extra = _flooding_a68g(tmp_path)
    try:
        with WpeSession(str(tmp_path), A68, filename="factorial.a68",
                        wait=2.0, env_extra=env_extra) as w:
            w.key(CTRL_G, "r", delay=1.0)
            deadline = time.time() + QUIT_TIMEOUT_S
            while not _live_fakes(tmp_path) and time.time() < deadline:
                w._drain(0.2)
            assert _live_fakes(tmp_path), "the fake a68g should be running"
            w.key(ALT_X, delay=0.5)
            deadline = time.time() + QUIT_TIMEOUT_S
            while w.proc.poll() is None and time.time() < deadline:
                w._drain(0.2)
            assert w.proc.poll() is not None, "Alt-X should quit the editor"
            assert not _live_fakes(tmp_path), \
                "the build the editor started outlived it"
    finally:
        _end_fakes(tmp_path)


# @scenario SC-debug-15
def test_sigterm_to_the_editor_ends_a_running_build(tmp_path):
    """SIGTERM to the editor, by its PID, while the fake a68g's check still
    floods: the editor ends and the check, in a process group of its own,
    ends with it."""
    env_extra = _flooding_a68g(tmp_path)
    try:
        with WpeSession(str(tmp_path), A68, filename="factorial.a68",
                        wait=2.0, env_extra=env_extra) as w:
            w.key(CTRL_G, "r", delay=1.0)
            deadline = time.time() + QUIT_TIMEOUT_S
            while not _live_fakes(tmp_path) and time.time() < deadline:
                w._drain(0.2)
            assert _live_fakes(tmp_path), "the fake a68g should be running"
            os.kill(w.proc.pid, signal.SIGTERM)
            deadline = time.time() + QUIT_TIMEOUT_S
            while w.proc.poll() is None and time.time() < deadline:
                w._drain(0.2)
            assert w.proc.poll() is not None, "SIGTERM should end the editor"
            deadline = time.time() + QUIT_TIMEOUT_S
            while _live_fakes(tmp_path) and time.time() < deadline:
                time.sleep(0.2)
            assert not _live_fakes(tmp_path), \
                "the build the editor started outlived its SIGTERM"
    finally:
        _end_fakes(tmp_path)


def _drive_the_flood(tmp_path, env_extra):
    with WpeSession(str(tmp_path), A68, filename="factorial.a68",
                    wait=2.0, env_extra=env_extra) as w:
        pid = w.proc.pid
        w.key(DOWN, delay=0.3)
        w.key(DOWN, delay=0.3)
        w.key(CTRL_G, "b", delay=0.4)     # breakpoint
        w.key(CTRL_G, "r", delay=1.0)     # run -> launches the flooding fake a68g
        # Give the watchdog time to trip, then confirm xwpe is NOT spinning:
        # sample CPU over a 1s window once it should have recovered.
        responsive = False
        for _ in range(20):
            c0 = _cpu_ticks(pid)
            time.sleep(1.0)
            c1 = _cpu_ticks(pid)
            if w.proc.poll() is not None:
                responsive = True            # exited cleanly -- not hung
                break
            # < ~0.2s of CPU per wall second and sleeping == back in the input
            # loop, i.e. the watchdog tripped instead of busy-looping.  macOS
            # ps reports state 'S'/'Ss'/'I'/... -- treat any non-R as sleeping.
            if _run_state(pid) != "R" and (c1 - c0) < 20:
                responsive = True
                break
        assert responsive, \
            "xwpe busy-looped on a flooding debugger instead of recovering"
