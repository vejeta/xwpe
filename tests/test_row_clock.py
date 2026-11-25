"""The row clock (tests/gui_owner.py, RowClock): a row that hangs is cut.

Without it a hung row costs the whole job: the Linux job of 2026-11-22 was
cut after six hours inside one pytest step, and no junit and no log were kept.
The clock gives each row a budget, ends a row that runs past it with its name
and what it waited for, and ends the editor it held through the owner's ladder
as for any failure.  Each row here runs a planted row of
tests/inner_row_clock.py under a child pytest with this tree's conftest, and
reads what the child printed and wrote.
"""
import importlib.util
import os
import sys

import pytest

import gui_owner

HERE = os.path.dirname(os.path.abspath(__file__))
INNER = os.path.join(HERE, "inner_row_clock.py")
INNER_TIMEOUT_S = 150
# What the child may take beyond the budget: its start, the editor's start
# and end ladder.
BUDGET_S = "3"
LONG_BUDGET_S = "600"
# The seconds a cut run may take to reach the row that hangs and leave its
# mark.  The cut waits for that mark and comes as soon as it is there, so a
# runner that is still bringing its test workers up delays the cut instead of
# losing it; the wrapper's own deadline is a net derived from this
# (gui_owner.cut_deadline_s), never the cut.
MARK_WAIT_S = 30
KILL_AFTER_GRACE_S = "20"
# The job's own cut, which every cut run here goes through.
JOB_CUT = os.path.join(HERE, "harness", "job_cut.py")
# The mark left once the rows that finished are reported and the row that
# hangs has started (tests/inner_row_clock.py).
HANG_MARK = "hang"


def inner_command(tmp_path, cases, budget, pytest_args=(), wrapper=(),
                  extra_env=None):
    """The child pytest of the planted rows named in @cases, with
    XWPE_ROW_BUDGET_S=@budget under @wrapper: (its argv, its environment, the
    folder the rows write their marks in)."""
    home = tmp_path / "home"
    home.mkdir(exist_ok=True)
    marks = tmp_path / "marks"
    marks.mkdir(exist_ok=True)
    env = {"HOME": str(home), "TERM": "xterm",
           gui_owner.ROW_BUDGET_VAR: budget,
           "XWPE_INNER_COUNT_FILE": str(marks / "count"),
           "XWPE_INNER_STARTED_FILE": str(marks / "started"),
           "XWPE_INNER_CLOSED_FILE": str(marks / "closed"),
           "XWPE_INNER_HANG_FILE": str(marks / HANG_MARK)}
    env.update(extra_env or {})
    argv = list(wrapper) + [
        sys.executable, "-m", "pytest", "-q", "-p", "no:cacheprovider", "-rfE",
        "--basetemp", str(tmp_path / "inner")] + list(pytest_args)
    argv += ["%s::%s" % (INNER, case) for case in cases]
    return argv, env, marks


def inner_run(tmp_path, cases, budget, pytest_args=(), extra_env=None):
    """Run the planted rows named in @cases under a child pytest with
    XWPE_ROW_BUDGET_S=@budget.  Returns (exit status, text, the folder with
    the files the rows wrote)."""
    argv, env, marks = inner_command(tmp_path, cases, budget,
                                     pytest_args=pytest_args,
                                     extra_env=extra_env)
    done = gui_owner.run_nested(argv, INNER_TIMEOUT_S, cwd=str(tmp_path),
                                extra_env=env)
    return done.returncode, done.stdout.decode("utf-8", "replace"), marks


def lines_of(marks, name):
    """How many lines the planted row wrote to its file @name."""
    path = marks / name
    return len(path.read_text().split()) if path.exists() else 0


def assert_cut_clean(status, text, marks, budget):
    """The row failed as RowOverBudget naming its budget, and nothing was
    left behind: an editor the row held would error the child's run."""
    assert status == 1, text[-1500:]
    assert "1 failed" in text, text[-1500:]
    assert "RowOverBudget" in text, text[-1500:]
    assert "ran past its budget of %s" % budget in text, text[-1500:]
    assert " error" not in text.splitlines()[-1], text[-1500:]


# @scenario SC-platform-row-clock-01
def test_a_row_that_waits_forever_holding_an_editor_is_cut_and_the_editor_ended(tmp_path):
    status, text, marks = inner_run(
        tmp_path, ["test_a_row_holding_an_editor_waits_forever"], BUDGET_S)
    assert_cut_clean(status, text, marks, "%s.0 s" % BUDGET_S)
    assert lines_of(marks, "started") == 1, "the row never held its editor:\n" + text[-800:]


def test_a_wait_that_swallows_every_exception_is_still_cut(tmp_path):
    """A hang inside `except Exception: pass` must fail, never pass."""
    status, text, marks = inner_run(
        tmp_path, ["test_a_wait_that_swallows_every_exception"], BUDGET_S)
    assert_cut_clean(status, text, marks, "%s.0 s" % BUDGET_S)


def test_a_row_marked_as_a_known_gap_that_hangs_fails_the_run(tmp_path):
    status, text, _marks = inner_run(
        tmp_path, ["test_an_incoherence_row_that_hangs"], "2")
    assert status == 1 and "1 failed" in text and "xfailed" not in text, text[-1500:]
    assert "RowOverBudget" in text, text[-1500:]


# @scenario SC-platform-row-clock-03
def test_a_plain_xfail_row_that_hangs_fails_the_run_and_is_not_run_again(tmp_path):
    """A row marked xfail with no reason to name is the common known gap.  The
    clock's cut must not read as that gap, and must not be run again: the
    rerun plugin reads the report before the conftest corrects it, so the
    cut has to be made known to it."""
    status, text, marks = inner_run(
        tmp_path, ["test_a_plain_xfail_row_that_hangs_and_counts_its_runs"], "2",
        pytest_args=rerun_args("--rerun-except", "RowOverBudget")
        if importlib.util.find_spec("pytest_rerunfailures") else ())
    assert status == 1 and "1 failed" in text and "xfailed" not in text, text[-1500:]
    assert "RowOverBudget" in text, text[-1500:]
    if importlib.util.find_spec("pytest_rerunfailures"):
        assert lines_of(marks, "count") == 1, "the cut row was run again:\n" + text[-1500:]


def test_a_budget_that_runs_out_while_the_editor_starts_waits_for_the_start(tmp_path):
    """The expiry comes inside WpeSession's start: the editor is started
    whole, and the row is cut after it, with nothing left behind."""
    status, text, marks = inner_run(
        tmp_path, ["test_a_row_waits_while_the_budget_runs_out_in_the_start"], "0.2")
    assert_cut_clean(status, text, marks, "0.2 s")
    assert lines_of(marks, "started") == 1, text[-800:]


def test_a_budget_that_runs_out_inside_the_end_ladder_lets_the_ladder_finish(tmp_path):
    status, text, marks = inner_run(
        tmp_path, ["test_the_budget_runs_out_inside_the_end_ladder"], LONG_BUDGET_S)
    assert status == 1 and "1 failed" in text and "RowOverBudget" in text, text[-1500:]
    assert " error" not in text.splitlines()[-1], text[-1500:]
    assert lines_of(marks, "started") == 1, text[-800:]
    assert lines_of(marks, "closed") == 1, (
        "the end ladder was cut before the editor was ended whole:\n" + text[-1500:])


def rerun_args(*extra):
    """The reruns of the Linux job, with a delay of nothing."""
    return ["--reruns", "2", "--reruns-delay", "0"] + list(extra)


needs_rerunfailures = pytest.mark.skipif(
    importlib.util.find_spec("pytest_rerunfailures") is None,
    reason="pytest-rerunfailures is not installed here (the Linux job pins it)")


@needs_rerunfailures
def test_a_cut_row_is_not_run_again_with_rerun_except(tmp_path):
    """A hung row costs one budget, not three."""
    status, text, marks = inner_run(
        tmp_path, ["test_a_hung_row_that_counts_its_runs"], "2",
        pytest_args=rerun_args("--rerun-except", "RowOverBudget"))
    assert status == 1 and "RowOverBudget" in text, text[-1500:]
    assert lines_of(marks, "count") == 1, text[-1500:]


@needs_rerunfailures
def test_without_rerun_except_the_same_row_runs_three_times(tmp_path):
    """The reason the Linux job passes --rerun-except: it is what keeps the
    cut row from running again."""
    status, text, marks = inner_run(
        tmp_path, ["test_a_hung_row_that_counts_its_runs"], "2",
        pytest_args=rerun_args())
    assert status == 1, text[-1500:]
    assert lines_of(marks, "count") == 3, text[-1500:]


def interrupted_run(tmp_path, signal_name, in_workers):
    """Two quick rows and a row that hangs, under the job's own cut
    (tests/harness/job_cut.py, `--signal <signal>`) with the clock out of the way, cut once the row that
    hangs has left its mark; in two test workers when @in_workers and xdist is
    installed.  The cut waits for the mark the collecting process leaves once
    the rows that finished are reported to it (tests/inner_row_clock.py), so
    the junit holds them.  Returns (status, junit path)."""
    junit = tmp_path / "inner.xml"
    workers = ["-n", "2"] if in_workers and importlib.util.find_spec("xdist") else []
    argv, env, marks = inner_command(
        tmp_path,
        ["test_a_quick_row_before_the_hang",
         "test_another_quick_row_before_the_hang",
         "test_a_row_that_hangs_with_no_clock_of_its_own"],
        LONG_BUDGET_S,
        pytest_args=workers + ["-p", "inner_row_clock", "--junitxml", str(junit)],
        wrapper=[sys.executable, JOB_CUT, "--signal", signal_name,
                 "--grace", KILL_AFTER_GRACE_S,
                 "--after", "%ds" % gui_owner.cut_deadline_s(MARK_WAIT_S), "--"],
        extra_env={"PYTHONPATH": HERE})
    done = gui_owner.run_nested_cut_on_mark(
        argv, INNER_TIMEOUT_S, str(marks / HANG_MARK), MARK_WAIT_S,
        cwd=str(tmp_path), extra_env=env)
    return done.returncode, junit


# @scenario SC-platform-row-clock-02
def test_a_job_cut_by_an_interrupt_keeps_the_junit_of_the_rows_it_finished(tmp_path):
    status, junit = interrupted_run(tmp_path, "INT", True)
    assert status == 124, "timeout ends a cut job with 124, not %s" % status
    assert junit.exists(), "the interrupt left no junit"
    text = junit.read_text()
    assert "test_a_quick_row_before_the_hang" in text
    assert "test_another_quick_row_before_the_hang" in text


def test_a_job_cut_by_a_kill_keeps_no_junit(tmp_path):
    """The reason the job is cut with INT: a kill writes nothing, so the
    check above reads the junit and not the exit status."""
    status, junit = interrupted_run(tmp_path, "KILL", False)
    assert status in (124, 137, -9), "the run was not killed: %s" % status
    assert not junit.exists(), "a killed run wrote a junit"
