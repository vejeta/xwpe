#!/usr/bin/env python3
"""Scenario harness: map spec scenarios to tests and report per-scenario verdicts.

Each capability has a manifest, ``tests/harness/<capability>/manifest.json``::

    {
      "capability": "ai-options-dialog",
      "scenarios": [
        {"id": "SC-ai-options-01",
         "requirement": "Field edits survive sibling pickers",
         "layer": "ncurses",
         "test": "tests/test_ai_provider_key.py::test_typed_key_survives_provider_switch",
         "status": "covered"},
        {"id": "SC-ai-options-11",
         "requirement": "A radio can be moved off its default by keyboard",
         "layer": "ncurses",
         "test": null,
         "status": "blocked",
         "reason": "tests/docs/picker-keyboard-investigation.md; x11 twin SC-ai-options-11x"}
      ]
    }

``status`` is ``covered`` (an existing test), ``new`` (written for the
scenario) or ``blocked`` (cannot be asserted in that layer; ``reason`` says
why).  ``layer`` is one of ncurses, x11, wayland, asan, config-file, c-unit,
live.  The test id uses pytest's node-id form for Python tests and the automake
test program name for C tests.

Tests that press a terminal function key run in the ``ncurses`` layer like any
other: measured on the hosted runner (2026-10-19), both the SS3 and the CSI
sequences reach the editor there, so the separate function-key layer that once
kept them out of the gate is gone.

Sub-commands (all stdlib, no third-party imports, so CI can run it bare):

    files  <capability> <layer>      print the test files to run
    report <capability> <layer> <junit.xml[,more.xml]|logdir>
                                     print a JSON object {scenario_id: verdict}
    missing <capability> <layer> <junit.xml[,more.xml]>
                                     print the mapped test ids the junit file(s)
                                     never ran (what a gate still has to run)
    check                            drift guard: every manifest id resolves,
                                     every tests/**/test_ai_*.py is mapped or
                                     listed in tests/harness/UNMAPPED.txt
    coverage edit-model <run.json|dir>
                                     the edit model's coverage matrix of one
                                     run (a coverage file, or the directory
                                     EDIT_MODEL_OUT collected): the actions
                                     against the option levels they never
                                     met, the option tuples no row whose
                                     sessions started with its labels
                                     covers, the levels not built yet, the
                                     disagreements by class; exit 1 while a
                                     cell or a tuple is empty

Verdicts are PASS, FAIL, BLOCKED (manifest says so), KNOWN (see below),
NOT_EXERCISED (a job switched the row's host tool off, XWPE_TEST_TOOLS_OFF;
the report fails on it, so a capability a job stopped checking never reads
green) or MISSING (the test did not run).  A skipped test counts as FAIL
unless its scenario is ``blocked``, its layer is ``live``, the row declares
what the test needs and the skip says that is what was missing, or the row
declares it ``known``: a silent skip must never read as coverage.  The report
passes (exit 0) on PASS, BLOCKED and KNOWN only.

A row may carry ``"known": "<text>"``: the recorded state of a row that does
not run clean and is not a tool missing from the host.  The text must be in
every skip message of the row.  ``INCOHERENCE: ...`` names an incoherence
(tests/wpe_driver.py incoherence(), an xfail that only an AssertionError
counts for: the junit type must be pytest.xfail); any other text must name the
approval an unapproved golden waits for (a pytest.skip).  An incoherence or a
golden the manifest does not declare is a FAIL, so a row cannot make its own
skip a reviewed state.  The report prints the KNOWN scenarios with their text
on stderr.

XWPE_TEST_TOOLS_EXPECTED (blank-separated, set by a job) lists the host tools
the job installed (tests/ci-tools.txt keeps it equal to the workflow).  A row
that declares one of them and skips for its absence is a FAIL there, with the
line "<tool> is installed by this job but not usable": on that job the tool is
meant to be there.  On a host without the variable the same skip is BLOCKED.

``check`` also reads, from the code of every collected row (the host tools it
declares with needs(), dumped by tests/conftest.py when XWPE_NEEDS_DUMP names a
file), that its manifest row declares each tool that is not one every host has
(kind ``base`` of tests/ci-tools.txt).

A row may carry ``"needs": ["clangd"]``: the host tools the test cannot run
without (a language server, a GPM daemon).  Where the tool is installed the
test runs and must pass.  Where it is not, the test skips itself, and the
skip reads as BLOCKED only when every skip message of that row names one of
the declared needs (case-insensitive); a skip for any other reason is still
a FAIL.  So a declared absence is reported as such, and an undeclared one
still turns the gate red.

``check`` collects with ``sys.executable``, so run it with the interpreter the
suite itself uses -- ``tests/.venv/bin/python tests/harness/harness.py check``.
Under a bare ``python3`` that has no pyte, nothing collects and every row is
reported as a problem, which reads like total drift and is only a missing
import.
"""
import ast
import json
import os
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(os.path.dirname(HERE))
# engine_guard.py is stdlib only: it names the live files.
sys.path.insert(0, os.path.dirname(HERE))
import engine_guard  # noqa: E402
PYTHON_LAYERS = ("ncurses", "x11", "wayland", "asan", "config-file", "live")
# The file tests/conftest.py writes the host tools of every collected row to
# when this variable names one (see collect_suite).
NEEDS_DUMP_VARIABLE = "XWPE_NEEDS_DUMP"
# The host tools the job installed (tests/ci-tools.txt keeps it equal to the
# workflow's apt-get list); a declared tool of the list that skips is a FAIL.
EXPECTED_VARIABLE = "XWPE_TEST_TOOLS_EXPECTED"
# The types pytest writes on a <skipped> element of the junit file.
XFAIL_TYPE = "pytest.xfail"
SKIP_TYPE = "pytest.skip"
# A manifest row's ``known`` names a recorded incoherence by this prefix (the
# text tests/wpe_driver.py incoherence() puts on its marker) or an unapproved
# golden by the approval text of tests/test_edit_model_golden.py.
KNOWN_XFAIL_PREFIX = "INCOHERENCE:"
KNOWN_APPROVAL_TEXT = "waits for the user's approval"
# Worst first: the verdict of a scenario pinned by several rows is the worst.
SEVERITY = {"FAIL": 0, "MISSING": 1, "NOT_EXERCISED": 2, "KNOWN": 3,
            "BLOCKED": 4, "PASS": 5}
# The verdicts that let the gate pass: a declared absence and a recorded
# known state are reviewed declarations of the manifest, nothing else is.
PASSING = ("PASS", "BLOCKED", "KNOWN")
# The record of the host tools (tests/ci-tools.txt) and the kind that needs no
# declaration: a tool every Linux host the suite runs on has.
CI_TOOLS_RECORD = os.path.join(os.path.dirname(HERE), "ci-tools.txt")
BASE_KIND = "base"


def manifest_path(capability):
    return os.path.join(HERE, capability, "manifest.json")


def load_manifest(capability):
    with open(manifest_path(capability), encoding="utf-8") as fh:
        return json.load(fh)


def scenarios_for_layer(manifest, layer):
    """The scenarios a run of this layer is responsible for."""
    wanted = {layer}
    if layer in ("ncurses", "asan"):
        # config-file scenarios are pyte tests that seed xwperc before launch;
        # they run with the ncurses binary.  The sanitizer run re-executes the
        # same scenarios under we-asan.
        wanted.update(("ncurses", "config-file"))
    return [s for s in manifest["scenarios"] if s["layer"] in wanted]


def test_file(test_id):
    """``tests/x.py::fn[param]`` -> ``tests/x.py``; C program names pass through."""
    return test_id.split("::", 1)[0]


def live_gated_test_exists(test_id):
    """True when @test_id names a test function of a live file (tests/conftest
    does not import a live file unless the run opened the gate, so pytest
    collects one skipping stand-in for it, never the functions).  The check
    reads the file as text, with ast, and never imports it."""
    path = test_file(test_id)
    if not engine_guard.is_live_file(path):
        return False
    name = test_id.split("::", 1)[1].split("[", 1)[0] if "::" in test_id else ""
    try:
        with open(os.path.join(TOP, path), encoding="utf-8") as fh:
            tree = ast.parse(fh.read(), filename=path)
    except (OSError, SyntaxError):
        return False
    return any(isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
               and node.name == name for node in ast.walk(tree))


def cmd_files(capability, layer):
    """The pytest arguments for this layer: the files the scenarios' tests live in."""
    manifest = load_manifest(capability)
    seen = []
    for scenario in scenarios_for_layer(manifest, layer):
        if not scenario.get("test"):
            continue
        item = test_file(scenario["test"])
        if item not in seen:
            seen.append(item)
    print("\n".join(seen))


def junit_results(xml_path, skip_reasons=None, skip_kinds=None):
    """{(classname, name): 'PASS'|'FAIL'|'SKIP'} from a pytest junit file.

    When ``skip_reasons`` is a dict it also receives, per skipped case, the
    skip message, so a row's declared needs can be matched against it.
    ``skip_kinds`` receives the type pytest wrote on the skip: XFAIL_TYPE
    for a row that failed as it is expected to (an xfail), SKIP_TYPE for a
    row that did not run.  The two are not the same thing, and a row that
    did not run must never read like one that ran and failed as expected.
    """
    results = {}
    for case in ET.parse(xml_path).iter("testcase"):
        key = (case.get("classname", ""), case.get("name", ""))
        skipped = case.find("skipped")
        if case.find("failure") is not None or case.find("error") is not None:
            results[key] = "FAIL"
        elif skipped is not None:
            results[key] = "SKIP"
            if skip_reasons is not None:
                skip_reasons[key] = (skipped.get("message") or "") + " " + (skipped.text or "")
            if skip_kinds is not None:
                skip_kinds[key] = skipped.get("type") or SKIP_TYPE
        else:
            results[key] = "PASS"
    return results


def junit_key(test_id):
    """pytest node id -> the (classname, name) pair pytest writes to junit.

    ``tests/x.py::test_a`` -> ("tests.x", "test_a");
    ``tests/x.py::TestY::test_a`` -> ("tests.x.TestY", "test_a").
    """
    parts = test_id.split("::")
    path, classes, name = parts[0], parts[1:-1], parts[-1]
    module = path[:-3] if path.endswith(".py") else path
    return ".".join([module.replace("/", ".")] + classes), name


def automake_results(logdir):
    """{program: 'PASS'|'FAIL'|'SKIP'} from automake ``*.trs`` files."""
    results = {}
    for entry in os.listdir(logdir):
        if not entry.endswith(".trs"):
            continue
        with open(os.path.join(logdir, entry), encoding="utf-8") as fh:
            for line in fh:
                if line.startswith(":test-result:"):
                    results[entry[:-4]] = line.split(":", 2)[2].strip()
    return results


def skip_names_a_need(scenario, reasons):
    """Did the test skip because a tool the row declares it needs is absent?

    True only when the row has ``needs`` and every skip message of the row's
    cases names one of them; an empty or unrelated message is not enough.
    """
    needs = [n.lower() for n in scenario.get("needs") or []]
    if not needs or not reasons:
        return False
    return all(any(n in r.lower() for n in needs) for r in reasons)


def expected_tools():
    """The host tools this job installed, from XWPE_TEST_TOOLS_EXPECTED
    (blank-separated, the same names the manifests' ``needs`` use)."""
    return os.environ.get(EXPECTED_VARIABLE, "").split()


def skip_names_an_expected_need(scenario, reasons, expected):
    """The declared tools a skip names that the job says it installed.

    A row that declares a tool and skips for its absence reads BLOCKED on a
    host that never had the tool.  On a job that installed it, the same skip
    means the tool is there and does not work, which is a failure of the
    image the report must not hide behind the declaration."""
    named = []
    wanted = {e.lower() for e in expected}
    for need in scenario.get("needs") or []:
        if need.lower() in wanted and any(need.lower() in r.lower() for r in reasons or []):
            named.append(need)
    return named


def skip_is_known(scenario, reasons, kinds):
    """Is every skip of the row the incoherence or unapproved golden the
    manifest row declares in ``known``?

    ``known`` is text the skip message must contain, so a row cannot become
    a known one by writing a marker of its own.  A row whose ``known`` starts
    with KNOWN_XFAIL_PREFIX must have failed as an xfail: a row that did not
    run, or one that ended on a crash, is not a recorded incoherence.  Any
    other ``known`` must name the approval a golden waits for (checked by
    ``check``) and must be a skip, not an xfail.
    """
    known = (scenario.get("known") or "").lower()
    if not known or not reasons:
        return False
    if not all(known in r.lower() for r in reasons):
        return False
    wanted_kind = XFAIL_TYPE if scenario["known"].startswith(KNOWN_XFAIL_PREFIX) else SKIP_TYPE
    return all(kind == wanted_kind for kind in kinds or [])


# The tail of the skip a job gives a row whose host tool it switched off
# (tests/host_tools.py, NOT_EXERCISED; tests/test_host_tools.py keeps the two
# equal).  This file takes no third-party import, so it repeats the text.
NOT_EXERCISED = "not exercised by this job"


def skip_says_not_exercised(reasons):
    """Did a job switch the row's tool off (the row never ran on purpose)?

    That is not an absence of the tool and it is not a block: the capability
    went unchecked because the job chose not to check it, and the report must
    say so, so that a job cannot turn a capability off and still read green.
    """
    return any(NOT_EXERCISED in r.lower() for r in reasons or [])


def verdict(scenario, raw, reasons=None, kinds=None, expected=()):
    """Fold a raw test result into the scenario's verdict.

    The one place a skip becomes a verdict.  In this order: a skip of a live
    row or a switched-off tool is BLOCKED or NOT_EXERCISED; a skip that names
    a declared tool the job installed is FAIL (the tool is there and does not
    work); a skip the manifest row declares as ``known`` is KNOWN; a skip that
    names a declared tool is BLOCKED; any other skip is FAIL."""
    if scenario["status"] == "blocked":
        return "BLOCKED"
    if raw is None:
        return "MISSING"
    if raw == "PASS":
        return "PASS"
    if raw == "SKIP" and scenario["layer"] == "live":
        return "BLOCKED"
    if raw == "SKIP" and skip_says_not_exercised(reasons):
        return "NOT_EXERCISED"
    if raw == "SKIP" and skip_names_an_expected_need(scenario, reasons, expected):
        return "FAIL"
    if raw == "SKIP" and skip_is_known(scenario, reasons, kinds):
        return "KNOWN"
    if raw == "SKIP" and skip_names_a_need(scenario, reasons):
        return "BLOCKED"
    return "FAIL"


def fold_results(results):
    """Combine several raw results into one: any FAIL fails, any SKIP skips."""
    if not results:
        return None
    if "FAIL" in results:
        return "FAIL"
    if "SKIP" in results:
        return "SKIP"
    return "PASS"


def junit_lookup(raw, test_id):
    """Result for a node id, folding the cases of a parametrized test.

    ``tests/x.py::test_a`` matches junit names ``test_a`` and ``test_a[...]``;
    a node id that already names one case matches that case only.
    """
    classname, name = junit_key(test_id)
    if "[" in name:
        return raw.get((classname, name))
    cases = [v for (c, n), v in raw.items()
             if c == classname and (n == name or n.startswith(name + "["))]
    return fold_results(cases)


def junit_skip_reasons(skip_reasons, test_id):
    """The skip messages of a node id's cases, folded like junit_lookup."""
    classname, name = junit_key(test_id)
    if "[" in name:
        reason = skip_reasons.get((classname, name))
        return [reason] if reason is not None else []
    return [r for (c, n), r in skip_reasons.items()
            if c == classname and (n == name or n.startswith(name + "["))]


def junit_results_of(sources, skip_reasons=None, skip_kinds=None):
    """Merged results of one or more junit files, given comma-separated.

    The CI suite writes one junit for the whole run, and the gate tops it up
    with a second one for the few manifest tests the suite does not select;
    the verdicts are read from both together.
    """
    raw = {}
    for path in sources.split(","):
        # A suite that died before writing its junit leaves no file: read it
        # as "nothing ran", so the gate falls back to running every test.
        if path and os.path.exists(path):
            raw.update(junit_results(path, skip_reasons, skip_kinds))
    return raw


def cmd_missing(capability, layer, sources):
    """Print the mapped test ids that the given junit file(s) never ran.

    Lets a gate run only what is left after a suite run, instead of running
    every mapped test a second time.  A blocked row has nothing to run.
    """
    raw = junit_results_of(sources)
    seen = set()
    for scenario in scenarios_for_layer(load_manifest(capability), layer):
        test_id = scenario.get("test")
        if scenario["status"] == "blocked" or not test_id or test_id in seen:
            continue
        seen.add(test_id)
        if junit_lookup(raw, test_id) is None:
            print(test_id)
    return 0


def junit_skip_kinds(skip_kinds, test_id):
    """The skip types of a node id's cases, folded like junit_lookup."""
    return junit_skip_reasons(skip_kinds, test_id)


def say_why(this, scenario, reasons, expected):
    """The line, on stderr, that says why a scenario read FAIL or KNOWN when
    the JSON verdict alone cannot: a tool the job installed that its row
    could not use, or the declared text of a known row."""
    if this == "FAIL":
        for tool in skip_names_an_expected_need(scenario, reasons, expected):
            sys.stderr.write("%s: %s is installed by this job but not usable\n"
                             % (scenario["id"], tool))
    elif this == "KNOWN":
        sys.stderr.write("%s: KNOWN, %s\n" % (scenario["id"], scenario["known"]))


def cmd_report(capability, layer, source):
    manifest = load_manifest(capability)
    skip_reasons = {}
    skip_kinds = {}
    expected = expected_tools()
    if layer == "c-unit":
        raw = automake_results(source)
        lookup = lambda test_id: raw.get(test_id)
    else:
        raw = junit_results_of(source, skip_reasons, skip_kinds)
        lookup = lambda test_id: junit_lookup(raw, test_id)
    # A scenario may be pinned by several tests (several rows with one id);
    # the scenario's verdict is the worst of them, never the last one listed.
    report = {}
    for scenario in scenarios_for_layer(manifest, layer):
        result = lookup(scenario["test"]) if scenario.get("test") else None
        reasons = junit_skip_reasons(skip_reasons, scenario["test"]) if scenario.get("test") else []
        kinds = junit_skip_kinds(skip_kinds, scenario["test"]) if scenario.get("test") else []
        this = verdict(scenario, result, reasons, kinds, expected)
        say_why(this, scenario, reasons, expected)
        seen = report.get(scenario["id"])
        if seen is None or SEVERITY[this] < SEVERITY[seen]:
            report[scenario["id"]] = this
    json.dump(report, sys.stdout, indent=1, sort_keys=True)
    print()
    return 0 if all(v in PASSING for v in report.values()) else 1


def collect_suite(suite):
    """(node ids, {node id: [tool, ...]}) of one suite, via --collect-only.

    The second part is what tests/conftest.py dumps when XWPE_NEEDS_DUMP
    names a file: the host tools each collected row declares with needs().
    """
    with tempfile.TemporaryDirectory(prefix="needs-dump.") as scratch:
        dump = os.path.join(scratch, "needs.json")
        env = dict(os.environ, **{NEEDS_DUMP_VARIABLE: dump})
        out = subprocess.run(
            [sys.executable, "-m", "pytest", "--collect-only", "-q", suite,
             "-p", "no:cacheprovider"],
            cwd=TOP, env=env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True,
        ).stdout
        ids = {line.strip() for line in out.splitlines() if "::" in line}
        needs = {}
        if os.path.exists(dump):
            with open(dump, encoding="utf-8") as fh:
                needs = json.load(fh)
    return ids, needs


def collected_items():
    """Every pytest node id of the three suites, and the host tools each one
    declares, via --collect-only.

    The GUI suites are collected on their own: their conftest files keep
    them out of a whole-tree ``pytest tests`` run so the default suite stays
    green on a machine without an X server or a compositor.
    """
    ids = set()
    needs = {}
    for suite in ("tests", "tests/x11", "tests/wayland"):
        suite_ids, suite_needs = collect_suite(suite)
        ids.update(suite_ids)
        needs.update(suite_needs)
    return ids, needs


def row_covers(row_test, node_id):
    """Does a manifest row's test id name this collected node?

    A row names a file, a test, or one case of a parametrized test."""
    if node_id == row_test or node_id.startswith(row_test + "[") \
            or node_id.startswith(row_test + "::"):
        return True
    return "::" not in row_test and node_id.split("::", 1)[0] == row_test


def tool_declared(spec, declared):
    """Is a code-side need ("gdb", or "a|b" for alternatives) named by the
    row's declared needs: the whole spec, or any one alternative."""
    return spec in declared or any(alt in declared for alt in spec.split("|"))


def known_incoherence_text(known):
    """True for an INCOHERENCE: text that says something after the prefix: a
    bare prefix would match every incoherence of the row."""
    return known.startswith(KNOWN_XFAIL_PREFIX) and bool(known[len(KNOWN_XFAIL_PREFIX):].strip())


def known_problems(manifests):
    """The rows whose ``known`` text is not one a skip may carry.

    A ``known`` is an incoherence (INCOHERENCE:, an xfail) or an unapproved
    golden (the approval text).  Anything else would let a row declare its
    own skip a reviewed state."""
    problems = []
    for capability, manifest in sorted(manifests.items()):
        for scenario in manifest["scenarios"]:
            known = scenario.get("known")
            if known is None:
                continue
            if scenario["status"] == "blocked" or not scenario.get("test"):
                problems.append("%s: %s has known and no test to run" % (capability, scenario["id"]))
            elif not (known_incoherence_text(known) or KNOWN_APPROVAL_TEXT in known):
                problems.append("%s: %s known must start with '%s' and say what, or name '%s'"
                                % (capability, scenario["id"], KNOWN_XFAIL_PREFIX,
                                   KNOWN_APPROVAL_TEXT))
    return problems


def read_ci_tools(path=CI_TOOLS_RECORD):
    """{need: kind} from the record of host tools; the kind is the word
    before any ':' (absent:no package -> absent)."""
    kinds = {}
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line.strip() or line.startswith("#"):
                continue
            fields = line.split("|")
            if len(fields) >= 3:
                kinds[fields[0]] = fields[2].split(":", 1)[0]
    return kinds


def base_tools(path=CI_TOOLS_RECORD):
    """The tools every Linux host the suite runs on has: a row that needs
    only these need not declare them."""
    return {need for need, kind in read_ci_tools(path).items() if kind == BASE_KIND}


def needs_drift(manifests, node_needs, exempt=()):
    """The rows whose code declares a host tool its manifest row does not.

    A row that needs a tool and does not say so skips, on a host without the
    tool, for a reason the harness cannot read as a declared absence, and the
    scenario reads FAIL.  ``manifests`` is {capability: manifest}; ``exempt``
    are the tools every host has (base_tools()), which no row declares."""
    problems = []
    for capability, manifest in sorted(manifests.items()):
        for scenario in manifest["scenarios"]:
            test_id = scenario.get("test")
            if not test_id or scenario["status"] == "blocked":
                continue
            declared = scenario.get("needs") or []
            for node, tools in sorted(node_needs.items()):
                if not row_covers(test_id, node):
                    continue
                lacking = [t for t in tools
                           if not tool_declared(t, declared)
                           and not tool_declared(t, exempt)]
                if lacking:
                    problems.append(
                        "%s: %s -> %s uses needs(%s) and the row's needs lack it"
                        % (capability, scenario["id"], node, ", ".join(lacking)))
    return problems


def ai_test_files():
    found = []
    for root, _dirs, files in os.walk(os.path.join(TOP, "tests")):
        for name in files:
            if name.startswith("test_ai_") and name.endswith(".py"):
                found.append(os.path.relpath(os.path.join(root, name), TOP))
    return sorted(found)


def unmapped_allowed():
    path = os.path.join(HERE, "UNMAPPED.txt")
    if not os.path.exists(path):
        return set()
    with open(path, encoding="utf-8") as fh:
        return {line.strip() for line in fh if line.strip() and not line.startswith("#")}


def cmd_check():
    problems = []
    mapped_files = set()
    node_ids, node_needs = collected_items()
    manifests = {}
    for entry in sorted(os.listdir(HERE)):
        if not os.path.exists(manifest_path(entry)):
            continue
        manifest = load_manifest(entry)
        manifests[entry] = manifest
        for scenario in manifest["scenarios"]:
            test_id = scenario.get("test")
            if not test_id:
                if scenario["status"] != "blocked":
                    problems.append("%s: %s has no test and is not blocked" % (entry, scenario["id"]))
                continue
            if scenario["layer"] in PYTHON_LAYERS:
                mapped_files.add(test_file(test_id))
                base = test_id.split("[", 1)[0]
                collects = any(n == test_id or n.startswith(base) for n in node_ids)
                if not collects and not live_gated_test_exists(test_id):
                    problems.append("%s: %s -> %s does not collect" % (entry, scenario["id"], test_id))
    problems.extend(needs_drift(manifests, node_needs, base_tools()))
    problems.extend(known_problems(manifests))
    allowed = unmapped_allowed()
    for path in ai_test_files():
        if path not in mapped_files and path not in allowed:
            problems.append("unmapped test file: %s (map it or list it in tests/harness/UNMAPPED.txt)" % path)
    for problem in problems:
        print(problem)
    print("harness check: %d problem(s)" % len(problems))
    return 1 if problems else 0


def edit_model_runs(source):
    """The files of an edit-model run: one coverage file, or every JSON file
    of a directory (one per case)."""
    if os.path.isdir(source):
        return sorted(os.path.join(source, n) for n in os.listdir(source) if n.endswith(".json"))
    return [source]


def cmd_coverage(capability, source):
    """Print the coverage matrix and the disagreement classes of an
    edit-model run; the exit status is 1 while some action never met some
    level of some factor."""
    if capability != "edit-model":
        sys.stderr.write("coverage is written for edit-model only\n")
        return 2
    sys.path.insert(0, os.path.join(TOP, "tests"))
    from edit_model import covering, option_row, walk
    cov = walk.Coverage()
    classes = {}
    walks = []
    cases = 0
    for path in edit_model_runs(source):
        with open(path, encoding="utf-8") as fh:
            data = json.load(fh)
        cases += 1
        cov.merge(walk.Coverage.from_json(data.get("coverage", data)))
        if "walk" in data:
            walks.append(data["walk"])
        for x in data.get("walk", {}).get("disagreements", []):
            key = (x.get("cls"), "known" if x.get("expected") else "new")
            classes[key] = classes.get(key, 0) + 1
    text, holes = walk.report(cov)
    print("cases %d" % cases)
    print(text)
    uncovered, bad_rows = covering.effective_missing(walks)
    print("effective tuples (rows whose sessions started with their labels): "
          "%d uncovered of %d required" % (len(uncovered), len(covering.required())))
    print("taken on the row's label, not read back: %s" % option_row.trusted_text())
    if bad_rows:
        print("rows that did not get their labels: %s" % " ".join(bad_rows))
    for tup in sorted(uncovered)[:5]:
        print("  uncovered  %s" % " ".join("%s=%s" % fl for fl in tup))
    for (cls, kind), n in sorted(classes.items()):
        print("disagreements  %-12s %-6s %d" % (cls, kind, n))
    return 1 if (holes or uncovered or bad_rows) else 0


def main(argv):
    if len(argv) >= 3 and argv[0] == "coverage":
        return cmd_coverage(argv[1], argv[2])
    if len(argv) >= 3 and argv[0] == "files":
        return cmd_files(argv[1], argv[2]) or 0
    if len(argv) >= 4 and argv[0] == "report":
        return cmd_report(argv[1], argv[2], argv[3])
    if len(argv) >= 4 and argv[0] == "missing":
        return cmd_missing(argv[1], argv[2], argv[3])
    if argv and argv[0] == "check":
        return cmd_check()
    sys.stderr.write(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
