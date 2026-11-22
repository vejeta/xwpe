"""The rows test_harness_verdicts.py runs, one by one, under a child pytest.

Each is marked as a recorded incoherence and ends in a different way.  Only
the first is the known gap; every other is a failure of the run, whatever it
is marked.  The file is in tests/ so that tests/conftest.py is the conftest
they run under, and its name does not begin with test_, so a plain run of
tests/ never collects it.
"""
import os
import signal

import gui_owner
from wpe_driver import WpeSession, incoherence


@incoherence("a known gap")
def test_a_failed_assertion():
    assert 1 == 2


@incoherence("a known gap")
def test_a_crash_of_the_row():
    raise RuntimeError("the row broke")


@incoherence("a known gap")
def test_an_owner_timeout():
    raise gui_owner.OwnerTimeout("waited for an event that never came")


@incoherence("a known gap")
def test_an_unexpected_exit():
    raise gui_owner.UnexpectedExit("the editor ended by itself")


@incoherence("a known gap")
def test_an_editor_killed_by_a_crash_signal(tmp_path):
    with WpeSession(str(tmp_path), "x\n", filename="t.c") as s:
        os.kill(s.proc.pid, signal.SIGSEGV)
        s._drain(1.0)
