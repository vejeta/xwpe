#!/usr/bin/env python3
"""Seeded walks over the model's actions, the coverage they leave, and the
size of a run.

A walk is a sequence of actions drawn with fixed weights from a
``random.Random`` seeded by the row and the walk's number, so a failure is
named by its row and its seed and runs again the same.  Typing, Enter and
the deletions are drawn most; blocks and the clipboard next; the cursor
keys that put the cursor on the places that have hidden bugs (the text's
ends, line ends and starts, the block's ends, a marker); Undo and Redo in
runs of one to three and zero to three; and now and then Save, a mode key,
the option dialog and a Change All.

Every step is counted under the action's name and the row's level of every
factor, in a ``Coverage``.  ``report`` prints the matrix of actions against
the levels that were never run and exits 1 for a level some action never
met, so a full run is checked for holes.

A run's size is ``EDIT_MODEL_BUDGET=rows,walks,steps``: how many rows of
the array, how many seeded walks each, how many steps each.
"""
import json
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if os.path.dirname(HERE) not in sys.path:
    sys.path.insert(0, os.path.dirname(HERE))

from edit_model import model, option_row  # noqa: E402

BUDGET_ENV = "EDIT_MODEL_BUDGET"
# The short gate: the first row of each key style, two walks each.
SHORT_BUDGET = (2, 2, 40)
# The full run: every row (0 means all), eight walks of 150 steps.
FULL_BUDGET = (0, 8, 150)

# What a walk draws, and how often.  Cursor kinds that reach the places
# with hidden bugs are listed twice where they matter.
WEIGHTS = {
    "type": 24, "enter": 10, "tab": 3, "backspace": 8, "delete": 7,
    "delete_word": 2, "kill_to_eol": 1, "delete_line": 2,
    "left": 1, "right": 1, "up": 1, "down": 2, "home": 2, "end": 2,
    "file_start": 1, "file_end": 1, "page_up": 1, "page_down": 1,
    "word_left": 1, "word_right": 1,
    "shift_left": 2, "shift_right": 2, "shift_down": 2, "shift_end": 2,
    "shift_up": 2, "shift_home": 1, "shift_word_left": 1, "shift_word_right": 1,
    "shift_page_up": 1, "shift_page_down": 1, "shift_file_start": 1,
    "shift_file_end": 1, "shift_window_top": 1, "shift_window_bottom": 1,
    "block_begin": 2, "block_end": 2, "block_line": 1, "block_all": 1,
    "block_hide": 1, "marker_set": 1, "marker_goto": 1,
    "block_goto_begin": 1, "block_goto_end": 1,
    "block_copy": 2, "block_move": 2, "block_delete": 2, "block_indent": 1,
    "block_unindent": 1, "edit_copy": 1, "edit_cut": 1, "edit_paste": 2,
    "help_button": 1, "help_highlight": 1, "help_header": 1, "help_end": 1,
    "undo_run": 6, "redo": 3,
    "save": 2, "insert_toggle": 1, "literal_toggle": 1,
    "option_set": 1, "replace_all": 1,
}
# What "type" draws: letters, a blank and a dash (the wrap points), braces
# and a hash (auto-indent's quirks), a multibyte letter.
TYPED = [b"x", b"q", b"a", b"Z", b" ", b" ", b"-", b"{", b"}", b"#", "é".encode()]
# In Alt-J's mode only these: letters, control keys that then go into the
# text, and Alt-J again (often, to leave).
LITERAL_TYPED = [b"x", b"q", b"\x14", b"\x19", b"\x1a"]
LITERAL_LEAVE_P = 0.4
UNDO_RUN_MAX = 3
REDO_RUN_MAX = 3
# What a Change All looks for and puts: text the seeds hold, replacements
# that are never empty and never part of what is searched.
SEARCH_TERMS = [b"a", b"e", b"o", b"line", b"the", b"w0"]
REPLACEMENTS = [b"ZZ", b"Q", b"nn-", b"##"]
OPTION_VALUES = {
    "maxcol": (40, 60, 118), "tabn": (1, 2, 4, 8), "maxchg": (5, 20, 999),
    "numundo": (0, 2, 10), "autoindent_col": (0, 2, 3, 5),
}
MARKER_COUNT = model.MARKER_COUNT


class Drawer:
    """The next action of a walk, as ``(keymap name, argument)``."""

    def __init__(self, rng, weights=None):
        self.rng = rng
        w = weights or WEIGHTS
        self.kinds = sorted(w)
        self.weights = [w[k] for k in self.kinds]
        self.queue = []

    def next(self, st):
        """The next step for the state @st (the model's, before the step)."""
        if self.queue:
            return self.queue.pop(0)
        if st.ins & model.INS_LITERAL:
            if self.rng.random() < LITERAL_LEAVE_P:
                return "literal_toggle", None
            return "type", self.rng.choice(LITERAL_TYPED)
        kind = self.rng.choices(self.kinds, self.weights)[0]
        return self.make(kind)

    def make(self, kind):
        rng = self.rng
        if kind == "type":
            return kind, rng.choice(TYPED)
        if kind == "undo_run":
            return self._undo_run()
        if kind == "replace_all":
            return kind, (rng.choice(SEARCH_TERMS), rng.choice(REPLACEMENTS))
        if kind == "option_set":
            field = rng.choice(sorted(OPTION_VALUES))
            return kind, (field, rng.choice(OPTION_VALUES[field]))
        if kind in ("marker_set", "marker_goto"):
            return kind, rng.randrange(MARKER_COUNT)
        return kind, None

    def _undo_run(self):
        """Ctrl-U one to three times, then Ctrl-R up to three: the first now,
        the rest queued."""
        n = self.rng.randint(1, UNDO_RUN_MAX)
        m = self.rng.randint(0, REDO_RUN_MAX)
        self.queue = [("undo", None)] * (n - 1) + [("redo", None)] * m
        return "undo", None


# ---- coverage --------------------------------------------------------------------

class Coverage:
    """How often each action ran under each level of each factor."""

    def __init__(self):
        self.cells = {}          # action -> {"factor=level": count}
        self.steps = 0

    def add(self, action, levels):
        self.steps += 1
        row = self.cells.setdefault(action, {})
        for f, lv in levels.items():
            key = "%s=%s" % (f, lv)
            row[key] = row.get(key, 0) + 1

    def merge(self, other):
        self.steps += other.steps
        for action, cells in other.cells.items():
            row = self.cells.setdefault(action, {})
            for k, n in cells.items():
                row[k] = row.get(k, 0) + n

    def to_json(self):
        return {"steps": self.steps, "cells": self.cells}

    @classmethod
    def from_json(cls, data):
        c = cls()
        c.steps = data.get("steps", 0)
        c.cells = data.get("cells", {})
        return c


def all_levels():
    """Every ``factor=level`` the array can reach."""
    return ["%s=%s" % (f, lv) for f, levels in option_row.FACTORS.items() for lv in levels]


def drawn_actions():
    """The actions a walk draws (an undo run is a Ctrl-U, with Ctrl-Rs
    after it); the breakpoint toggle is the prelude's alone."""
    return sorted((set(WEIGHTS) - {"undo_run"}) | {"undo"})


def never_hit(cov, actions=None):
    """[(action, factor=level)] that no step of @cov ran: the holes.  By
    default every action a walk draws, so one that never ran is a hole at
    every level."""
    levels = all_levels()
    out = []
    for action in (actions or drawn_actions()):
        cells = cov.cells.get(action, {})
        for lv in levels:
            if not cells.get(lv):
                out.append((action, lv))
    return out


def report(cov, actions=None):
    """The matrix as text: for each action the levels it never met, then the
    levels the harness cannot drive yet."""
    holes = never_hit(cov, actions)
    out = ["steps %d, actions %d, factor levels %d"
           % (cov.steps, len(cov.cells), len(all_levels()))]
    by = {}
    for action, lv in holes:
        by.setdefault(action, []).append(lv)
    for action in sorted(by):
        out.append("never hit  %-18s %s" % (action, " ".join(by[action])))
    pend = ["%s=%s" % (f, ",".join(lv)) for f, lv in option_row.PENDING.items()]
    out.append("not built   %s" % "  ".join(pend))
    out.append("cells never hit: %d" % len(holes))
    return "\n".join(out), len(holes)


# ---- the size of a run -------------------------------------------------------------

def budget(default=SHORT_BUDGET):
    """(rows, walks, steps) from EDIT_MODEL_BUDGET, or @default."""
    text = os.environ.get(BUDGET_ENV)
    if not text:
        return default
    parts = [int(p) for p in text.split(",")]
    return tuple(parts + list(default[len(parts):]))[:3]


def pick_rows(rows, n):
    """The first @n rows (all of them for 0), but the first of each key
    style before any second: a short run reaches both styles."""
    if n <= 0:
        return list(rows)
    first = {}
    for r in rows:
        if r.lv["window"] == "file" and not r.lv["file"].startswith("empty"):
            first.setdefault(r.style, r)
    chosen = list(first.values())
    for r in rows:
        if len(chosen) >= n:
            break
        if r not in chosen:
            chosen.append(r)
    return chosen[:n]


def main(argv):
    if len(argv) == 2 and argv[0] == "report":
        with open(argv[1], encoding="utf-8") as fh:
            cov = Coverage.from_json(json.load(fh))
        text, holes = report(cov)
        print(text)
        return 1 if holes else 0
    sys.stderr.write(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
