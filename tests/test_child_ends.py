"""A child's end is an event: the build's, the language server's, and the
Scala build's setup.

The editor starts its children through the children's owner (we_child.c),
which learns of each end from the SIGCHLD handler and reports it from the
loop with the child's own wait status; nobody asks waitpid, and nobody
waits.

- The build: a compiler that starts twenty short processes which end with
  it, and fails with exit status 3 printing nothing the error parser knows,
  is reported as failed with that exit status.
- The language server: one that ignores SIGTERM, and a helper it started,
  are ended on the owner's ladder when the editor is done with them; the
  editor goes on at once, and neither outlives the editor.
- `scala-cli setup-ide`: it runs as a child of the editor's, so the keys
  typed meanwhile reach the editor.

Stand-ins on PATH play the compiler (`gcc`), the server (`clangd`, `metals`)
and `scala-cli`; each writes its own PID, so every check is on an exact
process.
"""
import os
import signal
import stat
import sys
import time

import procinfo
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


# ---- the language server, and scala-cli setup-ide -------------------------
#
# A stand-in `clangd` on PATH that ignores SIGTERM and the end of its input,
# answers initialize and shutdown (so closing it is quick), and starts a
# helper of its own that ignores SIGTERM too, as a server's JVM or indexer
# would; both write their PIDs, so every check is on an exact process.

ALT_Q = "\033q"                   # the language server prefix
F3 = "\033OR"                     # the File Manager
ENTER = "\r"
GRACE_S = 2.0                     # the owner's grace after SIGTERM (we_child.h)
KILL_GRACE_S = 1.0                # after SIGKILL, before "did not end"
GONE_MARGIN_S = 3.0               # the loop's slack on a loaded machine
SERVER_GONE_S = GRACE_S + KILL_GRACE_S + GONE_MARGIN_S
SERVER_START_S = 20
STILL_ENDING_S = 0.3              # well inside the grace
EDITOR_END_S = 15
DID_NOT_END = "did not end"
SERVER_PID_PREFIX = "lsp-"
HELPER_PID_PREFIX = "helper-"
PID_SUFFIX = ".pid"

FAKE_CLANGD = r'''#!%(python)s -I
import json, os, signal, sys, time
signal.signal(signal.SIGTERM, signal.SIG_IGN)
here = %(dir)r
me = os.getpid()

def note(prefix, pid):
    with open(os.path.join(here, "%%s%%d.pid" %% (prefix, me)), "w") as fh:
        fh.write(str(pid))

helper = os.fork()
if helper == 0:
    os.close(0)
    os.close(1)
    while True:
        time.sleep(1)
note(%(helper)r, helper)
note(%(server)r, me)

inp, out = sys.stdin.buffer, sys.stdout.buffer

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

while True:
    msg = message()
    if msg is None:
        break
    if msg.get("method") == "initialize":
        answer(msg["id"], {"capabilities": {}})
    elif msg.get("method") == "shutdown":
        answer(msg["id"], None)
while True:
    time.sleep(1)
'''


def _gone(pid):
    """Not in the process table: ended and reaped, not a zombie."""
    return procinfo.run_state(pid) == procinfo.GONE


def _pids(tmp_path, prefix):
    """The PIDs the stand-ins wrote under @prefix, in start order."""
    found = []
    for name in os.listdir(str(tmp_path)):
        if name.startswith(prefix) and name.endswith(PID_SUFFIX):
            path = tmp_path / name
            found.append((path.stat().st_mtime, _read_pid(str(path))))
    return [pid for _, pid in sorted(found) if pid > 0]


def _server_env(tmp_path):
    bindir = tmp_path / "bin"
    bindir.mkdir()
    _write_executable(str(bindir / "clangd"),
                      FAKE_CLANGD % {"python": sys.executable,
                                     "dir": str(tmp_path),
                                     "server": SERVER_PID_PREFIX,
                                     "helper": HELPER_PID_PREFIX})
    return {"PATH": "%s:%s" % (bindir, os.environ.get("PATH", "")),
            "XWPE_LSP_NO_EAGER": "1"}


def _kill_quietly(pids):
    for pid in pids:
        try:
            os.kill(pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass


def _open_file(w, name):
    """Open @name through the File Manager, as the user opens a second file."""
    w.key(F3, delay=1.2)
    w.key(name, delay=0.6)
    w.key(ENTER, delay=1.2)


def _start_server(w, tmp_path, count):
    """Alt-Q E in the window on top starts its server; wait until @count
    servers have started in all."""
    w.key(ALT_Q, delay=0.4)
    w.key("e", delay=0.1)
    return _wait_for(w, lambda: len(_pids(tmp_path, SERVER_PID_PREFIX)) >= count,
                     SERVER_START_S)


# @scenario SC-debug-75
def test_a_language_server_that_ignores_sigterm_is_gone_after_the_grace_and_quit_took_no_time(
        tmp_path):
    """Alt-Q E in a second file replaces a server that ignores SIGTERM: the
    old one is still being ended while the editor goes on (guard: the editor
    did not wait for it), and once the grace is over it is gone, with the
    helper it started, by their PIDs, and nobody reads "did not end"."""
    (tmp_path / "b.c").write_text(SEED_C)
    started = []
    try:
        with WpeSession(str(tmp_path), SEED_C, filename="a.c", wait=1.5,
                        env_extra=_server_env(tmp_path)) as w:
            assert _start_server(w, tmp_path, 1), \
                "Alt-Q E in a.c should start its server:\n%s" % _screen(w)
            first = _pids(tmp_path, SERVER_PID_PREFIX)[0]
            helper = _pids(tmp_path, HELPER_PID_PREFIX)[0]
            started += [first, helper]
            _open_file(w, "b.c")
            w.key(ALT_Q, delay=0.4)
            w.key("e", delay=STILL_ENDING_S)
            assert not _gone(first), \
                "the editor waited for the old server (pid %d) to end" % first
            assert _wait_for(w, lambda: len(_pids(tmp_path,
                                                  SERVER_PID_PREFIX)) >= 2,
                             SERVER_START_S), \
                "Alt-Q E in b.c should start a server for it:\n%s" % _screen(w)
            started += _pids(tmp_path, SERVER_PID_PREFIX)[1:]
            started += _pids(tmp_path, HELPER_PID_PREFIX)[1:]
            assert _wait_for(w, lambda: _gone(first) and _gone(helper),
                             SERVER_GONE_S), \
                "the old server (pid %d, %s) and its helper (pid %d, %s) " \
                "should be gone after the grace" \
                % (first, procinfo.run_state(first),
                   helper, procinfo.run_state(helper))
            assert DID_NOT_END not in _screen(w), _screen(w)
            assert w.alive(), "the editor ended"
    finally:
        _kill_quietly(started)


# @scenario SC-debug-75
def test_a_language_server_that_ignores_sigterm_does_not_outlive_the_editor(
        tmp_path):
    """SIGTERM to the editor with a server that ignores SIGTERM and a helper
    of its own: the editor ends within its bound, and neither the server nor
    the helper is left behind."""
    started = []
    try:
        with WpeSession(str(tmp_path), SEED_C, filename="a.c", wait=1.5,
                        env_extra=_server_env(tmp_path)) as w:
            assert _start_server(w, tmp_path, 1), \
                "Alt-Q E should start the server:\n%s" % _screen(w)
            server = _pids(tmp_path, SERVER_PID_PREFIX)[0]
            helper = _pids(tmp_path, HELPER_PID_PREFIX)[0]
            started += [server, helper]
            os.kill(w.proc.pid, signal.SIGTERM)
            assert _wait_for(w, lambda: w.proc.poll() is not None,
                             EDITOR_END_S), "the editor did not end"
        assert _wait_for_gone([server, helper], GONE_MARGIN_S), \
            "left behind by the editor: server %d (%s), helper %d (%s)" \
            % (server, procinfo.run_state(server),
               helper, procinfo.run_state(helper))
    finally:
        _kill_quietly(started)


def _wait_for_gone(pids, timeout):
    """Wait, without the editor, until every one of @pids is gone."""
    deadline = time.time() + timeout * WAIT_SCALE
    while time.time() < deadline:
        if all(_gone(pid) for pid in pids):
            return True
        time.sleep(POLL_S)
    return all(_gone(pid) for pid in pids)


# A stand-in `metals` that, as Metals does through scala-cli, starts a shared
# build server (Bloop) in its own session without a session of the daemon's
# own, and ends at `exit`; the daemon is other editors' too, so it must
# outlive the language server.  Its PID goes in DAEMON_PID.
FAKE_METALS_WITH_DAEMON = r'''#!%(python)s -I
import json, os, sys, time
daemon = os.fork()
if daemon == 0:
    os.close(0)
    os.close(1)
    for _ in range(%(lives)d):
        time.sleep(1)
    os._exit(0)
with open(os.environ["DAEMON_PID"], "w") as fh:
    fh.write(str(daemon))
inp, out = sys.stdin.buffer, sys.stdout.buffer

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

while True:
    msg = message()
    if msg is None or msg.get("method") == "exit":
        sys.exit(0)
    if msg.get("method") == "initialize":
        answer(msg["id"], {"capabilities": {}})
    elif msg.get("method") == "shutdown":
        answer(msg["id"], None)
'''
DAEMON_LIVES_S = 60               # far longer than the row
DAEMON_START_S = 20
SHARED_DAEMON_CHECK_S = GRACE_S + KILL_GRACE_S + GONE_MARGIN_S
SETUP_IDE_DONE = "#!/bin/sh\nexit 0\n"


def _metals_daemon_env(tmp_path):
    bindir = tmp_path / "bin"
    bindir.mkdir()
    _write_executable(str(bindir / "scala-cli"), SETUP_IDE_DONE)
    _write_executable(str(bindir / "metals"),
                      FAKE_METALS_WITH_DAEMON % {"python": sys.executable,
                                                 "lives": DAEMON_LIVES_S})
    return {"PATH": "%s:%s" % (bindir, os.environ.get("PATH", "")),
            "XWPE_LSP_NO_EAGER": "1",
            "DAEMON_PID": str(tmp_path / "daemon.pid")}


# @scenario SC-debug-75
def test_the_build_server_metals_started_outlives_metals_and_the_editor(
        tmp_path):
    """Metals started for a Scala file starts the shared build server in
    its own session; SIGTERM to the editor ends Metals, and the build server,
    which other editors and the Scala debugger use, is still running after
    the editor and the owner's grace are over."""
    daemon = 0
    try:
        with WpeSession(str(tmp_path), SCALA_SEED, filename="A.scala",
                        wait=1.5, env_extra=_metals_daemon_env(tmp_path)) as w:
            w.key(ALT_Q, delay=0.4)
            w.key("e", delay=0.3)
            assert _wait_for(w, lambda: _read_pid(str(tmp_path /
                                                      "daemon.pid")),
                             DAEMON_START_S), \
                "Alt-Q E should start Metals:\n%s" % _screen(w)
            daemon = _read_pid(str(tmp_path / "daemon.pid"))
            os.kill(w.proc.pid, signal.SIGTERM)
            assert _wait_for(w, lambda: w.proc.poll() is not None,
                             EDITOR_END_S), "the editor did not end"
        time.sleep(SHARED_DAEMON_CHECK_S * WAIT_SCALE)
        assert _pid_alive(daemon), \
            "the build server Metals started (pid %d) was ended with it" \
            % daemon
    finally:
        _kill_quietly([daemon] if daemon else [])


# A stand-in `scala-cli` that sleeps through `setup-ide`, and writes when it
# began, so the row knows the setup runs while it types.
FAKE_SCALA_CLI = """#!/bin/sh
echo $$ > "$SETUP_PID"
exec sleep %(sleep)d
"""
SETUP_SLEEP_S = 30
SETUP_START_S = 10
TYPED_DURING_SETUP = "zq"
SCALA_SEED = "object A { def main(a: Array[String]): Unit = () }\n"


def _scala_setup_env(tmp_path):
    bindir = tmp_path / "bin"
    bindir.mkdir()
    _write_executable(str(bindir / "scala-cli"),
                      FAKE_SCALA_CLI % {"sleep": SETUP_SLEEP_S})
    _write_executable(str(bindir / "metals"),
                      FAKE_CLANGD % {"python": sys.executable,
                                     "dir": str(tmp_path),
                                     "server": SERVER_PID_PREFIX,
                                     "helper": HELPER_PID_PREFIX})
    return {"PATH": "%s:%s" % (bindir, os.environ.get("PATH", "")),
            "XWPE_LSP_NO_EAGER": "1",
            "SETUP_PID": str(tmp_path / "setup.pid")}


# @scenario SC-debug-76
def test_keys_reach_the_editor_while_scala_cli_setup_ide_runs(tmp_path):
    """Alt-Q E in a Scala file runs `scala-cli setup-ide` first; with a
    stand-in that sleeps, the letters typed meanwhile go into the file, and
    no language server has started yet."""
    setup = 0
    started = []
    try:
        with WpeSession(str(tmp_path), SCALA_SEED, filename="A.scala",
                        wait=1.5, env_extra=_scala_setup_env(tmp_path)) as w:
            w.key(ALT_Q, delay=0.4)
            w.key("e", delay=0.3)
            assert _wait_for(w, lambda: _read_pid(str(tmp_path / "setup.pid")),
                             SETUP_START_S), \
                "Alt-Q E should start scala-cli setup-ide:\n%s" % _screen(w)
            setup = _read_pid(str(tmp_path / "setup.pid"))
            w.key(*TYPED_DURING_SETUP, delay=0.1)
            w.save()
            assert TYPED_DURING_SETUP in w.text(), \
                "keys typed while setup-ide runs should edit the file:\n%s" \
                % _screen(w)
            assert _pid_alive(setup), "the setup should still be running"
            assert not _pids(tmp_path, SERVER_PID_PREFIX), \
                "Metals should start only after setup-ide has ended"
            started += _pids(tmp_path, SERVER_PID_PREFIX)
            started += _pids(tmp_path, HELPER_PID_PREFIX)
    finally:
        _kill_quietly(started + ([setup] if setup else []))


SETUP_CANCELLED = "scala-cli setup-ide cancelled"


# @scenario SC-debug-76
def test_esc_cancels_scala_cli_setup_ide_and_nothing_starts_after(tmp_path):
    """Esc while `scala-cli setup-ide` runs ends it, says so, and Metals
    never starts for it; the editor's end, with a setup running, leaves no
    setup behind."""
    setup = 0
    started = []
    try:
        with WpeSession(str(tmp_path), SCALA_SEED, filename="A.scala",
                        wait=1.5, env_extra=_scala_setup_env(tmp_path)) as w:
            w.key(ALT_Q, delay=0.4)
            w.key("e", delay=0.3)
            assert _wait_for(w, lambda: _read_pid(str(tmp_path / "setup.pid")),
                             SETUP_START_S), \
                "Alt-Q E should start scala-cli setup-ide:\n%s" % _screen(w)
            setup = _read_pid(str(tmp_path / "setup.pid"))
            w.key(ESC, delay=0.3)
            assert _wait_for(w, lambda: SETUP_CANCELLED in _screen(w),
                             SETUP_START_S), _screen(w)
            assert _wait_for(w, lambda: _gone(setup), SERVER_GONE_S), \
                "the cancelled setup (pid %d) should end" % setup
            w._drain(GRACE_S)
            assert not _pids(tmp_path, SERVER_PID_PREFIX), \
                "Metals should not start after a cancelled setup"
            os.unlink(str(tmp_path / "setup.pid"))
            w.key(ALT_Q, delay=0.4)
            w.key("e", delay=0.3)
            assert _wait_for(w, lambda: _read_pid(str(tmp_path / "setup.pid")),
                             SETUP_START_S), \
                "Alt-Q E again should start the setup again:\n%s" % _screen(w)
            setup = _read_pid(str(tmp_path / "setup.pid"))
            os.kill(w.proc.pid, signal.SIGTERM)
            assert _wait_for(w, lambda: w.proc.poll() is not None,
                             EDITOR_END_S), "the editor did not end"
        assert _wait_for_gone([setup], GONE_MARGIN_S), \
            "the setup (pid %d) outlived the editor" % setup
        started += _pids(tmp_path, SERVER_PID_PREFIX)
        started += _pids(tmp_path, HELPER_PID_PREFIX)
        assert not started, "Metals started after the setup was ended"
    finally:
        _kill_quietly(started + ([setup] if setup else []))
