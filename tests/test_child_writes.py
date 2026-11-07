"""Bytes for a child never freeze the editor: the language server's input.

The editor hands every message for a child to the loop's writer, which
writes what the child takes now and queues the rest.  A language server
that stops reading its input (busy indexing, wedged) leaves the editor
taking keys, the status row names it once it has read nothing for a while,
and the queued messages reach it whole and in order when it reads again.
A server that closes its input is reported, and its session ended, without
a signal reaching the editor and without a wait.

A stand-in `clangd` on PATH plays the server: it answers initialize and
then, as XWPE_STUB_LSP_MODE says, stops reading until the test writes to
its release pipe ("stop"), or closes its input ("close").  It writes its
PID, so every check is on an exact process.
"""
import os
import signal
import stat
import sys
import time

import procinfo
from wpe_driver import WpeSession, WAIT_SCALE

ALT_Q = "\033q"                   # the language server prefix
POLL_S = 0.2
SERVER_START_S = 20
STALL_SAID_S = 15                 # the writer's 5 s bound plus the loop's slack
SAID_S = 10
GONE_S = 8                        # the owner's 2 s grace, 1 s kill grace, slack
STUB_PID = "lsp-stub.pid"
STUB_LOG = "lsp-stub.log"
STUB_RELEASE = "lsp-stub.release"
STOPPED_NOTE = "STOPPED"
CLOSED_NOTE = "CLOSED"
RELEASE_LINE = b"go\n"
NOT_READING_LINE = "the language server is not reading its input"
INPUT_LOST_LINE = "Language server stopped reading its input"
TYPED = "zq"
# Far more than a pipe holds, so the didOpen fills it.
BIG_LINES = 20000
BIG_SEED = "".join("int v%06d;\n" % i for i in range(BIG_LINES))
SMALL_SEED = "int main(void){return 0;}\n"

FAKE_CLANGD = r'''#!%(python)s -I
import json, os, select, sys, time
here = %(dir)r
mode = os.environ.get("XWPE_STUB_LSP_MODE", "stop")
inp, out = sys.stdin.buffer, sys.stdout.buffer

def note(text):
    with open(os.path.join(here, %(log)r), "a") as fh:
        fh.write(text + "\n")

with open(os.path.join(here, %(pid)r), "w") as fh:
    fh.write(str(os.getpid()))
release_path = os.path.join(here, %(release)r)
os.mkfifo(release_path)
release = os.open(release_path, os.O_RDWR | os.O_NONBLOCK)

def answer(rid, result):
    body = json.dumps({"jsonrpc": "2.0", "id": rid, "result": result}).encode()
    out.write(b"Content-Length: %%d\r\n\r\n" %% len(body) + body)
    out.flush()

def message():
    length = None
    while True:
        line = inp.readline()
        if not line:
            return None
        line = line.strip()
        if not line:
            break
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":")[1])
    return json.loads(inp.read(length)) if length else {}

first = message()
if mode == "close":
    os.close(0)
    note(%(closed)r)
    answer(first["id"], {"capabilities": {}})
    while True:
        time.sleep(1)
answer(first["id"], {"capabilities": {}})
note(%(stopped)r)
select.select([release], [], [])
note("RELEASE")
while True:
    msg = message()
    if msg is None:
        break
    text = msg.get("params", {}).get("textDocument", {}).get("text")
    if text is None:
        note("MSG %%s -" %% msg.get("method"))
    else:
        note("MSG %%s %%d" %% (msg.get("method"), len(text)))
    if msg.get("method") == "shutdown":
        answer(msg["id"], None)
'''


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


def _server_env(tmp_path, mode):
    bindir = tmp_path / "bin"
    bindir.mkdir()
    _write_executable(str(bindir / "clangd"),
                      FAKE_CLANGD % {"python": sys.executable,
                                     "dir": str(tmp_path),
                                     "log": STUB_LOG, "pid": STUB_PID,
                                     "release": STUB_RELEASE,
                                     "stopped": STOPPED_NOTE,
                                     "closed": CLOSED_NOTE})
    return {"PATH": "%s:%s" % (bindir, os.environ.get("PATH", "")),
            "XWPE_LSP_NO_EAGER": "1", "XWPE_STUB_LSP_MODE": mode}


def _start_server_until(w, tmp_path, note):
    """Alt-Q E starts the server; wait until it says @note."""
    w.key(ALT_Q, delay=0.4)
    w.key("e", delay=0.1)
    return _wait_for(w, lambda: note in _stub_log(tmp_path), SERVER_START_S)


def _release_the_stub(tmp_path):
    """Let the stand-in read again; nothing when it is gone."""
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


def _messages_after_the_release(tmp_path):
    after = _stub_log(tmp_path).split("RELEASE", 1)
    if len(after) < 2:
        return []
    return [line[len("MSG "):] for line in after[1].splitlines()
            if line.startswith("MSG ")]


# @scenario SC-debug-77
def test_typing_goes_on_when_the_language_server_stops_reading(tmp_path):
    """A server that answers initialize and then reads nothing, sent a file
    far larger than its pipe holds: the editor goes on taking keys (the
    typed text lands in the file), the status row names the server as not
    reading its input, and once it reads again it gets `initialized` and
    the whole file, in that order."""
    try:
        with WpeSession(str(tmp_path), BIG_SEED, filename="a.c", wait=1.5,
                        env_extra=_server_env(tmp_path, "stop")) as w:
            assert _start_server_until(w, tmp_path, STOPPED_NOTE), \
                "Alt-Q E should start the server:\n%s" % _screen(w)
            w.key(TYPED, delay=0.6)
            assert _wait_for(w, lambda: (TYPED + "int v000000;") in _screen(w),
                             SAID_S), \
                "the typed text should land in the file:\n%s" % _screen(w)
            assert _wait_for(w, lambda: NOT_READING_LINE in _screen(w),
                             STALL_SAID_S), \
                "the status row should name the server:\n%s" % _screen(w)
            _release_the_stub(tmp_path)
            want = ["initialized -", "textDocument/didOpen %d" % len(BIG_SEED)]
            assert _wait_for(w, lambda: _messages_after_the_release(tmp_path)[:2]
                             == want, SAID_S), \
                "the server should read both messages whole and in order: %r" \
                % _messages_after_the_release(tmp_path)
            assert w.alive(), "the editor ended"
    finally:
        _release_the_stub(tmp_path)
        _end_the_stub(tmp_path)


# @scenario SC-debug-78
def test_a_language_server_that_closes_its_input_is_reported_and_ended(
        tmp_path):
    """A server that closes its input after initialize: the editor's next
    message finds the pipe closed.  The editor says the server stopped
    reading its input and ends the session; no signal ends the editor, it
    does not wait, and the server process is gone."""
    try:
        with WpeSession(str(tmp_path), SMALL_SEED, filename="a.c", wait=1.5,
                        env_extra=_server_env(tmp_path, "close")) as w:
            assert _start_server_until(w, tmp_path, CLOSED_NOTE), \
                "Alt-Q E should start the server:\n%s" % _screen(w)
            pid = _stub_pid(tmp_path)
            assert _wait_for(w, lambda: INPUT_LOST_LINE in _screen(w), SAID_S), \
                "the editor should say the server lost its input:\n%s" \
                % _screen(w)
            assert _wait_for(w, lambda: procinfo.run_state(pid) == procinfo.GONE,
                             GONE_S), \
                "the server (pid %d) should be ended: %s" \
                % (pid, procinfo.run_state(pid))
            assert w.alive(), "the editor ended"
    finally:
        _end_the_stub(tmp_path)
