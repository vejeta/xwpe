"""The prompt popup and the multi-line composer behind Edit, Multi-file and Agent.

The popup is opened from the Alt-G dropdown.  Each test reads the screen, the
XWPE_AI_TRACE log or the disk, with the mock backend answering every turn.
The Agent popup ("AI agent task") is used where the action does not matter;
the Edit popup where a turn must be streaming underneath it.
"""
import pytest
from wpe_driver import (WpeSession, ALT, WAIT_SCALE, assistant_build, title_pos,
                        title_row, wait_screen)

pytestmark = pytest.mark.skipif(not assistant_build(),
                                reason="wpe built without --enable-ai")

ENV = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_MOCK_REPLY": "ok"}
C_SRC = "int main(void){return 0;}\n"
AGENT_TITLE = "AI agent task"
EDIT_TITLE = "AI edit instruction"
TASK_PREFIX = "agent task="
ALT_C = "\033c"
ALT_M = "\033m"
ALT_S = "\033s"
FIELD_ROW_BELOW_TITLE = 3
TASK_LIMIT_BYTES = 1999
SCREEN_MESSAGE_TITLE = "Message"
AI_PANE_TITLE = "AI"


def _env(tmp_path, **extra):
    env = dict(ENV, XWPE_AI_TRACE=str(tmp_path / "t.trace"))
    env.update(extra)
    return env


def _trace(tmp_path):
    path = tmp_path / "t.trace"
    if path.exists():
        return path.read_text()
    return ""


def _screen(disp):
    return "\n".join(disp)


def _sgr(button, col, row, press=True):
    if press:
        final = "M"
    else:
        final = "m"
    return "\x1b[<%d;%d;%d%s" % (button, col, row, final)


def _open_popup(s, letter, title):
    s.key(ALT.AI, delay=0.5)
    s.key(letter, delay=0.8)
    assert title in _screen(s.display()), \
        "the %r popup did not open:\n%s" % (title, _screen(s.display()))


def _popup_field(disp, title):
    """The screen row of the popup's input field (the row below its label)."""
    for i, row in enumerate(disp):
        if title in row:
            return disp[i + FIELD_ROW_BELOW_TITLE]
    return ""


def _task_line(trace):
    """The trace's task text: the "agent task=" line and any continuation lines."""
    at = trace.find(TASK_PREFIX)
    if at < 0:
        return ""
    return trace[at + len(TASK_PREFIX):]


# @scenario SC-ai-menu-33
def test_field_scrolls_to_show_the_tail_of_a_long_text(tmp_path):
    """Seventy typed characters do not fit the field: it scrolls and shows the
    tail, the head is out of view."""
    seventy = "".join(chr(ord("a") + (i % 26)) for i in range(70))
    with WpeSession(str(tmp_path), C_SRC, filename="a.c", env_extra=_env(tmp_path)) as s:
        _open_popup(s, "g", AGENT_TITLE)
        s.key(seventy, delay=0.3)
        field = _popup_field(s.display(), AGENT_TITLE)
        s.key(ALT_C, delay=0.6)
    assert seventy[-10:] in field, "the field did not show the tail: %r" % field
    assert seventy[:10] not in field, "the field did not scroll (head still visible): %r" % field


# @scenario SC-ai-menu-33
def test_send_carries_1999_bytes_and_drops_the_rest(tmp_path):
    """2003 typed bytes: the field holds 1999 and Send hands exactly 1999 to
    the agent; the last bytes typed are not kept."""
    chunk = "0123456789" * 10                                   # 100 bytes
    with WpeSession(str(tmp_path), C_SRC, filename="a.c", env_extra=_env(tmp_path)) as s:
        _open_popup(s, "g", AGENT_TITLE)
        for _ in range(20):                                     # 2000 bytes
            s.key(chunk, delay=0.12)
        s.key("END", delay=0.3)                                 # three more, past the limit
        s.key("\r", delay=1.5)
        s._drain(1.0)
        task = _task_line(_trace(tmp_path)).split("\n")[0]
        s.key("\033", delay=0.4)
    assert task, "Send did not reach the agent (no task line in the trace)"
    assert len(task) == TASK_LIMIT_BYTES, "task is %d bytes, expected %d" % (len(task), TASK_LIMIT_BYTES)
    assert "END" not in task, "bytes past the limit were kept: %r" % task[-20:]


# @scenario SC-ai-menu-35
def test_enter_sends_the_typed_task(tmp_path):
    """Enter is Send: the typed text reaches the agent and the popup closes."""
    with WpeSession(str(tmp_path), C_SRC, filename="a.c", env_extra=_env(tmp_path)) as s:
        _open_popup(s, "g", AGENT_TITLE)
        s.key("hello task", delay=0.3)
        s.key("\r", delay=1.5)
        disp = s.display()
        trace = _trace(tmp_path)
    assert "agent task=hello task" in trace, "Enter did not send the task (trace):\n" + trace
    assert AGENT_TITLE not in _screen(disp), "the popup stayed open after Enter"


# @scenario SC-ai-menu-34
def test_alt_s_is_the_send_button(tmp_path):
    """Alt-S answers the Send button: the typed text reaches the agent and the
    popup closes."""
    with WpeSession(str(tmp_path), C_SRC, filename="a.c", env_extra=_env(tmp_path)) as s:
        _open_popup(s, "g", AGENT_TITLE)
        s.key("sent by alt s", delay=0.3)
        s.key(ALT_S, delay=1.5)
        disp = s.display()
        trace = _trace(tmp_path)
    assert "agent task=sent by alt s" in trace, "Alt-S did not send the task (trace):\n" + trace
    assert AGENT_TITLE not in _screen(disp), "the popup stayed open after Alt-S"


# @scenario SC-ai-menu-35
def test_esc_closes_the_popup_and_sends_nothing(tmp_path):
    """Esc closes the popup with text typed in it: no task line in the trace
    and the file is as it was."""
    with WpeSession(str(tmp_path), C_SRC, filename="a.c", env_extra=_env(tmp_path)) as s:
        _open_popup(s, "g", AGENT_TITLE)
        s.key("typed then escaped", delay=0.3)
        s.key("\033", delay=0.8)
        disp = s.display()
        s.save()
        trace = _trace(tmp_path)
        text = s.text()
    assert AGENT_TITLE not in _screen(disp), "Esc left the popup open"
    assert "task=" not in trace, "Esc sent the task (trace):\n" + trace
    assert text == C_SRC, "the buffer changed: %r" % text


# @scenario SC-ai-menu-36
def test_an_empty_send_closes_quietly_and_opens_no_pane(tmp_path):
    """Enter on an empty field closes the popup: nothing in the trace and the
    AI pane is not opened."""
    with WpeSession(str(tmp_path), C_SRC, filename="a.c", env_extra=_env(tmp_path)) as s:
        _open_popup(s, "g", AGENT_TITLE)
        s.key("\r", delay=1.0)
        disp = s.display()
        trace = _trace(tmp_path)
    assert AGENT_TITLE not in _screen(disp), "the empty Send left the popup open"
    assert "task=" not in trace, "an empty Send sent something (trace):\n" + trace
    assert title_row(disp, AI_PANE_TITLE) == -1, \
        "an empty Send opened the AI pane:\n" + _screen(disp)


# @scenario SC-ai-menu-38
def test_popup_is_centred_over_the_docked_pane_and_laid_out_again_on_resize(tmp_path):
    """Over the docked pane the popup frame spans columns 6..73; after a
    resize to 60x18 it is laid out again inside the screen and Esc closes it."""
    with WpeSession(str(tmp_path), C_SRC, filename="a.c", env_extra=_env(tmp_path)) as s:
        s.key(ALT.AI, delay=0.5)
        s.key("a", delay=0.8)                                   # Ask docks the pane
        s.key("\033", delay=0.5)                                # focus back to the file
        _open_popup(s, "e", EDIT_TITLE)
        wide = s.display()
        wide_at = title_pos(s, EDIT_TITLE)
        s.resize(60, 18, delay=1.2)
        narrow = s.display()
        narrow_at = title_pos(s, EDIT_TITLE)
        s.key("\033", delay=0.5)
        closed = _screen(s.display())
    assert wide_at, "the popup did not open over the pane"
    row = wide[wide_at[0]]
    left, right = row.index("lq"), row.rindex("qk") + 1
    assert (left, right) == (6, 73), "frame columns at 80 are %d..%d, expected 6..73" % (left, right)
    assert narrow_at, "the popup is gone after the resize:\n" + _screen(narrow)
    row = narrow[narrow_at[0]]
    left, right = row.index("lq"), row.rindex("qk") + 1
    assert 0 <= left and right <= 59, "popup off screen at 60 columns: %d..%d" % (left, right)
    assert abs((left + right) / 2.0 - 29.5) <= 1.5, "popup not re-centred at 60 columns: %d..%d" % (left, right)
    assert "Send" in _screen(narrow) and "Cancel" in _screen(narrow), \
        "the buttons are not on screen after the resize:\n" + _screen(narrow)
    assert EDIT_TITLE not in closed, "Esc did not close the popup after the resize"


# @scenario SC-ai-menu-39
def test_composer_with_no_text_closes_silently_on_esc(tmp_path):
    """Alt-M then Esc on an empty composer cancels the action: no question,
    nothing sent, no popup left."""
    with WpeSession(str(tmp_path), C_SRC, filename="a.c", env_extra=_env(tmp_path)) as s:
        _open_popup(s, "g", AGENT_TITLE)
        s.key(ALT_M, delay=1.2)
        s.key("\033", delay=0.8)
        disp = s.display()
        trace = _trace(tmp_path)
    shown = _screen(disp)
    assert "Send this prompt" not in shown, "an empty composer asked the question:\n" + shown
    assert AGENT_TITLE not in shown and "Esc = done" not in shown, \
        "the empty composer did not close silently:\n" + shown
    assert "task=" not in trace, "an empty composer sent a task (trace):\n" + trace


# @scenario SC-ai-menu-39
def test_composer_text_then_esc_asks_and_n_discards_it(tmp_path):
    """Text in the composer then Esc asks "Send this prompt to the AI?"; N
    sends nothing and the text is gone when the popup is opened again."""
    with WpeSession(str(tmp_path), C_SRC, filename="a.c", env_extra=_env(tmp_path)) as s:
        _open_popup(s, "g", AGENT_TITLE)
        s.key("seed text", delay=0.3)
        s.key(ALT_M, delay=1.2)
        s.key("\r", delay=0.3)
        s.key("second line", delay=0.3)
        s.key("\033", delay=0.8)
        question = _screen(s.display())
        s.key("n", delay=0.8)
        after_n = s.display()
        _open_popup(s, "g", AGENT_TITLE)
        reopened = _popup_field(s.display(), AGENT_TITLE)
        s.key(ALT_C, delay=0.6)
        trace = _trace(tmp_path)
    assert "Send this prompt to the AI?" in question, "no question after Esc with text:\n" + question
    assert "task=" not in trace, "N sent the task (trace):\n" + trace
    assert "Send this prompt" not in _screen(after_n), "the question stayed after N"
    assert "seed text" not in reopened, "N kept the text; the field reads %r" % reopened


# @scenario SC-ai-menu-39
def test_composer_y_sends_the_text_without_trailing_blank_lines_or_a_save_question(tmp_path):
    """Y hands the composed text to the action with the trailing blank lines
    dropped; the scratch window closes with no save question."""
    with WpeSession(str(tmp_path), C_SRC, filename="a.c", env_extra=_env(tmp_path)) as s:
        _open_popup(s, "g", AGENT_TITLE)
        s.key("ytext", delay=0.3)
        s.key(ALT_M, delay=1.2)
        s.key("\r", delay=0.2)
        s.key("more", delay=0.2)
        s.key("\r\r", delay=0.2)                                # two trailing blank lines
        s.key("\033", delay=0.8)
        s.key("y", delay=1.5)
        disp = s.display()
        trace = _trace(tmp_path)
        s.key("\033", delay=0.4)
    task = _task_line(trace)
    assert task.startswith("ytext\nmore\n"), "Y did not hand the text to the action (trace):\n" + trace
    assert not task.startswith("ytext\nmore\n\n"), "trailing blank lines were kept: %r" % task[:30]
    assert title_row(disp, SCREEN_MESSAGE_TITLE) == -1, \
        "a question box is on screen after Y:\n" + _screen(disp)


# @scenario SC-ai-menu-40
def test_popup_over_a_streaming_turn_keeps_the_caret_and_every_keystroke(tmp_path):
    """While an Ask turn streams, three letters typed a second apart in the
    popup all land on the field row, and the caret never leaves that row."""
    env = _env(tmp_path, XWPE_AI_MOCK_REPLY="LATE42", XWPE_AI_MOCK_DELAY_MS="6000")
    with WpeSession(str(tmp_path), C_SRC, filename="a.c", env_extra=env) as s:
        s.key(ALT.AI, delay=0.5)
        s.key("a", delay=0.8)
        s.key("q", delay=0.1)
        s.key("\r", delay=0.6)                                  # the turn starts, the reply is 6 s away
        s.key(_sgr(0, 10, 3, True), _sgr(0, 10, 3, False), delay=0.6)   # focus the file window
        _open_popup(s, "e", EDIT_TITLE)
        carets = []
        for ch in "abc":
            s.key(ch, delay=1.0)                                # across spinner ticks
            carets.append((s.screen.cursor.y, s.screen.cursor.x))
        field = _popup_field(s.display(), EDIT_TITLE)
        s.key(ALT_C, delay=0.6)
        arrived = wait_screen(s, "LATE42", tmo=10 * WAIT_SCALE)
    assert "abc" in field, "typed text was lost under the spinner: %r" % field
    assert len(set(y for y, _ in carets)) == 1, "the caret left the field row: %r" % carets
    assert arrived, "the streaming turn did not finish after the popup closed"
