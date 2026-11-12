#!/usr/bin/env python3
"""The model beside the editor, one key at a time.

A walk starts ``wpe`` on a seed file under one key style, draws an action,
asks the model what it does, sends its keys, and compares the editor's dump
with the model's prediction field by field: the rows, the cursor, the block
marks, the Shift anchor, the modes, the modified count, the undo depths,
the breakpoint rows, and after Save or an autosave the file itself.

Every difference is a disagreement, classed by where the model's prediction
came from (``model.classify``): ``editor`` when only the manual, the
compatibility chapter or the user's decision predicted the field,
``unspecified`` when the code or the design did.  A disagreement already
known (being fixed elsewhere, or decided) carries its tag.  After a
disagreement the model takes the editor's state and the walk goes on, so
one difference does not hide the next.  Nothing is fixed here.

Run as a script::

    tests/.venv/bin/python tests/edit_model/lockstep.py --style old --seed 1 --steps 50
"""
import argparse
import json
import os
import random
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
if os.path.dirname(HERE) not in sys.path:
    sys.path.insert(0, os.path.dirname(HERE))

from edit_model import keymap, model, option_row, selfcheck, walk  # noqa: E402
from edit_model.oracle import (ESC, ChannelMiscount, ChannelTimeout,  # noqa: E402
                               EditSession)
from edit_model.render import render_disk, screen_disagreements  # noqa: E402

FILENAME = "t.c"
DEFAULT_STEPS = 50
# A row this long is two bytes short of the default width of 120, so the
# next characters typed into it wrap.
NEAR_WIDTH_ROW = option_row.NEAR_WIDTH_ROW
SEED = ("#include <stdio.h>\n"
        "int main(void)\n"
        "{\n"
        "    int a = 1;\n"
        "\tif (a) {\n"
        "\t\treturn 0;\n"
        "\t}\n"
        "}\n"
        + NEAR_WIDTH_ROW + "\n"
        "café au lait\n"
        "   \n"
        "one-two three\n"
        "last line")
# What the walk of one key style (no row of the array) draws: the key loop's
# own edits and moves, the modes, Save, Undo and Redo.  Blocks and the rest
# are the array's walks (walk.WEIGHTS).
LEGACY_WEIGHTS = {
    "type": 24, "enter": 7, "tab": 4, "backspace": 9, "delete": 7,
    "delete_word": 3, "kill_to_eol": 2, "delete_line": 2,
    "left": 3, "right": 3, "up": 4, "down": 5, "home": 3, "end": 5,
    "file_start": 1, "file_end": 2, "page_up": 1, "page_down": 1,
    "window_top": 1, "window_bottom": 1, "word_left": 1, "word_right": 1,
    "shift_left": 1, "shift_right": 1, "shift_down": 1, "shift_end": 1,
    "file_end_primary": 1, "window_top_primary": 1,
    "insert_toggle": 1, "literal_toggle": 1,
    "undo": 6, "redo": 3, "save": 2,
}
# The keys of the programming layer that only the breakpoint prelude uses.
PRELUDE_ONLY = {"toggle_breakpoint": option_row.BREAKPOINT_KEYS}

CLASS_EDITOR = "editor"
CLASS_MODEL = "model"
CLASS_UNSPECIFIED = "unspecified"

# How the screen is compared: every this many steps.
SCREEN_EVERY = 10


class Disagreement(dict):
    """One field where the editor and the model differ after one action."""


# The manual's rule a move breaks when its key does nothing at all.
_MOVE_RULE = {model.F_CURSOR: "cursor_keys", model.F_ANCHOR: "cursor_keys",
              model.F_MARKS: "shift_marks"}


def _key_did_nothing(action, field, before, w):
    """A cursor key the manual names that left the cursor and the block
    where they were, where the model moved them: whatever the detail of the
    move, the manual alone says the key moves."""
    base = action.name[len("shift_"):] if action.name.startswith("shift_") else action.name
    if base not in model.MOVES or field not in _MOVE_RULE:
        return False
    if field == model.F_MARKS:
        return (w.mb, w.me) == (before.mb, before.me) and w.cursor == (before.y, before.x)
    return w.cursor == (before.y, before.x)


# Disagreements already known, by the name a report gives them.
KNOWN_STEPS = ("decided, not built yet: one Ctrl-U per Tab or Backspace in an "
               "indent, Ctrl-K I/U, Ctrl-O U/M/H and Change All")
KNOWN_UNDO_CURSOR = "open question: where Ctrl-U and Ctrl-R leave the cursor"
KNOWN_SOFT_WRAP = "known bug, fix pending: typing into a soft-wrapped row loses a byte"
KNOWN_UTF8_BACKSPACE = ("known bug, fix pending: Backspace with auto-indent deletes one "
                        "byte of a UTF-8 character")
# What the editor pushes as several steps today, where the user's decision is
# one.  Ctrl-U or Ctrl-R on those steps is the walk's to take from the editor
# (model.undo_unmodelled_step); the tag names the push itself.
STEP_ACTIONS = ("tab", "backspace", "block_indent", "block_unindent", "help_button",
                "help_highlight", "help_header", "replace_all")


def expected_tag(action, field, before, pred, editor_w):
    """The known name of a disagreement (a bug whose fix is pending, or a
    decision not built yet), or None for a new one.  A cursor or a mark
    outside the text is never excused by a known bug: that check reads the
    editor's dump alone, and the open question about where Undo leaves the
    cursor is not offered for a cursor that is not in the text."""
    name = action.name
    if field == FIELD_INSIDE:
        return None
    if field == model.F_STEPS and name in STEP_ACTIONS:
        if editor_w.ud - before.ud > 1:
            return KNOWN_STEPS
    if name in ("undo", "redo") and field in (model.F_CURSOR, model.F_ANCHOR) \
            and not _cursor_outside(editor_w):
        return KNOWN_UNDO_CURSOR
    if name == "type" and before.rows[before.y].end == model.SOFT \
            and field in (model.F_TEXT, FIELD_TYPED_KEPT, model.F_CURSOR,
                          model.F_SAVE, model.F_MARKS) \
            and _bytes_lost(pred, editor_w):
        return KNOWN_SOFT_WRAP
    if name == "backspace" and before.flg & model.FLG_AUTOINDENT \
            and _multibyte_just_before_cursor(before):
        return KNOWN_UTF8_BACKSPACE
    return None


def _cursor_outside(w):
    """Is the editor's cursor outside the editor's own text?"""
    return any(cls == "cursor" for cls, _detail in selfcheck.check_inside(w))


def _bytes_lost(pred, w):
    """Does the editor hold fewer bytes than the model predicted (what the
    soft-wrap typing bug does), rather than different ones?"""
    return sum(len(d) for _e, d in w.lines) < sum(len(r.data) for r in pred.rows)


def _multibyte_just_before_cursor(before):
    """Is the character Backspace takes a multibyte one (the auto-indent
    Backspace bug), not merely one somewhere earlier on the row?"""
    if not 0 <= before.y < len(before.rows) or before.x <= 0:
        return False
    return before.rows[before.y].data[before.x - 1] >= 0x80


def logical_offset(lines, y, x):
    """Where (y, x) falls in the text read as one string."""
    return sum(len(data) + (1 if end == "H" else 0) for end, data in lines[:y]) + x


def check_typed_bytes_kept(before, action, w):
    """The manual's promise for typing in insert mode, checked on the
    editor's dumps alone: the text after the key is the text before it
    with the typed bytes at the cursor.  A wrap may move bytes between
    rows but never drop one.  [(field, wanted, got)]."""
    if action.name != "type" or not (before.ins & model.INS_INSERT) \
            or before.ins == model.INS_READONLY:
        return []
    old = render_disk(before.lines())
    off = logical_offset(before.lines(), before.y, before.x)
    want = old[:off] + action.arg + old[off:]
    got = render_disk(list(w.lines))
    if got != want:
        return [(FIELD_TYPED_KEPT, _around(want, off), _around(got, off))]
    return []


FIELD_TYPED_KEPT = "typed-kept"
AROUND = 30


def _around(text, off):
    return text[max(0, off - AROUND):off + AROUND]


FIELD_UTF8 = "utf8-valid"
FIELD_INSIDE = "inside"


def compare(pred, d, session, action, check_screen, filename=FILENAME):
    """[(field, model's value, editor's value)] for one action; the fields
    the action says it does not predict are left out."""
    w = d.win
    out = []

    def differ(field, mine, theirs):
        if field in pred.resync_fields:
            return
        if mine != theirs:
            out.append((field, mine, theirs))

    differ(model.F_TEXT, pred.lines(), list(w.lines))
    differ(model.F_CURSOR, (pred.y, pred.x), w.cursor)
    differ(model.F_MARKS, (pred.mb, pred.me), (w.mb, w.me))
    differ(model.F_ANCHOR, pred.ks, w.ks)
    differ(model.F_MODE, (pred.ins, pred.flg), (w.ins, w.flg))
    if pred.save_known:
        differ(model.F_SAVE, pred.save, w.save)
    differ(model.F_STEPS, (pred.ud, pred.rd), (w.ud, w.rd))
    # the breakpoints are a set of rows: the editor keeps them in the order
    # they were set or given back, which says nothing about where they are
    differ(model.F_BREAKPOINTS, pred.brp_rows(), sorted(w.brp))
    differ(model.F_CLIP, list(pred.clip), model.clipboard_rows(d.windows.get(0)))
    differ(model.F_MARKERS, list(pred.pt), [model._dump_marker(p) for p in w.pt])
    if pred.save_known:         # Undo and Redo add the replayed characters to the count
        differ(model.F_AUTOSAVE, pred.autosaved, bool(d.autosaved))
    if d.autosaved and pred.autosaved:
        differ(model.F_AUTOSAVE, pred.asv, session.disk(filename + ".ASV"))
    if action.name == "save":
        differ(model.F_DISK, pred.disk, session.disk())
    if check_screen:
        bad = screen_disagreements(w.lines, w.view, pred.options.tabn, session.display(),
                                   pred.options.endmark)
        if bad:
            out.append(("screen", "the dump's rows", bad[:2]))
    return out


def status_row(display):
    """The words on the status row, the last row of the screen."""
    return display[-1].strip() if display else ""


def status_disagreements(pred, status, previous):
    """The status row against the model: a refusal says so, and a status row
    that says the text is unchanged after a key the model did not refuse is a
    finding too (unless the previous key left it there, which the next key
    that redraws the row clears).  [(field, wanted, got)]."""
    if pred.status is not None:
        if pred.status not in status:
            return [(model.F_STATUS, pred.status, status)]
        return []
    if previous is None and model.STATUS_READ_ONLY in status:
        return [(model.F_STATUS, "no refusal said", status)]
    return []


def model_free_checks(before, w):
    """What the editor's own dumps alone can say about one step, with no
    model: the cursor and the marks inside the text, and the soft rule that
    text which was valid UTF-8 stays valid.  [(field, wanted, got)]."""
    out = []
    for cls, detail in selfcheck.check_inside(w):
        out.append((FIELD_INSIDE, "inside the text", detail))
    for cls, detail in selfcheck.check_utf8_stays_valid(before.lines(), list(w.lines)):
        out.append((FIELD_UTF8, "valid UTF-8", detail))
    return out


class Walk:
    """One seeded lockstep walk: under one key style, or on one row of the
    option array (``row``), which fixes the options, the file and the
    prelude that brings the session to the row's state."""

    def __init__(self, base, style, seed, n_steps=DEFAULT_STEPS, seed_text=SEED,
                 binary=None, row=None, weights=None, coverage=None):
        self.base = base
        self.row = row
        self.style = row.style if row is not None else style
        self.seed = seed
        self.n_steps = n_steps
        self.seed_text = row.seed_text if row is not None else seed_text
        self.filename = row.filename if row is not None else FILENAME
        self.binary = binary
        if row is None:
            # the walk of one key style keeps the draws it had before the
            # array: the same seed is the same walk
            self.rng = random.Random("%s-%d" % (self.style, seed))
            weights = weights or LEGACY_WEIGHTS
        else:
            self.rng = random.Random("%s-%s-%d" % (row.name, self.style, seed))
        self.drawer = walk.Drawer(self.rng, weights)
        self.coverage = coverage if coverage is not None else walk.Coverage()
        self.levels = row.levels() if row is not None else {}
        self.disagreements = []
        self.lost = []
        self.history = []
        self.steps_run = 0
        self.state = None
        self.key_name = None
        self.prev_status = None
        # the levels the session had when its prelude was done; None until
        # then (a row whose prelude broke off never got there)
        self.start_observed = None
        self.start_mismatch = None

    def keys_of(self, name, arg):
        if name in PRELUDE_ONLY:
            return list(PRELUDE_ONLY[name])
        return keymap.keys_for(self.style, name, arg)

    def run(self):
        rc = self.row.xwperc() if self.row is not None else keymap.xwperc_for(self.style)
        mode = self.row.file_mode if self.row is not None else None
        wd = tempfile.mkdtemp(dir=self.base)
        try:
            with EditSession(wd, self.seed_text, filename=self.filename, xwperc=rc,
                             binary=self.binary, file_mode=mode) as s:
                st = model.state_from_window(s.last.win, model.Options.from_xwperc(rc),
                                             clip=s.last.windows.get(0))
                self.state = st
                prelude = self.row.prelude() if self.row is not None else []
                i = 0
                for name, arg in prelude:
                    if not self.step(s, self.state, i, (name, arg)):
                        return self
                    i += 1
                if self.row is not None:
                    self.check_row_start(s)
                for _ in range(self.n_steps):
                    if not self.step(s, self.state, i):
                        break
                    i += 1
        finally:
            shutil.rmtree(wd, ignore_errors=True)
        return self

    def check_row_start(self, s):
        """Read the levels the session really has now that the prelude is done
        (from the editor's dump and screen) and keep what differs from the
        row's labels: a row that did not get its label tests another state
        than the array says it does."""
        d = s.last
        if d.win is None:
            self.start_mismatch = [("window", "the file's window", d.ctx)]
            return
        self.start_observed = option_row.observed_levels(d.win, d.windows.get(0), s.display())
        self.start_mismatch = option_row.start_mismatches(self.row, self.start_observed)

    def step(self, s, st, i, forced=None):
        name, arg = forced if forced is not None else self.drawer.next(st)
        action = model.Action(keymap.model_action(name) if name not in PRELUDE_ONLY
                              else "toggle_breakpoint", arg)
        keys = self.keys_of(name, arg)
        st.view = s.last.win.view
        pred = model.apply(st, action)
        self.history.append(name if arg is None else "%s %r" % (name, arg))
        self.coverage.add(name, self.levels)
        try:
            d = s.send(keys)
        except (ChannelTimeout, ChannelMiscount) as e:
            self.lose(i, name, str(e), st, s)
            return False
        self.steps_run += 1
        box = None
        status = status_row(s.display())
        if d.ctx != "edit" or d.win is None or d.win.name != self.filename:
            box = box_text(s.display())
            ctx = d.ctx
            d = self.back_to_text(s)
            if d is None:
                self.lose(i, name, "ctx=%s after the action: %s" % (ctx, box), st, s)
                return False
        if pred.box != _box_named(box, pred.box):
            self.note(i, action, model.F_BOX, pred.box, box, st, pred, d.win)
        found = check_typed_bytes_kept(st, action, d.win)
        found += compare(pred, d, s, action, i % SCREEN_EVERY == 0, self.filename)
        found += model_free_checks(st, d.win)
        found += status_disagreements(pred, status, self.prev_status)
        self.prev_status = pred.status
        for field, mine, theirs in found:
            self.note(i, action, field, mine, theirs, st, pred, d.win)
        here = [x["field"] for x in self.disagreements if x["step"] == i]
        if pred.resync_fields or here:
            base = action.name.replace("shift_", "")
            rebase = (model.F_TEXT in pred.resync_fields or model.F_TEXT in here
                      or model.F_STEPS in here)
            model.resync(pred, d.win, vertical=base in ("up", "down"),
                         clip=d.windows.get(0), rebase_steps=rebase)
        self.state = pred
        return True

    def lose(self, i, name, message, st, s):
        """Record that the editor stopped answering at step @i.  No crash is
        known: every loss is the walk's to report as new."""
        self.lost.append((i, name, message))

    def new_losses(self):
        """The losses no known bug explains: all of them, since none is
        known."""
        return list(self.lost)

    def back_to_text(self, s):
        """Esc until the editor reads keys in the file's window again; the
        dump of that moment, or None."""
        for _ in range(LEAVE_ESC_MAX):
            d = s.send([ESC])
            if d.ctx == "edit" and d.win is not None and d.win.name == self.filename:
                return d
        return None

    def note(self, i, action, field, mine, theirs, before, pred, w):
        if field == "screen":
            cls, rules = CLASS_EDITOR, []
        elif field == FIELD_TYPED_KEPT:
            cls, rules = CLASS_EDITOR, [model.RULES["type_insert"]]
        elif field in (FIELD_UTF8, FIELD_INSIDE):
            cls, rules = CLASS_UNSPECIFIED, []
        elif field == "context":
            cls, rules = model.classify(pred.fired, model.F_TEXT)
        elif _key_did_nothing(action, field, before, w):
            cls, rules = CLASS_EDITOR, [model.RULES[_MOVE_RULE[field]]]
        else:
            cls, rules = model.classify(pred.fired, field)
        self.disagreements.append(Disagreement(
            style=self.style, seed=self.seed, step=i, action=repr(action),
            field=field, model=mine, editor=theirs, cls=cls,
            rules=["%s (%s, %s)" % (r.name, r.source, r.ref) for r in rules],
            expected=expected_tag(action, field, before, pred, w),
            cursor_before=(before.y, before.x),
            row=self.row.name if self.row is not None else "-",
            keys=list(self.history[-8:])))

    def result(self):
        """The walk as plain data, for a run's report file."""
        return {"row": self.row.name if self.row is not None else "-",
                "levels": self.levels,
                "start": {"checked": self.start_mismatch is not None,
                          "mismatch": _jsonable(self.start_mismatch or []),
                          "observed": _jsonable(self.start_observed or {})},
                "style": self.style, "seed": self.seed, "steps": self.steps_run,
                "lost": [list(x) for x in self.lost],
                "disagreements": [dict((k, _jsonable(v)) for k, v in x.items())
                                  for x in self.disagreements]}


def _jsonable(v):
    if isinstance(v, bytes):
        return v.decode("latin-1")
    if isinstance(v, (list, tuple)):
        return [_jsonable(x) for x in v]
    if isinstance(v, dict):
        return dict((str(k), _jsonable(x)) for k, x in v.items())
    if isinstance(v, (int, float, str, bool)) or v is None:
        return v
    return repr(v)


def summarize(walks):
    """The disagreements of some walks, grouped by class, action and field,
    with the first example of each, as text."""
    groups = {}
    for w in walks:
        for x in w.disagreements:
            act = x["action"].split("(")[0]
            groups.setdefault((x["cls"], act, x["field"], x["expected"]), []).append(x)
    out = []
    for key in sorted(groups, key=lambda k: (k[0], k[1], k[2], str(k[3]))):
        xs = groups[key]
        first = xs[0]
        cls, act, field, exp = key
        out.append("%-11s %-16s %-11s x%-3d %s" % (cls, act, field, len(xs),
                                                  ("expected: " + exp) if exp else "NEW"))
        out.append("    model  %s" % _short(first["model"]))
        out.append("    editor %s" % _short(first["editor"]))
        out.append("    keys   %s  (cursor before %s; row %s %s seed %d step %d)"
                   % (" ; ".join(first["keys"]), first["cursor_before"],
                      first.get("row", "-"), first["style"], first["seed"], first["step"]))
        if first["rules"]:
            out.append("    rules  %s" % "; ".join(first["rules"]))
    for w in walks:
        for lost in w.lost:
            out.append("LOST %s seed %d step %d %s: %s" % ((w.style, w.seed) + lost))
    return "\n".join(out)


def summary_counts(walks):
    """{class: n} over the disagreements of @walks that are not expected,
    and {tag: n} of the expected ones, for a report's header."""
    new, known = {}, {}
    for w in walks:
        for x in w.disagreements:
            if x["expected"]:
                known[x["expected"]] = known.get(x["expected"], 0) + 1
            else:
                new[x["cls"]] = new.get(x["cls"], 0) + 1
    return new, known


def box_text(disp):
    """The words of the box drawn over the file's window: the rows inside
    the first frame that starts below the window's own top border, joined
    (the terminal draws frames with the line-drawing set, which the screen
    reads as ``lqq...k`` and ``mqq...j``)."""
    top = None
    for y in range(BOX_SEARCH_FROM, len(disp) - 1):
        c0 = disp[y].find(BOX_TOP, 1)
        if c0 > 0:
            c1 = disp[y].find("k", c0)
            top = (y, c0, c1)
            break
    if top is None:
        return ""
    y0, c0, c1 = top
    words = [disp[y0][c0:c1].strip("lqk ✕")]
    for y in range(y0 + 1, len(disp) - 1):
        if disp[y][c0:c0 + len(BOX_BOTTOM)] == BOX_BOTTOM:
            break
        words.append(disp[y][c0 + 1:c1].strip())
    return " ".join(w for w in words if w)[:160]


def _box_named(box, want):
    """@want when the box shown names it, else what was shown (None: no box)."""
    if box is not None and want is not None and want in box:
        return want
    return box


# A box's frame as the screen reads it, and the first row below the
# window's own top border.
BOX_TOP = "lqq"
BOX_BOTTOM = "mqq"
BOX_SEARCH_FROM = 2
LEAVE_ESC_MAX = 4


def _short(v, limit=150):
    text = repr(v)
    return text if len(text) <= limit else text[:limit] + "..."


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--style", action="append", choices=keymap.STYLES, default=None)
    ap.add_argument("--seed", type=int, action="append", default=None)
    ap.add_argument("--steps", type=int, default=DEFAULT_STEPS)
    ap.add_argument("--row", action="append", default=None,
                    help="a row of option_rows.tsv by name; with it --style is ignored")
    ap.add_argument("--tmp", default=None,
                    help="where the sessions live; they are removed when the run ends")
    ap.add_argument("--coverage-out", default=None,
                    help="write the coverage of the run as JSON to this path")
    args = ap.parse_args(argv)
    from edit_model import covering
    cov = walk.Coverage()
    walks = []
    with tempfile.TemporaryDirectory(dir=args.tmp) as base:
        if args.row:
            table = dict((r.name, r) for r in covering.load())
            for name in args.row:
                for seed in args.seed or [1]:
                    walks.append(Walk(base, None, seed, args.steps, row=table[name],
                                      coverage=cov).run())
        else:
            for style in args.style or list(keymap.STYLES):
                for seed in args.seed or [1]:
                    walks.append(Walk(base, style, seed, args.steps, coverage=cov).run())
    print(summarize(walks))
    print("steps %d, disagreements %d" % (sum(w.steps_run for w in walks),
                                          sum(len(w.disagreements) for w in walks)))
    if args.coverage_out:
        with open(args.coverage_out, "w", encoding="utf-8") as fh:
            json.dump(cov.to_json(), fh)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
