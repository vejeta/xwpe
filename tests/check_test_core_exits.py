#!/usr/bin/env python3
"""check_test_core_exits.py -- a C test ends a child by a core signal only
after it gave up its core file.

A test child that abort()s, raises a core signal or puts a core signal back
to its default action on purpose, as the row's way of testing what the
parent does with a dead child, leaves a core file and a journal record on
the machine.  A developer who finds one cannot tell a test that crashed on
purpose from a real crash.  tests/test_watchdog.h has the one helper,
test_child_dies_on_purpose(), that takes the core file away (RLIMIT_CORE 0,
and no dump on Linux); a child that dies on purpose calls it first.

Rule CA: in tests/*.c and tests/*.h, every raise() of a signal that is not
known to be quiet (a literal core signal, or anything not a literal), every
abort(), every kill() of the process's own id with a core signal, and every
sigaction()/signal() that puts a core signal back to SIG_DFL must stand in a
function body that calls test_child_dies_on_purpose() earlier.  A test whose
own crash is the failure it exists to show (a fake engine that must dump) is
listed in tests/test-core-exits.allow as `file function count why`; a count
may only fall, and a line above the measurement is stale and fails.

  check_test_core_exits.py [--srcdir DIR] [--allow FILE] [--selftest]

--selftest plants offences and clean files in a scratch folder and fails
unless the scan names each offence and not the clean ones (a scanner that
sees nothing is not a check).  With no option the self-test runs, then the
tree.

Exit: 0 clean, 1 offenders or a blind scanner.
"""
import argparse
import os
import re
import sys
import tempfile

HELPER = "test_child_dies_on_purpose"
CORE_SIGNALS = "SEGV|ABRT|BUS|QUIT|ILL|FPE|TRAP|SYS"
QUIET_SIGNAL_WORDS = ("SIGTERM", "SIGUSR1", "SIGUSR2", "SIGINT", "SIGHUP",
                      "SIGPIPE", "SIGCHLD", "SIGSTOP", "SIGKILL", "SIGALRM",
                      "SIGTSTP", "SIGCONT", "SIGWINCH", "SIGTTIN", "SIGTTOU")
SELF_FILE = os.path.join("tests", "check_test_core_exits.py")
FILE_SUFFIXES = (".c", ".h")
FREE_FUNCTION = "<top>"

RAISE = re.compile(r"\braise\s*\(\s*([A-Za-z_0-9]*)")
ABORT = re.compile(r"\babort\s*\(")
SELF_KILL = re.compile(r"\bkill\s*\(\s*(?:getpid\s*\(\s*\)|0)\s*,\s*SIG(?:%s)\b"
                       % CORE_SIGNALS)
SIGNAL_DFL = re.compile(r"\bsignal\s*\(\s*SIG(?:%s)\s*,\s*SIG_DFL" % CORE_SIGNALS)
SIGACTION_CORE = re.compile(r"\bsigaction\s*\(\s*SIG(?:%s)\b" % CORE_SIGNALS)
SIG_DFL_WORD = re.compile(r"\bSIG_DFL\b")
HELPER_CALL = re.compile(r"\b%s\s*\(" % HELPER)
FUNCTION_NAME = re.compile(r"([A-Za-z_][A-Za-z_0-9]*)\s*\([^;{}]*\)\s*[^;{}()]*$")

# One offence per form, and clean files, planted by the self-test.  The text
# is a string, so this file holds no offence itself.
CANARIES = {
    "raise_core.c": ("void f(void)\n{\n\traise(SIGSEGV);\n}\n", "f"),
    "raise_var.c": ("void g(int sig)\n{\n\traise(sig);\n}\n", "g"),
    "abort_plain.c": ("static void h(void)\n{\n  abort();\n}\n", "h"),
    "self_kill.h": ("static void k(void)\n{\n kill(getpid(), SIGBUS);\n}\n", "k"),
    "dfl_signal.c": ("void m(void)\n{\n signal(SIGABRT, SIG_DFL);\n}\n", "m"),
    "dfl_sigaction.c": ("void n(void)\n{\n struct sigaction a;\n"
                        " a.sa_handler = SIG_DFL;\n sigaction(SIGQUIT, &a, 0);\n}\n",
                        "n"),
    "late_helper.c": ("void p(void)\n{\n abort();\n"
                      " test_child_dies_on_purpose();\n}\n", "p"),
    "ok_after_helper.c": ("void q(void)\n{\n test_child_dies_on_purpose();\n"
                          " raise(SIGSEGV);\n abort();\n}\n", None),
    "ok_quiet.c": ("/* abort() in a comment, raise(SIGSEGV) too */\n"
                   "void r(void)\n{\n raise(SIGTERM);\n"
                   " puts(\"abort() and raise(SIGSEGV) in a string\");\n}\n",
                   None),
}


def strip_comments_and_strings(text):
    """@text with comments, string and character literals blanked out, line
    breaks kept so positions still count lines."""
    out = []
    i = 0
    length = len(text)
    while i < length:
        pair = text[i:i + 2]
        if pair == "/*":
            end = text.find("*/", i + 2)
            end = length if end < 0 else end + 2
            out.append(re.sub(r"[^\n]", " ", text[i:end]))
            i = end
        elif pair == "//":
            end = text.find("\n", i)
            end = length if end < 0 else end
            out.append(" " * (end - i))
            i = end
        elif text[i] in "\"'":
            end = _literal_end(text, i)
            out.append(text[i] + re.sub(r"[^\n]", " ", text[i + 1:end - 1])
                       + text[end - 1])
            i = end
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def _literal_end(text, start):
    """The index just after the string or character literal at @start."""
    quote = text[start]
    i = start + 1
    while i < len(text):
        if text[i] == "\\":
            i += 2
        elif text[i] == quote or text[i] == "\n":
            return i + 1
        else:
            i += 1
    return len(text)


def function_bodies(clean):
    """[(name, body_start, body_text)] for every block at brace depth 0 of
    the comment-free @clean text; the name is the identifier before the
    parameter list, FREE_FUNCTION for a block that has none."""
    bodies = []
    depth = 0
    last_end = 0
    start = 0
    for i, ch in enumerate(clean):
        if ch == "{":
            if depth == 0:
                start = i
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                head = clean[last_end:start]
                bodies.append((_name_of(head), start, clean[start:i + 1]))
                last_end = i + 1
        elif ch == ";" and depth == 0:
            last_end = i + 1
    return bodies


def _name_of(head):
    """The function a block belongs to, from the text before its brace."""
    lines = [l for l in head.split("\n") if not l.lstrip().startswith("#")]
    found = FUNCTION_NAME.search(" ".join(lines).strip())
    return found.group(1) if found else FREE_FUNCTION


def _offence_positions(body):
    """The offsets in @body of every way to end by a core signal."""
    hits = [m.start() for m in ABORT.finditer(body)]
    hits += [m.start() for m in SELF_KILL.finditer(body)]
    hits += [m.start() for m in SIGNAL_DFL.finditer(body)]
    for found in RAISE.finditer(body):
        if found.group(1) not in QUIET_SIGNAL_WORDS:
            hits.append(found.start())
    if SIG_DFL_WORD.search(body):
        hits += [m.start() for m in SIGACTION_CORE.finditer(body)]
    return sorted(hits)


def offences(source):
    """{function: [line, ...]} of the offences in the C @source that do not
    follow a call of the helper in the same function."""
    clean = strip_comments_and_strings(source)
    found = {}
    for name, start, body in function_bodies(clean):
        helper = HELPER_CALL.search(body)
        limit = helper.start() if helper else len(body) + 1
        late = [pos for pos in _offence_positions(body) if pos < limit]
        if late:
            lines = sorted({clean.count("\n", 0, start + pos) + 1
                            for pos in late})
            found.setdefault(name, []).extend(lines)
    return found


def _c_files(root):
    """The .c and .h files directly in @root/tests, relative to @root."""
    base = os.path.join(root, "tests")
    for name in sorted(os.listdir(base)):
        if name.endswith(FILE_SUFFIXES):
            yield os.path.join("tests", name)


def scan(root):
    """{(file, function): [line, ...]} of the offences in the tree."""
    found = {}
    for rel in _c_files(root):
        if rel == SELF_FILE:
            continue
        with open(os.path.join(root, rel), encoding="utf-8",
                  errors="replace") as fh:
            hits = offences(fh.read())
        for name, lines in hits.items():
            found[(rel, name)] = lines
    return found


def read_allow(path):
    """{(file, function): count} from the allow file; comments and blanks
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
    """The messages that fail the check: a function with more offences than
    the allow file counts, and an allow line the tree no longer needs."""
    problems = []
    for (rel, name), lines in sorted(found.items()):
        allowance = allowed.get((rel, name), 0)
        if len(lines) > allowance:
            problems.append(
                "%s: %s: %d way(s) to end by a core signal at line(s) %s, "
                "allowed %d; call %s() first in the child"
                % (rel, name, len(lines), ", ".join(str(n) for n in lines),
                   allowance, HELPER))
    for (rel, name), count in sorted(allowed.items()):
        have = len(found.get((rel, name), []))
        if have < count:
            problems.append("%s: %s: allowed %d but the tree has %d; lower "
                            "the count in the allow file"
                            % (rel, name, count, have))
    return problems


def selftest():
    """Plant the canaries; the scan must name each offence in its function
    and spare the clean ones.  Returns the messages of a blind scanner."""
    blind = []
    with tempfile.TemporaryDirectory() as root:
        os.makedirs(os.path.join(root, "tests"))
        for name, (text, _function) in CANARIES.items():
            with open(os.path.join(root, "tests", name), "w") as fh:
                fh.write(text)
        found = scan(root)
    for name, (_text, function) in CANARIES.items():
        rel = os.path.join("tests", name)
        if function is None:
            if any(key[0] == rel for key in found):
                blind.append("selftest: the scan named the clean file %s"
                             % name)
        elif (rel, function) not in found:
            blind.append("selftest: the scan did not name %s in %s"
                         % (function, name))
    return blind


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--srcdir", default=os.environ.get("srcdir", "."))
    parser.add_argument("--allow", default=None)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)
    blind = selftest()
    for message in blind:
        print("check-test-core-exits: FAIL " + message)
    if blind or args.selftest:
        if not blind:
            print("check-test-core-exits: selftest ok")
        return 1 if blind else 0
    allow = args.allow or os.path.join(args.srcdir, "tests",
                                       "test-core-exits.allow")
    problems = judge(scan(args.srcdir), read_allow(allow))
    for message in problems:
        print("check-test-core-exits: FAIL " + message)
    if problems:
        return 1
    print("check-test-core-exits: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
