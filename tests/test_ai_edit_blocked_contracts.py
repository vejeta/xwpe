"""Five contracts of the Edit review that were written down but not kept.

- The assistant edits files, not its own transcript: with the AI pane focused,
  Alt-G e, c and f send nothing and the pane says there is no file here.
- An accepted edit that was saved and then undone leaves the window modified,
  so the status row carries its mark and Alt-X asks before the reverted text
  is lost; the disk keeps the saved edit until the next save.
- A terminal resize while a hunk box is open lays the box out again for the
  new size: its title and its key hint are both on screen and y still applies.
- The close glyph on the box's title row cancels the review as q does, the
  pane naming the accepted hunks that were dropped.
- A connection that dies under an Edit is reported as a transport error, and
  neither the buffer nor the disk nor a review box shows anything of it.

The first four run against the deterministic mock backend; the last one needs
a peer that hangs up, which the mock cannot do, so it reuses the loopback
fault server of test_ai_agent_gaps.py through the OpenAI-compatible backend.
"""
import os
import re
import time
import subprocess

import pytest

from wpe_driver import WpeSession, ALT, WPE_BIN
from test_ai_agent_gaps import _fault_server, _seed_openai_xwperc, _openai_env

ORIG = "int a = 1;\nint b = 2;\nint c = 3;\n"
NEW = "int a = 1;\nint b = 22;\nint c = 3;\n"
BOX = "Proposed change"
HINT = "y apply"
CHAT_REPLY = "PONGMARKER123"
# AI_NO_FILE_LINE in we_ai_ui.c, the one line the three actions share.
NO_FILE_LINE = "[AI] no file here - Edit, Complete and Multi-file work on a file window"
EDIT_TRANSPORT_LINE = "[AI edit] transport error"
NOT_SAVED_QUESTION = "NOT saved"
CLOSE_GLYPH = "\u2715"          # the close box every popup title row carries
CTRL_U = "\x15"
ALT_X = "\033x"
F2 = "\033OQ"
ESC = "\033"
MODIFIED_MARK = "*"
# e_zlsplt paints the modified mark three cells right of the window's left
# border on its status row; the file window starts at column 0.
MODIFIED_MARK_COL = 3
SMALL_COLS, SMALL_ROWS = 60, 18
TURN_POLL_S = 20


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _env(tmp_path, reply, **extra):
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_TRACE": str(tmp_path / "e.trace"),
    }
    env.update(extra)
    return env


def _trace(tmp_path):
    p = tmp_path / "e.trace"
    return p.read_text() if p.exists() else ""


def _screen(s):
    return "\n".join(s.display())


def _wait_screen(s, needle, tmo=12):
    end = time.time() + tmo
    while time.time() < end:
        s._drain(0.4)
        if needle in _screen(s):
            return True
    return False


def _wait_trace(s, tmp_path, needle, tmo=12):
    end = time.time() + tmo
    while time.time() < end:
        if needle in _trace(tmp_path):
            return True
        s._drain(0.3)
    return False


def _start_edit(s, instr="do it"):
    s.key(ALT.AI)
    s.key("e")
    s._drain(0.4)
    s.key(instr)
    s.key("\r", delay=0.8)


def _row_with(disp, needle):
    """Index of the first screen row containing needle, or -1."""
    return next((i for i, row in enumerate(disp) if needle in row), -1)


def _status_row(disp):
    """The file window's status row: its bottom border carrying line:column."""
    for i, row in enumerate(disp):
        if row.startswith("m") and re.search(r"\s\d+:\d+\s", row):
            return i
    return -1


def _disk(path):
    with open(path, "rb") as fh:
        return fh.read()


# ---------------------------------------------------------------- SC-01b
def _chat_once(s, prompt="hello"):
    """Ask once and wait for the reply: the pane is then up and focused."""
    s.key(ALT.AI)
    s.key("a")
    s.key(prompt)
    s.key("\r", delay=1.0)
    assert _wait_screen(s, CHAT_REPLY), "the chat reply never showed:\n" + _screen(s)


def _refusal_on_the_pane(s, tmp_path, letter, instr):
    """Alt-G <letter> with the pane focused: nothing is sent, the pane says
    there is no file, and the transcript is still there.  An Edit prompt that
    opens anyway is answered, so the unfixed editor shows what it does next."""
    seen = len(_trace(tmp_path))        # the chat's own lines end here
    s.key(ALT.AI)
    s.key(letter, delay=0.8)
    if "AI edit instruction" in _screen(s) or "Multi-file edit: task" in _screen(s):
        s.key(instr)
        s.key("\r", delay=0.8)
    s._drain(1.0)
    shown = _screen(s)
    txt = _trace(tmp_path)[seen:]
    assert "edit instr=" not in txt, \
        "Alt-G %s on the pane sent an edit:\n%s" % (letter, txt)
    assert "stream start" not in txt and "plan task=" not in txt, \
        "Alt-G %s on the pane opened a stream:\n%s" % (letter, txt)
    assert BOX not in shown and "Proposed completion" not in shown, \
        "Alt-G %s on the pane opened a review box over the transcript:\n%s" % (letter, shown)
    assert NO_FILE_LINE in shown, \
        "the pane does not say there is no file to edit after Alt-G %s:\n%s" % (letter, shown)
    assert CHAT_REPLY in shown, \
        "the transcript was lost after Alt-G %s on the pane:\n%s" % (letter, shown)


# @scenario SC-ai-edit-01b
def test_edit_complete_and_multifile_refuse_the_pane(tmp_path):
    with WpeSession(str(tmp_path), ORIG, env_extra=_env(tmp_path, CHAT_REPLY),
                    filename="t.c") as s:
        _chat_once(s)
        _refusal_on_the_pane(s, tmp_path, "e", "do it")
        _refusal_on_the_pane(s, tmp_path, "c", "")
        _refusal_on_the_pane(s, tmp_path, "f", "do it")
        assert s.alive(), "wpe died"
    assert _disk(tmp_path / "t.c") == ORIG.encode("utf-8"), "the file changed"


# ---------------------------------------------------------------- SC-27b
# @scenario SC-ai-edit-27b
def test_save_then_undo_marks_the_window_modified_and_quit_asks(tmp_path):
    with WpeSession(str(tmp_path), ORIG, env_extra=_env(tmp_path, NEW), filename="t.c") as s:
        _start_edit(s)
        assert _wait_screen(s, BOX), "no review box"
        s.key("y", delay=0.7)
        assert _wait_trace(s, tmp_path, "edit applied"), "the hunk was not applied"
        s.key(F2, delay=0.8)
        assert _disk(s.path) == NEW.encode("utf-8"), "F2 did not save the accepted edit"
        s.key(CTRL_U, delay=0.8)
        s._drain(0.6)
        disp = s.display()
        shown = "\n".join(disp)
        assert _row_with(disp, "int b = 2;") >= 0 and "int b = 22;" not in shown, \
            "Ctrl-U did not bring the original text back:\n" + shown
        assert _disk(s.path) == NEW.encode("utf-8"), \
            "Ctrl-U changed the disk; only the buffer reverts"
        status = _status_row(disp)
        assert status >= 0, "no status row on screen:\n" + shown
        assert disp[status][MODIFIED_MARK_COL] == MODIFIED_MARK, \
            "the status row does not show the modified mark after save then undo:\n%r\n%s" % (
                disp[status], shown)
        s.key(ALT_X, delay=0.8)
        assert _wait_screen(s, NOT_SAVED_QUESTION, tmo=6), \
            "Alt-X did not ask about the unsaved buffer:\n" + _screen(s)
        assert s.alive(), "wpe quit without asking and lost the reverted text"
        s.key(ESC, delay=0.6)
        assert s.alive(), "wpe died"
    assert _disk(tmp_path / "t.c") == NEW.encode("utf-8"), "the disk changed after Esc"


# ---------------------------------------------------------------- SC-18b
# @scenario SC-ai-edit-18b
def test_resize_relays_out_the_open_box_and_its_keys_keep_working(tmp_path):
    # A hunk that adds forty lines at the end of a five-line file: at 80x30 the
    # box opens under line 5 and takes the room down to the window's status
    # row, so a box redrawn with that geometry after the resize lands over the
    # relaid window's status row and the pane under it.
    seed = "".join("int v%02d = %d;\n" % (i, i) for i in range(5))
    reply = seed + "".join("int w%02d = %d;\n" % (i, i) for i in range(40))
    with WpeSession(str(tmp_path), seed, env_extra=_env(tmp_path, reply), filename="t.c") as s:
        _start_edit(s)
        assert _wait_screen(s, BOX), "no review box at 80x30"
        s.resize(SMALL_COLS, SMALL_ROWS, delay=1.2)
        assert s.alive(), "wpe died on the resize"
        disp = s.display()
        shown = "\n".join(disp)
        assert len(disp) == SMALL_ROWS, "the screen is not %d rows tall" % SMALL_ROWS
        title = _row_with(disp, BOX)
        hint = _row_with(disp, HINT)
        status = _status_row(disp)
        assert title >= 0, \
            "the box title is not on the %dx%d screen:\n%s" % (SMALL_COLS, SMALL_ROWS, shown)
        assert hint >= 0, \
            "the box's key hint is not on the %dx%d screen:\n%s" % (SMALL_COLS, SMALL_ROWS, shown)
        assert status >= 0, "the file window's status row is not on screen:\n" + shown
        assert title < hint < status, \
            "the box was not laid out again inside the relaid window (title row %d, hint row %d, " \
            "status row %d):\n%s" % (title, hint, status, shown)
        # The title row ends in the close glyph, a border cell and the corner;
        # a box kept at its 80-column width loses them past the right edge.
        assert CLOSE_GLYPH + "qk" in disp[title], \
            "the box's right border is off the %d-column screen:\n%s" % (SMALL_COLS, shown)
        s.key("y", delay=0.8)
        assert _wait_trace(s, tmp_path, "edit applied"), \
            "y did not apply the hunk after the resize:\n" + _trace(tmp_path)
        s.save()
        assert s.alive(), "wpe died"
    assert _disk(tmp_path / "t.c") == reply.encode("utf-8"), "the accepted hunk is not on disk"


# ---------------------------------------------------------------- SC-19c
def _click(s, col, row):
    """A left click on the 0-based screen cell (col, row), SGR encoded."""
    s.key("\033[<0;%d;%dM" % (col + 1, row + 1), delay=0.2)
    s.key("\033[<0;%d;%dm" % (col + 1, row + 1), delay=0.6)


# @scenario SC-ai-edit-19c
def test_close_glyph_cancels_the_review_like_q(tmp_path):
    seed = "int a = 1;\nint b = 2;\nint c = 3;\nint d = 4;\nint e = 5;\n"
    reply = "int a = 11;\nint b = 2;\nint c = 3;\nint d = 4;\nint e = 55;\n"
    with WpeSession(str(tmp_path), seed, env_extra=_env(tmp_path, reply), filename="t.c") as s:
        _start_edit(s)
        assert _wait_screen(s, "Proposed change 1/2"), "first hunk missing"
        s.key("y", delay=0.7)
        assert _wait_screen(s, "Proposed change 2/2"), "second hunk missing"
        disp = s.display()
        title = _row_with(disp, BOX)
        col = disp[title].rfind(CLOSE_GLYPH)
        assert col >= 0, "the box's title row carries no close glyph:\n%r" % disp[title]
        _click(s, col, title)
        s._drain(0.8)
        shown = _screen(s)
        assert BOX not in shown, "the box is still open after clicking its close glyph:\n" + shown
        assert "discarded (1 accepted hunk dropped)" in shown, \
            "the pane does not say the accepted hunk was dropped:\n" + shown
        assert "edit discarded" in _trace(tmp_path), _trace(tmp_path)
        s.save()
        assert s.alive(), "wpe died"
    assert _disk(tmp_path / "t.c") == seed.encode("utf-8"), \
        "the file changed although the review was cancelled"


# ---------------------------------------------------------------- SC-12
# @scenario SC-ai-edit-12
def test_connection_closed_under_an_edit_is_a_transport_error(tmp_path):
    captured = []
    srv, port = _fault_server(captured, ["unused"], fault_for_turn=lambda n: "close")
    try:
        _seed_openai_xwperc(tmp_path, port, "ask")
        with WpeSession(str(tmp_path), ORIG, env_extra=_openai_env(tmp_path),
                        filename="t.c") as s:
            _start_edit(s)
            end = time.time() + TURN_POLL_S
            while time.time() < end and not captured:
                s._drain(0.5)
            assert captured, "the edit request never reached the server"
            assert _wait_screen(s, EDIT_TRANSPORT_LINE, tmo=TURN_POLL_S), \
                "a dead connection under an Edit was not reported as a transport error:\n" + _screen(s)
            s._drain(1.0)
            disp = s.display()
            shown = "\n".join(disp)
            assert BOX not in shown, "a review box opened on a transport error:\n" + shown
            for line in ("int a = 1;", "int b = 2;", "int c = 3;"):
                assert _row_with(disp, line) >= 0, "the buffer lost %r:\n%s" % (line, shown)
            assert _disk(s.path) == ORIG.encode("utf-8"), "the disk changed"
            txt = (tmp_path / "ai.trace").read_text() if (tmp_path / "ai.trace").exists() else ""
            assert "edit applied" not in txt, txt
            # The failed task released its window: the editor takes text again.
            s.key("Z", delay=0.6)
            assert _wait_screen(s, "Zint a = 1;", tmo=4), \
                "the window stayed locked after the transport error:\n" + _screen(s)
            assert s.alive(), "wpe died"
    finally:
        srv.shutdown()
        srv.server_close()
    assert _disk(tmp_path / "t.c") == ORIG.encode("utf-8"), "the disk changed without a save"
