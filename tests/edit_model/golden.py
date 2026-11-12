#!/usr/bin/env python3
"""Golden traces: short key scripts and what today's editor did with them.

A golden is two files in ``goldens/``: ``<name>.keys``, the script (the
file it edits, its first text, the options, then one action per line in
the model's words), and ``<name>.trace``, the editor's state after each
action as the state channel reported it (the file window's ``W`` and ``L``
lines).  A golden becomes a source for the model only once the user has
read it and approved it: ``goldens/APPROVED`` lists the approved ones by
name, the sha256 of the trace, the date and the decision.  Until then the
golden rows skip.

Run as a script, it records a golden and prints its trace for review (it
writes nothing), checks the editor against itself at every step first, and
compares the model with the trace::

    tests/.venv/bin/python tests/edit_model/golden.py record goldens/enter_autoindent.keys
    tests/.venv/bin/python tests/edit_model/golden.py model goldens/enter_autoindent.keys
"""
import ast
import hashlib
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
if os.path.dirname(HERE) not in sys.path:
    sys.path.insert(0, os.path.dirname(HERE))

from edit_model import keymap, model, selfcheck  # noqa: E402
from edit_model.oracle import EditSession  # noqa: E402

GOLDEN_DIR = os.path.join(HERE, "goldens")
APPROVED = os.path.join(GOLDEN_DIR, "APPROVED")
COMMENT = "#"
STEP_HEAD = "step"


class Script:
    """A golden's script: file name, first text, options, actions."""

    def __init__(self, path):
        self.path = path
        self.name = os.path.splitext(os.path.basename(path))[0]
        self.filename = "t.c"
        self.seed = b""
        self.rc = None
        self.actions = []        # [(keymap name, bytes or None)]
        with open(path, encoding="utf-8") as fh:
            for line in fh:
                self._read(line.rstrip("\n"))

    def _read(self, line):
        if not line or line.startswith(COMMENT):
            return
        word, _, rest = line.partition(" ")
        if word == "file":
            self.filename = rest
        elif word == "seed":
            self.seed = ast.literal_eval(rest)
        elif word == "rc":
            self.rc = None if rest == "none" else ast.literal_eval(rest)
        elif word == "type":
            self.actions.append(("type", ast.literal_eval(rest)))
        else:
            self.actions.append((word, None))

    @property
    def trace_path(self):
        return os.path.splitext(self.path)[0] + ".trace"


# Fields of the W line a golden leaves out: the markers Ctrl-K 1-9 set,
# which no golden script touches.
OMITTED_FIELDS = ("pt=",)
EMPTY_MARKERS = "pt=" + "/".join(["-1,-1"] * 9)


def window_lines(dump, filename):
    """The ``W`` line (without the markers) and the ``L`` lines of
    @filename's window in a dump."""
    out = []
    n = None
    for line in dump.raw.split("\n"):
        if line.startswith("W "):
            fields = line.split(" ")
            n = fields[1] if fields[2] == filename else None
            if n is not None:
                out.append(" ".join(f for f in fields if not f.startswith(OMITTED_FIELDS)))
        elif line.startswith("L ") and n is not None and line.split(" ", 2)[1] == n:
            out.append(line)
    return out


def record(script, workdir):
    """Run @script on a fresh editor: [(action label, dump, findings)], where
    findings are the self-check's for that step."""
    out = []
    with EditSession(workdir, script.seed, filename=script.filename,
                     xwperc=script.rc) as s:
        s.trace_since_last()
        out.append(("start", s.last, []))
        for name, arg in script.actions:
            before = s.last.win
            d = s.send(keymap.keys_for(keymap.STYLE_OLD, name, arg))
            trace = s.trace_since_last()
            w = d.win
            found = []
            if w is None or d.ctx != "edit":
                found.append(("context", "ctx=%s" % d.ctx))
            else:
                found += selfcheck.check_inside(w)
                found += selfcheck.check_modified_mark(w, s.display())
                found += selfcheck.check_screen(w, s.display())
                bad, _fresh = selfcheck.check_trace(name, before, w, trace)
                found += bad
            label = name if arg is None else "type %r" % arg
            out.append((label, d, found))
    return out


# Each row line of a trace ends with this mark, so blanks at the end of a
# row survive editors and tools that trim trailing whitespace.
ROW_END = "|"


def trace_text(script, steps):
    """The .trace file's text for recorded @steps."""
    out = []
    for i, (label, d, _found) in enumerate(steps):
        out.append("%s %d %s" % (STEP_HEAD, i, label))
        for line in window_lines(d, script.filename):
            out.append(line + ROW_END if line.startswith("L ") else line)
    return "\n".join(out) + "\n"


def read_trace(path):
    """[(label, [W and L lines as the dump has them])] from a .trace file."""
    steps = []
    with open(path, encoding="latin-1") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if line.startswith(STEP_HEAD + " "):
                steps.append((line.split(" ", 2)[2], []))
            elif steps:
                if line.startswith("L ") and line.endswith(ROW_END):
                    line = line[:-len(ROW_END)]
                steps[-1][1].append(line)
    return steps


def trace_sha256(path):
    with open(path, "rb") as fh:
        return hashlib.sha256(fh.read()).hexdigest()


def approved():
    """{name: sha256} of the goldens the user approved."""
    out = {}
    if not os.path.exists(APPROVED):
        return out
    with open(APPROVED, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith(COMMENT):
                continue
            fields = line.split()
            out[fields[0]] = fields[1]
    return out


def scripts():
    """Every golden script, by name."""
    if not os.path.isdir(GOLDEN_DIR):
        return []
    return sorted(os.path.join(GOLDEN_DIR, f) for f in os.listdir(GOLDEN_DIR)
                  if f.endswith(".keys"))


def _with_markers(lines):
    """Trace lines as a dump has them: the W line with its (empty) markers."""
    return [line + " " + EMPTY_MARKERS if line.startswith("W ") else line
            for line in lines]


def model_against_trace(script, trace_steps):
    """Run the model over a golden: [(step, label, field, model, golden)]
    where they differ.  The model starts from the trace's first state."""
    from edit_model.oracle import Dump
    out = []
    dumps = [Dump(["S 0 ctx=edit top=1 autosaved=0"] + _with_markers(lines) + ["E 0"])
             for _label, lines in trace_steps]
    windows = [list(d.windows.values())[0] for d in dumps]
    st = model.state_from_window(windows[0], model.Options.from_xwperc(script.rc))
    for i, (name, arg) in enumerate(script.actions, start=1):
        w = windows[i]
        st.view = windows[i - 1].view
        st = model.apply(st, model.Action(keymap.model_action(name), arg))
        for field, mine, theirs in (
                (model.F_TEXT, st.lines(), list(w.lines)),
                (model.F_CURSOR, (st.y, st.x), w.cursor),
                (model.F_MARKS, (st.mb, st.me), (w.mb, w.me)),
                (model.F_STEPS, (st.ud, st.rd), (w.ud, w.rd)),
                (model.F_SAVE, st.save if st.save_known else w.save, w.save)):
            if mine != theirs:
                out.append((i, trace_steps[i][0], field, mine, theirs))
        model.resync(st, w)
    return out


def main(argv):
    if len(argv) != 2 or argv[0] not in ("record", "model"):
        print(__doc__)
        return 2
    script = Script(argv[1])
    if argv[0] == "record":
        steps = record(script, tempfile.mkdtemp())
        problems = [(i, f) for i, (_l, _d, fs) in enumerate(steps) for f in fs]
        for i, f in problems:
            print("SELF-CHECK step %d: %s" % (i, f), file=sys.stderr)
        sys.stdout.write(trace_text(script, steps))
        return 1 if problems else 0
    for row in model_against_trace(script, read_trace(script.trace_path)):
        print("step %d %-12s %-8s model %r golden %r" % row)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
