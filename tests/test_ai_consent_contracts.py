"""First-use consent: where the marker lives, what it holds, when the notice returns.

These tests enable the assistant the way a user does, in Options > AI, and not
with XWPE_AI_ENABLE, because that variable alone skips the notice and would
hide what is being pinned.  XDG_CONFIG_HOME points into the test's own
directory so the marker lands where the test can read it.
"""
import os
import stat
import pytest
from wpe_driver import WpeSession, ALT, assistant_build

pytestmark = pytest.mark.skipif(not assistant_build(),
                                reason="wpe built without --enable-ai")

DROPDOWN_MARK = "Ask (chat)"
MARKER_CONTENT = "1\n"
PRIVATE_DIR_MODE = 0o700


def _env(tmp_path, **extra):
    env = {"XWPE_AI_BACKEND": "mock", "XWPE_AI_MOCK_REPLY": "ok",
           "XDG_CONFIG_HOME": str(tmp_path / "cfg")}
    env.update(extra)
    return env


def _marker(tmp_path):
    return tmp_path / "cfg" / "xwpe" / "ai_consented"


def _enable_in_options(s):
    """Options > AI, toggle "Enable AI assistant", Ok: the user's own path."""
    s.key(ALT.OPTIONS, delay=0.8)
    s.key("i", delay=1.0)
    s.key(" ", delay=0.7)
    s.key("\033o", delay=1.0)
    s._drain(0.8)


def _screen(s):
    return "\n".join(s.display())


# @scenario SC-ai-menu-28
def test_accepting_writes_the_marker_and_the_notice_does_not_return(tmp_path):
    """After y the marker holds "1" and a newline in a 0700 directory under
    XDG_CONFIG_HOME, and the next Alt-G opens the dropdown with no notice."""
    with WpeSession(str(tmp_path), "hello\n", filename="t.txt", env_extra=_env(tmp_path)) as s:
        _enable_in_options(s)
        s.key(ALT.AI, delay=0.8)
        notice = _screen(s)
        s.key("y", delay=0.8)
        accepted = _screen(s)
        s.key("\033", delay=0.5)
        s.key(ALT.AI, delay=0.8)
        again = _screen(s)
        s.key("\033", delay=0.4)
    marker = _marker(tmp_path)
    assert "first use" in notice and DROPDOWN_MARK not in notice, \
        "the first Alt-G did not show the notice alone:\n" + notice
    assert DROPDOWN_MARK in accepted, "y did not open the dropdown:\n" + accepted
    assert marker.exists(), "no marker at %s" % marker
    assert marker.read_text() == MARKER_CONTENT, "marker content %r" % marker.read_text()
    mode = stat.S_IMODE(os.stat(str(marker.parent)).st_mode)
    assert mode == PRIVATE_DIR_MODE, "marker directory mode %o, expected %o" % (mode, PRIVATE_DIR_MODE)
    assert DROPDOWN_MARK in again, "the second Alt-G did not open the dropdown at once:\n" + again


# @scenario SC-ai-menu-29
def test_declining_says_so_writes_nothing_and_the_notice_comes_back(tmp_path):
    """After n the pane reports "not enabled" and no marker exists; the next
    Alt-G waits on the notice again (no dropdown) until y answers it."""
    with WpeSession(str(tmp_path), "hello\n", filename="t.txt", env_extra=_env(tmp_path)) as s:
        _enable_in_options(s)
        s.key(ALT.AI, delay=0.8)
        s.key("n", delay=0.7)
        declined = _screen(s)
        marker_after_n = _marker(tmp_path).exists()
        s.key(ALT.AI, delay=0.8)
        asked_again = _screen(s)
        s.key("y", delay=0.8)
        answered = _screen(s)
        s.key("\033", delay=0.4)
    assert "[AI] not enabled - press Alt-G again when you want it" in declined, \
        "declining did not report it:\n" + declined
    assert not marker_after_n, "declining wrote a marker"
    assert DROPDOWN_MARK not in asked_again, \
        "the second Alt-G opened the dropdown without asking again:\n" + asked_again
    assert DROPDOWN_MARK in answered, "y did not answer the second notice:\n" + answered
    assert _marker(tmp_path).exists(), "accepting the second notice wrote no marker"


# @scenario SC-ai-menu-31
def test_consented_environment_skips_the_notice_and_writes_no_marker(tmp_path):
    """With XWPE_AI_CONSENTED=1 and no marker, Alt-G opens the dropdown at once
    and no marker is written."""
    env = _env(tmp_path, XWPE_AI_CONSENTED="1")
    with WpeSession(str(tmp_path), "hello\n", filename="t.txt", env_extra=env) as s:
        _enable_in_options(s)
        s.key(ALT.AI, delay=0.8)
        shown = _screen(s)
        s.key("\033", delay=0.4)
    assert "first use" not in shown, "the notice was shown despite XWPE_AI_CONSENTED=1:\n" + shown
    assert DROPDOWN_MARK in shown, "the dropdown did not open:\n" + shown
    assert not _marker(tmp_path).exists(), "a marker was written under XWPE_AI_CONSENTED=1"
