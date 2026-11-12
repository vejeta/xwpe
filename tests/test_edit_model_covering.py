"""The option array and the walks drawn over it, without an editor.

The array is the table of option rows the lockstep runs: every pair of
factors is in some row, and the factors that decide the most (the key style,
the block mode, the insert mode, the auto-indent mode; and the width change,
the kind of file and what the clipboard holds) are covered three at a time.
These rows check the checked-in file, the generator that made it, and what a
row turns into: its options file, its file, its prelude.
"""
import json
import os

import pytest

pytest.importorskip("pyte")

from edit_model import covering, model, option_row, walk  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROWS = covering.load()
LEVELS_PER_FACTOR = dict((f, set(v)) for f, v in option_row.FACTORS.items())


# @scenario SC-edit-model-29
def test_the_checked_in_array_covers_every_pair_and_every_triple_of_its_groups():
    """No tuple the array must cover is missing, whatever tool made it."""
    left = covering.missing([r.levels() for r in ROWS])
    assert not left, sorted(left)[:5]
    assert 40 <= len(ROWS) <= 120, len(ROWS)


# @scenario SC-edit-model-29
def test_the_generator_gives_the_checked_in_file_again():
    """The command written at the top of the file regenerates it byte for
    byte, so a change to the factors is a change to the file."""
    with open(covering.TSV, encoding="utf-8") as fh:
        text = fh.read()
    assert text.splitlines()[0].startswith("# regenerate: ")
    seed = int(text.splitlines()[0].rsplit("--seed ", 1)[1])
    assert covering.to_tsv(covering.generate(seed), seed) == text


# @scenario SC-edit-model-29
def test_the_checker_finds_a_tuple_an_array_lacks():
    """A copy of the array without one row is reported as lacking tuples:
    the check is not vacuous."""
    levels = [r.levels() for r in ROWS]
    assert covering.missing(levels[1:]) != set() or len(levels) == 1
    one = [{"keys": "old"}]
    assert len(covering.missing(one)) > 800


# @scenario SC-edit-model-29
def test_every_row_picks_one_level_of_every_factor_and_obeys_the_constraints():
    for r in ROWS:
        assert set(r.lv) == set(option_row.FACTORS), r.name
        for f, lv in r.lv.items():
            assert lv in LEVELS_PER_FACTOR[f], (r.name, f, lv)
        assert not option_row.violates(r.lv), r.name
        if r.lv["breakpoints"] == "two":
            assert r.filename == "t.c" and r.seed_text.count(b"\n") >= 4, r.name
    names = [r.name for r in ROWS]
    assert len(set(names)) == len(names)


# @scenario SC-edit-model-29
def test_every_level_of_every_factor_is_in_some_row():
    for f, levels in option_row.FACTORS.items():
        met = set(r.lv[f] for r in ROWS)
        assert met == set(levels), (f, set(levels) - met)


# @scenario SC-edit-model-29
def test_a_row_becomes_an_options_file_a_file_and_a_prelude():
    """The row's options file is the editor's own, read back by the model's
    reader; its file and mode follow the levels; its prelude brings the
    session to the state it names, the modes last."""
    row = option_row.Row(dict(zip(option_row.FACTORS, (
        "cua", "wordstar", "overwrite", "always", "40", "4", "2", "changes", "on",
        "mixed.c", "two", "multiline", "on", "lowered", "readonly", "one"))), "t")
    rc = row.xwperc()
    o = model.Options.from_xwperc(rc)
    assert (o.cua, o.wordstar, o.endmark) == (True, True, True)
    assert (o.tabn, o.numundo, o.maxchg, o.autosave, o.maxcol) == (4, 2, 5, True, 118)
    assert "Options2 : %d" % (24 + 1 + 2 + 4 + 256) in rc
    assert row.filename == "t.c" and row.file_mode == option_row.READONLY_MODE
    names = [n for n, _a in row.prelude()]
    assert names[0] == "option_set" and names[-2:] == ["insert_toggle", "literal_toggle"]
    assert names.count("toggle_breakpoint") == 2 and "edit_copy" in names
    plain = option_row.Row(dict(zip(option_row.FACTORS, (
        "old", "modern", "insert", "never", "118", "8", "10", "off", "off",
        "empty.txt", "none", "empty", "off", "none", "file", "one"))), "p")
    assert plain.prelude() == [] and plain.file_mode is None and plain.seed_text == b""
    assert plain.filename == "t.txt"


# @scenario SC-edit-model-30
def test_a_walk_is_the_same_walk_for_the_same_seed():
    """The drawn actions depend on the seed and nothing else, so a failing
    row names its reproduction."""
    import random

    def draw(seed, n=60):
        d = walk.Drawer(random.Random(seed))
        st = model.State()
        return [d.next(st) for _ in range(n)]

    assert draw("r001-cua-1") == draw("r001-cua-1")
    assert draw("r001-cua-1") != draw("r001-cua-2")


# @scenario SC-edit-model-30
def test_a_walk_draws_every_action_of_the_weights_with_valid_arguments():
    import random
    d = walk.Drawer(random.Random(3))
    seen = set()
    st = model.State()
    for _ in range(4000):
        name, arg = d.next(st)
        seen.add(name)
        if name == "marker_set" or name == "marker_goto":
            assert 0 <= arg < model.MARKER_COUNT
        if name == "replace_all":
            assert arg[0] and arg[1] and arg[0] not in arg[1]
        if name == "option_set":
            assert arg[0] in walk.OPTION_VALUES and arg[1] in walk.OPTION_VALUES[arg[0]]
    wanted = set(walk.WEIGHTS) - {"undo_run"} | {"undo"}
    assert wanted <= seen, sorted(wanted - seen)
    for name in walk.WEIGHTS:
        if name != "undo_run":
            assert name in model.ACTIONS or name.endswith("_primary"), name


# @scenario SC-edit-model-30
def test_undo_comes_in_runs_of_one_to_three_then_up_to_three_redos():
    import random
    d = walk.Drawer(random.Random(1))
    d.queue = []
    first = d._undo_run()
    assert first == ("undo", None)
    assert 0 <= len(d.queue) <= walk.UNDO_RUN_MAX - 1 + walk.REDO_RUN_MAX
    assert all(a in (("undo", None), ("redo", None)) for a in d.queue)


# @scenario SC-edit-model-30
def test_the_size_of_a_run_comes_from_the_budget_variable(monkeypatch):
    monkeypatch.delenv(walk.BUDGET_ENV, raising=False)
    assert walk.budget() == walk.SHORT_BUDGET
    monkeypatch.setenv(walk.BUDGET_ENV, "5,3,70")
    assert walk.budget() == (5, 3, 70)
    monkeypatch.setenv(walk.BUDGET_ENV, "9")
    assert walk.budget() == (9, walk.SHORT_BUDGET[1], walk.SHORT_BUDGET[2])
    two = walk.pick_rows(ROWS, 2)
    assert set(r.style for r in two) == {"old", "cua"}
    assert all(r.lv["window"] == "file" for r in two)
    assert len(walk.pick_rows(ROWS, 0)) == len(ROWS)


# @scenario SC-edit-model-31
def test_the_coverage_report_lists_the_levels_an_action_never_met(tmp_path):
    """Steps are counted under the action and the row's levels; the report
    names each level no step of an action ran under, the levels the harness
    cannot drive yet, and exits 1 while a cell is empty; the JSON a run
    leaves is read back."""
    cov = walk.Coverage()
    row = ROWS[0].levels()
    cov.add("block_copy", row)
    cov.add("type", row)
    holes = walk.never_hit(cov)
    assert ("block_copy", "keys=cua") in holes and ("block_copy", "keys=old") not in holes
    text, n = walk.report(cov)
    assert n == len(holes) > 0
    assert "never hit  block_copy" in text and "not built" in text and "windows=second_narrower" in text
    path = tmp_path / "run.json"
    path.write_text(json.dumps(cov.to_json()))
    back = walk.Coverage.from_json(json.loads(path.read_text()))
    assert walk.never_hit(back) == holes
    full = walk.Coverage()
    for r in ROWS:
        full.add("type", r.levels())
    assert walk.never_hit(full, ["type"]) == [], "the array reaches every level"
