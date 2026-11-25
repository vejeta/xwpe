#!/usr/bin/env python3
"""The Linux CI suite plan: which rows run in which job, and for how long.

One owner of the time budget of the suite.  Three things hang on one record of
how long each row took on the last run that finished it
(tests/harness/suite-timings.json, or the newest `suite-timings` artifact of
a run of the workflow):

  * the plan: the rows are split into the fewest jobs that each end well
    inside the limit, longest first, from the node ids the collection gives,
    so a new test file is placed with no change to the workflow;
  * the row clock: each row gets a budget of a few times its recorded
    seconds (row_budget_s), so a row that hangs costs its own budget and not
    the job;
  * the fitness check: the suite must still fit (fit_problems), so the day it
    grows past what the jobs can hold, a build says so in minutes instead of
    a job being cut after six hours.

One plan holds three layers (LAYERS): the ncurses rows of the suite's
selection, the X11 rows of tests/x11 and the native Wayland rows of
tests/wayland.  A layer is what its rows need to run and what they cost: its
own test workers, its own target and cut, its own most-shards, and its own
facts in the record, so a run of one layer never teaches the plan anything
about another.  A shard holds rows of one layer, and every shard of every
layer is run by the same script and cut by the same tool, so every one of
them leaves a junit and a status whatever happened.

Sub-commands (stdlib only, so the build job and `harness.py check` run them
bare):

    collect OUT.txt                  the node ids of every layer: the suite's
                                     selection plus every ncurses row a
                                     manifest maps, plus tests/x11 and
                                     tests/wayland
    record JUNIT[,JUNIT...] OUT.json [--previous OLD.json] [--collected IDS]
           [--host-kind seed|runner] [--scale N] [--settle-ms N]
           [--workers N] [--source TEXT] [--plan PLAN.json]
                                     the seconds of each row that passed; a
                                     row the run cut, skipped or failed keeps
                                     the time the previous record holds.  The
                                     facts of a layer this run passed at least
                                     one row of are set, the others are kept
    plan --record R.json --collected IDS.txt [--jobs-max N] [--target-min T]
         [--workers W] [--scale S] OUT.json
                                     the jobs, one args file each, next to
                                     OUT.  The three options override the
                                     numbers of every layer; left out, each
                                     layer plans with its own
    fit  --record R.json --collected IDS.txt [--jobs-max N] [--target-min T]
         [--workers W]               exit 1 when a layer no longer fits
    shards PLAN.json                 the matrix of a run, as JSON: one object
                                     a shard, with its layer and its limits
    verdict PLAN.json --status DIR --junit A,B[,..]
                                     one line for every row of the plan that
                                     failed or never ran; exit 1 when a row
                                     that gates is among them
    cut-min  [SHARD] [--plan PLAN.json]
                                     the minutes after which that shard is cut
    workers  [SHARD] [--plan PLAN.json]
                                     the test workers of that shard
    logs     SHARD --plan PLAN.json  `yes` when the shard keeps a log tree
    status OUT.json --shard K --exit RC --started T --ended T --ids N
           [--cut-report R.json] [--plan PLAN.json]
                                     how one job ended, for the gate
    fetch --repo R --branch B --out OUT.json [--settle-ms N]
                                     the newest unexpired `suite-timings`
                                     artifact of that branch (gh api), merged
                                     by layer into the tracked record: the
                                     layers it measured give their rows and
                                     their facts, every other layer keeps the
                                     tracked record's
    --selftest                       an over-full record, a four-hour row, a
                                     display layer over its most-shards and a
                                     row under no layer must all fail `fit`,
                                     and the ncurses args must keep a cut row
                                     from being run again

Exit: 0 done, 1 a finding (fit, verdict) or a wrong canary, 2 a bad command
line.
"""
import argparse
import heapq
import json
import math
import os
import statistics
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(os.path.dirname(HERE))

# The record a checkout carries, refreshed from the newest artifact whenever a
# commit lands that changes tests/.
TRACKED_RECORD = os.path.join(HERE, "suite-timings.json")
# A job reads another record than the tracked one from this variable (the
# build job hands the one its plan was made from to every shard).
RECORD_VARIABLE = "XWPE_SUITE_TIMINGS"
ARTIFACT_NAME = "suite-timings"
RECORD_VERSION = 2

# The layers of the one plan, in the order their shards are numbered.
NCURSES = "ncurses"
X11 = "x11"
WAYLAND = "wayland"
LAYER_ORDER = (NCURSES, X11, WAYLAND)
DISPLAY_LAYERS = (X11, WAYLAND)

# The ncurses layer: the most jobs of one run, and how long one may take (the
# plan's target).
MAX_JOBS = 8
SHARD_TARGET_MIN = 60
# Each job runs this many test workers (pytest -n).
SHARD_WORKERS = 4
# A job is cut by tests/harness/job_cut.py after this many minutes; the
# step's own limit and the job's limit sit above it, so GitHub never ends a
# job.
SHARD_CUT_MIN = 100
STEP_LIMIT_MIN = 110
JOB_LIMIT_MIN = 130
GITHUB_JOB_LIMIT_MIN = 360
# A display layer runs one pytest process, so the session's one Xvfb or weston
# serves every row of the shard and a row that fails under load still fails.
# Its target is below the ncurses one: a display shard is the longest thing a
# run waits for, and it is cut sooner so that its logs leave the runner.
DISPLAY_WORKERS = 1
DISPLAY_TARGET_MIN = 45
# The cut of a display shard is its target stretched by what the ncurses layer
# stretches its own (a 60 min target, cut at 100 min): a shard that grew by
# two thirds past its target is still ended by its own cut, with its junit and its
# logs, and never by GitHub's limit.  Derived from the target, so that moving
# the target moves the cut with it and the headroom between the two never
# changes by accident.
DISPLAY_CUT_MIN = DISPLAY_TARGET_MIN * SHARD_CUT_MIN // SHARD_TARGET_MIN
DISPLAY_JOBS_MAX = 4
# The margins above a cut: the grace of the cut and the steps after it (the
# log copy, the upload) fit in the first, and the job keeps the second to end
# its own steps.  Both are what the ncurses layer's limits already are.
STEP_MARGIN_MIN = STEP_LIMIT_MIN - SHARD_CUT_MIN
JOB_MARGIN_MIN = JOB_LIMIT_MIN - STEP_LIMIT_MIN
# The grace tests/ci-shard.sh gives a cut run, in minutes.
CUT_GRACE_MIN = 2
# The share of the collected rows the record must hold a time for, in each
# layer.  Accepted limit: the tracked record covers the ncurses tree with a
# margin of under one point (90.85 % against 90), so a few new files fail `fit`
# until the record is refreshed from a run.  No slack is added: slack would
# hide a stale record.
RECORD_COVER = 90
# A file that holds more than this share of one job's capacity is split into
# its rows, so that no single file decides the largest job.
SPLIT_SHARE = 0.5
# A record made on another machine than the runner is multiplied by this
# factor in the plan, until a runner artifact replaces it: the ncurses seed was
# made on a development machine, and the runner needed more than six hours for
# a suite that took four there (run 45065693614).
SEED_HOST_FACTOR = 1.5
# A display layer the record holds no seed of yet plans with this: the runner
# needed about three times the local minutes for the display suites.
UNMEASURED_SEED_FACTOR = 3.0
# The minutes the runner needed for one run of a display suite.  The layer's
# factor is this over the minutes its own seed holds (seed_factor), so a seed
# recorded at a wait scale is never stretched by that scale a second time.
# The seeds of 2026-11-25, one local run of each suite at
# XWPE_TEST_WAIT_SCALE=3 on rdaneel with one pytest process:
#   x11      104 runner min /  96.8 seed min = 1.08, rounded up to 1.5
#   wayland  110 runner min / 100.9 seed min = 1.09, rounded up to 1.5
# so the two layers plan about 145 and 151 min of runner work: four shards of
# their 45 min target each, which is the most a display layer may have.
X11_RUNNER_MIN = 104
WAYLAND_RUNNER_MIN = 110
HOST_SEED = "seed"
HOST_RUNNER = "runner"
# The row clock: a row may take this many times its recorded seconds, never
# less than the floor (both scaled), never more than the ceiling, and a row
# with no recorded time gets the unrecorded budget (scaled).
ROW_FLOOR_S = 60
ROW_FACTOR = 3
ROW_UNRECORDED_S = 300
ROW_CEILING_S = SHARD_CUT_MIN * 60 // 2
# A row with no time at all, in a suite with none (an empty record).
GUESS_ROW_S = 10.0

# How a layer's rows gate a run: every row of it, only the rows of the files
# GATE_FILES names, or the scenarios the harness maps (its own report is the
# gate then, and `verdict` shows the rows no scenario maps).
GATE_BY_ROW = "row"
GATE_BY_FILE = "file"
GATE_BY_SCENARIO = "scenario"
# The Wayland rows that gate: the deterministic resize and relayout parity
# rows.  The rest of that layer is shown, because a full run of it on a shared
# runner fails on timing alone and not on the code.
WAYLAND_GATE_FILES = ("tests/wayland/test_resize.py",
                      "tests/wayland/test_resize_mouse.py")
# The pytest options of every layer's args file, the ones only the ncurses
# layer adds, and the ones only a display layer adds.  pytest reads an args
# file one argument to a line, so an option and its value are two lines: on one
# line the option becomes an ini key with a leading blank and is silently lost.
COMMON_OPTIONS = ("-rf", "--durations=40")
RERUN_OPTIONS = ("--reruns", "2", "--reruns-delay", "3",
                 "--rerun-except", "RowOverBudget")
# A row that stops moving prints every thread's stack after this long.
FAULTHANDLER_TIMEOUT_S = 300
DISPLAY_OPTIONS = ("-o", "faulthandler_timeout=%d" % FAULTHANDLER_TIMEOUT_S)

LAYERS = {
    NCURSES: {
        # Not a folder: the selection, collected once by `collect`.
        "roots": ("tests",),
        "workers": SHARD_WORKERS,
        "reruns": True,
        "target_min": SHARD_TARGET_MIN,
        "cut_min": SHARD_CUT_MIN,
        "jobs_max": MAX_JOBS,
        "gating": GATE_BY_SCENARIO,
        "gate_files": (),
        "logs": False,
        "runner_min": None,
    },
    X11: {
        "roots": ("tests/x11",),
        "workers": DISPLAY_WORKERS,
        "reruns": False,
        "target_min": DISPLAY_TARGET_MIN,
        "cut_min": DISPLAY_CUT_MIN,
        "jobs_max": DISPLAY_JOBS_MAX,
        "gating": GATE_BY_ROW,
        "gate_files": (),
        "logs": True,
        "runner_min": X11_RUNNER_MIN,
    },
    WAYLAND: {
        "roots": ("tests/wayland",),
        "workers": DISPLAY_WORKERS,
        "reruns": False,
        "target_min": DISPLAY_TARGET_MIN,
        "cut_min": DISPLAY_CUT_MIN,
        "jobs_max": DISPLAY_JOBS_MAX,
        "gating": GATE_BY_FILE,
        "gate_files": WAYLAND_GATE_FILES,
        "logs": True,
        "runner_min": WAYLAND_RUNNER_MIN,
    },
}
# The longest root first, so a row of tests/x11 is in that layer and not in
# the one whose root is tests.
ROOTS = tuple(sorted(
    ((root, name) for name in LAYER_ORDER for root in LAYERS[name]["roots"]),
    key=lambda pair: -len(pair[0])))
# The facts of one layer a record holds: what the run that measured its seconds
# used, so that a runner record of one layer never turns the factor of another
# off.
LAYER_FACTS = ("host", "host_kind", "scale", "settle_ms", "workers", "source")

# The rows the ncurses job leaves out of its selection; the mapped ones among
# them run anyway (collect_ids).  One definition, here: the workflow names no
# selection, the build job's collection applies it once and the jobs run the
# node ids that collection gave.
SELECTION = (
    "not (_live or clangd or gopls or rust or scala or metals or python_lsp "
    "or go_dap or dap or algol68 or ga68 or truecolor_console or compile "
    "or make_output or build_cwd or user_screen or debugger_watchdog "
    "or ctrl_f9 or alt_f5)")
# What the ncurses collection leaves out: the display layers collect on their
# own, one pytest process each (two files of the same name in tests/x11 and
# tests/wayland cannot be collected by one).
SUITE_IGNORES = ("tests/x11", "tests/wayland")
CAPTURE_TIMEOUT_S = 600

SHARD_STATUS_NAME = "shard-%d-status.json"
SHARD_JUNIT_NAME = "suite-junit-%d.xml"
ARGS_NAME = "args-%d.txt"
CUT_EXIT = 124
# The cut tool kills a job the interrupt did not end and exits 128 + 9: the
# shard was cut all the same, a grace after its cut, and its junit holds
# whatever it had finished.
KILL_EXIT = 137
CUT_EXITS = (CUT_EXIT, KILL_EXIT)


# ---------------------------------------------------------------------------
# The record


def empty_layer_facts(layer):
    """The facts of @layer in a record that holds no row of it yet."""
    return {"host": "", "host_kind": HOST_SEED, "scale": 1, "settle_ms": 0,
            "workers": LAYERS[layer]["workers"], "source": ""}


def empty_record():
    """A record that holds no row yet: every layer's facts at their own
    defaults."""
    return {"version": RECORD_VERSION,
            "layers": {name: empty_layer_facts(name) for name in LAYER_ORDER},
            "rows": {}}


def upgraded_record(found):
    """@found, a record of any version, as a version 2 one.

    A version 1 record holds its facts at the top: they are the ncurses
    layer's, and the display layers keep their defaults, so a record made
    before the layers existed never says anything about a suite it never
    ran."""
    layers = {name: empty_layer_facts(name) for name in LAYER_ORDER}
    held = found.get("layers") or {}
    for name, facts in held.items():
        if name in layers and isinstance(facts, dict):
            layers[name].update({key: facts[key] for key in LAYER_FACTS
                                 if key in facts})
    if not held:
        layers[NCURSES].update({key: found[key] for key in LAYER_FACTS
                                if key in found})
    return {"version": RECORD_VERSION, "layers": layers,
            "rows": found.get("rows") or {}}


def load_record(path):
    """The record in the file at @path (empty_record() when there is none)."""
    if not path or not os.path.exists(path):
        return empty_record()
    with open(path, encoding="utf-8") as fh:
        return upgraded_record(json.load(fh))


def save_record(record, path):
    """Write @record, one row to a line so a refresh is a readable diff."""
    head = {key: value for key, value in record.items() if key != "rows"}
    lines = ["%s: %s" % (json.dumps(node), json.dumps(row))
             for node, row in sorted(record["rows"].items())]
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(json.dumps(head, sort_keys=True)[:-1])
        fh.write(', "rows": {\n' + ",\n".join(lines) + "\n}}\n")


def default_record_path():
    """The record a row clock reads: the one the job names, else the tracked
    one."""
    return os.environ.get(RECORD_VARIABLE) or TRACKED_RECORD


def layer_facts(record, layer):
    """The facts @record holds for @layer: its own, else the ncurses layer's
    (a caller that asks about a row of no layer gets the facts the suite has
    always planned with), else the defaults of an empty record."""
    name = layer if layer in LAYERS else NCURSES
    found = (record.get("layers") or {}).get(name)
    if not isinstance(found, dict):
        return empty_layer_facts(name)
    return found


def row_seconds(record, node_id):
    """The recorded seconds of @node_id, or None."""
    row = record["rows"].get(node_id)
    return None if row is None else row[0]


def layer_rows(record, layer):
    """The seconds of every row of @layer the record holds."""
    return [row[0] for node, row in record["rows"].items()
            if layer_of(node) == layer]


def round_up_to_half(number):
    """@number rounded up to the next half (2.1 -> 2.5, 2.0 -> 2.0)."""
    return math.ceil(number * 2) / 2.0


def seed_factor(record, layer):
    """What the plan multiplies a seed layer's seconds by: the minutes the
    runner needed for the layer over the minutes this record holds for it,
    rounded up to a half.

    Both are minutes of the same thing, so the wait scale the seed was recorded
    at is in the denominator and is not applied a second time: the plan's own
    stretch (make_plan) is what moves the record's scale to this run's.  A
    layer the record holds no row of has nothing to divide by and plans with
    the unmeasured factor."""
    runner_min = LAYERS[layer]["runner_min"]
    if runner_min is None:
        return SEED_HOST_FACTOR
    seconds = sum(layer_rows(record, layer))
    if seconds <= 0:
        return UNMEASURED_SEED_FACTOR
    return round_up_to_half(runner_min / (seconds / 60.0))


def host_factor(record, layer=None):
    """What the plan multiplies the record's seconds of @layer by: the seed
    factor for a layer measured off the runner, 1 for one the runner measured.

    With no layer, the ncurses layer's: a caller that asks about a row of no
    layer (a made-up id of a row of these tests) gets the factor the suite has
    always planned with."""
    facts = layer_facts(record, layer if layer in LAYERS else NCURSES)
    if facts.get("host_kind") == HOST_RUNNER:
        return 1.0
    return seed_factor(record, layer if layer in LAYERS else NCURSES)


def row_budget_s(record, node_id, scale):
    """The seconds a row may run before the row clock cuts it.

    A user who runs one row gets a hung row ended with its name and its
    budget instead of waiting for the whole job to be cut.  The budget is a
    few times what the row took last time, stretched when this run's wait
    scale is above the scale its own layer was recorded at (a sanitizer build,
    a slower machine), and never below a floor or above the ceiling.  The host
    factor of the plan does not apply here: the record's own scale is the only
    stretch."""
    seconds = row_seconds(record, node_id)
    if seconds is None:
        return min(ROW_UNRECORDED_S * scale, ROW_CEILING_S)
    recorded_scale = layer_facts(record, layer_of(node_id)).get("scale") or 1
    stretch = max(1.0, scale / recorded_scale)
    return min(max(ROW_FLOOR_S * scale, ROW_FACTOR * seconds * stretch),
               ROW_CEILING_S)


# ---------------------------------------------------------------------------
# Reading a junit file


def junit_cases(path):
    """[(classname, name, seconds, outcome, hostname)] of one junit file.

    The outcome is passed, failed, skipped or cut (a failure that names the
    row clock's RowOverBudget)."""
    cases = []
    root = ET.parse(path).getroot()
    for suite in root.iter("testsuite"):
        host = suite.get("hostname", "")
        for case in suite.iter("testcase"):
            failure = case.find("failure")
            error = case.find("error")
            problem = failure if failure is not None else error
            if problem is not None:
                said = (problem.get("message") or "") + (problem.text or "")
                outcome = "cut" if "RowOverBudget" in said else "failed"
            elif case.find("skipped") is not None:
                outcome = "skipped"
            else:
                outcome = "passed"
            cases.append((case.get("classname", ""), case.get("name", ""),
                          float(case.get("time") or 0), outcome, host))
    return cases


def resolve_id(classname, name, known):
    """The node id a junit (classname, name) stands for, or None.

    A classname is ambiguous: `tests.console.test_x` is a file and
    `tests.test_y.TestZ` is a file and a class.  Each split into a path and
    classes is tried, longest path first, against the collected ids (or, with
    no collection, against the files of the tree)."""
    parts = classname.split(".")
    for cut in range(len(parts), 0, -1):
        path = "/".join(parts[:cut]) + ".py"
        node = "::".join([path] + parts[cut:] + [name])
        if known is not None:
            if node in known:
                return node
        elif os.path.exists(os.path.join(TOP, path)):
            return node
    return None


def read_ids(path):
    """The node ids of an ids file, one to a line."""
    with open(path, encoding="utf-8") as fh:
        return [line.strip() for line in fh if line.strip()]


def merge_record(previous, cases, known, meta, workers_by_layer=None):
    """(record, matched, unmatched, measured) after one run's cases.

    The run's seconds replace those of the rows it passed.  Every other row
    keeps the time and source the previous record holds: a row the run cut,
    failed or skipped did not show how long it takes, and a row a cut job
    never reached is absent from the junit.  Learning from a cut would make
    the next budget three times the last one.

    pytest-rerunfailures writes every attempt of a row as its own <testcase>,
    and the attempts it retried carry no failure child.  So the cases are
    grouped by node id first: a row any attempt of which failed or was cut
    keeps its old time, and a row that passed on a rerun learns the time of
    its last attempt.

    @meta is what this run measured with.  It becomes the facts of every layer
    of which the run passed at least one row (@measured names them); a layer
    it passed nothing of keeps the facts the previous record holds, so a run of
    one suite never claims to have measured another on this host at this scale.
    @workers_by_layer is the plan's workers a layer, else @meta's."""
    record = upgraded_record(previous)
    rows = dict(record["rows"])
    matched = 0
    unmatched = []
    attempts = {}
    for classname, name, seconds, outcome, _host in cases:
        node = resolve_id(classname, name, known)
        if node is None:
            unmatched.append("%s::%s" % (classname, name))
            continue
        matched += 1
        attempts.setdefault(node, []).append((seconds, outcome))
    measured = set()
    for node, tries in attempts.items():
        if all(outcome == "passed" for _seconds, outcome in tries):
            rows[node] = [round(tries[-1][0], 3), meta["source"]]
            measured.add(layer_of(node))
    layers = dict(record["layers"])
    for name in sorted(measured):
        if name not in LAYERS:
            continue
        facts = dict(meta)
        facts["workers"] = (workers_by_layer or {}).get(name, meta["workers"])
        layers[name] = facts
    record["layers"] = layers
    record["rows"] = rows
    return record, matched, unmatched, sorted(measured)


def read_plan(path):
    """The plan file at @path, or None when it is not there or is not JSON."""
    if not path or not os.path.isfile(path):
        return None
    try:
        with open(path, encoding="utf-8") as fh:
            found = json.load(fh)
    except (OSError, ValueError):
        return None
    return found if isinstance(found, dict) else None


def plan_workers(plan):
    """{layer: workers} of a plan's shards (empty when there is no plan)."""
    found = {}
    for shard in (plan or {}).get("shards", []):
        if shard.get("layer") in LAYERS and shard.get("workers"):
            found.setdefault(shard["layer"], shard["workers"])
    return found


def cmd_record(args):
    """Write the record a run's junit file(s) give, merged into the previous."""
    previous = load_record(args.previous)
    known = set(read_ids(args.collected)) if args.collected else None
    cases = []
    hosts = set()
    for path in args.junit.split(","):
        if path and os.path.exists(path):
            found = junit_cases(path)
            cases.extend(found)
            hosts.update(case[4] for case in found if case[4])
    meta = {"host": ",".join(sorted(hosts))
                   or layer_facts(previous, NCURSES).get("host", ""),
            "host_kind": args.host_kind,
            "scale": args.scale, "settle_ms": args.settle_ms,
            "workers": args.workers, "source": args.source}
    record, matched, unmatched, measured = merge_record(
        previous, cases, known, meta, plan_workers(read_plan(args.plan)))
    save_record(record, args.out)
    cover = ""
    if known:
        held = sum(1 for node in known if node in record["rows"])
        cover = ", %.1f%% of the %d collected rows hold a time" % (
            100.0 * held / len(known), len(known))
    print("record: %d cases read, %d matched a row, %d unmatched%s"
          % (len(cases), matched, len(unmatched), cover))
    print("record: layers measured by this run: %s"
          % (", ".join(measured) if measured else "none, every layer keeps its facts"))
    for line in unmatched[:20]:
        print("record: unmatched %s" % line, file=sys.stderr)
    return 0


# ---------------------------------------------------------------------------
# The plan


def file_of(node_id):
    """``tests/x.py::T::t[p]`` -> ``tests/x.py``."""
    return node_id.split("::", 1)[0]


def layer_of(node_id):
    """The layer a row runs in, named by the path of its file: the display
    folders are their own layers, every other row under tests/ is ncurses.
    None for a path no layer holds (a C unit's name, a file outside the
    suite), which the plan refuses to place."""
    path = file_of(node_id)
    for root, name in ROOTS:
        if path == root or path.startswith(root + "/"):
            return name
    return None


def gates(node_id):
    """True when a row of @node_id that failed or never ran fails the run:
    every X11 row, and a Wayland row of one of that layer's gate files.

    A ncurses row gates too: a scenario of the harness maps it and the
    harness's own report is what judges it, so the two never disagree about a
    row that gates.  A row of no layer (a C unit's) gates as it always has."""
    layer = layer_of(node_id)
    if layer is None:
        return True
    spec = LAYERS[layer]
    if spec["gating"] == GATE_BY_FILE:
        return file_of(node_id) in spec["gate_files"]
    return True


def plan_gates(node_id):
    """True when @node_id's failure fails the run and the plan's verdict is the
    one that says so.

    A ncurses row is left to the harness's report, which judges the scenarios
    that map it: a row no scenario maps does not gate a run today, and making
    one gate is a decision for the user, not for this file.  So the verdict
    shows every ncurses row and gates on the display layers' own."""
    layer = layer_of(node_id)
    return (layer is not None and LAYERS[layer]["gating"] != GATE_BY_SCENARIO
            and gates(node_id))


def rows_by_layer(ids):
    """({layer: [node id]}, [node id no layer holds]) of @ids."""
    by_layer = {name: [] for name in LAYER_ORDER}
    unplaced = []
    for node in ids:
        name = layer_of(node)
        if name is None:
            unplaced.append(node)
        else:
            by_layer[name].append(node)
    return by_layer, unplaced


class Estimator:
    """How many seconds a row is expected to take: its recorded time, else
    the median of its file's recorded rows, else the median of its layer's,
    else a guess.  Each is multiplied by the plan's factor.

    The medians are the layer's own: a display row the record never saw is
    estimated from the display rows it holds, never from a ncurses row, whose
    seconds say nothing about a row that paints on a display."""

    def __init__(self, record, ids, factor, layer=None):
        self.rows = record["rows"]
        self.factor = factor
        held = {node: row[0] for node, row in self.rows.items()
                if layer is None or layer_of(node) == layer}
        by_file = {}
        for node in ids:
            if node in held:
                by_file.setdefault(file_of(node), []).append(held[node])
        self.file_median = {f: statistics.median(v) for f, v in by_file.items()}
        times = list(held.values())
        self.layer_median = statistics.median(times) if times else GUESS_ROW_S

    def seconds(self, node):
        """The estimate for @node, in seconds on the runner."""
        row = self.rows.get(node)
        if row is not None:
            base = row[0]
        else:
            base = self.file_median.get(file_of(node), self.layer_median)
        return base * self.factor


def plan_items(ids, estimator, capacity_s):
    """[(seconds, [node ids])]: a file is one item, unless it holds more than
    SPLIT_SHARE of one job's capacity, when each of its rows is one."""
    by_file = {}
    for node in ids:
        by_file.setdefault(file_of(node), []).append(node)
    items = []
    for path in sorted(by_file):
        rows = by_file[path]
        total = sum(estimator.seconds(node) for node in rows)
        if total > capacity_s * SPLIT_SHARE:
            items.extend((estimator.seconds(node), [node]) for node in rows)
        else:
            items.append((total, rows))
    return items


def split_longest_first(items, jobs):
    """The items dealt into @jobs bins, longest first, each to the bin that
    holds the least: [(total seconds, [node ids])]."""
    heap = [(0.0, index, []) for index in range(jobs)]
    heapq.heapify(heap)
    for seconds, rows in sorted(items, key=lambda item: (-item[0], item[1][0])):
        load, index, held = heapq.heappop(heap)
        heapq.heappush(heap, (load + seconds, index, held + rows))
    return [(load, held) for load, _index, held in sorted(heap, key=lambda b: b[1])]


def shard_limits(cut_min):
    """(the step's minutes, the job's minutes) above a cut of @cut_min: the
    step leaves the cut its grace and the steps that follow it, and the job
    leaves the step its own end, so GitHub never ends a job."""
    step_min = cut_min + STEP_MARGIN_MIN
    return step_min, step_min + JOB_MARGIN_MIN


def layer_options(layer, workers):
    """The pytest options of a shard's args file, one token to a line.

    The ncurses layer runs its rows on @workers workers and runs a row that
    failed once again, except a row its own clock cut: a rerun of that one
    costs its whole budget a second time.  A display layer runs one pytest
    process, so the session's one Xvfb or weston serves every row of the
    shard, and runs every row once, because a retry turns a row that fails
    under load into a pass and hides the failure it names (the display
    conftests refuse --reruns themselves).  A display row that stops moving
    prints every thread's stack after faulthandler_timeout."""
    if LAYERS[layer]["reruns"]:
        return list(COMMON_OPTIONS) + ["-n", str(workers)] + list(RERUN_OPTIONS)
    return list(COMMON_OPTIONS) + list(DISPLAY_OPTIONS)


def ordered_rows(layer, rows):
    """The rows of one shard in the order its args file holds them: the rows of
    the layer's gate files first, so that a compositor which dies later in the
    shard cannot take a gating row down with it, and every other row by name."""
    spec = LAYERS[layer]
    if spec["gating"] != GATE_BY_FILE:
        return sorted(rows)
    named = sorted(rows)
    gating = {node for node in named if file_of(node) in spec["gate_files"]}
    return sorted(gating) + [node for node in named if node not in gating]


def empty_plan():
    """A plan that holds no shard yet."""
    return {"version": RECORD_VERSION, "jobs": 0, "fits": True, "layers": {},
            "shards": [], "assignment": {}, "unplaced": []}


def plan_layer(plan, record, layer, ids, jobs_max=None, target_min=None,
               workers=None, scale=None):
    """Add @layer's shards to @plan, numbered after the shards it holds, and
    its summary to plan["layers"]; returns the summary.

    The layer is planned with its own target, workers and most-shards unless
    the caller names others: the fewest shards whose largest bin, over the
    workers, ends inside the target.  @scale is this run's wait scale, which
    stretches a record made at another one."""
    spec = LAYERS[layer]
    jobs_max = spec["jobs_max"] if jobs_max is None else jobs_max
    target_min = spec["target_min"] if target_min is None else target_min
    workers = spec["workers"] if workers is None else workers
    facts = layer_facts(record, layer)
    factor = host_factor(record, layer)
    if scale is not None:
        factor *= scale / (facts.get("scale") or 1)
    estimator = Estimator(record, ids, factor, layer)
    capacity_s = workers * target_min * 60
    items = plan_items(ids, estimator, capacity_s)
    chosen = []
    for jobs in range(1, jobs_max + 1):
        chosen = split_longest_first(items, jobs)
        if max(load for load, _rows in chosen) <= capacity_s:
            break
    longest = max((load for load, _rows in chosen), default=0.0)
    fits = longest <= capacity_s
    cut_min = spec["cut_min"]
    step_min, job_min = shard_limits(cut_min)
    first = len(plan["shards"])
    for offset, (load, rows) in enumerate(chosen):
        index = first + offset
        held = ordered_rows(layer, rows)
        plan["shards"].append({
            "index": index, "layer": layer, "ids": len(held),
            "args": ARGS_NAME % index,
            "predicted_min": round(load / workers / 60.0, 1),
            "workers": workers, "cut_min": cut_min, "step_min": step_min,
            "job_min": job_min, "logs": spec["logs"], "rows": held})
        for node in held:
            plan["assignment"][node] = index
    summary = {"rows": len(ids), "jobs": len(chosen), "jobs_max": jobs_max,
               "workers": workers, "target_min": target_min, "cut_min": cut_min,
               "step_min": step_min, "job_min": job_min, "logs": spec["logs"],
               "gating": spec["gating"], "factor": round(factor, 3),
               "predicted_min": round(longest / workers / 60.0, 1),
               "fits": fits, "record_host": facts.get("host", ""),
               "record_host_kind": facts.get("host_kind", HOST_SEED),
               "record_scale": facts.get("scale", 1)}
    plan["layers"][layer] = summary
    plan["fits"] = plan["fits"] and fits
    return summary


def make_plan(record, ids, jobs_max=None, target_min=None, workers=None,
              scale=None):
    """The plan: every layer's rows in the fewest shards whose largest bin ends
    inside that layer's own target, the shards of all the layers numbered in
    one sequence.

    @jobs_max, @target_min and @workers name the numbers of every layer when
    the caller gives them (a row of these tests plans one layer under numbers
    of its own); left out, each layer plans with its own."""
    plan = empty_plan()
    by_layer, unplaced = rows_by_layer(ids)
    plan["unplaced"] = sorted(unplaced)
    for name in LAYER_ORDER:
        if by_layer[name]:
            plan_layer(plan, record, name, by_layer[name], jobs_max, target_min,
                       workers, scale)
    plan["jobs"] = len(plan["shards"])
    return plan


def write_plan(plan, out):
    """Write the plan to @out and each shard's args file next to it: the
    layer's pytest options first, then its node ids, one token to a line,
    which pytest reads with @file."""
    folder = os.path.dirname(os.path.abspath(out))
    os.makedirs(folder, exist_ok=True)
    listed = dict(plan)
    listed["shards"] = []
    for shard in plan["shards"]:
        lines = layer_options(shard["layer"], shard["workers"]) + shard["rows"]
        with open(os.path.join(folder, shard["args"]), "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines) + "\n")
        listed["shards"].append({k: v for k, v in shard.items() if k != "rows"})
    with open(out, "w", encoding="utf-8") as fh:
        json.dump(listed, fh, indent=1, sort_keys=True)
        fh.write("\n")


def plan_summary(plan):
    """The plan in a few lines for a log."""
    lines = ["plan: %d jobs over %d layer(s)" % (plan["jobs"], len(plan["layers"]))]
    for name in LAYER_ORDER:
        if name not in plan["layers"]:
            continue
        spec = plan["layers"][name]
        lines.append("plan: %s: %d job(s) of at most %d, %d worker(s), target "
                     "%d min, cut at %d min (step %d, job %d), longest %.1f min, "
                     "factor %.2f (record host %s, %s, scale %s)"
                     % (name, spec["jobs"], spec["jobs_max"], spec["workers"],
                        spec["target_min"], spec["cut_min"], spec["step_min"],
                        spec["job_min"], spec["predicted_min"], spec["factor"],
                        spec["record_host"] or "?", spec["record_host_kind"],
                        spec["record_scale"]))
    for shard in plan["shards"]:
        lines.append("plan: shard %d (%s): %d rows, predicted %.1f min%s"
                     % (shard["index"], shard["layer"], shard["ids"],
                        shard["predicted_min"],
                        ", keeps a log tree" if shard["logs"] else ""))
    for node in plan["unplaced"]:
        lines.append("plan: %s is under no layer's roots, so no shard runs it"
                     % node)
    if not plan["fits"]:
        lines.append("plan: a layer does not fit in the shards it may have")
    return "\n".join(lines)


def cmd_plan(args):
    """Write the plan and its args files, and print the plan."""
    record = load_record(args.record)
    ids = read_ids(args.collected)
    plan = make_plan(record, ids, args.jobs_max, args.target_min, args.workers,
                     args.scale)
    write_plan(plan, args.out)
    print(plan_summary(plan))
    return 1 if plan["unplaced"] else 0


# ---------------------------------------------------------------------------
# The fitness check


def limit_problems(shard):
    """The finding of a shard whose minutes do not rise: the cut, its grace
    inside the step's limit, then the job's, then GitHub's own."""
    cut, step, job = shard["cut_min"], shard["step_min"], shard["job_min"]
    if cut + CUT_GRACE_MIN < step < job < GITHUB_JOB_LIMIT_MIN:
        return []
    return ["fit: shard %d (%s): the cut (%d), the step (%d), the job (%d) and "
            "GitHub's limit (%d) minutes must rise in that order, the cut's "
            "grace of %d min inside the step"
            % (shard["index"], shard["layer"], cut, step, job,
               GITHUB_JOB_LIMIT_MIN, CUT_GRACE_MIN)]


def fit_problems(record, ids, jobs_max=None, target_min=None, workers=None):
    """The findings, as sentences with their numbers, when a layer does not
    fit: its largest shard at its most shards over its own target, a row longer
    than that target, a record that holds a time for too few of its rows, a
    shard whose limits do not rise above its cut, and a row no layer holds."""
    problems = []
    if not ids:
        return ["fit: no row was collected, the plan has nothing to place"]
    plan = make_plan(record, ids, jobs_max, target_min, workers)
    by_layer, _unplaced = rows_by_layer(ids)
    for node in plan["unplaced"]:
        problems.append("fit: %s is under no layer's roots, so no shard runs it"
                        % node)
    for name in LAYER_ORDER:
        spec = plan["layers"].get(name)
        if spec is None:
            continue
        target = spec["target_min"]
        if not spec["fits"]:
            problems.append(
                "fit: %s: the largest of %d jobs is predicted at %.1f min, over "
                "the %d min target" % (name, spec["jobs"], spec["predicted_min"],
                                       target))
        for node, row in sorted(record["rows"].items()):
            if layer_of(node) != name:
                continue
            if row[0] * spec["factor"] / 60.0 > target:
                problems.append(
                    "fit: %s took %.0f s (%.1f min with the host factor), over the "
                    "%d min target of %s: it cannot fit in any job"
                    % (node, row[0], row[0] * spec["factor"] / 60.0, target, name))
        held = sum(1 for node in by_layer[name] if node in record["rows"])
        cover = 100.0 * held / len(by_layer[name])
        if cover < RECORD_COVER:
            problems.append(
                "fit: %s: the record holds a time for %.1f%% of the %d collected "
                "rows, under the %d%% the estimate needs"
                % (name, cover, len(by_layer[name]), RECORD_COVER))
    for shard in plan["shards"]:
        problems.extend(limit_problems(shard))
    return problems


def cmd_fit(args):
    """Print the findings; exit 1 when there are any."""
    record = load_record(args.record)
    problems = fit_problems(record, read_ids(args.collected), args.jobs_max,
                            args.target_min, args.workers)
    for line in problems:
        print(line)
    if not problems:
        print("fit: every layer fits in the shards it may have")
    return 1 if problems else 0


def fake_record(seconds_by_row, host_kind=HOST_RUNNER):
    """A record of @seconds_by_row, for the self-test and the rows: every
    layer's facts say the same host kind."""
    record = empty_record()
    for name in LAYER_ORDER:
        record["layers"][name]["host_kind"] = host_kind
    record["rows"] = {node: [seconds, "fake"] for node, seconds in seconds_by_row.items()}
    return record


def option_problems():
    """The findings of the layers' pytest options: the ncurses args must keep a
    row the clock cut from running again, and a display layer's must run one
    pytest process, every row once, and ask for the stack of a row that stops
    moving."""
    problems = []
    ncurses = layer_options(NCURSES, SHARD_WORKERS)
    if "--rerun-except" not in ncurses or "RowOverBudget" not in ncurses:
        problems.append("options: the ncurses args lack --rerun-except "
                        "RowOverBudget, so a row the clock cut runs again")
    for name in DISPLAY_LAYERS:
        display = layer_options(name, DISPLAY_WORKERS)
        if "-n" in display or "--reruns" in display:
            problems.append("options: the %s args run workers or reruns, which "
                            "its conftest refuses" % name)
        if "faulthandler_timeout=%d" % FAULTHANDLER_TIMEOUT_S not in display:
            problems.append("options: the %s args never ask for the stack of a "
                            "row that stops moving" % name)
    return problems


def selftest_checks():
    """[(what must hold, the findings, the phrase one of them must hold)]: a
    None phrase is a check that must give no finding at all."""
    ids = ["tests/test_f%d.py::t%d" % (n, n) for n in range(40)]
    fits = fake_record({node: 60 for node in ids})
    over_full = fake_record({node: 60 * 60 * 3 for node in ids})
    one_long = fake_record({node: 60 for node in ids})
    one_long["rows"][ids[0]] = [4 * 3600, "fake"]
    display_rows = ["tests/x11/test_f%d.py::t%d" % (n, n) for n in range(8)]
    heavy_display = fake_record({node: 60 * 60 for node in display_rows})
    flat_limits = {"index": 0, "layer": NCURSES, "cut_min": SHARD_CUT_MIN,
                   "step_min": SHARD_CUT_MIN, "job_min": JOB_LIMIT_MIN}
    return [
        ("a record that fits is accepted", fit_problems(fits, ids), None),
        ("an over-full record is refused", fit_problems(over_full, ids), "largest"),
        ("a four-hour row is refused", fit_problems(one_long, ids), "cannot fit"),
        ("a display layer over the shards it may have is refused",
         fit_problems(heavy_display, display_rows), "x11"),
        ("a row no layer holds is refused",
         fit_problems(fits, ids + ["wpe.c::a_case"]), "no layer"),
        ("a shard whose limits do not rise above its cut is refused",
         limit_problems(flat_limits), "must rise"),
        ("the args files run every row once and a cut row never again",
         option_problems(), None),
    ]


def selftest():
    """True when the planner refuses every plan it must and accepts what it
    must accept."""
    ok = True
    for what, found, phrase in selftest_checks():
        holds = not found if phrase is None else any(
            phrase in line for line in found)
        if not holds:
            sys.stderr.write("suite_plan: %s (found: %s)\n" % (what, found[:2]))
            ok = False
    return ok


# ---------------------------------------------------------------------------
# The ids of the selection


class CollectionError(RuntimeError):
    """pytest could not collect every row it was asked for."""


# pytest's own exit codes: 0 collected, 5 nothing matched.  Any other one
# (2 interrupted by a collection error, 3 internal error, 4 usage) means the
# list of ids is short, so it must not be planned from.
COLLECT_OK_EXITS = (0, 5)
COLLECT_ERROR_LINES = 40


def run_collect(arguments):
    """The node ids pytest collects for @arguments (the suite's own
    interpreter, from the tree's top).  A collection that ended in anything but
    0 or 5 raises CollectionError with pytest's own words: a file that fails to
    import drops all its rows from the plan, and the plan would look smaller
    and green."""
    done = subprocess.run(
        [sys.executable, "-m", "pytest", "--collect-only", "-q",
         "-p", "no:cacheprovider"] + list(arguments),
        cwd=TOP, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        timeout=CAPTURE_TIMEOUT_S)
    if done.returncode not in COLLECT_OK_EXITS:
        said = [line for line in (done.stdout + done.stderr).splitlines()
                if "::" not in line or "ERROR" in line]
        raise CollectionError(
            "pytest --collect-only ended with exit %d; the rows of a file that "
            "does not collect are missing:\n%s"
            % (done.returncode, "\n".join(said[-COLLECT_ERROR_LINES:])))
    return [line.strip() for line in done.stdout.splitlines() if "::" in line]


def import_harness():
    """The harness module, which this file is imported by (it is read only
    here, inside a call, so the two never import each other at import time)."""
    sys.path.insert(0, HERE)
    try:
        import harness
    finally:
        sys.path.pop(0)
    return harness


def mapped_ncurses_ids():
    """The test ids every manifest maps in the ncurses layer (and the
    config-file one that runs with it), live and blocked rows left out."""
    harness = import_harness()
    sys.path.insert(0, os.path.dirname(HERE))
    try:
        import engine_guard
    finally:
        sys.path.pop(0)
    mapped = []
    for entry in sorted(os.listdir(HERE)):
        if not os.path.exists(harness.manifest_path(entry)):
            continue
        manifest = harness.load_manifest(entry)
        for scenario in harness.scenarios_for_layer(manifest, "ncurses"):
            test_id = scenario.get("test")
            if (test_id and scenario["status"] != "blocked"
                    and not engine_guard.is_live_file(harness.test_file(test_id))):
                mapped.append(test_id)
    return mapped


def ncurses_ids():
    """The node ids of the ncurses layer: the selection, plus every row a
    manifest maps in that layer that the selection leaves out, so the gate has
    nothing to top up."""
    ignores = ["--ignore=%s" % path for path in SUITE_IGNORES]
    ids = run_collect(["tests"] + ignores + ["-k", SELECTION])
    mapped = mapped_ncurses_ids()
    files = sorted({m.split("::", 1)[0] for m in mapped
                    if os.path.exists(os.path.join(TOP, m.split("::", 1)[0]))
                    and not m.startswith(SUITE_IGNORES)})
    held = set(ids)
    harness = import_harness()
    for node in run_collect(files) if files else []:
        if node not in held and any(harness.row_covers(m, node) for m in mapped):
            ids.append(node)
            held.add(node)
    return ids


def display_ids(layer):
    """The node ids of a display layer: every row of the folder it names,
    collected on its own.  One pytest cannot collect two layers of them: a file
    of tests/x11 and one of tests/wayland carry the same name, and pytest reads
    a test file as a module of that name."""
    return run_collect(list(LAYERS[layer]["roots"]))


def collect_ids():
    """The node ids the jobs run: every layer's rows."""
    ids = ncurses_ids()
    for layer in DISPLAY_LAYERS:
        ids.extend(display_ids(layer))
    return ids


def cmd_collect(args):
    """Write the node ids of the selection to a file, one to a line."""
    try:
        ids = collect_ids()
    except CollectionError as err:
        print("collect: %s" % err, file=sys.stderr)
        return 1
    with open(args.out, "w", encoding="utf-8") as fh:
        fh.write("\n".join(ids) + "\n")
    print("collect: %d node ids" % len(ids))
    return 0


# ---------------------------------------------------------------------------
# What a job writes about how it ended


def read_cut_report(path):
    """The report tests/harness/job_cut.py wrote about a job's end, or None
    when there is none (the tool never ran or died before it wrote)."""
    if not path or not os.path.isfile(path):
        return None
    try:
        with open(path, encoding="utf-8") as fh:
            report = json.load(fh)
    except (OSError, ValueError):
        return None
    return report if isinstance(report, dict) else None


def cut_facts(exit_code, report):
    """(cut, killed) of a job: what the cut tool says it did, else the exit
    code alone (124 a cut, 137 a cut that was killed).

    The report is the fact: a command that died of a signal of its own (the
    kernel's OOM killer) before the deadline also ends 137, and only the
    tool can tell it from a cut.  A status written before the tool said
    whether it killed holds `cut` and no `killed`: the exit code then says
    it (137), so a shard that was killed is never read as one that ended in
    its grace."""
    if report is not None and "cut" in report:
        if "killed" not in report:
            return bool(report["cut"]), exit_code == KILL_EXIT
        return bool(report["cut"]), bool(report["killed"])
    return exit_code in CUT_EXITS, exit_code == KILL_EXIT


def shard_of_plan(plan, shard):
    """The plan's entry for shard number @shard, or None when the plan does not
    hold it (a run of the script with no plan of its own)."""
    for entry in (plan or {}).get("shards", []):
        if entry.get("index") == shard:
            return entry
    return None


def shard_fact(plan, shard, key, default):
    """One fact of a shard from @plan, or @default when the plan holds no such
    shard."""
    entry = shard_of_plan(plan, shard)
    return default if entry is None else entry.get(key, default)


def cmd_status(args):
    """Write how a shard's pytest ended: the exit code, whether the job was
    cut and whether the cut had to kill it (from the cut tool's report when it
    left one, else from the exit code: 124 and 137), the layer it ran and the
    minutes it was cut after (both from the plan it was run from), the times,
    the number of rows it was given."""
    cut, killed = cut_facts(args.exit, read_cut_report(args.cut_report))
    plan = read_plan(args.plan)
    status = {"shard": args.shard, "exit": args.exit,
              "cut": cut, "killed": killed,
              "layer": shard_fact(plan, args.shard, "layer", NCURSES),
              "cut_min": shard_fact(plan, args.shard, "cut_min", SHARD_CUT_MIN),
              "started": args.started,
              "ended": args.ended, "ids": args.ids}
    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(status, fh, indent=1, sort_keys=True)
        fh.write("\n")
    return 0


def read_statuses(folder):
    """{shard: status} of every shard status file in @folder."""
    found = {}
    if not folder or not os.path.isdir(folder):
        return found
    for name in sorted(os.listdir(folder)):
        if name.startswith("shard-") and name.endswith("-status.json"):
            with open(os.path.join(folder, name), encoding="utf-8") as fh:
                status = json.load(fh)
            found[status["shard"]] = status
    return found


def shard_reason(shard, statuses):
    """Why a row of @shard has no result, in the words the gate prints."""
    status = statuses.get(shard)
    if status is None:
        return "shard %d uploaded nothing" % shard
    cut, killed = cut_facts(status["exit"], status)
    if cut and not killed:
        return "not run: shard %d was cut at %d min" % (shard, status["cut_min"])
    if cut:
        return ("not run: shard %d was cut at %d min and killed, for the "
                "interrupt did not end it" % (shard, status["cut_min"]))
    if status["exit"] in (0, 1):
        return "shard %d finished (exit %d) with no result for it" % (shard, status["exit"])
    return "shard %d ended (exit %d) before running it" % (shard, status["exit"])


def cmd_shards(args):
    """Print the matrix of a run as a JSON list: one object a shard, with the
    layer it runs and the limits its step and its job get."""
    plan = read_plan(args.plan)
    if plan is None:
        sys.stderr.write("shards: %s holds no plan\n" % args.plan)
        return 2
    print(json.dumps([{"shard": shard["index"], "layer": shard["layer"],
                       "step_min": shard["step_min"], "job_min": shard["job_min"]}
                      for shard in plan["shards"]]))
    return 0


def cmd_cut_min(args):
    """Print the minutes after which a shard is cut: its own layer's."""
    print(shard_fact(read_plan(args.plan), args.shard, "cut_min", SHARD_CUT_MIN))
    return 0


def cmd_workers(args):
    """Print the test workers of a shard: its own layer's."""
    print(shard_fact(read_plan(args.plan), args.shard, "workers", SHARD_WORKERS))
    return 0


def cmd_logs(args):
    """Print whether a shard keeps a log tree of its rows (`yes` or `no`): a
    display shard does, and the script gives it a --basetemp of its own and
    copies the tree out for the upload."""
    keeps = shard_fact(read_plan(args.plan), args.shard, "logs", False)
    print("yes" if keeps else "no")
    return 0


# ---------------------------------------------------------------------------
# The verdict of a run


# Worst first: the outcome of a row pytest ran more than once (a rerun of a
# row that failed) is the worst its attempts had.
OUTCOME_RANK = {"failed": 0, "cut": 1, "skipped": 2, "passed": 3}


def junit_outcomes(paths, known):
    """{node id: outcome} of the junit files @paths names (comma separated),
    for the node ids @known holds: the worst outcome any attempt of the row
    had.  A case that names no id of the plan is not one of its rows."""
    found = {}
    for path in (paths or "").split(","):
        if not path or not os.path.exists(path):
            continue
        for classname, name, _seconds, outcome, _host in junit_cases(path):
            node = resolve_id(classname, name, known)
            if node is None:
                continue
            seen = found.get(node)
            if seen is None or OUTCOME_RANK[outcome] < OUTCOME_RANK[seen]:
                found[node] = outcome
    return found


def verdict_lines(plan, statuses, outcomes):
    """[(it gates the run, the line)] for every row of @plan that failed or
    never ran: its layer, its shard, and why a row that never ran did not."""
    lines = []
    for node, shard in sorted(plan["assignment"].items()):
        outcome = outcomes.get(node)
        if outcome in ("passed", "skipped"):
            continue
        layer = shard_fact(plan, shard, "layer", layer_of(node) or NCURSES)
        if outcome is None:
            said = "MISSING %s, %s" % (node, shard_reason(shard, statuses))
        else:
            said = "%s %s" % (outcome.upper(), node)
        lines.append((plan_gates(node), "%s shard %d: %s" % (layer, shard, said)))
    return lines


def cmd_verdict(args):
    """Print one line for every row of the plan that failed or never ran, and
    exit 1 when a row that gates the run is among them.

    The gate of a run reads this after every shard has uploaded: a row that
    failed on a display layer fails the run here, a row that never ran says
    which shard held it and why it did not run, and a row the plan only shows
    (a ncurses row, which the harness's own report judges, and a wayland row
    outside the layer's gate files) is a warning line the summary counts."""
    plan = read_plan(args.plan)
    if plan is None:
        sys.stderr.write("verdict: %s holds no plan\n" % args.plan)
        return 2
    statuses = read_statuses(args.status)
    outcomes = junit_outcomes(args.junit, set(plan["assignment"]))
    found = verdict_lines(plan, statuses, outcomes)
    gating = [line for gates_the_run, line in found if gates_the_run]
    shown = [line for gates_the_run, line in found if not gates_the_run]
    for line in gating:
        print("verdict: %s" % line)
    for line in shown:
        print("::warning::verdict: %s" % line)
    print("verdict: %d row(s) failed or never ran, %d of them gate the run, "
          "%d are shown" % (len(found), len(gating), len(shown)))
    return 1 if gating else 0


# ---------------------------------------------------------------------------
# The newest record of the branch


def newest_artifact(listing, branch):
    """The newest unexpired `suite-timings` artifact of @branch in a
    `gh api .../actions/artifacts` answer, or None.  A run that was cut or
    failed may hold none, so the newest completed run is not the answer."""
    best = None
    for artifact in listing.get("artifacts", []):
        run = artifact.get("workflow_run") or {}
        if (artifact.get("name") != ARTIFACT_NAME or artifact.get("expired")
                or run.get("head_branch") != branch):
            continue
        if best is None or artifact["created_at"] > best["created_at"]:
            best = artifact
    return best


def gh_json(path):
    """The JSON `gh api` answers for @path (GH_TOKEN names the token)."""
    done = subprocess.run(["gh", "api", path], stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, check=True, timeout=120)
    return json.loads(done.stdout)


def gh_zip(path, out):
    """Download the zip `gh api` gives for @path into the file @out."""
    with open(out, "wb") as fh:
        subprocess.run(["gh", "api", path], stdout=fh, check=True, timeout=300)


def normalise_settle(record, settle_ms):
    """@record with each layer's seconds stretched to the settle of this run.

    A run that settles longer after each key takes longer per row, so a record
    made at a shorter settle would plan too few jobs.  A layer whose recorded
    settle is unknown (0), a run with none, or a longer recorded settle is left
    as it is (a shorter plan than the record is never made up), and a layer
    whose rows never read the settle (a display layer) is left alone with it.
    The scale a layer was recorded at is dealt with by the plan and the row
    clock, not here."""
    found = upgraded_record(record)
    layers = found["layers"]
    stretch = {name: float(settle_ms) / (facts.get("settle_ms") or 0)
               for name, facts in layers.items()
               if facts.get("settle_ms") and settle_ms
               and settle_ms > facts["settle_ms"]}
    if not stretch:
        return found
    stretched = dict(found)
    stretched["rows"] = {
        node: [round(row[0] * stretch.get(layer_of(node), 1.0), 3)] + list(row[1:])
        for node, row in found["rows"].items()}
    stretched["layers"] = {
        name: dict(facts, settle_ms=settle_ms) if name in stretch else dict(facts)
        for name, facts in layers.items()}
    return stretched


def measured_layers(record):
    """The sorted names of the layers @record holds a row of: the layers the
    run that wrote it measured.  A layer it holds no row of says nothing about
    that suite, whatever its facts claim."""
    return sorted({layer_of(node) for node in record["rows"]} & set(LAYERS))


def merge_fetched(fetched, tracked):
    """@fetched, the newest record of the branch, with every layer it never
    measured taken from @tracked: that layer's rows and that layer's facts.

    A record holds the rows of the layers the run that wrote it passed at least
    one row of, and nothing of the others.  Every artifact the branch holds
    today was written before the display layers existed, so it measured the
    ncurses layer alone: taken whole it would leave x11 and wayland with no row
    and no seed of their own, `fit` would refuse the plan for two suites the
    run never measured, and no display shard would run at all.  A row both
    records hold is the fetched one's, which is the newer."""
    found = upgraded_record(fetched)
    merged = upgraded_record(tracked)
    measured = set(measured_layers(found))
    merged["rows"].update({node: row for node, row in found["rows"].items()
                           if layer_of(node) in measured})
    for name in sorted(measured):
        merged["layers"][name] = dict(found["layers"][name])
    return merged


def cmd_fetch(args):
    """Download the newest record of the branch to @out, merged by layer into
    the tracked one (merge_fetched).  Exit 0 with no file when there is none,
    so the plan falls back to the tracked record."""
    try:
        listing = gh_json("repos/%s/actions/artifacts?name=%s&per_page=100"
                          % (args.repo, ARTIFACT_NAME))
        found = newest_artifact(listing, args.branch)
        if found is None:
            print("fetch: no unexpired %s artifact on %s; the tracked record is used"
                  % (ARTIFACT_NAME, args.branch))
            return 0
        with tempfile.TemporaryDirectory(prefix="timings.") as scratch:
            archive = os.path.join(scratch, "a.zip")
            gh_zip("repos/%s/actions/artifacts/%d/zip" % (args.repo, found["id"]), archive)
            with zipfile.ZipFile(archive) as bundle:
                names = [n for n in bundle.namelist() if n.endswith(".json")]
                with bundle.open(names[0]) as src:
                    fetched = json.loads(src.read())
        measured = measured_layers(upgraded_record(fetched))
        merged = merge_fetched(fetched, load_record(TRACKED_RECORD))
        save_record(normalise_settle(merged, args.settle_ms), args.out)
    except (OSError, subprocess.SubprocessError, ValueError, IndexError) as err:
        print("fetch: no record fetched (%s); the tracked record is used" % err)
        return 0
    print("fetch: record of run %s, which measured %s"
          % (found["workflow_run"].get("id"), ", ".join(measured) or "no layer"))
    kept = [name for name in LAYER_ORDER if name not in measured]
    if kept:
        print("fetch: %s keep the rows and the facts of the tracked record"
              % ", ".join(kept))
    return 0


# ---------------------------------------------------------------------------
# The command line


def build_parser():
    """The argument parser of every sub-command."""
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--selftest", action="store_true")
    sub = parser.add_subparsers(dest="command")
    collect = sub.add_parser("collect")
    collect.add_argument("out")
    record = sub.add_parser("record")
    record.add_argument("junit")
    record.add_argument("out")
    record.add_argument("--previous", default="")
    record.add_argument("--collected", default="")
    record.add_argument("--host-kind", default=HOST_SEED, choices=(HOST_SEED, HOST_RUNNER))
    record.add_argument("--scale", type=float, default=1)
    record.add_argument("--settle-ms", type=int, default=0)
    record.add_argument("--workers", type=int, default=SHARD_WORKERS)
    record.add_argument("--source", default="")
    record.add_argument("--plan", default="")
    plan = sub.add_parser("plan")
    plan.add_argument("out")
    plan.add_argument("--record", required=True)
    plan.add_argument("--collected", required=True)
    plan.add_argument("--jobs-max", type=int, default=None)
    plan.add_argument("--target-min", type=int, default=None)
    plan.add_argument("--workers", type=int, default=None)
    plan.add_argument("--scale", type=float, default=None)
    fit = sub.add_parser("fit")
    fit.add_argument("--record", required=True)
    fit.add_argument("--collected", required=True)
    fit.add_argument("--jobs-max", type=int, default=None)
    fit.add_argument("--target-min", type=int, default=None)
    fit.add_argument("--workers", type=int, default=None)
    shards = sub.add_parser("shards")
    shards.add_argument("plan")
    verdict = sub.add_parser("verdict")
    verdict.add_argument("plan")
    verdict.add_argument("--status", default="")
    verdict.add_argument("--junit", default="")
    for name in ("cut-min", "workers", "logs"):
        asked = sub.add_parser(name)
        asked.add_argument("shard", nargs="?", type=int, default=None)
        asked.add_argument("--plan", default="")
    status = sub.add_parser("status")
    status.add_argument("out")
    status.add_argument("--shard", type=int, required=True)
    status.add_argument("--exit", type=int, required=True)
    status.add_argument("--started", default="")
    status.add_argument("--ended", default="")
    status.add_argument("--ids", type=int, default=0)
    status.add_argument("--cut-report", default="")
    status.add_argument("--plan", default="")
    fetch = sub.add_parser("fetch")
    fetch.add_argument("--repo", required=True)
    fetch.add_argument("--branch", required=True)
    fetch.add_argument("--out", required=True)
    fetch.add_argument("--settle-ms", type=int, default=0)
    return parser


def main(argv):
    args = build_parser().parse_args(argv)
    if args.selftest:
        if selftest():
            print("suite_plan: every plan it must refuse is refused")
            return 0
        return 1
    handlers = {"collect": cmd_collect, "record": cmd_record, "plan": cmd_plan,
                "fit": cmd_fit, "shards": cmd_shards, "status": cmd_status,
                "verdict": cmd_verdict, "cut-min": cmd_cut_min,
                "workers": cmd_workers, "logs": cmd_logs, "fetch": cmd_fetch}
    if args.command not in handlers:
        build_parser().print_usage(sys.stderr)
        return 2
    return handlers[args.command](args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
