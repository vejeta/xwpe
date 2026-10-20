"""Contract tests for the AI multi-file approval box (Alt-G f)."""
import os
import time
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN, pane_rows

ORIG = "int a = 1;\nint b = 2;\nint c = 3;\n"
NEW = "int a = 1;\nint b = 22;\nint c = 3;\n"
# The Multi-file edit has one name everywhere: the pane lines carry it as a
# prefix and the approval box carries it as its title.
PREFIX = "[Multi-file edit]"
BOX_TITLE = "Multi-file edit -"


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"PROPOSE" in out
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


def _wait_screen(s, needle, tmo=12):
    end = time.time() + tmo
    while time.time() < end:
        s._drain(0.4)
        if needle in "\n".join(s.display()):
            return True
    return False


def _screen_words(s):
    """The pane's text as one line of single-spaced words.

    After a Multi-file accept the Messages window docks beside the pane and
    halves its width, so a pane line wraps at a word boundary; a phrase is
    then read across the wrapped rows, inside the pane's frame, not on one
    screen row.
    """
    return " ".join(" ".join(pane_rows(s.display())).split())


def _wait_words(s, needle, tmo=12):
    end = time.time() + tmo
    while time.time() < end:
        s._drain(0.4)
        if needle in _screen_words(s):
            return True
    return False


def _start_plan(s, task):
    s.key(ALT.AI)
    s.key("f")
    s._drain(0.5)
    s.key(task)
    s.key("\r", delay=0.8)


def _click_text(s, text):
    disp = s.display()
    for y, row in enumerate(disp):
        x = row.find(text)
        if x >= 0:
            cx = x + 2
            cy = y + 1
            s.key("\033[<0;%d;%dM" % (cx, cy), delay=0.2)
            s.key("\033[<0;%d;%dm" % (cx, cy), delay=0.4)
            return True
    return False


# @scenario SC-ai-edit-42
def test_plan_reports_task_scope_and_no_proposals(tmp_path):
    with WpeSession(str(tmp_path), ORIG,
                    env_extra=_env(tmp_path, "I would change t.c like this")) as s:
        _start_plan(s, "explain why this file is fine")
        assert _wait_screen(s, PREFIX + " task:"), "task+scope line not shown"
        assert _wait_screen(s, PREFIX + " the model proposed no file changes"), \
            "missing 'no proposals' message"
        disp = "\n".join(s.display())
        assert "scope:" in disp and "files" in disp, "scope count not visible:\n" + disp
        s.save()
        assert s.text() == ORIG, "a prose plan reply changed the file"


# @scenario SC-ai-edit-43
def test_plan_approval_lists_counts_and_cancel_keys_leave_everything_unchanged(tmp_path):
    (tmp_path / "other.c").write_text("int other = 1;\n")
    plan = (
        "PROPOSE t.c\n" + NEW + "@@END\n"
        "PROPOSE other.c\nint other = 2;\n@@END\n"
        "@@PLAN-DONE two files\n"
    )
    for i, cancel in enumerate(("q", "\033")):
        case = tmp_path / ("case%d" % i)
        case.mkdir()
        (case / "other.c").write_text("int other = 1;\n")
        with WpeSession(str(case), ORIG, env_extra=_env(case, plan), filename="t.c") as s:
            _start_plan(s, "bump both files")
            assert _wait_screen(s, BOX_TITLE), "approval box did not open"
            disp = "\n".join(s.display())
            assert "t.c" in disp and "other.c" in disp, "approval list is incomplete:\n" + disp
            assert "(+1 -1)" in disp, "per-file +/- counts are missing:\n" + disp
            s.key(cancel, delay=0.8)
            s._drain(0.6)
            after = "\n".join(s.display())
            assert BOX_TITLE not in after, "approval box stayed open after cancel"
            assert PREFIX + " cancelled" in after, "cancel feedback missing"
            s.save()
            assert s.text() == ORIG, "t.c changed after cancelling approval"
        assert (case / "other.c").read_text() == "int other = 1;\n", \
            "other.c changed after cancelling approval"


# @scenario SC-ai-edit-44
def test_plan_apply_all_opens_files_without_writing_disk_and_refuses_directories(tmp_path):
    (tmp_path / "other.c").write_text("int other = 1;\n")
    (tmp_path / "sub").mkdir()
    plan = (
        "PROPOSE t.c\n" + NEW + "@@END\n"
        "PROPOSE other.c\nint other = 2;\n@@END\n"
        "PROPOSE sub\nint x;\n@@END\n"
        "@@PLAN-DONE mixed targets\n"
    )
    with WpeSession(str(tmp_path), ORIG, env_extra=_env(tmp_path, plan), filename="t.c") as s:
        _start_plan(s, "apply all file updates")
        assert _wait_screen(s, BOX_TITLE), "approval box did not open"
        s.key("a", delay=0.9)
        assert _wait_words(s, PREFIX + " applied 2 of 3 files"), \
            "applied/total count did not report the directory as not-applied"
        assert "could not open sub" in _screen_words(s), "directory path refusal message missing"
        disp = "\n".join(s.display())
        assert " other.c " in disp, "a new window for unopened other.c was not shown"

        with open(s.path, "rb") as fh:
            assert fh.read() == ORIG.encode("utf-8"), "t.c was written to disk before Save"
        with open(tmp_path / "other.c", "rb") as fh:
            assert fh.read() == b"int other = 1;\n", "other.c was written to disk before Save"

        s.key("\x15", delay=0.8)   # undo active proposal window
        s._drain(0.5)
        if _click_text(s, " t.c "):
            s.key("\x15", delay=0.8)   # undo t.c window too
            s._drain(0.5)
        assert os.path.isdir(tmp_path / "sub"), "directory proposal touched the directory path"


# @scenario SC-ai-edit-44b
def test_plan_review_file_by_file_uses_the_same_hunk_review_box(tmp_path):
    plan = "PROPOSE t.c\n" + NEW + "@@END\n@@PLAN-DONE one file\n"
    with WpeSession(str(tmp_path), ORIG, env_extra=_env(tmp_path, plan), filename="t.c") as s:
        _start_plan(s, "review one file")
        assert _wait_screen(s, BOX_TITLE), "approval box did not open"
        s.key("f", delay=0.8)
        assert _wait_screen(s, "Proposed change"), "file-by-file review did not open hunk box"
        disp = "\n".join(s.display())
        assert "y apply  n skip  a all  q cancel" in disp, \
            "review hint differs from ordinary edit hunk review:\n" + disp
        s.key("q", delay=0.8)
        s._drain(0.5)
        s.save()
        assert s.text() == ORIG, "cancelling file-by-file review changed disk"


def _trace(tmp_path):
    p = tmp_path / "e.trace"
    return p.read_text() if p.exists() else ""


def _wait_trace(s, tmp_path, needle, tmo=12):
    end = time.time() + tmo
    while time.time() < end:
        s._drain(0.4)
        if needle in _trace(tmp_path):
            return True
    return False


# @scenario SC-ai-edit-43
def test_plan_counts_follow_the_unsaved_buffer_and_apply_all_replaces_it(tmp_path):
    """The (+a -b) of the box is counted against the buffer as it is now.

    An unsaved "ZZZ" typed into t.c before the plan turns line 1 into
    "ZZZint a = 1;", so the proposal (NEW) now changes two lines, not one: the
    box must say (+2 -2), and Apply all must replace that live buffer as one
    undo step, so one Ctrl-U brings the ZZZ back.  Before the fix the counts
    were taken from the file on disk (+1 -1) and the unsaved edit was lost
    without ever having been counted or shown.
    """
    plan = "PROPOSE t.c\n" + NEW + "@@END\n@@PLAN-DONE bumped b\n"
    with WpeSession(str(tmp_path), ORIG, env_extra=_env(tmp_path, plan), filename="t.c") as s:
        s.key("ZZZ", delay=0.3)
        assert _wait_screen(s, "ZZZint a = 1;", tmo=4), "the unsaved edit did not reach the buffer"
        _start_plan(s, "bump b")
        assert _wait_screen(s, BOX_TITLE), "approval box did not open"
        box = "\n".join(s.display())
        assert "(+2 -2)" in box, "counts ignore the unsaved buffer:\n" + box
        assert "(+1 -1)" not in box, "counts were taken from the disk, not the buffer:\n" + box
        s.key("a", delay=0.9)
        assert _wait_trace(s, tmp_path, "plan done applied=1"), "apply all did not finish:\n" + _trace(tmp_path)
        after = "\n".join(s.display())
        assert "ZZZ" not in after, "apply all did not replace the live buffer:\n" + after
        assert "int b = 22;" in after, "the proposal did not reach the buffer:\n" + after
        with open(s.path, "rb") as fh:
            assert fh.read() == ORIG.encode("utf-8"), "t.c was written to disk before Save"
        s.key("\x15", delay=0.8)   # Ctrl-U: one step back to the unsaved buffer
        assert _wait_screen(s, "ZZZint a = 1;", tmo=4), \
            "one Ctrl-U did not bring the unsaved edit back:\n" + "\n".join(s.display())


# @scenario SC-ai-edit-45
def test_plan_path_outside_the_workspace_is_marked_in_the_box_and_named_on_apply(tmp_path):
    """A proposal for a file outside the workspace is flagged, not refused.

    The box row carries a visible mark and the words "outside the workspace",
    and when the user applies it anyway the pane names the file and says it
    went outside.  Before the fix "../outside.c" was listed and applied like
    any other file, with no warning anywhere.
    """
    proj = tmp_path / "proj"
    proj.mkdir()
    plan = "PROPOSE ../outside.c\nint outside;\n@@END\n@@PLAN-DONE add a file\n"
    with WpeSession(str(proj), ORIG, env_extra=_env(tmp_path, plan), filename="t.c") as s:
        _start_plan(s, "add outside.c")
        assert _wait_screen(s, BOX_TITLE), "approval box did not open"
        box = "\n".join(s.display())
        row = [r for r in s.display() if "../outside.c" in r and "(+" in r]
        assert row, "the proposal row is missing:\n" + box
        assert "outside the workspace" in row[0], "the row does not say it is outside:\n" + row[0]
        assert "! ../outside.c" in row[0], "the row carries no visible mark before the path:\n" + row[0]
        assert "[new file]" in row[0], "a file that does not exist is not marked new:\n" + row[0]
        s.key("a", delay=0.9)
        assert _wait_trace(s, tmp_path, "plan done applied=1"), "apply all did not finish:\n" + _trace(tmp_path)
        pane = _screen_words(s)
        assert "outside.c" in pane and "outside the workspace" in pane, \
            "the pane did not name the file that went outside:\n" + pane
        assert not os.path.exists(tmp_path / "outside.c"), "the new file was written before Save"
        with open(s.path, "rb") as fh:
            assert fh.read() == ORIG.encode("utf-8"), "t.c changed for a proposal that named another file"
