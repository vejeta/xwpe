"""What the scenario harness reads a skip as (tests/harness/harness.py).

A row that skips is one of: a tool the host does not have and the manifest
row declares (BLOCKED), a tool the job installed that does not work (FAIL),
a recorded incoherence or an unapproved golden the manifest row declares
(KNOWN), or anything else (FAIL).  These rows pin the one classifier,
verdict(), the junit reading that feeds it, the drift rule between a row's
code and its manifest row, and the marker that keeps a crash from hiding in
a known row.  They take the host's real tools out of the question: every
input is a made-up scenario or a small junit file.
"""
import os
import subprocess
import sys

import pytest

import gui_owner
import host_tools
import wpe_driver

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "harness"))
try:
    import harness
finally:
    sys.path.pop(0)

INNER_TIMEOUT_S = 120
XFAIL = harness.XFAIL_TYPE
SKIP = harness.SKIP_TYPE
INCOHERENT = "INCOHERENCE: after a resize from 80x30 to 45x15 a '(' is left"
GOLDEN = "golden ctrl_y waits for the user's approval (goldens/APPROVAL.md)"


def scenario(**fields):
    """A manifest row of the ncurses layer, with `fields` set on it."""
    row = {"id": "SC-x-01", "status": "new", "layer": "ncurses", "test": "tests/t.py::t"}
    row.update(fields)
    return row


def test_a_skip_that_names_a_declared_tool_reads_blocked_on_a_host_without_it():
    row = scenario(needs=["gdb"])
    assert harness.verdict(row, "SKIP", ["no gdb on this host"], [SKIP]) == "BLOCKED"


def test_the_same_skip_reads_fail_on_a_job_that_installed_the_tool():
    row = scenario(needs=["gdb"])
    verdict = harness.verdict(row, "SKIP", ["no gdb on this host"], [SKIP],
                              expected=["gdb", "dlv"])
    assert verdict == "FAIL"


def test_an_installed_tool_of_another_row_does_not_fail_this_one():
    row = scenario(needs=["scala-cli"])
    verdict = harness.verdict(row, "SKIP", ["no scala-cli on this host"], [SKIP],
                              expected=["scala", "scalac"])
    assert verdict == "BLOCKED"


def test_a_recorded_incoherence_reads_known_when_its_row_declares_the_text():
    row = scenario(known="INCOHERENCE: after a resize from 80x30")
    assert harness.verdict(row, "SKIP", [INCOHERENT], [XFAIL]) == "KNOWN"


def test_an_incoherence_the_row_does_not_declare_reads_fail():
    assert harness.verdict(scenario(), "SKIP", [INCOHERENT], [XFAIL]) == "FAIL"


def test_a_row_that_never_ran_is_not_a_recorded_incoherence():
    """The same text on a skip (the row did not run) is not the xfail the
    manifest declared: a row cannot become known by writing the marker."""
    row = scenario(known="INCOHERENCE: after a resize from 80x30")
    assert harness.verdict(row, "SKIP", [INCOHERENT], [SKIP]) == "FAIL"


def test_an_unapproved_golden_reads_known_only_as_a_skip():
    row = scenario(known="waits for the user's approval (goldens/APPROVAL.md)")
    assert harness.verdict(row, "SKIP", [GOLDEN], [SKIP]) == "KNOWN"
    assert harness.verdict(row, "SKIP", [GOLDEN], [XFAIL]) == "FAIL"


def test_a_skip_naming_an_installed_tool_is_fail_even_when_it_matches_known():
    """The verdict order: a tool the job installed that does not work is the
    image's failure and is read before any declared known state."""
    row = scenario(needs=["gdb"], known="INCOHERENCE: no gdb")
    reasons = ["INCOHERENCE: no gdb on this host"]
    assert harness.verdict(row, "SKIP", reasons, [XFAIL], expected=["gdb"]) == "FAIL"
    assert harness.verdict(row, "SKIP", reasons, [XFAIL]) == "KNOWN"


def test_a_bare_incoherence_prefix_is_refused_as_a_known_text():
    manifests = {"cap": {"scenarios": [
        scenario(id="SC-a", known="INCOHERENCE:"),
        scenario(id="SC-b", known="INCOHERENCE:   "),
        scenario(id="SC-c", known=INCOHERENT)]}}
    problems = harness.known_problems(manifests)
    assert len(problems) == 2 and "SC-a" in problems[0] and "SC-b" in problems[1], problems


def test_a_known_text_must_be_in_every_skip_of_the_row():
    row = scenario(known="INCOHERENCE: after a resize from 80x30")
    reasons = [INCOHERENT, "INCOHERENCE: something else entirely"]
    assert harness.verdict(row, "SKIP", reasons, [XFAIL, XFAIL]) == "FAIL"


def test_known_passes_the_gate_like_blocked_and_fails_nothing_else():
    assert "KNOWN" in harness.PASSING and "BLOCKED" in harness.PASSING
    assert "FAIL" not in harness.PASSING and "NOT_EXERCISED" not in harness.PASSING
    order = sorted(harness.SEVERITY, key=harness.SEVERITY.get)
    assert order == ["FAIL", "MISSING", "NOT_EXERCISED", "KNOWN", "BLOCKED", "PASS"]


def test_a_known_text_that_is_neither_an_incoherence_nor_an_approval_is_refused():
    manifests = {"cap": {"scenarios": [
        scenario(id="SC-a", known="the row is flaky"),
        scenario(id="SC-b", known=INCOHERENT),
        scenario(id="SC-c", known=GOLDEN)]}}
    problems = harness.known_problems(manifests)
    assert len(problems) == 1 and "SC-a" in problems[0], problems


JUNIT = """<?xml version="1.0" encoding="utf-8"?>
<testsuites><testsuite name="pytest" tests="2">
<testcase classname="tests.t" name="test_x"><skipped type="pytest.xfail" message="%s"/></testcase>
<testcase classname="tests.t" name="test_y"><skipped type="pytest.skip" message="no gdb on this host">skipped</skipped></testcase>
</testsuite></testsuites>
""" % INCOHERENT


def test_the_junit_reading_keeps_an_xfail_apart_from_a_skip(tmp_path):
    path = tmp_path / "junit.xml"
    path.write_text(JUNIT)
    reasons, kinds = {}, {}
    harness.junit_results(str(path), reasons, kinds)
    assert kinds[("tests.t", "test_x")] == XFAIL
    assert kinds[("tests.t", "test_y")] == SKIP
    assert INCOHERENT in reasons[("tests.t", "test_x")]


def test_a_row_whose_code_needs_a_tool_its_manifest_row_lacks_is_named():
    manifests = {"cap": {"scenarios": [
        scenario(id="SC-a", test="tests/t.py::t_gdb", needs=["python3"]),
        scenario(id="SC-b", test="tests/t.py::t_both", needs=["gdb", "dlv"])]}}
    node_needs = {"tests/t.py::t_gdb": ["gdb"], "tests/t.py::t_both": ["gdb", "dlv"]}
    problems = harness.needs_drift(manifests, node_needs)
    assert len(problems) == 1 and "SC-a" in problems[0] and "gdb" in problems[0], problems


def test_a_tool_every_host_has_is_not_a_drift():
    manifests = {"cap": {"scenarios": [scenario(test="tests/t.py::t", needs=[])]}}
    node_needs = {"tests/t.py::t": ["gcc", "git"]}
    assert harness.needs_drift(manifests, node_needs, exempt={"gcc", "git"}) == []
    assert len(harness.needs_drift(manifests, node_needs)) == 1


def test_an_alternative_is_declared_by_any_of_its_names():
    manifests = {"cap": {"scenarios": [scenario(test="tests/t.py::t", needs=["pylsp"])]}}
    assert harness.needs_drift(manifests, {"tests/t.py::t": ["pyright-langserver|pylsp"]}) == []


def test_a_parametrized_node_is_covered_by_the_row_that_names_its_test():
    assert harness.row_covers("tests/t.py::t", "tests/t.py::t[a-b]")
    assert harness.row_covers("tests/t.py", "tests/t.py::t")
    assert not harness.row_covers("tests/t.py::t", "tests/t.py::t_other")


FILE_ROW = "tests/t.py"
FILE_CASES = {("tests.t", "test_a"): "PASS",
              ("tests.t", "test_b[one]"): "PASS",
              ("tests.t", "test_b[two]"): "PASS",
              ("tests.t.TestC", "test_d"): "PASS"}


def test_a_row_that_names_a_whole_file_reads_every_case_of_it():
    """A manifest row may name a file and no test: it then reads every
    testcase of that file, so it passes only when they all did, fails when any
    one failed, and is MISSING (None) only when none of them ran.  A path in
    the name slot, which junit_key gives a bare file, is no testcase's name,
    so 13 such rows of SC-config-14 read MISSING over files that ran green."""
    assert harness.junit_lookup(FILE_CASES, FILE_ROW) == "PASS"
    one_failed = dict(FILE_CASES)
    one_failed[("tests.t", "test_e")] = "FAIL"
    assert harness.junit_lookup(one_failed, FILE_ROW) == "FAIL"
    assert harness.junit_lookup(FILE_CASES, "tests/never_ran.py") is None


def test_a_row_that_names_a_whole_file_holds_no_other_file():
    """The file's own cases only: not a file whose module begins with the same
    letters, and not a case of another row's test."""
    other = {("tests.too", "test_a"): "FAIL", ("tests.u", "test_a"): "FAIL"}
    both = dict(FILE_CASES)
    both.update(other)
    assert harness.junit_lookup(both, FILE_ROW) == "PASS"
    assert harness.junit_lookup(other, FILE_ROW) is None


def test_a_row_that_names_a_whole_file_reads_its_skips():
    """A file that skipped reads SKIP, with the skip messages of its cases, so
    the classifier still decides between a declared absence and a gap."""
    skipped = {("tests.t", "test_a"): "SKIP", ("tests.t", "test_b"): "SKIP"}
    reasons = {("tests.t", "test_a"): "no gdb on this host",
               ("tests.t", "test_b"): "no dlv on this host"}
    assert harness.junit_lookup(skipped, FILE_ROW) == "SKIP"
    assert sorted(harness.junit_skip_reasons(reasons, FILE_ROW)) == sorted(reasons.values())
    assert harness.verdict(scenario(test=FILE_ROW), "SKIP",
                           ["no gdb on this host"], [SKIP]) == "FAIL"
    assert harness.verdict(scenario(test=FILE_ROW, needs=["gdb"]), "SKIP",
                           ["no gdb on this host"], [SKIP]) == "BLOCKED"


INCOHERENT_ROWS = os.path.join(HERE, "one_row_incoherent.py")


def _incoherent_row_run(tmp_path, row):
    """Run one row of tests/one_row_incoherent.py (marked with incoherence())
    in a child pytest under this tree's conftest; return its exit status and
    text."""
    home = tmp_path / "home"
    home.mkdir()
    argv = [sys.executable, "-m", "pytest", "-q", "-p", "no:cacheprovider", "-rxX",
            "--basetemp", str(tmp_path / "inner"), "%s::%s" % (INCOHERENT_ROWS, row)]
    done = gui_owner.run_nested(argv, INNER_TIMEOUT_S, cwd=str(tmp_path),
                                extra_env={"HOME": str(home), "TERM": "xterm"})
    return done.returncode, done.stdout.decode("utf-8", "replace")


def test_a_failed_assertion_in_an_incoherent_row_is_the_known_gap(tmp_path):
    status, text = _incoherent_row_run(tmp_path, "test_a_failed_assertion")
    assert status == 0 and "1 xfailed" in text, text


def test_a_crash_in_an_incoherent_row_fails_the_run(tmp_path):
    status, text = _incoherent_row_run(tmp_path, "test_a_crash_of_the_row")
    assert status == 1 and "1 failed" in text, text


def test_an_owner_timeout_in_an_incoherent_row_fails_the_run(tmp_path):
    """The owner's failures are assertion errors; they are not the gap."""
    status, text = _incoherent_row_run(tmp_path, "test_an_owner_timeout")
    assert status == 1 and "1 failed" in text and "xfailed" not in text, text


def test_an_unexpected_exit_in_an_incoherent_row_fails_the_run(tmp_path):
    status, text = _incoherent_row_run(tmp_path, "test_an_unexpected_exit")
    assert status == 1 and "1 failed" in text and "xfailed" not in text, text


def test_an_editor_killed_by_a_crash_signal_in_an_incoherent_row_fails_the_run(tmp_path):
    """The row itself ends clean (an XPASS); the owner's report of the crash at
    teardown is what must fail it, not be read as the known gap."""
    status, text = _incoherent_row_run(tmp_path, "test_an_editor_killed_by_a_crash_signal")
    assert status != 0 and "xfailed" not in text, text


def test_the_incoherence_text_is_the_one_the_harness_reads():
    assert wpe_driver.INCOHERENCE_PREFIX.strip() == harness.KNOWN_XFAIL_PREFIX


def test_a_layer_gate_names_the_tools_the_job_installed(monkeypatch):
    monkeypatch.setenv(host_tools.EXPECTED_VARIABLE, "Xvfb xwdtopnm magick")
    missing = ["xdotool", "matchbox-window-manager|twm", "xwdtopnm (netpbm)"]
    assert host_tools.expected_among(missing) == ["xwdtopnm (netpbm)"]


def test_a_layer_gate_stops_the_session_for_an_installed_tool(monkeypatch):
    monkeypatch.setenv(host_tools.EXPECTED_VARIABLE, "xwdtopnm")
    with pytest.raises(pytest.exit.Exception) as stopped:
        host_tools.fail_the_session_on_expected("X11 GUI layer", ["xwdtopnm (netpbm)"])
    assert "xwdtopnm" in str(stopped.value)
    assert stopped.value.returncode == host_tools.EXPECTED_TOOL_EXIT


def test_a_layer_gate_still_skips_on_a_job_that_installed_nothing(monkeypatch):
    monkeypatch.delenv(host_tools.EXPECTED_VARIABLE, raising=False)
    host_tools.fail_the_session_on_expected("X11 GUI layer", ["xwdtopnm (netpbm)"])


# The gate reads the plan of the run and what each job wrote about how it
# ended, so a row no junit holds says why (tests/harness/suite_plan.py).

PLAN_ROW = "tests/t.py::t"
PLAN_CASE = "tests/t.py::t[b]"


def planted_report(tmp_path, monkeypatch, capsys, shard_of, statuses, junit_cases=(),
                   with_plan=True):
    """Run `report` for one scenario of a made-up capability: @shard_of is
    {node id: shard} the plan holds, @statuses {shard: exit} the status files
    say (a shard missing from it uploaded nothing).  Returns (verdict,
    stderr)."""
    import json
    monkeypatch.setattr(harness, "load_manifest", lambda cap: {"scenarios": [
        scenario(id="SC-plan-01", test=PLAN_ROW)]})
    plan = tmp_path / "plan.json"
    plan.write_text(json.dumps({"assignment": shard_of}))
    status = tmp_path / "status"
    status.mkdir()
    for shard, code in statuses.items():
        (status / ("shard-%d-status.json" % shard)).write_text(json.dumps(
            {"shard": shard, "exit": code, "cut_min": 100}))
    cases = "".join('<testcase classname="tests.t" name="%s"/>' % name for name in junit_cases)
    junit = tmp_path / "j.xml"
    junit.write_text("<testsuites><testsuite>%s</testsuite></testsuites>" % cases)
    harness.cmd_report("plan", "ncurses", str(junit),
                       str(plan) if with_plan else None, str(status))
    seen = capsys.readouterr()
    return json.loads(seen.out)["SC-plan-01"], seen.err


# @scenario SC-platform-suite-plan-10
def test_a_mapped_row_in_a_cut_shard_is_missing_and_says_the_shard_was_cut(
        tmp_path, monkeypatch, capsys):
    got, err = planted_report(tmp_path, monkeypatch, capsys, {"tests/t.py::t": 2}, {2: 124})
    assert got == "MISSING"
    assert "SC-plan-01: MISSING, not run: shard 2 was cut at 100 min" in err, err


def test_a_mapped_row_in_a_shard_that_ended_early_says_the_exit(tmp_path, monkeypatch, capsys):
    got, err = planted_report(tmp_path, monkeypatch, capsys, {"tests/t.py::t": 1}, {1: 2})
    assert got == "MISSING" and "shard 1 ended (exit 2) before running it" in err, err


def test_a_mapped_row_in_a_shard_that_uploaded_nothing_says_so(tmp_path, monkeypatch, capsys):
    got, err = planted_report(tmp_path, monkeypatch, capsys, {"tests/t.py::t": 3}, {})
    assert got == "MISSING" and "shard 3 uploaded nothing" in err, err


def test_one_case_of_a_row_that_a_cut_shard_never_reached_makes_the_row_missing(
        tmp_path, monkeypatch, capsys):
    """The junit holds case a; case b was in the plan and never ran."""
    got, err = planted_report(
        tmp_path, monkeypatch, capsys,
        {"tests/t.py::t[a]": 0, PLAN_CASE: 0}, {0: 124}, junit_cases=["t[a]"])
    assert got == "MISSING" and "was cut at 100 min" in err, err


def test_a_row_every_planned_case_of_which_ran_is_judged_by_its_junit(
        tmp_path, monkeypatch, capsys):
    got, err = planted_report(
        tmp_path, monkeypatch, capsys,
        {"tests/t.py::t[a]": 0, PLAN_CASE: 0}, {0: 0}, junit_cases=["t[a]", "t[b]"])
    assert got == "PASS" and "MISSING" not in err, err


def test_a_report_with_no_plan_reads_the_junit_alone(tmp_path, monkeypatch, capsys):
    got, err = planted_report(tmp_path, monkeypatch, capsys, {"tests/t.py::t": 2}, {2: 124},
                              with_plan=False)
    assert got == "MISSING" and "shard" not in err, err


def test_the_plan_and_status_options_are_read_from_anywhere_on_the_line():
    value, rest = harness.take_option(["report", "--plan", "p.json", "cap", "ncurses", "j"], "--plan")
    assert value == "p.json" and rest == ["report", "cap", "ncurses", "j"]
    assert harness.take_option(["check"], "--plan") == (None, ["check"])


# @scenario SC-platform-suite-plan-11
def test_the_check_asks_the_planner_whether_the_suite_still_fits(tmp_path):
    ids = ["tests/test_a.py::t%d" % n for n in range(3)]
    thin = tmp_path / "thin.json"
    suite_plan_record = {"rows": {}, "host_kind": "runner", "scale": 3}
    import json
    thin.write_text(json.dumps(suite_plan_record))
    problems = harness.tree_fit_problems(str(thin), ids)
    assert any("%d%%" % harness.suite_plan.RECORD_COVER in line for line in problems), problems
    import ast
    with open(harness.__file__, encoding="utf-8") as fh:
        tree = ast.parse(fh.read())
    body = next(n for n in ast.walk(tree)
                if isinstance(n, ast.FunctionDef) and n.name == "cmd_check")
    called = {c.func.id for c in ast.walk(body)
              if isinstance(c, ast.Call) and isinstance(c.func, ast.Name)}
    assert "tree_fit_problems" in called, "harness.py check does not run the fitness check"


def test_the_check_fails_on_a_display_row_the_record_never_saw():
    """The fitness check runs over every layer of the plan, so a file added to
    a display layer tomorrow is a finding here, in minutes, instead of a
    display shard cut after its minutes with rows no estimate placed."""
    problems = harness.tree_fit_problems(None, ["tests/x11/test_brand_new.py::test_a"])
    assert any("x11" in line and "0.0%" in line for line in problems), problems
    problems = harness.tree_fit_problems(None, ["tests/wayland/test_brand_new.py::test_a"])
    assert any("wayland" in line and "0.0%" in line for line in problems), problems


# A junit case that failed and one that passed, for a file of a display layer.
FAILED_CASE = '<testcase classname="%s" name="t"><failure message="boom">x</failure></testcase>'
PASSED_CASE = '<testcase classname="%s" name="t"/>'


def junit_of(*cases):
    """A junit file text holding @cases."""
    return "<testsuites><testsuite>%s</testsuite></testsuites>" % "".join(cases)


# @scenario SC-platform-suite-plan-24
def test_a_failing_scenario_of_a_layer_the_plan_only_shows_does_not_fail_the_report(
        tmp_path, monkeypatch, capsys):
    """The Wayland rows outside the resize and relayout gates run on a shared
    runner that fails them on timing alone: the report keeps their verdict, so
    a regression in them is still read, and says SHOWN instead of failing."""
    import json
    monkeypatch.setattr(harness, "load_manifest", lambda cap: {"scenarios": [
        scenario(id="SC-wl-shown-01", layer="wayland", test="tests/wayland/test_grid.py::t"),
        scenario(id="SC-wl-gate-01", layer="wayland", test="tests/wayland/test_resize.py::t")]})
    junit = tmp_path / "j.xml"
    junit.write_text(junit_of(FAILED_CASE % "tests.wayland.test_grid",
                              PASSED_CASE % "tests.wayland.test_resize"))
    assert harness.cmd_report("fake", "wayland", str(junit)) == 0
    seen = capsys.readouterr()
    assert "SHOWN: SC-wl-shown-01: FAIL" in seen.err, seen.err
    assert json.loads(seen.out) == {"SC-wl-shown-01": "FAIL", "SC-wl-gate-01": "PASS"}, seen.out
    # The same failure in a row of a gate file fails the report, and is not
    # shown: the layer's own rule decides, never the report's caller.
    junit.write_text(junit_of(FAILED_CASE % "tests.wayland.test_grid",
                              FAILED_CASE % "tests.wayland.test_resize"))
    assert harness.cmd_report("fake", "wayland", str(junit)) == 1
    seen = capsys.readouterr()
    assert "SHOWN: SC-wl-gate-01" not in seen.err, seen.err
    assert "SHOWN: SC-wl-shown-01: FAIL" in seen.err, seen.err


def test_a_failing_scenario_of_the_x11_layer_or_of_the_ncurses_one_is_never_shown(
        tmp_path, monkeypatch, capsys):
    """Every X11 row gates, and a ncurses row gates through the scenario that
    maps it, so neither is shown: today's blocking gate does not move."""
    for layer, path in (("x11", "tests/x11/test_grid.py::t"),
                        ("ncurses", "tests/test_grid.py::t")):
        monkeypatch.setattr(harness, "load_manifest", lambda cap: {"scenarios": [
            scenario(id="SC-gating-01", layer=layer, test=path)]})
        junit = tmp_path / ("j-%s.xml" % layer)
        junit.write_text(junit_of(FAILED_CASE % harness.junit_key(path)[0]))
        assert harness.cmd_report("fake", layer, str(junit)) == 1, layer
        seen = capsys.readouterr()
        assert "SHOWN" not in seen.err, (layer, seen.err)
