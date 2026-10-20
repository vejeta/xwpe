"""An AI reply that lands while a dialog is open waits for the dialog to close.

Alt-G e starts an edit that finishes from the fd-loop.  That loop is pumped
from inside the key read of every dialog, prompt, picker and the file manager,
so a reply that arrived while one of those was open used to run its hunk
review right there: the " Proposed change " box stacked over the Alt-G task
prompt or the File-Manager, and the next letter the user typed into the
dialog ("a" of "add a comment") was read as the review key and applied the
whole edit unseen.

These tests start an edit against the mock backend with a long reply delay,
open a dialog before the reply lands, and assert that no review box appears
while the dialog is up, that keys typed into the dialog never act on the
review, and that the review opens on its own once the dialog closes, where y
applies (once) and q or Esc discards.

Red/green: before the fix the box is on screen over the open dialog within
the mock's delay and the typed letter applies the edit (trace "edit applied"
with the prompt still open); with it the trace shows "edit reply held" and
the box appears only after the dialog has closed.
"""
import os
import subprocess
import time
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN, WAIT_SCALE

ORIG = "int a = 1;\nint b = 2;\nint c = 3;\nint d = 4;\nint e = 5;\n"
NEW = "int a = 1;\nint b = 2;\nint c = 3;\nint d = 4;\nint e = 55;\n"
# The reply lands while the dialog is still open.  Scaled like every wait of
# the driver, or on a slow runner the reply beats the dialog it must wait for.
MOCK_DELAY_MS = int(3000 * WAIT_SCALE)
F3 = "\033OR"                 # File -> File-Manager
BOX = "Proposed change"       # the hunk review box title
PROMPT = "AI agent task"      # the Alt-G g task prompt title
FILE_MANAGER = "File-Manager"


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _trace(path):
    return path.read_text() if path.exists() else ""


def _wait_trace(s, path, needle, tmo):
    end = time.time() + tmo
    while time.time() < end:
        s._drain(0.5)
        if needle in _trace(path):
            return True
    return False


def _wait_screen(s, needle, tmo):
    end = time.time() + tmo
    while time.time() < end:
        s._drain(0.5)
        if any(needle in r for r in s.display(drain=0)):
            return True
    return False


def _on_screen(s, needle):
    return any(needle in r for r in s.display())


def _screen(s):
    return "\n".join(s.display())


def _env(trace):
    return {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": NEW,
        "XWPE_AI_MOCK_DELAY_MS": str(MOCK_DELAY_MS),
        "XWPE_AI_TRACE": str(trace),
    }


def _start_edit(s, trace):
    s.key(ALT.AI); s.key("e"); s._drain(0.4)
    s.key("do it"); s.key("\r", delay=0.8)
    assert _wait_trace(s, trace, "edit stream fd=", 10), \
        "the edit never started:\n" + _trace(trace)


def _outlive_the_reply(s):
    """Wait until the mock has certainly written its reply."""
    s._drain(MOCK_DELAY_MS / 1000.0 + 1.0)


# @scenario SC-ai-edit-08b
def test_reply_behind_task_prompt_waits_for_it(tmp_path):
    trace = tmp_path / "e.trace"
    with WpeSession(str(tmp_path), ORIG, env_extra=_env(trace), filename="t.c") as s:
        _start_edit(s, trace)

        # Open the Alt-G task prompt before the reply lands, then outlive it.
        s.key(ALT.AI, delay=0.3); s.key("g", delay=0.3)
        assert _on_screen(s, PROMPT), "the task prompt did not open:\n" + _screen(s)
        _outlive_the_reply(s)
        assert not _on_screen(s, BOX), \
            "the review box was drawn over the open task prompt:\n" + _screen(s)
        assert _on_screen(s, PROMPT), "the task prompt went away:\n" + _screen(s)
        assert "edit applied" not in _trace(trace), \
            "the edit was applied while the prompt was open:\n" + _trace(trace)
        assert "edit reply held" in _trace(trace), \
            "the reply was not held behind the prompt:\n" + _trace(trace)

        # Type a task into the prompt: its letters are the prompt's, never the
        # review's ("a" would be "apply all" in the box).
        s.key("add a comment", delay=0.3)
        assert not _on_screen(s, BOX), \
            "the review box appeared while typing into the prompt:\n" + _screen(s)
        assert "edit applied" not in _trace(trace), \
            "a letter typed into the prompt applied the edit:\n" + _trace(trace)
        assert _on_screen(s, "add a comment"), \
            "the typed task did not reach the prompt:\n" + _screen(s)

        # Close the prompt: the review opens as if the reply had just landed.
        s.key("\033", delay=0.8)
        assert _wait_screen(s, BOX, 5), \
            "the review did not open once the prompt closed:\n" + _screen(s)
        assert not _on_screen(s, PROMPT), "the prompt is still up:\n" + _screen(s)

        # y applies the one hunk, exactly once; the typed text never reached the file.
        s.key("y", delay=0.9)
        assert _wait_trace(s, trace, "edit applied", 8), \
            "the edit was not applied after y:\n" + _trace(trace)
        s._drain(0.6)
        s.save()
        assert s.text() == NEW, "the file on disk is not the reviewed change:\n" + repr(s.text())
        assert _trace(trace).count("edit applied") == 1, \
            "the edit was applied more than once:\n" + _trace(trace)
        assert s.alive(), "wpe died after the deferred review"


# @scenario SC-ai-edit-08b
def test_reply_behind_file_manager_waits_for_it(tmp_path):
    trace = tmp_path / "e.trace"
    with WpeSession(str(tmp_path), ORIG, env_extra=_env(trace), filename="t.c") as s:
        _start_edit(s, trace)

        # Open the File-Manager before the reply lands, then outlive it.
        s.key(F3, delay=0.8)
        assert _on_screen(s, FILE_MANAGER), "the file manager did not open:\n" + _screen(s)
        _outlive_the_reply(s)
        assert not _on_screen(s, BOX), \
            "the review box was drawn over the open file manager:\n" + _screen(s)
        assert _on_screen(s, FILE_MANAGER), "the file manager went away:\n" + _screen(s)
        assert "edit applied" not in _trace(trace) and "edit discarded" not in _trace(trace), \
            "the review ran while the file manager was open:\n" + _trace(trace)
        assert "edit reply held" in _trace(trace), \
            "the reply was not held behind the file manager:\n" + _trace(trace)

        # Close the file manager: the review opens on its own.
        s.key("\033", delay=0.8)
        assert _wait_screen(s, BOX, 5), \
            "the review did not open once the file manager closed:\n" + _screen(s)

        # q discards: nothing applied, and the screen is the plain editor again.
        s.key("q", delay=0.9)
        assert _wait_trace(s, trace, "edit discarded", 8), \
            "q did not discard the deferred review:\n" + _trace(trace)
        s._drain(0.6)
        disp = _screen(s)
        assert BOX not in disp, "the review box is still on screen:\n" + disp
        assert FILE_MANAGER not in disp and "DirTree" not in disp, \
            "the file manager was left half-painted:\n" + disp
        assert "int e = 5;" in disp, "the file window is not back on screen:\n" + disp
        s.save()
        assert s.text() == ORIG, "the file on disk changed after q:\n" + repr(s.text())
        assert s.alive(), "wpe died after the deferred review"


# @scenario SC-ai-edit-08b
def test_escape_after_deferral_applies_nothing(tmp_path):
    trace = tmp_path / "e.trace"
    with WpeSession(str(tmp_path), ORIG, env_extra=_env(trace), filename="t.c") as s:
        _start_edit(s, trace)

        # The reply lands behind the task prompt.
        s.key(ALT.AI, delay=0.3); s.key("g", delay=0.3)
        assert _on_screen(s, PROMPT), "the task prompt did not open:\n" + _screen(s)
        _outlive_the_reply(s)
        assert not _on_screen(s, BOX), \
            "the review box was drawn over the open task prompt:\n" + _screen(s)
        assert "edit reply held" in _trace(trace), \
            "the reply was not held behind the prompt:\n" + _trace(trace)

        # Esc closes the prompt; the deferred review opens; Esc ends it with
        # nothing applied.
        s.key("\033", delay=0.8)
        assert _wait_screen(s, BOX, 5), \
            "the review did not open once the prompt closed:\n" + _screen(s)
        s.key("\033", delay=0.9)
        assert _wait_trace(s, trace, "edit discarded", 8), \
            "Esc did not end the deferred review:\n" + _trace(trace)
        s._drain(0.6)
        txt = _trace(trace)
        assert "edit applied" not in txt, "the edit was applied:\n" + txt
        assert not _on_screen(s, BOX), "the review box is still on screen:\n" + _screen(s)
        s.save()
        assert s.text() == ORIG, "the file on disk changed:\n" + repr(s.text())

        # The task is over: a new edit can start (no "already running" refusal).
        s.key(ALT.AI); s.key("e"); s._drain(0.5)
        assert _on_screen(s, "AI edit") or _on_screen(s, "instruction"), \
            "a new edit prompt did not open after the discard:\n" + _screen(s)
        assert not _on_screen(s, "already running"), \
            "the discarded task is still counted as running:\n" + _screen(s)
        assert s.alive(), "wpe died after the deferred review"
