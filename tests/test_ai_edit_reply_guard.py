"""A reply that would destroy the file is never reviewed silently.

Two contracts of the hunk review box.

SC-ai-edit-11: when the reply is prose ("I cannot do that"), whitespace only, a
bare fence pair or a truncated file, the diff is not opened until a question box
that names the line counts is confirmed; declining leaves buffer and disk exactly
as they were.  And `a` (apply the rest) stops at any hunk that deletes more lines
than it adds, showing that hunk instead of applying it unseen.

SC-ai-edit-21: `q` or Esc after some hunks were accepted cancels the whole edit,
those hunks included, and the pane says how many accepted hunks were dropped.

Red/green: before the guard a prose reply replaced the file without a question
("reply has 1 line" never appears and the file on disk becomes the prose), `a`
on the first hunk applied a deletion hunk unseen, and the cancel message named
no count.
"""
import os
import time
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

ORIG3 = "int a = 1;\nint b = 2;\nint c = 3;\n"
BOX = "Proposed change"
QUESTION = "review anyway?"


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _env(workdir, reply):
    return {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_TRACE": str(workdir / "e.trace"),
    }


def _trace(workdir):
    p = workdir / "e.trace"
    return p.read_text() if p.exists() else ""


def _status_row(s):
    """The file window's status row, where a modified buffer shows its mark."""
    for line in s.display(drain=0.3):
        if ":1 " in line or "qIL" in line or "qRS" in line:
            return line
    return ""


def _wait_screen(s, needle, tmo=12):
    end = time.time() + tmo
    while time.time() < end:
        s._drain(0.4)
        if needle in "\n".join(s.display()):
            return True
    return False


def _wait_trace(s, workdir, needle, tmo=12):
    end = time.time() + tmo
    while time.time() < end:
        if needle in _trace(workdir):
            return True
        s._drain(0.4)
    return False


def _start_edit(s, instr="do it"):
    s.key(ALT.AI)
    s.key("e")
    s._drain(0.4)
    s.key(instr)
    s.key("\r", delay=0.8)


def _numbered_lines(count, value):
    return "".join("int v%02d = %d;\n" % (i, value + i) for i in range(count))


# @scenario SC-ai-edit-11
@pytest.mark.parametrize("name,reply", [
    ("prose", "I cannot do that\n"),
    ("whitespace", "   \n \t\n"),
    ("bare_fence", "```\n```\n"),
])
def test_destructive_reply_asks_before_review_and_declining_keeps_the_file(
        tmp_path, name, reply):
    case = tmp_path / name
    case.mkdir()
    with WpeSession(str(case), ORIG3, env_extra=_env(case, reply), filename="t.c") as s:
        _start_edit(s)
        assert _wait_screen(s, QUESTION), \
            "no confirmation for a %s reply:\n%s" % (name, "\n".join(s.display()))
        disp = "\n".join(s.display())
        assert "file has 3" in disp, "the question does not name the file's lines:\n" + disp
        assert BOX not in disp, "the diff was opened before the question was answered"
        s.key("\033", delay=0.8)                      # Esc = decline
        assert _wait_trace(s, case, "edit reply declined"), \
            "declining was not recorded:\n" + _trace(case)
        assert BOX not in "\n".join(s.display()), "declining still opened the diff"
        s.save()
        out = s.text()
        assert s.alive(), "wpe died"
    assert out == ORIG3, "a declined reply changed the file:\n" + repr(out)


# @scenario SC-ai-edit-11
def test_truncated_reply_names_both_line_counts_and_can_be_declined(tmp_path):
    seed = _numbered_lines(20, 0)
    reply = _numbered_lines(10, 100)                  # half the file, all changed
    with WpeSession(str(tmp_path), seed, env_extra=_env(tmp_path, reply),
                    filename="t.c") as s:
        _start_edit(s)
        assert _wait_screen(s, QUESTION), \
            "no confirmation for a truncated reply:\n" + "\n".join(s.display())
        disp = "\n".join(s.display())
        assert "reply has 10 lines" in disp, "reply line count missing:\n" + disp
        assert "file has 20" in disp, "file line count missing:\n" + disp
        s.key("\033", delay=0.8)
        assert _wait_trace(s, tmp_path, "edit reply declined"), _trace(tmp_path)
        s.save()
        out = s.text()
    assert out == seed, "a declined truncation changed the file"


# @scenario SC-ai-edit-11
def test_confirming_the_question_opens_the_review_as_usual(tmp_path):
    with WpeSession(str(tmp_path), ORIG3, env_extra=_env(tmp_path, "I cannot do that\n"),
                    filename="t.c") as s:
        _start_edit(s)
        assert _wait_screen(s, QUESTION), "no confirmation box"
        s.key("\r", delay=0.8)                        # Enter = Yes, review it
        assert _wait_screen(s, BOX), \
            "confirming did not open the review:\n" + "\n".join(s.display())
        s.key("q", delay=0.8)
        s.save()
        out = s.text()
    assert out == ORIG3, "cancelling the review changed the file"


# @scenario SC-ai-edit-11
def test_apply_rest_stops_at_a_hunk_that_only_deletes(tmp_path):
    seed = _numbered_lines(10, 0)
    lines = seed.splitlines(True)
    lines[0] = "int v00 = 999;\n"                     # hunk 1: one line changed
    del lines[7:9]                                    # hunk 2: two lines deleted
    reply = "".join(lines)
    with WpeSession(str(tmp_path), seed, env_extra=_env(tmp_path, reply),
                    filename="t.c") as s:
        _start_edit(s)
        assert _wait_screen(s, "Proposed change 1/2"), \
            "first hunk not shown:\n" + "\n".join(s.display())
        s.key("a", delay=0.8)                         # apply the rest
        assert _wait_screen(s, "Proposed change 2/2"), \
            "'a' applied the deletion hunk unseen:\n" + "\n".join(s.display())
        disp = "\n".join(s.display())
        assert "deletes 2 adds 0" in disp, \
            "the box does not say why 'a' stopped:\n" + disp
        s.key("n", delay=0.8)                         # skip the deletion
        assert _wait_trace(s, tmp_path, "edit applied 1/2"), _trace(tmp_path)
        s.save()
        out = s.text()
    assert "int v00 = 999;" in out, "the accepted hunk was not applied"
    assert "int v07 = 7;" in out and "int v08 = 8;" in out, \
        "the deletion hunk was applied although it was skipped:\n" + out


# @scenario SC-ai-edit-21
def test_quit_after_accepting_drops_the_accepted_hunks_and_says_how_many(tmp_path):
    seed = "int a = 1;\nint b = 2;\nint c = 3;\nint d = 4;\nint e = 5;\n"
    reply = "int a = 11;\nint b = 2;\nint c = 3;\nint d = 4;\nint e = 55;\n"
    with WpeSession(str(tmp_path), seed, env_extra=_env(tmp_path, reply),
                    filename="t.c") as s:
        _start_edit(s, "bump a and e")
        assert _wait_screen(s, "Proposed change 1/2"), "first hunk not shown"
        s.key("y", delay=0.8)                         # accept hunk 1
        assert _wait_screen(s, "Proposed change 2/2"), "second hunk not shown"
        s.key("q", delay=0.8)                         # quit the review
        assert _wait_trace(s, tmp_path, "edit discarded"), _trace(tmp_path)
        disp = "\n".join(s.display())
        assert "1 accepted hunk dropped" in disp, \
            "the pane does not say how many accepted hunks were dropped:\n" + disp
        assert "int a = 11;" not in disp, "the accepted hunk stayed in the buffer"
        assert "*" not in _status_row(s), \
            "the window was left modified by a cancelled review:\n" + _status_row(s)
        s.key("\033x", delay=0.8)                     # Alt-X: quit, nothing to save
        end = time.time() + 10                        # tearing down two windows
        while time.time() < end and s.alive():        # takes longer than one drain
            s._drain(0.5)
        assert not s.alive(), "Alt-X did not quit: something asked to be saved"
    with open(tmp_path / "t.c") as fh:
        assert fh.read() == seed, "disk changed after cancelling the review"
