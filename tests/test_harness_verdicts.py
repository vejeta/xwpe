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
