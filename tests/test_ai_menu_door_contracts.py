"""The doors of the assistant: the prompt popup's label and Alt-P, Clear during a
turn, the bar click and the first-use notice, the composer's title.

Each test asserts what the user sees or keeps: the screen, the XWPE_AI_TRACE
log, the files under the state directory and the working directory.  The
mock backend answers every turn, so nothing here reaches a network.
"""
import os
import pytest
from wpe_driver import WpeSession, ALT, wait_screen, assistant_build

pytestmark = pytest.mark.skipif(not assistant_build(),
                                reason="wpe built without --enable-ai")

ENV = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_MOCK_REPLY": "ok"}
C_SRC = "int main(void){return 0;}\n"
TAB = "\t"
ALT_P = "\033p"
ALT_M = "\033m"
AGENT_POPUP = "AI agent task"
EDIT_POPUP = "AI edit instruction"
DROPDOWN_MARK = "Ask (chat)"
# The popup's rows below its title: a blank row, the label, the field.
LABEL_ROW_BELOW_TITLE = 2
FIELD_ROW_BELOW_TITLE = 3
# The popup's frame: xa=6 and xe=73 are 0-based screen columns of the border.
POPUP_LEFT_BORDER = 6
POPUP_RIGHT_BORDER = 73
REPLY_MARK = "REPLYMARK77"
SLOW_REPLY_MS = "4000"
SECOND_REPLY_WAIT = 6.0                     # seconds: the slow reply plus the paint
COMPOSER_HINT = "Esc = done"
DEEP_WORKDIR_LEN = 70


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
    """SGR 1006 mouse report, 1-based column and row."""
    if press:
        final = "M"
    else:
        final = "m"
    return "\x1b[<%d;%d;%d%s" % (button, col, row, final)


def _find(disp, needle):
    """(1-based column, 1-based row) of ``needle`` on the screen, or None."""
    for r, line in enumerate(disp):
        i = line.find(needle)
        if i >= 0:
            return i + 1, r + 1
    return None


def _popup_rows(disp, title):
    """The label row and the field row of the popup titled ``title``."""
    top = next(i for i, r in enumerate(disp) if title in r)
    return disp[top + LABEL_ROW_BELOW_TITLE], disp[top + FIELD_ROW_BELOW_TITLE]


def _inside_popup(row):
    """The text between the popup's two border cells on one row."""
    return row[POPUP_LEFT_BORDER + 1:POPUP_RIGHT_BORDER].strip()


def _open_popup(s, letter):
    s.key(ALT.AI, delay=0.5)
    s.key(letter, delay=0.9)


def _assert_label_row_names_the_keys(label, title):
    inside = _inside_popup(label)
    assert inside, "the %s popup paints no label row above the field: %r" % (title, label)
    for word in ("Prompt", "Enter", "Alt-S", "Alt-M", "Alt-C", "Esc"):
        assert word in inside, "the %s label does not name %s: %r" % (title, word, inside)


# @scenario SC-ai-menu-37
def test_the_label_row_paints_inside_the_box_in_the_agent_and_edit_popups(tmp_path):
    """The Agent popup and the Edit popup both show the key hint above the
    field, inside the frame, naming Enter/Alt-S, Alt-M and Alt-C/Esc."""
    with WpeSession(str(tmp_path), C_SRC, filename="a.c", env_extra=_env(tmp_path)) as s:
        _open_popup(s, "g")
        agent = s.display()
        s.key("\033", delay=0.5)
        _open_popup(s, "e")
        edit = s.display()
        s.key("\033", delay=0.5)
    assert AGENT_POPUP in _screen(agent), "the Agent popup did not open:\n" + _screen(agent)
    assert EDIT_POPUP in _screen(edit), "the Edit popup did not open:\n" + _screen(edit)
    agent_label, _ = _popup_rows(agent, AGENT_POPUP)
    edit_label, _ = _popup_rows(edit, EDIT_POPUP)
    _assert_label_row_names_the_keys(agent_label, "Agent")
    _assert_label_row_names_the_keys(edit_label, "Edit")


# @scenario SC-ai-menu-37
def test_alt_p_moves_the_focus_back_to_the_field(tmp_path):
    """With the focus on a button, Alt-P returns it to the field: PPP typed
    next lands in the field."""
    with WpeSession(str(tmp_path), C_SRC, filename="a.c", env_extra=_env(tmp_path)) as s:
        _open_popup(s, "g")
        assert AGENT_POPUP in _screen(s.display()), "the Agent popup did not open"
        s.key(TAB, delay=0.3)                   # focus off the field, onto a button
        s.key(ALT_P, delay=0.4)
        s.key("PPP", delay=0.3)
        disp = s.display()
        s.key("\033", delay=0.5)
    _, field = _popup_rows(disp, AGENT_POPUP)
    assert "PPP" in field, "Alt-P did not focus the field (field row %r):\n%s" % (field, _screen(disp))


def _session_files(state):
    return sorted(p for p in state.rglob("*.session") if p.is_file())


def _orphan_assistant_files(state):
    """Session files holding an assistant message and no user message."""
    orphans = []
    for p in _session_files(state):
        text = p.read_text()
        if '"role":"assistant"' in text and '"role":"user"' not in text:
            orphans.append(str(p.relative_to(state)))
    return orphans


def _ask(s, question):
    s.key(ALT.AI, delay=0.5)
    s.key("a", delay=0.6)                       # Ask arms the input row
    s.key(question, delay=0.1)
    s.key("\r", delay=0.8)                      # the turn starts


# @scenario SC-ai-menu-45
def test_clear_is_refused_while_a_turn_runs_and_works_after_it(tmp_path):
    """Alt-G n during a streaming reply changes nothing on disk or in memory
    and the bar says a turn is running and Esc cancels it first; after the
    reply, Alt-G n clears and no session file holds a reply with no question."""
    state = tmp_path / "state"
    env = _env(tmp_path, XWPE_AI_MOCK_REPLY=REPLY_MARK, XWPE_AI_MOCK_DELAY_MS=SLOW_REPLY_MS,
               XDG_STATE_HOME=str(state))
    with WpeSession(str(tmp_path), "hello\n", filename="notes.txt", env_extra=env) as s:
        _ask(s, "question one")
        assert wait_screen(s, REPLY_MARK, tmo=12), "the first reply never arrived"
        s._drain(0.8)
        files_before = _session_files(state)
        _ask(s, "question two")                 # the second reply is seconds away
        s.key(ALT.AI, delay=0.5)
        s.key("n", delay=0.6)
        during = s.display()
        files_during = _session_files(state)
        trace_during = _trace(tmp_path)
        s._drain(SECOND_REPLY_WAIT)             # let the second reply land
        s.key(ALT.AI, delay=0.5)
        s.key("n", delay=0.8)
        after = s.display()
        s._drain(0.8)
        files_after = _session_files(state)
        orphans = _orphan_assistant_files(state)
        trace_after = _trace(tmp_path)
    bar_during = during[-1]
    assert files_before, "no session file after the first turn (nothing to protect)"
    assert "cleared" not in bar_during, "Alt-G n cleared during a stream: %r" % bar_during
    assert "running" in bar_during and "Esc" in bar_during, \
        "the bar did not say a turn is running and Esc cancels it: %r" % bar_during
    assert files_during == files_before, \
        "Alt-G n during a stream touched the session file: %r -> %r" % (files_before, files_during)
    assert "session reset" not in trace_during, "the session was reset during a stream (trace):\n" + trace_during
    assert "cleared" in after[-1], "Alt-G n after the turn did not clear: %r" % after[-1]
    assert "session reset" in trace_after, "no session reset after the turn (trace):\n" + trace_after
    assert not orphans, "a session file holds a reply with no question: %r" % orphans
    assert not files_after, "Clear left a session file behind: %r" % files_after


# @scenario SC-ai-menu-23
def test_the_bar_click_shows_the_first_use_notice_and_y_then_opens_the_dropdown(tmp_path):
    """Without consent, a click on "Alt-G AI" shows the first-use notice and
    no dropdown; y then opens the dropdown."""
    env = _env(tmp_path, XWPE_AI_CONSENTED="0", XWPE_AI_CONSENT_FILE=str(tmp_path / "marker"))
    with WpeSession(str(tmp_path), "hello\n", filename="notes.txt", env_extra=env) as s:
        disp = s.display()
        pos = _find(disp, "Alt-G AI")
        assert pos, "no bar entry:\n" + _screen(disp)
        col, row = pos
        s.key(_sgr(0, col, row, True), _sgr(0, col, row, False), delay=0.8)
        notice = s.display()
        s.key("y", delay=0.8)
        after_y = s.display()
        s.key("\033", delay=0.4)
    assert "first use" in _screen(notice), "the click skipped the first-use notice:\n" + _screen(notice)
    assert DROPDOWN_MARK not in _screen(notice), "the dropdown opened before consent:\n" + _screen(notice)
    assert DROPDOWN_MARK in _screen(after_y), "y did not open the dropdown after the notice:\n" + _screen(after_y)
    assert (tmp_path / "marker").exists(), "y did not write the consent marker"


def _deep_workdir(tmp_path):
    deep = tmp_path
    while len(str(deep)) < DEEP_WORKDIR_LEN:
        deep = deep / "deeper_directory_level"
    deep.mkdir(parents=True, exist_ok=True)
    return deep


def _visible_entries(path):
    return sorted(n for n in os.listdir(str(path)) if not n.startswith("."))


# @scenario SC-ai-menu-41
def test_the_composer_frame_shows_its_title_and_hint_from_a_deep_workdir(tmp_path):
    """From a working directory 70 characters deep, Alt-M opens the composer
    whose frame shows the title and "Esc = done", not the directory path; Esc
    then N leaves the working directory without a new file."""
    deep = _deep_workdir(tmp_path)
    with WpeSession(str(deep), C_SRC, filename="a.c", env_extra=_env(tmp_path)) as s:
        before = _visible_entries(deep)
        _open_popup(s, "g")
        s.key("hello", delay=0.3)
        s.key(ALT_M, delay=1.2)
        composer = s.display()
        s.key("\033", delay=0.6)                # Esc = done
        s.key("n", delay=0.6)                   # N cancels the send
        s._drain(0.5)
        after = _visible_entries(deep)
    shown = _screen(composer)
    title_rows = [r for r in composer if COMPOSER_HINT in r or "cancels" in r or "AI agent" in r]
    assert COMPOSER_HINT in shown, "the composer's frame does not show the hint:\n" + shown
    assert title_rows, "no composer title row:\n" + shown
    for r in title_rows:
        assert "/" not in r, "the composer's window name carries a path: %r" % r
    assert str(deep) not in shown, "the composer's frame shows the working directory:\n" + shown
    assert after == before, "the composer left files in the working directory: %r" % \
        sorted(set(after) - set(before))
