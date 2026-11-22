#!/usr/bin/env python3
"""check_test_waits.py -- one owner decides how long a wait lasts.

A row waits for a screen or a trace line for a time it names.  On a slow or
loaded host that time is stretched by XWPE_TEST_WAIT_SCALE, and the stretch
is applied in one place: tests/gui_owner.py reads the variable (scaled()),
and tests/wpe_driver.py (wait_deadline, wait_screen) turns a row's seconds
into a deadline.  A row that reads the variable itself, loops on its own
clock, or stretches a time before it hands it to the owner makes the wait
too short next to a mock reply that is stretched (the late reply came after
the wait ended) or too long (stretched twice).

Rule WA: no Python file under tests/ except tests/gui_owner.py reads
XWPE_TEST_WAIT_SCALE from the environment (environ.get, environ[...],
getenv).  Writing it into the environment of a child is not a read.

Rule WB: no file except tests/wpe_driver.py and tests/gui_owner.py runs a
wait loop of its own: no function named wait_screen or _wait_screen (any
suffix), and no deadline written as time.time() + tmo (with or without a
factor).  A file asks wpe_driver.wait_screen, or takes its deadline from
wpe_driver.wait_deadline.

Rule WC: no call of wait_screen passes a time that is already stretched (an
argument that names WAIT_SCALE or calls scaled()): the owner stretches it,
so the wait would last the scale twice.

The files of tests/eval/ are not scanned (another session's experiments);
nor is this file, which holds the names.

  check_test_waits.py [--srcdir DIR] [--selftest]

--selftest plants one offence per rule and clean files in a scratch folder
and fails unless the scan names the offences and not the clean files.  With
no option the self-test runs, then the tree.

Exit: 0 clean, 1 offenders or a blind scanner.
"""
import argparse
import ast
import os
import re
import sys
import tempfile

RULE_READ = "WA"
RULE_LOOP = "WB"
RULE_DOUBLE = "WC"
RULES = (RULE_READ, RULE_LOOP, RULE_DOUBLE)
SCALE_VARIABLE = "XWPE_TEST_WAIT_SCALE"
SCALE_NAME = "WAIT_SCALE"
# The functions that stretch a time by the scale, or return the scale itself.
SCALING_CALLS = ("scaled", "wait_scale")
WAIT_SCREEN = "wait_screen"
# The owner's waits: each stretches the time it is given.
OWNER_WAITS = (WAIT_SCREEN, "wait_until", "wait_deadline")
# wait_screen, _wait_screen and the "_text" copy of it; a name that merely
# starts with it (a wait for the screen to go quiet) is another wait.
WAIT_SCREEN_DEF = re.compile(r"^_?wait_screen(_text)?$")
TIMEOUT_NAME = "tmo"
SCALE_OWNER = os.path.join("tests", "gui_owner.py")
WAIT_OWNER = os.path.join("tests", "wpe_driver.py")
SELF_FILE = os.path.join("tests", "check_test_waits.py")
SKIPPED_FOLDERS = ("eval", ".venv", "__pycache__")

# One offence per rule and clean files, planted by the self-test.  The text
# is a string, so this file holds no offence itself.
CANARIES = {
    "read_get.py": ('import os\n'
                    'W = float(os.environ.get("XWPE_TEST_WAIT_SCALE", "1"))\n',
                    RULE_READ),
    "read_index.py": ('import os\n'
                      'W = os.environ["XWPE_TEST_WAIT_SCALE"]\n', RULE_READ),
    "read_getenv.py": ('import os\n'
                       'W = os.getenv("XWPE_TEST_WAIT_SCALE")\n', RULE_READ),
    "loop_def.py": ('def _wait_screen(s, needle, tmo=12):\n'
                    '    return True\n', RULE_LOOP),
    "loop_def_text.py": ('def wait_screen_text(s, needle):\n'
                         '    return True\n', RULE_LOOP),
    "loop_deadline.py": ('import time\n'
                         'def f(tmo):\n'
                         '    end = time.time() + tmo\n', RULE_LOOP),
    "loop_factor.py": ('import time\n'
                       'def f(tmo, W):\n'
                       '    end = time.time() + tmo * W\n', RULE_LOOP),
    "double_name.py": ('from wpe_driver import wait_screen, WAIT_SCALE\n'
                       'wait_screen(s, "x", 8 * WAIT_SCALE)\n', RULE_DOUBLE),
    "double_scaled.py": ('import gui_owner\n'
                         'wait_screen(s, "x", tmo=gui_owner.scaled(8))\n',
                         RULE_DOUBLE),
    "double_bare.py": ('wait_screen(s, "x", tmo=scaled(8))\n', RULE_DOUBLE),
    "double_constant.py": ('LIMIT_S = 10 * WAIT_SCALE\n'
                           'wait_screen(s, "x", LIMIT_S)\n', RULE_DOUBLE),
    "double_chained.py": ('A_S = 2 * WAIT_SCALE\n'
                          'B_S = A_S + 1\n'
                          'wait_until(s, ready, B_S)\n', RULE_DOUBLE),
    "double_wait_scale.py": ('import gui_owner\n'
                             'wait_screen(s, "x", 4 * gui_owner.wait_scale())\n',
                             RULE_DOUBLE),
    "double_wait_scale_bare.py": ('from gui_owner import wait_scale\n'
                                  'wait_until(s, ready, 4 * wait_scale())\n',
                                  RULE_DOUBLE),
    "double_alias_name.py": ('from wpe_driver import WAIT_SCALE as WS\n'
                             'wait_screen(s, "x", 4 * WS)\n', RULE_DOUBLE),
    "double_alias_scaled.py": ('from gui_owner import scaled as sc\n'
                               'wait_screen(s, "x", sc(4))\n', RULE_DOUBLE),
    "double_alias_wait_scale.py": ('from gui_owner import wait_scale as ws\n'
                                   'LIMIT_S = 3 * ws()\n'
                                   'wait_screen(s, "x", LIMIT_S)\n', RULE_DOUBLE),
    "double_deadline.py": ('A_S = scaled(2)\n'
                           'end = wait_deadline(A_S)\n', RULE_DOUBLE),
    "clean.py": ('"""XWPE_TEST_WAIT_SCALE and _wait_screen in a docstring."""\n'
                 'import os, time\n'
                 '# time.time() + tmo in a comment\n'
                 'env = {"XWPE_TEST_WAIT_SCALE": "3"}\n'
                 'os.environ["XWPE_TEST_WAIT_SCALE"] = "3"\n'
                 'def f(s, tmo, delay_ms):\n'
                 '    end = wait_deadline(tmo)\n'
                 '    stop = time.time() + 5\n'
                 '    mock = 5000 * WAIT_SCALE\n'
                 '    return wait_screen(s, "x", 10)\n'
                 'MOCK_MS = int(5000 * WAIT_SCALE)\n'
                 'PLAIN_S = 12\n'
                 'wait_screen(s, "x", PLAIN_S)\n', None),
}


def _is_environ(node):
    """True for os.environ, or a bare environ."""
    if isinstance(node, ast.Attribute):
        return node.attr == "environ"
    return isinstance(node, ast.Name) and node.id == "environ"


def _is_variable(node):
    return isinstance(node, ast.Constant) and node.value == SCALE_VARIABLE


def _reads_variable(node):
    """True when @node reads XWPE_TEST_WAIT_SCALE: environ.get("..."),
    getenv("..."), environ["..."] used as a value (a store is not a read)."""
    if isinstance(node, ast.Call):
        func = node.func
        named = isinstance(func, ast.Attribute) and func.attr in ("get", "getenv")
        named = named or (isinstance(func, ast.Name) and func.id == "getenv")
        if named and node.args and _is_variable(node.args[0]):
            if isinstance(func, ast.Attribute) and func.attr == "get":
                return _is_environ(func.value)
            return True
    if isinstance(node, ast.Subscript) and isinstance(node.ctx, ast.Load):
        return _is_environ(node.value) and _is_variable(node.slice)
    return False


def _is_time_call(node):
    """True for time.time()."""
    return (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
            and node.func.attr == "time"
            and isinstance(node.func.value, ast.Name)
            and node.func.value.id == "time")


def _names_in(node):
    return {n.id for n in ast.walk(node) if isinstance(n, ast.Name)}


def _is_own_deadline(node):
    """True for time.time() + <an expression that names tmo>."""
    return (isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add)
            and _is_time_call(node.left)
            and TIMEOUT_NAME in _names_in(node.right))


def _is_wait_screen_def(node):
    return (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and WAIT_SCREEN_DEF.match(node.name) is not None)


def _called_name(node):
    func = node.func
    if isinstance(func, ast.Attribute):
        return func.attr
    return getattr(func, "id", "")


def _import_aliases(tree):
    """(local names of WAIT_SCALE, local names of the functions that stretch a
    time) a file gets by `from x import WAIT_SCALE as W` and the like."""
    names = {SCALE_NAME}
    calls = set(SCALING_CALLS)
    for node in ast.walk(tree):
        if isinstance(node, ast.ImportFrom):
            for alias in node.names:
                if alias.asname and alias.name == SCALE_NAME:
                    names.add(alias.asname)
                if alias.asname and alias.name in SCALING_CALLS:
                    calls.add(alias.asname)
    return names, calls


def _is_scaling(node, stretched, aliases):
    """True when @node is the name WAIT_SCALE (or an alias of it), a call of
    scaled() or wait_scale() (or an alias, or a method of a module such as
    gui_owner.wait_scale()), or a name of the file that is already a
    stretched time (@stretched)."""
    names, calls = aliases
    if isinstance(node, ast.Name):
        return node.id in names or node.id in stretched
    if isinstance(node, ast.Attribute):
        return node.attr == SCALE_NAME
    return isinstance(node, ast.Call) and _called_name(node) in calls


def _stretched_names(tree, aliases):
    """The names the file gives a time it has already multiplied by the
    scale (`LIMIT_S = 10 * WAIT_SCALE`, and a name built from one): handing
    one to the owner stretches it twice."""
    assigned = {}
    for node in ast.walk(tree):
        if isinstance(node, ast.Assign) and len(node.targets) == 1 \
                and isinstance(node.targets[0], ast.Name):
            assigned[node.targets[0].id] = node.value
    stretched = set()
    grew = True
    while grew:
        grew = False
        for name, value in assigned.items():
            if name not in stretched and any(
                    _is_scaling(sub, stretched, aliases) for sub in ast.walk(value)):
                stretched.add(name)
                grew = True
    return stretched


def _passes_stretched_time(node, stretched, aliases):
    """True for a call of the owner's wait with a stretched time in its
    arguments."""
    if not isinstance(node, ast.Call) or _called_name(node) not in OWNER_WAITS:
        return False
    args = list(node.args) + [kw.value for kw in node.keywords]
    return any(_is_scaling(sub, stretched, aliases) for arg in args for sub in ast.walk(arg))


def offences(source, rel, name="<source>"):
    """{rule: [line, ...]} for the Python @source of the file @rel."""
    tree = ast.parse(source, filename=name)
    found = {rule: [] for rule in RULES}
    aliases = _import_aliases(tree)
    stretched = _stretched_names(tree, aliases)
    owns_scale = rel == SCALE_OWNER
    owns_wait = rel in (SCALE_OWNER, WAIT_OWNER)
    for node in ast.walk(tree):
        line = getattr(node, "lineno", None)
        if line is None:
            continue
        if not owns_scale and _reads_variable(node):
            found[RULE_READ].append(line)
        if not owns_wait and (_is_wait_screen_def(node) or _is_own_deadline(node)):
            found[RULE_LOOP].append(line)
        if _passes_stretched_time(node, stretched, aliases):
            found[RULE_DOUBLE].append(line)
    return found


def _python_files(root):
    """Every .py file below @root/tests, skipping the folders of
    SKIPPED_FOLDERS, as paths relative to @root."""
    base = os.path.join(root, "tests")
    for folder, names, files in os.walk(base):
        names[:] = [n for n in names if n not in SKIPPED_FOLDERS]
        for name in files:
            if name.endswith(".py"):
                yield os.path.relpath(os.path.join(folder, name), root)


def scan(root):
    """{(file, rule): [line, ...]} of the offences in the tree at @root."""
    found = {}
    for rel in sorted(_python_files(root)):
        if rel == SELF_FILE:
            continue
        with open(os.path.join(root, rel), encoding="utf-8",
                  errors="replace") as fh:
            hits = offences(fh.read(), rel, rel)
        for rule, lines in hits.items():
            if lines:
                found[(rel, rule)] = sorted(set(lines))
    return found


def report(found):
    """The lines that name the offences, one per file and rule."""
    return ["%s: rule %s at line %s" % (rel, rule, ", ".join(map(str, lines)))
            for (rel, rule), lines in sorted(found.items())]


def selftest():
    """Plant the canaries; True when each is named for its rule and the
    clean file is not."""
    ok = True
    with tempfile.TemporaryDirectory(prefix="waits.") as root:
        os.makedirs(os.path.join(root, "tests"))
        for name, (text, _rule) in CANARIES.items():
            with open(os.path.join(root, "tests", name), "w",
                      encoding="utf-8") as fh:
                fh.write(text)
        found = scan(root)
        for name, (_text, rule) in CANARIES.items():
            rel = os.path.join("tests", name)
            hit = {r for (f, r) in found if f == rel}
            if rule is None and hit:
                sys.stderr.write("check_test_waits: clean file %s was named: %s\n"
                                 % (name, sorted(hit)))
                ok = False
            if rule is not None and hit != {rule}:
                sys.stderr.write("check_test_waits: canary %s named %s, want %s\n"
                                 % (name, sorted(hit), rule))
                ok = False
    return ok


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--srcdir", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), ".."))
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)
    if not selftest():
        return 1
    if args.selftest:
        print("check_test_waits: the canaries are named, the clean file is not")
        return 0
    found = scan(os.path.abspath(args.srcdir))
    if found:
        for line in report(found):
            sys.stderr.write("check_test_waits: %s\n" % line)
        sys.stderr.write("check_test_waits: %d file(s) wait on a clock of their "
                         "own; use wpe_driver.wait_screen / wait_deadline\n"
                         % len({rel for rel, _rule in found}))
        return 1
    print("check_test_waits: one owner decides how long a wait lasts")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
