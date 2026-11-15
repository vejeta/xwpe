"""The Python side of the platform owner: what the system says about paths,
descriptors, CPU time, signals and the pty's name (tests/procinfo.py).

Each helper answers on every system, and the rows that used to read Linux's
/proc by hand now ask it.  The rows here run the helper twice where the
answer comes from two places: once as Linux gives it, and once with the
procfs switched off (has_procfs answers False, as on macOS and the BSDs), so
the path macOS takes is run on Linux and must give the same answer.  A row
that returns the old defect (None where a number is due, an assert on a
missing line) is red on a tree without the helper.

Run: tests/.venv/bin/python -m pytest -q tests/test_procinfo.py
"""
import os
import pty
import signal
import subprocess
import time

import pytest

import procinfo
from host_tools import needs

SLEEP_S = 60
CLOCK_TOLERANCE_S = 1.5     # ps prints whole seconds where /proc has ticks
BURN_S = 1.2
OPENBSD_FSTAT = """USER     CMD          PID   FD MOUNT        INUM MODE         R/W    SZ|DV
user     fake sleep  %(pid)d   wd /           2 drwxr-xr-x     r      512
user     fake sleep  %(pid)d text /           9 -rwxr-xr-x     r    12288
user     fake sleep  %(pid)d    0* pipe 0xfffffd803 state: R
user     fake sleep  %(pid)d    1 /dev     12 crw--w----   rw    pts/3
user     fake sleep  %(pid)d    2 /dev     12 crw--w----   rw    pts/3
user     other       %(other)d    9 /dev     12 crw--w----   rw    pts/3
"""


def _sleeper(*pass_fds):
    """A child that sleeps, holding @pass_fds open at the same numbers."""
    return subprocess.Popen(["sleep", str(SLEEP_S)], pass_fds=pass_fds)


def _end(child):
    child.kill()
    child.wait()


@pytest.fixture
def no_procfs(monkeypatch):
    """Make the helper answer as it does where there is no /proc."""
    monkeypatch.setattr(procinfo, "has_procfs", lambda: False)


# ---------------------------------------------------------------- path_max

def test_path_max_is_the_systems_limit_for_a_folder_that_exists(tmp_path):
    assert procinfo.path_max(tmp_path) == os.pathconf(str(tmp_path), "PC_PATH_MAX")


def test_path_max_of_a_folder_not_made_yet_asks_the_nearest_one(tmp_path):
    """A row asks before it makes its deep folder."""
    below = tmp_path / "not" / "made" / "yet"
    assert procinfo.path_max(below) == procinfo.path_max(tmp_path)


def test_path_max_when_the_system_will_not_say_is_the_smallest_limit(tmp_path, monkeypatch):
    def refuse(path, name):
        raise OSError("no limit")
    monkeypatch.setattr(os, "pathconf", refuse)
    assert procinfo.path_max(tmp_path) == procinfo.PATH_MAX_FALLBACK


# ------------------------------------------------------- open_descriptors

def test_open_descriptors_lists_the_numbers_a_child_holds():
    reader, writer = os.pipe()
    child = _sleeper(writer)
    try:
        held = procinfo.open_descriptors(child.pid)
    finally:
        _end(child)
        os.close(reader)
        os.close(writer)
    assert held is not None
    assert {0, 1, 2, writer} <= set(held), held


def test_open_descriptors_of_a_process_that_is_gone_is_none():
    child = _sleeper()
    _end(child)
    assert procinfo.open_descriptors(child.pid) is None


@needs("lsof")
def test_open_descriptors_without_a_procfs_answers_as_the_procfs_does(monkeypatch):
    """The macOS route (lsof) gives the same numbers as /proc does."""
    if not procinfo.has_procfs():
        pytest.skip("needs /proc to compare with")
    reader, writer = os.pipe()
    child = _sleeper(writer)
    try:
        by_procfs = procinfo.open_descriptors(child.pid)
        monkeypatch.setattr(procinfo, "has_procfs", lambda: False)
        by_lsof = procinfo.open_descriptors(child.pid)
    finally:
        _end(child)
        os.close(reader)
        os.close(writer)
    assert by_lsof is not None and by_lsof == by_procfs, (by_lsof, by_procfs)


def _only_fstat_on_the_path(tmp_path, monkeypatch, pid):
    """A PATH that has an fstat in the BSD format and no lsof."""
    bindir = tmp_path / "bin"
    bindir.mkdir()
    listing = tmp_path / "fstat.out"
    listing.write_text(OPENBSD_FSTAT % {"pid": pid, "other": pid + 1})
    tool = bindir / "fstat"
    tool.write_text("#!/bin/sh\ncat %s\n" % listing)
    tool.chmod(0o755)
    monkeypatch.setenv("PATH", "%s:/bin:/usr/bin" % bindir)
    # keep the system's own lsof out of the way
    monkeypatch.setattr(procinfo, "_lsof_descriptors", lambda pid: None)


def test_open_descriptors_reads_the_fstat_of_the_bsds(tmp_path, monkeypatch, no_procfs):
    """The row for the BSDs, where lsof is not in the base system: fstat -p
    prints the working folder and the program by a word, the others by number,
    and another process's rows are not taken."""
    _only_fstat_on_the_path(tmp_path, monkeypatch, 4242)
    assert procinfo.open_descriptors(4242) == [0, 1, 2]


def test_open_descriptors_with_no_tool_that_answers_is_none(tmp_path, monkeypatch, no_procfs):
    """The row then skips by name: needs a descriptor table."""
    monkeypatch.setenv("PATH", str(tmp_path))
    assert procinfo.open_descriptors(os.getpid()) is None


# ------------------------------------------------------------- cpu_seconds

def _burn(seconds):
    """Use CPU in this process for about @seconds."""
    end = time.process_time() + seconds
    while time.process_time() < end:
        pass


def test_cpu_seconds_counts_the_time_a_process_burned():
    before = procinfo.cpu_seconds(os.getpid())
    _burn(BURN_S)
    spent = procinfo.cpu_seconds(os.getpid()) - before
    assert BURN_S - CLOCK_TOLERANCE_S <= spent <= BURN_S + CLOCK_TOLERANCE_S, spent


def test_cpu_seconds_without_a_procfs_is_a_number_not_none(no_procfs):
    """The file-manager rows asked a /proc file by hand and got None on
    macOS; ps(1) answers there."""
    spent = procinfo.cpu_seconds(os.getpid())
    assert spent is not None and spent >= 0


def test_cpu_seconds_of_a_process_that_is_gone_is_none():
    child = _sleeper()
    _end(child)
    assert procinfo.cpu_seconds(child.pid) is None


# ------------------------------------------------------------- signal state

def _state_of_a_program_started_as(prog, ignoring=(), blocking=()):
    """What the sigstate program prints when it is started with @ignoring
    ignored and @blocking blocked, as the editor would hand them on."""
    def prepare():
        for number in ignoring:
            signal.signal(number, signal.SIG_IGN)
        signal.pthread_sigmask(signal.SIG_BLOCK, blocking)
    return subprocess.run([prog], preexec_fn=prepare, capture_output=True,
                          text=True, check=True).stdout


def test_sigstate_names_the_signals_it_was_started_ignoring_and_blocking(tmp_path):
    prog = procinfo.sigstate_program(tmp_path)
    out = _state_of_a_program_started_as(
        prog, ignoring=(signal.SIGINT, signal.SIGXFSZ), blocking=(signal.SIGCHLD,))
    ignored = procinfo.signal_names(out, procinfo.SIGSTATE_IGNORED)
    blocked = procinfo.signal_names(out, procinfo.SIGSTATE_BLOCKED)
    assert {"INT", "XFSZ"} <= set(ignored) and "PIPE" not in ignored, out
    assert blocked == ["CHLD"], out


def test_sigstate_of_a_program_started_plain_names_nothing(tmp_path):
    out = _state_of_a_program_started_as(procinfo.sigstate_program(tmp_path))
    assert procinfo.signal_names(out, procinfo.SIGSTATE_IGNORED) == [], out
    assert procinfo.signal_names(out, procinfo.SIGSTATE_BLOCKED) == [], out


def test_a_state_with_no_line_for_the_set_asked_is_an_error_of_the_row():
    """The old helper asserted too, but on a /proc line that only Linux has."""
    with pytest.raises(AssertionError, match="no BLOCKED line"):
        procinfo.signal_names("IGNORED: INT\n", procinfo.SIGSTATE_BLOCKED)


def test_sigstate_answers_without_a_procfs(tmp_path, no_procfs):
    """The state of a program the row supplies comes from the program, so it
    needs no /proc; the state of a running process says it cannot."""
    out = _state_of_a_program_started_as(procinfo.sigstate_program(tmp_path),
                                         ignoring=(signal.SIGINT,))
    assert "INT" in procinfo.signal_names(out, procinfo.SIGSTATE_IGNORED)
    assert procinfo.signal_state_of(os.getpid()) is None


def test_the_state_of_a_running_process_agrees_with_sigstate(tmp_path):
    prog = procinfo.sigstate_program(tmp_path)

    def prepare():
        signal.signal(signal.SIGUSR1, signal.SIG_IGN)
        signal.pthread_sigmask(signal.SIG_BLOCK, (signal.SIGUSR2,))
    child = subprocess.Popen(["sleep", str(SLEEP_S)], preexec_fn=prepare)
    try:
        running = procinfo.signal_state_of(child.pid)
    finally:
        _end(child)
    if running is None:
        pytest.skip("needs the signal state of a running process: /proc")
    assert "USR1" in procinfo.signal_names(running, procinfo.SIGSTATE_IGNORED)
    assert "USR2" in procinfo.signal_names(running, procinfo.SIGSTATE_BLOCKED)
    plain = _state_of_a_program_started_as(prog, ignoring=(signal.SIGUSR1,),
                                           blocking=(signal.SIGUSR2,))
    assert "USR1" in procinfo.signal_names(plain, procinfo.SIGSTATE_IGNORED)


# ---------------------------------------------------------------- is_pty_path

@pytest.mark.parametrize("name", ["/dev/pts/3", "/dev/ttys004", "/dev/ttyp0",
                                  "/dev/ttyq1", "/dev/ttyT7"])
def test_is_pty_path_knows_the_names_of_every_system(name):
    assert procinfo.is_pty_path(name)


@pytest.mark.parametrize("name", ["/tmp/x", "/dev/null", "/dev/tty", "/dev/ttyS0",
                                  "/dev/pts", "pts/3"])
def test_is_pty_path_does_not_take_other_names(name):
    assert not procinfo.is_pty_path(name)


def test_is_pty_path_takes_the_name_this_system_gave_a_real_pty():
    master, slave = pty.openpty()
    try:
        assert procinfo.is_pty_path(os.ttyname(slave))
    finally:
        os.close(master)
        os.close(slave)


# ------------------------------------------------------- self_memory_file

def test_self_memory_file_is_a_file_whose_read_fails():
    path = procinfo.self_memory_file()
    if path is None:
        pytest.skip("this system has no /proc/self/mem")
    with open(path, "rb") as fh:
        with pytest.raises(OSError):
            fh.read(1)


def test_self_memory_file_is_none_without_a_procfs(no_procfs):
    assert procinfo.self_memory_file() is None
