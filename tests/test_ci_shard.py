"""One job of the CI plan, run as the workflow runs it (tests/ci-shard.sh).

The Linux workflow runs each shard of the plan with `sh tests/ci-shard.sh K`:
pytest on the node ids of the shard, cut by tests/harness/job_cut.py, with its
exit status kept in a status file the gate reads.  These rows run the script on
planted rows (tests/inner_row_clock.py) with a plan made by the planner, so
the cut, the junit a cut keeps, the status and the gate's reading of both are
proved without CI.
"""
import importlib.util
import json
import os
import sys

import pytest

import gui_owner

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


def shard_command(tmp_path, plan_dir, cut):
    """`sh tests/ci-shard.sh 0` on the plan, with the clock out of the way and
    the shard's own deadline at @cut: (its argv, its environment, the
    folder the shard writes its junit and status in)."""
    out = tmp_path / "shard-out"
    env = {"HOME": str(tmp_path), "TERM": "xterm", "PY": sys.executable,
           "PLAN_DIR": str(plan_dir), "SHARD_OUT": str(out), "SHARD_CUT": cut,
           gui_owner.ROW_BUDGET_VAR: LONG_BUDGET_S,
           "XWPE_INNER_COUNT_FILE": str(tmp_path / "count"),
           "XWPE_INNER_STARTED_FILE": str(tmp_path / "started"),
           "XWPE_INNER_CLOSED_FILE": str(tmp_path / "closed"),
           "XWPE_INNER_HANG_FILE": str(tmp_path / HANG_MARK),
           # The planted rows leave the mark the cut waits for from the
           # process that collects the reports, once the rows that finished
           # are in it (tests/inner_row_clock.py).
           "PYTHONPATH": HERE, "PYTEST_ADDOPTS": "-p inner_row_clock"}
    return ["sh", "tests/ci-shard.sh", "0"], env, out


def run_shard(tmp_path, plan_dir, cut):
    """Run the shard to completion; returns (exit status, the output folder,
    what the script printed)."""
    argv, env, out = shard_command(tmp_path, plan_dir, cut)
    done = gui_owner.run_nested(argv, SCRIPT_TIMEOUT_S, cwd=TOP, extra_env=env)
    return done.returncode, out, done.stdout.decode("utf-8", "replace")


def run_cut_shard(tmp_path, plan_dir, first_on_path=None):
    """Run the shard and cut it once the row that hangs has left its mark, the
    way the job's own cut ends a shard that reached its minutes; returns
    (exit status, the output folder, what the script printed).  With
    @first_on_path, that folder is first on the shard's PATH."""
    argv, env, out = shard_command(
        tmp_path, plan_dir, "%ds" % gui_owner.cut_deadline_s(MARK_WAIT_S))
    if first_on_path:
        env["PATH"] = first_on_path + os.pathsep + os.environ["PATH"]
    done = gui_owner.run_nested_cut_on_mark(
        argv, SCRIPT_TIMEOUT_S, str(tmp_path / HANG_MARK), MARK_WAIT_S,
        cwd=TOP, extra_env=env)
    return done.returncode, out, done.stdout.decode("utf-8", "replace")


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
