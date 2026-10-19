"""The quick rows of the Alt-G dropdown (Permissions, Clear, Settings) and the bar entry.

Clear removes the workspace session file from disk, Permissions and Settings
keep working while a turn streams, and the "Alt-G AI" entry sits in a fixed
bottom-bar slot.  The mock backend answers every turn; XDG_STATE_HOME points
into the test's directory so the session file can be seen.
"""
import pytest
from wpe_driver import WpeSession, ALT, WAIT_SCALE, assistant_build, wait_screen

pytestmark = pytest.mark.skipif(not assistant_build(),
                                reason="wpe built without --enable-ai")

ENV = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_MOCK_REPLY": "ok"}
ALT_C = "\033c"
PLAIN_BAR_SLOT_COLUMN = 57          # "Alt-G AI" in the slot of "^L S.Again"
CUA_BAR_SLOT_COLUMN = 58            # ... and in the slot of "F3 S.Ag."
BAR_ENTRY = "Alt-G AI"


def _env(tmp_path, **extra):
    env = dict(ENV, XWPE_AI_TRACE=str(tmp_path / "t.trace"),
               XDG_STATE_HOME=str(tmp_path / "state"))
    env.update(extra)
    return env


def _trace(tmp_path):
    path = tmp_path / "t.trace"
    if path.exists():
        return path.read_text()
    return ""


def _session_files(tmp_path):
    state = tmp_path / "state"
    return sorted(str(p.relative_to(state)) for p in state.rglob("*.session"))


# @scenario SC-ai-menu-44
def test_clear_removes_the_session_file_flashes_and_resets(tmp_path):
    """After one answered turn the workspace session file exists; Alt-G n
    removes it, flashes the confirmation and records the reset."""
    env = _env(tmp_path, XWPE_AI_MOCK_REPLY="FIRSTREPLY")
    with WpeSession(str(tmp_path), "hello\n", filename="t.txt", env_extra=env) as s:
        s.key(ALT.AI, delay=0.5)
        s.key("a", delay=0.8)
        s.key("hi", delay=0.1)
        s.key("\r", delay=1.2)
        answered = wait_screen(s, "FIRSTREPLY", tmo=8 * WAIT_SCALE)
        written = _session_files(tmp_path)
        s.key("\033", delay=0.5)
        s.key(ALT.AI, delay=0.5)
        s.key("n", delay=0.8)
        bar = s.display()[-1]
        left = _session_files(tmp_path)
        trace = _trace(tmp_path)
    assert answered, "the first turn was not answered"
    assert written, "no session file was written after the first turn"
    assert not left, "the session file survived Alt-G n: %r" % left
    assert "cleared" in bar, "no confirmation flash on the bar: %r" % bar
    assert "session reset" in trace, "the in-memory session was not reset (trace):\n" + trace


# @scenario SC-ai-menu-46
def test_permissions_and_settings_work_while_a_turn_streams(tmp_path):
    """Alt-G y changes the level and Alt-G s opens Options > AI over the
    running turn; the reply still arrives afterwards."""
    env = _env(tmp_path, XWPE_AI_MOCK_REPLY="LATE46", XWPE_AI_MOCK_DELAY_MS="5000")
    with WpeSession(str(tmp_path), "hello\n", filename="t.txt", env_extra=env) as s:
        s.key(ALT.AI, delay=0.5)
        s.key("a", delay=0.8)
        s.key("q", delay=0.1)
        s.key("\r", delay=0.6)                      # the turn starts, the reply is 5 s away
        s.key(ALT.AI, delay=0.5)
        s.key("y", delay=0.7)
        flash = s.display()[-1]
        level_set = "policy set edits" in _trace(tmp_path)
        s.key(ALT.AI, delay=0.5)
        s.key("s", delay=1.0)
        settings = "\n".join(s.display())
        s.key("\033", delay=0.8)
        arrived = wait_screen(s, "LATE46", tmo=10 * WAIT_SCALE)
    assert "Permissions:" in flash, "no permission flash during the turn: %r" % flash
    assert level_set, "the level did not change during the turn (trace):\n" + _trace(tmp_path)
    assert "AI settings" in settings and "Backend" in settings, \
        "Alt-G s did not open Options > AI during the turn:\n" + settings
    assert arrived, "the turn did not continue after the dialog closed"


def _bar_entry_column(s):
    return s.display()[-1].find(BAR_ENTRY)


# @scenario SC-ai-menu-47
def test_bar_entry_sits_in_the_slot_of_the_plain_and_the_cua_bar(tmp_path):
    """"Alt-G AI" is at screen column 57 on the plain bar and, after Options >
    Editor > CUA-Style, at column 58 on the CUA bar."""
    with WpeSession(str(tmp_path), "hello\n", filename="t.txt", env_extra=_env(tmp_path)) as s:
        plain = _bar_entry_column(s)
        s.key(ALT.OPTIONS, delay=0.7)
        s.key("e", delay=0.9)                       # Options > Editor
        s.key(ALT_C, delay=0.5)                     # CUA-Style
        s.key("\r", delay=1.0)
        cua_bar = s.display()[-1]
        cua = cua_bar.find(BAR_ENTRY)
        s._drain(0.5)
    assert plain == PLAIN_BAR_SLOT_COLUMN, "plain bar column %d, expected %d" % (plain, PLAIN_BAR_SLOT_COLUMN)
    assert "Alt-F2" in cua_bar, "the CUA bar did not take effect: %r" % cua_bar
    assert cua == CUA_BAR_SLOT_COLUMN, "CUA bar column %d, expected %d" % (cua, CUA_BAR_SLOT_COLUMN)
