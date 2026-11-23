"""The rows tests/test_row_clock.py runs one by one under a child pytest.

Each holds an editor, or waits, in a way that a hung row does, so the row
clock (tests/gui_owner.py, RowClock) has something to cut.  The parent sets
the budget with XWPE_ROW_BUDGET_S.  The file is in tests/ so that
tests/conftest.py is the conftest they run under, and its name does not begin
with test_, so a plain run of tests/ never collects it.
"""
import os
import signal

import pytest

import gui_owner
from wpe_driver import WpeSession, incoherence

# The file the rows that count their runs, and the one that says the editor
# was up, write to (named by the parent).
COUNT_FILE_VAR = "XWPE_INNER_COUNT_FILE"
STARTED_FILE_VAR = "XWPE_INNER_STARTED_FILE"
CLOSED_FILE_VAR = "XWPE_INNER_CLOSED_FILE"
NEVER_S = 100000


def note_in(variable):
    """Append a line to the file the parent named in @variable."""
    with open(os.environ[variable], "a", encoding="utf-8") as fh:
        fh.write("x\n")


def test_a_row_holding_an_editor_waits_forever(tmp_path):
    with WpeSession(str(tmp_path), "x\n", filename="t.c") as s:
        note_in(STARTED_FILE_VAR)
        while True:
            s._drain(0.3)


def test_a_wait_that_swallows_every_exception(tmp_path):
    """The loop a row writes to wait for a state: it catches what a wait
    raises and goes on, so only a clock that no handler can catch ends it."""
    with WpeSession(str(tmp_path), "x\n", filename="t.c") as s:
        note_in(STARTED_FILE_VAR)
        while True:
            try:
                s._drain(0.3)
            except AssertionError:
                pass
            except Exception:
                pass


@incoherence("a known gap")
def test_an_incoherence_row_that_hangs():
    gui_owner.wait_for(lambda: False, "an event that never comes", NEVER_S)


@pytest.mark.xfail(reason="a plain known gap, not an incoherence")
def test_a_plain_xfail_row_that_hangs_and_counts_its_runs():
    """An xfail that names nothing takes any failure for the known gap,
    including the clock's own."""
    note_in(COUNT_FILE_VAR)
    gui_owner.wait_for(lambda: False, "an event that never comes", NEVER_S)


def test_a_row_waits_while_the_budget_runs_out_in_the_start(tmp_path):
    """The budget is shorter than the editor's start: the expiry waits for
    the start to finish and cuts the row after it."""
    with WpeSession(str(tmp_path), "x\n", filename="t.c"):
        note_in(STARTED_FILE_VAR)
        gui_owner.wait_for(lambda: False, "an event that never comes", NEVER_S)


def test_the_budget_runs_out_inside_the_end_ladder(tmp_path, monkeypatch):
    """The expiry arrives while the owner ends the editor: the ladder must
    finish, and the row is cut at its next wait."""
    original = gui_owner.OwnedProcess._ladder

    def ladder_with_an_expiry(self):
        signal.raise_signal(signal.SIGALRM)
        return original(self)

    monkeypatch.setattr(gui_owner.OwnedProcess, "_ladder", ladder_with_an_expiry)
    with WpeSession(str(tmp_path), "x\n", filename="t.c"):
        note_in(STARTED_FILE_VAR)
    note_in(CLOSED_FILE_VAR)
    gui_owner.wait_for(lambda: False, "an event that never comes", NEVER_S)


def test_a_hung_row_that_counts_its_runs():
    note_in(COUNT_FILE_VAR)
    gui_owner.wait_for(lambda: False, "an event that never comes", NEVER_S)


def test_a_quick_row_before_the_hang():
    assert True


def test_another_quick_row_before_the_hang():
    assert True


def test_a_row_that_hangs_with_no_clock_of_its_own():
    gui_owner.wait_for(lambda: False, "an event that never comes", NEVER_S)
