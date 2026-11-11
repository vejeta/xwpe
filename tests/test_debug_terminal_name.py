"""The debugger start learns the editor's terminal without another program.

The program under the debugger writes to its own pty, and only when no pty
could be opened to the editor's terminal.  A start with no terminal for the
program at all is refused ("No terminal for the program").  The editor's
terminal name used to come from running tty(1), which raced the editor's
child-exit handler and made about one start in fifteen fail on BSD systems.
It now comes from the kernel, so a start must work even where no tty
program can be found.  An editor whose input is not a terminal (started
from a desktop launcher) debugs too, because the program's pty is enough.
"""
import os
import shutil

import pytest

from wpe_driver import WpeSession
from test_debugger_integration import (C_PROG, _children, _need,
                                       _start_c_session, _wait_for, _screen,
                                       _step, F8, C_NEXT_LINE,
                                       C_AFTER_PRINT_LINE, STEP_TIMEOUT_S)
import test_legacy_debuggers as legacy

# What the session needs to build and debug a C file: nothing else, above
# all no tty.  Names are looked up on the host; one that is absent is
# skipped (cc may be the same program as gcc).
TOOLS_THE_SESSION_NEEDS = ["gdb", "gcc", "cc", "as", "ld", "collect2"]


def _bindir_without_tty(tmp_path):
    """A directory of links to the build and debug tools only, as a PATH."""
    bindir = tmp_path / "toolbin"
    bindir.mkdir()
    for tool in TOOLS_THE_SESSION_NEEDS:
        found = shutil.which(tool)
        if found is not None:
            os.symlink(found, str(bindir / tool))
    assert not (bindir / "tty").exists()
    return str(bindir)


# @scenario SC-debug-01
def test_gdb_starts_where_no_tty_program_can_be_found(tmp_path):
    """Ctrl-G R stops on the breakpoint with a PATH that holds no tty
    program: the terminal's name is not learned by running one."""
    _need("gcc", "gdb")
    env = {"PATH": _bindir_without_tty(tmp_path)}
    with WpeSession(str(tmp_path), C_PROG, filename="p.c", wait=1.5,
                    env_extra=env) as w:
        _start_c_session(w)
        assert _children(w, "gdb"), "gdb should be running while stopped"


# @scenario SC-debug-01
def test_gdb_starts_when_the_editors_input_is_not_a_terminal(tmp_path):
    """Ctrl-G R stops on the breakpoint when the editor's input is a pipe,
    as under a desktop launcher, and the program's output reaches the
    editor: the program's pty is its terminal."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), C_PROG, filename="p.c", wait=1.5,
                    stdin_is_terminal=False) as w:
        _start_c_session(w)
        assert _children(w, "gdb"), "gdb should be running while stopped"
        for line in (C_NEXT_LINE, C_NEXT_LINE + 1, C_AFTER_PRINT_LINE):
            assert _step(w, F8, line), \
                "F8 should reach line %d:\n%s" % (line, _screen(w))
        assert _wait_for(w, lambda: "OUTMARK 9" in _screen(w),
                         STEP_TIMEOUT_S), \
            "the program's output should reach the editor:\n%s" % _screen(w)


# @scenario SC-debug-01
@pytest.mark.parametrize("backend", legacy.BACKENDS)
def test_a_legacy_debugger_runs_the_program_on_its_pty_without_a_terminal(
        tmp_path, backend):
    """sdb, dbx and xdb are told to write the program to its pty when the
    editor's input is not a terminal, so the session starts instead of
    being refused.

    Limit: a stub of the debugger proves what xwpe sends, not how a real
    %s behaves.""" % backend
    with legacy._session(tmp_path, backend, stdin_is_terminal=False) as w:
        legacy._start_session(w, tmp_path, backend)
        runs = [c for c in legacy._commands(tmp_path, backend)
                if legacy._plain(c) == legacy.START_RUN_COMMAND[backend]]
        assert runs and runs[0].split(" > ")[1].startswith("/dev/pts/"), \
            "%s should run the program on a pty; it got %r" \
            % (backend, legacy._commands(tmp_path, backend))
        w.key(legacy.ALT_X, delay=0.5)
