"""Size limits never lose text: a file larger than the context cap keeps its tail.

The AI modes send at most AI_CTX_MAX (16000) bytes of the current file to the
model.  Before this guard, the same capped text was also what the reply was
diffed against and what the buffer was rebuilt from, so accepting an Edit, a
Complete or a block edit on a 1200-line file wrote a ~340-line file to disk.

The contract: the model sees a window of whole lines around the cursor (or the
marked block); the reply is spliced over exactly that window; every other line
is untouched and never shown as a hunk; the pane names the lines that were
sent.  Files under the cap behave exactly as before.

The seed is 1200 lines of 46 characters (56 KB).  For the whole-file Edit the
mock must answer the way a model answers for the WINDOW, so the test computes
the window the editor picks with the same rule: start at the cursor line and
grow one line downwards, then one upwards, as long as the next line still fits
in the cap.  With the cursor on line 600 that is lines 431-770 (340 lines,
15980 bytes).
"""
import os
import time
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

AI_CTX_MAX = 16000
LINES = ["int v%04d = %d; /* padding padding padding */" % (i, i) for i in range(1200)]
BIG = "".join(l + "\n" for l in LINES)
CURSOR_LINE = 600                      # 1-based, as typed into Go to Line


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _env(tmp_path, reply):
    return {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
            "XWPE_AI_MOCK_REPLY": reply, "XWPE_AI_TRACE": str(tmp_path / "e.trace")}


def _trace(tmp_path):
    p = tmp_path / "e.trace"
    return p.read_text() if p.exists() else ""


def _wait(s, needle, tmo=12):
    end = time.time() + tmo
    while time.time() < end:
        s._drain(0.5)
        if needle in "\n".join(s.display()):
            return True
    return False


def _goto_line(s, n):
    """Search -> Go to Line, type the number, Enter."""
    s.key(ALT.SEARCH, delay=0.4)
    s.key("g", delay=0.6)
    s.key(str(n)); s.key("\r", delay=0.6)


def _send_edit(s, instr):
    s.key(ALT.AI); s.key("e"); s._drain(0.5)
    s.key(instr); s.key("\r", delay=0.8)


def _count_lines(path):
    with open(path) as fh:
        return fh.read().count("\n")


def context_window(lines, cur, cap=AI_CTX_MAX):
    """The window the editor sends: the cursor line, grown one line down then
    one line up while the next line still fits in the cap (0-based, inclusive)."""
    lo = hi = cur
    used = len(lines[cur]) + 1
    grew = True
    while grew:
        grew = False
        if hi < len(lines) - 1 and used + len(lines[hi + 1]) + 1 <= cap:
            hi += 1; used += len(lines[hi]) + 1; grew = True
        if lo > 0 and used + len(lines[lo - 1]) + 1 <= cap:
            lo -= 1; used += len(lines[lo]) + 1; grew = True
    return lo, hi


# @scenario SC-ai-edit-33
# @scenario SC-ai-runtime-14
def test_whole_file_edit_over_cap_keeps_the_tail(tmp_path):
    """Edit with no block on a 1200-line file: the reply for the window is
    spliced over the window; the file keeps its 1200 lines."""
    cur = CURSOR_LINE - 1
    lo, hi = context_window(LINES, cur)
    assert lo < cur < hi and (hi - lo + 1) < 1200       # a real window, not the file
    window = LINES[lo:hi + 1]
    changed = "int v0599 = 999; /* padding padding padding */"
    window[cur - lo] = changed
    reply = "".join(l + "\n" for l in window)
    with WpeSession(str(tmp_path), BIG, env_extra=_env(tmp_path, reply)) as s:
        _goto_line(s, CURSOR_LINE)
        _send_edit(s, "bump v599")
        assert _wait(s, "Proposed change"), "no diff overlay:\n" + "\n".join(s.display())
        assert "Proposed change 1/1" in "\n".join(s.display()), \
            "the window edit must show exactly one hunk:\n" + "\n".join(s.display())
        s.key("y", delay=0.8); s._drain(0.8)
        pane = [r for r in s.display() if "[AI edit]" in r]
        s.save()
        n = _count_lines(s.path)
        out = s.text()
        assert s.alive()
    txt = _trace(tmp_path)
    assert "edit applied 1/1" in txt, txt
    assert n == 1200, "the tail was lost: %d lines on disk after accepting the edit" % n
    assert changed in out and "int v0599 = 599;" not in out, "the edited line did not change"
    assert "int v1199 = 1199;" in out and "int v0000 = 0;" in out
    sent = "lines %d-%d of 1200 sent" % (lo + 1, hi + 1)
    assert any(sent in r for r in pane), \
        "the pane must name the lines sent (%s):\n%s" % (sent, "\n".join(pane))


# @scenario SC-ai-edit-33
def test_complete_over_cap_keeps_the_tail(tmp_path):
    """Complete at line 600 of a 1200-line file: the suggestion is reviewed,
    y lands it at the cursor and the file keeps all 1200 lines around it.  The
    reply is inserted exactly as returned, so the model's own "PREFIX\\n" is a
    line of its own and the file is the 1200 original lines plus that one line,
    in order."""
    with WpeSession(str(tmp_path), BIG, env_extra=_env(tmp_path, "PREFIX\n")) as s:
        _goto_line(s, CURSOR_LINE)
        s.key(ALT.AI, delay=0.6); s.key("c", delay=0.8)
        assert _wait(s, "Proposed completion"), \
            "the completion was not offered for review:\n" + "\n".join(s.display())
        s.key("y", delay=0.8)
        assert _wait(s, "completed at cursor"), \
            "completion did not land:\n" + "\n".join(s.display())
        s.save()
        n = _count_lines(s.path)
        out = s.text()
        assert s.alive()
    assert n == 1200 + 1, "the tail was lost: %d lines on disk after Complete" % n
    got = out.split("\n")[:-1]
    assert got == LINES[:CURSOR_LINE - 1] + ["PREFIX"] + LINES[CURSOR_LINE - 1:], \
        "the file is not the original plus the suggestion at the cursor; last lines: %r" % got[-3:]


# @scenario SC-ai-edit-33
def test_block_edit_over_cap_shows_no_phantom_hunk(tmp_path):
    """Block edit of lines 2-3 on a 1200-line file: one hunk only (no phantom
    hunk for the tail outside the cap); after 'y' the file keeps its 1200 lines."""
    reply = ("int v0001 = 111; /* padding padding padding */\n"
             "int v0002 = 222; /* padding padding padding */\n")
    with WpeSession(str(tmp_path), BIG, env_extra=_env(tmp_path, reply)) as s:
        s.key("\033[B")                       # cursor -> line 2
        s.key(ALT.BLOCK, "b", delay=0.4)      # Begin Mark
        s.key("\033[B"); s.key("\033[B")      # cursor -> line 4 (col 0)
        s.key(ALT.BLOCK, "e", delay=0.4)      # End Mark => block = lines 2-3
        _send_edit(s, "edit block")
        assert _wait(s, "Proposed change"), "no diff overlay:\n" + "\n".join(s.display())
        box = "\n".join(s.display())
        assert " 1/1 " in box, "the block edit must show a single hunk:\n" + box
        s.key("y", delay=0.8); s._drain(0.8)
        after = "\n".join(s.display())
        assert "Proposed change 2/" not in after, "phantom hunk for the file tail:\n" + after
        s.save()
        n = _count_lines(s.path)
        out = s.text()
        assert s.alive()
    txt = _trace(tmp_path)
    assert "edit applied 1/1" in txt, txt
    assert n == 1200, "the tail was lost: %d lines on disk after the block edit" % n
    assert "int v0001 = 111;" in out and "int v0002 = 222;" in out
    assert "int v0000 = 0;" in out and "int v0003 = 3;" in out and "int v1199 = 1199;" in out


# @scenario SC-ai-edit-33
def test_small_file_edit_is_unchanged(tmp_path):
    """A file under the cap still goes through the whole-file path: the mock
    returns the whole file, one hunk, accept writes it, no window line."""
    seed = "int a = 1;\nint b = 2;\nint c = 3;\n"
    reply = "int a = 1;\nint b = 22;\nint c = 3;\n"
    with WpeSession(str(tmp_path), seed, env_extra=_env(tmp_path, reply)) as s:
        _send_edit(s, "double b")
        assert _wait(s, "Proposed change"), "no diff overlay:\n" + "\n".join(s.display())
        assert "Proposed change 1/1" in "\n".join(s.display())
        s.key("y", delay=0.8); s._drain(0.8)
        pane = [r for r in s.display() if "[AI edit]" in r]
        s.save()
        out = s.text()
        assert s.alive()
    txt = _trace(tmp_path)
    assert "sel=-1..-1" in txt, "a small file must not be windowed:\n" + txt
    assert "edit applied 1/1" in txt, txt
    assert out == reply, "disk: %r" % out
    assert not any(" sent" in r for r in pane), "no window line for a small file:\n" + "\n".join(pane)
