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

Sub-commands (stdlib only, so the build job and `harness.py check` run them
bare):

    collect OUT.txt                  the node ids of the suite's selection plus
                                     every ncurses row a manifest maps
    record JUNIT[,JUNIT...] OUT.json [--previous OLD.json] [--collected IDS]
           [--host-kind seed|runner] [--scale N] [--settle-ms N]
           [--workers N] [--source TEXT]
                                     the seconds of each row that passed; a
                                     row the run cut, skipped or failed keeps
                                     the time the previous record holds
    plan --record R.json --collected IDS.txt --jobs-max N --target-min T
         [--workers W] [--scale S] OUT.json
                                     the jobs, one args file each, next to OUT
    fit  --record R.json --collected IDS.txt [--jobs-max N] [--target-min T]
                                     exit 1 when the suite no longer fits
    shards PLAN.json                 the job indices, as JSON, for a matrix
    cut-min                          the minutes after which a job is cut
    workers                          the test workers of one job
    status OUT.json --shard K --exit RC --started T --ended T --ids N
           [--cut-report R.json]     how one job ended, for the gate
    fetch --repo R --branch B --out OUT.json
                                     the newest unexpired `suite-timings`
                                     artifact of that branch (gh api)
    --selftest                       an over-full record and a four-hour row
                                     must both fail `fit`

Exit: 0 done, 1 a finding (fit) or a wrong canary, 2 a bad command line.
"""
import argparse
import heapq
import json
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
RECORD_VERSION = 1

# The most jobs of one run, and how long one may take (the plan's target).
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
# The share of the collected rows the record must hold a time for.  Accepted
# limit: the tracked record covers the tree with a margin of under one point
# (90.85 % against 90), so a few new files fail `fit` until the record is
# refreshed from a run.  No slack is added: slack would hide a stale record.
RECORD_COVER = 90
# A file that holds more than this share of one job's capacity is split into
# its rows, so that no single file decides the largest job.
SPLIT_SHARE = 0.5
# A record made on another machine than the runner is multiplied by this
# factor in the plan, until a runner artifact replaces it: the seed was made
# on a development machine, and the runner needed more than six hours for a
# suite that took four there (run 45065693614).
SEED_HOST_FACTOR = 1.5
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

# The rows the ncurses job leaves out of its selection; the mapped ones among
# them run anyway (collect_ids).  One definition, here: the workflow names no
# selection, the build job's collection applies it once and the jobs run the
# node ids that collection gave.
SELECTION = (
    "not (_live or clangd or gopls or rust or scala or metals or python_lsp "
    "or go_dap or dap or algol68 or ga68 or truecolor_console or compile "
    "or make_output or build_cwd or user_screen or debugger_watchdog "
    "or ctrl_f9 or alt_f5)")
# What a shard's pytest never reaches: the display layers have their own job.
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


def empty_record():
    """A record that holds no row yet."""
    return {"version": RECORD_VERSION, "host": "", "host_kind": HOST_SEED,
            "scale": 1, "settle_ms": 0, "workers": SHARD_WORKERS, "source": "",
            "rows": {}}


def load_record(path):
    """The record in the file at @path (empty_record() when there is none)."""
    if not path or not os.path.exists(path):
        return empty_record()
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


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


def row_seconds(record, node_id):
    """The recorded seconds of @node_id, or None."""
    row = record["rows"].get(node_id)
    return None if row is None else row[0]


def host_factor(record):
    """What the plan multiplies the record's seconds by: SEED_HOST_FACTOR for
    a record made off the runner, 1 for one the runner made."""
    return 1.0 if record.get("host_kind") == HOST_RUNNER else SEED_HOST_FACTOR


def row_budget_s(record, node_id, scale):
    """The seconds a row may run before the row clock cuts it.

    A user who runs one row gets a hung row ended with its name and its
    budget instead of waiting for the whole job to be cut.  The budget is a
    few times what the row took last time, stretched when this run's wait
    scale is above the record's (a sanitizer build, a slower machine), and
    never below a floor or above the ceiling.  The host factor of the plan
    does not apply here: the record's own scale is the only stretch."""
    seconds = row_seconds(record, node_id)
    if seconds is None:
        return min(ROW_UNRECORDED_S * scale, ROW_CEILING_S)
    recorded_scale = record.get("scale") or 1
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


def merge_record(previous, cases, known, meta):
    """(record, matched, unmatched) after one run's cases.

    The run's seconds replace those of the rows it passed.  Every other row
    keeps the time and source the previous record holds: a row the run cut,
    failed or skipped did not show how long it takes, and a row a cut job
    never reached is absent from the junit.  Learning from a cut would make
    the next budget three times the last one.

    pytest-rerunfailures writes every attempt of a row as its own <testcase>,
    and the attempts it retried carry no failure child.  So the cases are
    grouped by node id first: a row any attempt of which failed or was cut
    keeps its old time, and a row that passed on a rerun learns the time of
    its last attempt."""
    record = dict(previous)
    record.update(meta)
    rows = dict(previous["rows"])
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
    for node, tries in attempts.items():
        if all(outcome == "passed" for _seconds, outcome in tries):
            rows[node] = [round(tries[-1][0], 3), meta["source"]]
    record["rows"] = rows
    return record, matched, unmatched


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
    meta = {"host": ",".join(sorted(hosts)) or previous.get("host", ""),
            "host_kind": args.host_kind,
            "scale": args.scale, "settle_ms": args.settle_ms,
            "workers": args.workers, "source": args.source}
    record, matched, unmatched = merge_record(previous, cases, known, meta)
    save_record(record, args.out)
    cover = ""
    if known:
        held = sum(1 for node in known if node in record["rows"])
        cover = ", %.1f%% of the %d collected rows hold a time" % (
            100.0 * held / len(known), len(known))
    print("record: %d cases read, %d matched a row, %d unmatched%s"
          % (len(cases), matched, len(unmatched), cover))
    for line in unmatched[:20]:
        print("record: unmatched %s" % line, file=sys.stderr)
    return 0


# ---------------------------------------------------------------------------
# The plan


def file_of(node_id):
    """``tests/x.py::T::t[p]`` -> ``tests/x.py``."""
    return node_id.split("::", 1)[0]


class Estimator:
    """How many seconds a row is expected to take: its recorded time, else
    the median of its file's recorded rows, else the median of the record,
    else a guess.  Each is multiplied by the plan's factor."""

    def __init__(self, record, ids, factor):
        self.rows = record["rows"]
        self.factor = factor
        by_file = {}
        for node in ids:
            if node in self.rows:
                by_file.setdefault(file_of(node), []).append(self.rows[node][0])
        self.file_median = {f: statistics.median(v) for f, v in by_file.items()}
        held = [row[0] for node, row in self.rows.items()]
        self.record_median = statistics.median(held) if held else GUESS_ROW_S

    def seconds(self, node):
        """The estimate for @node, in seconds on the runner."""
        row = self.rows.get(node)
        if row is not None:
            base = row[0]
        else:
            base = self.file_median.get(file_of(node), self.record_median)
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


def make_plan(record, ids, jobs_max, target_min, workers, scale=None):
    """The plan: the fewest jobs whose largest bin, over the workers, is at
    most @target_min minutes (@jobs_max jobs when none is), as a dict."""
    factor = host_factor(record)
    if scale is not None:
        factor *= scale / (record.get("scale") or 1)
    estimator = Estimator(record, ids, factor)
    capacity_s = workers * target_min * 60
    items = plan_items(ids, estimator, capacity_s)
    chosen = None
    for jobs in range(1, jobs_max + 1):
        bins = split_longest_first(items, jobs)
        chosen = bins
        if max(load for load, _rows in bins) <= capacity_s:
            break
    shards = []
    assignment = {}
    for index, (load, rows) in enumerate(chosen):
        shards.append({"index": index, "ids": len(rows), "args": ARGS_NAME % index,
                       "predicted_min": round(load / workers / 60.0, 1),
                       "rows": sorted(rows)})
        for node in rows:
            assignment[node] = index
    fits = max(load for load, _rows in chosen) <= capacity_s if chosen else True
    return {"version": RECORD_VERSION, "jobs": len(shards), "workers": workers,
            "target_min": target_min, "cut_min": SHARD_CUT_MIN, "fits": fits,
            "factor": factor, "record_host": record.get("host", ""),
            "record_host_kind": record.get("host_kind", HOST_SEED),
            "shards": shards, "assignment": assignment}


def write_plan(plan, out):
    """Write the plan to @out and each shard's args file next to it (one node
    id to a line, which pytest reads with @file)."""
    folder = os.path.dirname(os.path.abspath(out))
    os.makedirs(folder, exist_ok=True)
    listed = dict(plan)
    listed["shards"] = []
    for shard in plan["shards"]:
        with open(os.path.join(folder, shard["args"]), "w", encoding="utf-8") as fh:
            fh.write("\n".join(shard["rows"]) + "\n")
        listed["shards"].append({k: v for k, v in shard.items() if k != "rows"})
    with open(out, "w", encoding="utf-8") as fh:
        json.dump(listed, fh, indent=1, sort_keys=True)
        fh.write("\n")


def plan_summary(plan):
    """The plan in a few lines for a log."""
    lines = ["plan: %d jobs, %d workers each, target %d min, cut at %d min, "
             "factor %.2f (record host %s, %s)"
             % (plan["jobs"], plan["workers"], plan["target_min"],
                plan["cut_min"], plan["factor"], plan["record_host"] or "?",
                plan["record_host_kind"])]
    for shard in plan["shards"]:
        lines.append("plan: shard %d: %d rows, predicted %.1f min"
                     % (shard["index"], shard["ids"], shard["predicted_min"]))
    if not plan["fits"]:
        lines.append("plan: does not fit in %d jobs of %d min"
                     % (plan["jobs"], plan["target_min"]))
    return "\n".join(lines)


def cmd_plan(args):
    """Write the plan and its args files, and print the plan."""
    record = load_record(args.record)
    ids = read_ids(args.collected)
    plan = make_plan(record, ids, args.jobs_max, args.target_min, args.workers,
                     args.scale)
    write_plan(plan, args.out)
    print(plan_summary(plan))
    return 0


# ---------------------------------------------------------------------------
# The fitness check


def fit_problems(record, ids, jobs_max=MAX_JOBS, target_min=SHARD_TARGET_MIN,
                 workers=SHARD_WORKERS):
    """The findings, as sentences with their numbers, when the suite does not
    fit: the largest job at @jobs_max jobs over the target, a row longer than
    the target, a record that holds a time for too few of the rows."""
    problems = []
    if not ids:
        return ["fit: no row was collected, the plan has nothing to place"]
    factor = host_factor(record)
    plan = make_plan(record, ids, jobs_max, target_min, workers)
    longest = max(s["predicted_min"] for s in plan["shards"])
    if longest > target_min:
        problems.append(
            "fit: the largest of %d jobs is predicted at %.1f min, over the "
            "%d min target" % (plan["jobs"], longest, target_min))
    for node, row in sorted(record["rows"].items()):
        if row[0] * factor / 60.0 > target_min:
            problems.append(
                "fit: %s took %.0f s (%.1f min with the host factor), over the "
                "%d min target: it cannot fit in any job"
                % (node, row[0], row[0] * factor / 60.0, target_min))
    held = sum(1 for node in ids if node in record["rows"])
    cover = 100.0 * held / len(ids)
    if cover < RECORD_COVER:
        problems.append(
            "fit: the record holds a time for %.1f%% of the %d collected rows, "
            "under the %d%% the estimate needs" % (cover, len(ids), RECORD_COVER))
    return problems


def cmd_fit(args):
    """Print the findings; exit 1 when there are any."""
    record = load_record(args.record)
    problems = fit_problems(record, read_ids(args.collected), args.jobs_max,
                            args.target_min)
    for line in problems:
        print(line)
    if not problems:
        print("fit: the suite fits in %d jobs of %d min" % (args.jobs_max, args.target_min))
    return 1 if problems else 0


def fake_record(seconds_by_row, host_kind=HOST_RUNNER):
    """A record of @seconds_by_row, for the self-test and the rows."""
    record = empty_record()
    record["host_kind"] = host_kind
    record["rows"] = {node: [seconds, "fake"] for node, seconds in seconds_by_row.items()}
    return record


def selftest():
    """True when an over-full record and a four-hour row are both refused and
    a record that fits is not."""
    ids = ["tests/test_f%d.py::t%d" % (n, n) for n in range(40)]
    fits = fake_record({node: 60 for node in ids})
    over_full = fake_record({node: 60 * 60 * 3 for node in ids})
    one_long = fake_record({node: 60 for node in ids})
    one_long["rows"][ids[0]] = [4 * 3600, "fake"]
    ok = True
    if fit_problems(fits, ids):
        sys.stderr.write("suite_plan: a record that fits was refused\n")
        ok = False
    if not any("largest" in line for line in fit_problems(over_full, ids)):
        sys.stderr.write("suite_plan: an over-full record was accepted\n")
        ok = False
    if not any("cannot fit" in line for line in fit_problems(one_long, ids)):
        sys.stderr.write("suite_plan: a four-hour row was accepted\n")
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


def mapped_ncurses_ids():
    """The test ids every manifest maps in the ncurses layer (and the
    config-file one that runs with it), live and blocked rows left out."""
    sys.path.insert(0, HERE)
    try:
        import harness
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


def collect_ids():
    """The node ids the jobs run: the selection, plus every row a manifest
    maps in the ncurses layer that the selection leaves out, so the gate has
    nothing to top up."""
    ignores = ["--ignore=%s" % path for path in SUITE_IGNORES]
    ids = run_collect(["tests"] + ignores + ["-k", SELECTION])
    mapped = mapped_ncurses_ids()
    files = sorted({m.split("::", 1)[0] for m in mapped
                    if os.path.exists(os.path.join(TOP, m.split("::", 1)[0]))
                    and not m.startswith(SUITE_IGNORES)})
    held = set(ids)
    sys.path.insert(0, HERE)
    try:
        import harness
    finally:
        sys.path.pop(0)
    for node in run_collect(files) if files else []:
        if node not in held and any(harness.row_covers(m, node) for m in mapped):
            ids.append(node)
            held.add(node)
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


def cmd_status(args):
    """Write how a shard's pytest ended: the exit code, whether the job was
    cut and whether the cut had to kill it (from the cut tool's report when it
    left one, else from the exit code: 124 and 137), the times, the number of
    rows it was given."""
    cut, killed = cut_facts(args.exit, read_cut_report(args.cut_report))
    status = {"shard": args.shard, "exit": args.exit,
              "cut": cut, "killed": killed,
              "cut_min": SHARD_CUT_MIN, "started": args.started,
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
    """Print the job indices of a plan as a JSON list (a matrix input)."""
    with open(args.plan, encoding="utf-8") as fh:
        plan = json.load(fh)
    print(json.dumps([shard["index"] for shard in plan["shards"]]))
    return 0


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
    """@record with its seconds stretched to the settle of this run.

    A run that settles longer after each key takes longer per row, so a record
    made at a shorter settle would plan too few jobs.  A record with no known
    settle (0), a run with none, or a longer recorded settle is left as it is
    (a shorter plan than the record is never made up).  The record's own scale
    is dealt with by the plan and the row clock, not here."""
    recorded = record.get("settle_ms") or 0
    if not recorded or not settle_ms or settle_ms <= recorded:
        return record
    stretch = float(settle_ms) / recorded
    stretched = dict(record)
    stretched["rows"] = {node: [round(row[0] * stretch, 3)] + list(row[1:])
                         for node, row in record["rows"].items()}
    stretched["settle_ms"] = settle_ms
    return stretched


def cmd_fetch(args):
    """Download the newest record of the branch to @out.  Exit 0 with no file
    when there is none, so the plan falls back to the tracked record."""
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
        save_record(normalise_settle(fetched, args.settle_ms), args.out)
    except (OSError, subprocess.SubprocessError, ValueError, IndexError) as err:
        print("fetch: no record fetched (%s); the tracked record is used" % err)
        return 0
    print("fetch: record of run %s" % found["workflow_run"].get("id"))
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
    plan = sub.add_parser("plan")
    plan.add_argument("out")
    plan.add_argument("--record", required=True)
    plan.add_argument("--collected", required=True)
    plan.add_argument("--jobs-max", type=int, default=MAX_JOBS)
    plan.add_argument("--target-min", type=int, default=SHARD_TARGET_MIN)
    plan.add_argument("--workers", type=int, default=SHARD_WORKERS)
    plan.add_argument("--scale", type=float, default=None)
    fit = sub.add_parser("fit")
    fit.add_argument("--record", required=True)
    fit.add_argument("--collected", required=True)
    fit.add_argument("--jobs-max", type=int, default=MAX_JOBS)
    fit.add_argument("--target-min", type=int, default=SHARD_TARGET_MIN)
    shards = sub.add_parser("shards")
    shards.add_argument("plan")
    sub.add_parser("cut-min")
    sub.add_parser("workers")
    status = sub.add_parser("status")
    status.add_argument("out")
    status.add_argument("--shard", type=int, required=True)
    status.add_argument("--exit", type=int, required=True)
    status.add_argument("--started", default="")
    status.add_argument("--ended", default="")
    status.add_argument("--ids", type=int, default=0)
    status.add_argument("--cut-report", default="")
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
            print("suite_plan: an over-full record and a four-hour row are refused")
            return 0
        return 1
    if args.command == "cut-min":
        print(SHARD_CUT_MIN)
        return 0
    if args.command == "workers":
        print(SHARD_WORKERS)
        return 0
    handlers = {"collect": cmd_collect, "record": cmd_record, "plan": cmd_plan,
                "fit": cmd_fit, "shards": cmd_shards, "status": cmd_status,
                "fetch": cmd_fetch}
    if args.command not in handlers:
        build_parser().print_usage(sys.stderr)
        return 2
    return handlers[args.command](args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
