"""Debugger watchdog: xwpe must never busy-loop on a dead or runaway debugger.

A debugger backend can die abnormally mid-session (a68g, for instance, can fall
into an abend->io_write_string recursion that floods its output until it
SIGSEGVs).  Before the watchdog, xwpe's synchronous read loops would spin on
that flood -- a tight CPU loop that hangs the editor (observed: a session stuck
at ~100% CPU for many minutes).  the project convention forbids leaving xwpe in a busy
loop.

This test stands a FAKE debugger on PATH that floods its output forever and
never prints a prompt, drives xwpe into a debug session, and asserts that xwpe
stays responsive (low CPU / not in the run state) instead of spinning.  No real
debugger is needed, so it always runs.
"""
import os
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
    """utime+stime in clock ticks for pid, or -1 if gone."""
    try:
        f = open("/proc/%d/stat" % pid).read().split()
        return int(f[13]) + int(f[14])
    except Exception:
        return -1


def _run_state(pid):
    try:
        return open("/proc/%d/stat" % pid).read().split()[2]
    except Exception:
        return "gone"


def test_wpe_survives_a_flooding_debugger(tmp_path):
    """A debugger that floods output forever must not make xwpe busy-loop."""
    # Fake a68g: a short banner, then an endless flood with no prompt -- the
    # shape of a runaway abend.  Auto-selected because the file is .a68.
    bindir = tmp_path / "bin"
    bindir.mkdir()
    fake = bindir / "a68g"
    fake.write_text(
        "#!/bin/sh\n"
        'echo "Algol 68 Genie (fake flood)"\n'
        'while : ; do echo "abend: runaway error $RANDOM"; done\n'
    )
    fake.chmod(0o755)

    env_extra = {"PATH": str(bindir) + ":" + os.environ.get("PATH", "")}
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
            # loop, i.e. the watchdog tripped instead of busy-looping.
            if _run_state(pid) == "S" and (c1 - c0) < 20:
                responsive = True
                break
        assert responsive, \
            "xwpe busy-looped on a flooding debugger instead of recovering"
