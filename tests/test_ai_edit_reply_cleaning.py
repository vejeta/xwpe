"""Only the fenced code of an Edit reply becomes file content.

SC-ai-edit-09: when the reply wraps the code in ``` fences, with a prose
preamble, trailing prose after the closing fence, or a language tag, only the
content of the outermost fence pair lands in the file, ending in exactly one
newline.  The commentary around the fences (and any second fenced block) is not
file content: it is shown in the assistant pane under a line that says so.

Red/green: before the fix a preamble ("Here is the updated file:") and the
fence line were written to disk, trailing prose ("Let me know if you need
more.") followed the code into the file, and a reply with two fenced blocks
delivered both plus the prose between them.
"""
import os
import time
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

ORIG = "int a = 1;\nint b = 2;\nint c = 3;\n"
NEW = "int a = 1;\nint b = 22;\nint c = 3;\n"
BOX = "Proposed change"
COMMENTARY = "commentary"          # the pane line that introduces the prose


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _env(tmp_path, reply):
    return {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_TRACE": str(tmp_path / "e.trace"),
    }


def _trace(tmp_path):
    p = tmp_path / "e.trace"
    return p.read_text() if p.exists() else ""


def _wait_screen(s, needle, tmo=12):
    end = time.time() + tmo
    while time.time() < end:
        s._drain(0.4)
        if needle in "\n".join(s.display()):
            return True
    return False


def _start_edit(s, instr="bump b"):
    s.key(ALT.AI)
    s.key("e")
    s._drain(0.4)
    s.key(instr)
    s.key("\r", delay=0.8)


def _accept_with_y(tmp_path, reply):
    """Run one Edit against `reply`, press y on the (single) hunk, save.

    Returns (text on disk, screen after the apply, trace)."""
    with WpeSession(str(tmp_path), ORIG, env_extra=_env(tmp_path, reply), filename="t.c") as s:
        _start_edit(s)
        assert _wait_screen(s, BOX), "no review box"
        s.key("y", delay=0.7)
        for _ in range(20):            # the review ends when it says what it did
            if "edit applied" in _trace(tmp_path) or "edit discarded" in _trace(tmp_path):
                break
            s._drain(0.3)
        screen = "\n".join(s.display())
        s.save()
        out = s.text()
        assert s.alive(), "wpe died"
    return out, screen, _trace(tmp_path)


# @scenario SC-ai-edit-09
def test_preamble_before_the_fence_stays_out_of_the_file(tmp_path):
    reply = "Here is the updated file:\n```c\n" + NEW + "```\n"
    out, screen, trace = _accept_with_y(tmp_path, reply)
    assert out == NEW, "preamble or fence reached the disk:\n" + repr(out)
    assert "edit applied 1/1" in trace, "the fenced code should be one hunk:\n" + trace
    assert COMMENTARY in screen, "pane did not announce the commentary:\n" + screen
    assert "Here is the updated file:" in screen, "preamble not shown in the pane:\n" + screen


# @scenario SC-ai-edit-09
def test_prose_after_the_closing_fence_stays_out_of_the_file(tmp_path):
    reply = "```c\n" + NEW + "```\nLet me know if you need more.\n"
    out, screen, trace = _accept_with_y(tmp_path, reply)
    assert out == NEW, "trailing prose or fence reached the disk:\n" + repr(out)
    assert "edit applied 1/1" in trace, "the fenced code should be one hunk:\n" + trace
    assert COMMENTARY in screen, "pane did not announce the commentary:\n" + screen
    assert "Let me know if you need more." in screen, "trailing prose not shown in the pane:\n" + screen


# @scenario SC-ai-edit-09
def test_second_fenced_block_is_commentary_not_file_content(tmp_path):
    reply = "```c\n" + NEW + "```\n\nAnd a test:\n```c\nint t;\n```\n"
    out, screen, trace = _accept_with_y(tmp_path, reply)
    assert out == NEW, "the second block or the prose between reached the disk:\n" + repr(out)
    assert "edit applied 1/1" in trace, "the first fenced block should be one hunk:\n" + trace
    assert COMMENTARY in screen, "pane did not announce the commentary:\n" + screen
    assert "And a test:" in screen, "prose between the blocks not shown in the pane:\n" + screen
    assert "int t;" in screen, "the second block not shown in the pane:\n" + screen


# @scenario SC-ai-edit-09
def test_a_file_with_its_own_fences_is_taken_whole(tmp_path):
    """A Markdown file returned unfenced must not be cut at its own first fence.

    The page below carries a code sample between fence lines.  The model
    returns the whole page with one word changed and no wrapper.  Every line
    must survive, the fence lines included; before the guard the cleaner took
    the sample as the file and offered the rest back as deletions.
    """
    page = ("# Title\n\nSome prose.\n\n```c\nint a = 1;\n```\n\nMore prose.\n")
    reply = page.replace("Some prose.", "Some changed prose.")
    env = _env(tmp_path, reply)
    with WpeSession(str(tmp_path), page, env_extra=env, filename="page.md") as s:
        _start_edit(s)
        assert _wait_screen(s, BOX), "no review box"
        s.key("y", delay=0.9)
        s._drain(0.6)
        s.save()
        out = s.text()
    assert out == reply, "the fenced sample or the prose around it was lost:\n" + repr(out)
    assert "```c" in out and out.count("```") == 2, "the file's own fence lines did not survive"


# @scenario SC-ai-edit-09
def test_language_tag_is_dropped_and_the_code_ends_in_one_newline(tmp_path):
    reply = "```python\n" + NEW.rstrip("\n") + "\n\n\n```"
    out, screen, trace = _accept_with_y(tmp_path, reply)
    assert out == NEW, "language tag, fence or blank tail reached the disk:\n" + repr(out)
    assert "edit applied 1/1" in trace, "the fenced code should be one hunk:\n" + trace
    assert COMMENTARY not in screen, "a reply with nothing around the fences has no commentary:\n" + screen
