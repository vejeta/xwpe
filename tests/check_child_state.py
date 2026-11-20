#!/usr/bin/env python3
"""check_child_state.py -- the Python half of tests/check-child-state.sh.

A test asks whether a process is alive through gui_owner.process_running
(over procinfo.run_state) and nowhere else.  os.kill(pid, 0) and
os.killpg(pgid, 0) answer "does the pid exist", and an unreaped zombie
exists, so they say "alive" about a process that has ended.

Rule 3 of the same check: a test of a pid's /proc folder for existence
(os.path.exists("/proc/%d" % pid) and the like) is false on every system with
no /proc, so it calls a live process gone.

The scan uses Python's own tokenizer, so comments, docstrings and strings are
never read as code.  It flags a call of `kill` or `killpg` (as os.kill, as
os.killpg, or bare after `from os import ...`) whose second argument is the
number 0.  It does not flag a signal that ends a process, nor a method
(proc.kill()).

  check_child_state.py FILE...        one line "file:line: what" per offence
  check_child_state.py --selftest     fixtures: each offence is named, each
                                      clean file is not

Exit: 0 clean, 1 an offence or a file that could not be read.
"""
import io
import re
import sys
import tokenize

WATCHED = ("kill", "killpg")
SKIPPED = (tokenize.COMMENT, tokenize.NL, tokenize.NEWLINE, tokenize.INDENT,
           tokenize.DEDENT, tokenize.ENCODING)


def code_tokens(text):
    """The tokens of @text that are code: no comment, no blank, no layout."""
    out = []
    for tok in tokenize.generate_tokens(io.StringIO(text).readline):
        if tok.type in SKIPPED:
            continue
        out.append(tok)
    return out


def call_args(tokens, open_at):
    """The top-level arguments of the call whose "(" is tokens[open_at]: a list
    of token lists, and the index of the matching ")" (len(tokens) when none)."""
    depth = 0
    args = [[]]
    i = open_at
    while i < len(tokens):
        tok = tokens[i]
        if tok.type == tokenize.OP and tok.string in "([{":
            depth += 1
            if depth > 1:
                args[-1].append(tok)
        elif tok.type == tokenize.OP and tok.string in ")]}":
            depth -= 1
            if depth == 0:
                return args, i
            args[-1].append(tok)
        elif depth == 1 and tok.type == tokenize.OP and tok.string == ",":
            args.append([])
        else:
            args[-1].append(tok)
        i += 1
    return args, len(tokens)


def is_zero(arg):
    """True when the argument is the number 0 and nothing else."""
    return len(arg) == 1 and arg[0].type == tokenize.NUMBER and arg[0].string == "0"


def starts_a_name(tokens, i):
    """True when tokens[i] is a bare name or os.<name>, not a method of another object and not a def."""
    if i == 0:
        return True
    prev = tokens[i - 1]
    if prev.type == tokenize.NAME and prev.string == "def":
        return False
    if prev.type == tokenize.OP and prev.string == ".":
        return i >= 2 and tokens[i - 2].type == tokenize.NAME and tokens[i - 2].string == "os"
    return True


EXISTENCE_CALLS = ("exists", "lexists", "isdir", "stat", "lstat", "access")
PROC_PID_TEXT = re.compile(r"^/proc/(%d|%s|\{[^{}]*\}|)$")
FSTRING_MIDDLE = getattr(tokenize, "FSTRING_MIDDLE", None)


def literal_text(tok):
    """The text of a string token without its prefix and quotes (the middle of
    an f-string on a Python that splits them), or None for any other token."""
    if tok.type == tokenize.STRING:
        body = tok.string.lstrip("rbfuRBFU")
        return body[1:-1] if len(body) >= 2 else body
    if FSTRING_MIDDLE is not None and tok.type == FSTRING_MIDDLE:
        return tok.string
    return None


def names_a_pid_folder(arg):
    """True when the argument holds the text of a process's /proc folder
    ("/proc/%d", "/proc/%s", f"/proc/{pid}", or "/proc/" joined to a pid)."""
    for tok in arg:
        text = literal_text(tok)
        if text is not None and PROC_PID_TEXT.match(text):
            return True
    return False


def scan_proc_existence(tokens):
    """The lines of every existence test of /proc/<pid>: a test of a pid's /proc
    folder says "gone" on every system that has no /proc, which is every BSD."""
    found = []
    for i, tok in enumerate(tokens):
        if tok.type != tokenize.NAME or tok.string not in EXISTENCE_CALLS:
            continue
        if i + 1 >= len(tokens) or tokens[i + 1].string != "(" or i < 2:
            continue
        if tokens[i - 1].string != "." or tokens[i - 2].string not in ("path", "os"):
            continue
        args, _ = call_args(tokens, i + 1)
        if args and names_a_pid_folder(args[0]):
            found.append((tok.start[0], "rule 3: a test of /proc/<pid> is false on every system without /proc;"
                          " use gui_owner.process_running"))
    return found


def scan_text(text):
    """The line numbers and words of every liveness call in @text."""
    tokens = code_tokens(text)
    found = scan_proc_existence(tokens)
    for i, tok in enumerate(tokens):
        if tok.type != tokenize.NAME or tok.string not in WATCHED:
            continue
        if i + 1 >= len(tokens) or tokens[i + 1].string != "(":
            continue
        if not starts_a_name(tokens, i):
            continue
        args, _ = call_args(tokens, i + 1)
        if len(args) == 2 and is_zero(args[1]):
            found.append((tok.start[0], "rule 2: %s(..., 0) asks whether a pid exists, not whether it runs;"
                          " use gui_owner.process_running" % tok.string))
    return found


def scan_file(path):
    """Print the offences of one file; return how many (a file that cannot be read counts as one)."""
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
        found = scan_text(text)
    except (OSError, tokenize.TokenError, SyntaxError, IndentationError) as exc:
        print("%s:0: could not be read: %s" % (path, exc))
        return 1
    for line, what in found:
        print("%s:%d: %s" % (path, line, what))
    return len(found)


def selftest():
    """Each offence is named and each clean text is not."""
    must_hit = {
        "os.kill": "import os\nos.kill(pid, 0)\n",
        "os.killpg": "import os\nos.killpg(pgid, 0)\n",
        "spaced": "import os\nos.kill (pid , 0 )\n",
        "bare": "from os import kill\nkill(pid, 0)\n",
        "split": "import os\nos.kill(\n    pid,\n    0)\n",
        "nested": "import os\nos.kill(int(open(p).read()), 0)\n",
        "proc folder": 'import os\nos.path.exists("/proc/%d" % pid)\n',
        "proc folder, negated": 'import os\nx = not os.path.exists("/proc/%d" % pid)\n',
        "proc folder, f-string": 'import os\nos.path.isdir(f"/proc/{pid}")\n',
        "proc folder, os.stat": 'import os\nos.stat("/proc/%s" % pid)\n',
    }
    must_pass = {
        "comment": "# os.kill(pid, 0)\nx = 1\n",
        "docstring": 'def f():\n    """os.kill(pid, 0) says nothing"""\n',
        "string": 'x = "os.kill(pid, 0)"\n',
        "signal": "import os\nos.kill(pid, signal.SIGTERM)\n",
        "method": "proc.kill()\nproc.kill(0)\n",
        "other number": "import os\nos.kill(pid, 10)\n",
        "definition": "def kill(pid, sig=0):\n    pass\n",
        "proc read": 'import os\nos.path.exists("/proc/%d/fd" % pid)\nopen("/proc/%d/stat" % pid)\n',
        "proc self": 'import os\nos.path.exists("/proc/self/stat")\n',
        "proc comment": '# os.path.exists("/proc/%d" % pid)\nx = 1\n',
    }
    bad = 0
    for name, text in must_hit.items():
        if not scan_text(text):
            print("FAIL: selftest: %s was not flagged" % name)
            bad += 1
    for name, text in must_pass.items():
        got = scan_text(text)
        if got:
            print("FAIL: selftest: %s was flagged: %s" % (name, got))
            bad += 1
    if bad == 0:
        print("check_child_state: selftest ok (%d offences named, %d clean files passed)"
              % (len(must_hit), len(must_pass)))
    return bad


def main(argv):
    if argv[1:] == ["--selftest"]:
        return 1 if selftest() else 0
    bad = 0
    for path in argv[1:]:
        bad += scan_file(path)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
