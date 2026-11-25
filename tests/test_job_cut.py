"""The cut of a job (tests/harness/job_cut.py): the one program that ends a run
that ran out of minutes, the same on every image.

A job that is cut has to keep what it finished, and say how it ended.  The cut
interrupts the run's own process (so pytest writes its junit), waits its grace,
and then kills every process the run started, in whatever group it is.  Each
row runs the tool on a small command of tests/inner_job_cut.py that behaves the
way a run does at its cut, and reads the exit status, the report the tool
writes and the marks the command left.
"""
import argparse
import importlib.util
import json
import os
import signal
import sys

import pytest

import gui_owner

HERE = os.path.dirname(os.path.abspath(__file__))
HARNESS = os.path.join(HERE, "harness")
JOB_CUT = os.path.join(HARNESS, "job_cut.py")
INNER = os.path.join(HERE, "inner_job_cut.py")
INNER_ROWS = os.path.join(HERE, "inner_row_clock.py")
sys.path.insert(0, HARNESS)
try:
    import job_cut
    import suite_plan
finally:
    sys.path.pop(0)

TOOL_TIMEOUT_S = 150
# The seconds the command may take to leave its mark; the cut waits for the
# mark and comes at once, so a slow host delays the cut and never loses it.
MARK_WAIT_S = 30
# The tool's own deadline when a row cuts at a mark: a net, never the cut.
NET_AFTER = "600s"
GRACE_S = "2"
# What a kill that has to chase a run that keeps forking may take beyond the
# grace and the wait for the stack dump (the tool's own bound is 30 s).
KILL_BOUND_MARGIN_S = 20
KILL_BOUND_TEST_S = "2"
# For a run that ends itself after the cut and must not be taken for one that
# did not.
ROOMY_GRACE_S = "30"
# The code a parent runs to ignore the interrupt and then become the tool, as
# a command started in the background by a shell does.
IGNORE_THEN_RUN = ("import os, signal, sys; "
                   "signal.signal(signal.SIGINT, signal.SIG_IGN); "
                   "os.execv(sys.argv[1], sys.argv[1:])")
# A parent that ignores every signal a cut may need and blocks another one,
# then becomes the tool: what a shell that started the job with `nohup` and
# `&` leaves its child.  SIGQUIT is the one signal the tool does not handle
# itself, so only the reset before the exec gives it back its default.
IGNORE_ALL_THEN_RUN = ("import os, signal, sys; "
                       "[signal.signal(n, signal.SIG_IGN) for n in (signal.SIGINT, "
                       "signal.SIGTERM, signal.SIGHUP, signal.SIGQUIT)]; "
                       "signal.pthread_sigmask(signal.SIG_BLOCK, [signal.SIGUSR2]); "
                       "os.execv(sys.argv[1], sys.argv[1:])")
SIGINT_BIT =1 << (signal.SIGINT - 1)
STATUS_CUT, STATUS_KILLED = 124, 137
USAGE_ERROR = 2

needs_the_job_plugins = pytest.mark.skipif(
    importlib.util.find_spec("xdist") is None
    or importlib.util.find_spec("pytest_rerunfailures") is None,
    reason="pytest-xdist and pytest-rerunfailures are not installed here "
           "(the Linux job pins both)")


def tool_command(words, after=NET_AFTER, grace=GRACE_S, report=None,
                 signal_name=None, parent=()):
    """The tool's command line on the command of inner_job_cut.py named by
    @words, under @parent (a wrapper that starts it)."""
    argv = list(parent) + [sys.executable, JOB_CUT, "--after", after,
                           "--grace", grace]
    if report:
        argv += ["--report", str(report)]
    if signal_name:
        argv += ["--signal", signal_name]
    return argv + ["--", sys.executable, INNER] + [str(word) for word in words]


def read_report(path):
    """The report the tool wrote."""
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def pids_in(path):
    """The pids a command wrote to @path, one to a line."""
    with open(path, encoding="utf-8") as fh:
        return [int(word) for word in fh.read().split()]


def run_to_the_end(tmp_path, words, after=NET_AFTER, grace=GRACE_S,
                   parent=(), extra_env=None):
    """Run the tool until it ends by itself; returns (status, what it printed,
    the report)."""
    report = tmp_path / "report.json"
    done = gui_owner.run_nested(
        tool_command(words, after, grace, report, parent=parent),
        TOOL_TIMEOUT_S, cwd=str(tmp_path), extra_env=extra_env)
    text = done.stdout.decode("utf-8", "replace")
    return done.returncode, text, read_report(report) if report.exists() else None


def run_cut_at_mark(tmp_path, words, mark, grace=GRACE_S, parent=(),
                    signal_name=None):
    """Run the tool and make its deadline arrive once the command has left
    @mark (the event the rows use, never a clock); returns (status, what it
    printed, the report)."""
    report = tmp_path / "report.json"
    done = gui_owner.run_nested_cut_on_mark(
        tool_command(words, NET_AFTER, grace, report, signal_name, parent),
        TOOL_TIMEOUT_S, str(mark), MARK_WAIT_S, cwd=str(tmp_path))
    text = done.stdout.decode("utf-8", "replace")
    return done.returncode, text, read_report(report) if report.exists() else None


def start_tool(tmp_path, words, mark):
    """Start the tool on @words and wait for @mark; returns the owned process,
    still running.  For the rows that signal the tool themselves."""
    owner = gui_owner.current()
    env = gui_owner.tool_env(inherit=gui_owner.HARNESS_VARS)
    owned = owner.launch(tool_command(words, report=tmp_path / "report.json"), env,
                         str(tmp_path), owner.tool_home(), "tool", "job_cut",
                         stdio=gui_owner.LOG)
    gui_owner.wait_for(lambda: os.path.exists(mark), "the command's mark (%s)" % mark,
                       MARK_WAIT_S, state_fn=owned.log_text)
    return owned


def end_tool(owned):
    """Wait for the tool to end after a signal; returns its exit status."""
    if not owned.watch.wait(gui_owner.scaled(TOOL_TIMEOUT_S)):
        raise gui_owner.OwnerTimeout("the tool did not end after its signal")
    owned.end()
    return owned.proc.returncode


# @scenario SC-platform-job-cut-01
@pytest.mark.parametrize("code", [0, 1, 5])
def test_a_run_that_ends_in_time_keeps_its_own_status(tmp_path, code):
    status, text, report = run_to_the_end(tmp_path, ["exit", code])
    assert status == code, text
    assert report["cut"] is False and report["killed"] is False, report
    assert report["exit"] == code and report["command_exit"] == code, report


# @scenario SC-platform-job-cut-02
def test_a_run_whose_parent_ignored_the_interrupt_gets_the_default_one(tmp_path):
    status, text, _report = run_to_the_end(
        tmp_path, ["dispositions"], parent=[sys.executable, "-c", IGNORE_THEN_RUN])
    assert status == 0, text
    assert "SIGINT=interrupt" in text, text
    assert "SIGTERM=default" in text and "blocked=[]" in text, text


# @scenario SC-platform-job-cut-16
def test_a_run_gets_every_cut_signal_at_its_default_and_an_empty_mask(tmp_path):
    """The tool itself inherited signals ignored (quit among them) and one
    blocked: the run must start with none of it."""
    status, text, _report = run_to_the_end(
        tmp_path, ["dispositions"],
        parent=[sys.executable, "-c", IGNORE_ALL_THEN_RUN])
    assert status == 0, text
    for line in ("SIGINT=interrupt", "SIGTERM=default", "SIGHUP=default",
                 "SIGQUIT=default", "blocked=[]"):
        assert line in text, text


# @scenario SC-platform-job-cut-03
def test_a_run_whose_parent_ignored_the_interrupt_is_still_cut_and_takes_it(tmp_path):
    ready, got = tmp_path / "ready", tmp_path / "got"
    status, text, report = run_cut_at_mark(
        tmp_path, ["take-interrupt", ready, got], ready,
        parent=[sys.executable, "-c", IGNORE_THEN_RUN])
    assert status == STATUS_CUT, text
    assert got.exists(), "the run never took the interrupt:\n" + text
    assert report["cut"] is True and report["killed"] is False, report


# @scenario SC-platform-job-cut-04
def test_the_cut_reaches_the_controller_only_and_no_worker_is_cut_with_it(tmp_path):
    ready, got, worker_got = tmp_path / "ready", tmp_path / "got", tmp_path / "wgot"
    status, text, _report = run_cut_at_mark(
        tmp_path, ["controller", ready, got, worker_got], ready, grace=ROOMY_GRACE_S)
    assert status == STATUS_CUT, text
    assert got.exists(), "the controller never took the interrupt:\n" + text
    assert not worker_got.exists(), "the worker got the interrupt too:\n" + text


@needs_the_job_plugins
# @scenario SC-platform-job-cut-05
def test_a_cut_pytest_run_in_workers_keeps_its_junit_and_no_row_is_written_crashed(tmp_path):
    from test_row_clock import interrupted_run
    status, junit = interrupted_run(tmp_path, "INT", True)
    assert status == STATUS_CUT, status
    assert junit.exists(), "the interrupt left no junit"
    text = junit.read_text()
    assert "test_a_quick_row_before_the_hang" in text
    assert "crashed while running" not in text, "a row in flight was written as crashed"


# @scenario SC-platform-job-cut-06
def test_a_run_that_ignores_the_cut_is_killed_at_the_grace_and_the_report_names_it(tmp_path):
    ready = tmp_path / "ready"
    status, text, report = run_cut_at_mark(tmp_path, ["ignore-interrupt", ready], ready)
    assert status == STATUS_KILLED, "the status is an exit code, not a signal: %s\n%s" % (status, text)
    assert report["cut"] is True and report["killed"] is True, report
    assert report["exit"] == STATUS_KILLED, report
    command = pids_in(ready)[0]
    stuck = {entry["pid"]: entry for entry in report["stuck"]}
    assert command in stuck, "the report does not name the run's process: %s" % report
    assert int(stuck[command]["SigIgn"], 16) & SIGINT_BIT, stuck[command]


# @scenario SC-platform-job-cut-07
def test_nothing_of_a_killed_run_is_left_whatever_group_it_is_in(tmp_path):
    pids = tmp_path / "pids"
    status, text, _report = run_cut_at_mark(
        tmp_path, ["family", pids], str(pids) + ".up")
    assert status == STATUS_KILLED, text
    family = pids_in(pids)
    assert len(family) == 3, family
    left = [pid for pid in family if gui_owner.process_running(pid)]
    assert not left, "processes of the run outlived the tool: %s" % left


# @scenario SC-platform-job-cut-14
def test_a_run_that_keeps_forking_while_it_is_killed_is_ended_within_a_bound(tmp_path):
    pids = tmp_path / "pids"
    status, text, report = run_cut_at_mark(
        tmp_path, ["burst", pids], str(pids) + ".up")
    assert status == STATUS_KILLED, text
    assert report["elapsed_s"] < MARK_WAIT_S + int(GRACE_S) + KILL_BOUND_MARGIN_S, report
    assert "unkilled" not in report, report
    family = pids_in(pids)
    assert len(family) > 1, "the run never forked: %s" % family
    left = [pid for pid in family if gui_owner.process_running(pid)]
    assert not left, "processes of the run outlived the tool: %s" % left


# @scenario SC-platform-job-cut-15
def test_the_kill_gives_up_after_its_bound_and_names_what_it_could_not_end(tmp_path):
    """A process the listing names and no signal ends must not hold the tool:
    the kill waits for it at most its bound and the report names it."""
    done = gui_owner.run_nested(
        [sys.executable, INNER, "kill-bound", KILL_BOUND_TEST_S], TOOL_TIMEOUT_S,
        cwd=str(tmp_path))
    text = done.stdout.decode("utf-8", "replace")
    assert done.returncode == 0, text
    assert "unkilled=True" in text, text
    elapsed = float(text.split("elapsed=")[1].split()[0])
    assert float(KILL_BOUND_TEST_S) <= elapsed < float(KILL_BOUND_TEST_S) + 3, text


# @scenario SC-platform-job-cut-08
def test_a_run_that_dies_of_a_signal_of_its_own_before_the_deadline_is_not_a_cut(tmp_path):
    status, text, report = run_to_the_end(tmp_path, ["die-by-kill"])
    assert status == STATUS_KILLED, text
    assert report["cut"] is False and report["killed"] is False, report
    assert report["command_exit"] == -signal.SIGKILL, report
    written = tmp_path / "shard-0-status.json"
    suite_plan.cmd_status(argparse.Namespace(
        out=str(written), shard=0, exit=status, started="a", ended="b", ids=3,
        cut_report=str(tmp_path / "report.json")))
    status_file = json.loads(written.read_text())
    assert status_file["cut"] is False, status_file
    reason = suite_plan.shard_reason(0, {0: status_file})
    assert "was cut" not in reason and "ended (exit 137)" in reason, reason


@pytest.mark.parametrize("code,says", [(137, "cut at 100 min and killed"),
                                       (124, "was cut at 100 min")])
def test_a_status_with_no_word_on_a_kill_is_read_by_its_exit_code(code, says):
    """A status written before the tool said whether it killed holds `cut`
    and no `killed`: exit 137 is a kill, never a cut that ended in its grace."""
    status = {"shard": 0, "exit": code, "cut": True, "cut_min": 100}
    reason = suite_plan.shard_reason(0, {0: status})
    assert says in reason, reason
    assert (code == 137) == ("killed" in reason), reason


# @scenario SC-platform-job-cut-09
def test_a_terminated_tool_cuts_its_run_with_the_full_grace_and_the_junit_is_kept(tmp_path):
    ready, got = tmp_path / "ready", tmp_path / "got"
    owned = start_tool(tmp_path, ["take-interrupt", ready, got], ready)
    watch = gui_owner.watch_process(owned.pid)
    try:
        watch.signal(signal.SIGTERM)
    finally:
        watch.close()
    status = end_tool(owned)
    assert status == STATUS_CUT, status
    assert got.exists(), "the signal to the tool never reached the run as the cut"
    assert read_report(tmp_path / "report.json")["cut_by"] == "signalled"


# @scenario SC-platform-job-cut-10
def test_a_run_does_not_outlive_a_tool_that_was_killed(tmp_path):
    ready = tmp_path / "ready"
    owned = start_tool(tmp_path, ["ignore-interrupt", ready], ready)
    command = pids_in(ready)[0]
    watch = gui_owner.watch_process(owned.pid)
    try:
        watch.signal(signal.SIGKILL)
    finally:
        watch.close()
    end_tool(owned)
    gui_owner.wait_for(lambda: not gui_owner.process_running(command),
                       "the run to end with its tool", MARK_WAIT_S)


# @scenario SC-platform-job-cut-11
@pytest.mark.parametrize("duration", ["", "0", "1h", "-5", "1.5m", "abc", "5x"])
def test_a_duration_that_is_no_deadline_is_refused_and_nothing_is_run(tmp_path, duration):
    ready, got = tmp_path / "ready", tmp_path / "got"
    argv = [sys.executable, JOB_CUT, "--after", duration, "--", sys.executable,
            INNER, "take-interrupt", str(ready), str(got)]
    done = gui_owner.run_nested(argv, TOOL_TIMEOUT_S, cwd=str(tmp_path))
    assert done.returncode == USAGE_ERROR, done.stdout
    assert not ready.exists(), "a run was started with no usable deadline"


def test_the_durations_the_tool_accepts_are_seconds_and_minutes():
    assert job_cut.parse_duration("90s") == 90
    assert job_cut.parse_duration("100m") == 6000
    assert job_cut.parse_duration("120") == 120
    assert job_cut.parse_duration("0", allow_zero=True) == 0
    with pytest.raises(ValueError):
        job_cut.parse_duration("0")


# @scenario SC-platform-job-cut-12
def test_a_command_that_is_not_there_ends_127_as_a_shell_says(tmp_path):
    argv = [sys.executable, JOB_CUT, "--after", "10s", "--", str(tmp_path / "nothing")]
    done = gui_owner.run_nested(argv, TOOL_TIMEOUT_S, cwd=str(tmp_path))
    assert done.returncode == 127, done.stdout


@needs_the_job_plugins
# @scenario SC-platform-job-cut-13
def test_the_cut_of_a_run_holding_an_editor_ends_the_editor(tmp_path):
    started = tmp_path / "started"
    inner = "%s::test_a_row_holding_an_editor_waits_forever" % INNER_ROWS
    home = tmp_path / "home"
    home.mkdir()
    argv = [sys.executable, JOB_CUT, "--after", NET_AFTER, "--grace", "60", "--",
            sys.executable, "-m", "pytest", "-q", "-p", "no:cacheprovider",
            "--basetemp", str(tmp_path / "inner"), inner]
    done = gui_owner.run_nested_cut_on_mark(
        argv, TOOL_TIMEOUT_S, str(started), MARK_WAIT_S, cwd=str(tmp_path),
        extra_env={"HOME": str(home), "TERM": "xterm",
                   gui_owner.ROW_BUDGET_VAR: "600",
                   "XWPE_INNER_STARTED_FILE": str(started)})
    text = done.stdout.decode("utf-8", "replace")
    assert done.returncode == STATUS_CUT, text[-1500:]
    assert " error" not in text.splitlines()[-1], "the editor was left behind:\n" + text[-1500:]
