#!/usr/bin/env python3
"""The line layer of the editor, the buffer owner, against a small model of it.

``tests/edit_driver.c`` is the owner (``we_buffer.c``) behind a line-by-line
protocol: no terminal, no editor, thousands of operations a second.  This
module starts it, runs seeded walks of owner operations on it and on
``OwnerModel`` side by side, and reports every difference in the rows and
their line ends, the breakpoint rows, the block marks and the undo depths.

The model is written from the compatibility chapter's account of the owner
(marks move with their lines, a breakpoint stays on its code and comes back
with Undo, a copy has none, one operation is one step) and from the file
reader's wrap, not from reading ``we_buffer.c``.  It reuses ``wrap_reader``
from ``blocks.py``, so a line of text becomes rows the same way for both
the block commands and the owner, and a difference there shows twice.

Run as a script::

    tests/.venv/bin/python tests/edit_model/cdriver.py --seed 1 --steps 5000
"""
import argparse
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.dirname(HERE)
TOP = os.path.dirname(TESTS)
if TESTS not in sys.path:
    sys.path.insert(0, TESTS)

from edit_model import blocks  # noqa: E402

HARD, SOFT = "H", "S"
RC_OK = 0
RC_NOMEM = -1
RC_RANGE = -2
RC_NOTHING = -3
ROWS_FIT = -1        # the driver's K line: no row wider than the width
UNDO_EXTRA = 1
DEFAULT_WIDTH = 20
DEFAULT_NUMUNDO = 10
STEPS = 5000
WORDS = [b"alpha", b"be", b"gamma-delta", b"x", b"epsilon", b"zeta", b"w00000000000000000000000"]
BUILD_TIMEOUT = 120


# What the driver is made of: when any of these is newer than a binary, that
# binary describes an owner that no longer exists.
DRIVER_SOURCES = ("tests/edit_driver.c", "we_buffer.c", "we_buffer.h", "edit.h")


def is_current(binary):
    """Is @binary at least as new as every source the driver is made of?
    A walk run against an older one would test the owner of the past."""
    try:
        made = os.path.getmtime(binary)
        return all(os.path.getmtime(os.path.join(TOP, s)) <= made for s in DRIVER_SOURCES)
    except OSError:
        return False


def build(dest_dir):
    """The driver's binary: the one ``make check`` built beside the sources
    when it is newer than the sources, else one compiled into @dest_dir now;
    None when there is no compiler.  An old binary is never returned."""
    built = os.path.join(TOP, "edit_driver")
    if os.path.exists(built) and is_current(built):
        return built
    out = os.path.join(dest_dir, "edit_driver")
    cmd = ["gcc", "-std=gnu17", "-I", TOP, "-o", out,
           os.path.join(TOP, "tests", "edit_driver.c"), os.path.join(TOP, "we_buffer.c")]
    try:
        subprocess.run(cmd, check=True, timeout=BUILD_TIMEOUT, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
    except (OSError, subprocess.SubprocessError):
        return None
    return out


def esc(data):
    """A string the driver reads back as @data: no blanks, \\n, \\xHH."""
    out = []
    for b in data:
        if b == 0x0A:
            out.append("\\n")
        elif b == 0x5C:
            out.append("\\\\")
        elif 0x20 < b < 0x7F:
            out.append(chr(b))
        else:
            out.append("\\x%02x" % b)
    return "".join(out)


def unesc(text):
    out = bytearray()
    i = 0
    while i < len(text):
        if text[i] == "\\" and text[i + 1] == "x":
            out.append(int(text[i + 2:i + 4], 16))
            i += 4
        elif text[i] == "\\" and text[i + 1] == "n":
            out.append(0x0A)
            i += 2
        elif text[i] == "\\" and text[i + 1] == "\\":
            out.append(0x5C)
            i += 2
        else:
            out.append(ord(text[i]))
            i += 1
    return bytes(out)


class Result:
    """What the driver printed after one command."""

    def __init__(self, lines):
        self.rc = 0
        self.rows = []       # [(end, bytes)]
        self.rows2 = []
        self.brp = []
        self.marks = ((0, 0), (0, 0))
        self.ud = self.rd = self.save = 0
        self.rows_check = self.rows_check2 = ROWS_FIT
        for ln in lines:
            tag, _, rest = ln.partition(" ")
            if tag in ("K", "K2"):
                n = int(rest.split("rows=")[1])
                if tag == "K":
                    self.rows_check = n
                else:
                    self.rows_check2 = n
            elif tag == "R":
                self.rc = int(rest.split("rc=")[1])
            elif tag in ("L", "L2"):
                _i, end, data = (rest.split(" ", 2) + [""])[:3]
                (self.rows if tag == "L" else self.rows2).append((end, unesc(data)))
            elif tag == "B":
                self.brp = [] if rest.strip() == "-" else [int(v) for v in rest.split()]
            elif tag == "M":
                a, b = rest.split()
                self.marks = (tuple(int(v) for v in a.split(",")),
                              tuple(int(v) for v in b.split(",")))
            elif tag == "U":
                kv = dict(f.split("=") for f in rest.split())
                self.ud, self.rd, self.save = int(kv["ud"]), int(kv["rd"]), int(kv["save"])


class Driver:
    """A running ``edit_driver``."""

    def __init__(self, binary):
        self.proc = subprocess.Popen([binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     universal_newlines=True, bufsize=1)
        self.sent = 0

    def run(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        self.sent += 1
        lines = []
        while True:
            ln = self.proc.stdout.readline()
            if ln == "":
                raise EOFError("the driver ended after %r" % line)
            ln = ln.rstrip("\n")
            if ln == "E":
                return Result(lines)
            lines.append(ln)

    def close(self):
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        self.proc.wait(timeout=10)


class OwnerModel:
    """Rows with identities, breakpoints by identity, marks, undo as snapshots."""

    def __init__(self, width=DEFAULT_WIDTH, numundo=DEFAULT_NUMUNDO):
        self.width = width
        self.numundo = numundo
        self.next_id = 0
        self.rows = [[SOFT, b"", self.new_id()]]
        self.brp = set()
        self.marks = [[0, 0], [0, 0]]
        self.marks_known = True
        self.undo = []
        self.redo = []
        self.second = None

    def new_id(self):
        self.next_id += 1
        return self.next_id

    # -- snapshots
    def snap(self):
        return ([list(r) for r in self.rows], set(self.brp), [list(m) for m in self.marks])

    def restore(self, s):
        """Give back rows and marks.  The breakpoints are by identity and
        stay as they are: a row's breakpoint left with the row and comes
        back with it, and one set since is not undone."""
        self.rows = [list(r) for r in s[0]]
        self.marks = [list(m) for m in s[2]]

    def push(self, before):
        self.undo.append(before)
        self.redo = []
        while len(self.undo) > self.numundo + UNDO_EXTRA:
            self.undo.pop(0)

    # -- reading
    def lines(self):
        return [(r[0], r[1]) for r in self.rows]

    def brp_rows(self):
        return [i for i, r in enumerate(self.rows) if r[2] in self.brp]

    def wrap(self, text, width):
        """The rows the file reader makes of @text (lines_from_text): every
        logical line wrapped, every last row hard."""
        rows = []
        pos = 0
        while pos < len(text):
            nl = text.find(b"\n", pos)
            piece = text[pos:nl] if nl >= 0 else text[pos:]
            pos = (nl + 1) if nl >= 0 else len(text)
            rows.extend(blocks.wrap_reader(piece, width, True))
        return rows

    def clamp_marks(self):
        for m in self.marks:
            m[0] = max(0, min(m[0], len(self.rows) - 1))
            m[1] = max(0, min(m[1], len(self.rows[m[0]][1])))

    def move_marks(self, y, n_del, n_ins):
        """The marks after n_del rows from y were replaced by n_ins rows:
        a mark on a removed row goes to the start of the row that took its
        place, or to the end of the text; rows below shift."""
        stay = min(n_del, n_ins)
        for m in self.marks:
            if y <= m[0] < y + stay:
                continue                    # the row is still there, changed
            if m[0] < y + n_del and m[0] >= y:
                if y + n_ins < len(self.rows):
                    m[0], m[1] = y + n_ins, 0
                else:
                    m[0] = len(self.rows) - 1
                    m[1] = len(self.rows[-1][1])
            elif m[0] >= y + n_del:
                m[0] += n_ins - n_del
        self.clamp_marks()

    # -- operations: each returns the owner's return code
    def op_text(self, text):
        rows = self.wrap(text, self.width)
        self.rows = [[e, d, self.new_id()] for d, e in rows] or [[SOFT, b"", self.new_id()]]
        self.undo, self.redo = [], []
        self.clamp_marks()
        return RC_OK

    def op_splice(self, y, n_del, text):
        if y < 0 or y > len(self.rows) or n_del < 0 or y + n_del > len(self.rows):
            return RC_RANGE
        new = self.wrap(text, self.width)
        old = self.rows[y:y + n_del]
        if [(e, d) for d, e in new] == [(r[0], r[1]) for r in old]:
            return RC_OK            # the same rows again: nothing changes, nothing to undo
        before = self.snap()
        # a row put where a row stood is that row changed: it keeps its
        # identity, so its breakpoint and the marks in it stay
        built = [[e, d, old[k][2] if k < len(old) else self.new_id()]
                 for k, (d, e) in enumerate(new)]
        self.rows[y:y + n_del] = built
        if not self.rows:
            self.rows = [[SOFT, b"", self.new_id()]]
            built = list(self.rows)
        self.move_marks(y, n_del, len(built))
        self.push(before)
        return RC_OK

    def op_delete(self, y, n):
        if y < 0 or y >= len(self.rows) or n <= 0:
            return RC_RANGE
        n = min(n, len(self.rows) - y)
        before = self.snap()
        del self.rows[y:y + n]
        ins = 0
        if not self.rows:
            self.rows = [[SOFT, b"", self.new_id()]]
            ins = 1
        self.move_marks(y, n, ins)
        self.push(before)
        return RC_OK

    def op_blank(self, y):
        if y < 0 or y > len(self.rows):
            return RC_RANGE
        before = self.snap()
        self.rows.insert(y, [SOFT, b"", self.new_id()])
        self.move_marks(y, 0, 1)
        self.push(before)
        return RC_OK

    def op_split(self, y, x):
        if y < 0 or y >= len(self.rows):
            return RC_RANGE
        before = self.snap()
        end, data, ident = self.rows[y]
        x = min(x, len(data))
        if end == SOFT and x == len(data):
            # Enter at the end of a row without a break: the row takes one,
            # and the row below already is the rest of its line
            self.rows[y] = [HARD, data, ident]
            self.move_marks(y, 1, 1)
        elif x == 0:
            self.rows.insert(y, [HARD, b"", self.new_id()])
            self.move_marks(y, 0, 1)
        else:
            moved = [None, None]
            for k, m in enumerate(self.marks):
                if m[0] == y and m[1] > x:
                    moved[k] = [y + 1, m[1] - x]
            self.rows[y:y + 1] = [[HARD, data[:x], ident], [end, data[x:], self.new_id()]]
            self.move_marks(y, 1, 2)
            for k in range(2):
                if moved[k] is not None:
                    self.marks[k] = moved[k]
            self.clamp_marks()
        self.push(before)
        return RC_OK

    def op_move(self, y_from, n, y_to):
        if y_from < 0 or y_from >= len(self.rows) or n <= 0:
            return RC_RANGE
        n = min(n, len(self.rows) - y_from)
        if y_to < 0 or y_to > len(self.rows) - n:
            return RC_RANGE
        moved = self.rows[y_from:y_from + n]
        del self.rows[y_from:y_from + n]
        self.rows[y_to:y_to] = moved
        self.marks_known = False
        # the owner's move makes no record of its own: the editor's block
        # command reserves one ('v') and pushes it, so here Ctrl-U has
        # nothing to take back
        return RC_OK

    def op_copy(self, y_from, n, y_to):
        if y_from < 0 or y_from >= len(self.rows) or n <= 0:
            return RC_RANGE
        n = min(n, len(self.rows) - y_from)
        if y_to < 0 or y_to > len(self.rows):
            return RC_RANGE
        before = self.snap()
        copies = [[e, d, self.new_id()] for e, d, _i in self.rows[y_from:y_from + n]]
        self.rows[y_to:y_to] = copies
        self.marks_known = False
        self.push(before)
        return RC_OK

    def op_undo(self):
        if not self.undo:
            return RC_NOTHING
        self.redo.append(self.snap())
        self.restore(self.undo.pop())
        self.marks_known = False
        return RC_OK

    def op_redo(self):
        if not self.redo:
            return RC_NOTHING
        self.undo.append(self.snap())
        self.restore(self.redo.pop())
        self.marks_known = False
        return RC_OK

    def op_mark(self, y1, x1, y2, x2):
        self.marks = [[y1, x1], [y2, x2]]
        self.marks_known = True
        return RC_OK

    def op_brp(self, rows):
        """The breakpoint rows become @rows; those parked with removed rows
        stay parked."""
        here = set(r[2] for r in self.rows)
        self.brp = (self.brp - here) | set(self.rows[r][2] for r in rows
                                           if 0 <= r < len(self.rows))
        return RC_OK


def differences(model, res):
    """[(field, model's value, driver's value)] for one result."""
    out = []
    if (res.rows_check, res.rows_check2) != (ROWS_FIT, ROWS_FIT):
        out.append(("row width", (ROWS_FIT, ROWS_FIT), (res.rows_check, res.rows_check2)))
    if model.lines() != res.rows:
        out.append(("text", model.lines(), res.rows))
    if sorted(model.brp_rows()) != sorted(res.brp):
        out.append(("breakpoints", model.brp_rows(), res.brp))
    if model.marks_known and [tuple(m) for m in model.marks] != list(res.marks):
        out.append(("marks", [tuple(m) for m in model.marks], list(res.marks)))
    if (len(model.undo), len(model.redo)) != (res.ud, res.rd):
        out.append(("steps", (len(model.undo), len(model.redo)), (res.ud, res.rd)))
    return out


def model_call(model, name, args):
    """Run command @name on the model; its return code."""
    table = {
        "text": lambda: model.op_text(unesc(args[0])),
        "splice": lambda: model.op_splice(int(args[0]), int(args[1]), unesc(args[2])),
        "delete": lambda: model.op_delete(int(args[0]), int(args[1])),
        "blank": lambda: model.op_blank(int(args[0])),
        "split": lambda: model.op_split(int(args[0]), int(args[1])),
        "move": lambda: model.op_move(int(args[0]), int(args[1]), int(args[2])),
        "copy": lambda: model.op_copy(int(args[0]), int(args[1]), int(args[2])),
        "undo": model.op_undo,
        "redo": model.op_redo,
        "mark": lambda: model.op_mark(*[int(a) for a in args]),
        "brp": lambda: model.op_brp([int(a) for a in args]),
    }
    return table[name]()


class OwnerWalk:
    """One seeded walk of owner operations on the driver and the model."""

    def __init__(self, driver, seed, steps=STEPS, width=DEFAULT_WIDTH, numundo=DEFAULT_NUMUNDO):
        self.drv = driver
        self.rng = random.Random("owner-%d-%d-%d" % (seed, width, numundo))
        self.steps = steps
        self.model = OwnerModel(width, numundo)
        self.width, self.numundo = width, numundo
        self.history = []
        self.findings = []

    def text(self):
        rng = self.rng
        lines = []
        for _ in range(rng.randint(1, 3)):
            lines.append(b" ".join(rng.choice(WORDS) for _ in range(rng.randint(0, 6))))
        return b"\n".join(lines) + b"\n"

    def draw(self):
        rng, m = self.rng, self.model
        n = len(m.rows)
        # No move here: the owner's move leaves nothing for Ctrl-U (see
        # OwnerModel.op_move), so it is tried on its own, in the test file.
        kind = rng.choices(["splice", "delete", "blank", "split", "copy", "undo",
                            "redo", "mark", "brp"], [5, 4, 2, 4, 3, 4, 2, 2, 1])[0]
        if kind == "splice":
            y = rng.randint(0, n)
            return ["splice", str(y), str(rng.randint(0, min(2, n - y))), esc(self.text())]
        if kind == "delete":
            y = rng.randrange(n)
            return ["delete", str(y), str(rng.randint(1, min(2, n - y)))]
        if kind == "blank":
            return ["blank", str(rng.randint(0, n))]
        if kind == "split":
            y = rng.randrange(n)
            return ["split", str(y), str(rng.randint(0, len(m.rows[y][1]) + 1))]
        if kind == "move":
            y = rng.randrange(n)
            k = rng.randint(1, min(2, n - y))
            return ["move", str(y), str(k), str(rng.randint(0, n - k))]
        if kind == "copy":
            y = rng.randrange(n)
            return ["copy", str(y), str(rng.randint(1, min(2, n - y))), str(rng.randint(0, n))]
        if kind == "mark":
            a, b = rng.randrange(n), rng.randrange(n)
            return ["mark", str(a), str(rng.randint(0, len(m.rows[a][1]))),
                    str(b), str(rng.randint(0, len(m.rows[b][1])))]
        if kind == "brp":
            return ["brp"] + [str(r) for r in sorted(rng.sample(range(n), rng.randint(0, min(3, n))))]
        return [kind]

    def run(self):
        self.drv.run("open %d %d" % (self.width, self.numundo))
        self.drv.run("text " + esc(self.text()))
        model_call(self.model, "text", [esc(self.text())])
        res = self.drv.run("dump")
        # start both from the driver's own first text
        self.model.rows = [[e, d, self.model.new_id()] for e, d in res.rows]
        for i in range(self.steps):
            cmd = self.draw()
            self.history.append(" ".join(cmd))
            res = self.drv.run(" ".join(cmd))
            rc = model_call(self.model, cmd[0], cmd[1:])
            if rc != res.rc:
                self.findings.append((i, " ".join(cmd), "rc", rc, res.rc))
                self.model.rows = [[e, d, self.model.new_id()] for e, d in res.rows]
                continue
            for field, mine, theirs in differences(self.model, res):
                self.findings.append((i, " ".join(cmd), field, mine, theirs))
            if self.findings and self.findings[-1][0] == i:
                # take the driver's rows, keep the walk going
                self.model.rows = [[e, d, self.model.new_id()] for e, d in res.rows]
                self.model.brp = set(self.model.rows[r][2] for r in res.brp)
                self.model.marks = [list(m) for m in res.marks]
                self.model.undo = self.model.undo[-res.ud:] if res.ud else []
                self.model.redo = self.model.redo[-res.rd:] if res.rd else []
        return self


def first_diff(a, b):
    """Where two lists of rows first differ, as text."""
    for k in range(max(len(a), len(b))):
        x = a[k] if k < len(a) else None
        y = b[k] if k < len(b) else None
        if x != y:
            return "row %d model %r driver %r (%d and %d rows)" % (k, x, y, len(a), len(b))
    return "none"


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--seed", type=int, action="append", default=None)
    ap.add_argument("--steps", type=int, default=STEPS)
    ap.add_argument("--width", type=int, default=DEFAULT_WIDTH)
    args = ap.parse_args(argv)
    with tempfile.TemporaryDirectory() as tmp:
        binary = build(tmp)
        if binary is None:
            print("no compiler: the driver was not built")
            return 77
        for seed in args.seed or [1]:
            drv = Driver(binary)
            w = OwnerWalk(drv, seed, args.steps, args.width).run()
            drv.close()
            print("seed %d steps %d findings %d" % (seed, args.steps, len(w.findings)))
            for f in w.findings[:10]:
                print("  step %d %s: %s model %r driver %r" % (f[0], f[1][:60], f[2],
                                                               str(f[3])[:80], str(f[4])[:80]))
                if f[2] == "text":
                    print("     first different row: %s" % first_diff(f[3], f[4]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
