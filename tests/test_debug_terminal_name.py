"""The debugger start learns the editor's terminal without another program.

Without a pty for the program, gdb is told to write to the editor's own
terminal, and a start whose terminal name is unknown is refused ("tty
error").  The name used to come from running tty(1), which raced the
editor's child-exit handler and made about one start in fifteen fail on
BSD systems.  The name now comes from the kernel, so a start must work
even where no tty program can be found at all.
"""
import os
import shutil

import pytest

from wpe_driver import WpeSession
from test_debugger_integration import (C_PROG, _children, _need,
                                       _start_c_session)

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
