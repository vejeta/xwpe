"""The model beside the editor, over the option array.

Each case is one row of ``option_rows.tsv`` and one seeded walk: the session
starts with the row's options file and file, the row's prelude (the
dialog, the breakpoints, the clipboard, the modes) brings it to the row's
state, then the walk draws block commands, edits, cursor keys, Undo and Redo
runs, Change All and the option dialog, and after every step the model's
prediction is compared with the editor's dump.

A disagreement is not a failure here: it is a finding, listed with its class
and its source.  A case fails when the editor stops answering for a reason no
known bug explains, or when a disagreement has no class.  The size of a run is
``EDIT_MODEL_BUDGET=rows,walks,steps`` (the short gate is two rows of
two 40-step walks); ``EDIT_MODEL_OUT=<dir>`` makes each case leave its
result and its coverage there as JSON, for ``harness.py coverage``.
Run with ``pytest -n 4`` for many rows: the cases share nothing.
"""
import json
import os
import types

import pytest

pytest.importorskip("pyte")

from edit_model import covering, lockstep, model, walk  # noqa: E402

ROWS = covering.load()
N_ROWS, N_WALKS, N_STEPS = walk.budget()
CASES = [(r, seed) for r in walk.pick_rows(ROWS, N_ROWS) for seed in range(1, N_WALKS + 1)]
CLASSES = (lockstep.CLASS_EDITOR, lockstep.CLASS_MODEL, lockstep.CLASS_UNSPECIFIED)
# The single place a case leaves its result, when asked.
OUT_ENV = "EDIT_MODEL_OUT"


def _leave(case_id, w, cov):
    out = os.environ.get(OUT_ENV)
    if not out:
        return
    with open(os.path.join(out, case_id + ".json"), "w", encoding="utf-8") as fh:
        json.dump({"walk": w.result(), "coverage": cov.to_json()}, fh)


# @scenario SC-edit-model-33
@pytest.mark.parametrize("row,seed", CASES, ids=["%s-s%d" % (r.name, s) for r, s in CASES])
def test_a_walk_on_a_row_of_the_array_lists_its_disagreements_by_class(
        tmp_path, row, seed, capsys):
    """The editor answers every step of a seeded walk on a row of the array;
    each disagreement of the model with it is listed with a class, a source
    for the rule that predicted the field, and the keys that show it."""
    cov = walk.Coverage()
    w = lockstep.Walk(str(tmp_path), None, seed, N_STEPS, row=row, coverage=cov).run()
    new, known = lockstep.summary_counts([w])
    with capsys.disabled():
        print("\n[%s seed %d] steps %d, new %s, expected %s, known crashes %d"
              % (row.name, seed, w.steps_run, new, sum(known.values()), w.known_crashes))
        text = lockstep.summarize([w])
        if text:
            print(text)
    _leave("%s-s%d" % (row.name, seed), w, cov)
    assert not w.new_losses(), w.new_losses()
    for x in w.disagreements:
        assert x["cls"] in CLASSES, x
        if x["cls"] == lockstep.CLASS_EDITOR:
            assert x["rules"] or x["field"] in ("screen", "typed-kept", "box"), x
    assert w.steps_run >= 1


# @scenario SC-edit-model-33
def test_the_prelude_of_a_row_is_run_before_the_walk_and_counted(tmp_path):
    """A row whose prelude puts breakpoints on two lines and copies lines to
    the clipboard starts its walk with the editor in that state: the first
    steps are the prelude's, and the model agrees with the editor on them."""
    row = next(r for r in ROWS if r.lv["breakpoints"] == "two" and r.lv["window"] == "file"
               and r.lv["clipboard"] != "empty")
    cov = walk.Coverage()
    w = lockstep.Walk(str(tmp_path), None, 1, 0, row=row, coverage=cov).run()
    assert w.steps_run == len(row.prelude()) and not w.lost
    assert cov.cells.get("toggle_breakpoint") and cov.cells.get("edit_copy")
    assert [x for x in w.disagreements if x["field"] in ("breakpoints", "clipboard")] == []


def _state(rows, y, x, ins=model.INS_INSERT, flg=0):
    """A model state holding @rows ((end, bytes)) with the cursor at (@y, @x)."""
    st = model.State()
    st.rows = [model.Row(data, end, i) for i, (end, data) in enumerate(rows)]
    st.y, st.x, st.ins, st.flg = y, x, ins, flg
    return st


def _dump(rows, y, x, ud=0):
    """An editor dump window with @rows, the cursor at (@y, @x), no block."""
    return types.SimpleNamespace(lines=list(rows), cursor=(y, x), mb=(0, 0), me=(0, 0),
                                 ud=ud)


# @scenario SC-edit-model-39
def test_a_known_bug_tag_names_only_what_that_bug_does():
    """A disagreement is filed under a known bug only when the editor did
    what that bug does; anything else on the same key shows as new, and a
    cursor outside the text is never excused."""
    H, S = model.HARD, model.SOFT
    tag = lockstep.expected_tag
    # a locked window where Ctrl-K I changed nothing: not the lock bug
    rows = [(H, b"one"), (H, b"two")]
    before = _state(rows, 0, 0, ins=model.INS_READONLY)
    act = model.Action("block_indent", None)
    assert tag(act, model.F_MARKS, before, before, _dump(rows, 0, 0)) is None
    assert tag(act, model.F_TEXT, before, before,
               _dump([(H, b"    one"), (H, b"two")], 0, 0, ud=1)) == lockstep.KNOWN_LOCK
    # typing into a soft row that keeps every byte: not the soft-wrap bug
    before = _state([(S, b"abc"), (H, b"def")], 0, 3)
    pred = _state([(S, b"abcx"), (H, b"def")], 0, 4)
    act = model.Action("type", b"x")
    assert tag(act, model.F_TEXT, before, pred, _dump([(S, b"abc"), (H, b"xdef")], 1, 1)) is None
    assert tag(act, model.F_TEXT, before, pred,
               _dump([(S, b"abc"), (H, b"def")], 0, 3)) == lockstep.KNOWN_SOFT_WRAP
    # Backspace over an ASCII letter on a row with an accent earlier on it
    row = [(H, "café xy".encode())]
    act = model.Action("backspace", None)
    before = _state(row, 0, len(row[0][1]), flg=model.FLG_AUTOINDENT)
    assert tag(act, model.F_TEXT, before, before, _dump(row, 0, 0)) is None
    before = _state(row, 0, 5, flg=model.FLG_AUTOINDENT)
    assert tag(act, model.F_TEXT, before, before, _dump(row, 0, 0)) == \
        lockstep.KNOWN_UTF8_BACKSPACE
    # a cursor outside the text at the end-of-text Ctrl-T: always new
    before = _state([(H, b"one")], 0, 3)
    act = model.Action("delete_word", None)
    assert tag(act, model.F_TEXT, before, before, _dump([], 0, 0)) == lockstep.KNOWN_END_OF_TEXT
    assert tag(act, lockstep.FIELD_INSIDE, before, before, _dump([], 0, 0)) is None
