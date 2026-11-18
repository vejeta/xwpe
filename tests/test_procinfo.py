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
import shutil
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


# ------------------------------------------------------ program identity

CHILD_ARGV_WAIT_S = 5
GRANDCHILD_WAIT_S = 10
GRANDCHILD_POLL_S = 0.05
FAKE_PID = 4242
BINARY_MAGIC = b"\x7fELF"


def _write_script(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    path.chmod(0o755)
    return path


def _write_binary(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(BINARY_MAGIC)
    path.chmod(0o755)
    return path


def _fake_table(monkeypatch, args, comm, state="S"):
    """Make the process table answer as ps(1) does on a system with no /proc:
    one process, FAKE_PID, with these words and command column, and no lsof."""
    fields = {"state": state, "args": args, "comm": comm}
    monkeypatch.setattr(procinfo, "has_procfs", lambda: False)
    monkeypatch.setattr(
        procinfo, "_ps_field",
        lambda pid, field, *options: fields[field] if pid == FAKE_PID else None)
    monkeypatch.setattr(procinfo, "_tool_output", lambda argv: None)
    return FAKE_PID


def _until(read, done):
    """read() until done(value) holds or CHILD_ARGV_WAIT_S passes; the last
    value.  A child that has just been exec'd shows an empty command line for
    an instant, so a row that needs the line waits for it."""
    deadline = time.time() + CHILD_ARGV_WAIT_S
    value = read()
    while not done(value) and time.time() < deadline:
        time.sleep(GRANDCHILD_POLL_S)
        value = read()
    return value


def test_a_script_is_the_program_whatever_interpreter_runs_it(tmp_path):
    """An env shebang leaves python3 as argv[0] and the kernel's command name
    python3, so no name tells the stub from any other python3."""
    stub = _write_script(tmp_path / "bin" / "sdb",
                         "#!/usr/bin/env python3\nimport time\ntime.sleep(60)\n")
    child = subprocess.Popen([str(stub)])
    try:
        prog = procinfo.program("sdb", path=str(tmp_path / "bin"))
        found = _until(lambda: procinfo.children_running(os.getpid(), prog),
                       bool)
    finally:
        _end(child)
    assert prog.kind == procinfo.SCRIPT, prog.describe()
    assert found == [child.pid], prog.describe()


def test_another_script_is_not_the_program(tmp_path):
    stub = _write_script(tmp_path / "bin" / "sdb",
                         "#!/usr/bin/env python3\nimport time\ntime.sleep(60)\n")
    other = _write_script(tmp_path / "other" / "sdb",
                          "#!/usr/bin/env python3\n")
    child = subprocess.Popen([str(stub)])
    try:
        prog = procinfo.program("sdb", path=str(other.parent))
        found = procinfo.children_running(os.getpid(), prog)
    finally:
        _end(child)
    assert found == [], prog.describe()


def test_the_legacy_stub_under_the_framework_python_is_its_backend(
        tmp_path, monkeypatch):
    """Recorded on macOS: a "#!" stub run by the framework Python shows the
    Python.app as argv[0] and as the command column, the stub as argv[1]."""
    stub = _write_script(tmp_path / "work" / "bin" / "sdb",
                         "#!/tmp/ptenv/bin/python\n")
    app = _write_binary(tmp_path / "opt" / "Python.framework" / "Versions"
                        / "3.15" / "Resources" / "Python.app" / "Contents"
                        / "MacOS" / "Python")
    pid = _fake_table(monkeypatch, "%s %s" % (app, stub), str(app))
    prog = procinfo.program("sdb", path=str(stub.parent))
    facts = procinfo.child_facts(pid)
    assert prog.runs(facts), (prog.describe(), prog.verdict(facts))
    other = procinfo.program("dbx", path=str(stub.parent.parent))
    assert other.kind == procinfo.MISSING and not other.runs(facts)


def _framework_tree(tmp_path):
    """A fake Homebrew layout: python3 -> python3.15 in the framework's bin,
    and the Python.app beside it that the launcher re-executes."""
    version = (tmp_path / "Cellar" / "Frameworks" / "Python.framework"
               / "Versions" / "3.15")
    real = _write_binary(version / "bin" / "python3.15")
    (version / "bin" / "python3").symlink_to("python3.15")
    app = _write_binary(version / "Resources" / "Python.app" / "Contents"
                        / "MacOS" / "Python")
    return version / "bin", real, app


def test_pdb_under_the_framework_launcher_is_python3(tmp_path, monkeypatch):
    """Recorded on macOS: python3 runs as the Python.app, whose path is the
    command column, not as the python3.15 the PATH names."""
    bindir, real, app = _framework_tree(tmp_path)
    pid = _fake_table(monkeypatch, "%s -m pdb t.py" % app, str(app))
    prog = procinfo.program("python3", path=str(bindir))
    facts = procinfo.child_facts(pid)
    assert os.path.realpath(real) in prog.executables
    assert prog.runs(facts), (prog.describe(), prog.verdict(facts))


def test_a_binary_is_known_by_its_executable_not_its_comm(tmp_path):
    child = _sleeper()
    try:
        prog = procinfo.program("sleep")
        facts = procinfo.child_facts(child.pid)
        runs = prog.runs(facts)
        # the same name, another file: not the program the editor would run
        copy = tmp_path / "other" / "sleep"
        copy.parent.mkdir()
        # bytes and execute mode only: copy2 also copies file flags, which a
        # user cannot set on a copy of a SIP-protected system binary (macOS)
        original = shutil.which("sleep")
        shutil.copyfile(original, copy)
        os.chmod(copy, os.stat(original).st_mode & 0o755)
        impostor = procinfo.program("sleep", path=str(copy.parent))
        assert impostor.kind == procinfo.EXECUTABLE, impostor.describe()
        impostor_runs = impostor.runs(facts)
    finally:
        _end(child)
    assert facts.exe == os.path.realpath(shutil.which("sleep"))
    assert runs, prog.verdict(facts)
    assert not impostor_runs, impostor.describe()


def test_without_an_exe_a_bare_argv0_is_looked_up_on_the_editors_path(
        tmp_path, monkeypatch):
    """The BSDs print a base name as the command column and ship no lsof; the
    editor's execvp left argv[0] as the bare word, so it is looked up on the
    PATH the editor was given."""
    tool = _write_binary(tmp_path / "bin" / "gdb")
    pid = _fake_table(monkeypatch, "gdb --interpreter=mi", "gdb")
    prog = procinfo.program("gdb", path=str(tool.parent))
    facts = procinfo.child_facts(pid)
    assert facts.exe is None
    assert prog.runs(facts), prog.verdict(facts)
    _write_binary(tmp_path / "bin" / "lldb")
    other = procinfo.program("lldb", path=str(tool.parent))
    assert not other.runs(facts), other.describe()


def test_an_exe_found_by_lsof_is_resolved_to_its_real_path(tmp_path, monkeypatch):
    real = _write_binary(tmp_path / "real" / "gdb")
    link = tmp_path / "bin" / "gdb"
    link.parent.mkdir()
    link.symlink_to(real)
    pid = _fake_table(monkeypatch, "gdb", "gdb")
    monkeypatch.setattr(procinfo, "_tool_output",
                        lambda argv: "p%d\nftxt\nn%s\n" % (FAKE_PID, link))
    facts = procinfo.child_facts(pid)
    assert facts.exe == os.path.realpath(real)
    assert procinfo.program("gdb", path=str(link.parent)).runs(facts)


def test_the_command_column_is_resolved_to_its_real_path(tmp_path, monkeypatch):
    """macOS prints the executable as the command column; a Homebrew link
    in it points into the Cellar the PATH's own lookup also ends in."""
    real = _write_binary(tmp_path / "cellar" / "gdb")
    link = tmp_path / "bin" / "gdb"
    link.parent.mkdir()
    link.symlink_to(real)
    pid = _fake_table(monkeypatch, "%s --interpreter=mi" % link, str(link))
    prog = procinfo.program("gdb", path=str(link.parent))
    facts = procinfo.child_facts(pid)
    assert facts.exe == os.path.realpath(real)
    assert prog.runs(facts), prog.verdict(facts)


def test_a_relative_argv0_is_never_looked_up_against_this_processes_folder(
        tmp_path, monkeypatch):
    """The child runs ./bin/gdb from its own folder B and the system names no
    executable and no folder; the asked PATH is A/bin, where this process
    stands in A.  The answer is no, not a match against A/bin/gdb."""
    here = tmp_path / "A"
    _write_binary(here / "bin" / "gdb")
    monkeypatch.chdir(here)
    pid = _fake_table(monkeypatch, "./bin/gdb --interpreter=mi", "gdb")
    prog = procinfo.program("gdb", path=str(here / "bin"))
    facts = procinfo.child_facts(pid)
    assert facts.exe is None and facts.argv0_file is None
    assert not prog.runs(facts), prog.verdict(facts)


def test_children_running_leaves_out_a_grandchild_that_runs_the_program():
    """The shell is the child; the sleep it started is below it."""
    child = subprocess.Popen(["sh", "-c", "sleep %d; :" % SLEEP_S],
                             start_new_session=True)
    try:
        prog = procinfo.program("sleep")
        deadline = time.time() + GRANDCHILD_WAIT_S
        below = procinfo.descendants_running(os.getpid(), prog)
        while not below and time.time() < deadline:
            time.sleep(GRANDCHILD_POLL_S)
            below = procinfo.descendants_running(os.getpid(), prog)
        direct = procinfo.children_running(os.getpid(), prog)
    finally:
        os.killpg(child.pid, signal.SIGKILL)
        child.wait()
    assert below, "the shell started no sleep"
    assert child.pid not in below
    assert direct == [], direct


def test_an_ended_child_is_not_the_program(tmp_path, monkeypatch):
    app = _write_binary(tmp_path / "bin" / "gdb")
    pid = _fake_table(monkeypatch, "%s" % app, str(app), state=procinfo.ZOMBIE)
    prog = procinfo.program("gdb", path=str(app.parent))
    assert prog.verdict(procinfo.child_facts(pid)) == procinfo.VERDICT_ENDED
    assert prog.verdict(None) == procinfo.VERDICT_ENDED


def test_a_program_not_on_the_editors_path_matches_nothing_and_says_so(tmp_path):
    child = _sleeper()
    try:
        prog = procinfo.program("nosuch", path=str(tmp_path))
        facts = procinfo.child_facts(child.pid)
        verdict = prog.verdict(facts)
        found = procinfo.children_running(os.getpid(), prog)
    finally:
        _end(child)
    assert prog.kind == procinfo.MISSING
    assert found == [] and verdict == procinfo.VERDICT_NOT_ON_PATH
    assert "not on the editor's PATH" in prog.describe()


@pytest.mark.parametrize("path", ["bin", "", "/usr/bin" + os.pathsep + "bin"])
def test_a_relative_path_entry_is_refused(path):
    with pytest.raises(ValueError, match="not absolute"):
        procinfo.program("sleep", path=path)


def test_a_relative_argument_is_resolved_against_the_childs_folder(tmp_path):
    """The child runs ./tool from its own folder; this process, in another
    folder, must still find the file that word names."""
    work = tmp_path / "work"
    tool = _write_script(work / "tool", "#!/bin/sh\nread line\n")
    assert os.path.realpath(os.getcwd()) != os.path.realpath(work)
    child = subprocess.Popen(["./tool"], cwd=str(work), stdin=subprocess.PIPE)
    try:
        prog = procinfo.program("tool", path=str(work))
        facts = _until(lambda: procinfo.child_facts(child.pid),
                       lambda f: f and f.argv_files)
        runs = prog.runs(facts)
    finally:
        child.stdin.close()
        _end(child)
    assert os.path.realpath(tool) in facts.argv_files, facts.argv_files
    assert runs, prog.verdict(facts)


# ------------------------------------------------- a session asks by program

class _Asker(procinfo.ChildAsker):
    """The part of a session a row uses to ask: the editor's Popen and the
    PATH it was started with."""

    def __init__(self, proc, exec_path):
        self.proc = proc
        self.exec_path = exec_path


def test_a_session_finds_an_env_shebang_stub_by_the_word_it_was_started_as(
        tmp_path):
    """The editor's execvp of "sdb" ends in a script whose process is named
    python3 by the kernel; the session asks for "sdb" and gets that child, and
    remembers which file it looked for."""
    stub = _write_script(tmp_path / "bin" / "sdb",
                         "#!/usr/bin/env python3\nimport time\ntime.sleep(60)\n")
    child = subprocess.Popen([str(stub)])
    try:
        asker = _Asker(_Me(), str(stub.parent))
        found = _until(lambda: asker.running("sdb"), bool)
    finally:
        _end(child)
    assert found == [child.pid], asker.asked.describe()
    assert asker.asked.describe() == "sdb -> script %s" % os.path.realpath(stub)


def test_a_session_whose_row_removed_the_path_asks_on_the_default_path():
    """A removed PATH means execvp's own default path, not this process's."""
    asker = _Asker(_Me(), None)
    asker.running("sleep")
    expected = os.confstr("CS_PATH") if hasattr(os, "confstr") else None
    assert asker.asked.path == (expected or "/usr/bin:/bin")

def test_a_session_asking_for_a_program_not_on_its_path_says_so(tmp_path):
    asker = _Asker(_Me(), str(tmp_path))
    assert asker.running("sdb") == []
    assert "not on the editor's PATH" in asker.asked.describe()


def test_running_below_the_editor_leaves_out_what_is_not_below_it(tmp_path):
    child = subprocess.Popen(["sh", "-c", "sleep %d; :" % SLEEP_S],
                             start_new_session=True)
    try:
        asker = _Asker(_Me(), os.environ["PATH"])
        below = _until(lambda: asker.running_below("sleep"), bool)
        direct = asker.running("sleep")
    finally:
        os.killpg(child.pid, signal.SIGKILL)
        child.wait()
    assert below and child.pid not in below
    assert direct == []


class _Me:
    """The pid of this process, as a session's `proc` gives the editor's."""
    pid = os.getpid()


def test_a_cwd_keeps_only_the_processes_working_in_that_folder(tmp_path):
    here = tmp_path / "here"
    there = tmp_path / "there"
    here.mkdir()
    there.mkdir()
    near = subprocess.Popen(["sleep", str(SLEEP_S)], cwd=str(here))
    far = subprocess.Popen(["sleep", str(SLEEP_S)], cwd=str(there))
    try:
        prog = procinfo.program("sleep")
        both = _until(lambda: procinfo.children_running(os.getpid(), prog),
                      lambda pids: {near.pid, far.pid} <= set(pids))
        in_here = procinfo.children_running(os.getpid(), prog, str(here))
        anywhere = procinfo.processes_running(prog, str(there))
    finally:
        _end(near)
        _end(far)
    assert {near.pid, far.pid} <= set(both)
    assert in_here == [near.pid]
    assert anywhere == [far.pid]
