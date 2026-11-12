"""The editing model on its own: every rule names its source, and the
model keeps the promises the rest of the harness relies on.

No editor runs here.  Seeded walks over the model alone check that Undo
gives back the text from before an action and Redo the text after it,
that one action is one undo step, that the cursor and the marks stay
inside the text, that a wrap keeps every byte, and that a breakpoint
stays on its line through splits and joins and comes back with Undo.
"""
import random

import pytest

pytest.importorskip("pyte")   # the key tables share the session's key names

from edit_model import keymap, model, render  # noqa: E402

WIDTH = 120
NARROW = 20
WALKS = 40
WALK_LEN = 60
# Rows the model walks over: a long row, a tab, an indent, a multibyte
# character, an empty row.
ROWS = [("H", b"int main(void)"), ("H", b"{"), ("H", b"\tif (a) {"),
        ("H", b"    int a = 1;"), ("H", b""), ("H", "café au lait".encode()),
        ("H", b" ".join(b"w%03d" % i for i in range(23)) + b"xy"), ("H", b"last")]
EDITS = ["type", "enter", "tab", "backspace", "delete", "delete_word",
         "kill_to_eol", "delete_line"]
MOVES = ["left", "right", "up", "down", "home", "end", "file_start", "file_end",
         "page_up", "page_down", "word_left", "word_right"]
TYPED = [b"x", b" ", b"-", b"{", "é".encode()]


def _state(lines=ROWS, y=0, x=0, flg=model.FLG_AUTOINDENT, width=WIDTH):
    st = model.State()
    for end, data in lines:
        st.rows.append(model.Row(data, end, st.next_ident))
        st.next_ident += 1
    st.y, st.x, st.flg, st.width = y, x, flg, width
    st.mb = st.me = (len(lines) - 1, 0)
    return st


def _utf8(data):
    """Is @data valid UTF-8?  A wrap never cuts a character that was whole."""
    try:
        data.decode("utf-8")
    except UnicodeDecodeError:
        return False
    return True


def _auto_indent_tab(st, act):
    """Is @act a Tab with auto-indent in an unlocked window, which the
    editor records as one step whatever the row becomes?"""
    return act.name == "tab" and bool(st.flg & model.FLG_AUTOINDENT) and \
        st.ins != model.INS_READONLY


def _action(rng, pool):
    name = rng.choice(pool)
    if name == "type":
        return model.Action(name, rng.choice(TYPED))
    return model.Action(name)


def _inside(st):
    rows = st.rows
    y, x = st.y, st.x
    bad = []
    if not (0 <= y < len(rows) and 0 <= x <= len(rows[y].data)):
        bad.append("cursor %s" % ((y, x),))
    for name, (my, mx) in (("mb", st.mb), ("me", st.me)):
        if not (0 <= my < len(rows)):
            bad.append("%s row %d" % (name, my))
        elif not (0 <= mx <= len(rows[my].data) + 1):
            bad.append("%s %s" % (name, (my, mx)))
    return bad


# @scenario SC-edit-model-14
def test_every_rule_names_its_source_and_where_it_is_written():
    """Each rule says whether the manual, the compatibility chapter, the
    user's decision, an approved golden, the model's own design or the
    code backs it, and where; the counts per source are what a report
    quotes."""
    for r in model.RULES.values():
        assert r.source in model.SOURCES, r
        assert r.ref and r.says and r.fields, r
        assert set(r.fields) <= set(model.FIELDS), r
    counts = model.rule_counts()
    assert sum(counts.values()) == len(model.RULES)
    assert counts[model.SOURCE_MANUAL] > 0 and counts[model.SOURCE_CODE] > 0


# @scenario SC-edit-model-14
def test_a_rules_place_is_the_kind_of_text_its_source_names():
    """A rule is labelled by where it was learned: a manual rule points into
    the help text or the editor chapter, a chapter rule into the
    compatibility chapter, a decision at the user's word, a design rule at
    the design, a code rule at a C file or a probe.  The one-step rule of the
    block commands is the design's, the user's decision is the five actions
    it named, and the marker and help-marker rules cite the manual alone."""
    for r in model.RULES.values():
        if r.source == model.SOURCE_MANUAL:
            assert "help.xwpe_eng" in r.ref or "editor.texi" in r.ref, r
        elif r.source == model.SOURCE_CHAPTER:
            assert "compatibility.texi" in r.ref, r
        elif r.source == model.SOURCE_DECISION:
            assert r.ref == model.DECIDED_ONE_STEP, r
        elif r.source == model.SOURCE_DESIGN:
            assert "DESIGN" in r.ref or "this model" in r.ref or "REVIEW" in r.ref, r
        elif r.source == model.SOURCE_CODE:
            assert ".c" in r.ref or "probe" in r.ref \
                or "replay" in r.ref or "record" in r.ref, r
    decided = sorted(r.name for r in model.RULES.values() if r.source == model.SOURCE_DECISION)
    assert decided == ["backspace_indent_one_step", "help_markers_one_step",
                       "indent_one_step", "replace_one_step", "tab_one_step"]
    assert model.RULES["block_one_step"].source == model.SOURCE_DESIGN
    assert model.RULES["one_step_per_action"].source == model.SOURCE_DESIGN
    for name in ("marker_keys", "help_markers"):
        assert model.RULES[name].source == model.SOURCE_MANUAL
        assert "we_" not in model.RULES[name].ref, name
    for name in ("end_of_text_stays", "block_locked", "help_end_line"):
        assert model.RULES[name].source == model.SOURCE_CHAPTER, name


# @scenario SC-edit-model-14
@pytest.mark.parametrize("style", keymap.STYLES)
def test_every_action_has_its_keys_in_each_key_style(style):
    """No action of the model is left without keys: a walk can draw any of
    them under either key style."""
    for name in model.ACTIONS:
        if name in ("type", "toggle_breakpoint"):
            continue
        assert keymap.keys_for(style, name, keymap.sample_arg(name)), name
    assert keymap.keys_for(keymap.STYLE_CUA, "save") != keymap.keys_for(keymap.STYLE_OLD, "save")


# @scenario SC-edit-model-15
@pytest.mark.parametrize("seed", range(WALKS))
def test_undo_and_redo_give_back_the_text_around_each_action(seed):
    """After any action, Ctrl-U gives the rows, line identities and
    breakpoints from before it and Ctrl-R those after it, and one action
    that changed the text is exactly one undo step.  A Tab with auto-indent
    is one step even when the blanks it put in are those it erased."""
    rng = random.Random(seed)
    st = _state()
    st = model.apply(st, model.Action("toggle_breakpoint"))
    for _ in range(WALK_LEN):
        act = _action(rng, EDITS + MOVES)
        after = model.apply(st, act)
        changed = after.lines() != st.lines() or _auto_indent_tab(st, act)
        if changed:
            assert after.ud == min(st.ud + 1, st.options.numundo + 1), act
            back = model.apply(after, model.Action("undo"))
            assert [(r.data, r.end, r.ident) for r in back.rows] == \
                [(r.data, r.end, r.ident) for r in st.rows], act
            assert back.brp_rows() == st.brp_rows(), act
            again = model.apply(back, model.Action("redo"))
            assert again.lines() == after.lines(), act
            assert again.brp_rows() == after.brp_rows(), act
        else:
            assert after.ud == st.ud, act
        assert _inside(after) == [], (act, _inside(after))
        st = after


# @scenario SC-edit-model-15
@pytest.mark.parametrize("seed", range(WALKS))
def test_typing_keeps_every_byte_and_every_row_within_the_width(seed):
    """Typing anywhere in a window narrower than its lines wraps them, soft
    rows and full rows included: the text, read with soft rows joined, is
    the old text with the typed bytes at the cursor, every row holds at most
    Max. Columns bytes (help.xwpe_eng 1578-1580) and no character is cut."""
    rng = random.Random(seed)
    lines = [("S", b"one two three four "), ("H", b"five six"),
             ("S", b"seven-eight nine "), ("H", b"ten"),
             ("S", b"x" * NARROW), ("H", b"y" * NARROW), ("H", b"a " + b"z" * 18)]
    st = _state(lines, flg=0, width=NARROW)
    st = model.apply(st, model.Action("file_end"))
    for _ in range(WALK_LEN * 4):
        mv = model.Action(rng.choice(["left", "right", "up", "down", "home", "end"]))
        st = model.apply(st, mv)
        before = render.render_disk(st.lines())
        rows = st.lines()
        offset = sum(len(d) + (1 if e == "H" else 0) for e, d in rows[:st.y]) + st.x
        typed = rng.choice(TYPED + ["€".encode()])
        st = model.apply(st, model.Action("type", typed))
        text = render.render_disk(st.lines())
        assert text == before[:offset] + typed + before[offset:], (before, text)
        assert all(len(d) <= NARROW for _e, d in st.lines()), st.lines()
        if _utf8(text):
            assert all(_utf8(d) for _e, d in st.lines()), st.lines()


# @scenario SC-edit-model-16
def test_a_breakpoint_stays_on_its_line_through_a_split_and_a_join():
    """A breakpoint on a line moves down with it when Return splits the
    line at column 0, stays with the first half of a split elsewhere, and
    comes back with Undo when a join takes its line away."""
    st = _state([("H", b"alpha"), ("H", b"beta"), ("H", b"gamma")], flg=0)
    st = model.apply(st, model.Action("down"))
    st = model.apply(st, model.Action("toggle_breakpoint"))
    assert st.brp_rows() == [1]
    st = model.apply(st, model.Action("enter"))            # split at column 0
    assert st.brp_rows() == [2], "the line moved down with its breakpoint"
    st = model.apply(st, model.Action("right"))
    st = model.apply(st, model.Action("enter"))            # split inside "beta"
    assert st.brp_rows() == [2] and st.lines()[2] == ("H", b"b")
    st = model.apply(st, model.Action("up"))
    st = model.apply(st, model.Action("up"))
    st = model.apply(st, model.Action("type", b"z"))       # the blank line is "z" now
    st = model.apply(st, model.Action("down"))
    st = model.apply(st, model.Action("home"))
    st = model.apply(st, model.Action("backspace"))        # join "b" to "z" above
    assert st.lines()[1] == ("H", b"zb")
    assert st.brp_rows() == [], "the joined line's breakpoint goes"
    st = model.apply(st, model.Action("undo"))
    assert st.brp_rows() == [2], "and comes back with Undo"


# @scenario SC-edit-model-16
def test_render_disk_and_logical_lines_read_the_rows_as_the_file():
    """The file a Save writes is each row with a line break after a hard
    one; soft rows join into one line."""
    rows = [("S", b"one "), ("H", b"two"), ("H", b""), ("S", b"x")]
    assert render.render_disk(rows) == b"one two\n\nx"
    assert render.logical_lines(rows) == [b"one two", b"", b"x"]
    assert render.show_rows(rows, cursor=(1, 1)) == ["one ~", "t|wo$", "$", "x~"]


# @scenario SC-edit-model-16
def test_show_endmark_marks_a_soft_row_as_well_as_a_hard_one():
    """With Show Endmark a row that ends with a line break shows ``$`` after
    its text and a row that goes on in the next one shows ``\\``, as the
    window draws them; without it neither shows."""
    rows = [("S", b"one "), ("H", b"two"), ("S", b"x")]
    view = (0, 0, 1, 0, 5, 41)
    assert render.render_window(rows, view, 8, endmark=True) == \
        {2: "one \\", 3: "two$", 4: "x\\"}
    assert render.render_window(rows, view, 8) == {2: "one", 3: "two", 4: "x"}
