"""Every program the editor starts begins with SIGCHLD neither blocked nor
ignored.

The editor holds SIGCHLD back for a moment around some forks (the Java
program's VM under jdb, a command the assistant runs), so that the child is
its own to reap.  A signal mask survives exec, so a child forked in that
moment would run with SIGCHLD blocked unless it clears it, and a shell or a
debugger with SIGCHLD blocked may never notice its own children ending: a
build that hangs, a debugger that never sees the program stop.  These rows
read each child's own mask from /proc/<pid>/status (Linux only) while it
runs: gdb, jdb and the VM it debugs, the compiler of a build, the program
Run starts, and an assistant command.  The last two are started through
/bin/sh, and dash clears its own mask when it starts, so on such a system
those two rows see what the program gets rather than what the editor
passed on; the others are executed directly.
"""
import os
import shutil
import signal
import subprocess
import sys
import time

import pytest

import procinfo
from wpe_driver import WpeSession, ALT, WPE_BIN, WAIT_SCALE
from test_debugger_integration import (C_PROG, JAVA_PROG, _children, _need,
                                       _start_c_session, _start_java_session)

pytestmark = pytest.mark.skipif(
    not procinfo.has_procfs(),
    reason="reads signal masks from /proc/<pid>/status, which only Linux has")

SIGCHLD_BIT = 1 << (signal.SIGCHLD - 1)   # its bit in SigBlk / SigIgn
F9 = "\033[20~"                           # Make: compile and link
CTRL_F9 = "\033[20;5~"                    # Run: build, then run the program
POLL_S = 0.25
FILE_TIMEOUT_S = 20                       # a build, before scaling


def _mask_words(status_text):
    """The SigBlk and SigIgn masks out of a /proc status text, as ints."""
    masks = {}
    for line in status_text.splitlines():
        key, _, value = line.partition(":")
        if key in ("SigBlk", "SigIgn"):
            masks[key] = int(value.strip(), 16)
    return masks


def _sigchld_state(status_text):
    """(blocked, ignored) for SIGCHLD in a /proc status text."""
    masks = _mask_words(status_text)
    assert "SigBlk" in masks and "SigIgn" in masks, \
        "no signal masks in:\n%s" % status_text
    return (bool(masks["SigBlk"] & SIGCHLD_BIT),
            bool(masks["SigIgn"] & SIGCHLD_BIT))


def _status_of(pid):
    """/proc/<pid>/status of a child the test's editor started."""
    with open("/proc/%d/status" % pid) as fh:
        return fh.read()


def _assert_sigchld_clear(what, status_text):
    blocked, ignored = _sigchld_state(status_text)
    assert not blocked, "%s runs with SIGCHLD blocked" % what
    assert not ignored, "%s runs with SIGCHLD ignored" % what


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
    with WpeSession(str(tmp_path), C_PROG, filename="p.c", wait=1.5) as w:
        _start_c_session(w)
        status = _status_of(_only_child(w, "gdb"))
    _assert_sigchld_clear("gdb", status)


# @scenario SC-debug-13
def test_jdb_and_its_vm_start_with_sigchld_unblocked(tmp_path):
    """jdb and the VM the editor forks for the program (with SIGCHLD held
    while it is put on the watch list) both start with it cleared."""
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), JAVA_PROG, filename="P.java",
                    wait=1.5) as w:
        _start_java_session(w)
        jdb = _status_of(_only_child(w, "jdb"))
        vm = _status_of(_only_child(w, "java"))
    _assert_sigchld_clear("jdb", jdb)
    _assert_sigchld_clear("the program's VM", vm)


# A compiler that writes down its own signal masks and then runs gcc.  Not
# a shell script: dash clears its signal mask when it starts, which would
# hide a mask the editor passed on.
FAKE_GCC = ("#!%s\n"
            "import os, shutil, sys\n"
            "with open('/proc/self/status') as i, open(%r, 'w') as o:\n"
            "    o.writelines(l for l in i if l.startswith(('SigBlk', 'SigIgn')))\n"
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


# A program that writes its own /proc status where Run starts it.
RUN_PROG = ('#include <stdio.h>\n'
            'int main(void){FILE*i=fopen("/proc/self/status","r");'
            'FILE*o=fopen("run-masks.txt","w");char l[256];'
            'if(!i||!o)return 1;'
            'while(fgets(l,sizeof l,i))fputs(l,o);'
            'fclose(i);fclose(o);return 0;}\n')


# @scenario SC-compile-03
def test_run_program_starts_with_sigchld_unblocked(tmp_path):
    """The program Ctrl-F9 runs (the editor sets SIGCHLD to its default for
    the run) starts with it neither blocked nor ignored."""
    _need("gcc")
    masks = tmp_path / "run-masks.txt"
    with WpeSession(str(tmp_path), RUN_PROG, filename="r.c", wait=1.5) as w:
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
    reply = ("TOOL run_command grep -E '^Sig(Blk|Ign):' /proc/self/status"
             " > ai-masks.txt@@TURN@@DONE it ran")
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
