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

from edit_model import covering, lockstep, model, option_row, walk  # noqa: E402

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
        print("\n[%s seed %d] steps %d, new %s, expected %s"
              % (row.name, seed, w.steps_run, new, sum(known.values())))
        text = lockstep.summarize([w])
        if text:
            print(text)
    _leave("%s-s%d" % (row.name, seed), w, cov)
    assert not w.new_losses(), w.new_losses()
    assert w.start_mismatch == [], ("the row did not get its labels", w.start_mismatch)
    for x in w.disagreements:
        assert x["cls"] in CLASSES, x
        if x["cls"] == lockstep.CLASS_EDITOR:
            assert x["rules"] or x["field"] in ("screen", "typed-kept", "box", "status"), x
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


# @scenario SC-edit-model-39
def test_every_crash_is_a_new_loss():
    """No crash of the editor is known any more: Ctrl-K V after an Undo that
    left the block past the text (fixed: Undo keeps the marks in the text)
    and Backspace in the empty last row (fixed) are new losses like any
    other, and so is a Ctrl-K V the walk would once have excused."""
    walk_ = lockstep.Walk("unused", "old", 1, 0)
    for i, name in enumerate(("backspace", "block_move", "enter"), 1):
        walk_.lose(i, name, "no answer", None, None)
    assert walk_.new_losses() == [(1, "backspace", "no answer"),
                                  (2, "block_move", "no answer"),
                                  (3, "enter", "no answer")]


# @scenario SC-edit-model-39
def test_an_undo_after_a_change_all_the_editor_pushed_in_pieces_is_not_a_new_finding(tmp_path):
    """Change All is one Ctrl-U by the user's decision and one per hit in the
    editor today.  The walk files the pushes under that decision, then the
    Ctrl-U and Ctrl-R that follow act on steps the model holds no snapshot of:
    the editor's answer is taken and no new disagreement appears."""
    w = lockstep.Walk(str(tmp_path), "old", 1, 4)
    w.drawer.queue = [("replace_all", (b"a", b"Q")), ("undo", None), ("undo", None),
                      ("redo", None)]
    w.run()
    assert w.steps_run == 4 and not w.lost
    first = [x for x in w.disagreements if x["step"] == 0]
    assert first and all(x["field"] in (model.F_STEPS, "box") or x["expected"] for x in first), first
    steps = [x for x in first if x["field"] == model.F_STEPS]
    assert steps and all(x["expected"] == lockstep.KNOWN_STEPS for x in steps), steps
    later = [x for x in w.disagreements if x["step"] > 0 and not x["expected"]]
    assert later == [], later


# @scenario SC-edit-model-39
def test_the_status_row_is_compared_with_what_the_model_refused():
    """A refusal the model predicts must show on the status row; a status
    row that says the text is unchanged after a key the model did not refuse
    is a finding, unless the previous key left it there."""
    st = _state([(model.HARD, b"one")], 0, 0)
    seen = " Read-only window: the text is unchanged"
    other = " F1 Help F2 Save F3 Files"
    st.status = model.STATUS_READ_ONLY
    assert lockstep.status_disagreements(st, seen, None) == []
    assert lockstep.status_disagreements(st, other, None) == \
        [(model.F_STATUS, model.STATUS_READ_ONLY, other)]
    st.status = None
    assert lockstep.status_disagreements(st, other, None) == []
    assert lockstep.status_disagreements(st, seen, None) == \
        [(model.F_STATUS, "no refusal said", seen)]
    assert lockstep.status_disagreements(st, seen, model.STATUS_READ_ONLY) == []


def _editor_window(rows, ud, rd):
    """An editor dump window of @rows with no block, the cursor at the start."""
    return types.SimpleNamespace(
        lines=list(rows), y=0, x=0, mb=(0, 0), me=(0, 0), ks=(0, 0), ins=model.INS_INSERT,
        flg=0, save=0, view=(0, 0, 0, 0, 29, 79), pt=[(-1, -1)] * model.MARKER_COUNT, brp=[],
        ud=ud, rd=rd, cursor=(0, 0))


# @scenario SC-edit-model-39
def test_a_resync_after_a_text_disagreement_takes_the_editors_undo_depth():
    """Steps the model recorded from a wrong prediction must not be replayed:
    after a text or steps disagreement every step is one the model cannot
    replay and the depths are the editor's; otherwise the model keeps its
    steps, which were agreed when pushed."""
    H = model.HARD
    base = _state([(H, b"one")], 0, 3)
    base.width = 120
    st = model.apply(base, model.Action("type", b"x"))
    assert len(st.undo) == 1
    w = _editor_window([(H, b"onexy")], ud=3, rd=1)
    keep = model.apply(base, model.Action("type", b"x"))
    model.resync(keep, w)
    assert len(keep.undo) == 1 and (keep.ud, keep.rd) == (3, 1)
    model.resync(st, w, rebase_steps=True)
    assert st.undo == [] and st.redo == [] and (st.opaque_undo, st.opaque_redo) == (3, 1)
    assert (st.ud, st.rd) == (3, 1)


# @scenario SC-edit-model-39
def test_undo_of_a_step_the_model_holds_no_snapshot_of_is_the_editors_to_say():
    """After a rebase every step is opaque: Ctrl-U moves one step from the
    undo count to the redo count, predicts no text, and says why."""
    H = model.HARD
    st = _state([(H, b"one")], 0, 0)
    st.opaque_undo, st.opaque_redo = 2, 0
    done = model.apply(st, model.Action("undo"))
    assert (done.ud, done.rd) == (1, 1)
    assert "undo_unmodelled_step" in done.fired
    assert {model.F_TEXT, model.F_CURSOR, model.F_MARKS} <= done.resync_fields
    back = model.apply(done, model.Action("redo"))
    assert (back.ud, back.rd) == (2, 0) and "undo_unmodelled_step" in back.fired


# @scenario SC-edit-model-45
def test_a_row_that_asks_for_what_the_editor_refuses_is_caught_at_its_start(tmp_path):
    """The old array's rows that got other levels than their labels would have
    been caught here: a read-only window asked to be in overwrite mode and in
    Alt-J's mode, a clipboard asked to hold a line of an empty file, and
    auto-indent by the file's kind asked of a text file."""
    levels = dict(zip(option_row.FACTORS, (
        "old", "modern", "overwrite", "source", "118", "8", "10", "off", "off",
        "empty.txt", "none", "oneline", "on", "readonly", "one")))
    bad = option_row.Row(levels, "bad")
    assert option_row.violates(levels)
    w = lockstep.Walk(str(tmp_path), None, 1, 0, row=bad, coverage=walk.Coverage()).run()
    got = dict((f, (want, seen)) for f, want, seen in w.start_mismatch)
    assert got["ins"] == ("overwrite", "insert")
    assert got["alt_j"] == ("on", "off")
    assert got["clipboard"] == ("oneline", "empty")
    assert got["autoindent"] == ("on", "off")


# @scenario SC-edit-model-45
@pytest.mark.parametrize("row", ROWS, ids=[r.name for r in ROWS])
def test_every_row_of_the_array_starts_with_its_labels(tmp_path, row):
    """The session a row starts (its options file, its file, its prelude) has
    the levels the row names, read back from the editor's own dump and screen:
    the insert and Alt-J modes, auto-indent, the window's width, a read-only
    window, the breakpoints, what the clipboard holds, the file's kind, Show
    Endmark and, where the file has a tab, Tabstops."""
    w = lockstep.Walk(str(tmp_path), None, 1, 0, row=row, coverage=walk.Coverage()).run()
    assert w.start_mismatch == [], (row.name, w.start_mismatch, w.start_observed)


def _state(rows, y, x, ins=model.INS_INSERT, flg=0):
    """A model state holding @rows ((end, bytes)) with the cursor at (@y, @x)."""
    st = model.State()
    st.rows = [model.Row(data, end, i) for i, (end, data) in enumerate(rows)]
    st.y, st.x, st.ins, st.flg = y, x, ins, flg
    return st


def _dump(rows, y, x, ud=0):
    """An editor dump window with @rows, the cursor at (@y, @x), no block."""
    return types.SimpleNamespace(lines=list(rows), cursor=(y, x), y=y, x=x, mb=(0, 0),
                                 me=(0, 0), ud=ud, brp=[])


# @scenario SC-edit-model-39
def test_a_known_bug_tag_names_only_what_that_bug_does():
    """A disagreement is filed under a known bug only when the editor did
    what that bug does; anything else on the same key shows as new, and a
    cursor outside the text is never excused."""
    H, S = model.HARD, model.SOFT
    tag = lockstep.expected_tag
    # a locked window where Ctrl-K I changed the text is a new finding: the
    # lock bug is fixed and keeps no tag
    rows = [(H, b"one"), (H, b"two")]
    before = _state(rows, 0, 0, ins=model.INS_READONLY)
    act = model.Action("block_indent", None)
    assert tag(act, model.F_TEXT, before, before,
               _dump([(H, b"    one"), (H, b"two")], 0, 0, ud=1)) is None
    # the Undo cursor question is not offered for a cursor outside the text
    undo = model.Action("undo", None)
    assert tag(undo, model.F_CURSOR, before, before, _dump(rows, 1, 2)) == \
        lockstep.KNOWN_UNDO_CURSOR
    assert tag(undo, model.F_CURSOR, before, before, _dump(rows, 1, 9)) is None
    assert tag(undo, model.F_CURSOR, before, before, _dump(rows, 7, 0)) is None
    # a Tab that pushed several steps is the decision not built yet; one that
    # pushed one step is not
    tab = model.Action("tab", None)
    assert tag(tab, model.F_STEPS, before, before, _dump(rows, 0, 0, ud=3)) == \
        lockstep.KNOWN_STEPS
    assert tag(tab, model.F_STEPS, before, before, _dump(rows, 0, 0, ud=1)) is None
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
    # a cursor outside the text is never excused by any tag
    before = _state([(H, b"one")], 0, 3)
    act = model.Action("delete_word", None)
    assert tag(act, model.F_TEXT, before, before, _dump([], 0, 0)) is None
    assert tag(act, lockstep.FIELD_INSIDE, before, before, _dump([], 0, 0)) is None
