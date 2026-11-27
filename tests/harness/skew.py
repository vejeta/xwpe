#!/usr/bin/env python3
"""skew.py -- a row gives the same answer at every wait scale.

A row that passes at XWPE_TEST_WAIT_SCALE=1 and fails at 3 waits for something
on a clock it does not own.  No scan of its text can say which row that is, so
this tool is the net with no list: it runs the same rows twice on the same host
with the same editor, once at scale 1 and once at SKEW_SCALE, and names every
row whose result moved.

  skew.py run   [--base REV] [--head REV] [--scale N] [--jobs N] [--reruns N]
                [--keep] [TARGET ...] [-- PYTEST_ARG ...]
  skew.py diff  [--known FILE] LOCAL.xml CI.xml
  skew.py selftest

`run` picks the rows of a pass by AST and never from a hand list: a row is
picked when its own body differs from --base, or when it reaches a module-level
name that differs (a constant, a helper of its own file) through the functions
of its own file.  A name a row imports belongs to another module, so a pass
that changes a shared file (gui_owner.py, program_gate.py, a conftest.py) is
told so by name and must name the rows that change carries itself.  TARGETs
narrow the pick to the files or the rows they name; with none, every row file
that differs from --base is picked.  --head moves the newer side of the pick to
a revision; what runs is always the working tree.

The two scales run one after the other and never at the same time, with the
same --jobs, so the second run meets the host the first one left.  A row whose
result differs runs again, up to --reruns more times, and the verdict is:

  BLOCKS    the row differs again in a rerun, or it is a row the pass picked
            and it differed once: a race a pass brings in is the pass's own
  UNSTABLE  a row outside the pick differed once and agreed in every rerun

`diff` reads two junit reports made at different scales or on different hosts
(the local suite at a capability close and the CI run) and names the rows that
moved, unless --known names them.  It is advisory: such a pair differs in the
host, the tools and the load as well as in the scale, so every row it names is
decided by a `run` on one host, and it never fails a capability close by itself
(its exit status is 0 whatever it finds).

Run it with the suite's own interpreter: every child it starts is
`sys.executable -m pytest`, so both scales run under the same python and the
same editor.  A live row (test_*_live*.py) is refused whatever names it, and a
row of a display layer is left to the layer that can show it unless a TARGET
names it.

Exit: `run` 0 when nothing blocks and 1 when a row blocks or nothing could be
picked, `diff` always 0, `selftest` 0 when every canary is judged as its rule
says it is and 1 when one is not.
"""
import argparse
import ast
import os
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from collections import namedtuple

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.dirname(HERE)
TOP = os.path.dirname(TESTS)

# The scale a slow host runs at: what the CI suite and the macOS workflow set.
SKEW_SCALE = 3
PLAIN_SCALE = 1
# A row that differs is run again this many times: one differ and then any
# differ in the reruns blocks the pass.
RERUNS = 2
# Both scales run with the same number of workers, the one the suite measures
# its timings with.
JOBS = 4
LIVE_MARK = "_live"
ROW_PREFIX = "test"
# Folders whose rows need a display the terminal layer does not have.
DISPLAY_FOLDERS = (os.path.join("tests", "x11"), os.path.join("tests", "wayland"))
# Folders of the suite that hold no row of the product.
SKIPPED_FOLDERS = ("eval", ".venv", "__pycache__", "harness")

PASSED = "passed"
FAILED = "failed"
SKIPPED = "skipped"
# A row one junit holds and the other does not: a crash, or a cut job.
MISSING = "missing"
JUNIT_OUTCOMES = (("failure", FAILED), ("error", FAILED), ("skipped", SKIPPED))

BLOCKS = "BLOCKS"
UNSTABLE = "UNSTABLE"

# The two results of one round of a row, at each scale.
Round = namedtuple("Round", "plain skew")

# The canary rows of `selftest`: one that races a program's fixed stay with a
# drain of its own, and its twin that waits for the mark the program writes.
# No editor runs either, and tests/check_test_waits.py exempts this file for
# that reason.
RACING_ROWS = '''"""The two rows the skew run's own self-test plants."""
import os
import subprocess
import sys
import time

STAY_S = 2.0     # the program's own stay: a fixed bound, never stretched
DRAIN_S = 1.0    # the row's drain: stretched by the wait scale
MARK_S = 0.5     # how long the twin's program takes to write its mark
POLL_S = 0.05


def scale():
    """The factor this run stretches the row's own waits by."""
    return float(os.environ.get("XWPE_TEST_WAIT_SCALE", "1"))


def stay(wanted_s, mark=None):
    """A program that stays for a fixed time of its own and writes @mark."""
    body = "import time; time.sleep(%r)" % wanted_s
    if mark is not None:
        body += "; open(%r, 'w').close()" % mark
    return subprocess.Popen([sys.executable, "-c", body])


def wait_for_mark(mark, bound_s):
    """True when @mark is there within @bound_s."""
    end = time.monotonic() + bound_s
    while time.monotonic() < end:
        if os.path.exists(mark):
            return True
        time.sleep(POLL_S)
    return os.path.exists(mark)


def test_a_row_that_drains_past_the_program_stay():
    """Red at scale 3: the row's own drain outlasts the program's fixed stay."""
    program = stay(STAY_S)
    try:
        time.sleep(DRAIN_S * scale())
        assert program.poll() is None, "the program ended inside the drain"
    finally:
        program.wait()


def test_the_gated_twin_waits_for_the_program_mark(tmp_path):
    """Green at both scales: the row waits for the mark, not for a bound."""
    mark = str(tmp_path / "passed-1")
    program = stay(MARK_S, mark)
    try:
        assert wait_for_mark(mark, 5.0 * scale()), "the mark never came"
    finally:
        program.wait()
'''

# The pick's canary: a module constant changes and one row's own body changes,
# so two rows are picked and the third is not.
SELECTION_BASE = '''"""The file the pick's canary diffs against."""
TICK_MS = 100
STAY_S = 600


def tick_line():
    """The line a program ticks with."""
    return TICK_MS


def test_a_row_that_reaches_the_changed_constant():
    assert tick_line() > 0


def test_a_row_whose_own_body_changed():
    assert STAY_S == 600


def test_a_row_the_change_does_not_reach():
    assert STAY_S > 0
'''
SELECTION_CURRENT = SELECTION_BASE.replace(
    "TICK_MS = 100", "TICK_MS = 250").replace(
        "assert STAY_S == 600", "assert STAY_S > 0")
PICKED_ROWS = ("test_a_row_that_reaches_the_changed_constant",
               "test_a_row_whose_own_body_changed")
LEFT_ROWS = ("test_a_row_the_change_does_not_reach",)

# The diff canary: two reports of the same two rows, one of them red in the
# second, and a known file that names it.  The classname is one of this tree, so
# the node id is the one a real report gives.
JUNIT_PLAIN = ('<?xml version="1.0" encoding="utf-8"?>\n'
               '<testsuites><testsuite name="pytest" tests="2" failures="%d">\n'
               '<testcase classname="tests.test_child_ends" name="test_agrees"'
               ' time="0.1"/>\n'
               '<testcase classname="tests.test_child_ends" name="test_moves"'
               ' time="0.1">%s</testcase>\n'
               '</testsuite></testsuites>\n')
JUNIT_FAILURE = '<failure message="the row moved">assert</failure>'
KNOWN_ROW = "tests/test_child_ends.py::test_moves"


class SkewError(Exception):
    """A skew run that cannot judge: no rows, no junit, no git."""


def scale_variable():
    """The name of the variable a wait scale travels in, read from its owner so
    this file never spells it."""
    if TESTS not in sys.path:
        sys.path.insert(0, TESTS)
    try:
        import gui_owner
        return gui_owner.WAIT_SCALE_VAR
    finally:
        sys.path.pop(0)


def editor_binary():
    """The editor both scales run against, for the report's evidence line."""
    return os.environ.get("WPE_BIN") or os.path.join(TOP, "wpe")


# ---------------------------------------------------------------------------
# The pick: what a change reaches, by AST


def git_lines(tail):
    """The lines git prints for @tail in this tree."""
    done = subprocess.run(["git", *tail], cwd=TOP, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, text=True)
    if done.returncode != 0:
        raise SkewError("git %s ended with %d: %s"
                        % (" ".join(tail), done.returncode, done.stderr.strip()))
    return [line for line in done.stdout.splitlines() if line]


def side_text(rev, rel):
    """The text of @rel on side @rev (the working tree when @rev is empty), or
    None when that side does not hold the file."""
    if not rev:
        path = os.path.join(TOP, rel)
        if not os.path.exists(path):
            return None
        with open(path, encoding="utf-8", errors="replace") as fh:
            return fh.read()
    done = subprocess.run(["git", "show", "%s:%s" % (rev, rel)], cwd=TOP,
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    if done.returncode != 0:
        return None
    return done.stdout.decode("utf-8", "replace")


def changed_paths(base, head):
    """The paths that differ between @base and @head (the working tree when
    @head is empty), plus the untracked ones below tests/ when @head is it."""
    tail = ["diff", "--name-only", base]
    if head:
        tail.append(head)
    paths = set(git_lines(tail))
    if not head:
        paths.update(git_lines(["ls-files", "--others", "--exclude-standard",
                                "tests"]))
    return paths


def is_row_file(rel):
    """True for a file of pytest rows this tool may run."""
    return (os.path.basename(rel).startswith(ROW_PREFIX)
            and rel.endswith(".py")
            and LIVE_MARK not in os.path.basename(rel)
            and not any(part in SKIPPED_FOLDERS for part in rel.split(os.sep)))


def is_display_row_file(rel):
    """True for a row file of a layer that needs a display."""
    return rel.startswith(DISPLAY_FOLDERS)


def definitions(tree):
    """{a name the file defines: what it is defined as} for the constants, the
    functions and the classes at module level, and for the methods of a class.
    A dump carries no line numbers, so a name that only moved is not a changed
    name."""
    defined = {}
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            defined[node.name] = ast.dump(node)
            if isinstance(node, ast.ClassDef):
                for method in node.body:
                    if isinstance(method, (ast.FunctionDef, ast.AsyncFunctionDef)):
                        key = "%s.%s" % (node.name, method.name)
                        defined[key] = ast.dump(method)
        elif isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name):
                    defined[target.id] = ast.dump(node.value)
        elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
            defined[node.target.id] = ast.dump(node.value)
    return defined


def _names_used(node, known):
    """The names of @known that @node's body mentions."""
    return {sub.id for sub in ast.walk(node)
            if isinstance(sub, ast.Name) and sub.id in known}


def uses_map(tree, known):
    """{a function or a class of the file: the names of the file it reaches}, so
    a row is followed through the helpers of its own file."""
    uses = {}
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            uses[node.name] = _names_used(node, known)
        elif isinstance(node, ast.ClassDef):
            uses[node.name] = _names_used(node, known)
            for method in node.body:
                if isinstance(method, (ast.FunctionDef, ast.AsyncFunctionDef)):
                    key = "%s.%s" % (node.name, method.name)
                    uses[key] = _names_used(method, known)
    return uses


def row_names(tree):
    """The rows of a file as (the node id pytest wants, the name their own body
    is filed under, the classes they are in)."""
    rows = []
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) \
                and node.name.startswith(ROW_PREFIX):
            rows.append((node.name, node.name, ()))
        elif isinstance(node, ast.ClassDef):
            for method in node.body:
                if isinstance(method, (ast.FunctionDef, ast.AsyncFunctionDef)) \
                        and method.name.startswith(ROW_PREFIX):
                    rows.append(("%s::%s" % (node.name, method.name),
                                 "%s.%s" % (node.name, method.name),
                                 (node.name,)))
    return rows


def closure(start, uses):
    """The names @start reaches through the functions of its own file."""
    reached = {start}
    stack = [start]
    while stack:
        for used in uses.get(stack.pop(), ()):
            if used not in reached:
                reached.add(used)
                stack.append(used)
    return reached


def picked_rows(base_text, text):
    """(the rows of a file a change reaches, the names that changed) between
    @base_text and @text.  A row is reached when its own body changed, when its
    class changed, or when a name it reaches through the file's own functions
    changed.  An empty @base_text is a new file, so every row of it is picked."""
    tree = ast.parse(text)
    before = definitions(ast.parse(base_text)) if base_text else {}
    now = definitions(tree)
    changed = {name for name, body in now.items() if before.get(name) != body}
    uses = uses_map(tree, set(now))
    rows = []
    for node_id, key, classes in row_names(tree):
        if (closure(key, uses) | set(classes)) & changed:
            rows.append(node_id)
    return sorted(rows), sorted(changed)


def pick(base, head, targets):
    """(the pytest targets to run, the node ids the pass picked, the notes to
    print) for @targets against the pick @base..@head."""
    run_targets, picked, notes = [], set(), []
    paths = changed_paths(base, head)
    files = [target for target in targets if "::" not in target]
    named = [target for target in targets if "::" in target]
    if not files:
        files = sorted(rel for rel in paths
                       if is_row_file(rel) and not is_display_row_file(rel))
        for rel in sorted(rel for rel in paths if is_row_file(rel)
                          and is_display_row_file(rel)):
            notes.append("%s needs its display layer; run it there" % rel)
    for rel in files:
        if LIVE_MARK in rel:
            raise SkewError("refusing a live row: %s" % rel)
        if not is_row_file(rel):
            raise SkewError("%s is not a row file of the suite" % rel)
        text = side_text(head, rel)
        if text is None:
            notes.append("%s is not on the newer side of %s; nothing to run"
                         % (rel, base))
            continue
        before = side_text(base, rel)
        if before == text:
            notes.append("%s is unchanged since %s; name its rows to run them"
                         % (rel, base))
            continue
        rows, changed = picked_rows(before or "", text)
        notes.append("%s: %d row(s) reached by %d changed name(s)"
                     % (rel, len(rows), len(changed)))
        run_targets += ["%s::%s" % (rel, row) for row in rows]
        picked.update("%s::%s" % (rel, row) for row in rows)
    for node_id in named:
        if LIVE_MARK in node_id:
            raise SkewError("refusing a live row: %s" % node_id)
        run_targets.append(node_id)
    for rel in sorted(paths):
        if rel.startswith("tests") and rel.endswith(".py") and not is_row_file(rel):
            notes.append("%s changed and no row's own AST can see it: name the "
                         "rows it carries" % rel)
    return run_targets, picked, notes


# ---------------------------------------------------------------------------
# The runs and the verdict


def row_id(classname, name, root):
    """The pytest node id of a junit row, as @root sees it: the file its
    classname names (the longest prefix of it that is a file under @root), the
    class it ran in when it ran in one, and the row.  The id is one pytest can
    be given back."""
    parts = [part for part in classname.split(".") if part]
    for count in range(len(parts), 0, -1):
        rel = os.path.join(*parts[:count]) + ".py"
        if os.path.exists(os.path.join(root, rel)):
            return "::".join([rel] + parts[count:] + [name])
    return "%s::%s" % (classname, name)


def row_function(node_id):
    """The node id of a row without its parameter: what the pick names."""
    return node_id.split("[", 1)[0]


def case_outcome(case):
    """What one testcase of a junit report ended as."""
    for tag, outcome in JUNIT_OUTCOMES:
        if case.find(tag) is not None:
            return outcome
    return PASSED


def read_junit(path, root):
    """{node id: outcome} of the pytest junit report at @path."""
    rows = {}
    for case in ET.parse(path).getroot().iter("testcase"):
        rows[row_id(case.get("classname", ""), case.get("name", ""), root)] = \
            case_outcome(case)
    return rows


def run_once(scale, targets, folder, jobs, cwd, extra):
    """Run @targets once at @scale in @folder; {node id: outcome} of the junit
    it wrote.  The pytest output stays in the folder as the evidence of the
    round."""
    os.makedirs(folder, exist_ok=True)
    tag = "%g" % scale
    report = os.path.join(folder, "junit.xml")
    log = os.path.join(folder, "pytest.log")
    parallel = ["-n", str(jobs)] if jobs > 1 else []
    environment = dict(os.environ)
    environment[scale_variable()] = tag
    with open(log, "wb") as captured:
        subprocess.run(
            [sys.executable, "-m", "pytest", "-q", "-p", "no:cacheprovider",
             "--junitxml=" + report,
             "--basetemp=" + os.path.join(folder, "pyte"),
             *parallel, *extra, *targets],
            cwd=cwd, env=environment, stdout=captured, stderr=subprocess.STDOUT)
    if not os.path.exists(report):
        raise SkewError("pytest at scale %s wrote no junit; see %s" % (tag, log))
    rows = read_junit(report, cwd)
    if not rows:
        raise SkewError("pytest at scale %s collected none of the %d target(s); "
                        "see %s" % (tag, len(targets), log))
    return rows


def run_rounds(targets, picked, folder, scale, jobs, reruns, cwd, extra=()):
    """{node id: [Round, ...]} for @targets at scale 1 and then at @scale,
    running the rows that differ again, up to @reruns more times.  The two
    scales never run at the same time."""
    results = {}
    pending = list(targets)
    for number in range(reruns + 1):
        if not pending:
            break
        round_folder = os.path.join(folder, "round-%d" % (number + 1))
        plain = run_once(PLAIN_SCALE, pending,
                         os.path.join(round_folder, "scale-%d" % PLAIN_SCALE),
                         jobs, cwd, extra)
        skew = run_once(scale, pending,
                        os.path.join(round_folder, "scale-%d" % scale),
                        jobs, cwd, extra)
        moved = []
        for row in sorted(set(plain) | set(skew)):
            results.setdefault(row, []).append(
                Round(plain.get(row, MISSING), skew.get(row, MISSING)))
            if plain.get(row, MISSING) != skew.get(row, MISSING):
                moved.append(row)
        sys.stderr.write("skew: round %d of %d at scale 1 then %g: %d row(s), "
                         "%d differ\n" % (number + 1, reruns + 1, scale,
                                          len(set(plain) | set(skew)), len(moved)))
        pending = moved
    return results


def verdict_for(node_id, rounds, picked):
    """BLOCKS, UNSTABLE or None for the rounds of one row: a row the pass picked
    may not differ at all, and a row outside it may differ once and agree in
    every rerun."""
    differs = [one.plain != one.skew for one in rounds]
    if not differs or not differs[0]:
        return None
    if row_function(node_id) in picked or any(differs[1:]):
        return BLOCKS
    return UNSTABLE


def row_lines(results, picked, scale):
    """The lines naming every row that moved at @scale, and the counts of the
    verdict."""
    lines, blocking, unstable = [], 0, 0
    for node_id in sorted(results):
        rounds = results[node_id]
        verdict = verdict_for(node_id, rounds, picked)
        if verdict is None:
            continue
        if verdict == BLOCKS:
            blocking += 1
        else:
            unstable += 1
        first = rounds[0]
        lines.append("skew: %s %s  scale 1 %s, scale %g %s (differed in %d of "
                     "%d round(s))" % (verdict, node_id, first.plain, scale,
                                       first.skew,
                                       sum(one.plain != one.skew for one in rounds),
                                       len(rounds)))
    return lines, blocking, unstable


def cmd_run(args, extra):
    """Run the picked rows at both scales and report what moved."""
    targets, picked, notes = pick(args.base, args.head, args.targets)
    for note in notes:
        sys.stderr.write("skew: %s\n" % note)
    if not targets:
        raise SkewError("no row to run: name a row file that changed, or a row")
    folder = tempfile.mkdtemp(prefix="skew.")
    sys.stderr.write("skew: %d target(s), %s, scale 1 then %g, %d worker(s), "
                     "run folder %s\n"
                     % (len(targets), editor_binary(), args.scale, args.jobs,
                        folder))
    try:
        results = run_rounds(targets, picked, folder, args.scale, args.jobs,
                             args.reruns, TOP, extra)
        lines, blocking, unstable = row_lines(results, picked, args.scale)
        for line in lines:
            print(line)
        print("skew: %d row(s) ran, %d blocking, %d unstable"
              % (len(results), blocking, unstable))
        if args.keep:
            print("skew: the junits and the pytest logs are in %s" % folder)
        return 1 if blocking else 0
    finally:
        if not args.keep:
            shutil.rmtree(folder, ignore_errors=True)


def known_rows(path):
    """The node ids a known file names, when one is given."""
    if not path:
        return ()
    with open(path, encoding="utf-8") as fh:
        return tuple(line.split("#", 1)[0].strip()
                     for line in fh if line.split("#", 1)[0].strip())


def is_known(node_id, known):
    """True when @known names @node_id, or the file it is in."""
    return any(node_id == entry or node_id.startswith(entry + "::")
               for entry in known)


def diff_rows(local, other, known):
    """The lines naming the rows whose outcome differs between two junit
    reports, and the count of them."""
    first, second = read_junit(local, TOP), read_junit(other, TOP)
    lines, moved = [], 0
    for node_id in sorted(set(first) | set(second)):
        was, now = first.get(node_id, MISSING), second.get(node_id, MISSING)
        if was == now or is_known(node_id, known):
            continue
        moved += 1
        lines.append("skew diff: %s  %s=%s %s=%s"
                     % (node_id, os.path.basename(local), was,
                        os.path.basename(other), now))
    return lines, moved


def cmd_diff(args):
    """Name the rows two junit reports disagree about.  Advisory: the exit
    status is 0 whatever it finds, because such a pair differs in the host and
    the tools as well as in the scale."""
    lines, moved = diff_rows(args.local, args.other, known_rows(args.known))
    for line in lines:
        print(line)
    print("skew diff: %d row(s) moved; decide each with `skew.py run` on one "
          "host" % moved)
    return 0


# ---------------------------------------------------------------------------
# The canaries


def selection_canary():
    """True when the pick takes a row that reaches a changed constant and a row
    whose own body changed, and leaves the row the change does not reach."""
    ok = True
    rows, changed = picked_rows(SELECTION_BASE, SELECTION_CURRENT)
    for row in PICKED_ROWS:
        if row not in rows:
            sys.stderr.write("skew selftest: the pick left %s (picked %s, "
                             "changed %s)\n" % (row, rows, changed))
            ok = False
    for row in LEFT_ROWS:
        if row in rows:
            sys.stderr.write("skew selftest: the pick took %s, which the change "
                             "does not reach\n" % row)
            ok = False
    if "TICK_MS" not in changed:
        sys.stderr.write("skew selftest: the pick did not see TICK_MS change\n")
        ok = False
    fresh, _changed = picked_rows("", SELECTION_CURRENT)
    if len(fresh) != len(PICKED_ROWS) + len(LEFT_ROWS):
        sys.stderr.write("skew selftest: a new file picked %s, want every row\n"
                         % fresh)
        ok = False
    return ok


def verdict_canary():
    """True when a row the pass picked blocks on one differ, a row outside it
    is unstable on one and blocks on two."""
    ok = True
    agrees = [Round(PASSED, PASSED)] * 3
    differs_once = [Round(PASSED, FAILED), Round(PASSED, PASSED),
                    Round(PASSED, PASSED)]
    differs_twice = [Round(PASSED, FAILED), Round(PASSED, FAILED),
                     Round(PASSED, PASSED)]
    cases = (("picked.py::test_row", differs_once, {"picked.py::test_row"}, BLOCKS),
             ("picked.py::test_row[a]", differs_once, {"picked.py::test_row"}, BLOCKS),
             ("other.py::test_row", differs_once, set(), UNSTABLE),
             ("other.py::test_row", differs_twice, set(), BLOCKS),
             ("other.py::test_row", agrees, set(), None),
             ("picked.py::test_row", agrees, {"picked.py::test_row"}, None))
    for node_id, rounds, picked, want in cases:
        have = verdict_for(node_id, rounds, picked)
        if have != want:
            sys.stderr.write("skew selftest: %s with %s gave %s, want %s\n"
                             % (node_id, rounds, have, want))
            ok = False
    return ok


def diff_canary():
    """True when the diff names a row that moved and not one a known file
    names."""
    ok = True
    with tempfile.TemporaryDirectory(prefix="skew-diff.") as folder:
        plain = os.path.join(folder, "plain.xml")
        moved = os.path.join(folder, "moved.xml")
        known = os.path.join(folder, "known.txt")
        with open(plain, "w", encoding="utf-8") as fh:
            fh.write(JUNIT_PLAIN % (0, ""))
        with open(moved, "w", encoding="utf-8") as fh:
            fh.write(JUNIT_PLAIN % (1, JUNIT_FAILURE))
        with open(known, "w", encoding="utf-8") as fh:
            fh.write("# the rows a run on one host decided\n")
            fh.write(KNOWN_ROW + "\n")
        lines, moved_count = diff_rows(plain, moved, ())
        if moved_count != 1 or not any("test_moves" in line for line in lines):
            sys.stderr.write("skew selftest: the diff named %s, want the row "
                             "that moved\n" % lines)
            ok = False
        if any("test_agrees" in line for line in lines):
            sys.stderr.write("skew selftest: the diff named a row that did not "
                             "move\n")
            ok = False
        _lines, known_count = diff_rows(plain, moved, known_rows(known))
        if known_count:
            sys.stderr.write("skew selftest: the diff named %d row(s) a known "
                             "file names\n" % known_count)
            ok = False
    return ok


def run_canary(jobs):
    """True when a run of the planted rows at both scales names the racing row
    and leaves its gated twin alone."""
    ok = True
    folder = tempfile.mkdtemp(prefix="skew-run.")
    try:
        rows = os.path.join(folder, "test_skew_canary.py")
        with open(rows, "w", encoding="utf-8") as fh:
            fh.write(RACING_ROWS)
        results = run_rounds([rows], set(), folder, SKEW_SCALE, jobs, RERUNS,
                             folder)
        lines, blocking, _unstable = row_lines(results, set(), SKEW_SCALE)
        for line in lines:
            sys.stderr.write("%s\n" % line)
        racing = [row for row in results if "drains_past_the_program_stay" in row]
        gated = [row for row in results if "gated_twin" in row]
        if not racing or verdict_for(racing[0], results[racing[0]], set()) != BLOCKS:
            sys.stderr.write("skew selftest: the racing row was not named: %s\n"
                             % results)
            ok = False
        if not gated:
            sys.stderr.write("skew selftest: the gated twin never ran\n")
            ok = False
        elif verdict_for(gated[0], results[gated[0]], set()) is not None:
            sys.stderr.write("skew selftest: the gated twin was named: %s\n"
                             % results[gated[0]])
            ok = False
        if blocking != 1:
            sys.stderr.write("skew selftest: %d row(s) blocked, want 1\n" % blocking)
            ok = False
    finally:
        shutil.rmtree(folder, ignore_errors=True)
    return ok


def cmd_selftest(args):
    """Plant every canary and judge it."""
    ok = selection_canary()
    ok = verdict_canary() and ok
    ok = diff_canary() and ok
    ok = run_canary(args.jobs) and ok
    if not ok:
        return 1
    print("skew: the pick, the verdict, the diff and a run of the planted rows "
          "agree")
    return 0


def main(argv):
    """The command line: `run`, `diff` or `selftest`."""
    argv = list(argv)
    extra = ()
    if "--" in argv:
        at = argv.index("--")
        argv, extra = argv[:at], tuple(argv[at + 1:])
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    run = commands.add_parser("run", help="run rows at scale 1 and at SKEW_SCALE")
    run.add_argument("--base", default="HEAD",
                     help="the older side of the pick (a revision)")
    run.add_argument("--head", default="",
                     help="the newer side of the pick (default the working tree)")
    run.add_argument("--scale", type=float, default=SKEW_SCALE,
                     help="the slow scale to run against (default %(default)s)")
    run.add_argument("--jobs", type=int, default=JOBS,
                     help="workers for both scales (default %(default)s, 0 is "
                          "pytest's own serial run)")
    run.add_argument("--reruns", type=int, default=RERUNS,
                     help="reruns of a row that differs (default %(default)s)")
    run.add_argument("--keep", action="store_true",
                     help="keep the run folder and print its path")
    run.add_argument("targets", nargs="*", metavar="TARGET",
                     help="row files or node ids; default every changed file")
    diff = commands.add_parser("diff", help="name the rows two junits disagree "
                                            "about (advisory)")
    diff.add_argument("--known", default="",
                      help="a file of node ids already decided by a run")
    diff.add_argument("local", help="the junit of one side")
    diff.add_argument("other", help="the junit of the other side")
    probe = commands.add_parser("selftest", help="plant the canaries and judge "
                                                 "them")
    probe.add_argument("--jobs", type=int, default=0,
                       help="workers for the planted rows (default %(default)s)")
    args = parser.parse_args(argv)
    if args.command == "run":
        return cmd_run(args, extra)
    if args.command == "diff":
        return cmd_diff(args)
    return cmd_selftest(args)


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except SkewError as error:
        sys.stderr.write("skew: %s\n" % error)
        sys.exit(1)
