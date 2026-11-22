#!/usr/bin/env python3
"""check_test_paths.py -- a pytest row does not depend on how long its path is.

The pyte half of tests/check-test-paths.sh.  A row passed at a short
temporary folder and failed at a long one for four reasons the readers in
tests/wpe_driver.py and the folders of tests/deep_folder.py now own; this
scan fails a row that goes round them again.

Rule P1: every phrase a row asserts with `in` on the text read through
pane_text_with_history or pane_rows_with_history is one of that call's
needles (or part of one).  The reader pages back until its needles are
found, so a needle shorter than the asserted phrase can be found while the
rest of the phrase is still above the view.  The text is followed from the
name the reader's result is bound to and, further, from the names assigned
from it, up to ASSIGNMENT_HOPS of them
(`text = "\\n".join(pane_logical_lines(rows))`).  Two things
cannot be compared by a scan and are named in tests/test-pane-needles.allow
with the reason: a call whose needles are computed (a parameter, a list built
at run time), and an asserted phrase that is computed (a variable, a format,
a path).  An entry that matches no site is stale and fails.

Rule P2: no file but tests/wpe_driver.py reads a window's title off the
frame letters.  The zoom and close buttons of a window ("[] x" as the
driver writes them) and a pattern on the frame's letter q both break on a
name with a q in it and on a title the editor cut; wpe_driver's
active_window_title reads it by column.

Rule P3: no file but tests/deep_folder.py grows a path in a loop.  A `while`
loop that tests a length and joins a path, in a function that makes the
folder, is a path of N bytes made by hand; it fails when the test's own
folder is already longer than N (the folder exists).  deep_folder.at_least
makes it.

  check_test_paths.py [--srcdir DIR] [--allow FILE] [--selftest]

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

RULE_NEEDLES = "P1"
RULE_TITLE = "P2"
RULE_LOOP = "P3"
RULES = (RULE_NEEDLES, RULE_TITLE, RULE_LOOP)
PANE_READERS = ("pane_text_with_history", "pane_rows_with_history")
NEEDLES_KEYWORD = "needle_words"
NEEDLES_POSITION = 1
DRIVER_FILE = os.path.join("tests", "wpe_driver.py")
# The rows that test the readers feed them title rows and weak needles on
# purpose, to prove what the readers do with them.
READER_TEST_FILE = os.path.join("tests", "test_wpe_driver_readers.py")
TITLE_READER_FILES = (DRIVER_FILE, READER_TEST_FILE)
NEEDLE_EXEMPT_FILES = (DRIVER_FILE, READER_TEST_FILE)
OWNER_FILE = os.path.join("tests", "deep_folder.py")
SELF_FILE = os.path.join("tests", "check_test_paths.py")
SKIPPED_FOLDERS = ("eval", "harness", ".venv", "__pycache__")
MODULE_SCOPE = "<module>"
BUTTONS = "\u25a1 \u2715"            # the zoom and close buttons of a window
TITLE_PATTERN_HEAD = "q ("            # a title taken off the frame letters:
TITLE_PATTERN_TAIL = ") q"            # r"q ([^q]+?) q"
FOLDER_MAKERS = ("makedirs", "mkdir")
ALLOW_FIELDS = 3                      # file, function, reason
ASSIGNMENT_HOPS = 3                   # names followed from a reader's result

# One offence per rule and a clean file, planted by the self-test.  The text
# is a string, so this file holds no offence itself.
CANARIES = {
    "weak_needle.py": (
        'from wpe_driver import pane_text_with_history\n'
        'def test_a(s):\n'
        '    pane = " ".join(pane_text_with_history(s, ["regular file"]))\n'
        '    assert "not a regular file" in pane\n', RULE_NEEDLES, 4),
    "weak_const.py": (
        'from wpe_driver import pane_rows_with_history\n'
        'SAID = "the file changed while it was checked"\n'
        'def test_a(s):\n'
        '    rows = pane_rows_with_history(s, ["changed while"])\n'
        '    assert SAID in "\\n".join(rows)\n', RULE_NEEDLES, 5),
    "weak_hop.py": (
        'from wpe_driver import pane_rows_with_history, pane_logical_lines\n'
        'def test_a(s):\n'
        '    rows = pane_rows_with_history(s, ["regular file"])\n'
        '    text = " ".join(pane_logical_lines(rows))\n'
        '    assert "not a regular file" in text\n', RULE_NEEDLES, 5),
    "computed_needles.py": (
        'from wpe_driver import pane_text_with_history\n'
        'def helper(s, needles):\n'
        '    return pane_text_with_history(s, needles)\n', RULE_NEEDLES, 3),
    "computed_phrase.py": (
        'from wpe_driver import pane_text_with_history\n'
        'def test_a(s, path):\n'
        '    pane = " ".join(pane_text_with_history(s, ["a"]))\n'
        '    assert str(path) in pane\n', RULE_NEEDLES, 4),
    "title_buttons.py": (
        'ACTIVE = "\\u25a1 \\u2715"\n'
        'def title(row):\n'
        '    return ACTIVE in row\n', RULE_TITLE, 1),
    "title_regex.py": (
        'import re\n'
        'def title(row):\n'
        '    return re.search(r"q ([^q]+?) q", row)\n', RULE_TITLE, 3),
    "path_loop.py": (
        'import os\n'
        'def make(base, total):\n'
        '    deep = base\n'
        '    while len(deep) < total:\n'
        '        deep = os.path.join(deep, "d" * 40)\n'
        '    os.makedirs(deep)\n', RULE_LOOP, 4),
    "clean.py": (
        '"""A "\\u25a1 \\u2715" and q (x) q in a docstring."""\n'
        'import os\n'
        'from wpe_driver import pane_text_with_history\n'
        'NEEDLES = ["not a regular file", "Nothing was written"]\n'
        'def test_a(s):\n'
        '    pane = "\\n".join(pane_text_with_history(s, NEEDLES))\n'
        '    assert "Nothing was written" in pane\n'
        '    assert "regular file" in pane\n'
        '    assert "unrelated" not in pane\n'
        'def fields(text):\n'
        '    out = []\n'
        '    while len(out) < 4:\n'
        '        out.append(text)\n'
        '    return out\n', None, 0),
}
# What the self-test expects of the computed sites: file -> function.
CANARY_COMPUTED = {"computed_needles.py": "helper",
                   "computed_phrase.py": "test_a"}


def _walk_own(scope):
    """Every node of @scope except the inside of a function defined in it:
    a nested function is a scope of its own, so a call is named once."""
    pending = [scope]
    while pending:
        node = pending.pop()
        yield node
        for child in ast.iter_child_nodes(node):
            if isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef,
                                  ast.Lambda)):
                continue
            pending.append(child)


def _call_name(node):
    """The name a call is made by, whether bare or as an attribute."""
    func = node.func
    if isinstance(func, ast.Attribute):
        return func.attr
    return getattr(func, "id", "")


def _docstring_ids(tree):
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


def _module_constants(tree):
    """{name: expression} of the module's plain `NAME = expression`."""
    found = {}
    for node in tree.body:
        if (isinstance(node, ast.Assign) and len(node.targets) == 1
                and isinstance(node.targets[0], ast.Name)):
            found[node.targets[0].id] = node.value
    return found


def _resolve(node, constants, depth=0):
    """The list of strings @node stands for, or None when it is computed.

    A string, a module constant, a list or tuple of those and a sum of
    them are known; anything else (a parameter, a call, a format) is
    computed."""
    if depth > 8:
        return None
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return [node.value]
    if isinstance(node, ast.Name) and node.id in constants:
        return _resolve(constants[node.id], constants, depth + 1)
    if isinstance(node, (ast.List, ast.Tuple)):
        out = []
        for element in node.elts:
            part = _resolve(element, constants, depth + 1)
            if part is None:
                return None
            out.extend(part)
        return out
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
        left = _resolve(node.left, constants, depth + 1)
        right = _resolve(node.right, constants, depth + 1)
        if left is None or right is None:
            return None
        if isinstance(node.left, (ast.List, ast.Tuple, ast.Name)):
            return left + right
        return [left[0] + right[0]]
    return None


def _needles_of(call, constants):
    """The needles of a reader call as a list of strings, or None."""
    node = None
    for keyword in call.keywords:
        if keyword.arg == NEEDLES_KEYWORD:
            node = keyword.value
    if node is None and len(call.args) > NEEDLES_POSITION:
        node = call.args[NEEDLES_POSITION]
    if node is None:
        return None
    return _resolve(node, constants)


def _reader_calls(node):
    """The calls of a pane reader inside @node, outermost first."""
    return [n for n in _walk_own(node)
            if isinstance(n, ast.Call) and _call_name(n) in PANE_READERS]


def _names_in(node):
    """The identifiers @node reads."""
    return {n.id for n in ast.walk(node) if isinstance(n, ast.Name)}


def _functions(tree):
    """(name, node) of every function, plus the module itself."""
    found = [(MODULE_SCOPE, tree)]
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            found.append((node.name, node))
    return found


def _targets_of(node):
    """The plain names an assignment binds (a tuple target binds each)."""
    names = []
    for target in node.targets:
        if isinstance(target, ast.Name):
            names.append(target.id)
        elif isinstance(target, (ast.Tuple, ast.List)):
            names.extend(e.id for e in target.elts if isinstance(e, ast.Name))
    return names


def _bound_readers(scope):
    """{variable: [call, ...]}: the reader calls a name stands for in @scope's
    own statements (a nested function is its own scope).  A name is bound by
    an assignment from a reader call, and one hop further by an assignment
    from a name that is (text = join(pane_logical_lines(rows)))."""
    direct = {}
    assigns = [n for n in _walk_own(scope) if isinstance(n, ast.Assign)]
    for node in assigns:
        calls = _reader_calls(node.value)
        if calls:
            for name in _targets_of(node):
                direct.setdefault(name, []).extend(calls)
    bound = {name: list(calls) for name, calls in direct.items()}
    for _hop in range(ASSIGNMENT_HOPS):
        before = {name: list(calls) for name, calls in bound.items()}
        for node in assigns:
            reads = _names_in(node.value) & set(before)
            if not reads or _reader_calls(node.value):
                continue
            for name in _targets_of(node):
                for read in sorted(reads):
                    for call in before[read]:
                        if call not in bound.setdefault(name, []):
                            bound[name].append(call)
    return bound


def _asserted_phrases(scope, calls_of, constants):
    """[(line, phrase or None, reads, right)] of `phrase in <text of a reader
    call>` in @scope; the phrase is None when it is computed."""
    found = []
    for node in _walk_own(scope):
        if not (isinstance(node, ast.Compare) and len(node.ops) == 1
                and isinstance(node.ops[0], ast.In)):
            continue
        right = node.comparators[0]
        reads = _names_in(right) & set(calls_of)
        if not reads and not _reader_calls(right):
            continue
        phrase = _resolve(node.left, constants)
        if phrase is not None and len(phrase) != 1:
            phrase = None
        if phrase is not None:
            phrase = phrase[0]
        found.append((node.lineno, phrase, reads, right))
    return found


def _needle_offences(tree):
    """({line: why} of P1 offences, [(function, line)] of the computed sites:
    reader calls with computed needles and asserted phrases that are
    computed)."""
    constants = _module_constants(tree)
    weak = {}
    computed = []
    for scope_name, scope in _functions(tree):
        if scope_name == MODULE_SCOPE:
            continue
        bound = _bound_readers(scope)
        for call in _reader_calls(scope):
            if _needles_of(call, constants) is None:
                computed.append((scope_name, call.lineno))
        for line, phrase, reads, right in _asserted_phrases(scope, bound,
                                                            constants):
            if phrase is None:
                computed.append((scope_name, line))
                continue
            calls = []
            for name in reads:
                calls.extend(bound[name])
            calls.extend(_reader_calls(right))
            needles = []
            for call in calls:
                known = _needles_of(call, constants)
                if known is None:
                    needles = None
                    break
                needles.extend(known)
            if needles is None:
                continue
            if not any(phrase in needle for needle in needles):
                weak[line] = ("asserts %r, which is not part of any needle %r"
                              % (phrase, needles))
    return weak, sorted(set(computed))


def _title_offences(tree):
    """Lines that read a window's title off the frame."""
    docstrings = _docstring_ids(tree)
    lines = []
    for node in ast.walk(tree):
        if not (isinstance(node, ast.Constant) and isinstance(node.value, str)):
            continue
        if id(node) in docstrings:
            continue
        text = node.value
        if BUTTONS in text or (TITLE_PATTERN_HEAD in text
                               and TITLE_PATTERN_TAIL in text):
            lines.append(node.lineno)
    return sorted(set(lines))


def _is_path_growth(node):
    """True for os.path.join(...) and for a path joined with `/`."""
    if isinstance(node, ast.Call) and _call_name(node) == "join":
        return True
    return isinstance(node, ast.BinOp) and isinstance(node.op, ast.Div)


def _is_length_test(node):
    """True when a while-test holds a len() call."""
    return any(isinstance(n, ast.Call) and _call_name(n) == "len"
               for n in ast.walk(node))


def _loop_offences(tree):
    """Lines of the `while len(...)` loops that grow a path, in a function
    that makes the folder."""
    lines = []
    for _name, scope in _functions(tree):
        if scope is tree:
            continue
        makes = any(isinstance(n, ast.Call) and _call_name(n) in FOLDER_MAKERS
                    for n in _walk_own(scope))
        if not makes:
            continue
        for node in _walk_own(scope):
            if not (isinstance(node, ast.While) and _is_length_test(node.test)):
                continue
            if any(_is_path_growth(n) for n in ast.walk(node)):
                lines.append(node.lineno)
    return sorted(set(lines))


def offences(source, name="<source>"):
    """({rule: [line, ...]}, [(function, line)]): the offences of the
    Python @source and the computed sites of rule P1."""
    tree = ast.parse(source, filename=name)
    weak, computed = _needle_offences(tree)
    found = {RULE_NEEDLES: sorted(weak), RULE_TITLE: _title_offences(tree),
             RULE_LOOP: _loop_offences(tree)}
    return found, computed


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
    """({(file, rule): [line, ...]}, {(file, function): [line, ...]}): the
    offences and the computed sites, outside the owners."""
    found = {}
    computed = {}
    for rel in sorted(_python_files(root)):
        if rel == SELF_FILE:
            continue
        with open(os.path.join(root, rel), encoding="utf-8",
                  errors="replace") as fh:
            hits, sites = offences(fh.read(), rel)
        for rule, lines in hits.items():
            if rule == RULE_TITLE and rel in TITLE_READER_FILES:
                continue
            if rule == RULE_NEEDLES and rel in NEEDLE_EXEMPT_FILES:
                continue
            if rule == RULE_LOOP and rel == OWNER_FILE:
                continue
            if lines:
                found[(rel, rule)] = lines
        if rel in NEEDLE_EXEMPT_FILES:
            continue    # the reader's definition and the rows that test it
        for function, line in sites:
            computed.setdefault((rel, function), []).append(line)
    return found, computed


def read_allow(path):
    """{(file, function): reason} from the allow file; comments and blanks
    skipped."""
    allowed = {}
    if not os.path.exists(path):
        return allowed
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            words = line.split(None, ALLOW_FIELDS - 1)
            if len(words) < ALLOW_FIELDS:
                words += [""] * (ALLOW_FIELDS - len(words))
            allowed[(words[0], words[1])] = words[2]
    return allowed


WHY = {
    RULE_NEEDLES: "an asserted phrase is not one of the call's needles; "
                  "the needle is the whole phrase the row asserts",
    RULE_TITLE: "a window's title is read off the frame; use "
                "wpe_driver.active_window_title (or window_buttons_on)",
    RULE_LOOP: "a path is grown to a length by hand; use "
               "deep_folder.at_least",
}


def judge(found, computed, allowed):
    """The messages that fail the check: an offence, a computed site the
    allow file does not name (or names without a reason), and an allow line
    no site needs any more."""
    problems = []
    for (rel, rule), lines in sorted(found.items()):
        problems.append("%s: %s: line(s) %s: %s"
                        % (rel, rule, ", ".join(str(n) for n in lines),
                           WHY[rule]))
    for (rel, function), lines in sorted(computed.items()):
        reason = allowed.get((rel, function))
        if reason is None:
            problems.append("%s: %s: line(s) %s: %s() reads the pane with "
                            "computed needles or asserts a computed phrase; "
                            "name it in the allow file with the reason"
                            % (rel, RULE_NEEDLES,
                               ", ".join(str(n) for n in lines), function))
        elif not reason:
            problems.append("%s: %s: %s() is in the allow file without a "
                            "reason" % (rel, RULE_NEEDLES, function))
    for (rel, function) in sorted(allowed):
        if (rel, function) not in computed:
            problems.append("%s: %s: %s() is in the allow file but no longer "
                            "has a computed needle or phrase; remove the line"
                            % (rel, RULE_NEEDLES, function))
    return problems


def selftest():
    """Plant the canaries; the scan must name each offence under its rule
    on its line and spare the clean file.  Returns the messages of a blind
    scanner."""
    blind = []
    with tempfile.TemporaryDirectory() as root:
        os.makedirs(os.path.join(root, "tests"))
        for name, (text, _rule, _line) in CANARIES.items():
            with open(os.path.join(root, "tests", name), "w") as fh:
                fh.write(text)
        found, computed = scan(root)
    for name, (_text, rule, line) in CANARIES.items():
        rel = os.path.join("tests", name)
        if name in CANARY_COMPUTED:
            if (rel, CANARY_COMPUTED[name]) not in computed:
                blind.append("selftest: the scan did not see the computed "
                             "site of %s" % name)
        elif rule is None:
            if any(key[0] == rel for key in found) or any(
                    key[0] == rel for key in computed):
                blind.append("selftest: the scan named the clean file %s"
                             % name)
        elif line not in found.get((rel, rule), []):
            blind.append("selftest: the scan did not name %s line %d under %s"
                         % (name, line, rule))
    return blind


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--srcdir", default=os.environ.get("srcdir", "."))
    parser.add_argument("--allow", default=None)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)
    blind = selftest()
    for message in blind:
        print("check-test-paths: FAIL " + message)
    if blind or args.selftest:
        if not blind:
            print("check-test-paths: python selftest ok")
        return 1 if blind else 0
    allow = args.allow or os.path.join(args.srcdir, "tests",
                                       "test-pane-needles.allow")
    found, computed = scan(args.srcdir)
    problems = judge(found, computed, read_allow(allow))
    for message in problems:
        print("check-test-paths: FAIL " + message)
    if problems:
        return 1
    print("check-test-paths: python rules ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
