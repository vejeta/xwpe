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

Verdicts are PASS, FAIL, BLOCKED (manifest says so) or MISSING (the test did
not run).  A skipped test counts as FAIL unless its scenario is ``blocked`` or
its layer is ``live``: a silent skip must never read as coverage.

``check`` collects with ``sys.executable``, so run it with the interpreter the
suite itself uses -- ``tests/.venv/bin/python tests/harness/harness.py check``.
Under a bare ``python3`` that has no pyte, nothing collects and every row is
reported as a problem, which reads like total drift and is only a missing
import.
"""
import json
import os
import subprocess
import sys
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(os.path.dirname(HERE))
PYTHON_LAYERS = ("ncurses", "x11", "wayland", "asan", "config-file", "live")


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


def junit_results(xml_path):
    """{(classname, name): 'PASS'|'FAIL'|'SKIP'} from a pytest junit file."""
    results = {}
    for case in ET.parse(xml_path).iter("testcase"):
        key = (case.get("classname", ""), case.get("name", ""))
        if case.find("failure") is not None or case.find("error") is not None:
            results[key] = "FAIL"
        elif case.find("skipped") is not None:
            results[key] = "SKIP"
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


def verdict(scenario, raw):
    """Fold a raw test result into the scenario's verdict."""
    if scenario["status"] == "blocked":
        return "BLOCKED"
    if raw is None:
        return "MISSING"
    if raw == "PASS":
        return "PASS"
    if raw == "SKIP" and scenario["layer"] == "live":
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


def junit_results_of(sources):
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
            raw.update(junit_results(path))
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


def cmd_report(capability, layer, source):
    manifest = load_manifest(capability)
    if layer == "c-unit":
        raw = automake_results(source)
        lookup = lambda test_id: raw.get(test_id)
    else:
        raw = junit_results_of(source)
        lookup = lambda test_id: junit_lookup(raw, test_id)
    # A scenario may be pinned by several tests (several rows with one id);
    # the scenario's verdict is the worst of them, never the last one listed.
    severity = {"FAIL": 0, "MISSING": 1, "BLOCKED": 2, "PASS": 3}
    report = {}
    for scenario in scenarios_for_layer(manifest, layer):
        result = lookup(scenario["test"]) if scenario.get("test") else None
        this = verdict(scenario, result)
        seen = report.get(scenario["id"])
        if seen is None or severity[this] < severity[seen]:
            report[scenario["id"]] = this
    json.dump(report, sys.stdout, indent=1, sort_keys=True)
    print()
    return 0 if all(v in ("PASS", "BLOCKED") for v in report.values()) else 1


def collected_node_ids():
    """Every pytest node id of the three suites, via --collect-only.

    The GUI suites are collected on their own: their conftest files keep
    them out of a whole-tree ``pytest tests`` run so the default suite stays
    green on a machine without an X server or a compositor.
    """
    ids = set()
    for suite in ("tests", "tests/x11", "tests/wayland"):
        out = subprocess.run(
            [sys.executable, "-m", "pytest", "--collect-only", "-q", suite,
             "-p", "no:cacheprovider"],
            cwd=TOP, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
        ).stdout
        ids.update(line.strip() for line in out.splitlines() if "::" in line)
    return ids


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
    node_ids = collected_node_ids()
    for entry in sorted(os.listdir(HERE)):
        if not os.path.exists(manifest_path(entry)):
            continue
        manifest = load_manifest(entry)
        for scenario in manifest["scenarios"]:
            test_id = scenario.get("test")
            if not test_id:
                if scenario["status"] != "blocked":
                    problems.append("%s: %s has no test and is not blocked" % (entry, scenario["id"]))
                continue
            if scenario["layer"] in PYTHON_LAYERS:
                mapped_files.add(test_file(test_id))
                base = test_id.split("[", 1)[0]
                if not any(n == test_id or n.startswith(base) for n in node_ids):
                    problems.append("%s: %s -> %s does not collect" % (entry, scenario["id"], test_id))
    allowed = unmapped_allowed()
    for path in ai_test_files():
        if path not in mapped_files and path not in allowed:
            problems.append("unmapped test file: %s (map it or list it in tests/harness/UNMAPPED.txt)" % path)
    for problem in problems:
        print(problem)
    print("harness check: %d problem(s)" % len(problems))
    return 1 if problems else 0


def main(argv):
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
