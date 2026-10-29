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
import signal
import stat
import subprocess
import time

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
