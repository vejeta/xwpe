"""A completion is shown before it lands, and it lands exactly where the cursor is.

Alt-G c asks the model for the text to insert at the cursor.  Two contracts of
that path are pinned here.

The suggestion is reviewed first: when the reply arrives a one-hunk box titled
" Proposed completion " shows it at its place, with the same colours and keys as
the Edit review.  The buffer changes only on y or Enter; n, q or Esc leave it
untouched and the pane says "[AI complete] discarded".  A reply that lands while
a dialog is open waits, like an Edit's does, and opens its box once the dialog
closes.

The suggestion is spliced exactly as returned, trimmed only of markdown fences:
no newline is forced after it, so completing "foo(" with "bar)" gives
"foo(bar)" on one line, and one Ctrl-U removes it again.

Red/green: on the unfixed tree no box ever opens (the suggestion is written into
the buffer the moment the reply lands) and a mid-line completion left
"foo(bar)" followed by an empty line, because the cleaner appended a newline to
every reply.
"""
import os
import subprocess
import time
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

BOX = "Proposed completion"       # the completion review box title
PROMPT = "AI agent task"          # the Alt-G g task prompt title
DISCARDED = "[AI complete] discarded"
LANDED = "completed at cursor"
MOCK_DELAY_MS = 3000              # the reply lands while the dialog is still open
RIGHT = "\033[C"
CTRL_U = "\x15"
ESC = "\033"


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
        "XWPE_AI_TRACE": str(tmp_path / "c.trace"),
    }
    env.update(extra)
    return env


def _trace(tmp_path):
    p = tmp_path / "c.trace"
    return p.read_text() if p.exists() else ""


def _screen(s):
    return "\n".join(s.display())


def _on_screen(s, needle):
    return any(needle in r for r in s.display())


def _wait_screen(s, needle, tmo=12):
    end = time.time() + tmo
    while time.time() < end:
        s._drain(0.4)
        if _on_screen(s, needle):
            return True
    return False


def _wait_trace(s, tmp_path, needle, tmo=10):
    end = time.time() + tmo
    while time.time() < end:
        s._drain(0.4)
        if needle in _trace(tmp_path):
            return True
    return False


def _disk(s):
    with open(s.path) as fh:
        return fh.read()


def _start_complete(s, tmp_path):
    s.key(ALT.AI, delay=0.5)
    s.key("c", delay=0.6)
    assert _wait_trace(s, tmp_path, "complete stream fd="), \
        "the completion never started:\n" + _trace(tmp_path)


def _discard_with(tmp_path, key):
    seed = "ZZZ\n"
    with WpeSession(str(tmp_path), seed, env_extra=_env(tmp_path, "PREFIX"), filename="t.c") as s:
        _start_complete(s, tmp_path)
        assert _wait_screen(s, BOX), "no review box:\n" + _screen(s)
        s.key(key, delay=0.8)
        assert _wait_trace(s, tmp_path, "complete discarded", 8), \
            "the discard was not reported:\n" + _trace(tmp_path)
        s._drain(0.5)
        disp = _screen(s)
        assert BOX not in disp, "the box is still open:\n" + disp
        assert DISCARDED in disp, "the pane does not say discarded:\n" + disp
        assert "PREFIX" not in disp, "the suggestion reached the buffer:\n" + disp
        assert "complete applied" not in _trace(tmp_path), \
            "the completion was applied:\n" + _trace(tmp_path)
        s.save()
        assert s.text() == seed, "the file changed after %r:\n%r" % (key, s.text())
        assert s.alive(), "wpe died"


# @scenario SC-ai-edit-41
def test_completion_opens_review_box_and_lands_only_on_y(tmp_path):
    seed = "ZZZ\n"
    with WpeSession(str(tmp_path), seed, env_extra=_env(tmp_path, "PREFIX"), filename="t.c") as s:
        _start_complete(s, tmp_path)
        assert _wait_screen(s, BOX), "no review box:\n" + _screen(s)
        disp = _screen(s)
        assert "PREFIX" in disp, "the suggestion is not shown in the box:\n" + disp
        assert "complete applied" not in _trace(tmp_path), \
            "the completion was applied before the review:\n" + _trace(tmp_path)
        assert _disk(s) == seed, "disk changed while the box was open"
        s.key("y", delay=0.8)
        assert _wait_trace(s, tmp_path, "complete applied", 8), \
            "y did not apply the completion:\n" + _trace(tmp_path)
        s._drain(0.5)
        disp = _screen(s)
        assert BOX not in disp, "the box is still open after y:\n" + disp
        assert LANDED in disp, "the pane does not say the completion landed:\n" + disp
        s.save()
        out = s.text()
        assert s.alive(), "wpe died"
    assert out == "PREFIXZZZ\n", "the suggestion did not land at the cursor:\n" + repr(out)


# @scenario SC-ai-edit-41
def test_n_discards_the_completion(tmp_path):
    _discard_with(tmp_path, "n")


# @scenario SC-ai-edit-41
def test_esc_discards_the_completion(tmp_path):
    _discard_with(tmp_path, ESC)


# @scenario SC-ai-edit-38
def test_midline_completion_adds_no_newline_and_one_undo_removes_it(tmp_path):
    seed = "foo(\n"
    with WpeSession(str(tmp_path), seed, env_extra=_env(tmp_path, "bar)"), filename="t.c") as s:
        for _ in range(4):                       # cursor after "foo("
            s.key(RIGHT, delay=0.1)
        _start_complete(s, tmp_path)
        assert _wait_screen(s, BOX), "no review box:\n" + _screen(s)
        assert _on_screen(s, "foo(bar)"), \
            "the box does not show the completed line:\n" + _screen(s)
        s.key("y", delay=0.8)
        assert _wait_trace(s, tmp_path, "complete applied", 8), \
            "y did not apply the completion:\n" + _trace(tmp_path)
        s._drain(0.5)
        assert _on_screen(s, LANDED), "the pane does not say the completion landed:\n" + _screen(s)
        s.save()
        after_y = s.text()
        s.key(CTRL_U, delay=0.8)
        s.save()
        after_undo = s.text()
        assert s.alive(), "wpe died"
    assert after_y == "foo(bar)\n", \
        "the completion was not spliced exactly at the cursor:\n" + repr(after_y)
    assert after_undo == seed, "one Ctrl-U did not remove the completion:\n" + repr(after_undo)


# @scenario SC-ai-edit-41
def test_completion_behind_task_prompt_waits_and_opens_its_box_after_esc(tmp_path):
    seed = "ZZZ\n"
    env = _env(tmp_path, "PREFIX", XWPE_AI_MOCK_DELAY_MS=str(MOCK_DELAY_MS))
    with WpeSession(str(tmp_path), seed, env_extra=env, filename="t.c") as s:
        _start_complete(s, tmp_path)

        # Open the Alt-G task prompt before the reply lands, then outlive it.
        s.key(ALT.AI, delay=0.3); s.key("g", delay=0.3)
        assert _on_screen(s, PROMPT), "the task prompt did not open:\n" + _screen(s)
        s._drain(MOCK_DELAY_MS / 1000.0 + 1.0)
        assert not _on_screen(s, BOX), \
            "the review box was drawn over the open task prompt:\n" + _screen(s)
        assert _on_screen(s, PROMPT), "the task prompt went away:\n" + _screen(s)
        assert "complete applied" not in _trace(tmp_path), \
            "the completion landed while the prompt was open:\n" + _trace(tmp_path)
        assert "complete reply held" in _trace(tmp_path), \
            "the reply was not held behind the prompt:\n" + _trace(tmp_path)

        # Esc closes the prompt: the review opens as if the reply had just landed.
        s.key(ESC, delay=0.8)
        assert _wait_screen(s, BOX, 5), \
            "the review did not open once the prompt closed:\n" + _screen(s)
        assert not _on_screen(s, PROMPT), "the prompt is still up:\n" + _screen(s)
        assert _disk(s) == seed, "disk changed before the review was answered"

        s.key("y", delay=0.9)
        assert _wait_trace(s, tmp_path, "complete applied", 8), \
            "y did not apply the deferred completion:\n" + _trace(tmp_path)
        s._drain(0.5)
        s.save()
        out = s.text()
        assert s.alive(), "wpe died after the deferred review"
    assert out == "PREFIXZZZ\n", "the deferred completion did not land at the cursor:\n" + repr(out)
