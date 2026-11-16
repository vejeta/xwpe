"""What the terminal driver gives its editor, and what it does when it ends it.

WpeSession starts the editor through the process owner (tests/gui_owner.py),
which gives it an allow-listed environment, a pty of the session's size as its
controlling terminal and an end that always runs.  The rows here start a
stand-in for the editor (tests/test_gui_owner.py run as a program) and read
what it records, so they need no display and no built editor.
"""
import os
import shlex
import sys

import pytest

import gui_owner
import test_gui_owner as owner_rows
import wpe_driver
from test_debugger_integration import gdb_interface  # noqa: F401  (the fixture under test)
from test_debugger_integration import GDB_INTERFACE_ENV
from test_gui_owner import owner  # noqa: F401  (an owner of the row's own)

SESSION_COLS, SESSION_ROWS = 100, 40
WAIT_FOR_RECORD_S = owner_rows.READY_S


def _write_leaker_launcher(folder, comm):
    """An executable a session takes for the editor binary: it leaves a child
    behind that ignores SIGTERM, as an editor that left a debugger would."""
    path = os.path.join(str(folder), "leaker-editor")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("#!/bin/sh\nexec %s -E -s %s --leaker %s\n" % (
            shlex.quote(sys.executable), shlex.quote(owner_rows.THIS),
            shlex.quote(comm)))
    os.chmod(path, 0o755)
    return path


@pytest.fixture
def recording_editor(tmp_path, monkeypatch):
    """The session's editor binary replaced by the recording stand-in; the
    folder its records go to."""
    records = tmp_path / "records"
    monkeypatch.setattr(wpe_driver, "WPE_BIN",
                        owner_rows._write_launcher(tmp_path, records))
    work = tmp_path / "work"
    work.mkdir()
    return records, str(work)


def _session(work, **kwargs):
    """A session on the stand-in, which draws nothing, so no wait."""
    return wpe_driver.WpeSession(work, "int main(void){}\n", wait=0.2, **kwargs)


# @scenario SC-harness-owner-68
def test_a_session_editor_has_the_sessions_size_and_no_size_variables(
        recording_editor, monkeypatch):
    """The pty is the size the session asked for, and a LINES or COLUMNS in
    the row's environment (or the machine's) never reaches the editor."""
    records, work = recording_editor
    monkeypatch.setenv("LINES", "7")
    with _session(work, cols=SESSION_COLS, rows=SESSION_ROWS,
                  env_extra={"COLUMNS": "9"}):
        rec = owner_rows._wait_record(records)
    assert rec["size"] == "%dx%d" % (SESSION_COLS, SESSION_ROWS)
    assert not rec["names"] & {"LINES", "COLUMNS"}


# @scenario SC-harness-owner-69
def test_a_session_resize_reaches_the_editor_as_a_window_change(recording_editor):
    records, work = recording_editor
    with _session(work) as session:
        owner_rows._wait_record(records)
        session.resize(90, 25)
        gui_owner.wait_for(
            lambda: owner_rows._read_record(records)["winch"],
            "the editor to see the new size", WAIT_FOR_RECORD_S)


# @scenario SC-harness-owner-70
def test_a_session_takes_one_more_variable_only_when_it_names_it(
        recording_editor, monkeypatch):
    records, work = recording_editor
    monkeypatch.setenv("TPDE_KEY", "s3cret-value")
    monkeypatch.setenv("TPDE_OTHER", "not-asked-for")
    with _session(work, inherit=("TPDE_KEY",)):
        rec = owner_rows._wait_record(records)
    assert "TPDE_KEY" in rec["names"]
    assert "TPDE_OTHER" not in rec["names"]
    assert "s3cret" not in "".join(rec["values"].values())


# @scenario SC-harness-owner-71
def test_a_session_row_can_remove_the_key_style(recording_editor):
    """None in env_extra removes the variable the owner sets (the first-run
    question and the rows of another key style need the editor to have none)."""
    records, work = recording_editor
    with _session(work, env_extra={"XWPE_KEYS": None}):
        rec = owner_rows._wait_record(records)
    assert "XWPE_KEYS" not in rec["names"]


# @scenario SC-harness-owner-76
def test_a_trace_the_machine_asks_for_reaches_the_editor_and_the_rows_own_wins(
        recording_editor, monkeypatch):
    """A developer or CI step that sets XWPE_UI_TRACE for the whole run gets
    the editor's trace; a row that sets its own trace gets that one."""
    records, work = recording_editor
    monkeypatch.setenv("XWPE_UI_TRACE", "/machine/trace")
    with _session(work) as session:
        rec = owner_rows._wait_record(records)
        assert session.ui_trace == "/machine/trace"
    assert rec["names"] >= {"XWPE_UI_TRACE"}
    other = os.path.join(os.path.dirname(work), "work2")
    os.mkdir(other)
    with _session(other, env_extra={"XWPE_UI_TRACE": "/row/trace"}) as session:
        assert session.ui_trace == "/row/trace"


# @scenario SC-harness-owner-77
def test_a_session_with_the_real_login_gets_the_links_and_no_key(
        recording_editor, monkeypatch, tmp_path):
    """real_login links the login and nothing else: a key of the machine's
    reaches only a row that names it (the live ones).  Only names are read."""
    records, work = recording_editor
    fake_home = tmp_path / "home"
    fake_home.mkdir()
    (fake_home / ".claude").mkdir()
    monkeypatch.setenv("HOME", str(fake_home))
    monkeypatch.setenv("ANTHROPIC_API_KEY", "planted-fake-value")
    with _session(work, real_login=True):
        rec = owner_rows._wait_record(records)
    assert "ANTHROPIC_API_KEY" not in rec["names"]
    assert os.path.islink(os.path.join(work, ".claude"))


def _write_dump_launcher(folder, fifo):
    """An executable an EditSession takes for the editor: it speaks the dump
    protocol and, released through the named pipe, writes one more dump as
    it leaves."""
    path = os.path.join(str(folder), "dump-editor")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("#!/bin/sh\nexec %s -E -s %s --dump-editor %s\n" % (
            shlex.quote(sys.executable), shlex.quote(owner_rows.THIS),
            shlex.quote(str(fifo))))
    os.chmod(path, 0o755)
    return path


# @scenario SC-harness-owner-80
def test_a_dump_an_editor_writes_as_it_leaves_reaches_the_edit_session(tmp_path):
    """The last dump is written when the owner waits for the editor's end,
    after every read the owner makes before that wait
    (test_gui_owner.hold_the_last_write), so only the read after the end can
    take it."""
    from edit_model.oracle import EditSession
    fifo = tmp_path / "go"
    os.mkfifo(str(fifo))
    work = tmp_path / "work"
    work.mkdir()
    with EditSession(str(work), "a\n",
                     binary=_write_dump_launcher(tmp_path, fifo)) as s:
        assert not s.reader.dumps
        owner_rows.hold_the_last_write(s.owned, str(fifo))
    assert [d.seq for d in s.reader.dumps] == [1], \
        "the dump written on the way out was lost: %r" % s.reader.dumps


@pytest.mark.parametrize("gdb_interface", ["mi", "cli"], indirect=True)
# @scenario SC-harness-owner-72
def test_the_gdb_interface_a_row_asks_for_reaches_its_session_editor(
        gdb_interface, recording_editor):
    """The rows of test_debugger_integration.py say which gdb interface they
    drive in a fixture; an editor that is given nothing of the test
    process's environment must still get it, or every machine-interface row
    would quietly run the line interface."""
    records, work = recording_editor
    with _session(work):
        rec = owner_rows._wait_record(records)
    assert rec["values"].get(GDB_INTERFACE_ENV) == gdb_interface, \
        "the editor was not told the interface: %r" % rec["values"].get(GDB_INTERFACE_ENV)


# @scenario SC-harness-owner-73
def test_a_session_that_left_something_does_not_hide_the_rows_own_failure(
        tmp_path, monkeypatch, owner):
    """Closing a session whose editor left a process behind ends it and holds
    the leak for the row's end: the exit of a `with` block raises nothing
    that would replace the failure the row itself had."""
    monkeypatch.setattr(wpe_driver, "WPE_BIN",
                        _write_leaker_launcher(tmp_path, owner_rows.LEAK_COMM))
    work = tmp_path / "work"
    work.mkdir()
    previous = gui_owner._CURRENT
    gui_owner.install(owner)
    before = owner.ledger.pids()
    try:
        with pytest.raises(AssertionError, match="the row's own failure"):
            with _session(str(work)):
                raise AssertionError("the row's own failure")
        with pytest.raises(gui_owner.LeakedProcesses) as raised:
            owner.end_since(before)
        assert owner_rows.LEAK_COMM in str(raised.value)
    finally:
        gui_owner.install(previous)
