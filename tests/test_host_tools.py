"""The owner of the host tools a row needs (tests/host_tools.py).

A row declares needs("gdb"); the resolver in conftest.py is the only place
that skips for a tool, and the skip names the tool.  A job switches tools off
by name with XWPE_TEST_TOOLS_OFF, which replaces the -k expressions that
dropped rows by substring.  These rows run a small module under a child
pytest, with this tree's conftest as its plugin, so they test what a real row
gets, not the helper alone.
"""
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RUN_TIMEOUT_S = 120

CHILD_MODULE = '''
import pytest
from host_tools import needs, require

@needs("frobnicate")
def test_needs_a_tool_nobody_has():
    pass

@needs("cc")
def test_needs_the_compiler():
    pass

@needs("sh")
def test_needs_the_shell():
    pass

@needs("frobnicate|sh")
def test_one_of_two_is_enough():
    pass

@needs("frobnicate|quuxify")
def test_none_of_two():
    pass

def test_a_row_that_learns_its_tool_late():
    require("frobnicate")
'''


def run_child(tmp_path, off=None):
    """Run CHILD_MODULE under this tree's conftest; return (stdout, {id: outcome})."""
    module = tmp_path / "test_child_module.py"
    module.write_text(CHILD_MODULE)
    env = {k: v for k, v in os.environ.items() if k != "XWPE_TEST_TOOLS_OFF"}
    env["PYTHONPATH"] = HERE
    env["WPE_BIN"] = sys.executable      # never asked: the child's rows need no build facts
    if off is not None:
        env["XWPE_TEST_TOOLS_OFF"] = off
    junit = tmp_path / "child.xml"
    done = subprocess.run(
        [sys.executable, "-m", "pytest", "-p", "conftest", "-p", "no:cacheprovider",
         "--rootdir", str(tmp_path), "--junitxml", str(junit), str(module)],
        cwd=str(tmp_path), env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=RUN_TIMEOUT_S)
    text = done.stdout.decode("utf-8", "replace")
    return text, outcomes_of(junit)


def outcomes_of(junit):
    """{test name: ("PASSED"|"SKIPPED"|"FAILED", skip reason)} from a junit file."""
    outcomes = {}
    for case in ET.parse(str(junit)).getroot().iter("testcase"):
        skipped = case.find("skipped")
        if skipped is not None:
            outcomes[case.get("name")] = ("SKIPPED", skipped.get("message") or "")
        elif case.find("failure") is not None or case.find("error") is not None:
            outcomes[case.get("name")] = ("FAILED", "")
        else:
            outcomes[case.get("name")] = ("PASSED", "")
    return outcomes


# @scenario SC-platform-host-tools-01
def test_a_row_needing_a_tool_nobody_has_skips_naming_it(tmp_path):
    text, outcomes = run_child(tmp_path)
    status, reason = outcomes["test_needs_a_tool_nobody_has"]
    assert status == "SKIPPED", text
    assert "no frobnicate on this host" in reason, text


def test_a_row_that_learns_its_tool_late_skips_the_same_way(tmp_path):
    text, outcomes = run_child(tmp_path)
    status, reason = outcomes["test_a_row_that_learns_its_tool_late"]
    assert status == "SKIPPED" and "no frobnicate on this host" in reason, text


def test_a_tool_that_is_there_lets_the_row_run(tmp_path):
    text, outcomes = run_child(tmp_path)
    assert outcomes["test_needs_the_shell"][0] == "PASSED", text


def test_one_usable_alternative_is_enough_and_none_names_both(tmp_path):
    text, outcomes = run_child(tmp_path)
    assert outcomes["test_one_of_two_is_enough"][0] == "PASSED", text
    status, reason = outcomes["test_none_of_two"]
    assert status == "SKIPPED", text
    assert "frobnicate|quuxify" in reason, text


# @scenario SC-platform-host-tools-02
def test_a_tool_the_job_switched_off_skips_as_not_exercised(tmp_path):
    text, outcomes = run_child(tmp_path, off="cc sh")
    for name in ("test_needs_the_compiler", "test_needs_the_shell"):
        status, reason = outcomes[name]
        assert status == "SKIPPED", text
        assert "not exercised by this job" in reason, text
    assert "no cc on this host" not in text, text


def test_the_job_prints_what_it_switched_off(tmp_path):
    text, _ = run_child(tmp_path, off="gdb metals")
    assert "host tools switched off by XWPE_TEST_TOOLS_OFF: gdb metals" in text, text


def test_a_job_that_switched_nothing_off_prints_nothing(tmp_path):
    text, _ = run_child(tmp_path)
    assert "XWPE_TEST_TOOLS_OFF" not in text, text


def test_a_shim_that_does_not_run_is_not_usable(tmp_path, monkeypatch):
    import host_tools
    # The probe runs only for a tool of the baseline (engine_guard.py): a
    # name outside it is checked for presence and never started.
    shim = tmp_path / "gdb"
    shim.write_text("#!/bin/sh\nexit 3\n")
    shim.chmod(0o755)
    monkeypatch.setenv("PATH", str(tmp_path) + os.pathsep + os.environ["PATH"])
    assert not host_tools.tool_usable("gdb")
    good = tmp_path / "lldb"
    good.write_text("#!/bin/sh\nexit 0\n")
    good.chmod(0o755)
    assert host_tools.tool_usable("lldb")


# @scenario SC-platform-host-tools-03
def test_the_harness_does_not_read_a_switched_off_tool_as_blocked():
    sys.path.insert(0, os.path.join(HERE, "harness"))
    try:
        import harness
        import host_tools
    finally:
        sys.path.pop(0)
    assert harness.NOT_EXERCISED == host_tools.NOT_EXERCISED
    scenario = {"status": "covered", "layer": "ncurses", "needs": ["gdb"]}
    assert harness.verdict(scenario, "SKIP", ["gdb: not exercised by this job"]) == "NOT_EXERCISED"
    assert harness.verdict(scenario, "SKIP", ["no gdb on this host"]) == "BLOCKED"
    assert harness.verdict(scenario, "SKIP", ["something else"]) == "FAIL"


# @scenario SC-platform-host-tools-04
def test_the_macos_job_drops_no_row_by_substring():
    """macos.yml names no -k expression but the live word; its tools are
    switched off by name, and it prints them."""
    with open(os.path.join(ROOT, ".github", "workflows", "macos.yml"), encoding="utf-8") as fh:
        text = fh.read()
    # pytest's -k takes a quoted expression; make's own `-k` (keep going) does not.
    for expression in re.findall(r"""\s-k\s+["']([^"']*)["']""", text):
        assert expression == "not _live", "macos.yml drops rows with -k %r" % expression
    assert "XWPE_TEST_TOOLS_OFF" in text


def test_no_row_decides_on_its_own_whether_a_tool_is_usable():
    """No skipif guard asks a `.which(` of any receiver, tool_usable, usable
    or a local _have: a row says needs().

    A shutil.which that finds a binary to hand to a stand-in is not a guard
    and stays.  Only the skip decision belongs to the owner."""
    guard = re.compile(r"skipif\((?:[^()]|\([^()]*\))*?(\.which\(|usable\(|_have\()")
    offenders = []
    for folder, _dirs, files in os.walk(HERE):
        if ".venv" in folder or "__pycache__" in folder:
            continue
        for name in files:
            path = os.path.join(folder, name)
            if not name.endswith(".py") or name in ("host_tools.py", "test_host_tools.py"):
                continue
            with open(path, encoding="utf-8", errors="replace") as fh:
                if guard.search(fh.read()):
                    offenders.append(os.path.relpath(path, HERE))
    assert not offenders, "decide a skip with needs(): %s" % ", ".join(sorted(offenders))


LAYER_CONFTESTS = ("x11", "wayland", "console")


def which_calls_in(path):
    """The line numbers of every call of a function called `which`, on any
    receiver (shutil.which, a bare which), outside strings and comments."""
    import ast
    with open(path, encoding="utf-8") as fh:
        tree = ast.parse(fh.read(), filename=path)
    found = []
    for node in ast.walk(tree):
        if isinstance(node, ast.Call):
            func = node.func
            name = func.attr if isinstance(func, ast.Attribute) else getattr(func, "id", "")
            if name == "which":
                found.append(node.lineno)
    return sorted(found)


# @scenario SC-platform-host-tools-03
def test_no_layer_conftest_asks_shutil_which_it_asks_layer_unmet():
    """The three layer gates decide whether their tools are usable in one
    place (host_tools.layer_unmet, first_usable, layer_reason): a `which` of
    their own finds a program the tool farm denies, and the layer then runs
    and errors every row at setup."""
    offenders = []
    for layer in LAYER_CONFTESTS:
        path = os.path.join(HERE, layer, "conftest.py")
        for line in which_calls_in(path):
            offenders.append("tests/%s/conftest.py:%d" % (layer, line))
    assert not offenders, "ask host_tools.layer_unmet: %s" % ", ".join(offenders)


def test_the_scan_for_which_calls_sees_each_spelling(tmp_path):
    """A canary for the rule above: both spellings are found, a string and a
    comment are not."""
    source = tmp_path / "conftest.py"
    source.write_text("import shutil\nfrom shutil import which\n"
                      "a = shutil.which('x')\nb = which('y')\n"
                      "c = 'shutil.which(z)'  # which(w)\n")
    assert which_calls_in(str(source)) == [3, 4]
