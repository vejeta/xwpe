#!/usr/bin/env python3
"""Checks of the editor against itself, from its state dumps alone.

No model of editing is consulted: every check compares what the editor
reports with what it reported a moment before, with the screen, with the
file on disk and with its own trace.  So a mistake a model shares with the
editor cannot hide here.  The checks (numbered as the design numbers them):

1. Ctrl-U after an operation gives back the dump from before it (lines,
   line ends, breakpoint rows; the cursor is reported apart), and Ctrl-R
   gives back the dump after it.  Walks run this undo probe after an
   operation with a given probability.
2. In a locked window a change is refused: the lines, the modified count
   and the undo lists stay as they were.
3. The cursor and the block marks are inside the text.
4. Breakpoint rows are inside the text, with no row twice.
5. The window's modified mark shows exactly when the count is above 0, and
   an autosave's .ASV file holds the text of the same moment.
6. Each fresh change pushes as many undo steps as its records say
   (``buffer record ... more=0 replay=none``), a fresh change empties the
   redo list, a Ctrl-U moves one step from the undo list to the redo list
   and a Ctrl-R moves it back; no ``buffer depth leak``.
7. The text shown in the window, and the file a Save writes, are the dump's.
8. The editor answers every operation (liveness).

Run as a script for seeded walks::

    tests/.venv/bin/python tests/edit_model/selfcheck.py --seed 1 --ops 100
"""
import argparse
import os
import random
import statistics
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
if os.path.dirname(HERE) not in sys.path:
    sys.path.insert(0, os.path.dirname(HERE))

from edit_model.oracle import (ChannelMiscount, ChannelTimeout,  # noqa: E402
                               EditSession, ESC, ctrl)
from edit_model.inventory import OP_KINDS  # noqa: E402

UNDO = ctrl("u")
REDO = ctrl("r")
# The undo list keeps the user's Num. Undo steps and one more (we_buffer.c).
DEFAULT_NUMUNDO = 10
UNDO_STEPS_KEPT = DEFAULT_NUMUNDO + 1
# How often the screen is compared with the dump, in operations.
SCREEN_EVERY = 10
# Where the modified mark sits: on the window's bottom border, this many
# cells right of its left corner.
MODIFIED_MARK_OFFSET = 3
MODIFIED_MARK = "*"
DEFAULT_TABN = 8
FILENAME = "t.c"
# A file the user may not write: the editor opens it locked (the padlock).
READONLY_MODE = 0o444

# A file with the shapes that have hidden bugs before: a tab, a row near the
# window's width, a multibyte character, a row of spaces, start and end of
# text, no final line break.
SEED = ("int main(void)\n"
        "{\n"
        "\treturn 0;\n"
        "}\n"
        + " ".join(["word%02d" % i for i in range(16)]) + "\n"
        "café au lait\n"
        "   \n"
        "one\n"
        "two\n"
        "last line")

# The kinds a walk draws from, and their weights: the key loop's own edits
# most, then blocks, undo and redo, then the rest.  Kinds that open a
# dialog, change windows or read files are left to the context probe.
WALK_WEIGHTS = {
    "type_ascii": 20, "type_space": 4, "type_utf8": 2, "enter": 8, "tab": 3,
    "backspace": 8, "delete": 7, "ctrl_t_delete_word": 2,
    "ctrl_z_delete_to_eol": 1, "ctrl_y_delete_line": 2,
    "left": 3, "right": 3, "up": 3, "down": 3, "home": 2, "end": 3,
    "ctrl_home": 1, "ctrl_end": 1, "pgup": 1, "pgdn": 1,
    "ctrl_pgup": 2, "ctrl_pgdn": 2, "ctrl_left": 1, "ctrl_right": 2,
    "shift_left": 1, "shift_right": 2, "shift_up": 1, "shift_down": 2,
    "shift_home": 1, "shift_end": 1, "insert_toggle": 1, "alt_j": 1,
    "ctrl_k_b": 3, "ctrl_k_k": 3, "ctrl_k_l": 2, "ctrl_k_x": 1, "ctrl_k_h": 1,
    "ctrl_k_c": 2, "ctrl_k_v": 2, "ctrl_k_y": 2, "ctrl_k_i": 2, "ctrl_k_u": 2,
    "ctrl_k_a": 1, "ctrl_k_z": 1, "ctrl_k_0": 1, "ctrl_o_0": 1,
    "ctrl_o_y": 1, "ctrl_o_t": 1, "ctrl_o_e": 1, "ctrl_o_l": 1,
    "ctrl_o_u": 1, "ctrl_o_m": 1, "ctrl_o_h": 1,
    "cut": 2, "copy": 2, "paste": 3, "ctrl_del_delete": 1,
    "undo": 6, "redo": 3, "save": 2,
}
KEYS = dict(OP_KINDS)
UNDO_KINDS = ("undo", "alt_backspace_undo")
REDO_KINDS = ("redo",)
# f->ins: bit 1 is Alt-J's mode, where control keys go into the text as
# characters (Ctrl-U types ^U); the value 8 is a read-only window.
INS_LITERAL = 2
INS_READONLY = 8
# While Alt-J's mode is on, how often the walk presses Alt-J again: the
# undo probe and the Ctrl-U checks need the mode off.
LITERAL_LEAVE_P = 0.3


def literal_mode(w):
    """Do keys go into window @w as characters (Alt-J's mode)?"""
    return w.ins != INS_READONLY and (w.ins & INS_LITERAL) != 0


class Finding(dict):
    """One disagreement of the editor with itself."""


def _rows(w):
    return [] if w is None else list(w.lines)


def _pos_ok(y, x, rows, past_break):
    """Is (y, x) a place inside the text?  @past_break: a mark may sit just
    after a line's break (the block then holds the break)."""
    if y < 0 or y >= len(rows):
        return False
    end, data = rows[y]
    limit = len(data)
    if past_break and end == "H":
        limit += 1
    return 0 <= x <= limit


def check_inside(w):
    """Checks 3 and 4 on one window: what is wrong, as short strings."""
    bad = []
    rows = _rows(w)
    if not _pos_ok(w.y, w.x, rows, False):
        bad.append(("cursor", "cursor %s outside %d rows" % (w.cursor, len(rows))))
    for name, (y, x) in (("mark_begin", w.mb), ("mark_end", w.me)):
        if w.mb == w.me:
            break
        if not _pos_ok(y, x, rows, True):
            bad.append(("marks", "%s (%d,%d) outside the text" % (name, y, x)))
    for r in w.brp:
        if r < 0 or r >= len(rows):
            bad.append(("breakpoints", "breakpoint row %d outside %d rows" % (r, len(rows))))
    if len(set(w.brp)) != len(w.brp):
        bad.append(("breakpoints", "a breakpoint row twice: %s" % w.brp))
    return bad


def _screen_cell(disp, y, x):
    if 0 <= y < len(disp) and 0 <= x < len(disp[y]):
        return disp[y][x]
    return None


def check_modified_mark(w, disp):
    """Check 5's first half: the mark shows iff the count is above 0."""
    _cy, _cx, ay, ax, ey, _ex = w.view
    cell = _screen_cell(disp, ey, ax + MODIFIED_MARK_OFFSET)
    shown = cell == MODIFIED_MARK
    if shown != (w.save > 0):
        return [("modified", "save=%d but the mark cell reads %r" % (w.save, cell))]
    return []


def _visible(data, tabn):
    """A dump row as the window would show it, or None when a tab or a
    byte past ASCII makes the column arithmetic uncertain."""
    if any(b < 0x20 or b > 0x7e for b in data):
        return None
    return data.decode("ascii")


def check_screen(w, disp, tabn=DEFAULT_TABN):
    """Check 7's first half: the rows the window shows are the dump's."""
    cy, cx, ay, ax, ey, ex = w.view
    width = ex - ax - 1
    bad = []
    for sy in range(ay + 1, ey):
        li = cy + (sy - ay - 1)
        if li >= len(w.lines) or sy >= len(disp):
            continue
        want = _visible(w.lines[li][1], tabn)
        if want is None:
            continue
        want = want[cx:cx + width].rstrip()
        got = disp[sy][ax + 1:ax + 1 + width].rstrip()
        if got != want:
            bad.append(("screen", "row %d shows %r, the dump holds %r" % (li, got, want)))
            break
    return bad


def check_trace(kind, before, after, trace, n_keys=1):
    """Check 6 for one operation of @n_keys keys: the undo lists against
    the records."""
    bad = []
    if any("buffer depth leak" in t for t in trace):
        bad.append(("trace", "buffer depth leak"))
    fresh = sum(1 for t in trace if t.startswith("buffer record")
                and "more=0" in t and "replay=none" in t)
    dud = after.ud - before.ud
    drd = after.rd - before.rd
    steps_allowed = 1
    if literal_mode(before):
        kind = "typed"
        steps_allowed = n_keys     # each key went into the text
    if kind in UNDO_KINDS:
        if before.ud > 0 and not before.ro and (dud, drd) != (-1, 1):
            bad.append(("undo-lists", "Ctrl-U moved ud %+d rd %+d" % (dud, drd)))
        return bad, fresh
    if kind in REDO_KINDS:
        if before.rd > 0 and not before.ro and (dud, drd) != (1, -1):
            bad.append(("undo-lists", "Ctrl-R moved ud %+d rd %+d" % (dud, drd)))
        return bad, fresh
    kept = min(fresh, UNDO_STEPS_KEPT - before.ud)
    if fresh > 0 and dud != kept and not (before.ud + fresh > UNDO_STEPS_KEPT
                                          and after.ud == UNDO_STEPS_KEPT):
        bad.append(("trace", "%d fresh steps in the trace, ud moved %+d" % (fresh, dud)))
    if fresh == 0 and dud != 0:
        bad.append(("trace", "no fresh record in the trace, ud moved %+d" % dud))
    if fresh > 0 and after.rd != 0:
        bad.append(("trace", "a fresh change left %d redo steps" % after.rd))
    if fresh > steps_allowed:
        bad.append(("steps", "one %s pushed %d undo steps" % (kind, fresh)))
    return bad, fresh


class Walk:
    """One seeded walk of operations with the self-checks after each."""

    def __init__(self, base, seed, n_ops, probe_p, readonly=False, binary=None):
        self.base = base
        self.seed = seed
        self.n_ops = n_ops
        self.probe_p = probe_p
        self.readonly = readonly
        self.binary = binary
        self.rng = random.Random(seed)
        self.findings = []
        self.op_ms = []
        self.start_ms = []
        self.history = []          # the kinds sent so far, for a reproduction
        self.probes = 0
        self.last_fresh = 0
        self.session = None
        kinds = sorted(WALK_WEIGHTS)
        self.kinds = kinds
        self.weights = [WALK_WEIGHTS[k] for k in kinds]

    # -- the session ---------------------------------------------------------
    def start(self):
        if self.session is not None:
            self.session.close()
        wd = tempfile.mkdtemp(dir=self.base)
        mode = None
        if self.readonly:
            mode = READONLY_MODE
        s = EditSession(wd, SEED, filename=FILENAME, binary=self.binary,
                        file_mode=mode)
        self.session = s
        self.start_ms.append(s.start_ms)
        self.history = []
        s.trace_since_last()

    def note(self, step, kind, cls, detail):
        self.findings.append(Finding(walk=self.seed, step=step, op=kind, cls=cls,
                                     detail=detail, keys=list(self.history)))

    # -- one operation -------------------------------------------------------
    def send(self, keys):
        return self.session.send(keys)

    def back_to_text(self):
        for _ in range(4):
            d = self.session.last
            if d.ctx == "edit" and d.win is not None and d.win.name == FILENAME:
                return True
            self.send([ESC])
        d = self.session.last
        return d.ctx == "edit" and d.win is not None and d.win.name == FILENAME

    def draw(self, before):
        """The next kind: by weight, and Alt-J often while its mode is on."""
        if literal_mode(before) and self.rng.random() < LITERAL_LEAVE_P:
            return "alt_j"
        return self.rng.choices(self.kinds, self.weights)[0]

    def step(self, i):
        s = self.session
        before = s.last.win
        kind = self.draw(before)
        try:
            after_dump = self.send(KEYS[kind])
        except (ChannelTimeout, ChannelMiscount) as e:
            self.note(i, kind, "liveness", str(e))
            self.start()
            return
        self.op_ms.append(s.answers_ms[-1])
        self.history.append(kind)
        trace = s.trace_since_last()
        if not s.alive():
            self.note(i, kind, "liveness", "the editor exited")
            self.start()
            return
        if after_dump.ctx != "edit" or after_dump.win is None or after_dump.win.name != FILENAME:
            if not self.back_to_text():
                self.note(i, kind, "left", "ctx=%s after the op" % after_dump.ctx)
                self.start()
            return
        after = after_dump.win
        self.check_after(i, kind, before, after, trace)
        if self.rng.random() < self.probe_p:
            self.undo_probe(i, kind, before, after)

    def check_after(self, i, kind, before, after, trace):
        s = self.session
        disp = s.display()
        for cls, detail in check_inside(after):
            self.note(i, kind, cls, detail)
        for cls, detail in check_modified_mark(after, disp):
            self.note(i, kind, cls, detail)
        if i % SCREEN_EVERY == 0:
            for cls, detail in check_screen(after, disp):
                self.note(i, kind, cls, detail)
        bad, fresh = check_trace(kind, before, after, trace, len(KEYS[kind]))
        for cls, detail in bad:
            self.note(i, kind, cls, detail)
        self.last_fresh = fresh
        if before.ro and kind not in UNDO_KINDS + REDO_KINDS and \
                (_rows(before) != _rows(after) or after.ud != before.ud):
            self.note(i, kind, "lock", "a locked window changed: %d -> %d rows, ud %d -> %d"
                      % (len(before.lines), len(after.lines), before.ud, after.ud))
        if kind == "save":
            disk = s.disk()
            if disk != after.disk():
                self.note(i, kind, "disk", "Save wrote %r, the dump holds %r"
                          % (disk[-60:] if disk else disk, after.disk()[-60:]))
        if s.last.autosaved:
            asv = s.disk(FILENAME + ".ASV")
            if asv != after.disk():
                self.note(i, kind, "autosave", ".ASV differs from the dump")

    def undo_probe(self, i, kind, before, after):
        """Check 1: Ctrl-U as many times as the op pushed steps (its fresh
        records: the undo list stops growing at its bound) gives the dump
        before it back, and Ctrl-R as many gives the dump after it."""
        if after.ro or kind in UNDO_KINDS + REDO_KINDS or literal_mode(after):
            return
        n = min(self.last_fresh, after.ud)
        changed = _rows(before) != _rows(after)
        if n <= 0:
            if changed:
                self.note(i, kind, "no-step", "the text changed and no undo step was pushed")
            return
        if self.last_fresh > after.ud:
            return                  # the bound dropped steps of this very op
        self.probes += 1
        try:
            u = self.send([UNDO] * n).win
            self.session.trace_since_last()
            self.compare(i, kind, "after Ctrl-U", before, u)
            r = self.send([REDO] * n).win
            self.session.trace_since_last()
            self.compare(i, kind, "after Ctrl-R", after, r)
        except (ChannelTimeout, ChannelMiscount) as e:
            self.note(i, kind, "liveness", "undo probe: %s" % e)
            self.start()

    def compare(self, i, kind, when, want, got):
        if got is None:
            self.note(i, kind, "text", "%s no text window" % when)
            return
        if _rows(want) != _rows(got):
            self.note(i, kind, "text", "%s: %s" % (when, _row_diff(_rows(want), _rows(got))))
        if want.brp != got.brp:
            self.note(i, kind, "breakpoints", "%s: rows %s, expected %s" % (when, got.brp, want.brp))
        if want.cursor != got.cursor:
            self.note(i, kind, "cursor", "%s: cursor %s, expected %s" % (when, got.cursor, want.cursor))
        for cls, detail in check_inside(got):
            self.note(i, kind, cls, "%s: %s" % (when, detail))

    def run(self):
        self.start()
        try:
            for i in range(self.n_ops):
                self.step(i)
        finally:
            if self.session is not None:
                self.session.close()
        return self


def _row_diff(want, got):
    """The first row where two texts differ, short."""
    for k in range(max(len(want), len(got))):
        a = want[k] if k < len(want) else None
        b = got[k] if k < len(got) else None
        if a != b:
            return "row %d: expected %r, got %r (%d vs %d rows)" % (k, a, b, len(want), len(got))
    return "same rows"


def summarize(walks):
    """Findings by class, with the first example of each, as text."""
    out = []
    by = {}
    for w in walks:
        for f in w.findings:
            by.setdefault(f["cls"], []).append(f)
    for cls in sorted(by):
        fs = by[cls]
        first = fs[0]
        out.append("%-12s %3d  first: walk %d step %d %s: %s" %
                   (cls, len(fs), first["walk"], first["step"], first["op"], first["detail"]))
        out.append("%-12s      keys: %s" % ("", " ".join(first["keys"][-12:])))
    ms = [m for w in walks for m in w.op_ms]
    starts = [m for w in walks for m in w.start_ms]
    if ms:
        ms_sorted = sorted(ms)
        p95 = ms_sorted[int(0.95 * (len(ms_sorted) - 1))]
        out.append("ops %d  ms per op: median %.2f  p95 %.2f  max %.2f" %
                   (len(ms), statistics.median(ms), p95, max(ms)))
    if starts:
        out.append("sessions %d  ms per start: median %.1f  max %.1f" %
                   (len(starts), statistics.median(starts), max(starts)))
    out.append("undo probes %d" % sum(w.probes for w in walks))
    return "\n".join(out)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--seed", type=int, action="append", default=None)
    ap.add_argument("--ops", type=int, default=100)
    ap.add_argument("--undo-probe", type=float, default=0.3)
    ap.add_argument("--readonly-seed", type=int, action="append", default=[],
                    help="seeds whose walk runs in a read-only file")
    ap.add_argument("--tmp", default=None, help="where the sessions live")
    args = ap.parse_args(argv)
    seeds = args.seed or [1, 2, 3]
    base = tempfile.mkdtemp(dir=args.tmp)
    walks = []
    for seed in seeds:
        w = Walk(base, seed, args.ops, args.undo_probe,
                 readonly=seed in args.readonly_seed).run()
        walks.append(w)
        for f in w.findings:
            print("walk %d step %3d %-22s %-11s %s" % (f["walk"], f["step"], f["op"],
                                                    f["cls"], f["detail"]))
    print(summarize(walks))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
