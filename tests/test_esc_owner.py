"""Rows for the owner of the key that ends a running activity.

G1-G20 pin the behaviour the owner carries: each row asserts the owner's
trace line, the module's own cancelled line, and that the other activity
still runs (its process is alive).  The three rows that press Esc during a
hosted turn (G9, G17, G19) need a build with the agent host in it and drive a
stub engine on the editor's own protocol, never a real one.
P1-P12 are the order the owner takes later: each is xfail(strict) until then
and names that pass in its reason.

Slow activities are stub tools on PATH that wait on a FIFO the test controls.
A second Esc is sent only after the first activity's process is gone (a
process-gone wait), never after a sleep.
"""
import os
import stat
import signal
import pytest
import sys
import shutil

import gui_owner
import procinfo
from wpe_driver import WpeSession, ALT, wait_until, wait_screen, ui_trace_tail
from test_ai_async import _start_edit

# Reuse the existing stub helpers from other test modules
from test_debugger_integration import (
    STUBS_DIR, GDB_STUB_LOG, GDB_STUB_RELEASE,
    _gdb_stub_env, _gdb_stub_holds, _gdb_stub_release, _gdb_stub_log,
    WAITING_FOR_SET_UP, GDB_DID_NOT_ANSWER_SET_UP, _need, _screen, _wait_for,
    _status_row_says, _sgr, _click_bottom_bar, _go_to_line, _move_down,
    ESC, CTRL_C, F9, ALT_1, GDB_GLOBAL_PROG, GDB_GLOBAL_BREAK_LINE, GDB_SET_UP_FIRST, STEP_TIMEOUT_S,
)
from test_compile_and_messages import active_title
from test_child_ends import (
    FAKE_CLANGD, FAKE_SCALA_CLI, FAKE_GCC_MEMBER_DEAF,
    _write_executable, _scala_setup_env,
    _screen as _child_screen, _wait_for as _child_wait_for,
    SCALA_SEED, SETUP_START_S, SERVER_GONE_S, GRACE_S, SETUP_CANCELLED,
)
from test_bsp_start import (
    _project as _bsp_project, WAITING as BSP_WAITING, CANCELLED as BSP_CANCELLED,
    _screen as _bsp_screen, _wait_for as _bsp_wait_for, SEED as BSP_SEED,
)
from test_debugger_ends import (
    _slow_rustc_env, _read_pid, _gone, _wait_gone,
    RUSTC_PID_FILE, SLOW_RUSTC_S,
)
from test_debugger_integration import DAP_RUST_PROG
from test_lsp_on_the_loop import (
    WAITING_FOR, START_WAITING, START_CANCELLED,
    _server_env as _lsp_server_env, _screen as _lsp_screen,
    _wait_for as _lsp_wait_for, _stub_log as _lsp_stub_log,
)


WPE_BIN = os.environ.get("WPE_BIN") or os.path.join(os.path.dirname(__file__), "..", "wpe")
ALT_A = "\033a"
ALT_Q = "\033q"
CTRL_G = "\x07"
F6 = "\033[17~"


def _trace_path(tmp_path):
    return str(tmp_path / "ui.trace")


def _trace_contains(trace_path, needle, timeout=12):
    """Poll the trace file for @needle."""
    try:
        gui_owner.wait_for(
            lambda: os.path.exists(trace_path) and needle in open(trace_path, errors="replace").read(),
            f"trace to contain {needle!r}",
            timeout)
        return True
    except gui_owner.OwnerTimeout:
        return False


def _cancel_line(what, key="esc", state=""):
    """The line the owner of the key writes when the claim @what took it.

    @key is the key as the owner names it ("esc", "ctrl-c", or "esc-click"
    for the Esc a click on the bottom row comes back as); @state is what the
    key did not do ("ending": the claim's end was already on its way,
    "held": the cancel delivered a result that had ended behind a box)."""
    line = "e_cancel_key what=%s key=%s" % (what, key)
    return line + " " + state if state else line


def _trace_says_cancel(trace_path, what, key="esc", state="", timeout=12):
    """Wait for the owner's line naming the claim @what, and say whether it
    came.  A row asserts the module's own line first, so the trace is already
    written by the time this is asked."""
    return _trace_contains(trace_path, _cancel_line(what, key, state), timeout)


def _trace_cancel_lines(trace_path):
    """The owner's lines in the trace, in the order it wrote them.

    A row that presses a key no claim may take (a dialog's own Esc, an Esc
    while a debugged program reads what is typed for it) reads this to prove
    the owner took nothing."""
    try:
        with open(trace_path, errors="replace") as fh:
            text = fh.read()
    except OSError:
        return []
    return [line for line in text.splitlines()
            if line.startswith("e_cancel_key ")]


def _process_gone(pid, timeout=12):
    """Wait for a PID to be gone."""
    try:
        gui_owner.wait_for(
            lambda: not gui_owner.process_running(pid),
            f"process {pid} to be gone",
            timeout)
        return True
    except gui_owner.OwnerTimeout:
        return False


def _release_fifo(release_path):
    """Let whatever waits on this fifo go on, once no assertion needs it
    waiting.  An Esc that ended the waiter took the fifo's reader with it, and
    a fifo with no reader refuses the open: that is the wait being over, not a
    failure to end it."""
    try:
        fd = os.open(str(release_path), os.O_WRONLY | os.O_NONBLOCK)
    except OSError:
        return
    try:
        os.write(fd, b"go\n")
    finally:
        os.close(fd)


def _release_the_build(release_path):
    """Let a build that waits on its fifo go on, once no assertion needs it
    running (see _release_fifo)."""
    _release_fifo(release_path)


def _build_pid_gone(tmp_path):
    """Has the build's own process gone?

    The Makefile writes the pid of the shell that waits on the fifo, so a row
    waits for a process and not for a clock: the second Esc is sent once the
    first activity is really over."""
    path = tmp_path / "build.pid"
    if not path.exists():
        return False
    try:
        pid = int(path.read_text().strip())
    except (ValueError, OSError):
        return False
    return procinfo.run_state(pid) == procinfo.GONE


def _ai_env(tmp_path, trace, mock_delay_ms=8000, reply="int main(void){return 1;}"):
    """Common AI environment."""
    return {
        "XWPE_UI_TRACE": trace,
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        **gui_owner.product_env("XWPE_AI_MOCK_DELAY_MS", mock_delay_ms),
    }


def _bottom_bar(w):
    """The last screen row (status row)."""
    return w.display(drain=0)[-1]


# --- the hosted engine's stand-in ------------------------------------------
#
# The three rows that press Esc during a hosted turn drive a real turn: the
# editor starts the engine its configuration names, and here that engine is a
# stub speaking the same stream-json protocol.  No row reaches a real engine.
# A row waits for what the stub or the editor writes, never for a clock.

# The stub writes its pid, streams one text block and then waits for a line on
# its stdin.  An interrupt request ends the turn with a result and leaves the
# session alive for a next one; every line it reads is recorded.
HOST_STUB_TURN = """\
import json
import os
import sys

def emit(obj):
    sys.stdout.write(json.dumps(obj) + "\\n")
    sys.stdout.flush()

with open("host.pid", "w") as fh:
    fh.write(str(os.getpid()))
sys.stdin.readline()                      # the user's task
emit({"type": "system", "subtype": "init", "session_id": "stub"})
emit({"type": "assistant", "message": {"content": [
    {"type": "text", "text": "FIRSTBLOCK streamed before the wait"}]}})
while True:
    line = sys.stdin.readline()
    if not line:
        break
    with open("host.stdin.log", "a") as fh:
        fh.write(line)
    asked = json.loads(line)
    if asked.get("type") == "control_request" and \\
            asked.get("request", {}).get("subtype") == "interrupt":
        emit({"type": "result", "subtype": "success", "is_error": False,
              "result": "interrupted"})
"""

# The same stub with a turn that does not end on the interrupt: it records the
# request and then waits on the fifo the row writes, so the interrupt stays
# pending for exactly as long as the row needs it.
HOST_STUB_HELD_TURN = """\
import json
import os
import sys

def emit(obj):
    sys.stdout.write(json.dumps(obj) + "\\n")
    sys.stdout.flush()

with open("host.pid", "w") as fh:
    fh.write(str(os.getpid()))
sys.stdin.readline()                      # the user's task
emit({"type": "system", "subtype": "init", "session_id": "stub-held"})
emit({"type": "assistant", "message": {"content": [
    {"type": "text", "text": "FIRSTBLOCK streamed before the wait"}]}})
while True:
    line = sys.stdin.readline()
    if not line:
        break
    with open("host.stdin.log", "a") as fh:
        fh.write(line)
    asked = json.loads(line)
    if asked.get("type") != "control_request":
        continue
    if asked.get("request", {}).get("subtype") != "interrupt":
        continue
    with open("host.release") as fh:      # the row's fifo: it waits here
        fh.read(1)
    emit({"type": "result", "subtype": "success", "is_error": False,
          "result": "interrupted"})
"""


def _host_stub(tmp_path, source):
    """Write the stub engine into the session's folder and name it."""
    mock = tmp_path / "host_stub.py"
    mock.write_text(source)
    return mock


def _host_env(mock, trace):
    """The environment of a hosted turn: the stub as the engine, its own
    trace, and the editor's trace the owner of the key writes to."""
    return {"XWPE_AI_ENABLE": "1",
            "XWPE_AI_AGENT_ENGINE": "claude-code",
            "XWPE_AI_HOST_CMD": "python3 %s" % mock,
            "XWPE_AI_TRACE": str(os.path.join(os.path.dirname(trace),
                                              "host.trace")),
            "XWPE_UI_TRACE": trace}


def _host_pid(tmp_path):
    """The pid the stub engine wrote, or None while it has not written one."""
    path = tmp_path / "host.pid"
    if not path.exists():
        return None
    try:
        return int(path.read_text().strip())
    except (ValueError, OSError):
        return None


def _wait_host_started(w, tmp_path, tmo=15):
    """Wait for the hosted turn to be on: the editor traced the send and the
    stub wrote its pid, so a row can ask whether that process still lives.

    Read from the traces and not from the screen: a build's Messages window
    may be in front of the pane."""
    assert wait_until(w, lambda: "host send" in _host_trace_text(tmp_path),
                      tmo=tmo, what="the turn to reach the engine")
    assert wait_until(w, lambda: _host_pid(tmp_path) is not None, tmo=tmo,
                      what="the stub engine to write its pid")


def _host_trace_text(tmp_path):
    """What the assistant's trace holds so far: the hosted engine and a
    background task both write it."""
    return _trace_text(tmp_path / "host.trace")


def _trace_text(path):
    """What a trace file holds so far, or nothing while it is not there."""
    try:
        with open(path, errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def _start_hosted_turn(w, task="think for a while"):
    """Ask the hosted engine for @task: Alt-G g, then the task and Enter."""
    w.key(ALT.AI)
    w.key("g", delay=0.5)
    w.key(task)
    w.key("\r", delay=0.8)


# --- G rows (guards, green on HEAD) ---

# @scenario SC-esc-owner-01
@pytest.mark.wpe_requires("enable-ai")
def test_G1_build_cancel(tmp_path):
    """Esc ends a running build; other activity (none) still runs."""
    trace = _trace_path(tmp_path)
    src = "int main(void){return 0;}\n"
    slow_make = ("all:\n\t@echo BUILD_START\n\t@sleep 20\n\t@echo BUILD_DONE\n"
                 "\t@touch prog\n")
    (tmp_path / "Makefile").write_text(slow_make)
    with WpeSession(str(tmp_path), src, filename="t.c",
                    env_extra={"XWPE_UI_TRACE": trace}) as w:
        w.key(ALT_A, delay=0.8)
        assert wait_screen(w, "building ...", tmo=10), "waiting line not shown"
        w.key(ESC, delay=0.8)
        assert wait_screen(w, "build cancelled", tmo=10), "build not cancelled"
        assert _trace_says_cancel(trace, "the build"), \
            "the owner did not name the build:\n" + \
            "\n".join(_trace_cancel_lines(trace))


# What the multi-line composer shows while it is open, and the popup that
# leads to it.  The composer runs its own turn of the editor's key loop that
# ends on an Esc the loop reads, so it is the one place a key the owner took
# is seen not to reach the loop's head.
COMPOSER_HINT = "Esc = done"
AGENT_POPUP_TITLE = "AI agent task"
ALT_M_MULTILINE = "\033m"
COMPOSER_TEXT = "KEPTTEXT"


def _composer_open(w):
    """Is the multi-line composer on the screen?"""
    return COMPOSER_HINT in "\n".join(w.display())


# @scenario SC-esc-owner-33
@pytest.mark.wpe_requires("enable-ai")
def test_G1b_taken_key_stays_in_the_loop(tmp_path):
    """The Esc a claim takes is the claim's alone: it ends the build and goes
    no further, so the composer the user is writing in stays open and takes
    the next key as text.  A key reported as not taken would also leave the
    composer's loop, which closes it.  The Esc after that, with nothing left
    to end, is the composer's own and closes it, and the owner names nothing."""
    trace = _trace_path(tmp_path)
    slow_make = ("all:\n\t@echo BUILD_START\n\t@sleep 20\n\t@echo BUILD_DONE\n"
                 "\t@touch prog\n")
    (tmp_path / "Makefile").write_text(slow_make)
    env = _ai_env(tmp_path, trace)
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    filename="t.c", env_extra=env) as w:
        w.key(ALT_A, delay=0.8)
        assert wait_screen(w, "building ...", tmo=10), "the build did not start"
        w.key(ALT_1, delay=0.4)
        w.key(ALT.AI, delay=0.5)
        w.key("g", delay=0.8)
        assert wait_screen(w, AGENT_POPUP_TITLE, tmo=5), \
            "the agent popup did not open:\n" + "\n".join(w.display())
        w.key(ALT_M_MULTILINE, delay=1.2)
        assert wait_until(w, lambda: _composer_open(w), tmo=5,
                          what="the composer to open")
        w.key(ESC, delay=0.8)
        assert _trace_says_cancel(trace, "the build"), \
            "the owner did not name the build:\n" + \
            "\n".join(_trace_cancel_lines(trace))
        assert _composer_open(w), \
            "the Esc the build took also closed the composer:\n" + \
            "\n".join(w.display())
        w.key(COMPOSER_TEXT, delay=0.5)
        assert COMPOSER_TEXT in "\n".join(w.display()), \
            "the key after the taken Esc did not reach the composer's text:\n" + \
            "\n".join(w.display())
        assert _trace_cancel_lines(trace) == [_cancel_line("the build")], \
            "the owner named more than the build:\n" + \
            "\n".join(_trace_cancel_lines(trace))


# @scenario SC-esc-owner-02
def test_G2_debugger_start_cancel(tmp_path):
    """Esc ends the wait a gdb start leaves behind it.

    A pipe debugger answers its banner at once, so the start's own claim is
    over before its first command is even sent: the wait for that command is
    the debugger's answer claim, and that is the claim the key reaches.  Its
    line names the command that was never answered.  A start whose debugger
    has not answered at all keeps the start's own claim, which a debug
    adapter's start shows (G18 presses Esc there)."""
    trace = _trace_path(tmp_path)
    src = GDB_GLOBAL_PROG
    env = _gdb_stub_env(GDB_SET_UP_FIRST)
    env["XWPE_UI_TRACE"] = trace
    with WpeSession(str(tmp_path), src, filename="p.c", wait=1.5, env_extra=env) as w:
        # Breakpoint + Ctrl-G R to start the debugger
        _go_to_line(w, GDB_GLOBAL_BREAK_LINE)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=1.5)
        _gdb_stub_holds(w, tmp_path, GDB_SET_UP_FIRST)
        assert _status_row_says(w, WAITING_FOR_SET_UP, STEP_TIMEOUT_S), "gdb start wait not shown"
        w.key(ESC, delay=0.8)
        assert _status_row_says(w, GDB_DID_NOT_ANSWER_SET_UP, STEP_TIMEOUT_S), "gdb start not cancelled"
        assert _trace_says_cancel(trace, "the debugger's answer"), \
            "the owner did not name the wait the start left behind:\n" + \
            "\n".join(_trace_cancel_lines(trace))


# What an adapter's start says while it waits and once Esc ended it.
ADAPTER_WAITING = "starting dlv"
ADAPTER_CANCELLED = "dlv start cancelled"


def _silent_dlv_env(tmp_path, trace):
    """The environment of a debug adapter's start that is never answered: a
    dlv stand-in that dials nothing and says nothing, and a start timeout far
    beyond the row, so the only way the wait ends is the key."""
    bindir = tmp_path / "bin"
    bindir.mkdir()
    (bindir / "dlv").write_text("#!/bin/sh\nexec 3>&1\nexec cat 3>&3 > %s\n" %
                                (tmp_path / "adapter-stdin.log"))
    (bindir / "dlv").chmod(0o755)
    return {"PATH": str(bindir) + ":" + os.environ.get("PATH", ""),
            **gui_owner.product_env("XWPE_DEBUG_START_TIMEOUT", 600),
            "XWPE_UI_TRACE": trace}


# @scenario SC-esc-owner-35
def test_G2b_adapter_start_cancel(tmp_path):
    """Esc ends the start of a debug adapter that never answers.

    The adapter says nothing, so no command was ever sent and the debugger's
    answer is not what waits: the start's own claim is, and the owner names
    it.  G2 is the start whose debugger did answer its banner."""
    trace = _trace_path(tmp_path)
    src = "package main\n\nfunc main() {\n    x := 3\n    _ = x\n}\n"
    with WpeSession(str(tmp_path), src, filename="main.go", wait=1.5,
                    env_extra=_silent_dlv_env(tmp_path, trace)) as w:
        w.key(CTRL_G, "r", delay=1.5)
        assert wait_screen(w, ADAPTER_WAITING, tmo=15), \
            "the adapter start's waiting line was not shown"
        w.key(ESC, delay=0.8)
        assert wait_screen(w, ADAPTER_CANCELLED, tmo=10), \
            "the adapter's start was not cancelled"
        assert _trace_says_cancel(trace, "the debugger start"), \
            "the owner did not name the debugger start:\n" + \
            "\n".join(_trace_cancel_lines(trace))
        assert _trace_cancel_lines(trace) == [_cancel_line("the debugger start")], \
            "the Esc ended more than the start:\n" + \
            "\n".join(_trace_cancel_lines(trace))


# @scenario SC-esc-owner-03
def test_G3_scala_setup_cancel(tmp_path):
    """Esc ends scala-cli setup-ide; other activity (none) still runs."""
    trace = _trace_path(tmp_path)
    src = SCALA_SEED
    (tmp_path / "build.sc").write_text("// scala-cli setup-ide test\n")
    env = _scala_setup_env(tmp_path)
    env["XWPE_UI_TRACE"] = trace
    with WpeSession(str(tmp_path), src, filename="Main.scala",
                    wait=1.5, env_extra=env, answer_server_start=True) as w:
        w.key(ALT_Q, delay=0.5)
        w.key("e", delay=0.2)
        assert _child_wait_for(w, lambda: "scala-cli setup-ide" in _child_screen(w), SETUP_START_S), "setup-ide wait not shown"
        w.key(ESC, delay=0.8)
        assert _child_wait_for(w, lambda: SETUP_CANCELLED in _child_screen(w), SETUP_START_S), "setup-ide not cancelled"
        assert _trace_says_cancel(trace, "scala-cli setup"), \
            "the owner did not name the setup:\n" + \
            "\n".join(_trace_cancel_lines(trace))


# @scenario SC-esc-owner-04
def test_G4_build_server_cancel(tmp_path):
    """Esc ends BSP build server start; other activity (none) still runs."""
    trace = _trace_path(tmp_path)
    src = BSP_SEED
    env = _bsp_project(tmp_path)
    env["XWPE_UI_TRACE"] = trace
    with WpeSession(str(tmp_path), src, filename="p.scala", wait=1.5, env_extra=env) as w:
        w.key(CTRL_G, "r", delay=1.5)
        assert _bsp_wait_for(w, lambda: BSP_WAITING in _bsp_screen(w), 20), "bsp wait not shown"
        w.key(ESC, delay=0.8)
        assert _bsp_wait_for(w, lambda: BSP_CANCELLED in _bsp_screen(w), 10), "bsp not cancelled"
        assert _trace_says_cancel(trace, "the build server"), \
            "the owner did not name the build server:\n" + \
            "\n".join(_trace_cancel_lines(trace))


# @scenario SC-esc-owner-05
def test_G5_lsp_answer_cancel(tmp_path):
    """Esc ends LSP answer wait; other activity (none) still runs."""
    trace = _trace_path(tmp_path)
    src = "int add(int a, int b){return a+b;}\n"
    env = _lsp_server_env(tmp_path, ["textDocument/hover"])
    env["XWPE_UI_TRACE"] = trace
    with WpeSession(str(tmp_path), src, filename="a.c", wait=1.5, env_extra=env) as w:
        # Start the LSP server first (Alt-Q E), then ask for hover (Alt-Q H)
        w.key(ALT_Q, delay=0.4)
        w.key("e", delay=0.1)
        _lsp_wait_for(w, lambda: "REQ initialize" in _lsp_stub_log(tmp_path), 15), "server not started"
        w.key(ALT_Q, delay=0.4)
        w.key("h", delay=0.1)
        # The hover wait shows as "waiting for clangd to answer \`textDocument/hover\` ..."
        assert _lsp_wait_for(w, lambda: "waiting for clangd to answer" in _lsp_screen(w), 12), "LSP hover wait not shown"
        w.key(ESC, delay=0.8)
        # Esc gives up the answer wait -> "Gave up waiting for clangd"
        assert _lsp_wait_for(w, lambda: "Gave up waiting for clangd" in _lsp_screen(w), 12), "LSP answer not cancelled"
        assert _trace_says_cancel(trace, "the language server"), \
            "the owner did not name the language server:\n" + \
            "\n".join(_trace_cancel_lines(trace))


# @scenario SC-esc-owner-06
def test_G6_compile_for_debug_cancel(tmp_path):
    """Esc ends Rust compile for debug; other activity (none) still runs."""
    trace = _trace_path(tmp_path)
    src = DAP_RUST_PROG
    env = _slow_rustc_env(tmp_path)
    env["XWPE_UI_TRACE"] = trace
    with WpeSession(str(tmp_path), src, filename="main.rs", wait=1.5, env_extra=env) as w:
        w.key(CTRL_G, "r", delay=0.2)
        # the stand-in rustc writes its pid: the compiler is running
        assert _child_wait_for(w, lambda: _read_pid(tmp_path / RUSTC_PID_FILE),
                               SLOW_RUSTC_S / 2), "the compiler did not start"
        w.key(ESC, delay=0.5)
        assert _child_wait_for(w, lambda: "compile for debug cancelled" in _child_screen(w),
                               3.0), "compile not cancelled"
        assert _trace_says_cancel(trace, "the compile for debug"), \
            "the owner did not name the compile:\n" + \
            "\n".join(_trace_cancel_lines(trace))


# @scenario SC-esc-owner-07
def test_G7_debugger_answer_cancel(tmp_path):
    """Esc ends debugger answer wait; other activity (none) still runs."""
    trace = _trace_path(tmp_path)
    src = ("int main(void){\n"
           "  int x = 1;\n"
           "  while (x) x++;\n"
           "  return 0;\n"
           "}\n")
    env = _gdb_stub_env("where")
    env["XWPE_UI_TRACE"] = trace
    with WpeSession(str(tmp_path), src, filename="p.c", env_extra=env) as w:
        # Breakpoint + Ctrl-G R to reach the stop where 'where' is held
        _go_to_line(w, GDB_GLOBAL_BREAK_LINE)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=2.0)
        _gdb_stub_holds(w, tmp_path, "where")
        assert wait_screen(w, "waiting for gdb to answer", tmo=15), "gdb answer wait not shown"
        w.key(ESC, delay=0.8)
        assert wait_screen(w, "gdb did not answer", tmo=10), "answer wait not cancelled"
        assert _trace_says_cancel(trace, "the debugger's answer"), \
            "the owner did not name the answer wait:\n" + \
            "\n".join(_trace_cancel_lines(trace))


# @scenario SC-esc-owner-08
@pytest.mark.wpe_requires("enable-ai")
def test_G8_ai_task_cancel(tmp_path):
    """Esc ends AI background task; other activity (none) still runs."""
    trace = _trace_path(tmp_path)
    ai_trace = str(tmp_path / "ai.trace")
    src = "AAA\n"
    env = _ai_env(tmp_path, trace)
    env["XWPE_AI_TRACE"] = ai_trace
    with WpeSession(str(tmp_path), src, filename="stack.c", env_extra=env) as w:
        _start_edit(w)
        w._drain(1.0)
        w.key(ESC, delay=1.0)
        assert wait_screen(w, "cancelled", tmo=10), "AI task not cancelled"
        assert _trace_contains(ai_trace, "cancelled")
        assert _trace_says_cancel(trace, "the assistant's task"), \
            "the owner did not name the task:\n" + \
            "\n".join(_trace_cancel_lines(trace))


# @scenario SC-esc-owner-09
@pytest.mark.wpe_requires("enable-ai", "enable-ai-agent-host")
def test_G9_hosted_turn_cancel(tmp_path):
    """Esc ends the hosted engine's turn: the pane says the interrupt went and
    then that the turn ended, the engine's own process lives on (the session
    stays alive for a next turn), and the owner's trace names the turn."""
    trace = _trace_path(tmp_path)
    mock = _host_stub(tmp_path, HOST_STUB_TURN)
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    filename="t.c", env_extra=_host_env(mock, trace)) as w:
        w._drain(0.5)
        _start_hosted_turn(w)
        _wait_host_started(w, tmp_path)
        assert wait_screen(w, "FIRSTBLOCK", tmo=8), \
            "the turn never streamed into the pane:\n" + "\n".join(w.display())
        w.key(ESC, delay=0.8)
        assert wait_screen(w, "interrupting", tmo=5), \
            "Esc during the hosted turn did not say it was interrupting:\n" + \
            "\n".join(w.display())
        assert wait_screen(w, "turn interrupted", tmo=10), \
            "the turn never ended:\n" + "\n".join(w.display())
        assert _trace_says_cancel(trace, "the assistant's turn"), \
            "the owner did not name the turn:\n" + \
            "\n".join(_trace_cancel_lines(trace))
        assert gui_owner.process_running(_host_pid(tmp_path)), \
            "the engine's process went: the session must stay alive"


# @scenario SC-esc-owner-10
@pytest.mark.wpe_requires("enable-ai")
def test_G10_idle_pane_closes(tmp_path):
    """Esc in idle AI pane closes the chat input: the "> " input row goes and
    the focus returns to the file window; the pane stays as a transcript."""
    trace = _trace_path(tmp_path)
    src = "AAA\n"
    env = {"XWPE_UI_TRACE": trace,
           "XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": "done"}
    with WpeSession(str(tmp_path), src, filename="t.c", env_extra=env) as w:
        w.key(ALT.AI); w.key("a", delay=1.0)  # Alt-G a -> open chat pane
        assert wait_screen(w, "Esc leaves", tmo=5), "pane not open"
        assert active_title(w) == "AI", "the pane is not focused: %r" % active_title(w)
        w.key(ESC, delay=0.5)
        # the input row ("> ") is gone
        assert wait_until(w, lambda: not any(r.strip().startswith("x>") for r in w.display()),
                          tmo=5, what="input row to go")
        # and the focus is back on the file window
        assert wait_until(w, lambda: active_title(w) == "t.c", tmo=5,
                          what="focus to file window")
        assert _trace_says_cancel(trace, "the chat pane"), \
            "the owner did not name the pane:\n" + \
            "\n".join(_trace_cancel_lines(trace))


# @scenario SC-esc-owner-11
def test_G11_ctrl_c_debugger_answer(tmp_path):
    """Ctrl-C ends the debugger's answer wait: the first half of the one row
    that takes it.  The other half, Ctrl-C ending nothing else, is G11b."""
    trace = _trace_path(tmp_path)
    src = ("int main(void){\n"
           "  int x = 1;\n"
           "  while (x) x++;\n"
           "  return 0;\n"
           "}\n")
    env = _gdb_stub_env("where")
    env["XWPE_UI_TRACE"] = trace
    with WpeSession(str(tmp_path), src, filename="p.c", env_extra=env) as w:
        _go_to_line(w, GDB_GLOBAL_BREAK_LINE)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=2.0)
        _gdb_stub_holds(w, tmp_path, "where")
        assert wait_screen(w, "waiting for gdb to answer", tmo=15)
        w.key(CTRL_C, delay=0.8)
        assert wait_screen(w, "gdb did not answer", tmo=10), "Ctrl-C did not cancel answer wait"
        assert _trace_says_cancel(trace, "the debugger's answer", key="ctrl-c"), \
            "the owner did not name the answer wait for Ctrl-C:\n" + \
            "\n".join(_trace_cancel_lines(trace))


# @scenario SC-esc-owner-34
def test_G11b_ctrl_c_ends_nothing_else(tmp_path):
    """Ctrl-C during a build ends nothing: the debugger's answer is the one row
    that takes it.  The build keeps running and the owner names no claim; the
    Esc after it still ends the build, so the build was running and the key
    was the one that did not reach it."""
    trace = _trace_path(tmp_path)
    slow_make = ("all:\n\t@echo BUILD_START\n\t@sleep 20\n\t@echo BUILD_DONE\n"
                 "\t@touch prog\n")
    (tmp_path / "Makefile").write_text(slow_make)
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    filename="t.c", env_extra={"XWPE_UI_TRACE": trace}) as w:
        w.key(ALT_A, delay=0.8)
        assert wait_screen(w, "building ...", tmo=10), "the build did not start"
        w.key(CTRL_C, delay=1.0)
        assert _trace_cancel_lines(trace) == [], \
            "Ctrl-C reached a claim during a build:\n" + \
            "\n".join(_trace_cancel_lines(trace))
        assert wait_screen(w, "building ...", tmo=3), \
            "Ctrl-C ended the build:\n" + "\n".join(w.display())
        w.key(ESC, delay=0.8)
        assert wait_screen(w, "build cancelled", tmo=10), \
            "the Esc after Ctrl-C did not end the build"
        assert _trace_cancel_lines(trace) == [_cancel_line("the build")], \
            "the build was not ended by the Esc alone:\n" + \
            "\n".join(_trace_cancel_lines(trace))


# @scenario SC-esc-owner-12
def test_G12_esc_not_program_text(tmp_path):
    """Esc while debugged program takes typed text: nothing reaches the program.
    The program prints READY, does ONE read(stdin), logs every byte it reads,
    then stops at a breakpoint. Esc is below 32, so e_key_is_program_text
    returns 0 and the Esc byte never reaches the program; the text typed for it does.
    """
    trace = _trace_path(tmp_path)
    stdin_log = tmp_path / "stdin.log"
    # Program that logs ONE read from stdin to a file, then stops at breakpoint
    src = ("#include <stdio.h>\n"
           "#include <fcntl.h>\n"
           "#include <unistd.h>\n"
           "int main(void)\n"
           "{\n"
           "  char line[100];\n"
           "  int logfd = open(\"@LOG@\", O_WRONLY | O_CREAT | O_APPEND, 0644);\n"
           "  printf(\"READY\\n\");\n"
           "  fflush(stdout);\n"
           "  ssize_t n = read(STDIN_FILENO, line, sizeof line);\n"
           "  if (logfd >= 0 && n > 0) write(logfd, line, n);\n"
           "  if (n > 0) {\n"
           "    printf(\"GOT:%.*s\", (int)n, line);\n"
           "    fflush(stdout);\n"
           "  }\n"
           "  if (logfd >= 0) close(logfd);\n"
           "  return 0;\n"
           "}\n").replace("@LOG@", str(stdin_log))
    env = {"XWPE_UI_TRACE": trace}
    with WpeSession(str(tmp_path), src, filename="p.c", wait=1.5, env_extra=env) as w:
        # Breakpoint at return 0; (after the read) - line 14 in the program above
        _move_down(w, 14 - 1)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=0.2)
        assert wait_screen(w, "READY", tmo=15), "program not running and waiting on stdin"
        # The program is now blocked in read() waiting for stdin
        # Send "hello" and Enter: the line-buffered pty delivers it to the program
        w.key("h", "e", "l", "l", "o", delay=0.15)
        w.key("\r", delay=0.4)
        # Now send Esc - it must NOT reach the program (e_key_is_program_text)
        w.key(ESC, delay=0.5)
        # The program should have received "hello\n" but NOT the ESC byte (0x1b)
        def _stdin_log_has_hello():
            if not stdin_log.exists():
                return False
            content = stdin_log.read_bytes()
            got_esc = b"\x1b" in content
            assert not got_esc, "program should NOT have received ESC byte (0x1b)"
            return b"hello" in content

        assert wait_until(w, _stdin_log_has_hello, tmo=5,
                          what="the program to receive 'hello'"), \
            "program should have received 'hello'"
        # The editor should still be responsive (Esc did not cancel anything here)
        w.key("Q", delay=0.3)
        w.save()
        assert "Q" in w.text(), "editor should still take keys"
        assert _trace_cancel_lines(trace) == [], \
            "a key the program was typed for reached a claim:\n" + \
            "\n".join(_trace_cancel_lines(trace))


# @scenario SC-esc-owner-13
def test_G13_dialog_over_build(tmp_path):
    """A dialog over a running build: Esc closes the dialog, build goes on; next Esc cancels it."""
    trace = _trace_path(tmp_path)
    src = "int main(void){return 0;}\n"
    slow_make = ("all:\n\t@echo BUILD_START\n\t@sleep 20\n\t@echo BUILD_DONE\n"
                 "\t@touch prog\n")
    (tmp_path / "Makefile").write_text(slow_make)
    with WpeSession(str(tmp_path), src, filename="t.c",
                    env_extra={"XWPE_UI_TRACE": trace}) as w:
        w.key(ALT_A, delay=0.8)
        assert wait_screen(w, "building ...", tmo=10)
        w.key(ALT.OPTIONS); w.key("c", delay=0.5)  # Options -> Compiler
        assert wait_screen(w, "Compiler", tmo=5), "dialog not open"
        w.key(ESC, delay=0.5)  # closes dialog
        assert wait_until(w, lambda: "Compiler" not in "\n".join(w.display()), tmo=5,
                          what="dialog to close")
        assert _trace_cancel_lines(trace) == [], \
            "the dialog's own Esc reached a claim:\n" + \
            "\n".join(_trace_cancel_lines(trace))
        assert wait_screen(w, "building ...", tmo=3), "build stopped"
        w.key(ESC, delay=0.5)  # cancels build
        assert wait_screen(w, "build cancelled", tmo=10), "build not cancelled on second Esc"
        assert _trace_cancel_lines(trace) == [_cancel_line("the build")], \
            "the Esc after the dialog did not reach the build alone:\n" + \
            "\n".join(_trace_cancel_lines(trace))


# @scenario SC-esc-owner-14
@pytest.mark.wpe_requires("enable-ai")
def test_G14_build_then_ai_task(tmp_path):
    """Build and assistant task from a file window: Esc ends build; after process gone, Esc ends task.
    Uses a Makefile that writes its child PID to a file for process-gone wait.
    The build waits on a fifo the test can release (like test_bsp_start.py).
    Switches back to file window (Alt-1) before asking the AI task so Messages
    doesn't intercept the edit.
    """
    trace = _trace_path(tmp_path)
    ai_trace = str(tmp_path / "ai.trace")
    src = "AAA\n"
    # Build waits on a fifo so the row controls when it ends.
    release_path = tmp_path / "build.release"
    os.mkfifo(release_path)
    slow_make = ("all:\n\t@echo BUILD_START\n\t@sh -c 'echo $$PPID > build.pid; cat " +
                 str(release_path) + " > /dev/null'\n\t@echo BUILD_DONE\n"
                 "\t@touch prog\n")
    (tmp_path / "Makefile").write_text(slow_make)
    env = _ai_env(tmp_path, trace)
    env["XWPE_AI_TRACE"] = ai_trace
    with WpeSession(str(tmp_path), src, filename="t.c", env_extra=env) as w:
        w.key(ALT_A, delay=0.8)
        assert wait_screen(w, "building ...", tmo=10)
        w.key(ALT_1, delay=0.4)  # back to source window
        w.key(ALT.AI); w.key("e", delay=1.0)
        w.key("append a line"); w.key("\r", delay=1.5)
        w.key(ESC, delay=0.8)
        assert wait_screen(w, "build cancelled", tmo=10), "build not cancelled first"
        assert _trace_says_cancel(trace, "the build"), \
            "the owner did not name the build:\n" + \
            "\n".join(_trace_cancel_lines(trace))
        # wait for build pid file and process gone
        def _pid_gone():
            pid_path = tmp_path / "build.pid"
            if pid_path.exists():
                try:
                    pid = int(pid_path.read_text().strip())
                    return procinfo.run_state(pid) == procinfo.GONE
                except (ValueError, OSError):
                    return False
            return False
        assert wait_until(w, _pid_gone, tmo=15, what="build process to go")
        w.key(ESC, delay=1.0)
        assert wait_screen(w, "cancelled", tmo=10), "AI task not cancelled on second Esc"
        assert _trace_contains(ai_trace, "cancelled")
        assert _trace_cancel_lines(trace) == [_cancel_line("the build"),
                                              _cancel_line("the assistant's task")], \
            "one Esc did not end one activity, in the table's order:\n" + \
            "\n".join(_trace_cancel_lines(trace))


# @scenario SC-esc-owner-15
@pytest.mark.wpe_requires("enable-ai")
def test_G15_ai_pane_behind_file_window(tmp_path):
    """Esc in a file window with pane open behind leaves the pane's input/hints
    untouched (front_only row 10); focus stays on the file window."""
    trace = _trace_path(tmp_path)
    src = "AAA\n"
    env = {"XWPE_UI_TRACE": trace,
           "XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": "done"}
    with WpeSession(str(tmp_path), src, filename="t.c", env_extra=env) as w:
        w.key(ALT.AI); w.key("a", delay=1.0)  # Alt-G a -> open chat pane
        assert wait_screen(w, "Esc leaves", tmo=5)
        w.key(F6, delay=0.5)  # F6 = next window -> file window focused
        assert wait_until(w, lambda: "t.c" in "\n".join(w.display()), tmo=5,
                          what="file window on top")
        w.key(ESC, delay=0.5)
        # In the file window, Esc does nothing to the pane behind
        assert wait_screen(w, "Esc leaves", tmo=3), "pane closed unexpectedly"
        assert _trace_cancel_lines(trace) == [], \
            "a claim took the key from a window that is not the pane's:\n" + \
            "\n".join(_trace_cancel_lines(trace))


# @scenario SC-esc-owner-16
def test_G16_click_bottom_row_cancels_build(tmp_path):
    """(until E2) a click on the bottom row past hints during a build cancels it."""
    trace = _trace_path(tmp_path)
    src = "int main(void){return 0;}\n"
    slow_make = ("all:\n\t@echo BUILD_START\n\t@sleep 20\n\t@echo BUILD_DONE\n"
                 "\t@touch prog\n")
    (tmp_path / "Makefile").write_text(slow_make)
    with WpeSession(str(tmp_path), src, filename="t.c",
                    env_extra={"XWPE_UI_TRACE": trace}) as w:
        w.key(ALT_A, delay=0.8)
        assert wait_screen(w, "building ...", tmo=10)
        # SGR 1006: press past last hint on row MAXSLNS (30 in 1-based).
        # The bar's last hint ends around col 76 (0-based), so click at col 78 (1-based).
        w.key(_sgr(0, 78, 30, press=True), delay=0.3)
        w.key(_sgr(0, 78, 30, press=False), delay=0.3)
        assert wait_screen(w, "build cancelled", tmo=10), "click did not cancel build"
        assert _trace_says_cancel(trace, "the build", key="esc-click"), \
            "the owner did not name the click as a click:\n" + \
            "\n".join(_trace_cancel_lines(trace))


# @scenario SC-esc-owner-17
@pytest.mark.wpe_requires("enable-ai", "enable-ai-agent-host")
def test_G17_build_and_hosted_turn(tmp_path):
    """A build and a hosted turn run at the same time.

    The first Esc ends the build: that is the table's order, and the order the
    chain had.  Once the build's process is gone the next Esc asks the engine
    to stop its turn.  An engine that answers no interrupt keeps the key: the
    Esc after that ends nothing, is traced as an end already on its way, and
    leaves the turn open until the row lets the engine answer."""
    trace = _trace_path(tmp_path)
    mock = _host_stub(tmp_path, HOST_STUB_HELD_TURN)
    host_release = tmp_path / "host.release"
    os.mkfifo(host_release)
    build_release = tmp_path / "build.release"
    os.mkfifo(build_release)
    slow_make = ("all:\n\t@echo BUILD_START\n\t@sh -c 'echo $$PPID > build.pid; cat " +
                 str(build_release) + " > /dev/null'\n\t@echo BUILD_DONE\n"
                 "\t@touch prog\n")
    (tmp_path / "Makefile").write_text(slow_make)
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    filename="t.c", env_extra=_host_env(mock, trace)) as w:
        w.key(ALT_A, delay=0.8)
        assert wait_screen(w, "building ...", tmo=10), "the build did not start"
        w.key(ALT_1, delay=0.4)
        _start_hosted_turn(w)
        _wait_host_started(w, tmp_path)
        w.key(ESC, delay=0.8)
        assert wait_screen(w, "build cancelled", tmo=10), \
            "the first Esc did not end the build:\n" + "\n".join(w.display())
        assert wait_until(w, lambda: _build_pid_gone(tmp_path), tmo=20,
                          what="the build's process to go")
        w.key(ESC, delay=0.8)
        assert wait_until(w, lambda: "host interrupt sent" in _host_trace_text(tmp_path),
                          tmo=10, what="the second Esc to reach the turn")
        # The engine answers no interrupt before the row's fifo, so the turn is
        # still ending: the key stays with it and ends nothing else.
        w.key(ESC, delay=0.8)
        assert _trace_says_cancel(trace, "the assistant's turn", state="ending"), \
            "the swallowed Esc was not traced as an end on its way:\n" + \
            "\n".join(_trace_cancel_lines(trace))
        assert "host result" not in _host_trace_text(tmp_path), \
            "the turn ended on an Esc that ends nothing"
        assert gui_owner.process_running(_host_pid(tmp_path)), \
            "the engine's process went on a swallowed Esc"
        assert _trace_cancel_lines(trace) == [
            _cancel_line("the build"),
            _cancel_line("the assistant's turn"),
            _cancel_line("the assistant's turn", state="ending")], \
            "the three Escs did not end one activity each:\n" + \
            "\n".join(_trace_cancel_lines(trace))
        _release_fifo(host_release)
        assert wait_until(w, lambda: "host result" in _host_trace_text(tmp_path),
                          tmo=10, what="the turn to end once the engine answered")
    _release_fifo(build_release)


# @scenario SC-esc-owner-18
def test_G18_build_during_adapter_start(tmp_path):
    """Build during an adapter's start: both busy, one Esc each.
    Uses a DAP adapter (reverse-tcp dlv) that never answers. No compile step
    for Go, so e_d_swtch stays 0 during the handshake and a build can run
    at the same time.
    The adapter starts first, then the build; the first Esc ends the build
    (waited for by its process being gone) and the adapter's start goes on;
    the second Esc ends that start.
    """
    trace = _trace_path(tmp_path)
    # Go program for dlv reverse-tcp DAP
    src = ("package main\n\nfunc main() {\n    x := 3\n    _ = x\n}\n")
    # A build that records make's own PID, so the test can wait for it to go.
    # Uses a fifo the test writes to release the build (like test_bsp_start.py).
    release_path = tmp_path / "build.release"
    os.mkfifo(release_path)
    slow_make = ("all:\n\t@echo BUILD_START\n\t@echo $$PPID > build.pid\n"
                 "\t@read _ < " + str(release_path) + "\n\t@echo BUILD_DONE\n\t@touch prog\n")
    (tmp_path / "Makefile").write_text(slow_make)
    # Use the silent DAP adapter pattern from test_debugger_integration:
    # reverse-tcp transport = dlv that dials back (Go language .go file)
    bindir = tmp_path / "bin"
    bindir.mkdir()
    # dlv stand-in that never answers (like SILENT_ADAPTERS["reverse-tcp"])
    (bindir / "dlv").write_text("#!/bin/sh\nexec 3>&1\nexec cat 3>&3 > %s\n" % (tmp_path / "adapter-stdin.log"))
    (bindir / "dlv").chmod(0o755)
    env = {"PATH": str(bindir) + ":" + os.environ.get("PATH", ""),
           **gui_owner.product_env("XWPE_DEBUG_START_TIMEOUT", 600),
           "XWPE_UI_TRACE": trace}
    with WpeSession(str(tmp_path), src, filename="main.go", wait=1.5, env_extra=env) as w:
        # Start DAP adapter (reverse-tcp dlv) via Ctrl-G R FIRST
        w.key(CTRL_G, "r", delay=1.5)
        # Wait for the "starting dlv ... Ns (Esc cancels)" line
        assert wait_screen(w, "starting dlv", tmo=15), "DAP adapter start waiting line not shown"
        # NOW start the build (Alt-A) while the adapter is still waiting
        w.key(ALT_A, delay=0.8)
        assert wait_screen(w, "building ...", tmo=10), "build not started"
        # The first Esc ends the build, not the adapter's start
        w.key(ESC, delay=0.8)
        # The build's process must be gone (no sleep: the process-gone wait)
        def _build_gone():
            pid_file = tmp_path / "build.pid"
            if not pid_file.exists():
                return False
            try:
                pid = int(pid_file.read_text().strip())
            except (ValueError, OSError):
                return False
            return procinfo.run_state(pid) == procinfo.GONE
        assert wait_until(w, _build_gone, tmo=20, what="the build's process to go"), \
            "the first Esc should end the build"
        # The adapter's start goes on: its waiting line is still there
        assert wait_screen(w, "starting dlv", tmo=5), \
            "the adapter's start should still be waiting after the build ended"
        # The second Esc ends the adapter's start
        w.key(ESC, delay=0.8)
        assert wait_screen(w, "dlv start cancelled", tmo=10), \
            "second Esc should cancel the adapter start"


# @scenario SC-esc-owner-19
@pytest.mark.wpe_requires("enable-ai", "enable-ai-agent-host")
def test_G19_task_during_hosted_turn(tmp_path):
    """An assistant task and a hosted turn run at the same time: one Esc each.

    A task launch never asks whether a turn is open, so both are busy at once.
    The task is the earlier claim, so the first Esc ends it and leaves the turn
    streaming; the second Esc asks the engine to stop the turn."""
    trace = _trace_path(tmp_path)
    mock = _host_stub(tmp_path, HOST_STUB_TURN)
    env = _host_env(mock, trace)
    env.update(_ai_env(tmp_path, trace))
    with WpeSession(str(tmp_path), "AAA\n", filename="stack.c",
                    env_extra=env) as w:
        _start_hosted_turn(w)
        _wait_host_started(w, tmp_path)
        w.key(ALT_1, delay=0.4)
        _start_edit(w)
        # The task's own stream is open: the row waits for that line, not for
        # a spinner the pane may be holding behind another window.
        assert wait_until(w, lambda: "edit stream fd=" in _host_trace_text(tmp_path),
                          tmo=10, what="the task to start")
        w.key(ESC, delay=1.0)
        assert wait_screen(w, "cancelled", tmo=10), \
            "the first Esc did not end the task:\n" + "\n".join(w.display())
        assert _trace_says_cancel(trace, "the assistant's task"), \
            "the owner did not name the task:\n" + \
            "\n".join(_trace_cancel_lines(trace))
        assert "host result" not in _host_trace_text(tmp_path), \
            "the first Esc reached the turn as well"
        assert gui_owner.process_running(_host_pid(tmp_path)), \
            "the engine's process went: the turn must still be open"
        w.key(ESC, delay=1.0)
        assert wait_until(w, lambda: "host interrupt sent" in _host_trace_text(tmp_path),
                          tmo=10, what="the second Esc to reach the turn")
        assert _trace_cancel_lines(trace) == [
            _cancel_line("the assistant's task"),
            _cancel_line("the assistant's turn")], \
            "one Esc did not end one activity each:\n" + \
            "\n".join(_trace_cancel_lines(trace))


# @scenario SC-esc-owner-20
def test_G20_F9_during_pipe_debugger_start(tmp_path):
    """F9 during pipe debugger's start asks 'Quit Debugging?' and Yes forgets the start.
    A pipe debugger (e_exec_deb via Ctrl-G R) sets e_d_swtch = 1 before e_d_start_watch.
    F9 (Make) asks "The Debugger is Running\nDo You want to Quit Debugging ?"
    Answer with Alt-Y (\033y), not bare 'y'. The start is forgotten (no "gdb start cancelled").
    Uses the gdb stub to hold the first command so the editor waits during the start.
    """
    trace = _trace_path(tmp_path)
    src = ("#include <stdio.h>\n"
           "int main(void){\n"
           "  int x = 3;\n"
           "  printf(\"READY\\n\");\n"
           "  return x;\n"
           "}\n")
    # Use the gdb stub to hold the first command ("set confirm off")
    env = _gdb_stub_env(GDB_SET_UP_FIRST)
    env["XWPE_UI_TRACE"] = trace
    with WpeSession(str(tmp_path), src, filename="p.c", env_extra=env) as w:
        # Set breakpoint and start debugger; stub holds the first command
        _go_to_line(w, 4)  # printf("READY\n");
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=1.0)
        # Wait for the "waiting for gdb to answer `set confirm off`" line
        assert wait_screen(w, "waiting for gdb to answer", tmo=15), \
            "pipe debugger start wait not shown"
        # Now F9 (build) should ask "The Debugger is Running\nDo You want to Quit Debugging ?"
        w.key(F9, delay=0.5)
        assert wait_screen(w, "Quit Debugging", tmo=5), "Quit Debugging not asked"
        # Message box uses Alt-Y (\033y), not bare 'y'
        w.key("\033y", delay=0.5)
        # e_d_quit calls e_d_start_forget() - the start is forgotten, NOT "gdb start cancelled"
        assert wait_screen(w, "session gone", tmo=5) or \
               wait_screen(w, "gdb start cancelled", tmo=5) == False, \
            "debugger start should be forgotten, not 'gdb start cancelled'"
        assert _trace_cancel_lines(trace) == [], \
            "the box's answer was read as a cancel key:\n" + \
            "\n".join(_trace_cancel_lines(trace))


# --- P rows (xfail on HEAD, green after E2) ---
# All P rows marked xfail(strict=True) per instructions; unexpected XPASS = finding.

# @scenario SC-esc-owner-21
@pytest.mark.xfail(strict=True, reason="the Esc owner's order (pass E2)")
def test_P1_file_window_build_and_altq_wait(tmp_path):
    """From a file window, build runs, Alt-Q h wait: Esc ends the wait, build still runs."""
    trace = _trace_path(tmp_path)
    src = "int add(int a, int b){return a+b;}\n"
    slow_make = ("all:\n\t@echo BUILD_START\n\t@sleep 10\n\t@echo BUILD_DONE\n"
                 "\t@touch prog\n")
    (tmp_path / "Makefile").write_text(slow_make)
    env = _lsp_server_env(tmp_path, ["textDocument/hover"])
    env["XWPE_UI_TRACE"] = trace
    with WpeSession(str(tmp_path), src, filename="a.c", wait=1.5, env_extra=env) as w:
        w.key(ALT_A, delay=0.8)
        assert wait_screen(w, "building ...", tmo=10)
        w.key(ALT_Q, delay=0.4)
        w.key("h", delay=0.1)
        assert wait_screen(w, "waiting for language server", tmo=10), "hover wait not shown"
        w.key(ESC, delay=0.8)
        assert wait_screen(w, "Language server start cancelled", tmo=10), "hover not cancelled"
        assert wait_screen(w, "building ...", tmo=3), "build stopped"


# @scenario SC-esc-owner-22
@pytest.mark.xfail(strict=True, reason="the Esc owner's order (pass E2)")
def test_P2_file_window_debugger_start_and_altq_wait(tmp_path):
    """From a file window, debugger start runs, Alt-Q h wait: Esc ends the wait, debugger start still runs."""
    trace = _trace_path(tmp_path)
    src = ("int main(void){\n"
           "  int x = 1;\n"
           "  while (x) x++;\n"
           "  return 0;\n"
           "}\n")
    env = _gdb_stub_env(GDB_SET_UP_FIRST)
    env["XWPE_STUB_LSP_HOLD"] = "textDocument/hover"
    env["XWPE_UI_TRACE"] = trace
    with WpeSession(str(tmp_path), src, filename="p.c", env_extra=env) as w:
        _go_to_line(w, GDB_GLOBAL_BREAK_LINE)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=2.0)
        _gdb_stub_holds(w, tmp_path, GDB_SET_UP_FIRST)
        assert wait_screen(w, WAITING_FOR_SET_UP, tmo=15)
        w.key(ALT_Q, delay=0.4)
        w.key("h", delay=0.1)
        assert wait_screen(w, "waiting for language server", tmo=10), "hover wait not shown"
        w.key(ESC, delay=0.8)
        assert wait_screen(w, "Language server start cancelled", tmo=10), "hover not cancelled"
        assert wait_screen(w, "waiting for gdb", tmo=3), "debugger start stopped"


# @scenario SC-esc-owner-23
@pytest.mark.wpe_requires("enable-ai")
@pytest.mark.xfail(strict=True, reason="the Esc owner's order (pass E2)")
def test_P3_pane_focused_task_and_build(tmp_path):
    """Pane focused, task and build run: Esc ends the task, build still runs."""
    trace = _trace_path(tmp_path)
    src = "AAA\n"
    slow_make = ("all:\n\t@echo BUILD_START\n\t@sleep 10\n\t@echo BUILD_DONE\n"
                 "\t@touch prog\n")
    (tmp_path / "Makefile").write_text(slow_make)
    env = _ai_env(tmp_path, trace)
    with WpeSession(str(tmp_path), src, filename="t.c", env_extra=env) as w:
        w.key(ALT_A, delay=0.8)
        assert wait_screen(w, "building ...", tmo=10)
        w.key(ALT.AI); w.key("e", delay=1.0)
        w.key("append a line"); w.key("\r", delay=1.5)
        w.key(ESC, delay=1.0)
        assert wait_screen(w, "cancelled", tmo=10), "task not cancelled"
        assert wait_screen(w, "building ...", tmo=3), "build stopped"


# @scenario SC-esc-owner-24
@pytest.mark.wpe_requires("enable-ai")
@pytest.mark.xfail(strict=True, reason="the Esc owner's order (pass E2)")
def test_P4_locked_window_task_and_build(tmp_path):
    """An Edit or Complete task locks the window on top, task and build run: Esc ends the task, build still runs."""
    trace = _trace_path(tmp_path)
    src = "AAA\n"
    slow_make = ("all:\n\t@echo BUILD_START\n\t@sleep 10\n\t@echo BUILD_DONE\n"
                 "\t@touch prog\n")
    (tmp_path / "Makefile").write_text(slow_make)
    env = _ai_env(tmp_path, trace)
    with WpeSession(str(tmp_path), src, filename="t.c", env_extra=env) as w:
        w.key(ALT_A, delay=0.8)
        assert wait_screen(w, "building ...", tmo=10)
        w.key(ALT.AI); w.key("e", delay=1.0)
        w.key("append a line"); w.key("\r", delay=1.5)
        w.key(ESC, delay=1.0)
        assert wait_screen(w, "cancelled", tmo=10), "task not cancelled"
        assert wait_screen(w, "building ...", tmo=3), "build stopped"


# @scenario SC-esc-owner-25
@pytest.mark.wpe_requires("enable-ai")
def test_P5_agent_task_not_locked_build_wins(tmp_path):
    """Guard: Agent task asked from window on top (not locked) and build: Esc ends the build (green on HEAD)."""
    trace = _trace_path(tmp_path)
    src = "AAA\n"
    # The build waits on a fifo the row releases, so it is still running when
    # the Esc arrives: a sleep in the Makefile is the program's own clock,
    # which the row's scale does not stretch, and a scaled row outlives it.
    release_path = tmp_path / "build.release"
    os.mkfifo(release_path)
    slow_make = ("all:\n\t@echo BUILD_START\n\t@cat " + str(release_path) +
                 " > /dev/null\n\t@echo BUILD_DONE\n\t@touch prog\n")
    (tmp_path / "Makefile").write_text(slow_make)
    env = _ai_env(tmp_path, trace)
    with WpeSession(str(tmp_path), src, filename="t.c", env_extra=env) as w:
        w.key(ALT_A, delay=0.8)
        assert wait_screen(w, "building ...", tmo=10)
        w.key(ALT.AI); w.key("a", delay=1.0)  # Agent
        w.key("do something"); w.key("\r", delay=1.5)
        w.key(ESC, delay=1.0)
        assert wait_screen(w, "build cancelled", tmo=10), "build not cancelled"
    _release_the_build(release_path)


# @scenario SC-esc-owner-26
@pytest.mark.wpe_requires("enable-ai")
@pytest.mark.xfail(strict=True, reason="the Esc owner's order (pass E2)")
def test_P6_idle_pane_build_runs(tmp_path):
    """Idle pane focused, build runs: Esc closes the pane, build still runs."""
    trace = _trace_path(tmp_path)
    src = "AAA\n"
    slow_make = ("all:\n\t@echo BUILD_START\n\t@sleep 10\n\t@echo BUILD_DONE\n"
                 "\t@touch prog\n")
    (tmp_path / "Makefile").write_text(slow_make)
    env = {"XWPE_UI_TRACE": trace,
           "XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": "done"}
    with WpeSession(str(tmp_path), src, filename="t.c", env_extra=env) as w:
        w.key(ALT_A, delay=0.8)
        assert wait_screen(w, "building ...", tmo=10)
        w.key(ALT.AI); w.key("e", delay=1.0)
        assert wait_screen(w, "Esc leaves", tmo=5)
        w.key(ESC, delay=0.5)
        assert wait_until(w, lambda: "Esc leaves" not in "\n".join(w.display()), tmo=5,
                          what="pane to close"), "pane did not close"
        assert wait_screen(w, "building ...", tmo=3), "build stopped"


# @scenario SC-esc-owner-27
@pytest.mark.xfail(strict=True, reason="the Esc owner's order (pass E2)")
def test_P7_build_and_answer_wait_refusal(tmp_path):
    """5.2: build and answer wait: F9 refused, bottom row names what Esc ends first; same for one stall line."""
    trace = _trace_path(tmp_path)
    src = ("int main(void){\n"
           "  int x = 1;\n"
           "  while (x) x++;\n"
           "  return 0;\n"
           "}\n")
    slow_make = ("all:\n\t@echo BUILD_START\n\t@sleep 10\n\t@echo BUILD_DONE\n"
                 "\t@touch prog\n")
    (tmp_path / "Makefile").write_text(slow_make)
    with WpeSession(str(tmp_path), src, filename="p.c",
                    env_extra={"XWPE_UI_TRACE": trace}) as w:
        w.key(ALT_A, delay=0.8)
        assert wait_screen(w, "building ...", tmo=10)
        w.key(F9, delay=2.0)
        assert wait_screen(w, "waiting for gdb to answer", tmo=15)
        w.key(F9, delay=0.5)
        assert wait_screen(w, "Esc cancels", tmo=5), "refusal line not shown"
        assert "waiting for gdb" in "\n".join(w.display()), "answer wait line not on bottom row"


# @scenario SC-esc-owner-28
@pytest.mark.xfail(strict=True, reason="the Esc owner's order (pass E2)")
def test_P8_pipe_read_top_window_build(tmp_path):
    """A pipe read into the top window, build runs: Esc keeps what was read, build still runs."""
    pytest.skip("loading text from a pipe is not built yet")


# @scenario SC-esc-owner-29
@pytest.mark.wpe_requires("enable-ai")
@pytest.mark.xfail(strict=True, reason="the Esc owner's order (pass E2)")
def test_P9_pipe_read_other_window(tmp_path):
    """From another window, Esc does not stop the load and no line there says it would."""
    pytest.skip("loading text from a pipe is not built yet")


# @scenario SC-esc-owner-30
@pytest.mark.xfail(strict=True, reason="the Esc owner's order (pass E2)")
def test_P10_messages_on_top_build_and_altq_wait(tmp_path):
    """Guard: Messages on top, build and Alt-Q wait: Esc ends the build (green on HEAD)."""
    trace = _trace_path(tmp_path)
    src = "int add(int a, int b){return a+b;}\n"
    slow_make = ("all:\n\t@echo BUILD_START\n\t@sleep 10\n\t@echo BUILD_DONE\n"
                 "\t@touch prog\n")
    (tmp_path / "Makefile").write_text(slow_make)
    env = _lsp_server_env(tmp_path, ["textDocument/hover"])
    env["XWPE_UI_TRACE"] = trace
    with WpeSession(str(tmp_path), src, filename="a.c", wait=1.5, env_extra=env) as w:
        w.key(ALT_A, delay=0.8)
        assert wait_screen(w, "building ...", tmo=10)
        w.key(ALT_Q, delay=0.4)
        w.key("h", delay=0.1)
        assert wait_screen(w, "waiting for language server", tmo=10), "hover wait not shown"
        w.key(ESC, delay=0.8)
        assert wait_screen(w, "build cancelled", tmo=10), "build not cancelled"
        assert wait_screen(w, "waiting for language server", tmo=3), "hover wait stopped"


# @scenario SC-esc-owner-31
@pytest.mark.xfail(strict=True, reason="the Esc owner's order (pass E2)")
def test_P11_altq_wait_and_debugger_answer_wait(tmp_path):
    """5.1: Alt-Q wait and debugger answer wait: row shows language server's line only; Esc ends it; then debugger's line shows."""
    trace = _trace_path(tmp_path)
    src = ("int main(void){\n"
           "  int x = 1;\n"
           "  while (x) x++;\n"
           "  return 0;\n"
           "}\n")
    env = _gdb_stub_env("where")
    env["XWPE_STUB_LSP_HOLD"] = "textDocument/hover"
    env["XWPE_UI_TRACE"] = trace
    with WpeSession(str(tmp_path), src, filename="p.c", env_extra=env) as w:
        _go_to_line(w, GDB_GLOBAL_BREAK_LINE)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=2.0)
        _gdb_stub_holds(w, tmp_path, "where")
        assert wait_screen(w, "waiting for gdb to answer", tmo=15)
        w.key(ALT_Q, delay=0.4)
        w.key("h", delay=0.1)
        assert wait_screen(w, "waiting for language server", tmo=10), "hover wait not shown"
        w.key(ESC, delay=0.8)
        assert wait_screen(w, "Language server start cancelled", tmo=10) or \
               wait_screen(w, "hover cancelled", tmo=10), "hover not cancelled"
        assert wait_screen(w, "waiting for gdb", tmo=5), "debugger answer wait not shown"


# @scenario SC-esc-owner-32
@pytest.mark.xfail(strict=True, reason="the Esc owner's order (pass E2)")
def test_P12_click_bottom_row_during_build(tmp_path):
    """A click on the bottom row during a build cancels nothing; build still runs."""
    trace = _trace_path(tmp_path)
    src = "int main(void){return 0;}\n"
    slow_make = ("all:\n\t@echo BUILD_START\n\t@sleep 20\n\t@echo BUILD_DONE\n"
                 "\t@touch prog\n")
    (tmp_path / "Makefile").write_text(slow_make)
    with WpeSession(str(tmp_path), src, filename="t.c",
                    env_extra={"XWPE_UI_TRACE": trace}) as w:
        w.key(ALT_A, delay=0.8)
        assert wait_screen(w, "building ...", tmo=10)
        # SGR 1006: click past last hint on row MAXSLNS (30 in 1-based)
        w.key(_sgr(0, 78, 30, press=True), delay=0.3)
        w.key(_sgr(0, 78, 30, press=False), delay=0.3)
        assert wait_screen(w, "building ...", tmo=3), "build stopped by click"
        w.key(ESC, delay=0.8)
        assert wait_screen(w, "build cancelled", tmo=10), "ESC did not cancel build after click"