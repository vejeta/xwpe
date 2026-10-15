"""Six chat-pane contracts that previously had only a scratch probe behind
them: opening the pane, sharing the strip with Messages, reply formatting,
where the caret parks, Esc leaving to the home window, and a long session.

* SC-ai-chat-01  Alt-G a opens straight into the input row, no popup, and
  Ctrl-J inserts a newline in the row.  (we_ai_ui.c: e_ai_chat, "Open
  straight into the pane's input row -- no popup"; e_ai_chat_key, the
  WPE_WR branch.)
* SC-ai-chat-03  When Messages is already docked, opening the pane shares
  the strip with it instead of covering it.  (we_wind.c:
  e_position_ai_window / e_place_strip_pair.)
* SC-ai-chat-16  A reply's empty lines are kept, its '\\r' bytes are
  dropped, and a fenced code block is shown verbatim.  (we_ai_ui.c: the
  streaming paint loop, the '\\r' skip and the per-'\\n' commit.)
* SC-ai-chat-17  Where the caret parks once a reply ends: in the input row
  when the pane is the focused window, in the user's own window while an
  Edit runs in the background.  (we_ai_ui.c: ai_pane_place_view,
  ai_pane_is_focused.)
* SC-ai-chat-20  Esc closes the input and hands focus back to the window
  the chat was armed from, which Alt-G e then acts on.  (we_ai_ui.c:
  e_ai_chat_key's WPE_ESC branch, e_ai_chat_close.)
* SC-ai-chat-48  A long session (300 turns) drops no transcript line; it is
  gated behind XWPE_TEST_LONG so the default suite stays fast.

The mock backend makes every reply deterministic; nothing here talks to a
live provider.
"""
import os
import shutil
import subprocess
import time

import pytest
from wpe_driver import (
    ALT,
    WPE_BIN,
    WpeSession,
    pane_frame,
    pane_logical_lines,
    pane_rows,
    title_row,
)

SEED = "int main(void){return 0;}\n"
ESC = "\033"
CTRL_J = "\n"
F9 = "\033[20~"
PGUP = "\033[5~"
LONG_TEST_ENV_VAR = "XWPE_TEST_LONG"

WAIT_SCALE = float(os.environ.get("XWPE_TEST_WAIT_SCALE", "1") or 1)


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _env(tmp_path, reply, **more):
    e = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_TRACE": str(tmp_path / "ai.trace"),
    }
    e.update(more)
    return e


def _trace(tmp_path):
    t = tmp_path / "ai.trace"
    return t.read_text() if t.exists() else ""


def _traced_within(tmp_path, needle, timeout):
    """Poll the trace file until it holds `needle` or `timeout` (scaled) passes."""
    end = time.time() + timeout * WAIT_SCALE
    while time.time() < end:
        if needle in _trace(tmp_path):
            return True
        time.sleep(0.2)
    return needle in _trace(tmp_path)


def _seen_within(s, needle, timeout):
    """Drain the screen until a row holds `needle` or `timeout` (scaled) passes."""
    end = time.time() + timeout * WAIT_SCALE
    while time.time() < end:
        s._drain(0.25)
        if needle in "\n".join(s.display(drain=0)):
            return True
    return False


def _row_index(rows, needle):
    return next((i for i, r in enumerate(rows) if needle in r), -1)


def _exact_row(rows, text):
    """Index of the first row whose stripped text equals `text` exactly, -1
    if none -- used for the empty "> " input row, which a substring search
    would also match inside a longer row."""
    return next((i for i, r in enumerate(rows) if r.rstrip() == text), -1)


def _arm_chat(s):
    """Open the chat pane straight into its input row (Alt-G, a)."""
    s.key(ALT.AI)
    s.key("a", delay=0.6)


# @scenario SC-ai-chat-01
def test_alt_g_a_opens_input_row_with_no_popup_and_ctrl_j_inserts_newline(tmp_path):
    """Alt-G a drops straight into the pane's "> " input row: no popup box
    opens (the old wide prompt with a Multi-line button is gone), and a
    Ctrl-J in the row inserts a newline rather than sending.
    """
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path, "SMOKEREPLY")) as s:
        _arm_chat(s)
        fresh = s.display()
        s.key("ab")
        s.key(CTRL_J)
        s.key("cd")
        after_cj = s.display()

    assert title_row(fresh, "AI") >= 0, \
        "Alt-G a did not open the AI pane:\n" + "\n".join(fresh)
    assert not any("Multi-line" in r for r in fresh), \
        "a popup box opened instead of the input row:\n" + "\n".join(fresh)
    assert not any("Alt-S=Send" in r for r in fresh), \
        "a popup box opened instead of the input row:\n" + "\n".join(fresh)
    assert _exact_row(pane_rows(fresh), ">") >= 0, \
        "the empty input row is not ready after Alt-G a:\n" + "\n".join(fresh)

    cj_rows = pane_rows(after_cj)
    idx = _exact_row(cj_rows, "> ab")
    assert idx >= 0 and idx + 1 < len(cj_rows) and cj_rows[idx + 1].rstrip() == "  cd", \
        "Ctrl-J did not insert a newline in the input row:\n" + "\n".join(after_cj)


# @scenario SC-ai-chat-03
def test_messages_already_open_shares_the_strip_with_the_pane(tmp_path):
    """When Messages is already docked (a compile ran first), opening the
    pane with Alt-G a splits the strip with it -- Messages on the left half,
    the pane on the right -- instead of one covering the other.
    """
    if not shutil.which("gcc"):
        pytest.skip("gcc not installed")
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path, "SPLITREPLY"),
                    filename="t.c") as s:
        s.key(F9, delay=3.0)                   # Run -> Make: opens Messages
        s._drain(1.0)
        before = s.display()
        assert title_row(before, "Messages") >= 0, \
            "Messages did not open after F9:\n" + "\n".join(before)

        _arm_chat(s)
        s.key("hi")
        s.key("\r", delay=1.0)
        assert _seen_within(s, "SPLITREPLY", 5), \
            "the reply never arrived:\n" + "\n".join(s.display())
        disp = s.display()

    msg_y = title_row(disp, "Messages")
    ai_y = title_row(disp, "AI")
    assert msg_y >= 0, "Messages is gone:\n" + "\n".join(disp)
    assert ai_y >= 0, "Messages covered the AI pane:\n" + "\n".join(disp)
    assert msg_y == ai_y, \
        "Messages (row %d) and the pane (row %d) do not share the split row:\n%s" \
        % (msg_y, ai_y, "\n".join(disp))
    assert disp[msg_y].index(" Messages ") < disp[ai_y].index(" AI "), \
        "Messages is not left of the pane:\n" + "\n".join(disp)
    _, ai_x0, _ = pane_frame(disp)
    assert ai_x0 == 40, \
        "the pane's left border is not at the shared split column 40 (got %d):\n%s" \
        % (ai_x0, "\n".join(disp))
    assert "SPLITREPLY" in "\n".join(pane_logical_lines(pane_rows(disp))), \
        "the reply is not inside the pane:\n" + "\n".join(disp)


FENCED_REPLY = "line one\n\nline three\n```c\nint x;\n```\r\nend"


# @scenario SC-ai-chat-16
def test_blank_lines_and_fences_are_shown_verbatim(tmp_path):
    """A reply's empty lines are kept, a '\\r' byte is dropped (never shown),
    and a fenced code block is painted verbatim -- the fence lines and the
    code inside them are not reformatted or merged with their neighbours.
    """
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path, FENCED_REPLY)) as s:
        _arm_chat(s)
        s.key("show")
        s.key("\r", delay=1.0)
        assert _traced_within(tmp_path, "chat done", 10), _trace(tmp_path)
        s._drain(1.0)
        disp = s.display()

    rows = [r.rstrip() for r in pane_rows(disp)]
    one = _row_index(rows, "AI: line one")
    assert one >= 0, "the first reply line never painted:\n" + "\n".join(disp)
    assert rows[one + 1] == "", \
        "the empty line between the first and third was not kept: %r" % rows[one + 1]
    assert rows[one + 2] == "line three", \
        "the third line is not where expected: %r" % rows[one + 2]
    assert rows[one + 3] == "```c", \
        "the opening fence is not verbatim: %r" % rows[one + 3]
    assert rows[one + 4] == "int x;", \
        "the fenced code is not verbatim: %r" % rows[one + 4]
    assert rows[one + 5] == "```", \
        "the closing fence is not verbatim: %r" % rows[one + 5]
    assert rows[one + 6] == "end", \
        "the \\r was not dropped (the line after it reads wrong): %r" % rows[one + 6]
    assert rows[one + 7] == ">", \
        "the input row does not follow the reply: %r" % rows[one + 7]
    assert "\r" not in "\n".join(disp)


# @scenario SC-ai-chat-17
def test_caret_parks_in_the_input_row_after_a_focused_reply(tmp_path):
    """Once a chat reply finishes while the pane is the focused window, the
    caret sits in the input row, right past its "> " prompt -- not wherever
    the last transcript character happened to be painted.
    """
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path, "CARETREPLY")) as s:
        _arm_chat(s)
        s.key("hi")
        s.key("\r", delay=1.0)
        assert _traced_within(tmp_path, "chat done", 10), _trace(tmp_path)
        s._drain(0.5)
        disp = s.display()
        cy, cx = s.screen.cursor.y, s.screen.cursor.x

    ai_y, x0, _ = pane_frame(disp)
    rows = pane_rows(disp)
    idx = _exact_row(rows, ">")
    assert idx >= 0, \
        "the empty input row is not on screen:\n" + "\n".join(disp)
    expected_y = ai_y + 1 + idx
    assert cy == expected_y, \
        "the caret is not on the input row (expected row %d, got %d):\n%s" \
        % (expected_y, cy, "\n".join(disp))
    assert cx == x0 + 1 + 2, \
        "the caret is not just past the \"> \" prompt (expected col %d, got %d):\n%s" \
        % (x0 + 3, cx, "\n".join(disp))


# @scenario SC-ai-chat-17
def test_caret_stays_in_the_file_window_while_a_background_edit_finishes(tmp_path):
    """While an Edit (Alt-G e) streams and lands, the user's file window
    keeps the focus and the caret throughout -- the pane spins in the
    background, but typing after it lands in the file, not the pane.

    wpe_ai_window_locked() refuses keys while the edit owns the file window
    (the review must see the text it was sent, not text typed mid-wait), so
    the probe here is the one the user actually feels: once the task's done
    message is traced (the lock is freed in the same call that frees the
    op), a keystroke must insert into the FILE, proving the caret is there
    and not left parked in the pane the task spun in.
    """
    with WpeSession(str(tmp_path), SEED,
                    env_extra=_env(tmp_path, SEED,
                                   XWPE_AI_MOCK_DELAY_MS=str(int(2500 * WAIT_SCALE)))) as s:
        before = s.display()
        file_y = title_row(before, "t.c")

        s.key(ALT.AI)
        s.key("e", delay=0.6)
        s.key("check it")
        s.key("\r", delay=0.8)
        assert _seen_within(s, "[AI edit] working", 3), \
            "the edit never started:\n" + "\n".join(s.display())
        assert _traced_within(tmp_path, "edit no-change", 10), _trace(tmp_path)
        s._drain(0.3)
        s.key("Z")
        s._drain(0.3)
        disp = s.display()

    assert file_y >= 0, "the file window never opened:\n" + "\n".join(before)
    assert title_row(disp, "t.c") == file_y, \
        "the file window lost its place:\n" + "\n".join(disp)
    assert any("Zint main" in r for r in disp), \
        "the keystroke after the edit landed somewhere other than the file " \
        "(the caret was not back in the user's window):\n" + "\n".join(disp)


# @scenario SC-ai-chat-20
def test_esc_returns_the_caret_to_the_home_window(tmp_path):
    """Esc in the armed input closes the input region and hands the caret
    back to the window the chat was armed from; the next Alt-G e then acts
    on that file, not on "no file here".
    """
    reply = "int main(void){return 1;}\n"
    with WpeSession(str(tmp_path), SEED,
                    env_extra=_env(tmp_path, reply,
                                   XWPE_AI_MOCK_DELAY_MS=str(int(2500 * WAIT_SCALE)))) as s:
        before = s.display()
        file_y = title_row(before, "t.c")

        _arm_chat(s)
        armed = s.display()
        armed_cur = (s.screen.cursor.y, s.screen.cursor.x)

        s.key(ESC, delay=0.8)
        s._drain(0.5)
        disp = s.display()
        cur = (s.screen.cursor.y, s.screen.cursor.x)

        s.key(ALT.AI)
        s.key("e", delay=0.6)
        s.key("do it")
        s.key("\r", delay=0.8)
        edit_seen = _seen_within(s, "[AI edit] working", 3)
        after_edit = s.display()
        s.key(ESC, delay=1.0)                  # cancel the pending edit, tidy exit

    ai_y = title_row(armed, "AI")
    assert ai_y >= 0, "the chat never armed:\n" + "\n".join(armed)
    assert armed_cur[0] > ai_y, \
        "while armed the caret should be inside the pane: %r" % (armed_cur,)
    closed_rows = [r.rstrip() for r in pane_rows(disp)]
    assert not any(r == ">" for r in closed_rows), \
        "the input row is still open after Esc:\n" + "\n".join(disp)
    assert file_y >= 0 and cur == (file_y + 1, 1), \
        "after Esc the caret should be back on the file's one line (row %d, col 1), got %r:\n%s" \
        % (file_y + 1, cur, "\n".join(disp))
    assert edit_seen, "Alt-G e after Esc did not act on the file:\n" + "\n".join(after_edit)
    assert "no file here" not in "\n".join(after_edit), \
        "Alt-G e after Esc refused the file:\n" + "\n".join(after_edit)


# @scenario SC-ai-chat-48
@pytest.mark.skipif(not os.environ.get(LONG_TEST_ENV_VAR),
                     reason="95-second run, gated behind XWPE_TEST_LONG; passes when run")
def test_three_hundred_turns_keep_every_line(tmp_path):
    """300 turns in one session: every turn finishes, the editor stays
    alive, and PgUp still reaches the very first turn with the header and
    the two hint lines above it -- no transcript line is ever dropped.
    """
    turns = 300
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path, "r")) as s:
        _arm_chat(s)
        for i in range(turns):
            s.key("m%d" % i, delay=0.03)
            s.key("\r", delay=0.08)
        assert _traced_within(tmp_path, "chat done", 20), "turns not finishing"
        s._drain(1.5)
        done = _trace(tmp_path).count("chat done")
        end_rows = s.display()
        assert s.alive(), "the editor died"

        seen_first = False
        for _ in range(80):
            s.key(PGUP, delay=0.05)
            if "You: m0" in "\n".join(s.display(drain=0)):
                seen_first = True
                break
        s.key(PGUP, delay=0.2)                 # one more page: the header above m0
        top_rows = s.display()
        s.key("tail")
        s._drain(0.4)
        snapped = s.display()

    assert done == turns, "only %d of %d turns finished" % (done, turns)
    assert _row_index(end_rows, "You: m%d" % (turns - 1)) >= 0, \
        "the last turn is not on screen:\n" + "\n".join(end_rows)
    assert seen_first, "You: m0 never came back into view after PgUp"
    hdr = _row_index(top_rows, "[AI: mock")
    assert hdr >= 0, "the header line was dropped:\n" + "\n".join(top_rows)
    assert _row_index(top_rows, "You: m0") == hdr + 3, \
        "the first turn no longer follows the header and hints:\n" + "\n".join(top_rows)
    assert any("> tail" in r for r in snapped), "typing did not snap back to the input"
