"""Every program the editor starts begins with SIGCHLD neither blocked nor
ignored.

The editor holds SIGCHLD back for a moment around some forks (the Java
program's VM under jdb, a command the assistant runs), so that the child is
its own to reap.  A signal mask survives exec, so a child forked in that
moment would run with SIGCHLD blocked unless it clears it, and a shell or a
debugger with SIGCHLD blocked may never notice its own children ending: a
build that hangs, a debugger that never sees the program stop.  These rows
read each child's own signal state while it runs.  gdb, jdb and the VM it
debugs are already running when asked, so their state is read from the
system's process table (Linux only, the rows skip by name elsewhere); the
compiler of a build, the program Run starts and an assistant command are
programs the row supplies, so they write their own state (the sigstate
program, or Python's signal module) on every system.  The last two are
started through /bin/sh, and dash clears its own mask when it starts, so on
such a system those two rows see what the program gets rather than what the
editor passed on; the others are executed directly.
"""
import os
import shutil
import signal
import subprocess
import sys
import time

import pytest

import gui_owner
import procinfo
from wpe_driver import WpeSession, ALT, WPE_BIN, WAIT_SCALE
from test_debugger_integration import (C_PROG, JAVA_PROG, _children, _need,
                                       _start_c_session, _start_java_session)

F9 = "\033[20~"                           # Make: compile and link
CTRL_F9 = "\033[20;5~"                    # Run: build, then run the program
POLL_S = 0.25
FILE_TIMEOUT_S = 20                       # a build, before scaling


def _need_running_process_state():
    """Skip the row, naming why, when this system cannot read the signal
    state of a process that is already running."""
    if procinfo.signal_state_of(os.getpid()) is None:
        pytest.skip("needs the signal state of a running process: /proc")


def _state_of(pid):
    """The signal state of a child the test's editor started."""
    state = procinfo.signal_state_of(pid)
    assert state is not None, "the child %d is gone" % pid
    return state


def _assert_sigchld_clear(what, state_text):
    blocked = procinfo.signal_names(state_text, procinfo.SIGSTATE_BLOCKED)
    ignored = procinfo.signal_names(state_text, procinfo.SIGSTATE_IGNORED)
    assert "CHLD" not in blocked, "%s runs with SIGCHLD blocked" % what
    assert "CHLD" not in ignored, "%s runs with SIGCHLD ignored" % what


def _wait_for_file(w, path, timeout):
    """Drain the editor until `path` exists and is not empty."""
    deadline = time.time() + timeout * WAIT_SCALE
    while time.time() < deadline:
        w._drain(POLL_S)
        if os.path.exists(path) and os.path.getsize(path) > 0:
            return True
    return False


def _only_child(w, name):
    pids = _children(w, name)
    assert len(pids) == 1, "the editor's one %s child: %s" % (name, pids)
    return pids[0]


# @scenario SC-debug-13
def test_gdb_starts_with_sigchld_unblocked(tmp_path):
    """gdb, stopped on a breakpoint, has SIGCHLD neither blocked nor
    ignored, so it sees its program stop and end."""
    _need("gcc", "gdb")
    _need_running_process_state()
    with WpeSession(str(tmp_path), C_PROG, filename="p.c", wait=1.5) as w:
        _start_c_session(w)
        state = _state_of(_only_child(w, "gdb"))
    _assert_sigchld_clear("gdb", state)


# @scenario SC-debug-13
def test_jdb_and_its_vm_start_with_sigchld_unblocked(tmp_path):
    """jdb and the VM the editor forks for the program (with SIGCHLD held
    while it is put on the watch list) both start with it cleared."""
    _need("jdb", "javac")
    _need_running_process_state()
    with WpeSession(str(tmp_path), JAVA_PROG, filename="P.java",
                    wait=1.5) as w:
        _start_java_session(w)
        jdb = _state_of(_only_child(w, "jdb"))
        vm = _state_of(_only_child(w, "java"))
    _assert_sigchld_clear("jdb", jdb)
    _assert_sigchld_clear("the program's VM", vm)


# A compiler that writes down its own signal state and then runs gcc.  Not a
# shell script: dash clears its signal mask when it starts, which would hide
# a mask the editor passed on.  It writes what sigstate does.
FAKE_GCC = ("#!%s\n"
            "import os, shutil, signal, sys\n"
            "def name(s):\n"
            "    try:\n"
            "        return signal.Signals(s).name[3:]\n"
            "    except ValueError:\n"
            "        return str(int(s))\n"
            "ignored = [s for s in signal.Signals"
            " if signal.getsignal(s) == signal.SIG_IGN]\n"
            "blocked = signal.pthread_sigmask(signal.SIG_BLOCK, [])\n"
            "with open(%r, 'w') as o:\n"
            "    o.write('IGNORED: ' + ' '.join(name(s) for s in ignored) + '\\n')\n"
            "    o.write('BLOCKED: ' + ' '.join(name(s) for s in blocked) + '\\n')\n"
            "os.execv(%r, [%r] + sys.argv[1:])\n")


# @scenario SC-compile-08
def test_build_compiler_starts_with_sigchld_unblocked(tmp_path):
    """The compiler F9 runs starts with SIGCHLD neither blocked nor
    ignored (gcc waits for cc1 and as through it)."""
    _need("gcc")
    bindir = tmp_path / "bin"
    bindir.mkdir()
    masks = tmp_path / "gcc-masks.txt"
    gcc = bindir / "gcc"
    real_gcc = shutil.which("gcc")
    gcc.write_text(FAKE_GCC % (sys.executable, str(masks), real_gcc,
                               real_gcc))
    gcc.chmod(0o755)
    env = {"PATH": "%s:%s" % (bindir, os.environ.get("PATH", ""))}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    filename="t.c", wait=1.5, env_extra=env) as w:
        w.key(F9, delay=0.5)
        built = _wait_for_file(w, str(masks), FILE_TIMEOUT_S)
    assert built, "the build never ran the compiler"
    _assert_sigchld_clear("the compiler", masks.read_text())


# A program that writes the signal state it was started with where Run starts
# it: it becomes sigstate, standard output going to the file.  {sigstate} is
# the program's path.
RUN_PROG = ('#include <stdio.h>\n#include <unistd.h>\n'
            'int main(void){if(!freopen("run-masks.txt","w",stdout))return 1;'
            'execl("{sigstate}","sigstate",(char*)0);return 1;}\n')


def _run_prog(tmp_path):
    """The source Run builds and starts: it hands over to sigstate."""
    return RUN_PROG.replace("{sigstate}", procinfo.sigstate_program(tmp_path))


# @scenario SC-compile-03
def test_run_program_starts_with_sigchld_unblocked(tmp_path):
    """The program Ctrl-F9 runs (the editor sets SIGCHLD to its default for
    the run) starts with it neither blocked nor ignored."""
    _need("gcc")
    masks = tmp_path / "run-masks.txt"
    with WpeSession(str(tmp_path), _run_prog(tmp_path), filename="r.c",
                    wait=1.5) as w:
        w.key(CTRL_F9, delay=0.5)
        ran = _wait_for_file(w, str(masks), FILE_TIMEOUT_S)
    assert ran, "Run never started the program"
    _assert_sigchld_clear("the program Run started", masks.read_text())


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"autonomous coding agent" in out
    except Exception:
        return False


# @scenario SC-ai-agent-40
@pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")
def test_ai_command_starts_with_sigchld_unblocked(tmp_path):
    """A command the assistant runs (the editor holds SIGCHLD while it forks
    and reaps it) starts with SIGCHLD neither blocked nor ignored."""
    masks = tmp_path / "ai-masks.txt"
    reply = ("TOOL run_command \"%s\" > ai-masks.txt@@TURN@@DONE it ran"
             % procinfo.sigstate_program(tmp_path))
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": reply, "XWPE_AI_POLICY": "auto"}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as w:
        w.key(ALT.AI)
        w.key("g")
        w.key("run it")
        w.key("\r", delay=0.5)
        ran = _wait_for_file(w, str(masks), FILE_TIMEOUT_S)
    assert ran, "the assistant never ran the command"
    _assert_sigchld_clear("the assistant's command", masks.read_text())


def _assert_interrupt_signals_default(what, state_text):
    """Neither SIGINT nor SIGPIPE is ignored: the editor ignores both, and a
    program started from a shell ignores neither."""
    ignored = procinfo.signal_names(state_text, procinfo.SIGSTATE_IGNORED)
    assert "INT" not in ignored, \
        "%s runs with SIGINT ignored, so Ctrl-C cannot stop it" % what
    assert "PIPE" not in ignored, \
        "%s runs with SIGPIPE ignored" % what


# A compiler that writes down which signals it starts with ignored.  A shell
# script here, not the Python one above: Python ignores SIGPIPE by itself,
# and dash keeps an ignored signal ignored but clears nothing about it.
# Its first %s is sigstate's path, the second the file, the third gcc.
FAKE_GCC_SH = ("#!/bin/sh\n"
               "\"%s\" > %s\n"
               "exec %s \"$@\"\n")


# @scenario SC-compile-08
def test_build_compiler_starts_with_sigint_and_sigpipe_default(tmp_path):
    """The compiler F9 runs inherits neither SIGINT nor SIGPIPE as ignored."""
    _need("gcc")
    bindir = tmp_path / "bin"
    bindir.mkdir()
    masks = tmp_path / "gcc-masks.txt"
    gcc = bindir / "gcc"
    real_gcc = shutil.which("gcc")
    gcc.write_text(FAKE_GCC_SH % (procinfo.sigstate_program(tmp_path),
                                  str(masks), real_gcc))
    gcc.chmod(0o755)
    env = {"PATH": "%s:%s" % (bindir, os.environ.get("PATH", ""))}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    filename="t.c", wait=1.5, env_extra=env) as w:
        w.key(F9, delay=0.5)
        built = _wait_for_file(w, str(masks), FILE_TIMEOUT_S)
    assert built, "the build never ran the compiler"
    _assert_interrupt_signals_default("the compiler", masks.read_text())


# @scenario SC-compile-03
def test_run_program_starts_with_sigint_and_sigpipe_default(tmp_path):
    """The program Ctrl-F9 runs inherits neither SIGINT nor SIGPIPE as
    ignored."""
    _need("gcc")
    masks = tmp_path / "run-masks.txt"
    with WpeSession(str(tmp_path), _run_prog(tmp_path), filename="r.c",
                    wait=1.5) as w:
        w.key(CTRL_F9, delay=0.5)
        ran = _wait_for_file(w, str(masks), FILE_TIMEOUT_S)
    assert ran, "Run never started the program"
    _assert_interrupt_signals_default("the program Run started",
                                      masks.read_text())


# A program that writes its PID and then loops until it is stopped.
LOOP_PROG = ('#include <stdio.h>\n#include <unistd.h>\n'
             'int main(void){FILE*o=fopen("loop.pid","w");'
             'fprintf(o,"%d",(int)getpid());fclose(o);'
             'for(;;)usleep(100000);return 0;}\n')


# @scenario SC-compile-03
def test_ctrl_c_stops_the_program_ctrl_f9_runs(tmp_path):
    """The run says "Ctrl-C to stop": Ctrl-C ends a program that loops."""
    _need("gcc")
    pidfile = tmp_path / "loop.pid"
    with WpeSession(str(tmp_path), LOOP_PROG, filename="l.c", wait=1.5) as w:
        w.key(CTRL_F9, delay=0.5)
        assert _wait_for_file(w, str(pidfile), FILE_TIMEOUT_S), \
            "Run never started the program"
        pid = int(pidfile.read_text())
        w.key("\x03", delay=0.5)
        deadline = time.time() + FILE_TIMEOUT_S * WAIT_SCALE
        while time.time() < deadline and gui_owner.process_running(pid):
            w._drain(POLL_S)
        alive = gui_owner.process_running(pid)
        if alive:
            os.kill(pid, signal.SIGKILL)
    assert not alive, "Ctrl-C did not stop the program (pid %d)" % pid
