#!/usr/bin/env python3
"""Scoring for the agent evaluation suite.

A run leaves a working copy (``repo/``), the assistant's trace file and the
text of the pane.  This module turns them into one row of facts:

* the **outcome** columns (``files_ok``, ``build_ok``, ``test_ok``) come from
  ``fixtures/<id>/checks.json`` run on the copy AFTER the run, never from what
  the assistant said it did;
* the **behaviour** columns (``tool_calls``, ``denials``, and the per-turn
  token columns once the assistant's trace carries them) come from the trace.
  A column the trace cannot give yet stays ``None`` and prints empty: an empty
  cell means "not measured", never "zero";
* the checks look at results only (no leftover old name, program builds, its
  own test passes, the answer names the right things), so a different correct
  solution scores the same as the reference one.

``checks.json`` format (every key optional)::

    {
      "files": [
        {"type": "absent", "pattern": "old_name", "globs": ["*.c", "*.h"]},
        {"type": "present_in_each", "pattern": "new_name", "paths": ["a.c"]},
        {"type": "unchanged", "paths": ["check.c"]},
        {"type": "changed", "paths": ["lib.c"]}
      ],
      "answer": {"regex": "first\\s*>\\s*second"},
      "build": "make -B -f Makefile",
      "test": "make -B -f Makefile test",
      "hidden": {"source": "hidden/edges.c", "link": ["stats.c"],
                 "include": ["stats.h"]}
    }

``hidden`` is a held-out program kept in the fixture and never copied into the
folder the editor works in.  After the run it is built with the address and
undefined behaviour sanitizers against the copy's ``link`` sources and
``include`` headers, and run; a failure makes ``test_ok`` false.  It catches a
fix that only satisfies the visible test.

Build and test commands run with a clean environment (a search path and a HOME
of their own): ``CC``, ``CFLAGS`` and ``MAKEFLAGS`` of the caller are not seen.

Name the makefile with ``-f``: a ``GNUmakefile`` or ``makefile`` the agent
added would otherwise be read first and could replace the checks.

``files_ok`` is true when every ``files`` entry holds and the answer matches.
Globs are not recursive and never reach dot-directories, so the editor's own
files in the copy cannot change a score.

Command line (used for the by-hand proof of each fixture's checks)::

    score.py check <fixture-id> <repo-dir> [--answer-file FILE]
    score.py table <results.jsonl> [more.jsonl ...]
"""
import fnmatch
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURES = os.path.join(HERE, "fixtures")

# One build or test command may not run longer than this.
COMMAND_TIMEOUT_S = 90
# How much of a failing command's output goes into the notes.
NOTE_TAIL_CHARS = 160

COLUMNS = ["task", "lane", "model", "run", "done", "files_ok", "build_ok",
           "test_ok", "turns", "tool_calls", "tok_in", "tok_out", "ctx_peak",
           "wall_s", "end_reason", "folds", "loop_stops", "cut_retries",
           "malformed", "denials", "cost_usd", "notes"]

# The table of the overseer's loop: these columns, in this order.
TABLE_COLUMNS = ["task", "lane", "model", "run", "done", "files_ok",
                 "build_ok", "test_ok", "turns", "tok_in", "tok_out",
                 "ctx_peak", "wall_s", "end_reason", "folds", "loop_stops",
                 "cut_retries", "malformed", "cost_usd"]


def fixture_dir(task):
    return os.path.join(FIXTURES, task)


def load_checks(task):
    """The parsed ``checks.json`` of a fixture."""
    with open(os.path.join(fixture_dir(task), "checks.json")) as fh:
        return json.load(fh)


def _read_bytes(path):
    try:
        with open(path, "rb") as fh:
            return fh.read()
    except OSError:
        return None


def _globbed(repo, globs):
    """Regular files directly in ``repo`` matching any glob; dot names never."""
    names = []
    for name in sorted(os.listdir(repo)):
        if name.startswith("."):
            continue
        if not os.path.isfile(os.path.join(repo, name)):
            continue
        if any(fnmatch.fnmatch(name, g) for g in globs):
            names.append(name)
    return names


def _check_absent(repo, spec):
    pattern = spec["pattern"].encode()
    bad = [n for n in _globbed(repo, spec["globs"])
           if pattern in (_read_bytes(os.path.join(repo, n)) or b"")]
    if bad:
        return "%s still in %s" % (spec["pattern"], ",".join(bad))
    return None


def _check_present_in_each(repo, spec):
    pattern = spec["pattern"].encode()
    bad = [p for p in spec["paths"]
           if pattern not in (_read_bytes(os.path.join(repo, p)) or b"")]
    if bad:
        return "%s missing from %s" % (spec["pattern"], ",".join(bad))
    return None


def _check_unchanged(repo, task, spec):
    bad = [p for p in spec["paths"]
           if _read_bytes(os.path.join(repo, p))
           != _read_bytes(os.path.join(fixture_dir(task), p))]
    if bad:
        return "changed but must not be: %s" % ",".join(bad)
    return None


def _check_changed(repo, task, spec):
    bad = [p for p in spec["paths"]
           if _read_bytes(os.path.join(repo, p))
           == _read_bytes(os.path.join(fixture_dir(task), p))]
    if bad:
        return "untouched: %s" % ",".join(bad)
    return None


def check_files(repo, task, checks):
    """Failure texts of the ``files`` checks; empty when all hold."""
    problems = []
    for spec in checks.get("files", []):
        kind = spec["type"]
        if kind == "absent":
            why = _check_absent(repo, spec)
        elif kind == "present_in_each":
            why = _check_present_in_each(repo, spec)
        elif kind == "unchanged":
            why = _check_unchanged(repo, task, spec)
        elif kind == "changed":
            why = _check_changed(repo, task, spec)
        else:
            why = "unknown check type %s" % kind
        if why:
            problems.append(why)
    return problems


def check_answer(answer_text, checks):
    """None when the answer holds or none is required, else a failure text."""
    spec = checks.get("answer")
    if not spec:
        return None
    flat = re.sub(r"\s+", " ", answer_text or "")
    if re.search(spec["regex"], flat):
        return None
    return "answer does not match %s" % spec["regex"]


def clean_env(home):
    """The environment of a scoring command: a search path and a HOME of its
    own, so no ``CC``, ``CFLAGS`` or ``MAKEFLAGS`` of the caller's shell can
    change what a build means."""
    return {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": home,
            "LANG": "C", "LC_ALL": "C"}


def run_argv(argv, cwd, label):
    """(ok, note): run one command bounded, in a clean environment."""
    try:
        done = subprocess.run(argv, cwd=cwd, env=clean_env(cwd),
                              stdin=subprocess.DEVNULL,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=COMMAND_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        return False, "%s timed out" % label
    except OSError as exc:
        return False, "%s: %s" % (label, exc)
    if done.returncode == 0:
        return True, ""
    tail = done.stdout.decode("utf-8", "replace").strip().replace("\n", " | ")
    return False, "%s rc=%d: %s" % (label, done.returncode,
                                    tail[-NOTE_TAIL_CHARS:])


def run_command(repo, command):
    """(ok, note): run a build or test command in the copy, bounded."""
    if not command:
        return None, "no command"
    return run_argv(shlex.split(command), repo, command)


def run_hidden(repo, task, spec):
    """(ok, note): the held-out program, built with the address and undefined
    behaviour sanitizers against the copy's own sources.

    The program lives in the fixture, outside the copy the editor saw.  It is
    built in a folder of its own from the copy's ``link`` and ``include``
    files, so nothing the run left in the copy can stand in for them, and the
    folder is removed by its exact path."""
    if not spec:
        return None, ""
    work = tempfile.mkdtemp(prefix="xwpe-eval-hidden-")
    try:
        for name in list(spec.get("link", [])) + list(spec.get("include", [])):
            data = _read_bytes(os.path.join(repo, name))
            if data is None:
                return False, "hidden check: %s missing from the copy" % name
            with open(os.path.join(work, name), "wb") as fh:
                fh.write(data)
        program = os.path.basename(spec["source"])
        shutil.copy2(os.path.join(fixture_dir(task), spec["source"]),
                     os.path.join(work, program))
        argv = ["cc", "-g", "-O0", "-fsanitize=address,undefined",
                "-fno-sanitize-recover=all", "-o", "hidden_check", program]
        argv.extend(spec.get("link", []))
        ok, note = run_argv(argv, work, "hidden build")
        if not ok:
            return False, note
        return run_argv([os.path.join(work, "hidden_check")], work,
                        "hidden check")
    finally:
        shutil.rmtree(work)


def score_repo(repo, task, answer_text=""):
    """The outcome of a finished run: files_ok, build_ok, test_ok and notes.

    The file checks run first, before ``make`` leaves a binary behind."""
    checks = load_checks(task)
    notes = []
    problems = check_files(repo, task, checks)
    why = check_answer(answer_text, checks)
    if why:
        problems.append(why)
    notes.extend(problems)
    build_ok, note = run_command(repo, checks.get("build"))
    if note:
        notes.append(note)
    test_ok, note = run_command(repo, checks.get("test"))
    if note:
        notes.append(note)
    hidden_ok, note = run_hidden(repo, task, checks.get("hidden"))
    if note:
        notes.append(note)
    if hidden_ok is False:
        test_ok = False
    return {"files_ok": not problems, "build_ok": build_ok,
            "test_ok": test_ok, "notes": notes}


# -- the trace ---------------------------------------------------------------

_KV = re.compile(r"(\w+)=(\S+)")


def _ints(lines, key):
    values = []
    for ln in lines:
        for k, v in _KV.findall(ln):
            if k == key and v.lstrip("-").isdigit():
                values.append(int(v))
    return values


def trace_facts(trace_text):
    """What the assistant's trace says about the run.

    Per-turn columns are filled only when the trace has ``agent turn=`` lines;
    before that the assistant writes none, and the cells stay None."""
    lines = (trace_text or "").splitlines()
    facts = {
        "tool_calls": sum(1 for ln in lines if "agent tool=" in ln),
        "denials": sum(1 for ln in lines if "agent deny" in ln),
        "turns": None, "tok_in": None, "tok_out": None, "ctx_peak": None,
        "folds": None, "loop_stops": None, "cut_retries": None,
        "malformed": None, "end_reason": None,
    }
    turn_lines = [ln for ln in lines if "agent turn=" in ln]
    if turn_lines:
        tin = _ints(turn_lines, "tokens_in")
        tout = _ints(turn_lines, "tokens_out")
        facts["turns"] = len(turn_lines)
        facts["tok_in"] = sum(tin) if tin else None
        facts["tok_out"] = sum(tout) if tout else None
        facts["ctx_peak"] = max(tin) if tin else None
        facts["folds"] = sum(1 for ln in lines if "agent fold " in ln)
        facts["cut_retries"] = sum(1 for ln in lines if "agent cut " in ln)
        facts["loop_stops"] = sum(1 for ln in lines
                                  if "agent end" in ln and "reason=loop" in ln)
    for ln in lines:
        m = re.search(r"agent end .*reason=(\S+)", ln)
        if m:
            facts["end_reason"] = m.group(1)
    return facts


# Whole trace lines the editor writes when a run ends, and the end they mean.
# Compared as whole lines: a tool argument or a logged request that merely
# contains these words must not end the run.
TRACE_END_LINES = {
    "agent done": "done",
    "agent answer (no marker)": "no-marker",
    "conv error (stopped)": "error",
    "conv transport error (stopped)": "transport",
}


def end_reason_from_trace(trace_text):
    """done / no-marker / error / transport / None from the trace.

    A backend or transport failure stops the run with a trace line and a pane
    row the screen check does not recognise, so without these lines such a
    run would only end at its deadline."""
    lines = (trace_text or "").splitlines()
    for ln in lines:
        m = re.search(r"agent end .*reason=(\S+)", ln)
        if m:
            return m.group(1)
    for ln in lines:
        reason = TRACE_END_LINES.get(ln.strip())
        if reason:
            return reason
    return None


def make_row(task, lane, model, run, end_reason, wall_s, trace_text,
             outcome, cost_usd, extra_notes=()):
    """One result row with the design's columns."""
    facts = trace_facts(trace_text)
    notes = list(extra_notes) + list(outcome["notes"])
    row = {
        "task": task, "lane": lane, "model": model, "run": run,
        "done": end_reason == "done",
        "files_ok": outcome["files_ok"],
        "build_ok": outcome["build_ok"],
        "test_ok": outcome["test_ok"],
        "turns": facts["turns"], "tool_calls": facts["tool_calls"],
        "tok_in": facts["tok_in"], "tok_out": facts["tok_out"],
        "ctx_peak": facts["ctx_peak"], "wall_s": round(wall_s, 1),
        "end_reason": end_reason, "folds": facts["folds"],
        "loop_stops": facts["loop_stops"], "cut_retries": facts["cut_retries"],
        "malformed": facts["malformed"], "denials": facts["denials"],
        "cost_usd": cost_usd, "notes": "; ".join(notes),
    }
    row["ts"] = time.strftime("%Y-%m-%dT%H:%M:%S")
    return row


def _cell(value):
    if value is None:
        return ""
    if value is True:
        return "1"
    if value is False:
        return "0"
    return str(value)


def render_table(rows):
    """The markdown table of the overseer's loop."""
    out = ["| " + " | ".join(TABLE_COLUMNS) + " |",
           "|" + "|".join("---" for _ in TABLE_COLUMNS) + "|"]
    for row in rows:
        out.append("| " + " | ".join(_cell(row.get(c))
                                     for c in TABLE_COLUMNS) + " |")
    return "\n".join(out) + "\n"


def read_rows(paths):
    rows = []
    for path in paths:
        with open(path) as fh:
            for line in fh:
                line = line.strip()
                if line:
                    rows.append(json.loads(line))
    return rows


def main(argv):
    if len(argv) >= 4 and argv[1] == "check":
        answer = ""
        if len(argv) >= 6 and argv[4] == "--answer-file":
            with open(argv[5]) as fh:
                answer = fh.read()
        result = score_repo(os.path.abspath(argv[3]), argv[2], answer)
        print(json.dumps(result))
        return 0
    if len(argv) >= 3 and argv[1] == "table":
        sys.stdout.write(render_table(read_rows(argv[2:])))
        return 0
    sys.stderr.write(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
