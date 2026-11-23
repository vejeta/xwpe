"""A window the editor writes itself has no file behind it.

The assistant's multi-line composer (Alt-G g, then Alt-M in the prompt) and its
pane are windows named like files, but the name is a label: nothing is read
from the disk for them and nothing is ever written under it.  F2 says in one
sentence that the window is not a file and points to Save As; a file that has
the label's name keeps its text; the quit prompt does not ask about the window
and the emergency copy (.ESV) is not made.

Run: WPE_BIN=$PWD/wpe tests/.venv/bin/python -m pytest -q tests/test_window_no_file.py
"""
import os
import signal
import subprocess
import time

import pytest

from wpe_driver import WpeSession, ALT, WPE_BIN

SEED = "int t;\n"
TITLE = "AI agent task  [Esc = done, then Y sends, N cancels]"
F2 = "\033OQ"
NOT_A_FILE = "This window is not a file."
EXIT_DEADLINE_S = 15


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _env(tmp_path):
    return {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_MOCK_REPLY": "ok",
            "XWPE_AI_TRACE": str(tmp_path / "ai.trace")}


def _session(tmp_path):
    return WpeSession(str(tmp_path), SEED, filename="t.c", cols=140, rows=30,
                      env_extra=_env(tmp_path))


def _open_composer(w, text="hello"):
    """Alt-G g, the text, Alt-M: the multi-line composer window, changed."""
    w.key(ALT.AI)
    w.key("g", delay=1.0)
    w.key(text, delay=0.5)
    w.key("\033m", delay=1.0)


def _files_named_after_the_title(tmp_path):
    return sorted(n for n in os.listdir(str(tmp_path)) if "AI agent task" in n)


def _screen(w):
    return "\n".join(w.display())


# @scenario SC-window-no-file-01
def test_f2_in_the_composer_says_it_is_not_a_file_and_writes_nothing(tmp_path):
    """F2 in the composer: one sentence says the window is not a file and points
    to Save As, and no file named after the title appears.  On the unfixed
    tree "hello" was written to a file called after the title."""
    with _session(tmp_path) as w:
        _open_composer(w)
        w.key(F2, delay=1.0)
        assert NOT_A_FILE in _screen(w), _screen(w)
        assert "Save As" in _screen(w), _screen(w)
    assert _files_named_after_the_title(tmp_path) == []


# @scenario SC-window-no-file-02
def test_f2_in_the_composer_leaves_a_file_of_the_titles_name_alone(tmp_path):
    """A file that happens to have the title's name keeps its text and gets no
    backup: F2 in the composer used to replace it."""
    mine = tmp_path / TITLE
    mine.write_text("my own text\n")
    with _session(tmp_path) as w:
        _open_composer(w)
        w.key(F2, delay=1.0)
        assert NOT_A_FILE in _screen(w), _screen(w)
    assert mine.read_text() == "my own text\n"
    assert _files_named_after_the_title(tmp_path) == [TITLE]


# @scenario SC-window-no-file-03
def test_quitting_with_a_changed_composer_does_not_ask_to_save_it(tmp_path):
    """Alt-X with a changed composer open: no "NOT saved" question for it and no
    file is written; on the unfixed tree the question was asked and Y wrote
    the text under the title."""
    with _session(tmp_path) as w:
        _open_composer(w)
        w.key("\033x", delay=1.5)
        screen = _screen(w)
        assert "NOT saved" not in screen, screen
        w.key("n", delay=0.8)           # the composer's own "send it?" box, if it came
        w.key("\033x", delay=1.5)
        assert "NOT saved" not in _screen(w), _screen(w)
    assert _files_named_after_the_title(tmp_path) == []


# @scenario SC-window-no-file-04
def test_a_terminated_session_leaves_no_emergency_copy_of_the_composer(tmp_path):
    """SIGTERM with a changed composer open: the emergency copy (.ESV) is made
    for the user's files, not for a window with no file."""
    with _session(tmp_path) as w:
        _open_composer(w)
        os.kill(w.proc.pid, signal.SIGTERM)
        end = time.time() + EXIT_DEADLINE_S
        while time.time() < end and w.proc.poll() is None:
            w._drain(0.1)
        assert w.proc.poll() is not None, "wpe still running after SIGTERM"
    assert _files_named_after_the_title(tmp_path) == []
    assert not [n for n in os.listdir(str(tmp_path)) if n.endswith(".ESV") and "AI" in n]


# @scenario SC-window-no-file-07
def test_save_as_in_the_composer_writes_the_text_and_keeps_the_composer_open(tmp_path):
    """Alt-F a in the composer, a name, Enter: the file holds the text and the
    composer is still open with it: Esc then asks "Send this prompt?" as
    before and Y sends the text.  Save As used to end the composer
    into that question at once, so the sentence F2 gives ("Use Save As") led
    nowhere."""
    with _session(tmp_path) as w:
        _open_composer(w)
        w.key(ALT.FILE, delay=1.0)
        w.key("a", delay=1.0)
        for ch in "keep.txt":
            w.key(ch, delay=0.05)
        w.key("\r", delay=1.5)
        assert "Send this prompt" not in _screen(w), _screen(w)
        assert (tmp_path / "keep.txt").read_text().startswith("hello"), \
            os.listdir(str(tmp_path))
        w.key("more", delay=0.5)            # still the composer: keys go to its text
        w.key("\033", delay=1.0)
        assert "Send this prompt" in _screen(w), _screen(w)
        w.key("y", delay=1.0)               # Y sends what the composer holds
        w._drain(1.5)
    trace = tmp_path / "ai.trace"
    assert "hellomore" in (trace.read_text() if trace.exists() else "")


# @scenario SC-window-no-file-05
def test_f2_in_the_pane_never_writes_a_file_called_ai(tmp_path):
    """F2 in the assistant's pane asks for a name for the transcript; leaving
    that question writes nothing, and a file called AI keeps its text."""
    mine = tmp_path / "AI"
    with _session(tmp_path) as w:
        mine.write_text("my own text\n")
        w.key(ALT.AI)
        w.key("a", delay=1.0)
        w.key(F2, delay=1.0)
        w.key("\033", delay=0.8)
        w.key("\033", delay=0.8)
    assert mine.read_text() == "my own text\n"
