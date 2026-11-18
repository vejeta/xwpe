#!/usr/bin/env python3
"""check_test_processes.py -- a test learns about a process through procinfo.

A row that wants to know whether a child is "the program P", how much CPU a
process used or what state it is in asks tests/procinfo.py, which reads
/proc where it exists and ps(1) elsewhere and has one rule for naming a
program (Program.runs).  A row that runs ps(1) itself, or decides by a
process name it computed, decides by whatever column or string it happened
to get, which is how a row passes on Linux and means something else on
macOS.

Rule HA: no ps(1) argument vector outside tests/procinfo.py.  A ps argv is a
list or tuple literal whose first element is the string "ps".  Another
vector that merely holds "-E" (python -E, cc -E) is not one.

Rule HB: no name rule.  Outside tests/procinfo.py no file names the old
helpers that decided a child by a name (children_named, _live_and_named,
_ps_command_run, RUN_NAMES, _python3_run_name, processes_in, children_in),
reads basename(sys.executable) or the launcher variable of the framework
Python, or calls command_run (a report of what a process is, never a
decision).  An identifier, an attribute, an import, a definition or a string
literal counts; comments and docstrings do not.

The sites that remain are counted per file and rule in
tests/test-processes.allow (`file rule count pass`); a count may only fall,
and a line above the measurement is stale and fails.  The files of
tests/eval/ are not scanned: that folder holds experiments of another
session.  This file is not scanned either: it holds the names.

  check_test_processes.py [--srcdir DIR] [--allow FILE] [--selftest]

--selftest plants one offence per rule and a clean file in a scratch folder
and fails unless the scan names the offences and not the clean file (a
scanner that sees nothing is not a check).  With no option the self-test
runs, then the tree.

Exit: 0 clean, 1 offenders or a blind scanner.
"""
import argparse
import ast
import os
import sys
import tempfile

RULE_PS_ARGV = "HA"
RULE_NAME_RULE = "HB"
RULES = (RULE_PS_ARGV, RULE_NAME_RULE)
PS_PROGRAM = "ps"
OWNER_FILE = os.path.join("tests", "procinfo.py")
SELF_FILE = os.path.join("tests", "check_test_processes.py")
SKIPPED_FOLDERS = ("eval", ".venv", "__pycache__")

NAME_RULE_WORDS = ("children_named", "_live_and_named", "_ps_command_run",
                   "RUN_NAMES", "_python3_run_name", "processes_in",
                   "children_in", "command_run")
LAUNCHER_WORD = "__PYVENV_LAUNCHER__"

# One offence per rule and a clean file, planted by the self-test.  The text
# is a string, so this file holds no offence itself.
CANARIES = {
    "ps_list.py": ('import subprocess\n'
                   'subprocess.run(["ps", "-o", "comm=", "-p", "1"])\n',
                   RULE_PS_ARGV),
    "ps_tuple.py": ('ARGV = ("ps", "-A")\n', RULE_PS_ARGV),
    "name_call.py": ('import procinfo\n'
                     'procinfo.children_named(1, "gdb")\n', RULE_NAME_RULE),
    "name_def.py": ('RUN_NAMES = {}\n', RULE_NAME_RULE),
    "name_string.py": ('monkeypatch.setattr(procinfo, "_live_and_named", f)\n',
                       RULE_NAME_RULE),
    "name_report.py": ('x = procinfo.command_run(1)\n', RULE_NAME_RULE),
    "name_launcher.py": ('V = "__PYVENV_LAUNCHER__"\n', RULE_NAME_RULE),
    "name_exe.py": ('import os, sys\n'
                    'n = os.path.basename(sys.executable)\n', RULE_NAME_RULE),
    "clean.py": ('"""children_named and ps -o comm= in a docstring."""\n'
                 'import sys\n'
                 '# children_named in a comment\n'
                 'ARGV = [sys.executable, "-E", "-c", "pass"]\n'
                 'CC = ["cc", "-E", "x.c"]\n'
                 'ok = procinfo.children_running(1, prog)\n', None),
}


def _is_ps_argv(node):
    """True for a list or tuple literal whose first element is "ps"."""
    if not isinstance(node, (ast.List, ast.Tuple)) or not node.elts:
        return False
    first = node.elts[0]
    return isinstance(first, ast.Constant) and first.value == PS_PROGRAM


def _docstring_nodes(tree):
    """The ids of the string nodes that are docstrings."""
    ids = set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.Module, ast.FunctionDef,
                             ast.AsyncFunctionDef, ast.ClassDef)):
            body = node.body
            if (body and isinstance(body[0], ast.Expr)
                    and isinstance(body[0].value, ast.Constant)
                    and isinstance(body[0].value.value, str)):
                ids.add(id(body[0].value))
    return ids


def _is_executable_basename(node):
    """True for basename(sys.executable), by either spelling of basename."""
    if not isinstance(node, ast.Call) or len(node.args) != 1:
        return False
    func = node.func
    name = func.attr if isinstance(func, ast.Attribute) else getattr(func, "id", "")
    arg = node.args[0]
    return (name == "basename" and isinstance(arg, ast.Attribute)
            and arg.attr == "executable"
            and isinstance(arg.value, ast.Name) and arg.value.id == "sys")


def _name_rule_hit(node, docstrings):
    """True when @node names a helper of the name rule, reads the launcher
    variable or takes the basename of sys.executable."""
    if isinstance(node, ast.Name):
        return node.id in NAME_RULE_WORDS
    if isinstance(node, ast.Attribute):
        return node.attr in NAME_RULE_WORDS
    if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
        return node.name in NAME_RULE_WORDS
    if isinstance(node, ast.alias):
        return node.name in NAME_RULE_WORDS
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        if id(node) in docstrings:
            return False
        return node.value in NAME_RULE_WORDS or LAUNCHER_WORD in node.value
    return _is_executable_basename(node)


def offences(source, name="<source>"):
    """{rule: [line, ...]} for the Python @source."""
    tree = ast.parse(source, filename=name)
    docstrings = _docstring_nodes(tree)
    found = {rule: [] for rule in RULES}
    for node in ast.walk(tree):
        line = getattr(node, "lineno", None)
        if line is None:
            continue
        if _is_ps_argv(node):
            found[RULE_PS_ARGV].append(line)
        if _name_rule_hit(node, docstrings):
            found[RULE_NAME_RULE].append(line)
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
    """{(file, rule): [line, ...]} of the offences outside the owner."""
    found = {}
    for rel in sorted(_python_files(root)):
        if rel in (OWNER_FILE, SELF_FILE):
            continue
        with open(os.path.join(root, rel), encoding="utf-8",
                  errors="replace") as fh:
            hits = offences(fh.read(), rel)
        for rule, lines in hits.items():
            if lines:
                found[(rel, rule)] = sorted(set(lines))
    return found


def read_allow(path):
    """{(file, rule): count} from the allow file; comments and blanks
    skipped."""
    allowed = {}
    if not os.path.exists(path):
        return allowed
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            words = line.split(None, 3)
            allowed[(words[0], words[1])] = int(words[2])
    return allowed


def judge(found, allowed):
    """The messages that fail the check: a file with more offences than the
    allow file counts, and an allow line the tree no longer needs."""
    problems = []
    for (rel, rule), lines in sorted(found.items()):
        allowance = allowed.get((rel, rule), 0)
        if len(lines) > allowance:
            problems.append(
                "%s: %s: %d offence(s) at line(s) %s, allowed %d; ask "
                "tests/procinfo.py (Program.runs, run_state, cpu_seconds, "
                "child_facts) instead"
                % (rel, rule, len(lines),
                   ", ".join(str(n) for n in lines), allowance))
    for (rel, rule), count in sorted(allowed.items()):
        have = len(found.get((rel, rule), []))
        if rule in RULES and have < count:
            problems.append("%s: %s: allowed %d but the tree has %d; lower "
                            "the count in the allow file"
                            % (rel, rule, count, have))
    return problems


def selftest():
    """Plant the canaries; the scan must name each offence under its rule and
    spare the clean file.  Returns the messages of a blind scanner."""
    blind = []
    with tempfile.TemporaryDirectory() as root:
        os.makedirs(os.path.join(root, "tests"))
        for name, (text, _rule) in CANARIES.items():
            with open(os.path.join(root, "tests", name), "w") as fh:
                fh.write(text)
        found = scan(root)
    for name, (_text, rule) in CANARIES.items():
        rel = os.path.join("tests", name)
        if rule is None:
            if any(key[0] == rel for key in found):
                blind.append("selftest: the scan named the clean file %s"
                             % name)
        elif (rel, rule) not in found:
            blind.append("selftest: the scan did not name %s under %s"
                         % (name, rule))
    return blind


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--srcdir", default=os.environ.get("srcdir", "."))
    parser.add_argument("--allow", default=None)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)
    blind = selftest()
    for message in blind:
        print("check-test-processes: FAIL " + message)
    if blind or args.selftest:
        if not blind:
            print("check-test-processes: selftest ok")
        return 1 if blind else 0
    allow = args.allow or os.path.join(args.srcdir, "tests",
                                       "test-processes.allow")
    problems = judge(scan(args.srcdir), read_allow(allow))
    for message in problems:
        print("check-test-processes: FAIL " + message)
    if problems:
        return 1
    print("check-test-processes: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
