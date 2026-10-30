"""The terminal editor keeps unsaved work when a signal ends it.

A "kill PID", a logout or a closed terminal window sends SIGTERM or SIGHUP.
The original xwpe saved its windows then; for a long time this one only
restored the terminal and left, and typed work was lost.  Now the editor
writes every window with unsaved changes to its .ESV file (which `wpe -r`
reopens) and then exits, wherever it was waiting: at the keyboard or in a
dialog.  A window with nothing unsaved writes no file, and a second signal
still ends an editor whose save hangs.
"""
import os
import shutil
import signal
import stat
import subprocess
import time

import pytest

from wpe_driver import ALT, WpeSession, WAIT_SCALE

# The editor must be gone this long after the signal.
EXIT_DEADLINE_S = 10 * WAIT_SCALE
# How long a save that cannot finish is given to show that it waits.
HUNG_SAVE_WATCH_S = 1.5 * WAIT_SCALE
TYPED = "hello signal"
F3 = "\033OR"
# What the editor prints, after the terminal is restored, for a window whose
# .ESV it could not write.
SAVE_FAILED_REPORT = b"could not be saved"


def type_into_buffer(w):
    """Type TYPED into the empty t.c, leaving it unsaved."""
    w.key(TYPED, delay=0.6)
    assert TYPED in "\n".join(w.display()), "the typed text never reached the buffer"


def wait_for_exit(w, seconds):
    """The exit code, or None when the editor is still running.  Reads the
    screen meanwhile, so the editor's last output can never fill the pty
    and hold it up."""
    deadline = time.time() + seconds
    while time.time() < deadline:
        code = w.proc.poll()
        if code is not None:
            return code
        w._drain(0.1)
    return None


def esv_text(workdir):
    """What the emergency save wrote for t.c."""
    saved = os.path.join(workdir, "t.c.ESV")
    assert os.path.exists(saved), sorted(os.listdir(workdir))
    with open(saved) as fh:
        return fh.read()


def signal_typed_work(tmp_path, sig):
    """Type into t.c, send ``sig`` to the editor, and check the work is in
    t.c.ESV once it has gone."""
    with WpeSession(str(tmp_path), "") as w:
        type_into_buffer(w)
        os.kill(w.proc.pid, sig)
        code = wait_for_exit(w, EXIT_DEADLINE_S)
        assert code is not None, "wpe still running %ds after the signal" % EXIT_DEADLINE_S
        assert code == -sig, "wpe should end as killed by the signal, got %r" % code
        assert TYPED in esv_text(str(tmp_path))


# @scenario SC-signal-save-01
def test_sigterm_keeps_typed_work_in_an_esv_file(tmp_path):
    signal_typed_work(tmp_path, signal.SIGTERM)


# @scenario SC-signal-save-02
def test_sighup_keeps_typed_work_in_an_esv_file(tmp_path):
    signal_typed_work(tmp_path, signal.SIGHUP)


# @scenario SC-signal-save-02
def test_closing_the_terminal_keeps_typed_work(tmp_path):
    """The terminal window goes away: the pty hangs up and the kernel sends
    SIGHUP to the editor, whose controlling terminal it was."""
    with WpeSession(str(tmp_path), "") as w:
        type_into_buffer(w)
        os.close(w.master_fd)
        try:
            w.proc.wait(timeout=EXIT_DEADLINE_S)
        except subprocess.TimeoutExpired:
            pass
        assert w.proc.poll() is not None, "wpe still running after its terminal closed"
        assert TYPED in esv_text(str(tmp_path))


# @scenario SC-signal-save-03
def test_sigterm_in_a_dialog_still_ends_and_saves(tmp_path):
    """The File Manager (F3) runs a key loop of its own; the signal must end
    the editor from there too."""
    with WpeSession(str(tmp_path), "") as w:
        type_into_buffer(w)
        w.key(F3, delay=1.0)
        assert "File-Manager" in "\n".join(w.display()), "the File Manager did not open"
        os.kill(w.proc.pid, signal.SIGTERM)
        code = wait_for_exit(w, EXIT_DEADLINE_S)
        assert code is not None, "wpe still running in the File Manager after SIGTERM"
        assert TYPED in esv_text(str(tmp_path))


# @scenario SC-signal-save-04
def test_sigterm_with_nothing_unsaved_writes_no_esv(tmp_path):
    with WpeSession(str(tmp_path), "int x;\n") as w:
        os.kill(w.proc.pid, signal.SIGTERM)
        code = wait_for_exit(w, EXIT_DEADLINE_S)
        assert code is not None, "wpe still running after SIGTERM"
        assert not os.path.exists(str(tmp_path / "t.c.ESV")), sorted(os.listdir(str(tmp_path)))


# @scenario SC-signal-save-05
def test_a_second_sigterm_ends_an_editor_whose_save_hangs(tmp_path):
    """t.c.ESV is a FIFO nobody reads, so writing it waits for ever.  The
    first SIGTERM starts the save, which hangs; the second one ends the
    editor at once, without going on with the rest of the exit (a hang
    that the signal does not interrupt would never let it finish)."""
    os.mkfifo(str(tmp_path / "t.c.ESV"))
    with WpeSession(str(tmp_path), "") as w:
        type_into_buffer(w)
        os.kill(w.proc.pid, signal.SIGTERM)
        assert wait_for_exit(w, HUNG_SAVE_WATCH_S) is None, \
            "the save should be waiting on the FIFO"
        before = len(w.raw)
        os.kill(w.proc.pid, signal.SIGTERM)
        code = wait_for_exit(w, EXIT_DEADLINE_S)
        assert code is not None, "a second SIGTERM did not end the editor"
        assert code == -signal.SIGTERM, code
        assert SAVE_FAILED_REPORT not in bytes(w.raw[before:]), \
            "the second SIGTERM let the exit go on instead of ending the editor"
        assert stat.S_ISFIFO(os.stat(str(tmp_path / "t.c.ESV")).st_mode)


# A Rust program debugged through a debug adapter that speaks DAP on its
# stdin and stdout, as Rust's gdb does.
DAP_RUST_PROG = "fn main() {\n    let x = 3;\n}\n"
# Writes the empty binary the -o option names, so no toolchain is needed.
FAKE_RUSTC = ("#!/bin/sh\n"
              "while [ $# -gt 0 ]; do\n"
              "  if [ \"$1\" = -o ]; then : > \"$2\"; fi\n"
              "  shift\n"
              "done\n")
# An adapter that answers the whole start and then never reports a stop, as
# a program that loops for ever would: the editor waits for that stop.  It
# writes the name of every request it answered to the log next to it.
NEVER_STOPPING_ADAPTER = r'''#!/usr/bin/env python3
import json, sys
LOG = %r
seq = 0
def send(msg):
    global seq
    seq += 1
    msg["seq"] = seq
    body = json.dumps(msg).encode()
    sys.stdout.buffer.write(b"Content-Length: %%d\r\n\r\n" %% len(body) + body)
    sys.stdout.buffer.flush()
def answer(req, body=None):
    send({"type": "response", "request_seq": req["seq"], "success": True,
          "command": req["command"], "body": body or {}})
    with open(LOG, "a") as fh:
        fh.write(req["command"] + "\n")
inp = sys.stdin.buffer
while True:
    length = 0
    while True:
        line = inp.readline()
        if not line:
            sys.exit(0)
        line = line.strip()
        if not line:
            break
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":")[1])
    req = json.loads(inp.read(length))
    cmd = req.get("command")
    if cmd == "launch":
        send({"type": "event", "event": "initialized"})
        answer(req)
    elif cmd in ("setBreakpoints", "setFunctionBreakpoints"):
        answer(req, {"breakpoints": []})
    elif cmd == "disconnect":
        answer(req)
        sys.exit(0)
    else:
        answer(req)
'''
ADAPTER_LOG = "adapter-requests.log"
ALT_1 = "\0331"            # window 1, the source
CTRL_G = "\x07"            # the debugger's prefix
NONAME = "Noname"          # the name File > New gives a window
DEBUGGER_START_S = 20 * WAIT_SCALE


def never_stopping_adapter_env(tmp_path):
    """`gdb` first on PATH as the adapter above, and a stand-in rustc."""
    bindir = tmp_path / "bin"
    bindir.mkdir()
    adapter = bindir / "gdb"
    adapter.write_text(NEVER_STOPPING_ADAPTER % str(tmp_path / ADAPTER_LOG))
    adapter.chmod(0o755)
    rustc = bindir / "rustc"
    rustc.write_text(FAKE_RUSTC)
    rustc.chmod(0o755)
    return {"PATH": str(bindir) + ":" + os.environ.get("PATH", "")}


def adapter_answered(tmp_path, command):
    log = tmp_path / ADAPTER_LOG
    return log.exists() and command in log.read_text().split()


# @scenario SC-signal-save-07
def test_sigterm_while_waiting_for_a_debug_adapter_stop(tmp_path):
    """Ctrl-G R under an adapter whose program never stops: the editor
    waits for the first stop and reads no keys.  SIGTERM still ends it and
    saves the window with unsaved changes (an untitled one, since Run saves
    the source it builds)."""
    with WpeSession(str(tmp_path), DAP_RUST_PROG, filename="main.rs",
                    env_extra=never_stopping_adapter_env(tmp_path)) as w:
        w.key(ALT.FILE, "n", delay=0.9)
        w.key(TYPED, delay=0.6)
        w.key(ALT_1, delay=0.4)
        w.key(CTRL_G, "r", delay=1.0)
        deadline = time.time() + DEBUGGER_START_S
        while not adapter_answered(tmp_path, "configurationDone") and time.time() < deadline:
            w._drain(0.2)
        assert adapter_answered(tmp_path, "configurationDone"), \
            "the debugger start did not reach the adapter:\n%s" % "\n".join(w.display())
        assert wait_for_exit(w, 1.0 * WAIT_SCALE) is None
        os.kill(w.proc.pid, signal.SIGTERM)
        code = wait_for_exit(w, EXIT_DEADLINE_S)
        assert code is not None, "wpe still waiting for the adapter after SIGTERM"
        saved = tmp_path / (NONAME + ".ESV")
        assert saved.exists(), sorted(os.listdir(str(tmp_path)))
        assert TYPED in saved.read_text()


# An adapter that stops at the start and then never answers disconnect and
# ignores SIGTERM: Quit waits for it as long as it may.  It writes its PID
# first and every request after, to the log next to it.
SILENT_QUIT_ADAPTER = r'''#!/usr/bin/env python3
import json, os, sys, signal
signal.signal(signal.SIGTERM, signal.SIG_IGN)
LOG = %r
SRC = %r
seq = 0
def log(text):
    with open(LOG, "a") as fh:
        fh.write(text + "\n")
def send(msg):
    global seq
    seq += 1
    msg["seq"] = seq
    body = json.dumps(msg).encode()
    sys.stdout.buffer.write(b"Content-Length: %%d\r\n\r\n" %% len(body) + body)
    sys.stdout.buffer.flush()
def answer(req, body=None):
    send({"type": "response", "request_seq": req["seq"], "success": True,
          "command": req["command"], "body": body or {}})
log("pid %%d" %% os.getpid())
inp = sys.stdin.buffer
while True:
    length = 0
    while True:
        line = inp.readline()
        if not line:
            while True:
                signal.pause()
        line = line.strip()
        if not line:
            break
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":")[1])
    req = json.loads(inp.read(length))
    cmd = req.get("command")
    log(cmd)
    if cmd == "launch":
        send({"type": "event", "event": "initialized"})
        answer(req)
    elif cmd in ("setBreakpoints", "setFunctionBreakpoints"):
        answer(req, {"breakpoints": []})
    elif cmd == "configurationDone":
        answer(req)
        send({"type": "event", "event": "stopped",
              "body": {"reason": "breakpoint", "threadId": 1}})
    elif cmd == "stackTrace":
        answer(req, {"stackFrames": [{"id": 1, "name": "main", "line": 2,
                                      "source": {"path": SRC}}]})
    elif cmd != "disconnect":
        answer(req)
'''
QUIT_SIGNAL_AFTER_S = 0.3   # the signal comes while Quit waits
SECOND_SIGNAL_AFTER_S = 0.5


def silent_quit_adapter_env(tmp_path):
    """`gdb` first on PATH as the adapter above, and a stand-in rustc."""
    bindir = tmp_path / "bin"
    bindir.mkdir()
    adapter = bindir / "gdb"
    adapter.write_text(SILENT_QUIT_ADAPTER % (str(tmp_path / ADAPTER_LOG),
                                              str(tmp_path / "main.rs")))
    adapter.chmod(0o755)
    rustc = bindir / "rustc"
    rustc.write_text(FAKE_RUSTC)
    rustc.chmod(0o755)
    return {"PATH": str(bindir) + ":" + os.environ.get("PATH", "")}


def end_leftover_adapter(tmp_path):
    """A second signal ends the editor before it ends the adapter, which
    ignores SIGTERM: end it here, by the PID it logged."""
    log = tmp_path / ADAPTER_LOG
    if not log.exists():
        return
    for line in log.read_text().split("\n"):
        if line.startswith("pid "):
            try:
                os.kill(int(line.split()[1]), signal.SIGKILL)
            except ProcessLookupError:
                pass


# @scenario SC-signal-save-08
@pytest.mark.parametrize("signals", [1, 2])
def test_sigterm_during_debug_quit_saves_first(tmp_path, signals):
    """Ctrl-G Q at a stop, under an adapter that never answers disconnect:
    Quit waits for it, bounded.  A SIGTERM then still saves the unsaved
    window at once, and a second SIGTERM, which ends the editor without
    any save, finds the work already on disk."""
    try:
        with WpeSession(str(tmp_path), DAP_RUST_PROG, filename="main.rs",
                        env_extra=silent_quit_adapter_env(tmp_path)) as w:
            w.key(ALT.FILE, "n", delay=0.9)
            w.key(TYPED, delay=0.6)
            w.key(ALT_1, delay=0.4)
            w.key(CTRL_G, "r", delay=1.0)
            deadline = time.time() + DEBUGGER_START_S
            while not adapter_answered(tmp_path, "stackTrace") and \
                    time.time() < deadline:
                w._drain(0.2)
            assert adapter_answered(tmp_path, "stackTrace"), \
                "the session did not reach its first stop:\n%s" \
                % "\n".join(w.display())
            w._drain(1.0)
            w.key(CTRL_G, "q", delay=0.0)
            w._drain(QUIT_SIGNAL_AFTER_S)
            os.kill(w.proc.pid, signal.SIGTERM)
            if signals == 2:
                w._drain(SECOND_SIGNAL_AFTER_S)
                os.kill(w.proc.pid, signal.SIGTERM)
            code = wait_for_exit(w, EXIT_DEADLINE_S)
            assert code is not None, "wpe still running after the signal"
            saved = tmp_path / (NONAME + ".ESV")
            assert saved.exists(), "the work was not saved: %s" \
                % sorted(os.listdir(str(tmp_path)))
            assert TYPED in saved.read_text()
    finally:
        end_leftover_adapter(tmp_path)


# Programs that write their own PID, stop on a breakpoint, then sleep for
# ten minutes: a program left behind by the editor's end is found by it.
PROG_PID_FILE = "prog.pid"
LEFT_BEHIND_PROGS = {
    "lldb-dap": {
        "file": "main.rs", "line": 3, "need": ("rustc", "lldb-dap"),
        "env": {"XWPE_DAP_ADAPTER": "lldb"}, "start_s": 60,
        "prog": "fn main() {\n"
                "    std::fs::write(\"%s\", std::process::id().to_string())"
                ".unwrap();\n"
                "    println!(\"ready\");\n"
                "    std::thread::sleep("
                "std::time::Duration::from_secs(600));\n"
                "    let x = 3;\n"
                "    println!(\"{}\", x);\n"
                "}\n"},
    "dlv": {
        "file": "main.go", "line": 12, "need": ("dlv", "go"),
        "env": {}, "start_s": 60,
        "prog": "package main\n\nimport (\n\t\"fmt\"\n\t\"os\"\n"
                "\t\"strconv\"\n\t\"time\"\n)\n\n"
                "func main() {\n"
                "\tos.WriteFile(\"%s\", []byte(strconv.Itoa(os.Getpid())), "
                "0644)\n"
                "\tfmt.Println(\"ready\")\n"
                "\ttime.Sleep(600 * time.Second)\n"
                "\tx := 3\n"
                "\tfmt.Println(x)\n"
                "}\n"},
}
DOWN = "\033[B"
PROGRAM_GONE_S = 10 * WAIT_SCALE
RUNNING_SETTLE_S = 3


def program_pid(tmp_path):
    path = tmp_path / PROG_PID_FILE
    if not path.exists():
        return None
    text = path.read_text().strip()
    return int(text) if text.isdigit() else None


def pid_alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


def wait_program_gone(pid):
    deadline = time.time() + PROGRAM_GONE_S
    while time.time() < deadline and pid_alive(pid):
        time.sleep(0.1)
    return not pid_alive(pid)


def end_left_program(pid):
    """A program the editor left behind: end it, by its own PID."""
    if pid and pid_alive(pid):
        os.kill(pid, signal.SIGKILL)


# @scenario SC-signal-save-09
@pytest.mark.parametrize("state", ["stopped", "running"])
@pytest.mark.parametrize("adapter", ["lldb-dap", "dlv"])
def test_sigterm_in_a_debug_session_leaves_no_program(tmp_path, adapter,
                                                      state):
    """A logout or "kill PID" during an adapter session, at a stop or while
    the program runs: the editor ends, and so does the program it was
    debugging.  Only [lldb-dap-running] needs the editor to end the
    session itself (lldb-dap ends a running program only when told to);
    the other three also pass when the editor just goes, because dlv and a
    paused lldb-dap end the program when their client leaves.  They stay
    as guards."""
    spec = LEFT_BEHIND_PROGS[adapter]
    for tool in spec["need"]:
        if shutil.which(tool) is None:
            pytest.skip("no %s" % tool)
    if adapter == "dlv":
        (tmp_path / "go.mod").write_text("module leftbehind\n\ngo 1.21\n")
    prog = spec["prog"] % str(tmp_path / PROG_PID_FILE)
    pid = None
    try:
        with WpeSession(str(tmp_path), prog, filename=spec["file"],
                        wait=2.0, env_extra=spec["env"] or None) as w:
            for _ in range(spec["line"] - 1):
                w.key(DOWN, delay=0.1)
            w.key(CTRL_G, "b", delay=0.5)
            w.key(CTRL_G, "r", delay=1.0)
            deadline = time.time() + spec["start_s"] * WAIT_SCALE
            while w.mark_line() != spec["line"] and time.time() < deadline:
                w._drain(0.2)
            assert w.mark_line() == spec["line"], \
                "no stop on line %d:\n%s" % (spec["line"],
                                             "\n".join(w.display()))
            pid = program_pid(tmp_path)
            assert pid and pid_alive(pid), "the program wrote no PID"
            if state == "running":
                w.key(CTRL_G, "r", delay=0.5)
                w._drain(RUNNING_SETTLE_S)
            os.kill(w.proc.pid, signal.SIGTERM)
            code = wait_for_exit(w, EXIT_DEADLINE_S)
            assert code is not None, "wpe still running after SIGTERM"
        assert wait_program_gone(pid), \
            "the debugged program (PID %d) was left running" % pid
    finally:
        end_left_program(pid)
