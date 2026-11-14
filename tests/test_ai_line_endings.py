"""An accepted assistant Edit never changes a line it did not touch.

A file whose line breaks are mixed (some CR LF, some LF) is an LF file whose
CR bytes are text, shown ^M.  The model sees those lines and answers with LF
endings, or with CR LF; either way the review shows only the lines that
really changed, and the lines it leaves alone keep their carriage return on
disk.  Every row reads the file with open(path, "rb").
"""
import os
import subprocess
import time

import pytest

from wpe_driver import ALT, WPE_BIN, WpeSession


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")

MIXED = b"int a = 1;\r\nint b = 2;\nint c = 3;\r\n"
BOX = "Proposed change"


def _env(tmp_path, reply):
    return {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_TRACE": str(tmp_path / "e.trace"),
    }


def _wait_screen(s, needle, tmo=12):
    end = time.time() + tmo
    while time.time() < end:
        s._drain(0.4)
        if needle in "\n".join(s.display()):
            return True
    return False


def _start_edit(s, instr="do it"):
    s.key(ALT.AI)
    s.key("e")
    s._drain(0.4)
    s.key(instr)
    s.key("\r", delay=0.8)


def _disk(tmp_path, name="t.c"):
    with open(os.path.join(str(tmp_path), name), "rb") as fh:
        return fh.read()


def _edit_whole_file(tmp_path, seed, reply):
    """Open @seed, ask for an Edit, accept the one hunk, save: the review
    text and the bytes on disk."""
    with WpeSession(str(tmp_path), seed, env_extra=_env(tmp_path, reply),
                    filename="t.c") as s:
        _start_edit(s)
        assert _wait_screen(s, BOX), "no review box"
        shown = "\n".join(s.display())
        s.key("y", delay=0.7)
        s._drain(0.5)
        s.save()
    return shown, _disk(tmp_path)


# @scenario SC-line-endings-24
def test_an_edit_keeps_the_carriage_return_of_a_line_it_left_alone(tmp_path):
    """The reply has LF endings and changes line 2 only: the review shows
    one hunk, and lines 1 and 3 are still CR LF on disk."""
    shown, got = _edit_whole_file(tmp_path, MIXED,
                                  "int a = 1;\nint b = 22;\nint c = 3;\n")
    assert "Proposed change 1/1" in shown, shown
    assert "- int a = 1;" not in shown and "- int c = 3;" not in shown, shown
    assert got == b"int a = 1;\r\nint b = 22;\nint c = 3;\r\n", got


# @scenario SC-line-endings-25
def test_an_edit_answered_in_crlf_changes_the_same_one_line(tmp_path):
    """A model that answers with Windows line endings changes nothing but
    the line it meant to change."""
    shown, got = _edit_whole_file(tmp_path, MIXED,
                                  "int a = 1;\r\nint b = 22;\r\nint c = 3;\r\n")
    assert "Proposed change 1/1" in shown, shown
    assert got == b"int a = 1;\r\nint b = 22;\nint c = 3;\r\n", got


# @scenario SC-line-endings-26
def test_a_block_edit_keeps_the_carriage_returns_around_the_block(tmp_path):
    """A selection edit is spliced into the window's text: the lines outside
    the block come back unchanged, carriage returns included."""
    reply = "int b = 22;\n"
    with WpeSession(str(tmp_path), MIXED, env_extra=_env(tmp_path, reply),
                    filename="t.c") as s:
        s.key("\033[B")
        s.key(ALT.BLOCK, "b", delay=0.4)
        s.key("\033[C", delay=0.2)
        s.key(ALT.BLOCK, "e", delay=0.4)
        _start_edit(s, "edit line two")
        assert _wait_screen(s, BOX), "no review box for the block edit"
        s.key("y", delay=0.7)
        s._drain(0.5)
        s.save()
    got = _disk(tmp_path)
    assert got == b"int a = 1;\r\nint b = 22;\nint c = 3;\r\n", got


# @scenario SC-line-endings-27
def test_a_long_line_is_one_line_to_the_model(tmp_path):
    """A line wider than Max. Columns is folded over several rows in the
    window; the model gets it as one line, so a reply that returns it
    unchanged shows no change there and the file keeps it in one piece."""
    long_line = b"x" * 200 + b"\n"
    seed = long_line + b"int b = 2;\n"
    reply = long_line.decode() + "int b = 22;\n"
    with WpeSession(str(tmp_path), seed, env_extra=_env(tmp_path, reply),
                    filename="t.c") as s:
        _start_edit(s)
        assert _wait_screen(s, BOX), "no review box"
        shown = "\n".join(s.display())
        s.key("y", delay=0.7)
        s._drain(0.5)
        s.save()
    assert "Proposed change 1/1" in shown, shown
    assert "- " + "x" * 50 not in shown and "+ " + "x" * 50 not in shown, \
        "the long line is shown as changed:\n" + shown
    assert _disk(tmp_path) == long_line + b"int b = 22;\n"


def _trace(tmp_path):
    p = tmp_path / "e.trace"
    return p.read_text() if p.exists() else ""


def _wait_trace(s, tmp_path, needle, tmo=20):
    end = time.time() + tmo
    while time.time() < end:
        s._drain(0.4)
        if needle in _trace(tmp_path):
            return True
    return False


# @scenario SC-line-endings-31
def test_apply_all_keeps_the_carriage_return_of_a_line_it_left_alone(tmp_path):
    """Multi-file Apply all takes every change of the proposal without a
    review: the box counts one changed line, and only that line changes on
    disk, the carriage returns of the others kept."""
    plan = ("PROPOSE t.c\nint a = 1;\nint b = 22;\nint c = 3;\n@@END\n"
            "@@PLAN-DONE one file\n")
    with WpeSession(str(tmp_path), MIXED, env_extra=_env(tmp_path, plan),
                    filename="t.c") as s:
        s.key(ALT.AI)
        s.key("f")
        s._drain(0.5)
        s.key("bump b")
        s.key("\r", delay=0.8)
        assert _wait_screen(s, "Multi-file edit - 1 file"), "no plan box"
        shown = "\n".join(s.display())
        s.key("a", delay=1.0)
        assert _wait_trace(s, tmp_path, "plan done"), _trace(tmp_path)
        s._drain(0.5)
        s.save()
    assert "t.c  (+1 -1)" in shown, shown
    got = _disk(tmp_path)
    assert got == b"int a = 1;\r\nint b = 22;\nint c = 3;\r\n", got


# @scenario SC-line-endings-32
def test_a_tool_that_changes_only_line_endings_shows_every_line_it_changes(tmp_path):
    """The agent rewrites a CR LF file with LF line endings: the Write box
    shows each line as removed and added, so the user does not allow a
    change the box hides.  The reply rule of the Edit review is not for
    bytes a tool writes to disk."""
    turns = "TOOL write_file t.c\nint a = 1;\nint b = 2;\n@@END@@TURN@@DONE done"
    env = dict(_env(tmp_path, turns), XWPE_AI_POLICY="ask")
    with WpeSession(str(tmp_path), b"int a = 1;\r\nint b = 2;\r\n", env_extra=env,
                    filename="t.c") as s:
        s.key(ALT.AI)
        s.key("g")
        s._drain(0.5)
        s.key("rewrite")
        s.key("\r", delay=0.8)
        assert _wait_screen(s, "Write t.c"), "no write box"
        shown = "\n".join(s.display())
        s.key("n", delay=0.8)
    assert "-int a = 1;" in shown and "+int a = 1;" in shown, shown
    assert "-int b = 2;" in shown and "+int b = 2;" in shown, shown
    assert _disk(tmp_path) == b"int a = 1;\r\nint b = 2;\r\n"
