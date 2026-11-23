"""The plan of the Linux suite (tests/harness/suite_plan.py).

The suite no longer runs as one job: a record of how long each row took
places every collected row in one of a few jobs, gives each row a budget, and
fails a build when the suite stops fitting.  These rows pin each of those
with made-up records and ids, so they need no collection of the real suite
and no CI; the commands run as the workflow runs them, as child processes.
"""
import json
import os
import sys

import pytest

import gui_owner

HERE = os.path.dirname(os.path.abspath(__file__))
HARNESS = os.path.join(HERE, "harness")
SUITE_PLAN = os.path.join(HARNESS, "suite_plan.py")
sys.path.insert(0, HARNESS)
try:
    import suite_plan
finally:
    sys.path.pop(0)

RUN_TIMEOUT_S = 120
SECONDS_PER_MIN = 60


def ids_of(*files, rows=3):
    """Node ids of @rows rows in each of @files."""
    return ["tests/%s.py::test_%d" % (name, n) for name in files for n in range(rows)]


def record_of(seconds_by_row, host_kind=suite_plan.HOST_RUNNER, scale=3):
    """A record holding @seconds_by_row (node id -> seconds)."""
    record = suite_plan.fake_record(seconds_by_row, host_kind)
    record["scale"] = scale
    return record


def run_cli(tmp_path, *args):
    """Run suite_plan.py as the workflow does; returns (status, text)."""
    argv = [sys.executable, SUITE_PLAN] + [str(a) for a in args]
    done = gui_owner.run_nested(argv, RUN_TIMEOUT_S, cwd=str(tmp_path),
                                extra_env={"HOME": str(tmp_path)})
    return done.returncode, (done.stdout + done.stderr).decode("utf-8", "replace")


def junit_text(cases, host="runner-1"):
    """A pytest junit file with @cases: (classname, name, seconds, outcome)."""
    body = []
    for classname, name, seconds, outcome in cases:
        inner = {"passed": "", "failed": '<failure message="boom">x</failure>',
                 "skipped": '<skipped type="pytest.skip" message="no gdb"/>',
                 "cut": '<failure message="RowOverBudget: ran past">x</failure>'}[outcome]
        body.append('<testcase classname="%s" name="%s" time="%s">%s</testcase>'
                    % (classname, name, seconds, inner))
    return ('<?xml version="1.0"?><testsuites><testsuite name="pytest" hostname="%s">'
            '%s</testsuite></testsuites>' % (host, "".join(body)))


# @scenario SC-platform-suite-plan-01
def test_a_new_file_that_the_record_never_saw_lands_in_a_job(tmp_path):
    """A test file added tomorrow is collected, estimated and placed: nothing
    in the workflow names it."""
    known = ids_of("test_a", "test_b")
    record = record_of({node: 30 for node in known})
    ids = known + ids_of("test_brand_new")
    plan = suite_plan.make_plan(record, ids, 4, 60, 4)
    for node in ids:
        assert node in plan["assignment"], node
    placed = sorted(plan["assignment"])
    assert placed == sorted(ids), "a row was placed twice or dropped"


# @scenario SC-platform-suite-plan-02
def test_a_file_over_half_a_job_is_split_into_its_rows():
    """One huge file must not decide the largest job: its rows are dealt on."""
    big = ["tests/test_big.py::test_%d" % n for n in range(80)]
    record = record_of({node: 300 for node in big})   # 100 min of four workers
    plan = suite_plan.make_plan(record, big, 8, 60, 4)
    shards_with_big = {plan["assignment"][node] for node in big}
    assert len(shards_with_big) > 1, "the file was kept whole: %s" % [
        (s["index"], s["ids"], s["predicted_min"]) for s in plan["shards"]]
    assert plan["fits"]


def test_a_file_under_half_a_job_stays_whole():
    small = ids_of("test_small", rows=5)
    record = record_of({node: 10 for node in small})
    plan = suite_plan.make_plan(record, small + ids_of("test_other"), 4, 60, 4)
    assert len({plan["assignment"][node] for node in small}) == 1


def test_the_jobs_are_the_fewest_that_end_inside_the_target():
    ids = ids_of("test_a", "test_b", "test_c", "test_d", rows=4)
    # 16 rows of 15 min: 240 min of work in 4 workers is 60 min in one job.
    record = record_of({node: 15 * SECONDS_PER_MIN for node in ids})
    plan = suite_plan.make_plan(record, ids, 8, 60, 4)
    assert plan["jobs"] == 1 and plan["fits"], plan["shards"]
    record = record_of({node: 16 * SECONDS_PER_MIN for node in ids})
    plan = suite_plan.make_plan(record, ids, 8, 60, 4)
    assert plan["jobs"] == 2, plan["shards"]


def test_a_row_with_no_time_gets_the_median_of_its_file():
    ids = ids_of("test_a", rows=4) + ["tests/test_a.py::test_unrecorded"]
    record = record_of({ids[0]: 10, ids[1]: 20, ids[2]: 30, ids[3]: 40})
    estimate = suite_plan.Estimator(record, ids, 1.0)
    assert estimate.seconds("tests/test_a.py::test_unrecorded") == 25
    assert estimate.seconds("tests/test_new.py::test_x") == 25, "the median of the record"


# @scenario SC-platform-suite-plan-03
def test_a_seed_record_is_stretched_for_the_runner_and_a_runner_record_is_not():
    ids = ids_of("test_a", rows=4)
    seed = record_of({node: 60 for node in ids}, host_kind=suite_plan.HOST_SEED)
    runner = record_of({node: 60 for node in ids}, host_kind=suite_plan.HOST_RUNNER)
    seed_min = suite_plan.make_plan(seed, ids, 1, 600, 1)["shards"][0]["predicted_min"]
    runner_min = suite_plan.make_plan(runner, ids, 1, 600, 1)["shards"][0]["predicted_min"]
    assert runner_min == 4.0
    assert seed_min == runner_min * suite_plan.SEED_HOST_FACTOR


# @scenario SC-platform-suite-plan-04
def test_a_junit_classname_is_resolved_to_a_file_or_a_class_against_the_collection():
    known = {"tests/console/test_console_tty_predicate.py::test_a",
             "tests/test_dialog_resize.py::TestDialogResizeSurvival::test_b[x]"}
    assert suite_plan.resolve_id("tests.console.test_console_tty_predicate",
                                 "test_a", known) == "tests/console/test_console_tty_predicate.py::test_a"
    assert suite_plan.resolve_id("tests.test_dialog_resize.TestDialogResizeSurvival",
                                 "test_b[x]", known) == \
        "tests/test_dialog_resize.py::TestDialogResizeSurvival::test_b[x]"
    assert suite_plan.resolve_id("tests.test_gone", "test_c", known) is None


# @scenario SC-platform-suite-plan-05
def test_the_record_keeps_the_old_time_of_a_row_a_run_cut_failed_skipped_or_never_reached(tmp_path):
    """Learning from a cut would give the next run three times the budget the
    cut row had, up to the ceiling."""
    known = ["tests/test_x.py::test_%s" % n for n in ("pass", "cut", "fail", "skip", "gone")]
    old = record_of({node: 100 for node in known})
    old["rows"]["tests/test_x.py::test_pass"] = [100, "old"]
    previous = tmp_path / "old.json"
    suite_plan.save_record(old, str(previous))
    junit = tmp_path / "run.xml"
    junit.write_text(junit_text([
        ("tests.test_x", "test_pass", 7, "passed"),
        ("tests.test_x", "test_cut", 3000, "cut"),
        ("tests.test_x", "test_fail", 1, "failed"),
        ("tests.test_x", "test_skip", 0.01, "skipped")]))
    ids = tmp_path / "ids.txt"
    ids.write_text("\n".join(known) + "\n")
    out = tmp_path / "new.json"
    status, text = run_cli(tmp_path, "record", junit, out, "--previous", previous,
                           "--collected", ids, "--host-kind", "runner", "--source", "run-9")
    assert status == 0, text
    rows = suite_plan.load_record(str(out))["rows"]
    assert rows["tests/test_x.py::test_pass"] == [7, "run-9"]
    for kept in ("cut", "fail", "skip", "gone"):
        assert rows["tests/test_x.py::test_%s" % kept] == [100, "fake"], kept


def test_a_row_that_was_never_recorded_is_not_made_up_by_a_cut(tmp_path):
    junit = tmp_path / "run.xml"
    junit.write_text(junit_text([("tests.test_x", "test_cut", 3000, "cut"),
                                 ("tests.test_x", "test_skip", 0.01, "skipped")]))
    out = tmp_path / "new.json"
    status, text = run_cli(tmp_path, "record", junit, out)
    assert status == 0, text
    assert suite_plan.load_record(str(out))["rows"] == {}


def test_the_record_says_what_host_and_scale_it_was_made_at(tmp_path):
    junit = tmp_path / "run.xml"
    junit.write_text(junit_text([("tests.test_x", "test_a", 5, "passed")], host="vm-7"))
    out = tmp_path / "new.json"
    status, text = run_cli(tmp_path, "record", junit, out, "--scale", "3",
                           "--settle-ms", "400", "--host-kind", "runner")
    assert status == 0, text
    record = suite_plan.load_record(str(out))
    assert (record["host"], record["host_kind"], record["scale"], record["settle_ms"]) \
        == ("vm-7", "runner", 3, 400)


def test_unmatched_cases_are_counted_and_printed(tmp_path):
    junit = tmp_path / "run.xml"
    junit.write_text(junit_text([("tests.test_x", "test_a", 5, "passed"),
                                 ("tests.test_gone", "test_b", 5, "passed")]))
    ids = tmp_path / "ids.txt"
    ids.write_text("tests/test_x.py::test_a\n")
    status, text = run_cli(tmp_path, "record", junit, tmp_path / "o.json", "--collected", ids)
    assert status == 0 and "1 unmatched" in text, text
    assert "tests.test_gone::test_b" in text


# @scenario SC-platform-suite-plan-06
def test_a_job_runs_node_ids_with_no_selection_and_every_row_once(tmp_path):
    """The selection is applied once, in the build job's collection; a job
    that applied it again would leave out the mapped rows the selection
    drops, and the gate would call them not run."""
    ids = ids_of("test_a", "test_b", "test_c", rows=4)
    ids += ["tests/test_a.py::TestK::test_p[a b]"]
    idfile = tmp_path / "ids.txt"
    idfile.write_text("\n".join(ids) + "\n")
    recfile = tmp_path / "rec.json"
    suite_plan.save_record(record_of({node: 100 for node in ids}), str(recfile))
    plan = tmp_path / "out" / "plan.json"
    status, text = run_cli(tmp_path, "plan", plan, "--record", recfile,
                           "--collected", idfile, "--jobs-max", "3", "--target-min", "1")
    assert status == 0, text
    listed = json.loads(plan.read_text())
    seen = []
    for shard in listed["shards"]:
        lines = (plan.parent / shard["args"]).read_text().splitlines()
        assert all("::" in line and not line.startswith("-") for line in lines), lines
        assert len(lines) == shard["ids"]
        seen.extend(lines)
    assert sorted(seen) == sorted(ids)
    status, text = run_cli(tmp_path, "shards", plan)
    assert json.loads(text) == [s["index"] for s in listed["shards"]]


def test_pytest_reads_the_args_file_with_an_id_that_has_a_space(tmp_path):
    """`pytest @args` takes each line as one argument, a parametrized id with
    a blank in it included."""
    module = tmp_path / "test_spaced.py"
    module.write_text("import pytest\n\n"
                      "@pytest.mark.parametrize('v', ['a b', 'c'])\n"
                      "def test_p(v):\n    pass\n")
    args = tmp_path / "args.txt"
    args.write_text("%s::test_p[a b]\n" % module)
    done = gui_owner.run_nested(
        [sys.executable, "-m", "pytest", "-q", "-p", "no:cacheprovider",
         "--collect-only", "@%s" % args], RUN_TIMEOUT_S, cwd=str(tmp_path),
        extra_env={"HOME": str(tmp_path)})
    text = done.stdout.decode("utf-8", "replace")
    assert done.returncode == 0 and "test_p[a b]" in text and "test_p[c]" not in text, text


# @scenario SC-platform-suite-plan-07
def test_fit_refuses_an_over_full_record_and_a_four_hour_row_and_a_thin_record():
    ids = ids_of(*["test_f%d" % n for n in range(20)], rows=2)
    fits = record_of({node: 60 for node in ids})
    assert suite_plan.fit_problems(fits, ids) == []
    over_full = record_of({node: 3 * 3600 for node in ids})
    assert any("largest" in line for line in suite_plan.fit_problems(over_full, ids))
    one_long = record_of({node: 60 for node in ids})
    one_long["rows"][ids[0]] = [4 * 3600, "fake"]
    assert any("cannot fit" in line for line in suite_plan.fit_problems(one_long, ids))
    thin = record_of({node: 60 for node in ids[:20]})
    found = suite_plan.fit_problems(thin, ids)
    assert any("%d%%" % suite_plan.RECORD_COVER in line for line in found), found


def test_fit_names_its_numbers_on_the_command_line(tmp_path):
    ids = ids_of("test_a", rows=2)
    idfile = tmp_path / "ids.txt"
    idfile.write_text("\n".join(ids) + "\n")
    recfile = tmp_path / "rec.json"
    suite_plan.save_record(record_of({node: 5 * 3600 for node in ids}), str(recfile))
    status, text = run_cli(tmp_path, "fit", "--record", recfile, "--collected", idfile)
    assert status == 1 and "predicted at" in text and "60 min target" in text, text


def test_the_selftest_of_the_planner_passes():
    assert suite_plan.selftest()


def test_the_tracked_record_fits_the_ids_it_holds():
    """The record the tree carries is one `fit` accepts: it holds a time for
    every id it is asked about and no row longer than a job."""
    record = suite_plan.load_record(suite_plan.TRACKED_RECORD)
    assert record["rows"], "the tracked record is empty"
    assert record["host_kind"] == suite_plan.HOST_SEED
    ids = sorted(record["rows"])
    assert suite_plan.fit_problems(record, ids) == []


# @scenario SC-platform-row-clock-03
def test_the_budget_of_a_row_is_a_few_times_its_record_with_a_floor_and_a_ceiling():
    record = record_of({"a": 100, "tiny": 0.5, "huge": 10 ** 6}, scale=3)
    scale = 3
    assert suite_plan.row_budget_s(record, "a", scale) == 300
    assert suite_plan.row_budget_s(record, "tiny", scale) == suite_plan.ROW_FLOOR_S * scale
    assert suite_plan.row_budget_s(record, "huge", scale) == suite_plan.ROW_CEILING_S
    assert suite_plan.row_budget_s(record, "unknown", scale) == suite_plan.ROW_UNRECORDED_S * scale


def test_a_sanitizer_scale_stretches_the_budget_and_the_floor_keeps_it_above_the_record():
    record = record_of({"a": 100, "tiny": 1}, scale=3)
    assert suite_plan.row_budget_s(record, "a", 5) == 3 * 100 * 5 / 3
    assert suite_plan.row_budget_s(record, "tiny", 5) == suite_plan.ROW_FLOOR_S * 5


def test_the_host_factor_of_the_plan_is_not_in_the_budget():
    seed = record_of({"a": 100}, host_kind=suite_plan.HOST_SEED, scale=3)
    runner = record_of({"a": 100}, host_kind=suite_plan.HOST_RUNNER, scale=3)
    assert suite_plan.row_budget_s(seed, "a", 3) == suite_plan.row_budget_s(runner, "a", 3)


# @scenario SC-platform-suite-plan-08
def test_a_shard_that_ended_without_a_result_is_named_for_its_state():
    statuses = {0: {"shard": 0, "exit": 124, "cut_min": 100},
                1: {"shard": 1, "exit": 2, "cut_min": 100},
                2: {"shard": 2, "exit": 1, "cut_min": 100},
                4: {"shard": 4, "exit": 137, "cut_min": 100}}
    assert suite_plan.shard_reason(0, statuses) == "not run: shard 0 was cut at 100 min"
    assert suite_plan.shard_reason(1, statuses) == "shard 1 ended (exit 2) before running it"
    assert suite_plan.shard_reason(3, statuses) == "shard 3 uploaded nothing"
    assert "finished (exit 1)" in suite_plan.shard_reason(2, statuses)
    # `timeout -k` kills a shard the interrupt did not end: that is a cut too,
    # not a shard that ended on its own before running the row.
    killed = suite_plan.shard_reason(4, statuses)
    assert killed.startswith("not run: shard 4 was cut at 100 min"), killed


def test_a_status_file_keeps_the_cut_exit_and_never_turns_it_into_success(tmp_path):
    out = tmp_path / "shard-4-status.json"
    status, text = run_cli(tmp_path, "status", out, "--shard", "4", "--exit", "124",
                           "--started", "a", "--ended", "b", "--ids", "12")
    assert status == 0, text
    written = json.loads(out.read_text())
    assert written["exit"] == 124 and written["cut"] and written["ids"] == 12
    assert suite_plan.read_statuses(str(tmp_path))[4]["exit"] == 124


# @scenario SC-platform-suite-plan-09
def test_the_newest_record_of_the_branch_that_has_not_expired_is_taken():
    listing = {"artifacts": [
        {"id": 1, "name": "suite-timings", "expired": False, "created_at": "2026-11-20T10:00:00Z",
         "workflow_run": {"id": 11, "head_branch": "main"}},
        {"id": 2, "name": "suite-timings", "expired": False, "created_at": "2026-11-22T10:00:00Z",
         "workflow_run": {"id": 12, "head_branch": "main"}},
        {"id": 3, "name": "suite-timings", "expired": True, "created_at": "2026-11-23T10:00:00Z",
         "workflow_run": {"id": 13, "head_branch": "main"}},
        {"id": 4, "name": "suite-timings", "expired": False, "created_at": "2026-11-23T11:00:00Z",
         "workflow_run": {"id": 14, "head_branch": "other"}},
        {"id": 5, "name": "linux-test-logs", "expired": False, "created_at": "2026-11-23T12:00:00Z",
         "workflow_run": {"id": 15, "head_branch": "main"}}]}
    assert suite_plan.newest_artifact(listing, "main")["id"] == 2
    assert suite_plan.newest_artifact(listing, "none") is None


GH_STUB = '''#!/bin/sh
# a stand-in for `gh api`: the artifact list, then the zip of the record
case "$2" in
  *artifacts?name=*) cat "$STUB_LISTING" ;;
  *zip) cat "$STUB_ZIP" ;;
  *) exit 1 ;;
esac
'''


def test_fetch_downloads_the_record_through_gh_and_falls_back_to_the_tracked_one(tmp_path):
    import zipfile
    record = record_of({"tests/test_x.py::test_a": 9}, scale=3)
    inner = tmp_path / "suite-timings.json"
    suite_plan.save_record(record, str(inner))
    archive = tmp_path / "a.zip"
    with zipfile.ZipFile(archive, "w") as bundle:
        bundle.write(inner, "suite-timings.json")
    listing = tmp_path / "list.json"
    listing.write_text(json.dumps({"artifacts": [
        {"id": 7, "name": "suite-timings", "expired": False,
         "created_at": "2026-11-22T10:00:00Z",
         "workflow_run": {"id": 21, "head_branch": "main"}}]}))
    bindir = tmp_path / "bin"
    bindir.mkdir()
    gh = bindir / "gh"
    gh.write_text(GH_STUB)
    gh.chmod(0o755)
    out = tmp_path / "fetched.json"
    env = {"HOME": str(tmp_path), "STUB_LISTING": str(listing), "STUB_ZIP": str(archive),
           "PATH": "%s:%s" % (bindir, os.environ["PATH"])}
    argv = [sys.executable, SUITE_PLAN, "fetch", "--repo", "o/r", "--branch", "main",
            "--out", str(out)]
    done = gui_owner.run_nested(argv, RUN_TIMEOUT_S, cwd=str(tmp_path), extra_env=env)
    assert done.returncode == 0, done.stdout + done.stderr
    assert suite_plan.load_record(str(out))["rows"]["tests/test_x.py::test_a"][0] == 9
    miss = tmp_path / "missing.json"
    argv[-1] = str(miss)
    argv[argv.index("main")] = "no-such-branch"
    done = gui_owner.run_nested(argv, RUN_TIMEOUT_S, cwd=str(tmp_path), extra_env=env)
    assert done.returncode == 0 and not miss.exists(), done.stdout


def collect_in(tmp_path, monkeypatch, files, *arguments):
    """suite_plan.run_collect over a tree of @files {name: text} in tmp_path."""
    for name, text in files.items():
        (tmp_path / name).write_text(text)
    monkeypatch.setattr(suite_plan, "TOP", str(tmp_path))
    return suite_plan.run_collect(list(arguments) or ["."])


def test_a_file_that_does_not_collect_fails_the_collection_and_says_why(tmp_path, monkeypatch):
    """A file that cannot be imported drops all its rows from the plan; the
    plan would then look smaller and green.  pytest's exit 2 and its words
    must reach the person who ran the collection."""
    files = {"test_good.py": "def test_a():\n    pass\n",
             "test_bad.py": "import no_such_module_for_the_plan\n"
                            "def test_b():\n    pass\n"}
    with pytest.raises(suite_plan.CollectionError) as err:
        collect_in(tmp_path, monkeypatch, files)
    assert "exit 2" in str(err.value) and "no_such_module_for_the_plan" in str(err.value)


def test_a_collection_with_no_match_is_empty_and_a_good_one_lists_its_rows(tmp_path, monkeypatch):
    files = {"test_good.py": "def test_a():\n    pass\n"}
    assert collect_in(tmp_path, monkeypatch, files) == ["test_good.py::test_a"]
    assert collect_in(tmp_path, monkeypatch, {}, ".", "-k", "no_row_has_this_name") == []


def test_the_collect_command_exits_1_naming_the_error_when_a_file_does_not_collect(tmp_path, monkeypatch):
    """The command the build job runs: a non-zero status and the error on stderr."""
    (tmp_path / "tests").mkdir()
    (tmp_path / "tests" / "test_bad.py").write_text(
        "raise RuntimeError('planted collection error')\n")
    monkeypatch.setattr(suite_plan, "TOP", str(tmp_path))
    monkeypatch.setattr(suite_plan, "SELECTION", "not x")
    monkeypatch.setattr(suite_plan, "SUITE_IGNORES", ())
    monkeypatch.setattr(suite_plan, "mapped_ncurses_ids", lambda: [])
    assert suite_plan.main(["collect", str(tmp_path / "ids.txt")]) == 1
    assert not (tmp_path / "ids.txt").exists()


def test_the_tool_children_file_collects_with_no_binary_built(tmp_path):
    """Collecting a tree before the build must not open ./wpe: the drift guard
    and the plan read the collection."""
    import subprocess
    env = dict(os.environ, WPE_BIN=str(tmp_path / "no-such-wpe"))
    done = subprocess.run(
        [sys.executable, "-m", "pytest", "--collect-only", "-q", "-p", "no:cacheprovider",
         os.path.join(HERE, "test_ai_tool_children.py")],
        cwd=os.path.dirname(HERE), env=env, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, timeout=RUN_TIMEOUT_S)
    assert done.returncode == 0, done.stdout[-1500:]
    assert "::" in done.stdout


def junit_with_reruns():
    """A junit as pytest-rerunfailures writes it: every attempt is a testcase,
    and the attempts it retried carry no child."""
    return ('<?xml version="1.0"?><testsuites><testsuite name="pytest" hostname="r">'
            '<testcase classname="tests.test_x" name="test_always" time="1.5"/>'
            '<testcase classname="tests.test_x" name="test_always" time="1.5"/>'
            '<testcase classname="tests.test_x" name="test_always" time="1.501">'
            '<failure message="boom">x</failure></testcase>'
            '<testcase classname="tests.test_x" name="test_flaky" time="9"/>'
            '<testcase classname="tests.test_x" name="test_flaky" time="4"/>'
            '<testcase classname="tests.test_x" name="test_xfail" time="1.2"/>'
            '<testcase classname="tests.test_x" name="test_xfail" time="1.2"/>'
            '<testcase classname="tests.test_x" name="test_xfail" time="60">'
            '<failure message="RowOverBudget: ran past">x</failure></testcase>'
            '</testsuite></testsuites>')


def test_a_row_rerun_and_failed_in_the_end_keeps_its_old_time_and_a_flaky_one_learns_the_last(tmp_path):
    """The retried attempts of a row that always fails read as passed and
    taught the record 1.5 s for a row that never passes."""
    known = ["tests/test_x.py::test_%s" % n for n in ("always", "flaky", "xfail")]
    old = record_of({node: 100 for node in known})
    previous = tmp_path / "old.json"
    suite_plan.save_record(old, str(previous))
    junit = tmp_path / "run.xml"
    junit.write_text(junit_with_reruns())
    ids = tmp_path / "ids.txt"
    ids.write_text("\n".join(known) + "\n")
    out = tmp_path / "new.json"
    status, text = run_cli(tmp_path, "record", junit, out, "--previous", previous,
                           "--collected", ids, "--host-kind", "runner", "--source", "run-9")
    assert status == 0, text
    rows = suite_plan.load_record(str(out))["rows"]
    assert rows["tests/test_x.py::test_always"] == [100, "fake"]
    assert rows["tests/test_x.py::test_xfail"] == [100, "fake"]
    assert rows["tests/test_x.py::test_flaky"] == [4, "run-9"]


def test_fetch_stretches_a_record_made_at_a_shorter_settle_and_leaves_the_rest():
    record = record_of({"tests/test_x.py::test_a": 10})
    record["settle_ms"] = 200
    stretched = suite_plan.normalise_settle(record, 400)
    assert stretched["rows"]["tests/test_x.py::test_a"][0] == 20 and stretched["settle_ms"] == 400
    assert suite_plan.normalise_settle(record, 100)["rows"]["tests/test_x.py::test_a"][0] == 10
    record["settle_ms"] = 0
    assert suite_plan.normalise_settle(record, 400)["rows"]["tests/test_x.py::test_a"][0] == 10


def test_the_drift_guard_runs_in_the_job_that_built_the_tree():
    """On an unbuilt checkout rows fail to collect and read as drift."""
    with open(os.path.join(os.path.dirname(HERE), ".github", "workflows", "linux.yml"),
              encoding="utf-8") as fh:
        jobs = fh.read().split("\njobs:", 1)[1]
    build = jobs.split("\n  pyte:", 1)[0]
    gate = jobs.split("\n  gate:", 1)[1]
    assert "harness.py check" in build and "harness.py check" not in gate
