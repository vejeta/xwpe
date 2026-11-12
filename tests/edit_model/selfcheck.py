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
   and the undo lists stay as they were, and the status row says so.
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
        # a mark is never past its row's end: just after a line break is
        # the start of the next row (the owner keeps it there after a key)
        if not _pos_ok(y, x, rows, False):
            bad.append(("marks", "%s (%d,%d) outside the text" % (name, y, x)))
    for r in w.brp:
        if r < 0 or r >= len(rows):
            bad.append(("breakpoints", "breakpoint row %d outside %d rows" % (r, len(rows))))
    if len(set(w.brp)) != len(w.brp):
        bad.append(("breakpoints", "a breakpoint row twice: %s" % w.brp))
    return bad


# The dump's rows= when the editor's own check found every row in shape.
ROWS_FIT = -1


def check_rows_fit(w):
    """Check 3b on one window: no row holds more than Max. Columns bytes
    (help.xwpe_eng 1578-1580: after that many columns a word wrap is
    performed), and the editor's own check of its rows agrees."""
    bad = []
    for y, (end, data) in enumerate(_rows(w)):
        if len(data) > w.mx:
            bad.append(("rows", "row %d holds %d bytes, Max. Columns is %d"
                        % (y, len(data), w.mx)))
    if getattr(w, "rows", ROWS_FIT) != ROWS_FIT:
        bad.append(("rows", "the editor found row %d out of shape" % w.rows))
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


def check_lock(kind, before, after):
    """Check 2's first half: a key in a locked window changes no row and
    pushes no undo step (Undo and Redo themselves are refused elsewhere)."""
    if not before.ro or kind in UNDO_KINDS + REDO_KINDS:
        return []
    if _rows(before) != _rows(after) or after.ud != before.ud:
        return [("lock", "a locked window changed: %d -> %d rows, ud %d -> %d"
                 % (len(before.lines), len(after.lines), before.ud, after.ud))]
    return []


# Kinds that change text: in a locked window each must be refused, and the
# refusal must say so on the status row.
TEXT_CHANGING_KINDS = (
    "type_ascii", "type_space", "type_utf8", "enter", "tab", "backspace",
    "delete", "ctrl_t_delete_word", "ctrl_z_delete_to_eol",
    "ctrl_y_delete_line", "ctrl_k_c", "ctrl_k_v", "ctrl_k_y", "ctrl_k_i",
    "ctrl_k_u", "ctrl_o_y", "ctrl_o_t", "ctrl_o_e", "ctrl_o_u", "ctrl_o_m",
    "ctrl_o_h", "cut", "paste", "ctrl_del_delete", "undo", "redo")


def status_row(disp):
    """The bottom row of the screen, where the editor says what it did not
    do."""
    return disp[-1].rstrip() if disp else ""


def check_says_so(kind, before, after, status_before, status_after, idle=None):
    """Check 2's second half: a change the editor refused in a locked window
    is named on the status row, so the key does not vanish in silence.  A
    refusal is a text-changing key after which the rows are as they were.
    @idle is the status row of a session that has refused nothing yet: a
    row that already differs from it when the key comes, and is still there
    after it, is the first refusal's words still showing, so a second
    refusal in a row is not silent."""
    if not before.ro or kind not in TEXT_CHANGING_KINDS:
        return []
    if _rows(before) != _rows(after):
        return []
    if status_after == status_before and (idle is None or status_after == idle):
        return [("says-so", "%s refused in a locked window and the status row "
                 "still reads %r" % (kind, status_after[:40]))]
    return []


def check_utf8_stays_valid(before_lines, after_lines):
    """The soft rule (9): text that was valid UTF-8 is still valid after a
    key.  Reports what the key did to a character: a lone continuation or
    lead byte, as Backspace or Ctrl-T leave after an overwrite of a
    multibyte letter.  [("encoding", detail)]."""
    def disk(lines):
        out = bytearray()
        for end, data in lines:
            out += data
            if end == "H":
                out += b"\n"
        return bytes(out)

    try:
        disk(before_lines).decode("utf-8")
    except UnicodeDecodeError:
        return []                       # not valid before: nothing to keep
    after = disk(after_lines)
    try:
        after.decode("utf-8")
    except UnicodeDecodeError as e:
        return [("encoding", "valid UTF-8 became invalid at byte %d: %r"
                 % (e.start, after[max(0, e.start - 4):e.start + 4]))]
    return []


def check_disk(kind, disk, after):
    """Check 7's second half: after Save, the file is the dump's text."""
    if kind != "save":
        return []
    if disk != after.disk():
        return [("disk", "Save wrote %r, the dump holds %r"
                 % (disk[-60:] if disk else disk, after.disk()[-60:]))]
    return []


def check_autosave(autosaved, asv, after):
    """Check 5's second half: an autosave's .ASV file holds the dump's text."""
    if autosaved and asv != after.disk():
        return [("autosave", ".ASV differs from the dump")]
    return []


def check_no_step(before, after, n_steps):
    """Check 6, the undo probe's guard: a change that pushed no undo step
    cannot be undone."""
    if n_steps <= 0 and _rows(before) != _rows(after):
        return [("no-step", "the text changed and no undo step was pushed")]
    return []


# Which record kinds mark the span a replay changes ('a' and 'r' are the
# character records); a block copy's 'c' leaves both marks where the copy
# stood; every other kind gives the marks back as they were.
SPAN_KINDS = ("a", "r")
COPY_KIND = "c"


def record_kinds(trace):
    """The kinds of the fresh records an operation pushed, in order, from
    the trace's ``buffer record kind=<k> ... replay=none`` lines."""
    out = []
    for t in trace:
        if t.startswith("buffer record kind=") and "replay=none" in t:
            out.append(t[len("buffer record kind="):][:1])
    return out


def _model_rows(w):
    from edit_model import model
    return [model.Row(data, end, 0) for end, data in w.lines]


def expected_marks(direction, kind, want, other):
    """The block marks a Ctrl-U or Ctrl-R should leave (the table the
    design calls MARKS_AFTER_UNDO), as ``(begin, end)``, or None when the
    kind's rule is not written down.  @want is the dump the replay returns
    to; @other the dump it came from."""
    from edit_model import model
    mine = (want.mb, want.me)
    if kind in SPAN_KINDS:
        want_rows, other_rows = _model_rows(want), _model_rows(other)
        st = model.State()
        st.rows = want_rows
        model._marks_on_inserted(st, other_rows, want_rows, mine)
        return (st.mb, st.me)
    if kind == COPY_KIND and direction == "undo":
        st = model.State()
        st.rows = _model_rows(want)
        sp = model._span(st.rows, _model_rows(other))
        if sp is None:
            return None
        text, starts = model._text_offsets(st.rows)
        pt = model._offset_point(st.rows, starts, sp[0])
        return (pt, pt)
    if kind == COPY_KIND:
        return mine
    return mine


def compare_windows(when, want, got, marks=None):
    """Check 1: the dump an undo probe gets back against the one it should
    (text, breakpoint rows, cursor, and the marks when @marks says what
    they should be), and the result inside the text."""
    if got is None:
        return [("text", "%s no text window" % when)]
    bad = []
    if _rows(want) != _rows(got):
        bad.append(("text", "%s: %s" % (when, _row_diff(_rows(want), _rows(got)))))
    if want.brp != got.brp:
        bad.append(("breakpoints", "%s: rows %s, expected %s" % (when, got.brp, want.brp)))
    if want.cursor != got.cursor:
        bad.append(("cursor", "%s: cursor %s, expected %s" % (when, got.cursor, want.cursor)))
    if marks is not None and (got.mb, got.me) != marks:
        bad.append(("marks", "%s: marks %s, expected %s" % (when, (got.mb, got.me), marks)))
    for cls, detail in check_inside(got):
        bad.append((cls, "%s: %s" % (when, detail)))
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
        self.last_kinds = []
        self.status_before = ""
        self.idle_status = None
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
        self.idle_status = status_row(s.display())
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
        self.status_before = status_row(s.display())
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
        for cls, detail in check_rows_fit(after):
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
        self.last_kinds = record_kinds(trace)
        found = check_lock(kind, before, after)
        found += check_says_so(kind, before, after, self.status_before, status_row(disp),
                               self.idle_status)
        found += check_utf8_stays_valid(_rows(before), _rows(after))
        if kind == "save":
            found += check_disk(kind, s.disk(), after)
        if s.last.autosaved:
            found += check_autosave(True, s.disk(FILENAME + ".ASV"), after)
        for cls, detail in found:
            self.note(i, kind, cls, detail)

    def undo_probe(self, i, kind, before, after):
        """Check 1: Ctrl-U as many times as the op pushed steps (its fresh
        records: the undo list stops growing at its bound) gives the dump
        before it back, and Ctrl-R as many gives the dump after it."""
        if after.ro or kind in UNDO_KINDS + REDO_KINDS or literal_mode(after):
            return
        n = min(self.last_fresh, after.ud)
        if n <= 0:
            for cls, detail in check_no_step(before, after, n):
                self.note(i, kind, cls, detail)
            return
        if self.last_fresh > after.ud:
            return                  # the bound dropped steps of this very op
        self.probes += 1
        # The marks rule is written for one record; a step of several keeps
        # only the text, cursor and breakpoint checks.
        record = self.last_kinds[0] if n == 1 and len(self.last_kinds) == 1 else None
        try:
            u = self.send([UNDO] * n).win
            self.session.trace_since_last()
            self.compare(i, kind, "after Ctrl-U", before, u,
                         None if record is None else expected_marks("undo", record, before, after))
            r = self.send([REDO] * n).win
            self.session.trace_since_last()
            self.compare(i, kind, "after Ctrl-R", after, r,
                         None if record is None else expected_marks("redo", record, after, before))
        except (ChannelTimeout, ChannelMiscount) as e:
            self.note(i, kind, "liveness", "undo probe: %s" % e)
            self.start()

    def compare(self, i, kind, when, want, got, marks=None):
        for cls, detail in compare_windows(when, want, got, marks):
            self.note(i, kind, cls, detail)

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
    ap.add_argument("--tmp", default=None,
                    help="where the sessions live; the directory is removed at the end")
    args = ap.parse_args(argv)
    seeds = args.seed or [1, 2, 3]
    walks = []
    with tempfile.TemporaryDirectory(dir=args.tmp) as base:
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
