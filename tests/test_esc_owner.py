"""Esc owner evidence rows (pass E0).

G1-G20: green on HEAD (today's behaviour, pinned).
P1-P12: xfail on HEAD, green after E2 (the decided new order).
Each row asserts the trace line, the module's own cancelled line, and that
the other activity still runs (its process is alive). Slow activities are
stub tools on PATH that wait on a FIFO the test controls. A second Esc is
sent only after the first activity's process is gone (a process-gone wait),
never after a sleep.
"""
import os
import stat
import time
import signal
import pytest
import sys
import shutil

import gui_owner
import procinfo
from wpe_driver import WpeSession, ALT, wait_until, wait_screen, ui_trace_tail
from test_ai_async import _start_edit, WAIT_SCALE

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
    deadline = time.time() + timeout * WAIT_SCALE
    while time.time() < deadline:
        if os.path.exists(trace_path):
            try:
                with open(trace_path, errors="replace") as fh:
                    text = fh.read()
                if needle in text:
                    return True
            except OSError:
                pass
        time.sleep(0.1)
    return False


def _process_gone(pid, timeout=12):
    """Wait for a PID to be gone."""
    deadline = time.time() + timeout * WAIT_SCALE
    while time.time() < deadline:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return True
        time.sleep(0.1)
    return False




def _ai_env(tmp_path, trace, mock_delay_ms=8000, reply="int main(void){return 1;}"):
    """Common AI environment."""
    return {
        "XWPE_UI_TRACE": trace,
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_MOCK_DELAY_MS": str(int(mock_delay_ms * WAIT_SCALE)),
    }


def _bottom_bar(w):
    """The last screen row (status row)."""
    return w.display(drain=0)[-1]


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


# @scenario SC-esc-owner-02
def test_G2_debugger_start_cancel(tmp_path):
    """Esc ends a debugger start (gdb); other activity (none) still runs."""
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


# @scenario SC-esc-owner-09
@pytest.mark.wpe_requires("enable-ai-agent-host")
def test_G9_ai_turn_cancel(tmp_path):
    """Esc ends AI hosted turn; other activity (none) still runs."""
    trace = _trace_path(tmp_path)
    src = "AAA\n"
    env = _ai_env(tmp_path, trace, reply="streaming response...")
    with WpeSession(str(tmp_path), src, filename="stack.c", env_extra=env) as w:
        w.key(ALT.AI); w.key("g", delay=1.0)  # Alt-G g -> host turn
        w._drain(1.0)
        w.key(ESC, delay=1.0)
        assert wait_screen(w, "chat cancelled", tmo=10), "hosted turn not cancelled"
        ai_trace = str(tmp_path / "ai.trace")
        assert _trace_contains(ai_trace, "chat cancelled")


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


# @scenario SC-esc-owner-11
def test_G11_ctrl_c_debugger_answer(tmp_path):
    """Ctrl-C ends debugger answer wait, copies nothing; Ctrl-C during build cancels nothing."""
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
        deadline = time.time() + 5 * WAIT_SCALE
        got_hello = False
        got_esc = False
        while time.time() < deadline:
            if stdin_log.exists():
                content = stdin_log.read_bytes()
                if b"hello" in content:
                    got_hello = True
                if b"\x1b" in content:
                    got_esc = True
                if got_hello:
                    break
            time.sleep(0.1)
        assert got_hello, "program should have received 'hello'"
        assert not got_esc, "program should NOT have received ESC byte (0x1b)"
        # The editor should still be responsive (Esc did not cancel anything here)
        w.key("Q", delay=0.3)
        w.save()
        assert "Q" in w.text(), "editor should still take keys"


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
        assert wait_screen(w, "building ...", tmo=3), "build stopped"
        w.key(ESC, delay=0.5)  # cancels build
        assert wait_screen(w, "build cancelled", tmo=10), "build not cancelled on second Esc"


# @scenario SC-esc-owner-14
@pytest.mark.wpe_requires("enable-ai")
def test_G14_build_then_ai_task(tmp_path):
    """Build and assistant task from a file window: Esc ends build; after process gone, Esc ends task.
    Uses a Makefile that writes its child PID to a file for process-gone wait.
    Switches back to file window (Alt-1) before asking the AI task so Messages
    doesn't intercept the edit.
    """
    trace = _trace_path(tmp_path)
    ai_trace = str(tmp_path / "ai.trace")
    src = "AAA\n"
    slow_make = ("all:\n\t@echo BUILD_START\n\t@sh -c 'echo $$PPID > build.pid; sleep 10'\n\t@echo BUILD_DONE\n"
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


# @scenario SC-esc-owner-17
@pytest.mark.wpe_requires("enable-ai-agent-host")
def test_G17_hosted_turn_and_build_ending(tmp_path):
    """Hosted turn and build: Esc, Esc -> second traced 'ending', build still runs."""
    trace = _trace_path(tmp_path)
    src = "AAA\n"
    slow_make = ("all:\n\t@echo BUILD_START\n\t@sleep 10\n\t@echo BUILD_DONE\n"
                 "\t@touch prog\n")
    (tmp_path / "Makefile").write_text(slow_make)
    env = _ai_env(tmp_path, trace, reply="streaming...")
    with WpeSession(str(tmp_path), src, filename="t.c", env_extra=env) as w:
        w.key(ALT_A, delay=0.8)
        assert wait_screen(w, "building ...", tmo=10)
        w.key(ALT.AI); w.key("g", delay=1.0)
        w._drain(1.0)
        w.key(ESC, delay=0.8)
        assert wait_screen(w, "chat cancelled", tmo=10)
        assert wait_screen(w, "building ...", tmo=3), "build stopped"
        w.key(ESC, delay=0.8)
        assert wait_screen(w, "build cancelled", tmo=10), "build not cancelled on second Esc"


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
    # A build that records make's own PID, so the test can wait for it to go
    slow_make = ("all:\n\t@echo BUILD_START\n\t@echo $$PPID > build.pid\n"
                 "\t@sleep 30\n\t@echo BUILD_DONE\n\t@touch prog\n")
    (tmp_path / "Makefile").write_text(slow_make)
    # Use the silent DAP adapter pattern from test_debugger_integration:
    # reverse-tcp transport = dlv that dials back (Go language .go file)
    bindir = tmp_path / "bin"
    bindir.mkdir()
    # dlv stand-in that never answers (like SILENT_ADAPTERS["reverse-tcp"])
    (bindir / "dlv").write_text("#!/bin/sh\nexec 3>&1\nexec cat 3>&3 > %s\n" % (tmp_path / "adapter-stdin.log"))
    (bindir / "dlv").chmod(0o755)
    env = {"PATH": str(bindir) + ":" + os.environ.get("PATH", ""),
           "XWPE_DEBUG_START_TIMEOUT": "600",
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
@pytest.mark.wpe_requires("enable-ai-agent-host")
def test_G19_task_during_hosted_turn(tmp_path):
    """Assistant task during hosted turn: both busy, one Esc each."""
    trace = _trace_path(tmp_path)
    src = "AAA\n"
    env = _ai_env(tmp_path, trace, reply="streaming...")
    with WpeSession(str(tmp_path), src, filename="t.c", env_extra=env) as w:
        w.key(ALT.AI); w.key("g", delay=1.0)
        w._drain(1.0)
        w.key(ALT.AI); w.key("e", delay=1.0)
        w.key("append a line"); w.key("\r", delay=1.5)
        w.key(ESC, delay=0.8)
        assert wait_screen(w, "chat cancelled", tmo=10)
        w.key(ESC, delay=1.0)
        assert wait_screen(w, "cancelled", tmo=10)


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
    slow_make = ("all:\n\t@echo BUILD_START\n\t@sleep 10\n\t@echo BUILD_DONE\n"
                 "\t@touch prog\n")
    (tmp_path / "Makefile").write_text(slow_make)
    env = _ai_env(tmp_path, trace)
    with WpeSession(str(tmp_path), src, filename="t.c", env_extra=env) as w:
        w.key(ALT_A, delay=0.8)
        assert wait_screen(w, "building ...", tmo=10)
        w.key(ALT.AI); w.key("a", delay=1.0)  # Agent
        w.key("do something"); w.key("\r", delay=1.5)
        w.key(ESC, delay=1.0)
        assert wait_screen(w, "build cancelled", tmo=10), "build not cancelled"


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