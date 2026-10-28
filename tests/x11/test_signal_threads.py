"""The end of a program the editor started is only ever taken on the
editor's own thread.

The graphical window brings threads the editor did not write (Pango loads
fonts on one).  The kernel hands a process-wide signal such as SIGCHLD to
any thread that does not block it, and blocking it in the editor's thread
says nothing about the others.  When a library thread took SIGCHLD, the
handler recorded a child's end while the editor's thread was still putting
that child on its watch list, and a Java program's session under jdb could
stay "running" until Quit.  Every library thread now starts with SIGCHLD
blocked, while the editor's own thread keeps it open (else no program's end
would ever be seen, and every program it starts would inherit the block).

Linux only: reads each thread's masks from /proc/<pid>/task/<tid>/status.
"""
import os
import signal

import pytest

SIGCHLD_BIT = 1 << (signal.SIGCHLD - 1)   # its bit in SigBlk / SigIgn

pytestmark = pytest.mark.skipif(
    not os.path.exists("/proc/self/task"),
    reason="reads thread signal masks from /proc, which only Linux has")


def _thread_status(pid, tid):
    """Name and signal masks of one thread of `pid` (the editor the test
    started), as a dict of the /proc status fields used here."""
    fields = {}
    with open("/proc/%d/task/%s/status" % (pid, tid)) as fh:
        for line in fh:
            key, _, value = line.partition(":")
            if key in ("Name", "SigBlk", "SigIgn"):
                fields[key] = value.strip()
    return fields


def _threads(pid):
    """{tid: status fields} for every thread of the editor."""
    return {int(tid): _thread_status(pid, tid)
            for tid in os.listdir("/proc/%d/task" % pid)}


def _blocks_sigchld(fields):
    return bool(int(fields["SigBlk"], 16) & SIGCHLD_BIT)


# @scenario SC-debug-13
def test_only_the_editors_thread_takes_sigchld(xwpe):
    """Every thread but the editor's own blocks SIGCHLD; the editor's own
    thread neither blocks nor ignores it."""
    pid = xwpe.proc.pid
    threads = _threads(pid)
    main = threads.pop(pid)
    if not threads:
        pytest.skip("the graphical libraries started no thread on this host")
    assert not _blocks_sigchld(main), \
        "the editor's own thread blocks SIGCHLD: %s" % main
    assert not int(main["SigIgn"], 16) & SIGCHLD_BIT, \
        "the editor ignores SIGCHLD: %s" % main
    open_to_it = {tid: f["Name"] for tid, f in threads.items()
                  if not _blocks_sigchld(f)}
    assert not open_to_it, \
        "library threads that can take SIGCHLD: %s" % open_to_it
