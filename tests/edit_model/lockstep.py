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
import os
import random
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
if os.path.dirname(HERE) not in sys.path:
    sys.path.insert(0, os.path.dirname(HERE))

from edit_model import keymap, model  # noqa: E402
from edit_model.oracle import (ESC, ChannelMiscount, ChannelTimeout,  # noqa: E402
                               EditSession)
from edit_model.render import render_disk, screen_disagreements  # noqa: E402

FILENAME = "t.c"
DEFAULT_STEPS = 50
# A row this long is two bytes short of the default width of 120, so the
# next characters typed into it wrap.
NEAR_WIDTH_ROW = " ".join("w%03d" % i for i in range(23)) + "xy"
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

# What a walk draws, and how often.  Typing and the deletions most; the
# moves that put the cursor at the hot spots (row ends, the long row, the
# text's ends) next; the keys this terminal may not decode least.
WALK_WEIGHTS = {
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
# What "type" draws: letters, a blank and a dash (the wrap points), braces
# and a hash (auto-indent's quirks), a multibyte letter.
TYPED = [b"x", b"q", b"a", b"Z", b" ", b" ", b"-", b"{", b"}", b"#", "é".encode()]
# In Alt-J's mode only these: printable letters, control keys that then go
# into the text, and Alt-J again (often, to leave).
LITERAL_TYPED = [b"x", b"q", b"\x14", b"\x19", b"\x1a"]
LITERAL_LEAVE_P = 0.4

CLASS_EDITOR = "editor"
CLASS_MODEL = "model"
CLASS_UNSPECIFIED = "unspecified"

# How the screen is compared: every this many steps.
SCREEN_EVERY = 10


class Disagreement(dict):
    """One field where the editor and the model differ after one action."""


def _is_overwrite_step(step):
    return step is not None and step.action.name == "type" and not (step.ins & model.INS_INSERT)


def _at_end_of_text(st):
    return st.y == len(st.rows) - 1 and st.x >= len(st.rows[-1].data)


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
KNOWN_OVERWRITE_REDO = "known bug, fix pending: Ctrl-R after undoing overwrite typing"
KNOWN_END_OF_TEXT = "known bug, fix pending: Ctrl-T / Ctrl-Right at the end of the text"
KNOWN_STEPS = "decided, not built yet: one Ctrl-U per Tab or Backspace in an indent"
KNOWN_UNDO_CURSOR = "open question: where Ctrl-U and Ctrl-R leave the cursor"


def expected_tag(action, field, before, pred, editor_w):
    """The known name of a disagreement (a bug whose fix is pending, or a
    decision not built yet), or None for a new one."""
    name = action.name
    # Undoing overwrite typing gives the text back but leaves nothing to
    # redo: only the undo depths carry the known bug, so a wrong text or
    # count after that Ctrl-U still shows as new.
    if name == "undo" and field == model.F_STEPS \
            and _is_overwrite_step(before.undo[-1] if before.undo else None):
        return KNOWN_OVERWRITE_REDO
    if name == "redo" and _is_overwrite_step(before.redo[-1] if before.redo else None):
        return KNOWN_OVERWRITE_REDO
    if name in ("delete_word", "word_right") and _at_end_of_text(before):
        return KNOWN_END_OF_TEXT
    if field == model.F_STEPS and name in ("tab", "backspace"):
        if editor_w.ud - before.ud > 1:
            return KNOWN_STEPS
    if name in ("undo", "redo") and field in (model.F_CURSOR, model.F_ANCHOR):
        return KNOWN_UNDO_CURSOR
    return None


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


def compare(pred, d, session, action, check_screen):
    """[(field, model's value, editor's value)] for one action."""
    w = d.win
    out = []

    def differ(field, mine, theirs):
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
    differ(model.F_BREAKPOINTS, pred.brp_rows(), list(w.brp))
    differ(model.F_AUTOSAVE, pred.autosaved, bool(d.autosaved))
    if d.autosaved and pred.autosaved:
        differ(model.F_AUTOSAVE, pred.asv, session.disk(FILENAME + ".ASV"))
    if action.name == "save":
        differ(model.F_DISK, pred.disk, session.disk())
    if check_screen:
        bad = screen_disagreements(w.lines, w.view, pred.options.tabn, session.display())
        if bad:
            out.append(("screen", "the dump's rows", bad[:2]))
    return out


class Walk:
    """One seeded lockstep walk under one key style."""

    def __init__(self, base, style, seed, n_steps=DEFAULT_STEPS, seed_text=SEED,
                 binary=None):
        self.base = base
        self.style = style
        self.seed = seed
        self.n_steps = n_steps
        self.seed_text = seed_text
        self.binary = binary
        self.rng = random.Random("%s-%d" % (style, seed))
        self.disagreements = []
        self.lost = []
        self.history = []
        self.steps_run = 0
        self.state = None
        self.kinds = sorted(WALK_WEIGHTS)
        self.weights = [WALK_WEIGHTS[k] for k in self.kinds]

    def draw(self, st):
        """(keymap action, typed bytes or None) for the next step."""
        if st.ins & model.INS_LITERAL:
            if self.rng.random() < LITERAL_LEAVE_P:
                return "literal_toggle", None
            return "type", self.rng.choice(LITERAL_TYPED)
        name = self.rng.choices(self.kinds, self.weights)[0]
        if name == "type":
            return name, self.rng.choice(TYPED)
        return name, None

    def run(self):
        rc = keymap.xwperc_for(self.style)
        wd = tempfile.mkdtemp(dir=self.base)
        with EditSession(wd, self.seed_text, filename=FILENAME, xwperc=rc,
                         binary=self.binary) as s:
            st = model.state_from_window(s.last.win, model.Options.from_xwperc(rc))
            for i in range(self.n_steps):
                if not self.step(s, st, i):
                    break
                st = self.state
        return self

    def step(self, s, st, i):
        name, arg = self.draw(st)
        action = model.Action(keymap.model_action(name), arg)
        keys = keymap.keys_for(self.style, name, arg)
        st.view = s.last.win.view
        pred = model.apply(st, action)
        self.history.append(name if arg is None else "type %r" % arg)
        try:
            d = s.send(keys)
        except (ChannelTimeout, ChannelMiscount) as e:
            self.lost.append((i, name, str(e)))
            return False
        self.steps_run += 1
        box = None
        if d.ctx != "edit" or d.win is None or d.win.name != FILENAME:
            box = box_text(s.display())
            ctx = d.ctx
            d = self.back_to_text(s)
            if d is None:
                self.lost.append((i, name, "ctx=%s after the action: %s" % (ctx, box)))
                return False
        if pred.box != _box_named(box, pred.box):
            self.note(i, action, model.F_BOX, pred.box, box, st, pred, d.win)
        found = check_typed_bytes_kept(st, action, d.win)
        found += compare(pred, d, s, action, i % SCREEN_EVERY == 0)
        for field, mine, theirs in found:
            self.note(i, action, field, mine, theirs, st, pred, d.win)
        if any(x["step"] == i for x in self.disagreements):
            base = action.name.replace("shift_", "")
            model.resync(pred, d.win, vertical=base in ("up", "down"))
        self.state = pred
        return True

    def back_to_text(self, s):
        """Esc until the editor reads keys in the file's window again; the
        dump of that moment, or None."""
        for _ in range(LEAVE_ESC_MAX):
            d = s.send([ESC])
            if d.ctx == "edit" and d.win is not None and d.win.name == FILENAME:
                return d
        return None

    def note(self, i, action, field, mine, theirs, before, pred, w):
        if field == "screen":
            cls, rules = CLASS_EDITOR, []
        elif field == FIELD_TYPED_KEPT:
            cls, rules = CLASS_EDITOR, [model.RULES["type_insert"]]
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
            keys=list(self.history[-8:])))


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
        out.append("    keys   %s  (cursor before %s; %s seed %d step %d)"
                   % (" ; ".join(first["keys"]), first["cursor_before"],
                      first["style"], first["seed"], first["step"]))
        if first["rules"]:
            out.append("    rules  %s" % "; ".join(first["rules"]))
    for w in walks:
        for lost in w.lost:
            out.append("LOST %s seed %d step %d %s: %s" % ((w.style, w.seed) + lost))
    return "\n".join(out)


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
    ap.add_argument("--tmp", default=None)
    args = ap.parse_args(argv)
    base = tempfile.mkdtemp(dir=args.tmp)
    walks = []
    for style in args.style or list(keymap.STYLES):
        for seed in args.seed or [1]:
            walks.append(Walk(base, style, seed, args.steps).run())
    print(summarize(walks))
    print("steps %d, disagreements %d" % (sum(w.steps_run for w in walks),
                                          sum(len(w.disagreements) for w in walks)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
