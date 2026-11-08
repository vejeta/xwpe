"""How a line debugger's session ends: at Quit, when it dies, and with the
editor.

A debugger the editor starts (gdb, pdb, a68g, jdb and the stand-ins for
sdb, dbx and xdb) is ended without the editor waiting for it, and its end
is reported once, from the moment the system says it ended:

- the editor's own end (a "kill PID", a logout) asks the debugger to quit,
  so it ends its program itself; neither is left running, and the debugger
  is never sent a signal that dumps core;
- a debugger that dies while its program runs is reported once, with its
  exit status, even when a process it started keeps its output open, so
  the end of that output never comes;
- Quit returns at once and the session's end line follows;
- compiling a Rust program for its debug session leaves the editor its
  keys, as a build does.
"""
import os
import signal
import time

import pytest

import procinfo
from wpe_driver import ALT, WpeSession, WAIT_SCALE
from test_debugger_integration import (
    A68_PRINT_FIRST_LINE, A68_PRINT_PROG, A68_PRINT_STOP_LINE, JAVA_PROG,
    PY_BREAK_LINE, PY_PROG, START_TIMEOUT_S, _c_session, _children, _need,
    _start_a68_session_on, _start_c_session, _start_java_session,
    _start_py_session)
from test_legacy_debuggers import (
    _cursor_field, _mark_line, _messages, _screen,
    _sent, _session, _start_session, _wait_for)
from test_child_ends import _open_file
from test_signal_save import (
    ADAPTER_LOG, DAP_RUST_PROG, NEVER_STOPPING_ADAPTER, adapter_answered,
    wait_for_exit)

CTRL_G = "\x07"            # the debugger's prefix
DOWN = "\033OB"
EXIT_DEADLINE_S = 10 * WAIT_SCALE
GONE_S = 6                 # the owner's grace and kill grace, and slack
STUB_PID_FILE = "stub.pid"
HOLDER_PID_FILE = "holder.pid"
PROGRAM_PID_FILE = "prog.pid"
# Signals whose default action writes a core file.
CORE_SIGNALS = ("SIGBUS", "SIGSEGV", "SIGQUIT", "SIGABRT", "SIGILL",
                "SIGFPE")
END_LINE = "End of Debugging"
DIED_FMT = "%s ended unexpectedly (exit status %d)"
DIED_STATUS = 3            # what the dying stand-in exits with
QUIT_RETURNS_S = 1.5       # Quit's end line on screen, before scaling


def _read_pid(path):
    """The PID a process wrote into @path, or None."""
    if not os.path.exists(str(path)):
        return None
    text = open(str(path)).read().strip()
    return int(text) if text.isdigit() else None


def _gone(pid):
    return procinfo.run_state(pid) in (procinfo.GONE, procinfo.ZOMBIE)


def _wait_gone(pid, seconds=GONE_S):
    deadline = time.time() + seconds * WAIT_SCALE
    while time.time() < deadline and not _gone(pid):
        time.sleep(0.1)
    return _gone(pid)


def _end_if_left(pid):
    """A process a row left behind: end it, by its own PID."""
    if pid and not _gone(pid):
        os.kill(pid, signal.SIGKILL)


def _messages_above_status_row(w):
    """Messages' text without the screen's last row, the status row, which
    says the same line once more."""
    return "\n".join(_messages(w).split("\n")[:-1])


def _stub_signals(tmp_path, backend):
    """The signals the stand-in logged before it ended."""
    log = tmp_path / (backend + ".log")
    if not log.exists():
        return []
    return [line[len("SIGNAL "):] for line in log.read_text().splitlines()
            if line.startswith("SIGNAL ")]


# @scenario SC-debug-57
def test_the_editors_exit_ends_the_debugger_and_its_program(tmp_path):
    """SIGTERM to the editor during an sdb session stopped at a breakpoint:
    the editor ends, the debugger is asked to quit and goes, and it was
    sent no signal that dumps core.  The editor used to send the debugger
    signal 7, a SIGBUS on Linux: a core file for every logout during a
    debug session."""
    extra = {"XWPE_STUB_NOTE_SIGNALS": "1",
             "XWPE_STUB_PID_FILE": str(tmp_path / STUB_PID_FILE)}
    pid = None
    try:
        with _session(tmp_path, "sdb", extra=extra) as w:
            _start_session(w, tmp_path, "sdb")
            pid = _read_pid(tmp_path / STUB_PID_FILE)
            assert pid, "the stand-in wrote no PID"
            os.kill(w.proc.pid, signal.SIGTERM)
            code = wait_for_exit(w, EXIT_DEADLINE_S)
            assert code is not None, "wpe still running after SIGTERM"
        assert _wait_gone(pid), "the debugger (pid %d) was left running" % pid
        cores = [s for s in _stub_signals(tmp_path, "sdb")
                 if s in CORE_SIGNALS]
        assert not cores, "the debugger was sent %s, which dumps core" % cores
        assert "q" in _sent(tmp_path, "sdb"), \
            "the debugger should have been asked to quit: %r" \
            % _sent(tmp_path, "sdb")
    finally:
        _end_if_left(pid)


# A C program that names its PID and then loops at its breakpoint's line.
C_LOOP_PROG = (
    "#include <stdio.h>\n"
    "#include <unistd.h>\n"
    "int main(void)\n"
    "{\n"
    "  FILE *fp = fopen(\"%s\", \"w\");\n"
    "  fprintf(fp, \"%%d\\n\", (int)getpid());\n"
    "  fclose(fp);\n"
    "  int x = 3;\n"
    "  while (x)\n"
    "    sleep(1);\n"
    "  return 0;\n"
    "}\n"
)


# @scenario SC-debug-57
def test_the_editors_exit_ends_gdb_and_its_program(tmp_path):
    """SIGTERM to the editor while gdb holds the program at a breakpoint:
    neither gdb nor the program is left.  Guard row: the program also ends
    by its terminal's hangup when gdb is killed, so this passed before
    gdb was asked to quit."""
    _need("gcc", "gdb")
    prog_pid = gdb = None
    prog = C_LOOP_PROG % str(tmp_path / PROGRAM_PID_FILE)
    try:
        with WpeSession(str(tmp_path), prog, filename="p.c",
                        wait=1.5) as w:
            _start_c_session(w)
            prog_pid = _read_pid(tmp_path / PROGRAM_PID_FILE)
            gdbs = _children(w, "gdb")
            assert prog_pid and len(gdbs) == 1, (prog_pid, gdbs)
            gdb = gdbs[0]
            os.kill(w.proc.pid, signal.SIGTERM)
            assert wait_for_exit(w, EXIT_DEADLINE_S) is not None, \
                "wpe still running after SIGTERM"
        assert _wait_gone(gdb), "gdb (pid %d) was left running" % gdb
        assert _wait_gone(prog_pid), \
            "the program (pid %d) was left running" % prog_pid
    finally:
        _end_if_left(gdb)
        _end_if_left(prog_pid)


# @scenario SC-debug-79
def test_a_debugger_that_dies_is_reported_once_with_its_status(tmp_path):
    """sdb dies with exit status 3 after a continue, while a process it
    started keeps its output open: no end of file ever comes.  The session
    ends at the debugger's own end, Messages says once that it ended
    unexpectedly with that status, and the editor takes keys.  It used to
    wait for the end of the output, so the session never ended."""
    holder = tmp_path / HOLDER_PID_FILE
    extra = {"XWPE_STUB_CONTINUE_DIES_HOLDING": str(DIED_STATUS),
             "XWPE_STUB_HOLDER_PID": str(holder)}
    died = DIED_FMT % ("sdb", DIED_STATUS)
    try:
        with _session(tmp_path, "sdb", extra=extra) as w:
            _start_session(w, tmp_path, "sdb")
            w.key(CTRL_G, "r", delay=0.5)
            assert _wait_for(w, lambda: died in _screen(w), GONE_S), \
                "Messages should say %r; sdb was sent %r:\n%s" \
                % (died, _sent(tmp_path, "sdb"), _screen(w))
            w._drain(1.0)
            said = _messages_above_status_row(w).count("ended unexpectedly")
            assert said == 1, "the end was said %d times:\n%s" \
                % (said, _screen(w))
            assert END_LINE not in _screen(w), _screen(w)
            assert _mark_line(w) is None, _screen(w)
            before = _cursor_field(w)
            w.key(DOWN, delay=0.5)
            assert _cursor_field(w) != before, \
                "the editor should take keys:\n%s" % _screen(w)
    finally:
        _end_if_left(_read_pid(holder))


def _quit_and_time(w):
    """Debug > Quit; seconds until the end line is on screen, or None."""
    began = time.time()
    w.menu(ALT.DEBUG, "q", delay=0.0)
    if not _wait_for(w, lambda: END_LINE in _screen(w), QUIT_RETURNS_S * 4):
        return None
    return time.time() - began


def _start_real(tmp_path, debugger):
    """A session of @debugger stopped at its first breakpoint."""
    if debugger == "gdb":
        _need("gcc", "gdb")
        w = _c_session(tmp_path)
        _start_c_session(w)
        return w
    if debugger == "pdb":
        _need("python3")
        w = WpeSession(str(tmp_path), PY_PROG, filename="p.py", wait=1.5)
        _start_py_session(w)
        assert _wait_for(w, lambda: _mark_line(w) == PY_BREAK_LINE,
                         START_TIMEOUT_S), _screen(w)
        return w
    if debugger == "a68g":
        _need("a68g")
        w = WpeSession(str(tmp_path), A68_PRINT_PROG, filename="p.a68",
                       wait=1.5)
        _start_a68_session_on(w, A68_PRINT_FIRST_LINE, A68_PRINT_STOP_LINE)
        return w
    _need("jdb", "javac")
    w = WpeSession(str(tmp_path), JAVA_PROG, filename="P.java", wait=1.5)
    _start_java_session(w)
    return w


def _start_any(tmp_path, debugger):
    if debugger in ("sdb", "dbx", "xdb"):
        w = _session(tmp_path, debugger)
        _start_session(w, tmp_path, debugger)
        return w
    return _start_real(tmp_path, debugger)


# @scenario SC-debug-80
@pytest.mark.parametrize("debugger",
                         ["gdb", "pdb", "a68g", "jdb", "sdb", "dbx", "xdb"])
def test_quit_returns_at_once_and_the_end_line_comes_after(tmp_path,
                                                           debugger):
    """Debug > Quit at a stop: the end line is on screen at once, the
    editor takes keys, and the debugger is gone within the owner's grace.
    Guard row: Quit already returned at once, by SIGKILL."""
    name = {"pdb": "python3"}.get(debugger, debugger)
    w = _start_any(tmp_path, debugger)
    try:
        pids = _children(w, name)
        assert len(pids) == 1, "the editor's one %s: %s" % (name, pids)
        took = _quit_and_time(w)
        assert took is not None and took < QUIT_RETURNS_S * WAIT_SCALE, \
            "Quit took %s s to say %r:\n%s" % (took, END_LINE, _screen(w))
        assert _screen(w).count(END_LINE) == 1, _screen(w)
        before = _cursor_field(w)
        w.key(DOWN, delay=0.5)
        assert _cursor_field(w) != before, \
            "the editor should take keys:\n%s" % _screen(w)
        assert _wait_gone(pids[0]), "%s (pid %d) is still running" \
            % (name, pids[0])
    finally:
        w.close()


# A rustc that names its PID and takes its time before it writes the empty
# binary the -o option names.
SLOW_RUSTC = ("#!/bin/sh\n"
              "echo $$ > %(pidfile)s\n"
              "sleep %(sleep)d\n"
              "while [ $# -gt 0 ]; do\n"
              "  if [ \"$1\" = -o ]; then : > \"$2\"; fi\n"
              "  shift\n"
              "done\n")
SLOW_RUSTC_S = 6
RUSTC_PID_FILE = "rustc.pid"
WINDOW_MENU_ITEM = "Size/Move"     # the Window menu's first item


def _slow_rustc_env(tmp_path):
    """A stand-in rustc that sleeps, and a stand-in adapter called gdb."""
    bindir = tmp_path / "bin"
    bindir.mkdir()
    adapter = bindir / "gdb"
    adapter.write_text(NEVER_STOPPING_ADAPTER % str(tmp_path / ADAPTER_LOG))
    adapter.chmod(0o755)
    rustc = bindir / "rustc"
    rustc.write_text(SLOW_RUSTC % {"pidfile": str(tmp_path / RUSTC_PID_FILE),
                                   "sleep": SLOW_RUSTC_S})
    rustc.chmod(0o755)
    return {"PATH": str(bindir) + ":" + os.environ.get("PATH", "")}


# @scenario SC-debug-81
def test_compile_for_debug_takes_keys(tmp_path):
    """Ctrl-G R on a Rust file compiles it first, with a compiler that takes
    six seconds: the editor takes keys meanwhile, and the session starts
    once the compiler has ended.  It used to wait for the compiler, deaf to
    every key."""
    rustc = None
    try:
        with WpeSession(str(tmp_path), DAP_RUST_PROG, filename="main.rs",
                        wait=1.5, env_extra=_slow_rustc_env(tmp_path)) as w:
            w.key(CTRL_G, "r", delay=0.2)
            assert _wait_for(w, lambda: _read_pid(tmp_path / RUSTC_PID_FILE),
                             SLOW_RUSTC_S / 2), \
                "the compiler did not start:\n%s" % _screen(w)
            rustc = _read_pid(tmp_path / RUSTC_PID_FILE)
            w.key(ALT.WINDOW, delay=0.2)
            assert _wait_for(w, lambda: WINDOW_MENU_ITEM in _screen(w), 1.0), \
                "the editor should take keys while the compiler runs:\n%s" \
                % _screen(w)
            assert not _gone(rustc), \
                "the compiler should still be running for this to show"
            w.key("\033", delay=0.3)        # the menu closes; nothing else
            assert _wait_for(w, lambda: adapter_answered(tmp_path, "launch"),
                             SLOW_RUSTC_S + START_TIMEOUT_S), \
                "the session should start once compiled:\n%s" % _screen(w)
    finally:
        _end_if_left(rustc)


# @scenario SC-debug-81
def test_esc_cancels_compile_for_debug(tmp_path):
    """Esc while the program compiles for its debug session ends the
    compiler, says so, and starts no session after."""
    rustc = None
    try:
        with WpeSession(str(tmp_path), DAP_RUST_PROG, filename="main.rs",
                        wait=1.5, env_extra=_slow_rustc_env(tmp_path)) as w:
            w.key(CTRL_G, "r", delay=0.2)
            assert _wait_for(w, lambda: _read_pid(tmp_path / RUSTC_PID_FILE),
                             SLOW_RUSTC_S / 2), _screen(w)
            rustc = _read_pid(tmp_path / RUSTC_PID_FILE)
            w.key("\033", delay=0.5)
            assert _wait_for(w, lambda: "compile for debug cancelled"
                             in _screen(w), 2.0), _screen(w)
            assert _wait_gone(rustc), "the compiler (pid %d) is still " \
                "running" % rustc
            w._drain(SLOW_RUSTC_S)
            assert not adapter_answered(tmp_path, "launch"), \
                "no session should start after Esc:\n%s" % _screen(w)
    finally:
        _end_if_left(rustc)


# @scenario SC-debug-81
def test_quit_cancels_compile_for_debug(tmp_path):
    """Debug > Quit while the program compiles for its debug session ends
    the compiler and starts no session after it.  The session used to start
    anyway once the compiler had finished."""
    rustc = None
    try:
        with WpeSession(str(tmp_path), DAP_RUST_PROG, filename="main.rs",
                        wait=1.5, env_extra=_slow_rustc_env(tmp_path)) as w:
            w.key(CTRL_G, "r", delay=0.2)
            assert _wait_for(w, lambda: _read_pid(tmp_path / RUSTC_PID_FILE),
                             SLOW_RUSTC_S / 2), _screen(w)
            rustc = _read_pid(tmp_path / RUSTC_PID_FILE)
            w.menu(ALT.DEBUG, "q", delay=0.5)
            assert _wait_gone(rustc), "the compiler (pid %d) is still " \
                "running" % rustc
            w._drain(SLOW_RUSTC_S)
            assert not adapter_answered(tmp_path, "launch"), \
                "no session should start after Quit:\n%s" % _screen(w)
    finally:
        _end_if_left(rustc)


# The slow rustc, also writing down each source it is asked to compile.
NAMING_RUSTC = ("#!/bin/sh\n"
                "echo \"$@\" >> %(argslog)s\n") + SLOW_RUSTC.split("\n", 1)[1]
RUSTC_ARGS_LOG = "rustc.args"
OTHER_RUST_FILE = "other.rs"


# @scenario SC-debug-81
def test_a_file_opened_while_it_compiles_is_never_debugged_with_its_binary(
        tmp_path):
    """Ctrl-G R on main.rs, and other.rs opened while main.rs compiles: the
    session that starts once the compiler has ended is other.rs's, on top,
    so other.rs is compiled for it too.  It used to be launched at once on
    main.rs's binary."""
    env = _slow_rustc_env(tmp_path)
    (tmp_path / "bin" / "rustc").write_text(
        NAMING_RUSTC % {"argslog": str(tmp_path / RUSTC_ARGS_LOG),
                        "pidfile": str(tmp_path / RUSTC_PID_FILE),
                        "sleep": SLOW_RUSTC_S})
    (tmp_path / OTHER_RUST_FILE).write_text(DAP_RUST_PROG)
    rustc = None
    try:
        with WpeSession(str(tmp_path), DAP_RUST_PROG, filename="main.rs",
                        wait=1.5, env_extra=env) as w:
            w.key(CTRL_G, "r", delay=0.2)
            assert _wait_for(w, lambda: _read_pid(tmp_path / RUSTC_PID_FILE),
                             SLOW_RUSTC_S / 2), _screen(w)
            rustc = _read_pid(tmp_path / RUSTC_PID_FILE)
            _open_file(w, OTHER_RUST_FILE)
            assert _wait_for(w, lambda: adapter_answered(tmp_path, "launch"),
                             2 * SLOW_RUSTC_S + START_TIMEOUT_S), \
                "a session should start:\n%s" % _screen(w)
            compiled = (tmp_path / RUSTC_ARGS_LOG).read_text()
            assert OTHER_RUST_FILE in compiled, \
                "the session of %s started on a binary compiled from: %r" \
                % (OTHER_RUST_FILE, compiled)
    finally:
        _end_if_left(rustc)
        _end_if_left(_read_pid(tmp_path / RUSTC_PID_FILE))
