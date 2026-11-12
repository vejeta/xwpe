#!/usr/bin/env python3
"""The evaluation runner: tasks x lanes x models x runs, serial, one row each.

    tests/.venv/bin/python tests/eval/run.py --lanes mock --tasks E03
    tests/.venv/bin/python tests/eval/run.py --lanes local --tasks E01,E02,E03 \
        --models <ollama-model> --runs 1

``--lanes mock``   the scripted backend only: the suite's own self-test.
``--lanes local``  the self-test, then xwpe against the local Ollama server.
                   The model comes from ``--models`` or ``XWPE_AI_MODEL``.
``--lanes <name>`` the self-test, then a lane loaded from ``--lane-dir`` (or
                   ``XWPE_EVAL_LANE_DIR``): each ``*.py`` file there exposes
                   ``LANE`` and ``run(driver, ctx, model, endpoint)`` like
                   ``lanes/xwpe_ollama.py``.  Several lanes: a comma list.

Each run gets one scratch folder under /var/tmp holding ``repo/`` (the copy
the editor works in) and ``home/`` (its HOME and XDG roots).  The path is
printed when it is made and removed by exactly that path when the run is
scored; a copy that did not fully pass is first saved under
``out/<stamp>/keep/``.  Results go to ``out/<stamp>.jsonl`` (one row per
task, lane, model, run) and ``out/<stamp>.md`` (the table).

The runner unloads only a model it loaded itself.  If another model it did not
load is resident, it waits (at most 30 minutes, looking every minute) for the
server to be free of it, and skips the lane with a note otherwise.

A server that cannot be reached gives one row per task with ``end_reason``
``server-unreachable``.  An exception inside one run gives a row with
``end_reason`` ``exception`` and the suite goes on with the next run.
"""
import argparse
import json
import os
import shutil
import signal
import sys
import tempfile
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS_DIR = os.path.dirname(HERE)
REPO_ROOT = os.path.dirname(TESTS_DIR)
sys.path.insert(0, HERE)

import score  # noqa: E402
from lanes import external, xwpe_common, xwpe_mock, xwpe_ollama  # noqa: E402

SCRATCH_ROOT = "/var/tmp"
SCRATCH_PREFIX = "xwpe-eval-"
# The editor's own variable for its model: the runner's fallback for --models.
MODEL_ENV = "XWPE_AI_MODEL"
LANE_DIR_ENV = "XWPE_EVAL_LANE_DIR"
LANE_MOCK = "mock"
LANE_LOCAL = "local"
BUILTIN_LANES = (LANE_MOCK, LANE_LOCAL)
LOCAL_DEADLINE_S = 25 * 60
MOCK_DEADLINE_S = 120
# Not copied into the editor's folder: the answer key must not be readable.
HIDDEN_FROM_COPY = ("checks.json", "expected", "hidden")
# Waiting for a model the runner did not load.
BUSY_WAIT_MAX_S = 30 * 60
BUSY_LOOK_S = 60
HTTP_TIMEOUT_S = 10
UNLOAD_TIMEOUT_S = 60


class TaskContext:
    """Everything one lane run needs about its scratch folder and task."""

    def __init__(self, task, scratch, deadline_s):
        self.task = task
        self.scratch = scratch
        self.repo = os.path.join(scratch, "repo")
        self.home = os.path.join(scratch, "home")
        self.trace_path = os.path.join(scratch, "trace")
        self.deadline_s = deadline_s
        with open(os.path.join(self.repo, "TASK.md")) as fh:
            self.task_text = fh.read()


def make_scratch(task, deadline_s):
    """A fresh scratch folder holding the task's copy and an empty HOME."""
    scratch = tempfile.mkdtemp(prefix=SCRATCH_PREFIX, dir=SCRATCH_ROOT)
    print("scratch: %s" % scratch, flush=True)
    repo = os.path.join(scratch, "repo")
    os.makedirs(repo)
    source = score.fixture_dir(task)
    for name in sorted(os.listdir(source)):
        if name in HIDDEN_FROM_COPY:
            continue
        shutil.copy2(os.path.join(source, name), os.path.join(repo, name))
    home = os.path.join(scratch, "home")
    os.makedirs(os.path.join(home, ".config"))
    os.makedirs(os.path.join(home, ".local", "state"))
    return TaskContext(task, scratch, deadline_s)


def remove_scratch(scratch):
    """Remove exactly the folder this run made, nothing else."""
    shutil.rmtree(scratch)
    print("removed: %s" % scratch, flush=True)


# -- the local model server --------------------------------------------------

class OllamaGuard:
    """Who is loaded on the local server, and what the runner may unload."""

    def __init__(self, endpoint):
        self.endpoint = endpoint.rstrip("/")
        self.loaded_by_us = set()

    def _call(self, path, body=None, timeout=HTTP_TIMEOUT_S):
        data = None if body is None else json.dumps(body).encode()
        req = urllib.request.Request(self.endpoint + path, data=data)
        if data is not None:
            req.add_header("Content-Type", "application/json")
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode() or "{}")

    def resident(self):
        """The models now loaded: a list of the server's /api/ps entries."""
        return self._call("/api/ps").get("models", [])

    @staticmethod
    def same_model(name, wanted):
        return name == wanted or name == wanted + ":latest"

    def others_resident(self, model):
        return [m["name"] for m in self.resident()
                if not self.same_model(m["name"], model)]

    def wait_until_free(self, model):
        """None when the server holds no other model, else a skip note.

        Never unloads a model the runner did not load: it waits."""
        waited = 0
        while True:
            others = self.others_resident(model)
            if not others:
                return None
            if waited >= BUSY_WAIT_MAX_S:
                return "server busy with %s; lane skipped" % ",".join(others)
            print("waiting: %s is loaded by someone else (%ds)"
                  % (",".join(others), waited), flush=True)
            time.sleep(BUSY_LOOK_S)
            waited += BUSY_LOOK_S

    def was_loaded(self, model):
        return any(self.same_model(m["name"], model) for m in self.resident())

    def note_loaded(self, model, was_loaded_before):
        """Remember a model the runner brought in; return its window, if any."""
        window = None
        for entry in self.resident():
            if self.same_model(entry["name"], model):
                window = entry.get("context_length")
                if not was_loaded_before:
                    self.loaded_by_us.add(model)
        return window

    def unload_ours(self, model):
        """Unload ``model`` only when this runner loaded it."""
        if model not in self.loaded_by_us:
            return
        self._call("/api/generate", {"model": model, "keep_alive": 0},
                   timeout=UNLOAD_TIMEOUT_S)
        self.loaded_by_us.discard(model)
        print("unloaded: %s" % model, flush=True)


# -- one run -----------------------------------------------------------------

def passed_everything(row):
    return bool(row["done"] and row["files_ok"] and row["build_ok"]
                and row["test_ok"])


def keep_failed_copy(ctx, out_dir, label):
    """Save the copy and its trace under out/<stamp>/keep/ before removal."""
    keep = os.path.join(out_dir, "keep", label)
    shutil.copytree(ctx.repo, keep)
    if os.path.exists(ctx.trace_path):
        shutil.copy2(ctx.trace_path, os.path.join(keep, "..", label + ".trace"))
    return keep


def save_answer(out_dir, label, answer_text, pane_text):
    """The reply the scorer read the answer from (``.answer``) and the whole
    pane it was taken from (``.pane``), kept beside the trace."""
    folder = os.path.join(out_dir, "traces")
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, label + ".answer"), "w") as fh:
        fh.write(answer_text)
    with open(os.path.join(folder, label + ".pane"), "w") as fh:
        fh.write(pane_text)


def save_trace(ctx, out_dir, label):
    folder = os.path.join(out_dir, "traces")
    os.makedirs(folder, exist_ok=True)
    if os.path.exists(ctx.trace_path):
        shutil.copy2(ctx.trace_path, os.path.join(folder, label + ".trace"))


def failure_row(task, lane, model, run, end_reason, note):
    """A row for a run that produced nothing to score: the outcome columns are
    empty (not measured) and ``notes`` says why."""
    outcome = {"files_ok": None, "build_ok": None, "test_ok": None,
               "notes": [note]}
    return score.make_row(task, lane, model, run, end_reason, 0.0, "",
                          outcome, 0.0)


def scored_row(ctx, task, lane, model, run, out_dir, label, execute,
               extra_notes):
    """Run the lane, score the copy and keep what the run left; the row."""
    result = execute(ctx)
    outcome = score.score_repo(ctx.repo, task, result.answer_text)
    row = score.make_row(task, lane, model, run, result.end_reason,
                         result.wall_s, result.trace_text, outcome, 0.0,
                         list(extra_notes) + result.notes)
    save_trace(ctx, out_dir, label)
    save_answer(out_dir, label, result.answer_text, result.pane_text)
    if not passed_everything(row):
        keep_failed_copy(ctx, out_dir, label)
    return row


def exception_row(ctx, task, lane, model, run, out_dir, label, exc):
    """The row of a run that raised: what was left of it is kept if it can be."""
    note = "%s: %s" % (type(exc).__name__, exc)
    print("run failed: %s" % note, flush=True)
    for keep in (lambda: save_trace(ctx, out_dir, label),
                 lambda: keep_failed_copy(ctx, out_dir, label)):
        try:
            keep()
        except OSError:
            pass
    return failure_row(task, lane, model, run, xwpe_common.END_EXCEPTION, note)


def run_one(driver, lane, task, model, run, deadline_s, out_dir, execute,
            extra_notes=()):
    """Make the scratch, run the lane through ``execute(ctx)``, score, clean.

    Returns the row.  An exception in the run (not a stop request) becomes a
    row with end_reason ``exception``, so the suite goes on.  The scratch
    folder is removed by its exact path in every case."""
    ctx = make_scratch(task, deadline_s)
    label = "%s-%s-%s-r%d" % (task, lane, (model or "default").replace(":", "_"),
                              run)
    try:
        try:
            return scored_row(ctx, task, lane, model, run, out_dir, label,
                              execute, extra_notes)
        except Exception as exc:
            return exception_row(ctx, task, lane, model, run, out_dir, label,
                                 exc)
    finally:
        remove_scratch(ctx.scratch)


class Results:
    """Rows appended to the stamp's .jsonl as they come, table at the end."""

    def __init__(self, out_root, stamp):
        os.makedirs(out_root, exist_ok=True)
        self.out_dir = os.path.join(out_root, stamp)
        os.makedirs(self.out_dir, exist_ok=True)
        self.jsonl = os.path.join(out_root, stamp + ".jsonl")
        self.markdown = os.path.join(out_root, stamp + ".md")
        self.rows = []

    def add(self, row):
        self.rows.append(row)
        with open(self.jsonl, "a") as fh:
            fh.write(json.dumps(row) + "\n")
        print("row: %s %s %s r%s done=%s files_ok=%s build_ok=%s test_ok=%s "
              "end=%s wall=%ss" % (
                  row["task"], row["lane"], row["model"], row["run"],
                  row["done"], row["files_ok"], row["build_ok"],
                  row["test_ok"], row["end_reason"], row["wall_s"]),
              flush=True)

    def finish(self):
        table = score.render_table(self.rows)
        with open(self.markdown, "w") as fh:
            fh.write(table)
        print()
        print(table)
        print("rows: %s" % self.jsonl)


# -- the lanes ---------------------------------------------------------------

def run_mock_lane(driver, tasks, deadline_s, results):
    """Self-test: every task with scripts, each variant. Returns failures."""
    failures = []
    for task in tasks:
        for variant in xwpe_mock.VARIANTS:
            if not xwpe_mock.has_script(task, variant):
                continue
            row = run_one(driver, xwpe_mock.LANE, task, "mock:" + variant, 1,
                          deadline_s, results.out_dir,
                          lambda ctx, v=variant: xwpe_mock.run(driver, ctx, v))
            results.add(row)
            problem = selftest_problem(row, variant)
            if problem:
                failures.append("%s %s: %s" % (task, variant, problem))
    return failures


def selftest_problem(row, variant):
    """Why a mock row is not what its script promises, or None."""
    if variant == "reply" and not passed_everything(row):
        return "the solving script did not score done+files_ok+build_ok+test_ok"
    if variant == "wrong" and row["done"]:
        return "the failing script scored done"
    if variant in ("wrong", "wrong_done") and passed_everything(row):
        return "the failing script scored a full pass"
    if variant == "wrong_done" and not row["done"]:
        return "the script that claims DONE did not end done"
    return None


def unreachable_rows(tasks, lane, model, runs, note, results):
    """One row per task and run for a server that cannot be reached."""
    for task in tasks:
        for run in range(1, runs + 1):
            results.add(failure_row(task, lane, model, run,
                                    xwpe_common.END_SERVER_UNREACHABLE, note))


def server_down_note(exc):
    return "model server unreachable: %s: %s" % (type(exc).__name__, exc)


def run_ollama_lane(driver, tasks, models, runs, deadline_s, endpoint,
                    results):
    guard = OllamaGuard(endpoint)
    for model in models:
        try:
            busy = guard.wait_until_free(model)
            before = None if busy else guard.was_loaded(model)
        except (urllib.error.URLError, OSError, ValueError) as exc:
            print("server unreachable: %s" % exc, flush=True)
            unreachable_rows(tasks, xwpe_ollama.LANE, model, runs,
                             server_down_note(exc), results)
            continue
        if busy:
            print("skip %s: %s" % (model, busy), flush=True)
            continue
        try:
            run_model_tasks(driver, guard, tasks, model, runs, deadline_s,
                            endpoint, before, results)
        finally:
            unload_quietly(guard, model)


def unload_quietly(guard, model):
    """Unload the runner's own model; a server that went away is reported."""
    try:
        guard.unload_ours(model)
    except (urllib.error.URLError, OSError, ValueError) as exc:
        print("unload failed: %s" % exc, flush=True)


def window_note(guard, model, was_loaded):
    """The ``ollama ctx=N`` note of a finished run, or "" when the server
    cannot be asked."""
    try:
        window = guard.note_loaded(model, was_loaded)
    except (urllib.error.URLError, OSError, ValueError):
        return ""
    if window:
        return "ollama ctx=%s" % window
    return ""


def run_external_lane(driver, module, tasks, models, runs, deadline_s,
                      endpoint, results):
    """Every task and run of a lane loaded from the lane directory."""
    for model in models:
        for task in tasks:
            for run in range(1, runs + 1):
                row = run_one(driver, module.LANE, task, model, run,
                              deadline_s, results.out_dir,
                              lambda ctx, m=model: module.run(driver, ctx, m,
                                                              endpoint))
                results.add(row)


def run_model_tasks(driver, guard, tasks, model, runs, deadline_s, endpoint,
                    was_loaded, results):
    for task in tasks:
        for run in range(1, runs + 1):
            notes = []
            row = run_one(driver, xwpe_ollama.LANE, task, model, run,
                          deadline_s, results.out_dir,
                          lambda ctx: xwpe_ollama.run(driver, ctx, model,
                                                      endpoint),
                          notes)
            note = window_note(guard, model, was_loaded)
            was_loaded = True
            if note:
                row["notes"] = (row["notes"] + "; " if row["notes"] else "") \
                    + note
            results.add(row)


# -- command line ------------------------------------------------------------

def all_tasks():
    names = []
    for name in sorted(os.listdir(score.FIXTURES)):
        if os.path.isfile(os.path.join(score.FIXTURES, name, "checks.json")):
            names.append(name)
    return names


def parse_args(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--lanes", required=True,
                   help="comma list: mock, local, or the name of a lane "
                   "loaded from --lane-dir")
    p.add_argument("--lane-dir", default=os.environ.get(LANE_DIR_ENV, ""),
                   help="folder of extra lane modules (default: $%s)"
                   % LANE_DIR_ENV)
    p.add_argument("--tasks", default="",
                   help="comma list of fixture ids (default: all)")
    p.add_argument("--models", default=os.environ.get(MODEL_ENV, ""),
                   help="comma list of models for the local lane and the "
                   "lane-dir lanes (default: $%s; required for local)"
                   % MODEL_ENV)
    p.add_argument("--runs", type=int, default=1,
                   help="runs per task and model (3 for a parity call)")
    p.add_argument("--deadline-s", type=int, default=0,
                   help="per-run deadline (default %ds local, %ds mock)"
                   % (LOCAL_DEADLINE_S, MOCK_DEADLINE_S))
    p.add_argument("--endpoint", default=xwpe_ollama.DEFAULT_ENDPOINT)
    p.add_argument("--bin", default=os.path.join(REPO_ROOT, "wpe"),
                   help="the terminal binary, named wpe")
    p.add_argument("--out", default=os.path.join(HERE, "out"))
    return p.parse_args(argv)


def on_terminate(signum, frame):
    """Turn SIGTERM into an exit that runs the cleanup in finally blocks."""
    raise SystemExit(128 + signum)


def load_extra_lanes(lane_dir):
    """{name: module} of the lane directory; {} when none is given."""
    if not lane_dir:
        return {}
    return external.load_lane_dir(lane_dir, taken=BUILTIN_LANES)


def lane_problem(wanted, extra, models):
    """Why the requested lanes cannot run, or None."""
    for name in wanted:
        if name not in BUILTIN_LANES and name not in extra:
            return ("unknown lane %s (built in: %s; from the lane directory: "
                    "%s)" % (name, ", ".join(BUILTIN_LANES),
                             ", ".join(sorted(extra)) or "none given"))
    if LANE_LOCAL in wanted and not models:
        return ("the local lane needs a model: pass --models or set %s"
                % MODEL_ENV)
    return None


def main(argv):
    args = parse_args(argv)
    signal.signal(signal.SIGTERM, on_terminate)
    if os.path.basename(args.bin) != "wpe":
        sys.stderr.write("the binary must be named wpe, not %s\n"
                         % os.path.basename(args.bin))
        return 2
    tasks = [t for t in args.tasks.split(",") if t] or all_tasks()
    for task in tasks:
        if task not in all_tasks():
            sys.stderr.write("no fixture %s\n" % task)
            return 2
    models = [m for m in args.models.split(",") if m]
    wanted = [n for n in args.lanes.split(",") if n]
    try:
        extra = load_extra_lanes(args.lane_dir)
    except external.LaneLoadError as exc:
        sys.stderr.write("%s\n" % exc)
        return 2
    problem = lane_problem(wanted, extra, models)
    if problem:
        sys.stderr.write(problem + "\n")
        return 2
    driver = xwpe_common.import_driver(TESTS_DIR, os.path.abspath(args.bin))
    results = Results(args.out, time.strftime("%Y%m%d-%H%M%S"))
    failures = []
    try:
        failures = run_mock_lane(driver, tasks, args.deadline_s
                                 or MOCK_DEADLINE_S, results)
        deadline = args.deadline_s or LOCAL_DEADLINE_S
        if LANE_LOCAL in wanted:
            run_ollama_lane(driver, tasks, models, args.runs, deadline,
                            args.endpoint, results)
        for name in wanted:
            if name in extra:
                run_external_lane(driver, extra[name], tasks, models or [""],
                                  args.runs, deadline, args.endpoint, results)
    finally:
        results.finish()
    for line in failures:
        print("SELFTEST FAIL: " + line)
    mock_rows = [r for r in results.rows if r["lane"] == xwpe_mock.LANE]
    if failures:
        pass
    elif mock_rows:
        print("selftest: ok (%d mock rows)" % len(mock_rows))
    else:
        print("selftest: not run (no mock script for the chosen tasks)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
