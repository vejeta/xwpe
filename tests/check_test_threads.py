#!/usr/bin/env python3
"""check_test_threads.py -- a test launches an editor from the main thread.

The process owner (tests/gui_owner.py) refuses a launch from any thread but
the main one: on Linux an editor asks for SIGTERM when its parent dies
(PR_SET_PDEATHSIG), and the kernel ties that signal to the thread that
forked, not the process.  An editor forked by a pool thread is killed when
that thread ends, and a killed pytest no longer takes its editors with it.
A row that starts its sessions from a pool therefore fails at once with
OwnerRefused.  Rows that need many sessions run them one after another
(wpe_driver.sweep_cuts) and let pytest-xdist spread the rows over processes.

Rule TA (default deny): no test file names a way to run work on another
thread or process unless tests/test-threads.allow lists it with a reason:
ThreadPoolExecutor, ProcessPoolExecutor, ThreadPool, a multiprocessing Pool,
an import of concurrent.futures, multiprocessing or _thread,
asyncio.to_thread, run_in_executor, start_new_thread.  A file that has no
session in it today would otherwise gain one tomorrow, with a pool helper
that hands the launch to its callers.

Rule TB: a function handed to a thread (threading.Thread(target=...),
threading.Timer, a pool's submit, map or apply_async) and a class that
subclasses Thread must not name a launch (WpeSession, EditSession, Editor,
Terminal, Client, start_service, run_tool, run_nested), directly or through
the functions of the same file it calls, however deep.  A target is a lambda
or a function defined in the same file; a thread that only serves a fake
HTTP server is none of this and is not named.

Residual: a threading.Thread whose target is imported from another module is
accepted, because the scan reads one file at a time.  Fake-server threads
are legitimate, and the owner refuses a launch off the main thread at run
time anyway.

An identifier, an attribute, an import or a definition counts; comments,
docstrings and string literals do not.  A site that is right (the rows that
prove the refusal, the source parser's pool) is counted per file and rule in
tests/test-threads.allow (`file rule count reason`); a count may only fall,
and a line above the measurement is stale and fails.  The files of
tests/eval/ are not scanned, nor is this file.

  check_test_threads.py [--srcdir DIR] [--allow FILE] [--selftest]

--selftest plants one offence per rule and clean files in a scratch folder
and fails unless the scan names the offences and not the clean files (a
scanner that sees nothing is not a check).  With no option the self-test
runs, then the tree.

Exit: 0 clean, 1 offenders or a blind scanner.
"""
import argparse
import ast
import os
import sys
import tempfile

RULE_POOL = "TA"
RULE_TARGET = "TB"
RULES = (RULE_POOL, RULE_TARGET)
SELF_FILE = os.path.join("tests", "check_test_threads.py")
SKIPPED_FOLDERS = ("eval", ".venv", "__pycache__")

POOL_WORDS = ("ThreadPoolExecutor", "ProcessPoolExecutor", "ThreadPool",
              "Pool", "to_thread", "run_in_executor", "start_new_thread")
POOL_MODULES = ("concurrent", "multiprocessing", "_thread")
LAUNCH_WORDS = ("WpeSession", "EditSession", "Editor", "Terminal", "Client",
                "start_service", "run_tool", "run_nested")
THREAD_STARTERS = ("Thread", "Timer", "submit", "map", "apply_async")
THREAD_BASES = ("Thread",)

# One offence per rule and clean files, planted by the self-test.  The text
# is a string, so this file holds no offence itself.
CANARIES = {
    "pool_session.py": ('import concurrent.futures\n'
                        'from wpe_driver import WpeSession\n'
                        'def f(d):\n'
                        '    with concurrent.futures.ThreadPoolExecutor(4) as p:\n'
                        '        p.map(lambda n: WpeSession(d, "x"), range(3))\n',
                        RULE_POOL),
    "pool_edit.py": ('from concurrent.futures import ThreadPoolExecutor\n'
                     'from edit_model.oracle import EditSession\n',
                     RULE_POOL),
    "thread_target.py": ('import threading\n'
                         'def work(d):\n'
                         '    return WpeSession(d, "x")\n'
                         'def f(d):\n'
                         '    threading.Thread(target=work, args=(d,)).start()\n',
                         RULE_TARGET),
    "thread_lambda.py": ('import threading\n'
                         'def f(d):\n'
                         '    threading.Thread(target=lambda: EditSession(d, "x"))\n',
                         RULE_TARGET),
    "thread_class.py": ('import threading\n'
                        'class Runner(threading.Thread):\n'
                        '    def run(self):\n'
                        '        self.s = WpeSession(self.d, "x")\n',
                        RULE_TARGET),
    "clean_server.py": ('"""WpeSession and ThreadPoolExecutor in a docstring."""\n'
                        'import threading\n'
                        '# ThreadPoolExecutor in a comment\n'
                        'def serve(srv):\n'
                        '    srv.serve_forever()\n'
                        'def f(srv, d):\n'
                        '    threading.Thread(target=serve, args=(srv,)).start()\n'
                        '    with WpeSession(d, "x") as s:\n'
                        '        s.key("a")\n',
                        None),
    "pool_nolaunch.py": ('from concurrent.futures import ThreadPoolExecutor\n'
                         'def f(items):\n'
                         '    with ThreadPoolExecutor(4) as p:\n'
                         '        return list(p.map(len, items))\n',
                         RULE_POOL),
    "pool_helper.py": ('from concurrent.futures import ThreadPoolExecutor\n'
                       'def run_all(one, cuts):\n'
                       '    with ThreadPoolExecutor(8) as p:\n'
                       '        return list(p.map(one, cuts))\n',
                       RULE_POOL),
    "pool_to_thread.py": ('import asyncio\n'
                          'async def f(one):\n'
                          '    return await asyncio.to_thread(one, 1)\n',
                          RULE_POOL),
    "pool_executor.py": ('import asyncio\n'
                         'async def f(one):\n'
                         '    loop = asyncio.get_running_loop()\n'
                         '    return await loop.run_in_executor(None, one, 1)\n',
                         RULE_POOL),
    "pool_mp_threadpool.py": ('from multiprocessing.pool import ThreadPool\n'
                              'def f(one, cuts):\n'
                              '    return ThreadPool(4).map(one, cuts)\n',
                              RULE_POOL),
    "pool_mp_pool.py": ('import multiprocessing\n'
                        'def f(one, cuts):\n'
                        '    with multiprocessing.Pool(4) as p:\n'
                        '        return p.map(one, cuts)\n',
                        RULE_POOL),
    "pool_process.py": ('from concurrent.futures import ProcessPoolExecutor\n',
                        RULE_POOL),
    "pool_low_level.py": ('import _thread\n'
                          'def f(one):\n'
                          '    _thread.start_new_thread(one, ())\n',
                          RULE_POOL),
    "thread_transitive.py": ('import threading\n'
                             'def one(d, n):\n'
                             '    return WpeSession(d, "x")\n'
                             'def f(d):\n'
                             '    threading.Thread(target=lambda: one(d)).start()\n',
                             RULE_TARGET),
    "pool_transitive.py": ('from multiprocessing.pool import ThreadPool\n'
                           'def one(d, n):\n'
                           '    return WpeSession(d, "x")\n'
                           'def f(d):\n'
                           '    p = ThreadPool(4)\n'
                           '    return p.map(lambda n: one(d, n), range(3))\n',
                           RULE_TARGET),
    "thread_chain.py": ('import threading\n'
                        'def one(d):\n'
                        '    return EditSession(d, "x")\n'
                        'def mid(d):\n'
                        '    return one(d)\n'
                        'def f(d):\n'
                        '    threading.Thread(target=mid, args=(d,)).start()\n',
                        RULE_TARGET),
}


def _word_of(node):
    """The identifier a node spells (Name, Attribute, alias, definition),
    or None."""
    if isinstance(node, ast.Name):
        return node.id
    if isinstance(node, ast.Attribute):
        return node.attr
    if isinstance(node, ast.alias):
        return node.name
    if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
        return node.name
    return None


def _dotted(node):
    """The dotted name of a Name or Attribute chain, or None."""
    if isinstance(node, ast.Name):
        return node.id
    if isinstance(node, ast.Attribute):
        head = _dotted(node.value)
        return head + "." + node.attr if head else None
    return None


def _is_pool_module(name):
    """True for the name of a module that runs work elsewhere: the module
    or a submodule of one of POOL_MODULES."""
    return bool(name) and name.split(".")[0] in POOL_MODULES


def _names_pool(node):
    """True when @node imports a module of POOL_MODULES or names one of
    POOL_WORDS."""
    if isinstance(node, ast.Import):
        return any(_is_pool_module(a.name) for a in node.names)
    if isinstance(node, ast.ImportFrom):
        return _is_pool_module(node.module)
    return _word_of(node) in POOL_WORDS


def _names_launch(tree):
    """The line of the first launch word under @tree, or None."""
    for node in ast.walk(tree):
        if _word_of(node) in LAUNCH_WORDS:
            return node.lineno
    return None


def _functions(tree):
    """{name: node} of the functions defined anywhere in @tree."""
    return {n.name: n for n in ast.walk(tree)
            if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))}


def _called_names(tree):
    """The set of names called (by name or as an attribute) under @tree."""
    return {_word_of(n.func) for n in ast.walk(tree)
            if isinstance(n, ast.Call) and _word_of(n.func)}


def _launching_functions(functions):
    """The names of the functions that launch, directly or through the
    functions of the file they call: the fixed point of 'names a launch, or
    calls a function that does'."""
    launching = {name for name, node in functions.items()
                 if _names_launch(node) is not None}
    calls = {name: _called_names(node) for name, node in functions.items()}
    grew = True
    while grew:
        grew = False
        for name, called in calls.items():
            if name not in launching and called & launching:
                launching.add(name)
                grew = True
    return launching


def _handed_callables(call):
    """The expressions a thread-starting call hands over to run: the target
    keyword, and the first positional argument of submit, map, apply_async
    (a Thread's own first positional is its group, never a callable)."""
    word = _word_of(call.func)
    handed = [kw.value for kw in call.keywords if kw.arg == "target"]
    if word in ("submit", "map", "apply_async") and call.args:
        handed.append(call.args[0])
    if word == "Timer" and len(call.args) > 1:
        handed.append(call.args[1])
    return handed


def _target_launches(expr, launching):
    """True when a handed callable launches: a lambda that names a launch or
    calls a launching function, or the name of a launching function."""
    if isinstance(expr, ast.Lambda):
        return (_names_launch(expr) is not None
                or bool(_called_names(expr) & launching))
    return _word_of(expr) in launching


def _subclasses_thread(node):
    """True for a class with Thread among its bases."""
    return (isinstance(node, ast.ClassDef)
            and any(_word_of(b) in THREAD_BASES for b in node.bases))


def _class_launches(node, launching):
    """True when a class names a launch or calls a launching function."""
    return (_names_launch(node) is not None
            or bool(_called_names(node) & launching))


def offences(source, name="<source>"):
    """{rule: [line, ...]} for the Python @source."""
    tree = ast.parse(source, filename=name)
    launching = _launching_functions(_functions(tree))
    found = {rule: [] for rule in RULES}
    for node in ast.walk(tree):
        if _names_pool(node):
            found[RULE_POOL].append(node.lineno)
        if isinstance(node, ast.Call) and _word_of(node.func) in THREAD_STARTERS:
            for expr in _handed_callables(node):
                if _target_launches(expr, launching):
                    found[RULE_TARGET].append(node.lineno)
        if _subclasses_thread(node) and _class_launches(node, launching):
            found[RULE_TARGET].append(node.lineno)
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
    """{(file, rule): [line, ...]} of the offences in the tree."""
    found = {}
    for rel in sorted(_python_files(root)):
        if rel == SELF_FILE:
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
                "%s: %s: %d offence(s) at line(s) %s, allowed %d; run the "
                "sessions one after another on the main thread "
                "(wpe_driver.sweep_cuts), never from a pool or a thread; a "
                "pool that starts no session needs a line with its reason "
                "in tests/test-threads.allow"
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
    blind.extend(_selftest_allow(found))
    return blind


def _selftest_allow(found):
    """The messages of a judge that does not honour the allow file: a
    listed offence must pass, an unlisted one and a stale line must fail."""
    key = (os.path.join("tests", "pool_nolaunch.py"), RULE_POOL)
    if key not in found:
        return []
    count = len(found[key])
    blind = []
    if judge({key: found[key]}, {key: count}):
        blind.append("selftest: an allowed offence was still refused")
    if not judge({key: found[key]}, {}):
        blind.append("selftest: an offence with no allow line passed")
    if not judge({}, {key: count}):
        blind.append("selftest: a stale allow line passed")
    return blind


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--srcdir", default=os.environ.get("srcdir", "."))
    parser.add_argument("--allow", default=None)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)
    blind = selftest()
    for message in blind:
        print("check-test-threads: FAIL " + message)
    if blind or args.selftest:
        if not blind:
            print("check-test-threads: selftest ok")
        return 1 if blind else 0
    allow = args.allow or os.path.join(args.srcdir, "tests",
                                       "test-threads.allow")
    problems = judge(scan(args.srcdir), read_allow(allow))
    for message in problems:
        print("check-test-threads: FAIL " + message)
    if problems:
        return 1
    print("check-test-threads: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
