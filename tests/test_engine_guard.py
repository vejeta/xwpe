"""The tool farm of the owner: a row never reaches a program of the machine.

Every row here builds its own "machine": a folder in the row's scratch whose
PATH holds a fake AI engine (a script that writes a marker file when it
runs), so the rows mean the same on a machine with a real `claude` and on a
runner with none.  The real engines are never on the PATH of these rows
(it is that folder plus /usr/bin:/bin) and nothing here starts one.
"""
import os
import pwd
import shlex
import sys

import pytest

import engine_guard
import gui_owner

SYSTEM_PATH = "/usr/bin:/bin"
FAKE_ENGINE = "newengine"
FAKE_NAMED_ENGINE = "claude"
LIVE_FILE = "/work/tests/test_something_live.py"
PLAIN_FILE = "/work/tests/test_something.py"
ROW = "tests/test_something.py::test_row"
RUN_S = 10.0


def _write_script(path, text):
    """Write an executable script (the row's own stand-in)."""
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("#!/bin/sh\n" + text)
    os.chmod(path, 0o755)


class Machine:
    """A fake machine in a row's scratch: a folder of fake engines, the
    PATH of that machine, and the owner built on it."""

    def __init__(self, tmp_path):
        self.tmp = tmp_path
        self.folder = tmp_path / "machine"
        self.folder.mkdir()
        self.work = tmp_path / "work"
        self.work.mkdir()
        self.home = tmp_path / "home"
        self.home.mkdir()
        self.path = "%s:%s" % (self.folder, SYSTEM_PATH)
        for name in (FAKE_ENGINE, FAKE_NAMED_ENGINE):
            _write_script(str(self.folder / name),
                          ": > %s\n" % self.marker(name))
        tools = engine_guard.MachineTools.from_path(self.path)
        self.owner = gui_owner.Owner(str(tmp_path / "out"), machine=tools)
        self.owner.begin()

    def marker(self, name):
        """The file the fake engine `name` creates when it runs."""
        return str(self.tmp / ("ran-" + name))

    def ran(self, name):
        """True when the fake engine `name` really ran."""
        return os.path.exists(self.marker(name))

    def env(self, **more):
        """The environment of a row: the fake machine's PATH."""
        env = {"PATH": self.path, "HOME": str(self.home)}
        env.update(more)
        return env

    def run(self, command, env=None, cwd=None):
        """Run `/bin/sh -c command` through the owner, as a row would, and
        wait for it; returns the exit code."""
        owned = self.owner.launch(["/bin/sh", "-c", command],
                                  env if env is not None else self.env(),
                                  cwd or str(self.work), str(self.tmp), "tool",
                                  "guard row")
        return owned.proc.wait(timeout=RUN_S)

    def row(self, path=PLAIN_FILE):
        """Begin a row of the file at `path`; returns what end_since needs."""
        before = self.owner.ledger.pids()
        self.owner.begin_row(ROW, path)
        return before

    def finish_row(self, before):
        """End the row as the row-end fixture does (it raises on a hit)."""
        self.owner.end_since(before)

    def close(self):
        """Finish the owner; it must have nothing left."""
        late = self.owner.finish()
        assert not late


@pytest.fixture
def machine(tmp_path):
    """A fake machine and an owner on it, finished with the row."""
    fake = Machine(tmp_path)
    yield fake
    fake.close()


# @scenario SC-test-real-tools-01
def test_a_program_only_on_the_machine_path_fails_the_row_by_name(machine):
    """The canary: a program of the machine's PATH that the baseline does not
    list is not run; it hits a stub, the stub logs, the row fails naming it,
    and the fake's marker never appears.  Skipping the farm's PATH makes the
    marker appear and this row fail."""
    before = machine.row()
    code = machine.run(FAKE_ENGINE)
    assert code == engine_guard.STUB_EXIT_CODE
    with pytest.raises(gui_owner.EngineReached) as hit:
        machine.finish_row(before)
    assert FAKE_ENGINE in str(hit.value) and ROW in str(hit.value)
    assert not machine.ran(FAKE_ENGINE)


# @scenario SC-test-real-tools-02
def test_a_machine_program_named_like_a_known_engine_fails_the_row(machine):
    """The same with a name every list has: claude on the machine's PATH."""
    before = machine.row()
    machine.run(FAKE_NAMED_ENGINE)
    with pytest.raises(gui_owner.EngineReached) as hit:
        machine.finish_row(before)
    assert FAKE_NAMED_ENGINE in str(hit.value)
    assert not machine.ran(FAKE_NAMED_ENGINE)


# @scenario SC-test-real-tools-03
def test_a_row_stand_in_first_on_path_still_runs(machine):
    """The row's own folder comes first: its `claude` runs and the row ends
    clean."""
    own = machine.tmp / "bin"
    own.mkdir()
    _write_script(str(own / FAKE_NAMED_ENGINE),
                  ": > %s\n" % (machine.tmp / "own-ran"))
    before = machine.row()
    env = machine.env(PATH="%s:%s" % (own, machine.path))
    assert machine.run(FAKE_NAMED_ENGINE, env) == 0
    machine.finish_row(before)
    assert (machine.tmp / "own-ran").exists()
    assert not machine.ran(FAKE_NAMED_ENGINE)


# @scenario SC-test-real-tools-04
def test_baseline_tools_are_found_and_the_machine_folder_is_not_on_path(machine):
    """A baseline tool (ls) runs; the machine's own folder is not on the PATH
    the process sees."""
    before = machine.row()
    log = machine.tmp / "path.txt"
    assert machine.run("ls / > /dev/null && echo \"$PATH\" > %s" % log) == 0
    machine.finish_row(before)
    seen = log.read_text()
    assert str(machine.folder) not in seen
    assert "/usr/bin" not in seen.split(":")


# @scenario SC-test-real-tools-05
def test_a_declared_tool_is_added_and_runs(machine):
    """A row names a machine tool it needs: only that one symlink is added."""
    before = machine.row()
    env = machine.env(**{engine_guard.TOOLS_VAR: FAKE_ENGINE})
    assert machine.run(FAKE_ENGINE, env) == 0
    machine.finish_row(before)
    assert machine.ran(FAKE_ENGINE)
    assert not machine.ran(FAKE_NAMED_ENGINE)


# @scenario SC-test-real-tools-06
@pytest.mark.parametrize("path", [None, ""])
def test_a_row_without_path_is_refused(machine, path):
    """No PATH, or an empty one, sends the editor to built-in folders."""
    env = machine.env()
    if path is None:
        del env["PATH"]
    else:
        env["PATH"] = path
    with pytest.raises(gui_owner.OwnerRefused, match="PATH"):
        machine.run("true", env)


# @scenario SC-test-real-tools-07
def test_a_working_folder_that_holds_the_farm_is_refused(machine):
    """The product drops folders inside the workspace from an engine's PATH:
    a farm there would be dropped with them."""
    with pytest.raises(gui_owner.OwnerRefused, match="inside the working"):
        machine.run("true", cwd="/")


# @scenario SC-test-real-tools-08
def test_the_opt_out_is_refused_in_a_file_that_is_not_live(machine):
    """A non-live file setting the opt-out is refused, and no machine
    program runs."""
    env = machine.env(**{engine_guard.REAL_ENGINE_VAR: "1"})
    machine.owner.begin_row(ROW, PLAIN_FILE)
    with pytest.raises(gui_owner.OwnerRefused, match="live file"):
        machine.run(FAKE_ENGINE, env)
    assert not machine.ran(FAKE_ENGINE)


# @scenario SC-test-real-tools-09
def test_the_opt_out_of_a_live_file_keeps_the_machine_path(machine, monkeypatch):
    """A live row, with the gate open in the test run's environment, keeps the
    machine's PATH, and the variable is stripped before the process starts."""
    monkeypatch.setenv(engine_guard.LIVE_GATE_VAR, engine_guard.LIVE_GATE_ON)
    before = machine.row(LIVE_FILE)
    env = machine.env(**{engine_guard.REAL_ENGINE_VAR: "1"})
    log = machine.tmp / "seen.txt"
    machine.run("%s; env > %s" % (FAKE_ENGINE, log), env)
    machine.finish_row(before)
    assert machine.ran(FAKE_ENGINE)
    assert engine_guard.REAL_ENGINE_VAR not in log.read_text()


# @scenario SC-test-real-tools-10
def test_the_opt_out_cannot_be_inherited_from_the_machine(machine):
    """The variable is never inherited by name: a developer's exported value
    cannot turn the guard off."""
    with pytest.raises(gui_owner.OwnerRefused, match="never inherited"):
        gui_owner._inherit(machine.owner, {}, (engine_guard.REAL_ENGINE_VAR,),
                           str(machine.tmp))


# @scenario SC-test-real-tools-11
def test_an_api_key_is_inherited_only_by_a_live_file(machine, monkeypatch):
    """The same live-only rule as the opt-out, over HTTP."""
    monkeypatch.setenv("ANTHROPIC_API_KEY", "not-a-real-key")
    monkeypatch.setenv(engine_guard.LIVE_GATE_VAR, engine_guard.LIVE_GATE_ON)
    machine.owner.begin_row(ROW, PLAIN_FILE)
    with pytest.raises(gui_owner.OwnerRefused, match="live file"):
        gui_owner._inherit(machine.owner, {}, ("ANTHROPIC_API_KEY",),
                           str(machine.tmp))
    monkeypatch.delenv(engine_guard.LIVE_GATE_VAR)
    machine.owner.begin_row(ROW, LIVE_FILE)
    with pytest.raises(gui_owner.OwnerRefused, match="XWPE_TEST_LIVE"):
        gui_owner._inherit(machine.owner, {}, ("ANTHROPIC_API_KEY",),
                           str(machine.tmp))
    monkeypatch.setenv(engine_guard.LIVE_GATE_VAR, engine_guard.LIVE_GATE_ON)
    env = {}
    gui_owner._inherit(machine.owner, env, ("ANTHROPIC_API_KEY",),
                       str(machine.tmp))
    assert env["ANTHROPIC_API_KEY"] == "not-a-real-key"


# @scenario SC-test-real-tools-12
def test_a_host_command_by_an_absolute_path_outside_the_scratch_is_refused(machine):
    """An absolute program in XWPE_AI_HOST_CMD does not go through PATH: it
    must be inside the row's scratch."""
    outside = str(machine.folder / FAKE_ENGINE)
    env = machine.env(**{engine_guard.HOST_CMD_VAR: "%s -p" % shlex.quote(outside)})
    with pytest.raises(gui_owner.OwnerRefused, match=engine_guard.HOST_CMD_VAR):
        machine.run("true", env)
    inside = machine.work / "stub.sh"
    _write_script(str(inside), "true\n")
    env = machine.env(**{engine_guard.HOST_CMD_VAR: str(inside)})
    before = machine.row()
    assert machine.run("true", env) == 0
    machine.finish_row(before)


# @scenario SC-test-real-tools-13
def test_a_hit_between_rows_fails_the_session(machine):
    """A stub run when no row was charged with it is kept for the session
    fixture, which fails on it."""
    before = machine.row()
    machine.finish_row(before)
    machine.run(FAKE_ENGINE)
    machine.owner.finish()
    assert any(FAKE_ENGINE in line for line in machine.owner.stray_engine_hits)


# @scenario SC-test-real-tools-14
def test_folders_under_the_real_home_are_dropped_from_the_path(machine, monkeypatch):
    """~/.local/bin, ~/.cargo/bin and the like are never on the row's PATH,
    but a folder in the row's scratch is, first."""
    fake_home = machine.tmp / "fakehome"
    (fake_home / ".local" / "bin").mkdir(parents=True)
    monkeypatch.setenv("HOME", str(fake_home))
    own = machine.tmp / "own"
    own.mkdir()
    farm = machine.owner._farm_made()
    path = engine_guard.farm_path(
        os.pathsep.join([str(own), str(fake_home / ".local" / "bin"),
                         str(machine.folder)]),
        machine.owner._machine, farm, (), [str(machine.work)])
    entries = path.split(os.pathsep)
    assert entries[0] == str(own)
    assert str(fake_home / ".local" / "bin") not in entries
    assert str(machine.folder) not in entries
    assert entries[-1] == farm.deny_folder()


# @scenario SC-test-real-tools-15
def test_a_stand_in_of_the_checkout_survives_a_home_above_it(machine, monkeypatch):
    """The tree's own stand-in folder stays first on the row's PATH when the
    checkout lies under the real HOME, which is where a CI runner puts its
    workspace: a checked-in stand-in belongs to the row, not to the machine,
    and dropping it hands the row the real debugger the stand-in stands in
    front of."""
    home = machine.tmp / "realhome"
    tree = home / "work" / "xwpe"
    stubs = tree / "tests" / "stubs"
    stubs.mkdir(parents=True)
    (home / ".local" / "bin").mkdir(parents=True)
    monkeypatch.setenv("HOME", str(home))
    monkeypatch.setattr(engine_guard, "SUITE_ROOT", str(tree))
    farm = machine.owner._farm_made()
    path = engine_guard.farm_path(
        os.pathsep.join([str(stubs), str(home / ".local" / "bin"),
                         str(machine.folder)]),
        machine.owner._machine, farm, (), [str(machine.work)])
    entries = path.split(os.pathsep)
    assert entries[0] == str(stubs), \
        "the checkout's own stand-ins are off the row's PATH: %s" % entries
    assert str(home / ".local" / "bin") not in entries
    assert str(machine.folder) not in entries
    assert entries[-1] == farm.deny_folder()


# ---------------------------------------------------------------------------
# The live gate

PLANTED_LIVE = "test_planted_live_x.py"
PLANTED_MARK = "PLANTED-LIVE-MODULE-IMPORTED"
PYTEST_ARGS = ["-q", "-p", "no:cacheprovider", "-rs"]
NESTED_S = 60
NO_TESTS_COLLECTED = 5      # pytest's exit status when a module skipped itself


def _planted_folder(tmp_path):
    """A folder with a live-named file that fails when imported, and a
    conftest that installs the gate's hook: what a stray run of a live file
    looks like.  Returns the folder."""
    folder = tmp_path / "planted"
    folder.mkdir()
    (folder / PLANTED_LIVE).write_text(
        "raise RuntimeError(%r)\n\ndef test_x():\n    pass\n" % PLANTED_MARK)
    (folder / "conftest.py").write_text(
        "import sys\nsys.path.insert(0, %r)\n"
        "from live_gate import pytest_pycollect_makemodule  # noqa\n"
        % os.path.dirname(os.path.abspath(__file__)))
    return folder


def _nested_pytest(folder, targets, extra_env=None):
    """Run pytest on @targets (relative to @folder) as a nested run of the
    harness, through the owner."""
    argv = [sys.executable, "-m", "pytest"] + PYTEST_ARGS + list(targets)
    return gui_owner.run_nested(argv, NESTED_S, cwd=str(folder),
                                extra_env=extra_env)


# @scenario SC-test-real-tools-15
@pytest.mark.parametrize("targets", [["."], [PLANTED_LIVE]],
                         ids=["by-directory", "by-explicit-path"])
def test_a_live_file_is_skipped_and_never_imported_while_the_gate_is_closed(
        tmp_path, targets):
    """With the gate unset a live file is a stand-in that skips; the module
    is never imported, whether the run names the folder or the file."""
    folder = _planted_folder(tmp_path)
    run = _nested_pytest(folder, targets)
    out = run.stdout.decode() + run.stderr.decode()
    assert run.returncode == 0, out
    assert "1 skipped" in out and engine_guard.LIVE_GATE_VAR in out, out
    assert PLANTED_MARK not in out, out


# @scenario SC-test-real-tools-16
def test_a_live_file_is_imported_when_the_run_opens_the_gate(tmp_path):
    """The other half: with the gate open the module is imported (and the
    planted file fails), so it is the gate that skipped it above."""
    folder = _planted_folder(tmp_path)
    run = _nested_pytest(folder, ["."], {engine_guard.LIVE_GATE_VAR:
                                         engine_guard.LIVE_GATE_ON})
    out = run.stdout.decode() + run.stderr.decode()
    assert run.returncode != 0 and PLANTED_MARK in out, out


GATED_LIVE = "test_planted_gated_live.py"


def _planted_gated_file(folder):
    """A live-named file that opens as the real ones do (the shared call after
    its imports) and fails when it goes on."""
    (folder / GATED_LIVE).write_text(
        "import sys\nsys.path.insert(0, %r)\n"
        "from live_gate import require_live_gate\n"
        "require_live_gate()\nraise RuntimeError(%r)\n"
        % (os.path.dirname(os.path.abspath(__file__)), PLANTED_MARK))


# @scenario SC-test-real-tools-29
@pytest.mark.parametrize("target", [".", GATED_LIVE],
                         ids=["by-directory", "by-explicit-path"])
def test_a_gated_live_file_run_with_no_conftest_shows_no_marker(tmp_path, target):
    """The second gate: even with --noconftest (the hook is gone) a live file
    that opens with require_live_gate() skips itself at import."""
    folder = tmp_path / "gated-only"
    folder.mkdir()
    _planted_gated_file(folder)
    run = _nested_pytest(folder, ["--noconftest", target])
    out = run.stdout.decode() + run.stderr.decode()
    # A module skipped at import yields no item: pytest exits 5, "no tests
    # collected", and reports the skip.
    assert run.returncode == NO_TESTS_COLLECTED and "skipped" in out, out
    assert PLANTED_MARK not in out, out


# @scenario SC-test-real-tools-30
def test_a_live_file_without_the_second_gate_is_imported_by_no_conftest(tmp_path):
    """Why the check exists: a live file that lacks the call IS imported when
    the conftest is dropped, and reaches whatever its import reaches."""
    folder = _planted_folder(tmp_path)
    run = _nested_pytest(folder, ["--noconftest", PLANTED_LIVE])
    out = run.stdout.decode() + run.stderr.decode()
    assert PLANTED_MARK in out, out


# @scenario SC-test-real-tools-31
def test_require_live_gate_skips_the_module_unless_the_gate_is_open(monkeypatch):
    """The open-gate path, in process: the shared call raises a module-level
    skip while the gate is closed and returns when the run opened it."""
    import live_gate
    monkeypatch.delenv(engine_guard.LIVE_GATE_VAR, raising=False)
    with pytest.raises(pytest.skip.Exception):
        live_gate.require_live_gate()
    monkeypatch.setenv(engine_guard.LIVE_GATE_VAR, engine_guard.LIVE_GATE_ON)
    assert live_gate.require_live_gate() is None


# @scenario SC-test-real-tools-17
def test_the_opt_out_is_refused_in_a_live_file_while_the_gate_is_closed(
        machine, monkeypatch):
    """The gate belongs to the person running the suite: a live file's row
    that sets the opt-out is refused when the run did not open it."""
    monkeypatch.delenv(engine_guard.LIVE_GATE_VAR, raising=False)
    machine.owner.begin_row(ROW, LIVE_FILE)
    env = machine.env(**{engine_guard.REAL_ENGINE_VAR: "1"})
    with pytest.raises(gui_owner.OwnerRefused, match=engine_guard.LIVE_GATE_VAR):
        machine.run(FAKE_ENGINE, env)
    assert not machine.ran(FAKE_ENGINE)


# @scenario SC-test-real-tools-18
def test_the_opt_out_in_the_machines_environment_is_never_honoured(
        machine, monkeypatch):
    """A developer's exported XWPE_TEST_REAL_ENGINE, with the gate open and a
    live file, does not turn the guard off for a row whose own variables do
    not set it: the fake engine does not run and the row fails by name."""
    monkeypatch.setenv(engine_guard.LIVE_GATE_VAR, engine_guard.LIVE_GATE_ON)
    monkeypatch.setenv(engine_guard.REAL_ENGINE_VAR, "1")
    before = machine.row(LIVE_FILE)
    machine.run(FAKE_ENGINE)
    with pytest.raises(gui_owner.EngineReached) as hit:
        machine.finish_row(before)
    assert FAKE_ENGINE in str(hit.value)
    assert not machine.ran(FAKE_ENGINE)


# @scenario SC-test-real-tools-19
def test_real_login_is_refused_outside_a_live_row_with_the_gate_open(
        machine, tmp_path, monkeypatch):
    """real_login links the user's engine login into the row: only a live
    file's row may ask, and only with the gate open."""
    import wpe_driver
    account, scratch = _account_and_scratch_home(machine.tmp, monkeypatch)
    previous = gui_owner._CURRENT
    gui_owner.install(machine.owner)
    try:
        monkeypatch.setenv(engine_guard.LIVE_GATE_VAR, engine_guard.LIVE_GATE_ON)
        machine.owner.begin_row(ROW, PLAIN_FILE)
        with pytest.raises(gui_owner.OwnerRefused, match="real_login"):
            wpe_driver.WpeSession._refuse_account_login(str(scratch))
        monkeypatch.delenv(engine_guard.LIVE_GATE_VAR)
        machine.owner.begin_row(ROW, LIVE_FILE)
        with pytest.raises(gui_owner.OwnerRefused, match="real_login"):
            wpe_driver.WpeSession._refuse_account_login(str(scratch))
        monkeypatch.setenv(engine_guard.LIVE_GATE_VAR, engine_guard.LIVE_GATE_ON)
        wpe_driver.WpeSession._refuse_account_login(str(scratch))
    finally:
        gui_owner.install(previous)


def _account_and_scratch_home(tmp_path, monkeypatch):
    """A folder the password file calls the account's home, with a login in
    it, and a scratch HOME whose .claude is a link into that login (the way
    a row can reach the real one while HOME names a scratch folder)."""
    import types
    account = tmp_path / "account-home"
    (account / ".claude").mkdir(parents=True)
    scratch = tmp_path / "scratch-home"
    scratch.mkdir()
    os.symlink(str(account / ".claude"), str(scratch / ".claude"))
    monkeypatch.setattr(pwd, "getpwuid",
                        lambda uid: types.SimpleNamespace(pw_dir=str(account)))
    # The account here is in the row's scratch, which is inside the session's
    # basetemp: the basetemp exemption is not what these rows are about.
    monkeypatch.setattr(gui_owner, "_SESSION_BASETEMP", None)
    return account, scratch


# @scenario SC-test-real-tools-28
def test_a_login_is_judged_per_file_not_by_the_folder_home_names(
        machine, monkeypatch):
    """A scratch HOME with a link into the account's home is refused in a
    plain file; a scratch HOME with a login of its own is not."""
    import wpe_driver
    _account, scratch = _account_and_scratch_home(machine.tmp, monkeypatch)
    own_home = machine.tmp / "own-login-home"
    (own_home / ".claude").mkdir(parents=True)
    previous = gui_owner._CURRENT
    gui_owner.install(machine.owner)
    try:
        machine.owner.begin_row(ROW, PLAIN_FILE)
        with pytest.raises(gui_owner.OwnerRefused, match=r"\.claude"):
            wpe_driver.WpeSession._refuse_account_login(str(scratch))
        wpe_driver.WpeSession._refuse_account_login(str(own_home))
    finally:
        gui_owner.install(previous)


# ---------------------------------------------------------------------------
# A program named by its path: the doors the PATH does not cover


def _quoted(path):
    """@path as a shell word."""
    return shlex.quote(str(path))


# @scenario SC-test-real-tools-20
def test_a_machine_program_given_by_its_path_in_the_argument_vector_is_refused(machine):
    """Door: argv[0] is the file of a machine program the farm does not
    provide."""
    engine = machine.folder / FAKE_ENGINE
    with pytest.raises(gui_owner.OwnerRefused, match="does not provide"):
        machine.owner.launch([str(engine)], machine.env(), str(machine.work),
                             str(machine.tmp), "tool", "guard row")
    assert not machine.ran(FAKE_ENGINE)


# @scenario SC-test-real-tools-21
@pytest.mark.parametrize("name", ["XWPE_AI_HOST_CMD", "SOME_PLAIN_VARIABLE"])
def test_a_machine_program_in_a_word_of_an_environment_value_is_refused(machine, name):
    """Door: a value the program later runs, with the path after an `=` or in
    a list of folders, or as the first word of a command line."""
    engine = machine.folder / FAKE_ENGINE
    for value in ("%s -p" % _quoted(engine), "x=%s" % engine,
                  "/usr/bin:%s" % engine, "'%s'" % engine):
        env = machine.env(**{name: value})
        # The host command has its own, older refusal (absolute path outside
        # the scratch); any other variable is the generic one alone.
        with pytest.raises(gui_owner.OwnerRefused,
                           match="does not provide|outside the row's scratch"):
            machine.run("true", env)
    assert not machine.ran(FAKE_ENGINE)


# @scenario SC-test-real-tools-22
def test_a_machine_program_in_a_shell_string_of_the_arguments_is_refused(machine):
    """Door: `sh -c "<path> args"`, the path is a word of one argument."""
    engine = str(machine.folder / FAKE_ENGINE)
    forms = ("%s --go" % _quoted(engine), "true;%s" % engine,
             "$(%s)" % engine, "(%s)" % engine, "x|%s" % engine,
             "true&&%s" % engine, "`%s`" % engine, "{ %s; }" % engine,
             "cat<%s" % engine)
    for command in forms:
        with pytest.raises(gui_owner.OwnerRefused, match="does not provide"):
            machine.run(command)
    assert not machine.ran(FAKE_ENGINE)


# @scenario SC-test-real-tools-23
def test_a_link_to_a_machine_program_in_the_rows_own_path_folder_is_refused(machine):
    """Door: the row links the real binary into its own bin, so the PATH
    lookup finds it first.  The file is judged by its realpath."""
    own = machine.tmp / "ownbin"
    own.mkdir()
    os.symlink(str(machine.folder / FAKE_ENGINE), str(own / "tool-of-mine"))
    env = machine.env(PATH="%s:%s" % (own, machine.path))
    with pytest.raises(gui_owner.OwnerRefused, match="own PATH folder"):
        machine.run("tool-of-mine", env)
    assert not machine.ran(FAKE_ENGINE)


# @scenario SC-test-real-tools-24
def test_a_stand_in_and_a_baseline_path_are_not_refused(machine):
    """What the doors must let through: a stand-in script of the row, in its
    scratch, named by its path in an argument and in a value, and a baseline
    program named by its path."""
    stand_in = machine.tmp / "stand-in.sh"
    _write_script(str(stand_in), ": > %s\n" % (machine.tmp / "stand-in-ran"))
    before = machine.row()
    env = machine.env(SOME_VALUE="x:%s" % stand_in)
    assert machine.run("%s; /usr/bin/env true" % _quoted(stand_in), env) == 0
    machine.finish_row(before)
    assert (machine.tmp / "stand-in-ran").exists()


# ---------------------------------------------------------------------------
# The session, the farm's place and the host-tool probe


# @scenario SC-test-real-tools-25
def test_the_session_error_collects_both_what_was_denied_and_what_leaked():
    """One problem never hides the other: the text names the stray hits and
    the leaked processes, and the kind is EngineReached when a denied program
    ran."""
    class Leak:
        def describe(self):
            return "pid 1 leaked"
    error = gui_owner.session_error(["claude\tHOME=/h"], [Leak()])
    assert isinstance(error, gui_owner.EngineReached)
    assert "claude" in str(error) and "pid 1 leaked" in str(error)
    assert isinstance(gui_owner.session_error([], [Leak()]),
                      gui_owner.LeakedProcesses)
    assert gui_owner.session_error([], []) is None


# @scenario SC-test-real-tools-26
@pytest.mark.parametrize("inside", [True, False],
                         ids=["basetemp-inside-cwd", "basetemp-outside-cwd"])
def test_the_farm_is_made_under_the_folder_the_session_names(
        machine, monkeypatch, inside):
    """The farm goes under the pytest basetemp of the session and goes with
    it, wherever the session was started: a row runs in its own folder (a
    sibling of the farm), and a launch with no folder of its own is no row's
    workspace."""
    base = machine.tmp / "basetemp"
    base.mkdir()
    elsewhere = machine.tmp / "elsewhere"
    elsewhere.mkdir()
    monkeypatch.chdir(machine.tmp if inside else elsewhere)
    monkeypatch.setattr(gui_owner, "_SESSION_BASETEMP", None)   # restored after
    machine.owner.use_farm_base(str(base))
    assert machine.run("true") == 0
    owned = machine.owner.launch(["/bin/sh", "-c", "true"], machine.env(), None,
                                 str(machine.tmp), "tool", "no folder")
    assert owned.proc.wait(timeout=RUN_S) == 0
    assert os.path.dirname(machine.owner._farm.root) == str(base)


# @scenario SC-test-real-tools-32
def test_a_fake_login_under_a_basetemp_inside_the_account_home_is_allowed(
        machine, monkeypatch):
    """When TMPDIR or --basetemp sits inside the account's home, a login a row
    made in its scratch is not the user's; a link from the scratch to the
    account's own .claude still resolves outside the basetemp and is refused."""
    import types
    import wpe_driver
    account = machine.tmp / "acct-home"
    (account / ".claude").mkdir(parents=True)
    base = account / "base"
    base.mkdir()
    fake_login = base / "fake-login-home"
    (fake_login / ".claude").mkdir(parents=True)
    linked = base / "linked-home"
    linked.mkdir()
    os.symlink(str(account / ".claude"), str(linked / ".claude"))
    monkeypatch.setattr(pwd, "getpwuid",
                        lambda uid: types.SimpleNamespace(pw_dir=str(account)))
    monkeypatch.setattr(gui_owner, "_SESSION_BASETEMP", None)   # restored after
    machine.owner.use_farm_base(str(base))
    previous = gui_owner._CURRENT
    gui_owner.install(machine.owner)
    try:
        machine.owner.begin_row(ROW, PLAIN_FILE)
        wpe_driver.WpeSession._refuse_account_login(str(fake_login))
        with pytest.raises(gui_owner.OwnerRefused, match=r"\.claude"):
            wpe_driver.WpeSession._refuse_account_login(str(linked))
    finally:
        gui_owner.install(previous)


# @scenario SC-test-real-tools-27
def test_a_tool_outside_the_baseline_is_checked_for_presence_and_never_run(
        machine, monkeypatch):
    """host_tools.needs() runs `--version` only for a baseline tool: a fake
    engine on the PATH of the test process is found and not started."""
    import host_tools
    monkeypatch.setenv("PATH", "%s:%s" % (machine.folder, SYSTEM_PATH))
    assert host_tools.tool_usable(FAKE_ENGINE) is True
    assert not machine.ran(FAKE_ENGINE)
