"""The Scala build server's start never holds the editor.

A Scala debug session reaches its adapter through the build server: five
requests (initialize, the targets, a compile, the main classes, the debug
session) that take tens of seconds on a cold project.  They run on the
editor's loop: the editor takes keys meanwhile, the status row names the
request waited for after a quiet second, and Esc ends the build server and
starts nothing.

A stand-in build server plays Bloop: the project's `.bsp` connection file
names it, and it holds `build/initialize` until the test writes to its
release pipe.  A stand-in `scala-cli` on PATH only has to exist.  The
stand-in writes its PID and a log of what it read.
"""
import json
import os
import signal
import stat
import sys
import time

import procinfo
from wpe_driver import WpeSession, WAIT_SCALE

CTRL_G = "\x07"
ESC = "\033"
ALT_1 = "\0331"                   # window 1, the source (Messages came up)
POLL_S = 0.2
ASKED_S = 20                      # the start's first request reaches the stub
SAID_S = 10
GONE_S = 8                        # the owner's grace, the kill grace, slack
STUB_PID = "bsp-stub.pid"
STUB_LOG = "bsp-stub.log"
STUB_RELEASE = "bsp-stub.release"
RELEASE_LINE = b"go\n"
# The status row keeps the end of a line too long for it, so the check is on
# the part that names the server and the request.
WAITING = "build server to answer `build/initialize`"
CANCELLED = "Scala build server start cancelled."
TYPED = "zq"
SEED = ("object P:\n"
        "  def main(args: Array[String]): Unit =\n"
        "    println(1)\n")

FAKE_BSP = r'''#!%(python)s -I
import json, os, select, sys
here = %(dir)r
inp, out = sys.stdin.buffer, sys.stdout.buffer

def note(text):
    with open(os.path.join(here, %(log)r), "a") as fh:
        fh.write(text + "\n")

with open(os.path.join(here, %(pid)r), "w") as fh:
    fh.write(str(os.getpid()))
release_path = os.path.join(here, %(release)r)
os.mkfifo(release_path)
release = os.open(release_path, os.O_RDWR | os.O_NONBLOCK)

# Messages are split from raw reads, so select() never waits while one sits
# in a read buffer.
pending = b""

def messages():
    """Every whole message read so far."""
    global pending
    out = []
    while True:
        head, sep, rest = pending.partition(b"\r\n\r\n")
        if not sep:
            return out
        length = 0
        for line in head.split(b"\r\n"):
            if line.lower().startswith(b"content-length:"):
                length = int(line.split(b":")[1])
        if len(rest) < length:
            return out
        out.append(json.loads(rest[:length]))
        pending = rest[length:]

while True:
    ready, _, _ = select.select([0, release], [], [])
    if release in ready:
        os.read(release, 64)
        note("RELEASE")
        continue
    data = os.read(0, 65536)
    if not data:
        note("EOF")
        break
    pending += data
    for msg in messages():
        note("REQ %%s" %% msg.get("method"))
'''

FAKE_SCALA_CLI = "#!/bin/sh\nexit 0\n"


def _write_executable(path, text):
    with open(path, "w") as fh:
        fh.write(text)
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)


def _screen(w):
    return "\n".join(w.display(drain=0))


def _wait_for(w, predicate, timeout):
    deadline = time.monotonic() + timeout * WAIT_SCALE
    while time.monotonic() < deadline:
        w._drain(POLL_S)
        if predicate():
            return True
    return predicate()


def _read(path):
    try:
        with open(path) as fh:
            return fh.read()
    except OSError:
        return ""


def _stub_log(tmp_path):
    return _read(str(tmp_path / STUB_LOG))


def _stub_pid(tmp_path):
    try:
        return int(_read(str(tmp_path / STUB_PID)).strip() or 0)
    except ValueError:
        return 0


def _project(tmp_path):
    """The stand-ins and the project's `.bsp` file; the editor's PATH."""
    bindir = tmp_path / "bin"
    bindir.mkdir()
    server = str(bindir / "fake-bsp")
    _write_executable(server, FAKE_BSP % {"python": sys.executable,
                                          "dir": str(tmp_path),
                                          "log": STUB_LOG, "pid": STUB_PID,
                                          "release": STUB_RELEASE})
    _write_executable(str(bindir / "scala-cli"), FAKE_SCALA_CLI)
    bsp = tmp_path / ".bsp"
    bsp.mkdir()
    with open(str(bsp / "stub.json"), "w") as fh:
        json.dump({"name": "stub", "argv": [server], "version": "1",
                   "bspVersion": "2.1.1", "languages": ["scala"]}, fh)
    # The user already said Run it for this folder's build server (the
    # session's HOME is tmp_path): a .bsp file that came with a folder is
    # asked about first (tests/test_startcmd.py), these rows test the start.
    store = tmp_path / ".local" / "state" / "xwpe" / "start-commands"
    store.parent.mkdir(parents=True)
    store.write_text("xwpe-start-commands 1\nyes\tBSP/stub.json\t%s\t%s\n"
                     % (os.path.realpath(str(tmp_path)), server))
    return {"PATH": "%s:%s" % (bindir, os.environ.get("PATH", ""))}


def _release_the_stub(tmp_path):
    try:
        fd = os.open(str(tmp_path / STUB_RELEASE), os.O_WRONLY | os.O_NONBLOCK)
    except OSError:
        return
    try:
        os.write(fd, RELEASE_LINE)
    finally:
        os.close(fd)


def _end_the_stub(tmp_path):
    pid = _stub_pid(tmp_path)
    if pid <= 0:
        return
    try:
        os.kill(pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass


# @scenario SC-debug-93
def test_scala_debug_start_keeps_keys_while_the_build_server_answers(tmp_path):
    """Ctrl-G R on a Scala file while the build server holds its answer to
    `build/initialize`: keys typed meanwhile edit the file, the status row
    names the request waited for, and Esc ends the build server and says
    the start was cancelled."""
    try:
        with WpeSession(str(tmp_path), SEED, filename="p.scala", wait=1.5,
                        env_extra=_project(tmp_path)) as w:
            w.key(CTRL_G, "r", delay=0.8)
            assert _wait_for(w, lambda: "REQ build/initialize"
                             in _stub_log(tmp_path), ASKED_S), \
                "Ctrl-G R should start the build server:\n%s" % _screen(w)
            pid = _stub_pid(tmp_path)
            w.key(ALT_1, delay=0.4)
            w.key(TYPED, delay=0.6)
            assert _wait_for(w, lambda: (TYPED + "object P") in _screen(w),
                             SAID_S), \
                "keys typed while the build server answers should edit the " \
                "file:\n%s" % _screen(w)
            assert _wait_for(w, lambda: WAITING in _screen(w), SAID_S), \
                "the status row should name the request waited for:\n%s" \
                % _screen(w)
            w.key(ESC, delay=0.6)
            assert _wait_for(w, lambda: CANCELLED in _screen(w), SAID_S), \
                "Esc should cancel the start and say so:\n%s" % _screen(w)
            assert _wait_for(w, lambda: procinfo.run_state(pid) == procinfo.GONE,
                             GONE_S), \
                "the build server (pid %d) should be ended: %s" \
                % (pid, procinfo.run_state(pid))
            assert "buildTargets" not in _stub_log(tmp_path), \
                "nothing more should be asked after Esc"
            assert w.alive(), "the editor ended"
    finally:
        _release_the_stub(tmp_path)
        _end_the_stub(tmp_path)
