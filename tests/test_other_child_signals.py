"""Servers, assistant commands and scripts start with their signals at the defaults.

The editor ignores SIGINT and SIGPIPE itself, and a signal set to ignore stays
ignored through fork and exec.  A language server, a command the assistant
runs, or a script run from the Run menu must not inherit that: an ignored
SIGPIPE makes a program print broken-pipe errors or spin after the editor
closes its pipe, and an ignored SIGINT makes Ctrl-C do nothing.  Each stand-in
here records the SigIgn line of its own /proc status; neither signal may be
in it.  (The build and Run children are covered in test_child_signals.py.)
"""
import os
import re
import stat
import subprocess
import time

import pytest

from wpe_driver import WpeSession, ALT, WPE_BIN

LSP_ALT_Q = "\033q"
SCRIPT_CTRL_F9 = "\033[20;5~"
ENTER = "\r"
SIGINT_MASK_BIT = 1 << (2 - 1)
SIGPIPE_MASK_BIT = 1 << (13 - 1)
RECORD_WAIT_S = 20
RECORD_POLL_S = 0.25
SEED_C_SOURCE = "int main(void){return 0;}\n"
SIGIGN_LINE = re.compile(r"^SigIgn:\s*([0-9a-fA-F]+)", re.M)
STANDIN_RECORD = "grep '^SigIgn' /proc/$$/status > \"$STANDIN_OUT\"\n"
FILES_F3 = "\033OR"
CTRL_C = "\x03"
CTRL_C_SETTLE_S = 2.0
LOOPING_SCRIPT = "#!/bin/sh\necho $$ > script.pid\nwhile :; do sleep 1; done\n"


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"autonomous coding agent" in out
    except Exception:
        return False


def _write_executable(path, text):
    with open(path, "w") as fh:
        fh.write(text)
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)


def _record_arrives(w, path):
    """Drain the editor until `path` holds a line, or the wait ends."""
    end = time.time() + RECORD_WAIT_S
    while time.time() < end:
        w._drain(RECORD_POLL_S)
        if os.path.exists(path) and os.path.getsize(path) > 0:
            return True
    return False


def _ignored_signals(path):
    """The SigIgn mask a stand-in recorded."""
    with open(path) as fh:
        found = SIGIGN_LINE.search(fh.read())
    assert found, "no SigIgn line recorded in %s" % path
    return int(found.group(1), 16)


def _assert_defaults(path, who):
    """Neither SIGINT nor SIGPIPE is ignored in what `who` recorded."""
    mask = _ignored_signals(path)
    assert not mask & SIGINT_MASK_BIT, \
        "%s inherited SIGINT ignored (%x)" % (who, mask)
    assert not mask & SIGPIPE_MASK_BIT, \
        "%s inherited SIGPIPE ignored (%x)" % (who, mask)


def test_a_language_server_starts_with_default_signals(tmp_path):
    """Alt-Q E starts the server for a C file; the stand-in named clangd
    records the dispositions it was started with."""
    bindir = tmp_path / "bin"
    bindir.mkdir()
    out = tmp_path / "lsp-sigign.txt"
    _write_executable(str(bindir / "clangd"),
                      "#!/bin/sh\n" + STANDIN_RECORD + "cat > /dev/null\n")
    env = {"PATH": "%s:%s" % (bindir, os.environ.get("PATH", "")),
           "STANDIN_OUT": str(out)}
    with WpeSession(str(tmp_path), SEED_C_SOURCE, filename="demo.c",
                    wait=1.5, env_extra=env) as w:
        w.key(LSP_ALT_Q, delay=0.4)
        w.key("e", delay=1.0)
        assert _record_arrives(w, str(out)), \
            "the stand-in server never started"
    _assert_defaults(str(out), "the language server")


@pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")
def test_a_command_the_assistant_runs_starts_with_default_signals(tmp_path):
    """The assistant's run_command tool runs a shell command; it records the
    dispositions it was started with."""
    out = tmp_path / "tool-sigign.txt"
    reply = ("TOOL run_command grep '^SigIgn' /proc/self/status > %s"
             "@@TURN@@DONE recorded" % out)
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": reply, "XWPE_AI_POLICY": "auto"}
    with WpeSession(str(tmp_path), SEED_C_SOURCE, env_extra=env) as w:
        w.key(ALT.AI)
        w.key("g")
        w.key("record it")
        w.key(ENTER, delay=0.5)
        assert _record_arrives(w, str(out)), \
            "the assistant's command never ran"
    _assert_defaults(str(out), "the assistant's command")


def test_a_script_run_from_the_editor_starts_with_default_signals(tmp_path):
    """Ctrl-F9 on a shell script runs it in the terminal; the script records
    the dispositions it was started with."""
    out = tmp_path / "script-sigign.txt"
    script = "#!/bin/sh\ngrep '^SigIgn' /proc/$$/status > %s\n" % out
    with WpeSession(str(tmp_path), script, filename="run.sh",
                    wait=1.5) as w:
        w.key(SCRIPT_CTRL_F9, delay=0.8)
        assert _record_arrives(w, str(out)), "the script never ran"
        w.key(ENTER, delay=0.4)
    _assert_defaults(str(out), "the script")


def _running(pid):
    """True while `pid` exists and is not a zombie."""
    try:
        with open("/proc/%d/status" % pid) as fh:
            return "State:\tZ" not in fh.read()
    except OSError:
        return False


def _kill_quietly(pid):
    """End a stand-in the row started; it may already be gone."""
    try:
        os.kill(pid, 9)
    except OSError:
        pass


def test_ctrl_c_that_stops_a_script_leaves_the_language_server_running(
        tmp_path):
    """A script run from the editor gets the terminal, and Ctrl-C stops it.
    The language server started earlier for a C file runs on: the Ctrl-C
    was meant for the script, and losing the server would silently end
    completion and diagnostics."""
    bindir = tmp_path / "bin"
    bindir.mkdir()
    server_pid = tmp_path / "server.pid"
    script_pid = tmp_path / "script.pid"
    _write_executable(str(bindir / "clangd"),
                      "#!/bin/sh\necho $$ > %s\nexec sleep 300\n" % server_pid)
    _write_executable(str(tmp_path / "run.sh"), LOOPING_SCRIPT)
    env = {"PATH": "%s:%s" % (bindir, os.environ.get("PATH", ""))}
    with WpeSession(str(tmp_path), SEED_C_SOURCE, filename="demo.c",
                    wait=1.5, env_extra=env) as w:
        w.key(LSP_ALT_Q, delay=0.4)
        w.key("e", delay=1.0)
        assert _record_arrives(w, str(server_pid)), \
            "the stand-in server never started"
        server = int(server_pid.read_text())
        w.key(FILES_F3, delay=0.8)
        w.key("run.sh", delay=0.3)
        w.key(ENTER, delay=1.0)
        w.key(SCRIPT_CTRL_F9, delay=0.5)
        assert _record_arrives(w, str(script_pid)), "the script never ran"
        script = int(script_pid.read_text())
        assert _running(server), "the server ended before the Ctrl-C"
        w.key(CTRL_C, delay=0.5)
        w._drain(CTRL_C_SETTLE_S)
        script_ran_on = _running(script)
        server_ran_on = _running(server)
        w.key(ENTER, delay=0.5)
        _kill_quietly(script)
        _kill_quietly(server)
    assert not script_ran_on, "Ctrl-C did not stop the script"
    assert server_ran_on, \
        "the Ctrl-C that stopped the script also ended the language server"
