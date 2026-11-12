"""The editing model against the editor: the approved goldens, replayed
exactly, and one lockstep walk per key style.

A golden is a short key script and the editor's state after each key as
the user approved it (``tests/edit_model/goldens``).  Its rows skip until
the golden is listed in ``goldens/APPROVED`` with the sha256 of its trace,
so an unread recording never becomes a reference.

The lockstep rows walk the model and the editor side by side for 50 seeded
steps.  They do not demand agreement: each disagreement is listed with its
class (``editor`` where only the manual, the compatibility chapter or the
user's decision predicted the field, ``unspecified`` where the code did)
and its source.  They fail when the walk cannot go on, or when a
disagreement has no class.
"""
import os

import pytest

pytest.importorskip("pyte")

from edit_model import golden, keymap, lockstep, model  # noqa: E402

WALK_STEPS = 50
WALK_SEED = 1
CLASSES = (lockstep.CLASS_EDITOR, lockstep.CLASS_MODEL, lockstep.CLASS_UNSPECIFIED)


def _golden_ids():
    return [os.path.splitext(os.path.basename(p))[0] for p in golden.scripts()]


def _approved_or_skip(script):
    sha = golden.approved().get(script.name)
    if sha is None:
        pytest.skip("golden %s waits for the user's approval (goldens/APPROVAL.md)"
                    % script.name)
    assert sha == golden.trace_sha256(script.trace_path), \
        "the trace of %s changed after it was approved" % script.name


# @scenario SC-edit-model-10
def test_every_golden_has_its_trace_and_its_entry_for_approval():
    """Each golden script has its recorded trace beside it, a step in the
    trace for each action, and a section in the approval list the user
    reads."""
    with open(os.path.join(golden.GOLDEN_DIR, "APPROVAL.md"), encoding="utf-8") as fh:
        approval = fh.read()
    names = _golden_ids()
    assert names, "no golden scripts"
    for path in golden.scripts():
        script = golden.Script(path)
        assert os.path.exists(script.trace_path), script.name
        steps = golden.read_trace(script.trace_path)
        assert len(steps) == len(script.actions) + 1, script.name
        assert "## %s" % script.name in approval, script.name


# @scenario SC-edit-model-11
@pytest.mark.parametrize("name", _golden_ids())
def test_the_editor_replays_each_approved_golden_exactly(tmp_path, name):
    """An approved golden's keys, typed again, give the same state after
    every key: the rows, the cursor, the block marks, the modes, the
    modified count and the undo depths."""
    script = golden.Script(os.path.join(golden.GOLDEN_DIR, name + ".keys"))
    _approved_or_skip(script)
    want = golden.read_trace(script.trace_path)
    got = golden.record(script, str(tmp_path))
    for i, ((label, lines), (_l, d, _f)) in enumerate(zip(want, got)):
        assert golden.window_lines(d, script.filename) == lines, \
            "step %d (%s) differs from the approved golden" % (i, label)


# @scenario SC-edit-model-12
@pytest.mark.parametrize("name", _golden_ids())
def test_the_model_agrees_with_each_approved_golden(name):
    """The model, run over an approved golden's actions from its first
    state, predicts every recorded step."""
    script = golden.Script(os.path.join(golden.GOLDEN_DIR, name + ".keys"))
    _approved_or_skip(script)
    rows = golden.model_against_trace(script, golden.read_trace(script.trace_path))
    assert rows == [], rows


# @scenario SC-edit-model-13
@pytest.mark.parametrize("style", keymap.STYLES)
def test_a_lockstep_walk_lists_every_disagreement_with_its_class(tmp_path, style, capsys):
    """Fifty seeded steps of typing, Enter, Tab, Backspace, Delete, the
    word and line deletions, the cursor keys, the modes, Save, Undo and
    Redo, with the model predicting each: the walk reaches its end, and
    every place the editor and the model differ is listed with its class
    and the rules behind the prediction."""
    w = lockstep.Walk(str(tmp_path), style, WALK_SEED, WALK_STEPS).run()
    with capsys.disabled():
        print("\n[%s] %d steps, %d disagreements\n%s"
              % (style, w.steps_run, len(w.disagreements), lockstep.summarize([w])))
    assert not w.lost, w.lost
    assert w.steps_run == WALK_STEPS
    for x in w.disagreements:
        assert x["cls"] in CLASSES, x
        if x["cls"] == lockstep.CLASS_EDITOR and x["field"] not in ("screen",):
            assert x["rules"], x
    assert all(r.source in model.SOURCES for r in model.RULES.values())
