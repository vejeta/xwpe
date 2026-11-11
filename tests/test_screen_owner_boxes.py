"""The screen has one owner: the boxes read their keys through it.

A dialog, a dropdown, an error box, a picker or one of the assistant's boxes
owns the screen while it is up and reads every key through the screen's
owner.  What arrives meanwhile (a reply, program output, a waiting line) is
not drawn on it and is shown, once, when the editor is back; a resize lays
the desk out again and the box with it.  These rows drive the real editor
(wpe, terminal mode) and read the screen and the editor's trace
(XWPE_UI_TRACE).
"""
import re
import time

import pytest

import test_screen_owner as owner
from wpe_driver import WpeSession, ALT, WAIT_SCALE, assistant_build

ESC = "\033"
F10 = "\033[21~"
ALT_F5 = "\033[15;3~"
CTRL_G = "\x07"
POLL_S = 0.2
TRACE = "ui.trace"
AI_TRACE = "ai.trace"
HOLD_S = 2.0

SEED = "int main(void)\n{\n  return 0;\n}\n"

# pyte shows the DEC line-drawing set as its ASCII code points.
CORNER_BL = "m"


def _screen(w):
    return "\n".join(w.display(drain=0))


def _read(path):
    try:
        with open(path) as fh:
            return fh.read()
    except OSError:
        return ""


def _wait_for(w, predicate, timeout):
    deadline = time.time() + timeout * WAIT_SCALE
    while time.time() < deadline:
        w._drain(POLL_S)
        if predicate():
            return True
    return predicate()


def _need_assistant():
    if not assistant_build():
        pytest.skip("wpe built without the assistant")


# ---- a waiting line said from the loop -----------------------------------

# The stand-in server of tests/test_screen_owner.py, changed to keep its
# answer to `initialize` for ever: the editor waits for it to start and says
# so on the status row every second.
STAND_IN_ANSWER = '    result = {"capabilities": {}} if method == "initialize" else None\n'
STAND_IN_STUCK = ('    if method == "initialize":\n'
                  '        note("HELD initialize")\n'
                  '        return True\n')
WAITING_START_RE = re.compile(r"waiting for clangd to start \.\.\. (\d+)s")
USER_SCREEN_PROMPT = "press any key to return to the editor"
HELD_S = 20
WAITING_S = 10
UNDER_USER_SCREEN_S = 4.0


def _stand_in_that_never_starts(tmp_path):
    """The stand-in clangd on PATH, which never answers `initialize`."""
    env = owner._stand_in_server(tmp_path)
    script = tmp_path / "bin" / "clangd"
    text = script.read_text()
    assert STAND_IN_ANSWER in text, "the stand-in server changed its answer"
    script.write_text(text.replace(STAND_IN_ANSWER,
                                   STAND_IN_STUCK + STAND_IN_ANSWER))
    env["XWPE_LSP_NO_EAGER"] = "1"
    return env


def _waiting_start_seconds(w):
    """The seconds the start's waiting line shows, or -1."""
    found = WAITING_START_RE.search(_screen(w))
    if not found:
        return -1
    return int(found.group(1))


# @scenario SC-screen-05
def test_a_waiting_line_from_the_loop_under_the_user_screen_waits_for_it(tmp_path):
    """Alt-Q E starts a language server that never answers: every second
    the status row says how long the editor has waited.  Alt-F5 shows the
    User Screen; while it is up the waiting line is not drawn (on the
    terminal that would bring the editor's screen back over the program's),
    and once a key leaves it the line is there again with the seconds of
    now, not those of when the User Screen opened."""
    env = _stand_in_that_never_starts(tmp_path)
    try:
        with WpeSession(str(tmp_path), owner.SEED, filename="a.c", wait=1.5,
                        env_extra=env) as w:
            w.key("\033q", delay=0.4)
            w.key("e", delay=0.2)
            assert _wait_for(w, lambda: "HELD initialize"
                             in _read(str(tmp_path / owner.STUB_LOG)), HELD_S), \
                "Alt-Q E should start the stand-in server:\n%s" % _screen(w)
            assert _wait_for(w, lambda: _waiting_start_seconds(w) >= 1,
                             WAITING_S), \
                "the status row should say the editor waits for the " \
                "server:\n%s" % _screen(w)
            w.key(ALT_F5, delay=0.8)
            assert _wait_for(w, lambda: USER_SCREEN_PROMPT in _screen(w),
                             WAITING_S), \
                "Alt-F5 should show the User Screen:\n%s" % _screen(w)
            opened_at = _waiting_start_seconds(w)
            w._drain(UNDER_USER_SCREEN_S)
            under = _screen(w)
            assert USER_SCREEN_PROMPT in under and \
                not WAITING_START_RE.search(under), \
                "the waiting line was drawn over the User Screen:\n%s" % under
            w.key("\r", delay=0.8)
            assert _wait_for(w, lambda: _waiting_start_seconds(w) >=
                             opened_at + int(UNDER_USER_SCREEN_S), WAITING_S), \
                "back in the editor the waiting line should count on " \
                "(it said %ds when the User Screen opened):\n%s" \
                % (opened_at, _screen(w))
            assert w.alive(), "wpe died leaving the User Screen"
    finally:
        owner._end_the_stand_in(tmp_path)


DEFERRED_WAITING = "paint deferred a waiting line"
REPLAYED_WAITING = "paint replay a waiting line"


# @scenario SC-screen-01
def test_a_waiting_line_from_the_loop_under_a_dialog_waits_for_it(tmp_path):
    """The status row counts the seconds the editor has waited for a
    language server that never starts.  Search > Find opens its dialog:
    while it is up nothing on the screen changes, the seconds included,
    and the trace says the line waited.  Once Esc closes the dialog the line
    is drawn again with the seconds of now."""
    env = _stand_in_that_never_starts(tmp_path)
    try:
        with WpeSession(str(tmp_path), owner.SEED, filename="a.c", wait=1.5,
                        env_extra=env) as w:
            w.key("\033q", delay=0.4)
            w.key("e", delay=0.2)
            assert _wait_for(w, lambda: _waiting_start_seconds(w) >= 1,
                             HELD_S), \
                "the status row should say the editor waits for the " \
                "server:\n%s" % _screen(w)
            w.menu(ALT.SEARCH, "f", delay=0.8)
            assert _wait_for(w, lambda: "Find" in _screen(w), WAITING_S), \
                "Search > Find should open its dialog:\n%s" % _screen(w)
            w._drain(0.5)
            before = w.display(drain=0)
            opened_at = _waiting_start_seconds(w)
            w._drain(UNDER_USER_SCREEN_S)
            after = w.display(drain=0)
            assert after == before, \
                "nothing may be drawn while the dialog is up:\nbefore:\n%s" \
                "\nafter:\n%s" % ("\n".join(before), "\n".join(after))
            assert DEFERRED_WAITING in _read(str(tmp_path / TRACE)), \
                "the waiting line said under the dialog should wait for " \
                "it:\n%s" % _read(str(tmp_path / TRACE))
            w.key(ESC, delay=0.8)
            assert _wait_for(w, lambda: _waiting_start_seconds(w) >=
                             opened_at + int(UNDER_USER_SCREEN_S), WAITING_S), \
                "once the dialog closes the waiting line should count on " \
                "(it said %ds when the dialog opened):\n%s" \
                % (opened_at, _screen(w))
            assert REPLAYED_WAITING in _read(str(tmp_path / TRACE)), \
                _read(str(tmp_path / TRACE))
            assert w.alive(), "wpe died closing the dialog"
    finally:
        owner._end_the_stand_in(tmp_path)


# ---- a closed window and its language server -----------------------------

CTRL_W = "\x17"
PUBLISHED_S = 10


# @scenario SC-screen-02
def test_a_closed_window_gets_no_news_from_its_language_server(tmp_path):
    """The language server is started from a.c, then the user closes a.c
    and Messages is on top.  The server publishes its problems for the
    file: nothing is drawn into the closed window (under AddressSanitizer
    that was a use after free) and the editor goes on."""
    env = owner._stand_in_server(tmp_path)
    try:
        with WpeSession(str(tmp_path), owner.SEED, filename="a.c", wait=1.5,
                        env_extra=env) as w:
            w.key("\033q", delay=0.4)
            w.key("e", delay=0.2)
            assert _wait_for(w, lambda: "NOTE textDocument/didOpen"
                             in _read(str(tmp_path / owner.STUB_LOG)),
                             owner.LSP_STARTED_S), \
                "Alt-Q E should start the stand-in server:\n%s" % _screen(w)
            w._drain(1.0)
            assert "Messages" in _screen(w), \
                "the start should open Messages:\n%s" % _screen(w)
            w.key(CTRL_W, delay=0.8)
            assert "a.c" not in _screen(w).splitlines()[1], \
                "Ctrl-W should close a.c:\n%s" % _screen(w)
            owner._release_the_stand_in(tmp_path)
            assert _wait_for(w, lambda: "PUBLISHED"
                             in _read(str(tmp_path / owner.STUB_LOG)),
                             PUBLISHED_S), "the stand-in should have published"
            w._drain(HOLD_S)
            assert w.alive(), \
                "wpe died when the server published for a closed window " \
                "(exit %r)" % (w.exit_code(),)
            w.key(ESC, delay=0.4)
            assert w.alive(), "wpe died after the publication"
    finally:
        owner._end_the_stand_in(tmp_path)


# ---- boxes inside boxes, and keys between --------------------------------

SPINNER_SEED = "int a = 1;\nint b = 2;\nint c = 3;\n"
SPINNER_REPLY = "int a = 1;\nint b = 22;\nint c = 3;\n"
LONG_REPLY_MS = int(30000 * WAIT_SCALE)
DEFERRED_PANE = "paint deferred the assistant's pane"
REPLAYED_PANE = "paint replay the assistant's pane"
WORKING_S = 15
AI_OPTIONS_TITLE = "AI"
PICKER_TITLE = "AI providers"


def _assistant_env(tmp_path, reply, delay_ms):
    return {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
            "XWPE_AI_MOCK_REPLY": reply,
            "XWPE_AI_MOCK_DELAY_MS": str(delay_ms),
            "XWPE_AI_TRACE": str(tmp_path / AI_TRACE),
            "XWPE_UI_TRACE": str(tmp_path / TRACE)}


def _ask_for_an_edit(w):
    w.key(ALT.AI)
    w.key("e")
    w._drain(0.4)
    w.key("change b")
    w.key("\r", delay=0.6)


def _replays(tmp_path, line):
    return _read(str(tmp_path / TRACE)).count(line)


# @scenario SC-screen-03
def test_two_nested_boxes_closed_in_order_replay_once(tmp_path):
    """The assistant works on an edit and its waiting line ticks.  Options
    > AI opens a dialog and Alt-V the provider picker inside it: the ticks
    wait.  Esc closes the picker and the dialog is still up: nothing is
    drawn and nothing is replayed.  Esc closes the dialog: what waited is
    drawn once."""
    _need_assistant()
    env = _assistant_env(tmp_path, SPINNER_REPLY, LONG_REPLY_MS)
    with WpeSession(str(tmp_path), SPINNER_SEED, filename="t.c", wait=1.5,
                    env_extra=env) as w:
        _ask_for_an_edit(w)
        assert _wait_for(w, lambda: "working" in _screen(w), WORKING_S), \
            "the assistant's waiting line should show:\n%s" % _screen(w)
        w.key(ALT.OPTIONS, delay=0.5)
        w.key("i", delay=0.8)
        w.key("\033v", delay=0.8)
        assert _wait_for(w, lambda: PICKER_TITLE in _screen(w), WORKING_S), \
            "Alt-V should open the provider picker in the dialog:\n%s" \
            % _screen(w)
        replayed_before = _replays(tmp_path, REPLAYED_PANE)
        deferred_before = _replays(tmp_path, DEFERRED_PANE)
        assert _wait_for(w, lambda: _replays(tmp_path, DEFERRED_PANE)
                         > deferred_before, WORKING_S), \
            "a tick under the boxes should wait:\n%s" \
            % _read(str(tmp_path / TRACE))
        w.key(ESC, delay=0.8)
        assert PICKER_TITLE not in _screen(w), \
            "Esc should close the picker:\n%s" % _screen(w)
        w._drain(HOLD_S)
        before = w.display(drain=0)
        w._drain(HOLD_S)
        assert w.display(drain=0) == before, \
            "nothing may be drawn while the dialog is still up:\n%s" \
            % _screen(w)
        assert _replays(tmp_path, REPLAYED_PANE) == replayed_before, \
            "closing the inner box must not replay what waited:\n%s" \
            % _read(str(tmp_path / TRACE))
        w.key(ESC, delay=1.0)
        assert _wait_for(w, lambda: _replays(tmp_path, REPLAYED_PANE)
                         > replayed_before, WORKING_S), \
            "closing the dialog should draw what waited:\n%s" \
            % _read(str(tmp_path / TRACE))
        w._drain(HOLD_S)
        assert _replays(tmp_path, REPLAYED_PANE) == replayed_before + 1, \
            "what waited behind two boxes is drawn once:\n%s" \
            % _read(str(tmp_path / TRACE))
        assert w.alive(), "wpe died closing the boxes"


REPLY_LANDS_MS = int(3000 * WAIT_SCALE)
REVIEW_BOX = "Proposed change"
TYPED = "abcdefgh"
KEY_GAP_S = 0.6
REPLY_HELD = "reply held"
REVIEW_S = 15


# @scenario SC-screen-01
def test_a_reply_that_arrives_between_two_keys_of_a_dialog_is_not_drawn_on_it(tmp_path):
    """The Find dialog is open and the user types into it, one key at a
    time, while the assistant's reply lands.  The dialog reads each key
    through the screen's owner, so between two keys the screen is still the
    dialog's: the review box does not open over it, and it opens once Esc
    closes the dialog."""
    _need_assistant()
    env = _assistant_env(tmp_path, SPINNER_REPLY, REPLY_LANDS_MS)
    with WpeSession(str(tmp_path), SPINNER_SEED, filename="t.c", wait=1.5,
                    env_extra=env) as w:
        _ask_for_an_edit(w)
        w.menu(ALT.SEARCH, "f", delay=0.6)
        assert _wait_for(w, lambda: "Find" in _screen(w), WORKING_S), \
            "Search > Find should open its dialog:\n%s" % _screen(w)
        deadline = time.time() + REVIEW_S * WAIT_SCALE
        typed = 0
        while time.time() < deadline and typed < len(TYPED):
            w.key(TYPED[typed], delay=KEY_GAP_S)
            typed += 1
            assert REVIEW_BOX not in _screen(w), \
                "the review box opened over the dialog:\n%s" % _screen(w)
        assert _wait_for(w, lambda: REPLY_HELD
                         in _read(str(tmp_path / AI_TRACE)), REVIEW_S), \
            "the reply should land while the dialog is up:\n%s" \
            % _read(str(tmp_path / AI_TRACE))
        w.key("i", delay=KEY_GAP_S)
        assert REVIEW_BOX not in _screen(w), \
            "the review box opened over the dialog:\n%s" % _screen(w)
        w.key(ESC, delay=1.0)
        assert _wait_for(w, lambda: REVIEW_BOX in _screen(w), REVIEW_S), \
            "once the dialog closes the review should open:\n%s" % _screen(w)
        w.key("n", delay=0.8)
        assert w.alive(), "wpe died after the review"


# ---- a resize under a box ------------------------------------------------

START_COLS, START_ROWS = 80, 30
OTHER_COLS = 90
FILE_MENU_ROW = "Save aLl"        # a row of the File dropdown
DROPDOWN_COLS = 30                # the File dropdown lies left of this column
STATUS_TEXT = "F1 Help"


def _dropdown_bottom(rows):
    """The last screen row the File dropdown covers: the row below its last
    item, which holds its bottom frame."""
    last = None
    for i, row in enumerate(rows):
        if "Quit" in row[:DROPDOWN_COLS]:
            last = i
    return None if last is None else last + 1


def _without_last_cell(rows):
    """The screen without its bottom-right cell, which curses never writes
    (writing it would scroll the terminal), so it keeps whatever an earlier
    size left there."""
    return rows[:-1] + [rows[-1][:-1]]


LEFT = "\033[D"
ABOUT_TEXT = "Fred Kruse"         # a line of the About box
ABOUT_ROWS = 24                   # the About box (rows 3 to 20) still fits


def _rows_under_the_dropdown(rows):
    """Shrink until the window's bottom edge lies under the dropdown."""
    bottom = _dropdown_bottom(rows)
    assert bottom is not None, \
        "Alt-F should open the File dropdown:\n%s" % "\n".join(rows)
    return bottom + 2


def _rows_for_the_about_box(rows):
    return ABOUT_ROWS


# What each box is opened with, the text that shows it is up, and the
# height the terminal is shrunk to while it is.
BOXES = {
    "the File dropdown": ((ALT.FILE,), FILE_MENU_ROW, _rows_under_the_dropdown),
    "the About box": ((ALT.FILE, LEFT, "a"), ABOUT_TEXT, _rows_for_the_about_box),
}


# @scenario SC-screen-04
@pytest.mark.parametrize("box", sorted(BOXES))
def test_resize_under_a_box_repaints_the_desk_and_the_box(tmp_path, box):
    """A box is open when the terminal shrinks (for the dropdown, so that
    the editor window's bottom edge moves under it).  The desk is laid out
    for the new size, the box is drawn again, and once Esc closes it the
    screen is exactly what the editor draws at that size with no box: not
    the cells saved behind the box before the resize."""
    keys, shown, shrink_to = BOXES[box]
    with WpeSession(str(tmp_path), SEED, filename="t.c",
                    cols=START_COLS, rows=START_ROWS, wait=1.3) as w:
        for k in keys:
            w.key(k, delay=0.6)
        before = w.display(drain=0.3)
        assert any(shown in r for r in before), \
            "%s should be open:\n%s" % (box, "\n".join(before))
        rows = shrink_to(before)
        w.resize(START_COLS, rows, delay=1.2)
        assert w.alive(), "wpe died on the resize under %s" % box
        under = w.display(drain=0.3)
        assert any(STATUS_TEXT in r for r in under[rows - 1:]), \
            "the status line should be on the new last row %d:\n%s" \
            % (rows - 1, "\n".join(under))
        assert any(shown in r for r in under), \
            "%s should be drawn again after the resize:\n%s" \
            % (box, "\n".join(under))
        w.key(ESC, delay=0.8)
        closed = w.display(drain=0.3)
        assert not any(shown in r for r in closed), \
            "Esc should close %s:\n%s" % (box, "\n".join(closed))
        w.resize(OTHER_COLS, rows, delay=1.0)
        w.resize(START_COLS, rows, delay=1.2)
        fresh = w.display(drain=0.3)
        assert _without_last_cell(closed) == _without_last_cell(fresh), \
            "after the dropdown closed the screen should be the desk at the " \
            "new size, not the cells saved before the resize:\nafter Esc:\n%s" \
            "\nthe desk drawn afresh:\n%s" % ("\n".join(closed), "\n".join(fresh))
