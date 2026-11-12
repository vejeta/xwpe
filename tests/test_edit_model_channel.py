"""The editor's state channel: with XWPE_TEST_DUMP_FD set, the editor
writes what it holds each time it waits for a key, so a test knows the
editor has taken its keys without waiting a fixed time, and reads the text,
cursor, marks and undo depth without saving the file.

These rows pin the channel itself: it answers every kind of operation
wherever the keys land, it counts keys, it names where the next key is
read, it reports what an autosave wrote, the trace tells a fresh change
from a replay, and the self-checks built on it have teeth.
"""
import os

import pytest

pytest.importorskip("pyte")

from edit_model import inventory, probe, selfcheck  # noqa: E402
from edit_model.oracle import (ESC, F2, F3, EditSession, Window,  # noqa: E402
                               alt, ctrl)

SEED = "one\ntwo\nthree\n"
AUTOSAVE_ON = 2        # Options > Editor's Autosave, as xwperc writes it
AUTOSAVE_AFTER = 3     # Max. Changes: an autosave after the fourth character


def _session(tmp_path, seed=SEED, **kw):
    return EditSession(str(tmp_path), seed, **kw)


# @scenario SC-edit-model-01
@pytest.mark.parametrize("context", inventory.CONTEXT_NAMES)
def test_every_kind_of_operation_is_answered_wherever_the_keys_land(tmp_path, context):
    """Each operation kind, sent in the text, after a prefix, in the menu
    bar, in a dialog, in a message box or in the File Manager, gets the
    dump that counts its keys: no key path leaves the test waiting."""
    contexts = [c for c in inventory.CONTEXTS if c[0] == context]
    table, _prober, _ms = probe.probe_all(str(tmp_path), contexts=contexts)
    missed = {op: v for (_c, op), v in table.items() if v[0] != "yes"}
    assert not missed, "no answer in %s for: %s" % (context, missed)


# @scenario SC-edit-model-02
def test_the_dump_names_where_the_next_key_is_read(tmp_path):
    """The context field follows the user: a prefix, the menu bar, a
    dialog with its title, a message box, the File Manager, and the text
    again after Esc; and the key count is the keys sent."""
    with _session(tmp_path) as s:
        assert (s.last.seq, s.last.ctx) == (0, "edit")
        assert s.send([ctrl("k")]).ctx == "prefix"
        assert s.send(["h"]).ctx == "edit"
        assert s.send([ctrl("o")]).ctx == "prefix"
        assert s.send([ESC]).ctx == "edit"
        assert s.send([alt("o")]).ctx == "menu"
        assert s.send(["e"]).ctx == "dialog:Editor-Options"
        assert s.send([ESC]).ctx == "edit"
        assert s.send([alt("#"), "a"]).ctx == "message"
        assert s.send([ESC]).ctx == "edit"
        assert s.send([F3]).ctx == "filemanager"
        d = s.send([ESC])
        assert d.ctx == "edit"
        assert d.seq == s.keys_sent == 12


# @scenario SC-edit-model-03
def test_the_dump_holds_the_text_cursor_and_undo_lists(tmp_path):
    """Typing, Enter, Ctrl-U and Ctrl-R as the dump reports them: the
    lines with their ends, the cursor, the modified count, one undo step
    per key, and each Ctrl-U moving one step to the redo list and back;
    Save writes exactly the dump's text."""
    with _session(tmp_path) as s:
        w = s.send(["a", "b"]).win
        assert w.lines[0] == ("H", b"abone") and w.cursor == (0, 2)
        assert (w.ud, w.rd) == (2, 0) and w.save > 0
        w = s.send(["\r"]).win
        assert [l for l in w.lines] == [("H", b"ab"), ("H", b"one"),
                                         ("H", b"two"), ("H", b"three")]
        assert w.cursor == (1, 0) and (w.ud, w.rd) == (3, 0)
        w = s.send([ctrl("u")]).win
        assert w.lines[0] == ("H", b"abone") and (w.ud, w.rd) == (2, 1)
        w = s.send([ctrl("r")]).win
        assert len(w.lines) == 4 and (w.ud, w.rd) == (3, 0)
        w = s.send([F2]).win
        assert w.save == 0
        assert s.disk() == w.disk() == b"ab\none\ntwo\nthree\n"
        disp = s.display()
        assert selfcheck.check_screen(w, disp) == [], "the window does not show the dump"
        assert selfcheck.check_modified_mark(w, disp) == []
        w.lines[1] = ("H", b"0ne")
        assert selfcheck.check_screen(w, disp) != [], "the screen check compares nothing"


# @scenario SC-edit-model-04
def test_an_autosave_is_reported_with_the_text_it_wrote(tmp_path):
    """With Autosave on and Max. Changes 3, the fourth character typed
    makes the editor write the .ASV file before it reads the next key; the
    dump of that moment says so, and the file holds the dump's text."""
    rc = "[General]\nAutosave : %d\nMaxChanges : %d\n" % (AUTOSAVE_ON, AUTOSAVE_AFTER)
    with _session(tmp_path, xwperc=rc) as s:
        d = s.send(["x", "y", "z"])
        assert not d.autosaved
        d = s.send(["w"])
        assert d.autosaved, "the dump did not report the autosave"
        assert s.disk("t.c.ASV") == d.win.disk()
        assert s.disk() == SEED.encode(), "the autosave wrote the file itself"


# @scenario SC-edit-model-05
def test_the_trace_tells_a_fresh_change_from_a_replay(tmp_path):
    """Every undo record in the trace says what pushed it: a fresh change
    (replay=none), a Ctrl-U (replay=undo) or a Ctrl-R (replay=redo), so a
    count of the user's steps leaves the replays out."""
    with _session(tmp_path) as s:
        s.trace_since_last()
        s.send(["q"])
        typed = [t for t in s.trace_since_last() if t.startswith("buffer record")]
        s.send([ctrl("u")])
        undone = [t for t in s.trace_since_last() if t.startswith("buffer record")]
        s.send([ctrl("r")])
        redone = [t for t in s.trace_since_last() if t.startswith("buffer record")]
    assert typed and all("replay=none" in t for t in typed), typed
    assert undone and all("replay=undo" in t for t in undone), undone
    assert redone and all("replay=redo" in t for t in redone), redone


def _window(lines, **kw):
    """A dump window built by hand, for the self-checks' own rows."""
    fields = {"ro": 0, "mx": 120, "y": 0, "x": 0, "mb": "0,0", "me": "0,0",
              "ks": "0,0", "ins": 1, "flg": 0, "save": 0,
              "view": "0,0,1,0,20,79", "pt": "-1,-1", "brp": "-", "ud": 0, "rd": 0}
    fields.update(kw)
    w = Window(["W", "1", "t.c"] + ["%s=%s" % kv for kv in fields.items()])
    w.lines = list(lines)
    return w


# @scenario SC-edit-model-06
def test_the_self_checks_refuse_a_dump_that_breaks_them(tmp_path):
    """Each model-free check fails on a dump made to break it and passes on
    a sound one, so a green walk means the editor kept them."""
    rows = [("H", b"one"), ("H", b"two")]
    assert selfcheck.check_inside(_window(rows, y=1, x=3)) == []
    assert selfcheck.check_inside(_window(rows, y=2, x=0))[0][0] == "cursor"
    assert selfcheck.check_inside(_window(rows, x=4))[0][0] == "cursor"
    assert selfcheck.check_inside(_window(rows, mb="0,0", me="5,0"))[0][0] == "marks"
    assert selfcheck.check_inside(_window(rows, mb="0,0", me="1,4")) == []
    assert selfcheck.check_inside(_window(rows, brp="1,1"))[0][0] == "breakpoints"
    assert selfcheck.check_inside(_window(rows, brp="2"))[0][0] == "breakpoints"
    disp = [" " * 80 for _ in range(30)]
    marked = list(disp)
    marked[20] = "   *" + " " * 76
    assert selfcheck.check_modified_mark(_window(rows, save=1), disp)[0][0] == "modified"
    assert selfcheck.check_modified_mark(_window(rows, save=0), marked)[0][0] == "modified"
    assert selfcheck.check_modified_mark(_window(rows, save=1), marked) == []
    before, after = _window(rows, ud=1), _window(rows, ud=1)
    rec = ["buffer record kind=a more=0 replay=none"]
    assert selfcheck.check_trace("type_ascii", before, after, rec)[0][0][0] == "trace"
    assert selfcheck.check_trace("type_ascii", before, _window(rows, ud=2), rec)[0] == []
    undo_after = _window(rows, ud=0, rd=0)
    assert selfcheck.check_trace("undo", before, undo_after, [])[0][0][0] == "undo-lists"
    shown = list(disp)
    shown[2] = " two" + " " * 76
    assert selfcheck.check_screen(_window(rows), shown) != []
    shown[2] = " one" + " " * 76
    shown[3] = " two" + " " * 76
    assert selfcheck.check_screen(_window(rows), shown) == []


ROWS = [("H", b"one"), ("H", b"two")]
OTHER_ROWS = [("H", b"one"), ("H", b"tw0")]
STATUS_IDLE = "F1 Help  F2 Save"
STATUS_SAID = "The window is read-only"


def _undo_probe_cases():
    """(name, want, got, the class the comparison must report or None)."""
    sound = _window(ROWS, y=1, x=2, brp="1")
    return [
        ("same dump", sound, _window(ROWS, y=1, x=2, brp="1"), None),
        ("other text", sound, _window(OTHER_ROWS, y=1, x=2, brp="1"), "text"),
        ("lost row", sound, _window(ROWS[:1], y=0, x=2), "text"),
        ("breakpoint moved", sound, _window(ROWS, y=1, x=2, brp="0"), "breakpoints"),
        ("cursor moved", sound, _window(ROWS, y=0, x=2, brp="1"), "cursor"),
        ("no window", sound, None, "text"),
    ]


# @scenario SC-edit-model-06
@pytest.mark.parametrize("case", _undo_probe_cases(), ids=lambda c: c[0])
def test_the_undo_probe_comparison_refuses_a_dump_that_breaks_it(case):
    """The undo probe's comparison (check 1) reports a Ctrl-U that did not
    give back the dump from before: other text, a lost row, a breakpoint on
    another row, the cursor elsewhere, no window at all; and nothing when
    the dump is the one expected."""
    _name, want, got, cls = case
    found = [c for c, _d in selfcheck.compare_windows("after Ctrl-U", want, got)]
    if cls is None:
        assert found == []
    else:
        assert cls in found, found


# @scenario SC-edit-model-06
def test_the_lock_check_refuses_a_locked_window_that_changed():
    """Check 2: in a locked window a key that changed a row, or pushed an
    undo step, is reported; the same change in an open window, and Ctrl-U
    in a locked one, are not."""
    locked = _window(ROWS, ro=1, ud=0)
    assert selfcheck.check_lock("type_ascii", locked, _window(ROWS, ro=1)) == []
    assert selfcheck.check_lock("type_ascii", locked,
                                _window(OTHER_ROWS, ro=1))[0][0] == "lock"
    assert selfcheck.check_lock("ctrl_k_i", locked, _window(ROWS, ro=1, ud=1))[0][0] == "lock"
    assert selfcheck.check_lock("type_ascii", _window(ROWS), _window(OTHER_ROWS)) == []
    assert selfcheck.check_lock("undo", locked, _window(OTHER_ROWS, ro=1)) == []


# @scenario SC-edit-model-06
def test_the_refusal_check_wants_the_status_row_to_say_so():
    """Check 2's "says so": a text-changing key refused in a locked window
    with the status row unchanged is reported; a refusal the status row
    names, a key that is not a change, and an open window are not."""
    locked = _window(ROWS, ro=1)
    same = _window(ROWS, ro=1)
    assert selfcheck.check_says_so("type_ascii", locked, same, STATUS_IDLE,
                                   STATUS_IDLE)[0][0] == "says-so"
    assert selfcheck.check_says_so("ctrl_y_delete_line", locked, same, STATUS_IDLE,
                                   STATUS_IDLE)[0][0] == "says-so"
    assert selfcheck.check_says_so("type_ascii", locked, same, STATUS_IDLE,
                                   STATUS_SAID) == []
    assert selfcheck.check_says_so("left", locked, same, STATUS_IDLE, STATUS_IDLE) == []
    assert selfcheck.check_says_so("type_ascii", _window(ROWS), _window(ROWS),
                                   STATUS_IDLE, STATUS_IDLE) == []
    assert selfcheck.status_row(["", "  " + STATUS_IDLE + "   "]) == "  " + STATUS_IDLE


# @scenario SC-edit-model-06
def test_the_disk_and_autosave_checks_refuse_a_file_that_differs():
    """Checks 5 and 7 on disk: after Save the file must be the dump's text
    and after an autosave the .ASV file must be; a file one byte off, or
    missing, is reported."""
    w = _window(ROWS)
    right = b"one\ntwo\n"
    assert selfcheck.check_disk("save", right, w) == []
    assert selfcheck.check_disk("save", b"one\ntw0\n", w)[0][0] == "disk"
    assert selfcheck.check_disk("save", None, w)[0][0] == "disk"
    assert selfcheck.check_disk("type_ascii", b"stale", w) == []
    assert selfcheck.check_autosave(True, right, w) == []
    assert selfcheck.check_autosave(True, b"one\ntwo", w)[0][0] == "autosave"
    assert selfcheck.check_autosave(True, None, w)[0][0] == "autosave"
    assert selfcheck.check_autosave(False, None, w) == []


# @scenario SC-edit-model-06
def test_the_no_step_check_refuses_a_change_with_no_undo_step():
    """The undo probe's guard: text that changed while no undo step was
    pushed is reported, since Ctrl-U cannot give it back."""
    before, after = _window(ROWS), _window(OTHER_ROWS)
    assert selfcheck.check_no_step(before, after, 0)[0][0] == "no-step"
    assert selfcheck.check_no_step(before, after, 1) == []
    assert selfcheck.check_no_step(before, _window(ROWS), 0) == []


# @scenario SC-edit-model-07
def test_a_seeded_walk_with_the_undo_probe_is_answered_to_the_end(tmp_path):
    """A seeded walk of 40 operations with the undo probe: the editor
    answers every operation and never ends.  What the self-checks find is
    listed by the walk; this row pins only that the walk can be run."""
    w = selfcheck.Walk(str(tmp_path), 7, 40, 0.3).run()
    lost = [f for f in w.findings if f["cls"] in ("liveness", "left")]
    assert not lost, lost
    assert len(w.op_ms) == 40


# @scenario SC-edit-model-08
def test_without_the_variable_the_editor_writes_no_dump(tmp_path):
    """Unset, the variable changes nothing: a session without it gets no
    descriptor and the editor answers keys as before (the driver's own
    rows run this way)."""
    from wpe_driver import WpeSession
    with WpeSession(str(tmp_path), SEED) as s:
        s.key("k", F2, delay=0.5)
        assert s.text() == "k" + SEED
    assert "XWPE_TEST_DUMP_FD" not in os.environ


# The editor's standard input, output and error: in a session all three are
# the terminal the user looks at.
STANDARD_STREAMS = ("0", "1", "2")


# @scenario SC-edit-model-09
@pytest.mark.parametrize("fd", STANDARD_STREAMS)
def test_a_dump_never_lands_on_the_terminal(tmp_path, fd):
    """A test that names the terminal's descriptor by mistake gets no dump:
    the dump's lines would otherwise be drawn over the editor's screen."""
    from wpe_driver import WpeSession
    with WpeSession(str(tmp_path), SEED, env_extra={"XWPE_TEST_DUMP_FD": fd}) as s:
        s.key("k", F2, delay=0.5)
        shown = "\n".join(s.display())
        assert s.text() == "k" + SEED
    assert "ctx=" not in shown, "a dump was drawn on the screen"
