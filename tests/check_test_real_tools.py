#!/usr/bin/env python3
"""check_test_real_tools.py -- no test row reaches a tool of the machine by a
door the tool farm does not cover.

tests/gui_owner.py (Owner.launch) gives every process it starts the PATH of a
tool farm (tests/engine_guard.py), so a program that is not in the baseline is
not found.  That holds only for processes the owner starts.  This check scans
every Python file below tests/ and fails on the doors around it.

Rule T1: a subprocess, os.system, os.exec* or pty.spawn call outside
tests/gui_owner.py whose program is not a literal name of ALLOWED_PROGRAMS
(or sys.executable).  A shell, or a program given by a variable, cannot be
judged: it is counted in the allow file with its reason.
Rule T2: the editor binary (WPE_BIN or XWPE_BIN) started outside the owner:
it would run with the machine's whole environment and PATH.
Rule T3: REAL_ENGINE_VAR (the live opt-out) or an API key name in an
`inherit=` argument, in a file that is not a live file (test_*_live*.py).
Rule T4: shutil.which() of a known engine name, or a literal absolute path
of a known engine, or an AIHostCommand line with a literal absolute program:
the guard cannot see an absolute path.  KNOWN_ENGINE_NAMES is advisory here;
the farm itself does not depend on it.
Rule T5: a live file (test_*_live*.py, and tests/live_ai.py) whose first
statement after its docstring and its imports is not a call of
require_live_gate().  That call is the gate that does not depend on conftest:
without it a run with --noconftest imports the file and reaches the engine.

The sites that remain are counted per file and rule in
tests/test-real-tools.allow (`file rule count reason`); a count may only
fall, and a line above the measurement is stale and fails.  tests/eval/ is
not scanned.  The owner files and this file are not scanned either.

  check_test_real_tools.py [--srcdir DIR] [--allow FILE] [--selftest]

--selftest plants one offence per rule and a clean file in a scratch folder
and fails unless the scan names the offences and not the clean file.  With no
option the self-test runs, then the tree.

Exit: 0 clean, 1 offenders or a blind scanner.
"""
import argparse
import ast
import os
import re
import shlex
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import engine_guard  # noqa: E402

RULE_PROGRAM = "T1"
RULE_EDITOR = "T2"
RULE_OPT_OUT = "T3"
RULE_ABSOLUTE = "T4"
RULE_GATE = "T5"
RULES = (RULE_PROGRAM, RULE_EDITOR, RULE_OPT_OUT, RULE_ABSOLUTE, RULE_GATE)
GATE_CALL = "require_live_gate"
# Not a test_*_live*.py name, but it reaches the local Ollama at import.
GATE_EXTRA_FILES = (os.path.join("tests", "live_ai.py"),)
OWNER_FILES = (os.path.join("tests", "gui_owner.py"),
               os.path.join("tests", "engine_guard.py"),
               os.path.join("tests", "live_ai.py"),
               os.path.join("tests", "check_test_real_tools.py"))
SKIPPED_FOLDERS = ("eval", ".venv", "__pycache__")

# Programs a row may run by name outside the owner: none of them runs a model
# or acts on an account.  A row that needs another declares it here, with the
# reason in the commit that adds it.
ALLOWED_PROGRAMS = ("strings", "ldd", "git", "openssl", "gcc", "cc", "sleep",
                    "true", "false", "xdotool", "python3", "gdb", "jdb",
                    "tput", "infocmp", "tic", "locale", "scalac", "ps", "stat")
SUBPROCESS_CALLS = ("run", "Popen", "call", "check_call", "check_output",
                    "getoutput", "getstatusoutput")
OS_CALLS = ("system", "popen", "execv", "execve", "execl", "execle", "execlp",
            "execlpe", "execvp", "execvpe", "spawnv", "spawnve", "spawnl",
            "spawnle", "spawnlp", "spawnlpe", "spawnvp", "spawnvpe",
            "posix_spawn", "posix_spawnp")
ASYNCIO_PREFIX = "create_subprocess_"
EDITOR_NAMES = ("WPE_BIN", "XWPE_BIN")
INHERIT_KEYWORD = "inherit"
KEY_CONSTANTS = ("KEY_INHERIT", "OPENAI_KEY_INHERIT")
ENGINE_PATH = re.compile(r"/(%s)(\s|$|['\"])"
                         % "|".join(re.escape(n) for n in
                                    engine_guard.KNOWN_ENGINE_NAMES))
HOST_COMMAND_PATH = re.compile(r"AIHostCommand\W{0,3}\s*/")

# One offence per rule and a clean file, planted by the self-test.  The text
# is a string, so this file holds no offence itself.
CANARIES = {
    "program_var.py": ('import subprocess\n'
                       'subprocess.run(args)\n', RULE_PROGRAM),
    "program_name.py": ('import subprocess\n'
                        'subprocess.run(["claude", "-p"])\n', RULE_PROGRAM),
    "program_os.py": ('import os\nos.system("newengine --go")\n', RULE_PROGRAM),
    "editor.py": ('import subprocess\n'
                  'subprocess.Popen([WPE_BIN, "a.c"])\n', RULE_EDITOR),
    "opt_out.py": ('env = {"XWPE_TEST_REAL_ENGINE": "1"}\n', RULE_OPT_OUT),
    "inherit_key.py": ('WpeSession(w, s, inherit=("ANTHROPIC_API_KEY",))\n',
                       RULE_OPT_OUT),
    "from_import.py": ('from subprocess import run\nrun(["claude", "-p"])\n',
                       RULE_PROGRAM),
    "from_import_as.py": ('from subprocess import Popen as P\nP(args)\n',
                          RULE_PROGRAM),
    "module_alias.py": ('import subprocess as sp\nsp.run(args)\n',
                        RULE_PROGRAM),
    "asyncio_exec.py": ('import asyncio\n'
                        'async def f():\n'
                        '    await asyncio.create_subprocess_exec("claude")\n',
                        RULE_PROGRAM),
    "asyncio_shell.py": ('import asyncio\n'
                         'async def f():\n'
                         '    await asyncio.create_subprocess_shell(cmd)\n',
                         RULE_PROGRAM),
    "posix_spawn.py": ('import os\nos.posix_spawn("/x/claude", ["claude"], {})\n',
                       RULE_PROGRAM),
    "pexpect_spawn.py": ('import pexpect\npexpect.spawn("claude")\n',
                         RULE_PROGRAM),
    "pexpect_from.py": ('from pexpect import run\nrun("newengine")\n',
                        RULE_PROGRAM),
    "which.py": ('import shutil\nshutil.which("claude")\n', RULE_ABSOLUTE),
    "path.py": ('CMD = "/home/x/.local/bin/claude -p"\n', RULE_ABSOLUTE),
    "host.py": ('TEXT = "AIHostCommand=/opt/x/engine"\n', RULE_ABSOLUTE),
    "test_a_live_b.py": ('"""doc"""\n'
                         'import os\n'
                         'from live_gate import require_live_gate\n'
                         'require_live_gate()\n'
                         'env = {"XWPE_TEST_REAL_ENGINE": "1"}\n', None),
    "test_no_gate_live.py": ('import os\nX = 1\n', RULE_GATE),
    "test_late_gate_live.py": ('import os\nX = 1\n'
                               'require_live_gate()\n', RULE_GATE),
    "test_empty_live.py": ('"""only a docstring"""\n', RULE_GATE),
    "clean.py": ('"""subprocess.run(args) and claude in a docstring."""\n'
                 'import subprocess, sys\n'
                 '# subprocess.run(args) in a comment\n'
                 'subprocess.run(["strings", "x"])\n'
                 'subprocess.run([sys.executable, "-c", "pass"])\n'
                 'NAME = "claude"\n', None),
}


def import_aliases(tree):
    """{local name: dotted target} for the imports of @tree: `import
    subprocess as sp` gives sp -> subprocess, and `from subprocess import
    run as r` gives r -> subprocess.run."""
    aliases = {}
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for item in node.names:
                aliases[item.asname or item.name.split(".")[0]] = \
                    item.name if item.asname else item.name.split(".")[0]
        elif isinstance(node, ast.ImportFrom) and node.module:
            for item in node.names:
                aliases[item.asname or item.name] = \
                    "%s.%s" % (node.module, item.name)
    return aliases


def _dotted(node):
    """The dotted text of a Name or an Attribute chain, or ''."""
    if isinstance(node, ast.Name):
        return node.id
    if isinstance(node, ast.Attribute):
        head = _dotted(node.value)
        return "%s.%s" % (head, node.attr) if head else ""
    return ""


def _call_name(node, aliases=None):
    """The dotted name of the function a Call calls, with its first part
    resolved through the file's import aliases, or ''."""
    name = _dotted(node.func)
    head, _, rest = name.partition(".")
    target = (aliases or {}).get(head)
    if target:
        return "%s.%s" % (target, rest) if rest else target
    return name


def _is_start_call(name):
    """True for a call that starts a program."""
    module, _, attr = name.rpartition(".")
    if module == "subprocess":
        return attr in SUBPROCESS_CALLS
    if module == "os":
        return attr in OS_CALLS
    if module == "asyncio":
        return attr.startswith(ASYNCIO_PREFIX)
    if name == "pty.spawn" or name.split(".")[0] == "pexpect":
        return True
    return name == "Popen"


def _program_node(call, aliases):
    """The expression that holds the program of a start call, or None."""
    name = _call_name(call, aliases)
    if name.startswith("os.exec") or name.startswith("os.spawn"):
        index = 1 if name.startswith("os.spawn") else 0
        return call.args[index] if len(call.args) > index else None
    first = call.args[0] if call.args else None
    if first is None:
        for keyword in call.keywords:
            if keyword.arg == "args":
                first = keyword.value
    if isinstance(first, (ast.List, ast.Tuple)):
        return first.elts[0] if first.elts else None
    return first


def _mentions_editor(node):
    """True when @node names WPE_BIN or XWPE_BIN anywhere inside."""
    for sub in ast.walk(node):
        if isinstance(sub, ast.Name) and sub.id in EDITOR_NAMES:
            return True
        if isinstance(sub, ast.Attribute) and sub.attr in EDITOR_NAMES:
            return True
    return False


def _literal_program(node):
    """The program name of a string constant (the first word, when the whole
    command line is one string), or None."""
    if not (isinstance(node, ast.Constant) and isinstance(node.value, str)):
        return None
    try:
        words = shlex.split(node.value)
    except ValueError:
        words = node.value.split()
    return words[0] if words else ""


def _is_sys_executable(node):
    """True for sys.executable."""
    return (isinstance(node, ast.Attribute) and node.attr == "executable"
            and isinstance(node.value, ast.Name) and node.value.id == "sys")


def _program_rule(call, aliases):
    """T1 or T2 when the start call @call is an offence, else None."""
    program = _program_node(call, aliases)
    if program is None:
        return RULE_PROGRAM
    if _mentions_editor(program):
        return RULE_EDITOR
    if _is_sys_executable(program):
        return None
    name = _literal_program(program)
    if name is None:
        return RULE_PROGRAM
    if name in ALLOWED_PROGRAMS:
        return None
    return RULE_PROGRAM


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


def _inherits_a_key(call):
    """True when the call's inherit= argument names an API key."""
    for keyword in call.keywords:
        if keyword.arg != INHERIT_KEYWORD:
            continue
        for sub in ast.walk(keyword.value):
            if isinstance(sub, ast.Constant) and sub.value in engine_guard.API_KEY_NAMES:
                return True
            if isinstance(sub, ast.Name) and sub.id in KEY_CONSTANTS:
                return True
            if isinstance(sub, ast.Attribute) and sub.attr in KEY_CONSTANTS:
                return True
    return False


def _opt_out_hit(node):
    """True for the opt-out variable's name as a string, or an inherited key."""
    if isinstance(node, ast.Constant) and node.value == engine_guard.REAL_ENGINE_VAR:
        return True
    return isinstance(node, ast.Call) and _inherits_a_key(node)


def _absolute_hit(node, docstrings, aliases):
    """True for shutil.which of an engine name, a literal absolute path of an
    engine, or an AIHostCommand line with a literal absolute program."""
    if isinstance(node, ast.Call) \
            and _call_name(node, aliases) == "shutil.which":
        first = node.args[0] if node.args else None
        return (isinstance(first, ast.Constant)
                and first.value in engine_guard.KNOWN_ENGINE_NAMES)
    if isinstance(node, ast.Constant) and isinstance(node.value, str) \
            and id(node) not in docstrings:
        return bool(ENGINE_PATH.search(node.value)
                    or HOST_COMMAND_PATH.search(node.value))
    return False


def offences(source, name="<source>", live=False):
    """{rule: [line, ...]} for the Python @source.  A live file may use the
    opt-out and reach engines by their paths."""
    tree = ast.parse(source, filename=name)
    docstrings = _docstring_nodes(tree)
    aliases = import_aliases(tree)
    found = {rule: [] for rule in RULES}
    for node in ast.walk(tree):
        line = getattr(node, "lineno", None)
        if line is None:
            continue
        if isinstance(node, ast.Call) \
                and _is_start_call(_call_name(node, aliases)):
            rule = _program_rule(node, aliases)
            if rule:
                found[rule].append(line)
        if live:
            continue
        if _opt_out_hit(node):
            found[RULE_OPT_OUT].append(line)
        if _absolute_hit(node, docstrings, aliases):
            found[RULE_ABSOLUTE].append(line)
    return found


def _python_files(root):
    """Every .py file below @root/tests, skipping SKIPPED_FOLDERS, as paths
    relative to @root."""
    base = os.path.join(root, "tests")
    for folder, names, files in os.walk(base):
        names[:] = [n for n in names if n not in SKIPPED_FOLDERS]
        for name in files:
            if name.endswith(".py"):
                yield os.path.relpath(os.path.join(folder, name), root)


def gate_offence(source, name="<source>"):
    """The line of the first statement of @source, after its docstring and its
    imports, when that statement is not a call of require_live_gate(); the
    line 1 when there is no statement at all.  None when the gate is there."""
    tree = ast.parse(source, filename=name)
    body = list(tree.body)
    if body and isinstance(body[0], ast.Expr) \
            and isinstance(body[0].value, ast.Constant) \
            and isinstance(body[0].value.value, str):
        body = body[1:]
    for node in body:
        if isinstance(node, (ast.Import, ast.ImportFrom)):
            continue
        call = node.value if isinstance(node, ast.Expr) else None
        if isinstance(call, ast.Call) and _dotted(call.func).split(".")[-1] \
                == GATE_CALL:
            return None
        return node.lineno
    return 1


def _gate_files(root, rel):
    """True when @rel is a file whose import must open with the gate."""
    return engine_guard.is_live_file(rel) or rel in GATE_EXTRA_FILES


def scan(root):
    """{(file, rule): [line, ...]} of the offences."""
    found = {}
    for rel in sorted(_python_files(root)):
        with open(os.path.join(root, rel), encoding="utf-8",
                  errors="replace") as fh:
            source = fh.read()
        if _gate_files(root, rel):
            line = gate_offence(source, rel)
            if line is not None:
                found[(rel, RULE_GATE)] = [line]
        if rel in OWNER_FILES:
            continue
        hits = offences(source, rel, engine_guard.is_live_file(rel))
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
                "%s: %s: %d offence(s) at line(s) %s, allowed %d; start the "
                "program through tests/gui_owner.py (run_tool, Terminal.start) "
                "or name the site, with its reason, in the allow file"
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
    spare the clean files.  Returns the messages of a blind scanner."""
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
        print("check-test-real-tools: FAIL " + message)
    if blind or args.selftest:
        if not blind:
            print("check-test-real-tools: selftest ok")
        return 1 if blind else 0
    allow = args.allow or os.path.join(args.srcdir, "tests",
                                       "test-real-tools.allow")
    problems = judge(scan(args.srcdir), read_allow(allow))
    for message in problems:
        print("check-test-real-tools: FAIL " + message)
    if problems:
        return 1
    print("check-test-real-tools: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
