"""End-to-end journeys of an assistant change, from the prompt to the outcome.

Each surface an edit crosses (the Edit review, Complete, the Agent and its
changeset review, the chat pane, the permission dial, the prompt popup) has
its own contract tests.  These tests follow one user from the request to an
accepted or rejected change across several of them, and read at EACH step
what the user has: the screen, the buffer (saved and read back from disk),
the file on disk before any save, and the behaviour trace.  A step that is
right only at the end would hide a wrong state the user saw on the way.

All of them run against the deterministic mock backend, except the backend
error, which needs a server that fails on purpose: a local OpenAI-compatible
one on the loopback interface.
"""
import os
import re
import subprocess
import time

import pytest

from wpe_driver import (WpeSession, ALT, WPE_BIN, WAIT_SCALE, wait_screen,
                        pane_text_with_history)
from test_ai_agent_gaps import (_fault_server, _seed_openai_xwperc, _openai_env,
                                SERVER_MESSAGE)


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")

BOX = "Proposed change"
COMPLETION_BOX = "Proposed completion"
ESC = "\033"
CTRL_U = "\x15"
F3 = "\033OR"
DOWN = "\033[B"
RIGHT = "\033[C"
ALT_T = "\033t"
ALT_V = "\033v"
ALT_M = "\033m"
ACTIVE_BUTTONS = "□ ✕"
MODIFIED_MARK = "*"
TRACE_POLL_S = 12
TURN_POLL_S = 20
# The status row of the active file window: its bottom border carries the
# modified mark two cells after the corner(s), then line:column.
STATUS_RE = re.compile(r"^m+q.(.)q\s+(\d+):(\d+)\s")
STATUS_ANYWHERE_RE = re.compile(r"mq.(.)q\s+(\d+):(\d+)\s")
# A deleted row of the review box over the file window: the window border,
# blanks, then the '-' mark (an empty deleted line is the mark alone).
DELETED_ROW_RE = re.compile(r"^x\s+-")
# A shell path that cannot reach a real assistant program, so no step of a
# journey can leave the machine whatever the session's backend becomes.
SAFE_PATH = "/usr/bin:/bin"


def _env(trace, reply, **extra):
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_TRACE": str(trace),
        "PATH": SAFE_PATH,
    }
    env.update(extra)
    return env


def _trace(trace):
    return trace.read_text() if trace.exists() else ""


def _screen(s):
    return "\n".join(s.display())


def _wait_trace(s, trace, needle, tmo=TRACE_POLL_S):
    end = time.time() + tmo * WAIT_SCALE
    while time.time() < end:
        if needle in _trace(trace):
            return True
        s._drain(0.3)
    return False


def _disk(path):
    with open(path) as fh:
        return fh.read()


def _status(s):
    """(modified mark, line, column) of the active file window's status row,
    or None when no file window is active.  Only the active window's bottom
    border carries line:column, so the row is found by that, wherever the
    border starts: with several windows open the borders nest ("xmq*q 3:1")
    and the status row no longer begins at the screen's left edge."""
    for row in s.display():
        m = STATUS_ANYWHERE_RE.search(row)
        if m:
            return m.group(1), int(m.group(2)), int(m.group(3))
    return None


def _start_edit(s, instr="do it"):
    s.key(ALT.AI)
    s.key("e")
    s._drain(0.4)
    s.key(instr)
    s.key("\r", delay=0.8)


def _start_agent(s, task):
    s.key(ALT.AI)
    s.key("g")
    s.key(task)
    s.key("\r", delay=1.0)


def _ask(s, question):
    s.key(question)
    s.key("\r", delay=1.0)


FIVE = "int a = 1;\nint b = 2;\nint c = 3;\nint d = 4;\nint e = 5;\n"
FIVE_BOTH = "int a = 1;\nint b = 22;\nint c = 3;\nint d = 4;\nint e = 55;\n"
FIVE_FIRST = "int a = 1;\nint b = 22;\nint c = 3;\nint d = 4;\nint e = 5;\n"
THREE = "int a = 1;\nint b = 2;\nint c = 3;\n"
THREE_B = "int a = 1;\nint b = 22;\nint c = 3;\n"


# @scenario SC-ai-journey-01
def test_edit_accept_all_save_then_one_undo(tmp_path):
    trace = tmp_path.parent / (tmp_path.name + ".trace")
    with WpeSession(str(tmp_path), FIVE, env_extra=_env(trace, FIVE_BOTH)) as s:
        _start_edit(s)
        assert wait_screen(s, BOX + " 1/2"), "the review did not open:\n" + _screen(s)
        assert "edit instr=do it" in _trace(trace), _trace(trace)
        assert "int b = 2;" in _screen(s) and _disk(s.path) == FIVE, \
            "the review changed something before a key was pressed"

        s.key("a", delay=0.8)
        assert _wait_trace(s, trace, "edit applied 2/2"), _trace(trace)
        shown = _screen(s)
        assert "int b = 22;" in shown and "int e = 55;" in shown, shown
        assert "[AI edit] applied 2 of 2 hunks - Ctrl-U to undo" in shown, shown
        assert _status(s)[0] == MODIFIED_MARK, "the window is not marked modified:\n" + shown
        assert _disk(s.path) == FIVE, "the disk changed before Save"

        s.save()
        assert _disk(s.path) == FIVE_BOTH, "Save did not write the accepted edit"

        s.key(CTRL_U, delay=0.8)
        shown = _screen(s)
        assert "int b = 2;" in shown and "int e = 5;" in shown, shown
        assert "int b = 22;" not in shown and "int e = 55;" not in shown, \
            "one Ctrl-U did not take the whole edit back:\n" + shown
        assert _status(s)[0] == MODIFIED_MARK, \
            "the window is not marked modified after the undo:\n" + shown
        assert _disk(s.path) == FIVE_BOTH, "Ctrl-U changed the disk"
        assert _trace(trace).count("edit applied") == 1, _trace(trace)

        s.save()
        assert _disk(s.path) == FIVE, "the undone buffer is not the original file"
        assert s.alive(), "wpe died"


# @scenario SC-ai-journey-02
def test_edit_accept_first_hunk_reject_second(tmp_path):
    trace = tmp_path.parent / (tmp_path.name + ".trace")
    with WpeSession(str(tmp_path), FIVE, env_extra=_env(trace, FIVE_BOTH)) as s:
        _start_edit(s)
        assert wait_screen(s, BOX + " 1/2"), "the review did not open:\n" + _screen(s)

        s.key("y", delay=0.8)
        assert wait_screen(s, BOX + " 2/2"), "the second hunk was not shown:\n" + _screen(s)
        assert "int e = 55;" in _screen(s), _screen(s)
        assert _disk(s.path) == FIVE, "the disk changed during the review"
        assert "edit applied" not in _trace(trace), _trace(trace)

        s.key("n", delay=0.8)
        assert _wait_trace(s, trace, "edit applied 1/2"), _trace(trace)
        shown = _screen(s)
        assert "int b = 22;" in shown and "int e = 5;" in shown and "int e = 55;" not in shown, shown
        assert "[AI edit] applied 1 of 2 hunks - Ctrl-U to undo" in shown, shown
        assert _status(s)[0] == MODIFIED_MARK, shown
        assert _disk(s.path) == FIVE, "the disk changed before Save"

        s.save()
        assert _disk(s.path) == FIVE_FIRST, "the buffer is not exactly hunk 1"
        assert s.alive(), "wpe died"


# @scenario SC-ai-journey-03
def test_edit_esc_in_the_review_drops_the_accepted_hunk(tmp_path):
    trace = tmp_path.parent / (tmp_path.name + ".trace")
    with WpeSession(str(tmp_path), FIVE, env_extra=_env(trace, FIVE_BOTH)) as s:
        _start_edit(s)
        assert wait_screen(s, BOX + " 1/2"), "the review did not open:\n" + _screen(s)
        s.key("y", delay=0.8)
        assert wait_screen(s, BOX + " 2/2"), _screen(s)
        last_hunk_line = _status(s)[1]

        s.key(ESC, delay=0.9)
        assert _wait_trace(s, trace, "edit discarded (1 accepted dropped)"), _trace(trace)
        shown = _screen(s)
        assert BOX not in shown, "the review stayed open after Esc:\n" + shown
        assert "[AI edit] discarded (1 accepted hunk dropped)" in shown, shown
        assert "int b = 2;" in shown and "int b = 22;" not in shown, shown
        mark, line, _col = _status(s)
        assert mark != MODIFIED_MARK, "a cancelled review left the window modified:\n" + shown
        # The cursor stays on the last hunk shown, as Search leaves it on its
        # last match: position is not content.
        assert line == last_hunk_line, \
            "the cursor moved to line %d, the last hunk shown is on %d" % (line, last_hunk_line)
        assert _disk(s.path) == FIVE, "the disk changed"

        s.save()
        assert _disk(s.path) == FIVE, "the buffer kept part of the cancelled edit"
        assert "edit applied" not in _trace(trace), _trace(trace)


# @scenario SC-ai-journey-04
def test_complete_at_cursor_accept_then_one_undo(tmp_path):
    seed = "int x = ;\nint y = 2;\n"
    done = "int x = 42;\nint y = 2;\n"
    trace = tmp_path.parent / (tmp_path.name + ".trace")
    with WpeSession(str(tmp_path), seed, env_extra=_env(trace, "42")) as s:
        s.key(RIGHT * 8, delay=0.5)
        assert _status(s)[1:] == (1, 9), "the cursor is not after 'int x = ':\n" + _screen(s)

        s.key(ALT.AI)
        s.key("c", delay=0.8)
        assert wait_screen(s, COMPLETION_BOX), "the completion was not offered:\n" + _screen(s)
        assert "complete stream fd=" in _trace(trace) and "off=8" in _trace(trace), _trace(trace)
        assert "+int x = 42;" in _screen(s), _screen(s)
        assert _disk(s.path) == seed

        s.key("y", delay=0.8)
        assert wait_screen(s, "completed at cursor - Ctrl-U to undo"), _screen(s)
        assert "complete applied" in _trace(trace), _trace(trace)
        assert "int x = 42;" in _screen(s), _screen(s)
        assert _status(s)[0] == MODIFIED_MARK, _screen(s)
        assert _disk(s.path) == seed, "the disk changed before Save"

        s.save()
        assert _disk(s.path) == done, "the suggestion did not land at the cursor"

        s.key(CTRL_U, delay=0.8)
        shown = _screen(s)
        assert "int x = ;" in shown and "int x = 42;" not in shown, \
            "one Ctrl-U did not remove the suggestion:\n" + shown
        s.save()
        assert _disk(s.path) == seed, "the undone buffer is not the original file"


def _active_title(s):
    """The title row of the active window: the one carrying its buttons."""
    return next((r for r in s.display() if ACTIVE_BUTTONS in r), "")


# @scenario SC-ai-journey-05
def test_multifile_accept_lists_rows_and_alt_t_walks_them(tmp_path):
    other = "int u = 1;\nint v = 2;\nint w = 3;\n"
    other_new = "int u = 1;\nint v = 2;\nint w = 33;\n"
    (tmp_path / "u.c").write_text(other)
    trace = tmp_path.parent / (tmp_path.name + ".trace")
    plan = ("PROPOSE t.c\n" + THREE_B + "@@END\n"
            "PROPOSE u.c\n" + other_new + "@@END\n@@PLAN-DONE two files\n")
    with WpeSession(str(tmp_path), THREE, env_extra=_env(trace, plan)) as s:
        s.key(ALT.AI)
        s.key("f")
        s._drain(0.5)
        s.key("bump both")
        s.key("\r", delay=0.8)
        assert wait_screen(s, "Multi-file edit - 2 files"), _screen(s)
        s.key("a", delay=1.0)
        assert _wait_trace(s, trace, "plan done applied=2"), _trace(trace)
        s._drain(0.6)
        shown = _screen(s)
        assert _disk(s.path) == THREE and _disk(tmp_path / "u.c") == other, \
            "the disk changed before Save"
        assert "t.c:2: [Multi-file edit] changed" in shown, "no Messages row for t.c:\n" + shown
        assert "u.c:3: [Multi-file edit] changed" in shown, "no Messages row for u.c:\n" + shown

        s.key(ALT_T, delay=0.8)
        assert " t.c " in _active_title(s) and _status(s)[1] == 2, \
            "Alt-T did not land on t.c line 2:\n" + _screen(s)
        s.key(ALT_T, delay=0.8)
        assert " u.c " in _active_title(s) and _status(s)[1] == 3, \
            "Alt-T again did not land on u.c line 3:\n" + _screen(s)
        s.key(ALT_V, delay=0.8)
        assert " t.c " in _active_title(s) and _status(s)[1] == 2, \
            "Alt-V did not step back to t.c line 2:\n" + _screen(s)
        assert s.alive(), "wpe died"


# @scenario SC-ai-journey-06
def test_agent_under_edits_writes_then_the_review_reverts_all(tmp_path):
    ws = tmp_path / "ws"
    ws.mkdir()
    orig = "int main(void){return 0;}\n"
    t = ws / "t.c"
    trace = tmp_path / "ai.trace"
    reply = ("TOOL write_file %s\nint main(void){return 99;}\n@@END" % t
             + "@@TURN@@DONE rewrote it")
    env = _env(trace, reply, XWPE_AI_POLICY="edits",
               XDG_STATE_HOME=str(tmp_path / "state"))
    with WpeSession(str(ws), orig, env_extra=env) as s:
        _start_agent(s, "rewrite it")
        assert _wait_trace(s, trace, "changeset n=1", TURN_POLL_S), _trace(trace)
        s._drain(0.8)
        shown = _screen(s)
        txt = _trace(trace)
        assert "agent policy=edits" in txt and "checkpoint snapshot" in txt, txt
        assert "agent auto-approve write" in txt, txt
        assert _disk(t) == "int main(void){return 99;}\n", "the write did not land on disk"
        assert "return 99;" in shown, "the open window was not reloaded:\n" + shown
        assert "=== AI changeset: 1 change" in shown, "no review in Messages:\n" + shown
        assert "t.c:1: [AI] +1 -1" in shown, shown
        assert "r = REVERT all" in shown, shown

        s.key("r", delay=1.5)
        assert _wait_trace(s, trace, "changeset revert all failed=0"), _trace(trace)
        s._drain(0.6)
        shown = _screen(s)
        assert _disk(t) == orig, "revert did not put the disk back"
        assert "return 0;" in shown and "return 99;" not in shown, \
            "the window still shows the reverted text:\n" + shown
        assert "ALL changes reverted" in shown, shown
        assert s.alive(), "wpe died"


# @scenario SC-ai-journey-07
def test_agent_under_ask_write_is_denied_and_the_run_goes_on(tmp_path):
    ws = tmp_path / "ws"
    ws.mkdir()
    orig = "int main(void){return 0;}\n"
    t = ws / "t.c"
    trace = tmp_path / "ai.trace"
    reply = ("TOOL write_file %s\nint main(void){return 99;}\n@@END" % t
             + "@@TURN@@DONE nothing written")
    env = _env(trace, reply, XWPE_AI_POLICY="ask",
               XDG_STATE_HOME=str(tmp_path / "state"))
    with WpeSession(str(ws), orig, env_extra=env) as s:
        _start_agent(s, "rewrite it")
        assert wait_screen(s, " Write ", TURN_POLL_S), "the write did not ask:\n" + _screen(s)
        shown = _screen(s)
        assert "y allow" in shown and "n / Esc deny" in shown, shown
        assert "+int main(void){return 99;}" in shown, shown
        assert _disk(t) == orig, "the file was written before the answer"
        assert "agent policy=ask" in _trace(trace), _trace(trace)

        s.key("n", delay=1.0)
        assert _wait_trace(s, trace, "agent done", TURN_POLL_S), _trace(trace)
        txt = _trace(trace)
        assert "-> deny" in txt and "agent deny write_file" in txt, txt
        assert wait_screen(s, "type a follow-up", TURN_POLL_S), _screen(s)
        text = "\n".join(pane_text_with_history(s, ["[agent] denied: write_file",
                                                    "DONE nothing written"]))
        assert "[agent] denied: write_file" in text, text
        assert "DONE nothing written" in text, "the run did not go on:\n" + text
        assert _disk(t) == orig, "a denied write reached the disk"
        assert "return 0;" in _screen(s) and "return 99;" not in _screen(s), _screen(s)
        assert s.alive(), "wpe died"


# @scenario SC-ai-journey-08
# @scenario SC-ai-journey-09
def test_queued_second_edit_diffs_only_what_the_file_has(tmp_path):
    first = THREE_B
    second = "int a = 1;\nint b = 22;\nint c = 33;\n"
    delay_ms = int(5000 * WAIT_SCALE)
    trace = tmp_path.parent / (tmp_path.name + ".trace")
    env = _env(trace, first + "@@TURN@@" + second, XWPE_AI_MOCK_DELAY_MS=str(delay_ms))
    with WpeSession(str(tmp_path), THREE, env_extra=env) as s:
        _start_edit(s, "FIRST")
        _start_edit(s, "SECOND")
        assert wait_screen(s, 'queued "SECOND"', 3), "the second Edit was not queued:\n" + _screen(s)
        assert wait_screen(s, BOX + " 1/1", TURN_POLL_S), _screen(s)
        s.key("y", delay=0.8)
        assert _wait_trace(s, trace, "edit applied 1/1"), _trace(trace)
        assert _wait_trace(s, trace, "edit instr=SECOND", TURN_POLL_S), \
            "the queued Edit did not run:\n" + _trace(trace)

        assert wait_screen(s, BOX + " 1/1", TURN_POLL_S), _screen(s)
        shown = _screen(s)
        deleted = [r for r in s.display() if DELETED_ROW_RE.match(r)]
        assert "-int c = 3;" in shown and "+int c = 33;" in shown, shown
        assert len(deleted) == 1 and "deletes 2" not in shown, \
            "the review deletes a line the file does not have:\n" + shown
        s.key("y", delay=0.8)
        assert _wait_trace(s, trace, "edit applied 1/1", TURN_POLL_S) \
            and _trace(trace).count("edit applied") == 2, _trace(trace)
        s.save()
        assert _disk(s.path) == second, "the buffer is not exactly the accepted text"

        s.key(CTRL_U, delay=0.8)
        s.save()
        assert _disk(s.path) == first, "one Ctrl-U did not leave the first edit's file byte for byte"
        s.key(CTRL_U, delay=0.8)
        s.save()
        assert _disk(s.path) == THREE, "two Ctrl-U did not give the original file byte for byte"
        assert s.alive(), "wpe died"


# @scenario SC-ai-journey-10
def test_backend_error_mid_edit_then_the_next_edit_works(tmp_path):
    captured = []
    srv, port = _fault_server(captured, ["unused", THREE_B],
                              fault_for_turn=lambda n: "error_event" if n == 0 else None)
    try:
        _seed_openai_xwperc(tmp_path, port, "ask")
        trace = tmp_path / "ai.trace"
        env = dict(_openai_env(tmp_path), PATH=SAFE_PATH)
        with WpeSession(str(tmp_path), THREE, env_extra=env) as s:
            _start_edit(s, "FIRST")
            assert _wait_trace(s, trace, "edit instr=FIRST", TURN_POLL_S), _trace(trace)
            assert wait_screen(s, SERVER_MESSAGE, TURN_POLL_S), \
                "the pane does not name the cause:\n" + _screen(s)
            s._drain(1.0)
            shown = _screen(s)
            assert BOX not in shown, "a review opened on a failed stream:\n" + shown
            assert "int b = 2;" in shown, shown
            assert "edit applied" not in _trace(trace), _trace(trace)
            assert _disk(s.path) == THREE, "the disk changed"

            s.key(ESC, delay=0.8)                   # leave the pane
            s.key("Z", delay=0.6)
            assert wait_screen(s, "Zint a = 1;", 4), "the editor is not usable:\n" + _screen(s)
            s.key("\x7f", delay=0.6)                # Backspace takes the Z back
            assert wait_screen(s, "xint a = 1;", 4), _screen(s)

            _start_edit(s, "SECOND")
            assert _wait_trace(s, trace, "edit instr=SECOND", TURN_POLL_S), _trace(trace)
            assert wait_screen(s, BOX + " 1/1", TURN_POLL_S), \
                "the next Edit did not open a review:\n" + _screen(s)
            s.key("y", delay=0.8)
            assert _wait_trace(s, trace, "edit applied 1/1"), _trace(trace)
            s.save()
            assert _disk(s.path) == THREE_B, "the next Edit did not land"
            assert s.alive(), "wpe died"
    finally:
        srv.shutdown()
        srv.server_close()


# @scenario SC-ai-journey-11
def test_esc_while_an_edit_streams_then_the_next_edit_works(tmp_path):
    delay_ms = int(4000 * WAIT_SCALE)
    trace = tmp_path.parent / (tmp_path.name + ".trace")
    env = _env(trace, "NEVERSHOWN\n@@TURN@@" + THREE_B, XWPE_AI_MOCK_DELAY_MS=str(delay_ms))
    with WpeSession(str(tmp_path), THREE, env_extra=env) as s:
        _start_edit(s, "FIRST")
        assert wait_screen(s, "[AI edit] working", 3), _screen(s)
        assert "The assistant is working on this window" in s.display()[-1], _screen(s)

        s.key(ESC, delay=0.8)
        assert _wait_trace(s, trace, "edit cancelled"), _trace(trace)
        assert "[AI] cancelled" in _screen(s), _screen(s)
        s._drain(delay_ms / 1000.0 + 0.5)          # the mock's reply would land now
        shown = _screen(s)
        assert BOX not in shown and "NEVERSHOWN" not in shown, \
            "the cancelled reply surfaced:\n" + shown
        assert "int b = 2;" in shown, shown
        assert "edit applied" not in _trace(trace), _trace(trace)
        assert _disk(s.path) == THREE

        s.key(ESC, delay=0.8)                       # leave the armed pane
        _start_edit(s, "SECOND")
        assert wait_screen(s, BOX, TURN_POLL_S), "the next Edit did not open a review:\n" + _screen(s)
        assert "edit instr=SECOND" in _trace(trace), _trace(trace)
        s.key("y", delay=0.8)
        assert _wait_trace(s, trace, "edit applied 1/1"), _trace(trace)
        assert "int b = 22;" in _screen(s), _screen(s)
        s.save()
        assert _disk(s.path) == THREE_B, "the next Edit did not land"


# @scenario SC-ai-journey-15
def test_altg_y_mid_chat_keeps_the_transcript_and_shows_in_the_next_header(tmp_path):
    trace = tmp_path.parent / (tmp_path.name + ".trace")
    env = _env(trace, "ONE@@TURN@@TWO", XWPE_AI_POLICY="ask")
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n", env_extra=env) as s:
        s.key(ALT.AI)
        s.key("a", delay=0.6)
        _ask(s, "first")
        assert wait_screen(s, "AI: ONE"), _screen(s)
        assert "[AI: mock | model: mock-model | policy: ask]" in _screen(s), _screen(s)
        s.key("draft", delay=0.4)

        s.key(ALT.AI, delay=0.5)
        s.key("y", delay=0.8)
        assert _wait_trace(s, trace, "policy set edits"), _trace(trace)
        rows = s.display()
        assert "auto-accept edits, ask before commands" in rows[-1], rows[-1]
        shown = "\n".join(rows)
        assert "You: first" in shown and "AI: ONE" in shown, "the transcript was lost:\n" + shown
        assert "> draft" in shown, "the typed input was lost:\n" + shown

        s.key("\r", delay=1.0)
        assert wait_screen(s, "AI: TWO"), _screen(s)
        rows = s.display()
        idx = {k: next((i for i, r in enumerate(rows) if k in r), -1) for k in (
            "You: first", "AI: ONE", "[AI: mock | model: mock-model | policy: edits]",
            "You: draft", "AI: TWO")}
        order = [idx[k] for k in ("You: first", "AI: ONE",
                                  "[AI: mock | model: mock-model | policy: edits]",
                                  "You: draft", "AI: TWO")]
        assert -1 not in order and order == sorted(order), \
            "the new header does not precede the next turn:\n" + "\n".join(rows)
        assert "chat identity backend=mock" in _trace(trace).split("policy set edits")[1], \
            _trace(trace)


def _click(s, col, row):
    s.key("\033[<0;%d;%dM" % (col + 1, row + 1), delay=0.2)
    s.key("\033[<0;%d;%dm" % (col + 1, row + 1), delay=0.6)


# @scenario SC-ai-journey-16
def test_options_ok_changes_only_the_permission_mid_chat(tmp_path):
    trace = tmp_path.parent / (tmp_path.name + ".trace")
    env = _env(trace, "ONE@@TURN@@TWO", XWPE_AI_POLICY="ask")
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n", env_extra=env) as s:
        s.key(ALT.AI)
        s.key("a", delay=0.6)
        _ask(s, "first")
        assert wait_screen(s, "AI: ONE"), _screen(s)
        assert "[AI: mock | model: mock-model | policy: ask]" in _screen(s), _screen(s)
        s.key("draft", delay=0.4)

        s.key(ALT.OPTIONS, delay=0.5)
        s.key("i", delay=0.8)
        rows = s.display()
        y = next((i for i, r in enumerate(rows) if "Auto-accept edits" in r), -1)
        assert y >= 0, "Options > AI did not open:\n" + "\n".join(rows)
        _click(s, rows[y].index("Auto-accept edits") - 3, y)
        assert any("(*) Auto-accept edits" in r for r in s.display()), _screen(s)
        s.key(ALT.OPTIONS, delay=1.0)               # Alt-O: Ok
        options = [l for l in _trace(trace).splitlines() if l.startswith("options ")]
        assert options and "backend=mock model=mock-model policy=edits" in options[-1], \
            "Ok changed more than the permission: %s" % options
        shown = _screen(s)
        assert "You: first" in shown and "AI: ONE" in shown and "> draft" in shown, \
            "the transcript or the typed input was lost:\n" + shown

        s.key("\r", delay=1.0)
        assert wait_screen(s, "AI: TWO"), _screen(s)
        assert "[AI: mock | model: mock-model | policy: edits]" in _screen(s), \
            "the next header does not show the same backend and model:\n" + _screen(s)


# @scenario SC-ai-journey-17
def test_composer_over_the_pane_shows_its_text_at_open_and_lands_in_the_file(tmp_path):
    trace = tmp_path.parent / (tmp_path.name + ".trace")
    with WpeSession(str(tmp_path), THREE, env_extra=_env(trace, "CHATREPLY@@TURN@@" + THREE_B)) as s:
        s.key(ALT.AI)
        s.key("a", delay=0.6)
        _ask(s, "hello")
        assert wait_screen(s, "AI: CHATREPLY"), "the pane is not docked:\n" + _screen(s)
        s.key(ESC, delay=0.5)
        s.key(ALT.AI, delay=0.5)
        s.key("e", delay=0.8)
        assert wait_screen(s, "AI edit instruction", 3), _screen(s)
        s.key("bump b", delay=0.3)

        s.key(ALT_M, delay=2.0)                     # the composer; no key after it
        rows = s.display()
        shown = "\n".join(rows)
        assert any("Esc = done" in r for r in rows), "the composer did not open:\n" + shown
        assert "bump b" in shown, "the composer opened without the typed text:\n" + shown
        assert "nt b = 2;" not in shown, "the file shows through the composer:\n" + shown

        s.key(ESC, delay=0.8)
        s.key("y", delay=0.8)
        assert _wait_trace(s, trace, "edit instr=bump b", TURN_POLL_S), _trace(trace)
        assert wait_screen(s, BOX + " 1/1", TURN_POLL_S), _screen(s)
        s.key("y", delay=0.8)
        assert _wait_trace(s, trace, "edit applied 1/1"), _trace(trace)
        s.save()
        assert _disk(s.path) == THREE_B, "the reply did not land in the window the user started from"


# @scenario SC-ai-journey-12
def test_resize_with_the_review_open_then_y_applies(tmp_path):
    seed = "".join("int v%02d = %d;\n" % (i, i) for i in range(5))
    reply = seed.replace("int v02 = 2;", "int v02 = 200;")
    trace = tmp_path.parent / (tmp_path.name + ".trace")
    with WpeSession(str(tmp_path), seed, env_extra=_env(trace, reply)) as s:
        _start_edit(s)
        assert wait_screen(s, BOX + " 1/1"), _screen(s)

        s.resize(60, 18, delay=1.2)
        assert s.alive(), "wpe died on the resize"
        rows = s.display()
        shown = "\n".join(rows)
        assert len(rows) == 18, "the screen is not 18 rows tall"
        title = next((i for i, r in enumerate(rows) if BOX in r), -1)
        hint = next((i for i, r in enumerate(rows) if "y apply" in r), -1)
        assert 0 <= title < hint, "the box was not laid out again:\n" + shown
        assert all(len(r.rstrip()) <= 60 for r in rows), shown
        assert "+int v02 = 200;" in shown, shown
        assert _disk(s.path) == seed and "edit applied" not in _trace(trace)

        s.key("y", delay=0.8)
        assert _wait_trace(s, trace, "edit applied 1/1"), _trace(trace)
        shown = _screen(s)
        assert BOX not in shown and "int v02 = 200;" in shown, shown
        assert _disk(s.path) == seed, "the disk changed before Save"
        s.save()
        assert _disk(s.path) == reply


# @scenario SC-ai-journey-13
def test_edit_in_the_second_window_changes_only_that_window(tmp_path):
    other = "int u = 1;\nint v = 2;\n"
    other_new = "int u = 1;\nint v = 22;\n"
    (tmp_path / "u.c").write_text(other)
    trace = tmp_path.parent / (tmp_path.name + ".trace")
    with WpeSession(str(tmp_path), THREE, env_extra=_env(trace, other_new)) as s:
        s.key(F3, delay=1.0)
        s.key("u.c", delay=0.3)
        s.key("\r", delay=1.0)
        assert " u.c " in s.display()[2], "u.c did not open in a second window:\n" + _screen(s)

        _start_edit(s)
        assert wait_screen(s, BOX + " 1/1"), _screen(s)
        assert "+int v = 22;" in _screen(s), _screen(s)

        s.key("y", delay=0.8)
        assert _wait_trace(s, trace, "edit applied 1/1"), _trace(trace)
        shown = _screen(s)
        assert "int v = 22;" in shown, shown
        assert _status(s)[0] == MODIFIED_MARK, shown
        assert _disk(tmp_path / "u.c") == other and _disk(s.path) == THREE

        s.save()
        assert _disk(tmp_path / "u.c") == other_new, "the second window was not saved"
        assert _disk(s.path) == THREE, "the first file changed"
        s.key("\x17", delay=0.8)                    # Ctrl-W closes u.c
        rows = s.display()
        assert " t.c " in rows[1], "t.c is not the window under u.c:\n" + "\n".join(rows)
        assert rows[2].startswith("xint a = 1;") and rows[3].startswith("xint b = 2;"), \
            "the first window's text changed:\n" + "\n".join(rows)
        assert _disk(s.path) == THREE, "the first file changed"


# @scenario SC-ai-journey-14
def test_edit_on_a_marked_block_changes_only_the_block(tmp_path):
    seed = "int a = 1;\nint b = 2;\nint c = 3;\nint d = 4;\n"
    region = "int b = 20;\nint c = 30;\n"
    spliced = "int a = 1;\nint b = 20;\nint c = 30;\nint d = 4;\n"
    trace = tmp_path.parent / (tmp_path.name + ".trace")
    with WpeSession(str(tmp_path), seed, env_extra=_env(trace, region)) as s:
        s.key(DOWN)
        s.key(ALT.BLOCK, "b", delay=0.4)
        s.key(DOWN)
        s.key(DOWN)
        s.key(ALT.BLOCK, "e", delay=0.4)            # lines 2-3 marked

        _start_edit(s)
        assert _wait_trace(s, trace, "sel=1..2"), _trace(trace)
        assert wait_screen(s, BOX + " 1/1"), _screen(s)
        shown = _screen(s)
        assert "-int b = 2;" in shown and "+int c = 30;" in shown, shown
        assert "-int a = 1;" not in shown and "-int d = 4;" not in shown, shown

        s.key("a", delay=0.8)
        assert _wait_trace(s, trace, "edit applied 1/1"), _trace(trace)
        shown = _screen(s)
        assert "int b = 20;" in shown and "int a = 1;" in shown and "int d = 4;" in shown, shown
        assert _disk(s.path) == seed, "the disk changed before Save"
        s.save()
        assert _disk(s.path) == spliced, "more than the block changed"

        s.key(CTRL_U, delay=0.8)
        s.save()
        assert _disk(s.path) == seed, "one Ctrl-U did not take the block edit back"


def _lines(prefix, n):
    return "".join("%s%d;\n" % (prefix, i) for i in range(1, n + 1))


# @scenario SC-ai-journey-05
def test_alt_t_walks_the_rows_in_order_when_a_later_file_has_a_lower_line(tmp_path):
    """Three changed files at lines 5, 2 and 9: Alt-T steps t.c:5, u.c:2,
    v.c:9 and wraps to t.c:5; Alt-V steps back to v.c:9.  Stepping by bare
    line number would skip u.c, whose line is lower than t.c's."""
    t_old, u_old, v_old = _lines("int t", 6), _lines("int u", 3), _lines("int v", 10)
    t_new = t_old.replace("int t5;", "int t5 = 5;")
    u_new = u_old.replace("int u2;", "int u2 = 2;")
    v_new = v_old.replace("int v9;", "int v9 = 9;")
    (tmp_path / "u.c").write_text(u_old)
    (tmp_path / "v.c").write_text(v_old)
    trace = tmp_path.parent / (tmp_path.name + ".trace")
    plan = ("PROPOSE t.c\n" + t_new + "@@END\n"
            "PROPOSE u.c\n" + u_new + "@@END\n"
            "PROPOSE v.c\n" + v_new + "@@END\n@@PLAN-DONE three files\n")
    with WpeSession(str(tmp_path), t_old, env_extra=_env(trace, plan)) as s:
        s.key(ALT.AI)
        s.key("f")
        s._drain(0.5)
        s.key("set them")
        s.key("\r", delay=0.8)
        assert wait_screen(s, "Multi-file edit - 3 files"), _screen(s)
        s.key("a", delay=1.0)
        assert _wait_trace(s, trace, "plan done applied=3"), _trace(trace)
        s._drain(0.6)
        shown = _screen(s)
        for row in ("t.c:5:", "u.c:2:", "v.c:9:"):
            assert row in shown, "no Messages row %s:\n%s" % (row, shown)
        walk = []
        for _ in range(4):
            s.key(ALT_T, delay=0.8)
            walk.append((_active_title(s).strip(" q"), _status(s)[1]))
        s.key(ALT_V, delay=0.8)
        back = (_active_title(s).strip(" q"), _status(s)[1])
    assert [w[1] for w in walk] == [5, 2, 9, 5], "Alt-T did not step the rows in order: %r" % walk
    assert [n in w[0] for w, n in zip(walk, ("t.c", "u.c", "v.c", "t.c"))] == [True] * 4, \
        "Alt-T landed in the wrong files: %r" % walk
    assert "v.c" in back[0] and back[1] == 9, "Alt-V did not step back to v.c:9: %r" % (back,)
