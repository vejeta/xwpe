"""The debugger loads the program F9 wrote, under Exedir, on X11.

The program-name owner (we_progname.c) names the program F9 links, Run
starts and the debugger loads.  With Exedir set, F9 links <Exedir>/t.e, and
Ctrl-G R must tell the debugger to load that file.  The debugger was told the
bare t.e, a file F9 never wrote, until the debugger named its program through
the owner.  The terminal row is in tests/test_program_name.py
(test_debugger_loads_what_f9_wrote_under_exedir); the Wayland twin
(tests/wayland/test_program_name.py) runs this same body.

A stand-in gdb stands for the real one: it answers every line with a prompt
and logs what the editor sent it, so the row reads the word the debugger was
told to load and nothing else.
"""
import os
import time

import pytest
from host_tools import needs

from conftest import _sleep

EXEDIR = "out.d"
STAND_IN_LOG = "standin.log"
BUILD_WAIT_S = 40          # the compile and link are done by then
LOAD_WAIT_S = 30           # the debugger has been told its file by then

STAND_IN_GDB = """#!/bin/sh
case "$1" in --version) echo "GNU gdb (GDB) 15.1"; exit 0 ;; esac
printf '(gdb) '
while IFS= read -r line; do
  echo "gdb-sent $line" >> "%s"
  printf '(gdb) '
done
"""


def _wait(predicate, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        _sleep(0.2)
    return predicate()


@pytest.fixture
def exedir_with_stand_in_gdb(tmp_path, monkeypatch):
    """Exedir out.d in the user's own xwperc, a stand-in gdb first on PATH,
    both in the home the editor is started with (asked for before the
    editor).  Returns the stand-in's log."""
    log = tmp_path / STAND_IN_LOG
    log.write_text("")
    bindir = tmp_path / "bin"
    bindir.mkdir()
    gdb = bindir / "gdb"
    gdb.write_text(STAND_IN_GDB % log)
    gdb.chmod(0o755)
    (tmp_path / EXEDIR).mkdir()
    cfg = tmp_path / ".config" / "xwpe"
    cfg.mkdir(parents=True)
    (cfg / "xwperc").write_text("[Programming]\nExedir : %s\n" % EXEDIR)
    monkeypatch.setenv("PATH", str(bindir) + ":" + os.environ.get("PATH", ""))
    return log


def _loads(log):
    """The words the debugger was told to load: `file <word>`."""
    return [l.split(" file ", 1)[1].strip() for l in log.read_text().splitlines()
            if l.startswith("gdb-sent") and " file " in l]


# @scenario SC-program-name-01
@needs("gcc")
def test_debugger_loads_what_f9_wrote_under_exedir(tmp_path, exedir_with_stand_in_gdb, xwpe):
    """Exedir out.d, F9 links out.d/t.e; Ctrl-G R tells gdb to load that
    file, not the bare t.e."""
    log = exedir_with_stand_in_gdb
    _sleep(1.5)
    xwpe.key("F9", delay=1.0)
    program = tmp_path / EXEDIR / "t.e"
    assert _wait(program.exists, BUILD_WAIT_S), "F9 wrote no %s" % program
    xwpe.key("ctrl+g", "r", delay=1.0)
    assert _wait(lambda: _loads(log), LOAD_WAIT_S), \
        "gdb was told no file:\n" + log.read_text()
    loads = _loads(log)
    assert all(os.path.realpath(os.path.join(str(tmp_path), l)) == os.path.realpath(str(program))
               for l in loads), loads
    assert xwpe.proc.poll() is None, "xwpe died starting the debugger"
