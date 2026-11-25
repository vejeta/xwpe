"""One job of the CI plan, run as the workflow runs it (tests/ci-shard.sh).

The Linux workflow runs each shard of the plan with `sh tests/ci-shard.sh K`:
pytest on the node ids of the shard, cut by tests/harness/job_cut.py, with its
exit status kept in a status file the gate reads.  These rows run the script on
planted rows (tests/inner_row_clock.py) with a plan made by the planner, so
the cut, the junit a cut keeps, the status and the gate's reading of both are
proved without CI.  A shard of a display layer is planted the same way: its
plan names the layer, so its args file holds that layer's pytest options and
the script keeps its log tree, copies it out for its own artifact, makes the
runtime folder a compositor needs and leaves no folder of its own behind, with
no display anywhere.
"""
import importlib.util
import json
import os
import stat
import sys

import pytest

import gui_owner
import inner_row_clock

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(HERE)
HARNESS = os.path.join(HERE, "harness")
sys.path.insert(0, HARNESS)
try:
    import harness
    import suite_plan
finally:
    sys.path.pop(0)

INNER = "tests/inner_row_clock.py"
QUICK = ["%s::test_a_quick_row_before_the_hang" % INNER,
         "%s::test_another_quick_row_before_the_hang" % INNER]
HANGS = "%s::test_a_row_that_hangs_with_no_clock_of_its_own" % INNER
# A planted row of a display shard: it leaves a log tree, and a link loop the
# copy of that tree must leave behind, in its own folder under --basetemp.
LEAVES_LOGS = "%s::test_a_row_that_leaves_a_log_and_a_link_loop" % INNER
SCRIPT_TIMEOUT_S = 150
# The seconds a shard may take to reach the row that hangs and leave its mark.
# The cut waits for that mark and comes as soon as it is there, so a runner
# that is still bringing its test workers up delays the cut instead of losing
# it; the shard's own deadline is a net derived from this
# (gui_owner.cut_deadline_s), never the cut.
MARK_WAIT_S = 30
# A folder with a cut program that keeps no grace, as the runner's image did:
# first on the PATH of a row, it is the program a job that calls one gets.
NO_GRACE_CUT_DIR = os.path.join(HERE, "stubs", "no-grace")
LONG_BUDGET_S = "600"
# The mark left once the rows that finished are reported and the row that
# hangs has started (tests/inner_row_clock.py).
HANG_MARK = "hang"
# Where the script copies a display shard's log tree, and the names it gives
# the folders it makes for itself there: the copy is a sibling of the folder
# the gate reads, so the artifact of a shard's junit, status and trace holds no
# log tree and the gate, which downloads every shard's, downloads no
# screenshot.
LOGS_OUT_NAME = "shard-logs"
SHARD_FOLDER_PREFIXES = ("suite-logs-", "runtime-")
# The mode of a runtime folder the shard inherits: a shard that reused it would
# change it to the 0700 a compositor's socket needs.
INHERITED_MODE = 0o755

needs_the_job_plugins = pytest.mark.skipif(
    importlib.util.find_spec("xdist") is None
    or importlib.util.find_spec("pytest_rerunfailures") is None,
    reason="pytest-xdist and pytest-rerunfailures are not installed here "
           "(the Linux job pins both)")


def make_plan(tmp_path, ids):
    """Write ids and a one-job plan of them with the planner's command line;
    returns the folder that holds plan.json and args-0.txt."""
    idfile = tmp_path / "ids.txt"
    idfile.write_text("\n".join(ids) + "\n")
    record = tmp_path / "record.json"
    suite_plan.save_record(suite_plan.empty_record(), str(record))
    plan_dir = tmp_path / "plan"
    argv = [sys.executable, os.path.join(HARNESS, "suite_plan.py"), "plan",
            str(plan_dir / "plan.json"), "--record", str(record),
            "--collected", str(idfile), "--jobs-max", "1"]
    done = gui_owner.run_nested(argv, SCRIPT_TIMEOUT_S, cwd=TOP,
                                extra_env={"HOME": str(tmp_path)})
    assert done.returncode == 0, done.stdout + done.stderr
    return plan_dir


def shard_command(tmp_path, plan_dir, cut, extra_env=None):
    """`sh tests/ci-shard.sh 0` on the plan, with the clock out of the way and
    the shard's own deadline at @cut: (its argv, its environment, the
    folder the shard writes its junit and status in).  @extra_env names the
    variables a row adds (a display shard's log root)."""
    out = tmp_path / "shard-out"
    env = {"HOME": str(tmp_path), "TERM": "xterm", "PY": sys.executable,
           "PLAN_DIR": str(plan_dir), "SHARD_OUT": str(out), "SHARD_CUT": cut,
           "SHARD_LOGS_OUT": str(tmp_path / LOGS_OUT_NAME),
           gui_owner.ROW_BUDGET_VAR: LONG_BUDGET_S,
           "XWPE_INNER_COUNT_FILE": str(tmp_path / "count"),
           "XWPE_INNER_STARTED_FILE": str(tmp_path / "started"),
           "XWPE_INNER_CLOSED_FILE": str(tmp_path / "closed"),
           "XWPE_INNER_HANG_FILE": str(tmp_path / HANG_MARK),
           # The planted rows leave the mark the cut waits for from the
           # process that collects the reports, once the rows that finished
           # are in it (tests/inner_row_clock.py).
           "PYTHONPATH": HERE, "PYTEST_ADDOPTS": "-p inner_row_clock"}
    env.update(extra_env or {})
    return ["sh", "tests/ci-shard.sh", "0"], env, out


def what_it_said(done):
    """What the script printed, its output and then its log: a shard that stops
    for a folder it could not make says so on its log, which the owner keeps."""
    return (done.stdout + done.stderr).decode("utf-8", "replace")


def run_shard(tmp_path, plan_dir, cut, extra_env=None):
    """Run the shard to completion; returns (exit status, the output folder,
    what the script printed)."""
    argv, env, out = shard_command(tmp_path, plan_dir, cut, extra_env)
    done = gui_owner.run_nested(argv, SCRIPT_TIMEOUT_S, cwd=TOP, extra_env=env)
    return done.returncode, out, what_it_said(done)


def run_cut_shard(tmp_path, plan_dir, first_on_path=None, extra_env=None):
    """Run the shard and cut it once the row that hangs has left its mark, the
    way the job's own cut ends a shard that reached its minutes; returns
    (exit status, the output folder, what the script printed).  With
    @first_on_path, that folder is first on the shard's PATH."""
    argv, env, out = shard_command(
        tmp_path, plan_dir, "%ds" % gui_owner.cut_deadline_s(MARK_WAIT_S),
        extra_env)
    if first_on_path:
        env["PATH"] = first_on_path + os.pathsep + os.environ["PATH"]
    done = gui_owner.run_nested_cut_on_mark(
        argv, SCRIPT_TIMEOUT_S, str(tmp_path / HANG_MARK), MARK_WAIT_S,
        cwd=TOP, extra_env=env)
    return done.returncode, out, what_it_said(done)


def make_display_plan(tmp_path, ids, layer=suite_plan.X11):
    """Write a plan of one shard of a display layer holding @ids, with the
    planner's own writer: the shard keeps a log tree, is cut after its layer's
    own minutes, and its args file holds that layer's pytest options and no
    other.  The rows are the planted ones, so no display is needed to prove
    what the script does with a display shard."""
    plan = suite_plan.empty_plan()
    suite_plan.plan_layer(plan, suite_plan.empty_record(), layer, list(ids))
    plan["jobs"] = len(plan["shards"])
    plan_dir = tmp_path / "plan"
    suite_plan.write_plan(plan, str(plan_dir / "plan.json"))
    return plan_dir


def copied_logs(tmp_path, shard=0):
    """(the folder the shard's log copy went to, the names of the files in it)."""
    folder = tmp_path / LOGS_OUT_NAME / ("logs-%d" % shard)
    names = []
    for root, _dirs, found in os.walk(str(folder)):
        names.extend(found)
    return folder, sorted(names)


def is_a_shard_folder(name):
    """True for the name of a folder a shard makes for itself: the log tree of
    its rows or the runtime folder of its compositor."""
    return any(name.startswith(prefix) for prefix in SHARD_FOLDER_PREFIXES)


def shard_folders_left(root):
    """The folders a shard made for itself and left under @root, or under one
    folder of it (the temporary one a shard with no RUNNER_TEMP makes for
    itself).  The script removes the folder it made once the copy for the
    upload is done, so a run leaves none."""
    entries = sorted(os.listdir(str(root)))
    left = [name for name in entries if is_a_shard_folder(name)]
    for name in entries:
        folder = os.path.join(str(root), name)
        if os.path.isdir(folder):
            left += ["%s/%s" % (name, inner)
                     for inner in sorted(os.listdir(folder))
                     if is_a_shard_folder(inner)]
    return left


@needs_the_job_plugins
# @scenario SC-platform-ci-shard-01
def test_a_shard_runs_its_node_ids_and_says_it_ended_with_zero(tmp_path):
    plan_dir = make_plan(tmp_path, QUICK)
    status, out, text = run_shard(tmp_path, plan_dir, "100s")
    assert status == 0, text[-1500:]
    written = json.loads((out / "shard-0-status.json").read_text())
    assert written["exit"] == 0 and not written["cut"] and written["ids"] == 2, written
    junit = (out / "suite-junit-0.xml").read_text()
    for node in QUICK:
        assert node.split("::")[1] in junit, node


@needs_the_job_plugins
# @scenario SC-platform-ci-shard-02
def test_a_cut_shard_keeps_exit_124_and_the_junit_of_the_rows_it_finished(tmp_path):
    plan_dir = make_plan(tmp_path, QUICK + [HANGS])
    status, out, text = run_cut_shard(tmp_path, plan_dir)
    assert status == 124, "the cut is not masked: %s\n%s" % (status, text[-1500:])
    written = json.loads((out / "shard-0-status.json").read_text())
    assert written["exit"] == 124 and written["cut"], written
    junit = (out / "suite-junit-0.xml").read_text()
    for node in QUICK:
        assert node.split("::")[1] in junit, "a finished row is not in the junit"


@needs_the_job_plugins
# @scenario SC-platform-ci-shard-04
def test_a_cut_shard_keeps_its_grace_and_its_junit_whatever_cut_program_the_image_ships(tmp_path):
    """The runner's image shipped a cut program that sent its kill at the
    deadline, with no grace, so the interrupted pytest never wrote its junit
    and the shard ended 137.  The shard is cut by the tree's own tool, so a
    program of the image first on PATH changes nothing: 124 and the junit."""
    plan_dir = make_plan(tmp_path, QUICK + [HANGS])
    status, out, text = run_cut_shard(tmp_path, plan_dir, first_on_path=NO_GRACE_CUT_DIR)
    assert status == 124, "the image's cut program decided the cut: %s\n%s" % (status, text[-1500:])
    written = json.loads((out / "shard-0-status.json").read_text())
    assert written["exit"] == 124 and written["cut"] and not written["killed"], written
    junit = (out / "suite-junit-0.xml").read_text()
    for node in QUICK:
        assert node.split("::")[1] in junit, "a finished row is not in the junit"


@needs_the_job_plugins
# @scenario SC-platform-ci-shard-03
def test_the_gate_reads_a_cut_shard_and_names_the_rows_it_never_ran(
        tmp_path, monkeypatch, capsys):
    """The cut shard's junit holds the quick rows and not the row that hung:
    the gate calls that one MISSING and says the shard was cut."""
    plan_dir = make_plan(tmp_path, QUICK + [HANGS])
    status, out, _text = run_cut_shard(tmp_path, plan_dir)
    assert status == 124
    monkeypatch.setattr(harness, "load_manifest", lambda cap: {"scenarios": [
        {"id": "SC-hang-01", "status": "new", "layer": "ncurses", "test": HANGS},
        {"id": "SC-quick-01", "status": "new", "layer": "ncurses", "test": QUICK[0]}]})
    junit = out / "suite-junit-0.xml"
    harness.cmd_report("fake", "ncurses", str(junit), str(plan_dir / "plan.json"), str(out))
    seen = capsys.readouterr()
    report = json.loads(seen.out)
    assert report == {"SC-hang-01": "MISSING", "SC-quick-01": "PASS"}, report
    assert "SC-hang-01: MISSING, not run: shard 0 was cut at 100 min" in seen.err, seen.err


@needs_the_job_plugins
def test_the_gate_names_a_shard_that_uploaded_nothing(tmp_path, monkeypatch, capsys):
    """The planted missing upload: the shard's junit and status never came."""
    plan_dir = make_plan(tmp_path, QUICK + [HANGS])
    empty = tmp_path / "nothing"
    empty.mkdir()
    monkeypatch.setattr(harness, "load_manifest", lambda cap: {"scenarios": [
        {"id": "SC-quick-01", "status": "new", "layer": "ncurses", "test": QUICK[0]}]})
    harness.cmd_report("fake", "ncurses", str(empty / "suite-junit-0.xml"),
                       str(plan_dir / "plan.json"), str(empty))
    seen = capsys.readouterr()
    assert json.loads(seen.out) == {"SC-quick-01": "MISSING"}, seen.out
    assert "SC-quick-01: MISSING, shard 0 uploaded nothing" in seen.err, seen.err


# @scenario SC-platform-ci-shard-05
def test_a_display_shard_copies_the_files_of_its_log_tree_and_leaves_a_link_loop(tmp_path):
    """A display shard keeps the log tree of its rows under --basetemp, and the
    script copies the regular files of it out for the upload.  A row plants a
    link loop in its own folder on purpose (a folder operation must not follow
    one) and an upload that stats every entry it walks ends with ELOOP holding
    nothing at all, so the copy leaves the loop behind, and a link out of the
    tree with it.  A log whose name starts with a dash goes with the rest: a
    copy that hands the names it found to a program as it found them gives that
    program an option, and one such file loses every log of the shard."""
    plan_dir = make_display_plan(tmp_path, QUICK + [LEAVES_LOGS])
    logs_root = tmp_path / "logs"
    status, _out, text = run_shard(tmp_path, plan_dir, "100s",
                                   extra_env={"SHARD_LOGS_ROOT": str(logs_root)})
    assert status == 0, text[-1500:]
    assert (logs_root / "suite-logs-0").is_dir(), "the shard kept no log tree"
    _folder, names = copied_logs(tmp_path)
    assert inner_row_clock.LOG_NAME in names, \
        "the copy held nothing: %s\n%s" % (names, text[-1500:])
    assert inner_row_clock.NESTED_LOG_NAME in names, names
    assert inner_row_clock.DASH_LOG_NAME in names, \
        "a log named as an option was not copied: %s" % names
    assert inner_row_clock.LOOP_NAME not in names, "the copy took a link loop"
    assert inner_row_clock.OUTSIDE_LINK_NAME not in names, "the copy followed a link"


# @scenario SC-platform-ci-shard-06
def test_a_display_shard_keeps_its_log_tree_under_the_runners_own_temporary_folder(tmp_path):
    """On a runner the log tree lives under its own temporary folder, which the
    upload step of the workflow reaches."""
    plan_dir = make_display_plan(tmp_path, QUICK + [LEAVES_LOGS])
    runner_temp = tmp_path / "runner-temp"
    runner_temp.mkdir()
    status, _out, text = run_shard(tmp_path, plan_dir, "100s",
                                   extra_env={"RUNNER_TEMP": str(runner_temp)})
    assert status == 0, text[-1500:]
    assert (runner_temp / "suite-logs-0").is_dir(), "the shard kept no log tree"
    _folder, names = copied_logs(tmp_path)
    assert inner_row_clock.LOG_NAME in names, names


# @scenario SC-platform-ci-shard-07
def test_a_display_shard_with_no_runner_temp_keeps_its_log_tree_outside_the_checkout(tmp_path):
    """RUNNER_TEMP is the runner's own and is set nowhere else: a basetemp
    built from it when it is unset is /suite-logs-K, which no user can write,
    and a basetemp in the checkout breaks a row instead (a launch that names
    the checkout as its working folder refuses a tool farm inside it).  With
    neither named, the shard takes a new temporary folder of its own."""
    plan_dir = make_display_plan(tmp_path, QUICK + [LEAVES_LOGS])
    status, _out, text = run_shard(tmp_path, plan_dir, "100s")
    assert status == 0, text[-1500:]
    assert not os.path.exists("/suite-logs-0"), "the log tree went to the root"
    assert not os.path.exists(os.path.join(TOP, "suite-logs-0")), \
        "the log tree went to the checkout"
    _folder, names = copied_logs(tmp_path)
    assert inner_row_clock.LOG_NAME in names, names


# @scenario SC-platform-ci-shard-08
def test_a_cut_display_shard_is_cut_after_its_own_minutes_and_the_gate_says_them(
        tmp_path, monkeypatch, capsys):
    """A display shard is cut after its own layer's minutes and never after the
    ncurses one's: the status file, and the line the gate prints for a row the
    cut never reached, both say the shard's own."""
    plan_dir = make_display_plan(tmp_path, QUICK + [HANGS])
    status, out, text = run_cut_shard(
        tmp_path, plan_dir, extra_env={"SHARD_LOGS_ROOT": str(tmp_path / "logs")})
    assert status == 124, "the cut is not masked: %s\n%s" % (status, text[-1500:])
    written = json.loads((out / "shard-0-status.json").read_text())
    assert written["exit"] == 124 and written["cut"], written
    assert written["layer"] == suite_plan.X11, written
    assert written["cut_min"] == suite_plan.DISPLAY_CUT_MIN, written
    junit = out / "suite-junit-0.xml"
    written_junit = junit.read_text()
    order = [line for line in (plan_dir / "args-0.txt").read_text().splitlines()
             if "::" in line]
    finished = order[:order.index(HANGS)]
    assert finished, "the shard ran nothing before the row that hangs"
    for node in finished:
        assert node.split("::")[1] in written_junit, "a finished row is not in the junit"
    assert HANGS.split("::")[1] not in written_junit, "the row that hung is in the junit"
    monkeypatch.setattr(harness, "load_manifest", lambda cap: {"scenarios": [
        {"id": "SC-hang-01", "status": "new", "layer": "x11", "test": HANGS}]})
    assert harness.cmd_report("fake", "x11", str(junit),
                              str(plan_dir / "plan.json"), str(out)) == 1
    seen = capsys.readouterr()
    assert "SC-hang-01: MISSING, not run: shard 0 was cut at %d min" \
        % suite_plan.DISPLAY_CUT_MIN in seen.err, seen.err


# @scenario SC-platform-ci-shard-09
def test_a_shard_with_no_runner_temp_removes_the_folder_it_made_for_itself(tmp_path):
    """A shard with no RUNNER_TEMP of its own makes a temporary folder for its
    log tree and its runtime folder, and removes it once the copy for the
    upload is done: nothing reads it after that, and a machine that runs a
    shard a day would otherwise keep the log tree of every one of them.  The
    folder is made under TMPDIR, which this row names, so what is left of it is
    seen here and not in the machine's own temporary folder."""
    plan_dir = make_display_plan(tmp_path, QUICK + [LEAVES_LOGS])
    machine_tmp = tmp_path / "machine-tmp"
    machine_tmp.mkdir()
    status, _out, text = run_shard(tmp_path, plan_dir, "100s",
                                   extra_env={"TMPDIR": str(machine_tmp)})
    assert status == 0, text[-1500:]
    _folder, names = copied_logs(tmp_path)
    assert inner_row_clock.LOG_NAME in names, \
        "the folder went before the copy was done: %s\n%s" % (names, text[-1500:])
    assert shard_folders_left(machine_tmp) == [], \
        "the shard left the folder it made behind: %s" % shard_folders_left(machine_tmp)
    assert shard_folders_left(tmp_path) == [], \
        "the shard wrote a folder of its own beside the ones this row named"


# @scenario SC-platform-ci-shard-10
def test_a_shard_makes_its_own_runtime_folder_and_never_takes_an_inherited_one(tmp_path):
    """A compositor puts its socket in XDG_RUNTIME_DIR, so a display shard
    needs one: always its own, 0700, under the folder it keeps its log tree in.
    An inherited one belongs to whoever set it, the desktop or another job, and
    a shard that reused it would put its socket where another run looks for its
    own and change the mode of a folder that is none of its business."""
    plan_dir = make_display_plan(tmp_path, QUICK + [LEAVES_LOGS])
    logs_root = tmp_path / "logs"
    inherited = gui_owner.current().make_runtime_dir()
    os.chmod(inherited, INHERITED_MODE)
    status, _out, text = run_shard(tmp_path, plan_dir, "100s", extra_env={
        "SHARD_LOGS_ROOT": str(logs_root), "XDG_RUNTIME_DIR": inherited})
    assert status == 0, text[-1500:]
    own = logs_root / "runtime-0"
    assert own.is_dir(), "the shard made no runtime folder of its own"
    assert stat.S_IMODE(own.stat().st_mode) == 0o700, \
        "its own runtime folder is %s" % oct(stat.S_IMODE(own.stat().st_mode))
    assert stat.S_IMODE(os.stat(inherited).st_mode) == INHERITED_MODE, \
        "the shard took the inherited runtime folder and changed it"


# @scenario SC-platform-ci-shard-11
def test_a_shard_that_cannot_make_its_own_folders_says_so_and_runs_nothing(tmp_path):
    """A shard with nowhere to write has nowhere to put the junit and the status
    the gate reads, so it names what it could not make and stops instead of
    running a suite whose result never reaches the gate."""
    plan_dir = make_display_plan(tmp_path, QUICK + [LEAVES_LOGS])
    blocked = tmp_path / "blocked"
    blocked.write_text("a file, so nothing can be made under it\n")
    status, out, text = run_shard(tmp_path, plan_dir, "100s", extra_env={
        "SHARD_LOGS_ROOT": str(blocked / "logs")})
    assert status != 0, "the shard ran with no folder of its own: %s" % text[-1500:]
    assert "cannot make" in text, text[-1500:]
    assert not (out / "suite-junit-0.xml").exists(), "the shard ran its rows"
