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
RULE_CLOCK = "WS"
RULE_DURATION = "WD"
RULE_END = "WE"
RULE_PROGRAM = "WF"
RULES = (RULE_READ, RULE_LOOP, RULE_DOUBLE, RULE_CLOCK, RULE_DURATION, RULE_END,
         RULE_PROGRAM)
# Rule WS: a reference to one of these of the time module is a clock of the
# row's own, whatever it is called (alias, from-import, getattr).
CLOCK_NAMES = ("sleep", "time", "monotonic", "perf_counter", "time_ns",
               "monotonic_ns", "perf_counter_ns")
# Rule WF: a sleep of a known form inside the text of a program or of a
# stand-in script: a call in a language (time.sleep, Thread.sleep, usleep,
# nanosleep, sleep_for, from_secs) and the shell's `sleep N` at the start of
# a command.
PROGRAM_SLEEP = re.compile(
    r"\b(?:time\.sleep|time\.Sleep|Thread\.sleep|thread::sleep|nanosleep|usleep"
    r"|sleep_for|sleep)\s*\(|"
    r"\bfrom_secs\s*\(|"
    r"(?:^|;|&&|\|\||\||\bdo\b|\bthen\b|\bexec\b|\()[ \t]*sleep[ \t]+[\d$]", re.M)
PROGRAM_GATE = os.path.join("tests", "program_gate.py")
# The files that own a clock, an end or a duration: the two drivers.  A test
# file is never an owner; its own sites are counted in the allow file.
CLOCK_OWNERS = (os.path.join("tests", "gui_owner.py"), os.path.join("tests", "wpe_driver.py"))
# The allow file: the sites a sweep has not moved yet, counted per file and
# rule; a count may only fall.
ALLOW_FILE = os.path.join("tests", "test-waits.allow")
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
    "clock_alias.py": ('from time import monotonic as m\n'
                       'x = m()\n', RULE_CLOCK),
    "clock_module_alias.py": ('import time as t\n'
                              't.sleep(1)\n', RULE_CLOCK),
    "clock_getattr.py": ('import time\n'
                         'getattr(time, "sleep")(1)\n', RULE_CLOCK),
    "clock_subtraction.py": ('import time\n'
                             'def f(start):\n'
                             '    return time.monotonic() - start\n', RULE_CLOCK),
    "clock_select.py": ('import select\n'
                        'select.select([], [], [], 1)\n', RULE_CLOCK),
    "end_alive.py": ('def t(s):\n'
                     '    assert not s.alive()\n', RULE_END),
    "end_poll.py": ('def t(s):\n'
                    '    assert s.proc.poll() is not None\n', RULE_END),
    "end_exit_code.py": ('def t(s):\n'
                         '    assert s.exit_code() == 0\n', RULE_END),
    "clock_select_alias.py": ('import select as sl\n'
                              'sl.select([], [], [], 5)\n', RULE_CLOCK),
    "clock_select_from.py": ('from select import select as pick\n'
                             'pick([], [], [], 5)\n', RULE_CLOCK),
    "clock_import_by_name.py": ('__import__("time").sleep(1)\n', RULE_CLOCK),
    "clock_star_import.py": ('from time import *\n'
                             'sleep(1)\n', RULE_CLOCK),
    # A test file is never an owner of a clock.
    "test_wait_owner.py": ('import time\n'
                           'time.sleep(1)\n', RULE_CLOCK),
    "end_returncode.py": ('def t(s):\n'
                          '    assert s.proc.returncode is not None\n', RULE_END),
    "end_named.py": ('def t(s):\n'
                     '    ended = not s.alive()\n'
                     '    assert ended\n', RULE_END),
    "end_named_poll.py": ('def t(s):\n'
                          '    gone = s.proc.poll() is not None\n'
                          '    assert gone\n', RULE_END),
    "duration_literal.py": ('ENV = {"XWPE_AI_MOCK_DELAY_MS": "5000"}\n', RULE_DURATION),
    "duration_xwperc_lines.py": ('RC = "Keys : borland\\nAIToolTimeout : 2\\n"\n', RULE_DURATION),
    "duration_option_tight.py": ('LINE = "AIToolTimeout: 2\\n"\n', RULE_DURATION),
    "duration_joined.py": ('NAME = "XWPE_AI_MOCK" + "_DELAY_MS"\n', RULE_DURATION),
    "duration_keyword.py": ('import os\n'
                            'os.environ.update(XWPE_AI_MOCK_DELAY_MS="5000")\n', RULE_DURATION),
    "duration_option.py": ('LINE = "AIToolTimeout : 2\\n"\n', RULE_DURATION),
    "duration_fstring.py": ('kind = "AI"\n'
                            'name = f"XWPE_{kind}_TIMEOUT"\n', RULE_DURATION),
    "duration_concat.py": ('kind = "AI"\n'
                           'name = "XWPE_" + kind + "_MS"\n', RULE_DURATION),
    "program_percent.py": ('PROG = "while True:\\n    time.sleep(%d)\\n" % 2\n', RULE_PROGRAM),
    "program_fstring.py": ('n = 2\n'
                           'PROG = f"  Thread.sleep({n});"\n', RULE_PROGRAM),
    "program_rust.py": ('PROG = "std::thread::sleep(std::time::Duration::from_secs(8));"\n',
                        RULE_PROGRAM),
    "program_shell.py": ('SCRIPT = "#!/bin/sh\\necho up\\nsleep 30\\n"\n', RULE_PROGRAM),
    "program_shell_chained.py": ('SCRIPT = "trap : TERM; while :; do sleep $N; done"\n',
                                 RULE_PROGRAM),
    "program_shell_exec.py": ('SCRIPT = "#!/bin/sh\\nexec sleep 20\\n"\n', RULE_PROGRAM),
    "program_nanosleep.py": ('PROG = "struct timespec t = {1, 0}; nanosleep(&t, 0);"\n',
                             RULE_PROGRAM),
    "program_sleep_for.py": ('PROG = "std::this_thread::sleep_for(std::chrono::seconds(8));"\n',
                             RULE_PROGRAM),
    "clean_still_running.py": ('def t(s):\n'
                               '    assert s.alive()\n'
                               '    assert s.proc.poll() is None\n'
                               '    assert s.proc.returncode is None\n', None),
    "clean_prose.py": ('MSG = "the row should sleep for a while, then go on"\n'
                       'OPT = "AIToolTimeoutNote : none"\n', None),
    "clean_api.py": ('from gui_owner import product_duration, product_option_line\n'
                     'v = product_duration("XWPE_AI_MOCK_DELAY_MS", 5000)\n'
                     'o = product_option_line("AIToolTimeout", 2)\n'
                     '"""a docstring that says time.sleep(1) and XWPE_AI_TIMEOUT"""\n',
                     None),
    "clean.py": ('"""XWPE_TEST_WAIT_SCALE and _wait_screen in a docstring."""\n'
                 'import os, time\n'
                 '# time.time() + tmo in a comment\n'
                 'env = {"XWPE_TEST_WAIT_SCALE": "3"}\n'
                 'os.environ["XWPE_TEST_WAIT_SCALE"] = "3"\n'
                 'def f(s, tmo, delay_ms):\n'
                 '    end = wait_deadline(tmo)\n'
                 '    stop = 5\n'
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


def _time_aliases(tree):
    """(local names of the time module, {local name: clock name}) a file
    binds by `import time as t` and `from time import sleep as z`."""
    modules, names = set(), {}
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                if alias.name == "time":
                    modules.add(alias.asname or "time")
        elif isinstance(node, ast.ImportFrom) and node.module == "time":
            for alias in node.names:
                if alias.name == "*":
                    names.update({clock: clock for clock in CLOCK_NAMES})
                elif alias.name in CLOCK_NAMES:
                    names[alias.asname or alias.name] = alias.name
    return modules, names


def _select_aliases(tree):
    """(local names of the select module, local names of select.select) a
    file binds by `import select as sl` and `from select import select as
    pick`."""
    modules, functions = {"select"}, set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                if alias.name == "select":
                    modules.add(alias.asname or "select")
        elif isinstance(node, ast.ImportFrom) and node.module == "select":
            for alias in node.names:
                if alias.name == "select":
                    functions.add(alias.asname or "select")
    return modules, functions


def _is_star_time_import(node):
    """True for `from time import *`: every clock of the module, unnamed."""
    return (isinstance(node, ast.ImportFrom) and node.module == "time"
            and any(alias.name == "*" for alias in node.names))


def _imports_time_by_name(node):
    """True for __import__("time") and importlib.import_module("time"): the
    module reached with no import statement to read."""
    if not isinstance(node, ast.Call) or not node.args:
        return False
    first = node.args[0]
    if not (isinstance(first, ast.Constant) and first.value == "time"):
        return False
    func = node.func
    return (isinstance(func, ast.Name) and func.id == "__import__") or (
        isinstance(func, ast.Attribute) and func.attr == "import_module")


def _has_timeout(node):
    """True when a select call carries a timeout (a fourth argument or the
    keyword)."""
    return len(node.args) >= 4 or any(k.arg == "timeout" for k in node.keywords)


def _is_clock_reference(node, modules, names, selects=({"select"}, set())):
    """True for time.sleep (any alias of time, or the module reached by
    __import__), a clock name taken from the module (from-import, star
    import), getattr(time, ...), and a select.select with a timeout (any
    alias of the module or of the function)."""
    select_modules, select_functions = selects
    if isinstance(node, ast.Attribute):
        if isinstance(node.value, ast.Name) and node.value.id in modules \
                and node.attr in CLOCK_NAMES:
            return True
        if _imports_time_by_name(node.value) and node.attr in CLOCK_NAMES:
            return True
    if _is_star_time_import(node):
        return True
    if isinstance(node, ast.Name) and isinstance(node.ctx, ast.Load) and node.id in names:
        return True
    if isinstance(node, ast.Call):
        func = node.func
        if isinstance(func, ast.Name) and func.id == "getattr" and node.args \
                and isinstance(node.args[0], ast.Name) and node.args[0].id in modules:
            return True
        if isinstance(func, ast.Attribute) and func.attr == "select" \
                and isinstance(func.value, ast.Name) and func.value.id in select_modules:
            return _has_timeout(node)
        if isinstance(func, ast.Name) and func.id in select_functions:
            return _has_timeout(node)
    return False


def _docstring_nodes(tree):
    """The string constants that are docstrings or lone expression strings."""
    return {id(n.value) for n in ast.walk(tree)
            if isinstance(n, ast.Expr) and isinstance(n.value, ast.Constant)
            and isinstance(n.value.value, str)}


_DURATIONS = []


def _duration_names():
    """The environment variables and options of the product that hold a
    duration, read from the sources by the owner (gui_owner), once."""
    if _DURATIONS:
        return _DURATIONS[0]
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    try:
        import gui_owner
        names = set(gui_owner.product_duration_names())
    finally:
        sys.path.pop(0)
    top = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    with open(os.path.join(top, "we_opt.c"), encoding="utf-8", errors="replace") as fh:
        names.update(re.findall(r'"(\w+Timeout)"', fh.read()))
    _DURATIONS.append(names)
    return names


def _is_duration_api_argument(node, parents):
    """True when @node is the first argument of product_duration or
    product_option_line."""
    parent = parents.get(id(node))
    return (isinstance(parent, ast.Call) and parent.args and parent.args[0] is node
            and _called_name(parent) in ("product_duration", "product_option_line"))


def _folded_string(node):
    """The text of a string made only of literals joined with + (`"XWPE_AI_MOCK"
    + "_DELAY_MS"`), or None."""
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
        left, right = _folded_string(node.left), _folded_string(node.right)
        if left is not None and right is not None:
            return left + right
    return None


def _builds_a_product_name(node, durations):
    """True for an f-string or a concatenation or %-format whose literal part
    starts a name with the XWPE_ prefix, and for literals joined into the name
    of a duration: no alias route around WD."""
    if isinstance(node, ast.JoinedStr):
        first = node.values[0] if node.values else None
        return isinstance(first, ast.Constant) and str(first.value).startswith("XWPE_")
    if isinstance(node, ast.BinOp) and isinstance(node.op, (ast.Add, ast.Mod)):
        if _folded_string(node) in durations:
            return True
        left = node.left
        return (isinstance(left, ast.Constant) and isinstance(left.value, str)
                and left.value.startswith("XWPE_") and left.value.endswith("_"))
    return False


def _names_an_option_line(text, durations):
    """True when a line of @text starts with the name of a duration option and
    a colon (`AIToolTimeout : 2`, `AIToolTimeout: 2`), in a text of any number
    of lines (a whole xwperc)."""
    options = [n for n in durations if n.endswith("Timeout")]
    return any(re.match(r"\s*%s\s*:" % re.escape(option), line)
               for line in text.splitlines() for option in options)


def _is_duration_keyword(node, durations):
    """True for a keyword argument spelt as the name of a duration
    (`os.environ.update(XWPE_AI_MOCK_DELAY_MS="5000")`, `dict(...)`)."""
    return isinstance(node, ast.keyword) and node.arg in durations


def _is_method_call(node, name):
    """True for `x.<name>(...)`."""
    return (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
            and node.func.attr == name)


def _is_none(node):
    return isinstance(node, ast.Constant) and node.value is None


def _reads_the_end(node):
    """True when the expression @node reads the END of a session once: `not
    s.alive()`, `s.proc.poll() is not None`, `x.returncode is not None`, or
    any use of exit_code().  A "still running" check (`s.alive()`, `poll() is
    None`) is not a read of the end: the row has not moved past it."""
    if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.Not):
        return _is_method_call(node.operand, "alive")
    if isinstance(node, ast.Compare) and len(node.ops) == 1 \
            and isinstance(node.ops[0], ast.IsNot) and _is_none(node.comparators[0]):
        left = node.left
        return _is_method_call(left, "poll") or (
            isinstance(left, ast.Attribute) and left.attr == "returncode")
    return _is_method_call(node, "exit_code")


def _ended_names(tree):
    """The names a file gives a read of the end (`ended = not s.alive()`):
    asserting one is the same single read."""
    return {node.targets[0].id for node in ast.walk(tree)
            if isinstance(node, ast.Assign) and len(node.targets) == 1
            and isinstance(node.targets[0], ast.Name)
            and any(_reads_the_end(sub) for sub in ast.walk(node.value))}


def _is_end_read_assert(node, ended=frozenset()):
    """True for an assert whose test reads the end of a session once (see
    _reads_the_end), directly or through a name set from such a read."""
    if not isinstance(node, ast.Assert):
        return False
    if isinstance(node.test, ast.Name) and node.test.id in ended:
        return True
    return any(_reads_the_end(sub) for sub in ast.walk(node.test))


def _extra_offences(tree, rel, found, durations):
    """Rules WS, WD, WE and WF over @tree, added to @found."""
    modules, names = _time_aliases(tree)
    selects = _select_aliases(tree)
    ended = _ended_names(tree)
    docs = _docstring_nodes(tree)
    parents = {id(child): parent for parent in ast.walk(tree)
               for child in ast.iter_child_nodes(parent)}
    owns_clock = rel in CLOCK_OWNERS
    for node in ast.walk(tree):
        line = getattr(node, "lineno", None)
        if line is None:
            continue
        if not owns_clock and _is_duration_keyword(node, durations):
            found[RULE_DURATION].append(line)
        if not owns_clock and _is_clock_reference(node, modules, names, selects):
            found[RULE_CLOCK].append(line)
        if not owns_clock and _is_end_read_assert(node, ended):
            found[RULE_END].append(line)
        if owns_clock:
            continue
        if isinstance(node, ast.Constant) and isinstance(node.value, str) \
                and id(node) not in docs:
            text = node.value
            if text in durations or _names_an_option_line(text, durations):
                if not _is_duration_api_argument(node, parents):
                    found[RULE_DURATION].append(line)
            if rel != PROGRAM_GATE and PROGRAM_SLEEP.search(text):
                found[RULE_PROGRAM].append(line)
        if _builds_a_product_name(node, durations):
            found[RULE_DURATION].append(line)


def offences(source, rel, name="<source>"):
    """{rule: [line, ...]} for the Python @source of the file @rel."""
    tree = ast.parse(source, filename=name)
    found = {rule: [] for rule in RULES}
    _extra_offences(tree, rel, found, _duration_names())
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


# The rules a sweep moves the sites of; their sites that are left are counted
# in the allow file and the count may only fall.
SWEPT_RULES = (RULE_CLOCK, RULE_DURATION, RULE_END, RULE_PROGRAM)


def read_allow(root):
    """{(file, rule): count} from the allow file (`file rule count`)."""
    allowed = {}
    path = os.path.join(root, ALLOW_FILE)
    if not os.path.exists(path):
        return allowed
    with open(path, encoding="utf-8") as fh:
        for raw in fh:
            words = raw.split()
            if not words or raw.startswith("#"):
                continue
            for pair in words[1:]:
                rule, count = pair.split("=")
                allowed[(words[0], rule)] = int(count)
    return allowed


def over_the_allowance(found, allowed):
    """The lines for a swept rule with more sites than the allow file counts
    (a new one), and for a count above the measurement (stale: lower it)."""
    lines = []
    keys = set(allowed) | {k for k in found if k[1] in SWEPT_RULES}
    for rel, rule in sorted(keys):
        have = len(found.get((rel, rule), []))
        count = allowed.get((rel, rule), 0)
        if have > count:
            lines.append("%s: rule %s has %d site(s), %d allowed, at line %s"
                         % (rel, rule, have, count,
                            ", ".join(map(str, found[(rel, rule)]))))
        elif have < count:
            lines.append("%s: rule %s has %d site(s) and %s lists %d: lower it"
                         % (rel, rule, have, ALLOW_FILE, count))
    return lines


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
            # A deadline built on time.time() is also a clock of its own (WS).
            if rule is not None and (rule not in hit or hit - {rule} - {RULE_CLOCK}):
                sys.stderr.write("check_test_waits: canary %s named %s, want %s\n"
                                 % (name, sorted(hit), rule))
                ok = False
    return ok


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--srcdir", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), ".."))
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--list", action="store_true",
                        help="print the swept rules' sites as allow lines")
    args = parser.parse_args(argv)
    if not selftest():
        return 1
    if args.selftest:
        print("check_test_waits: the canaries are named, the clean file is not")
        return 0
    root = os.path.abspath(args.srcdir)
    found = scan(root)
    if args.list:
        for (rel, rule), lines in sorted(found.items()):
            if rule in SWEPT_RULES:
                print("%s %s %d" % (rel, rule, len(lines)))
        return 0
    problems = report({k: v for k, v in found.items() if k[1] not in SWEPT_RULES})
    problems += over_the_allowance(found, read_allow(root))
    if problems:
        for line in problems:
            sys.stderr.write("check_test_waits: %s\n" % line)
        sys.stderr.write("check_test_waits: %d problem(s); use wpe_driver.wait_screen / "
                         "wait_until / wait_deadline and the owner's duration APIs\n"
                         % len(problems))
        return 1
    print("check_test_waits: one owner decides how long a wait lasts")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
