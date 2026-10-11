"""Contract tests for AI edit review behavior and edit outcomes.

These scenarios pin user-visible behavior from the review box and from the
buffer/disk boundary. They run against the deterministic mock backend.
"""
import os
import time
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

ORIG = "int a = 1;\nint b = 2;\nint c = 3;\n"
NEW = "int a = 1;\nint b = 22;\nint c = 3;\n"
BOX = "Proposed change"


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
        "XWPE_AI_TRACE": str(tmp_path / "e.trace"),
    }
    env.update(extra)
    return env


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


def _start_edit(s, instr="do it"):
    s.key(ALT.AI)
    s.key("e")
    s._drain(0.4)
    s.key(instr)
    s.key("\r", delay=0.8)


def _run_edit_once(tmp_path, reply, decision):
    tmp_path.mkdir(parents=True, exist_ok=True)   # callers pass a fresh subdirectory
    with WpeSession(str(tmp_path), ORIG, env_extra=_env(tmp_path, reply), filename="t.c") as s:
        _start_edit(s)
        assert _wait_screen(s, BOX), "no review box"
        s.key(decision, delay=0.7)
        for _ in range(20):            # the review ends when it says what it did,
            tr = _trace(tmp_path)      # which on a loaded machine is not immediate
            if "edit applied" in tr or "edit discarded" in tr:
                break
            s._drain(0.3)
        s.save()
        out = s.text()
        assert s.alive(), "wpe died"
    return out, _trace(tmp_path)


# @scenario SC-ai-edit-13
def test_identical_or_newline_only_reply_reports_no_change(tmp_path):
    seed = "int a = 1;\nint b = 2;\n"
    replies = [seed, seed.rstrip("\n")]
    for i, reply in enumerate(replies):
        case = tmp_path / ("case%d" % i)
        case.mkdir()
        with WpeSession(str(case), seed, env_extra=_env(case, reply), filename="t.c") as s:
            _start_edit(s)
            end = time.time() + 10
            saw_box = False
            while time.time() < end:
                s._drain(0.4)
                if BOX in "\n".join(s.display()):
                    saw_box = True
                    break
                if "edit no-change" in _trace(case):
                    break
            assert not saw_box, "a no-op reply opened a review box"
            disp = "\n".join(s.display())
            assert "no change" in disp.lower(), "pane did not report no change:\n" + disp
            s.save()
            assert s.text() == seed, "disk changed for a no-op reply"


# @scenario SC-ai-edit-19
def test_enter_applies_the_same_change_as_y(tmp_path):
    enter_out, _ = _run_edit_once(tmp_path / "enter", NEW, "\r")
    y_out, _ = _run_edit_once(tmp_path / "y", NEW, "y")
    assert enter_out == y_out == NEW, "Enter and y must apply the same hunk result"


# @scenario SC-ai-edit-19b
def test_review_box_ignores_unrelated_keys_altg_and_mouse(tmp_path):
    with WpeSession(str(tmp_path), ORIG, env_extra=_env(tmp_path, NEW), filename="t.c") as s:
        _start_edit(s)
        assert _wait_screen(s, BOX), "no review box"
        for k in ("x", "1", ALT.AI):
            s.key(k, delay=0.5)
            assert BOX in "\n".join(s.display()), "box closed after ignored key %r" % (k,)
        s.key("\033[<0;20;10M", delay=0.2)
        s.key("\033[<0;20;10m", delay=0.5)
        assert BOX in "\n".join(s.display()), "box closed after mouse click"
        s.key("q", delay=0.7)
        s._drain(0.5)
        s.save()
        assert s.text() == ORIG, "ignored keys must not change the file"


# @scenario SC-ai-edit-22
def test_pgdn_pgup_scroll_inside_tall_hunk_and_return(tmp_path):
    seed = "".join("int v%02d = %d;\n" % (i, i) for i in range(5))
    reply = seed + "".join("int w%02d = %d;\n" % (i, i) for i in range(40))
    with WpeSession(str(tmp_path), seed, env_extra=_env(tmp_path, reply), filename="t.c") as s:
        _start_edit(s)
        assert _wait_screen(s, BOX), "no review box"
        top = "\n".join(s.display())
        s.key("\033[6~", delay=0.8)  # PgDn
        after_pgdn = "\n".join(s.display())
        assert top != after_pgdn, "PgDn did not scroll the hunk"
        s.key("\033[5~", delay=0.8)  # PgUp
        assert "\n".join(s.display()) == top, "PgUp did not return to the hunk start"
        s.key("q", delay=0.6)


# @scenario SC-ai-edit-25
def test_skip_all_hunks_leaves_window_unmodified_and_quit_needs_no_prompt(tmp_path):
    seed = "int a = 1;\nint b = 2;\nint c = 3;\nint d = 4;\nint e = 5;\n"
    reply = "int a = 11;\nint b = 2;\nint c = 3;\nint d = 4;\nint e = 55;\n"
    with WpeSession(str(tmp_path), seed, env_extra=_env(tmp_path, reply), filename="t.c") as s:
        _start_edit(s)
        assert _wait_screen(s, "Proposed change 1/2"), "first hunk missing"
        s.key("n", delay=0.7)
        assert _wait_screen(s, "Proposed change 2/2"), "second hunk missing"
        s.key("n", delay=0.7)
        s._drain(0.5)
        s.key("\033x", delay=0.8)  # Alt-X: quit
        end = time.time() + 10     # tearing down the pane as well takes a while,
        while time.time() < end and s.alive():   # so wait for the exit, do not
            s._drain(0.5)                        # assume one drain is enough
        assert not s.alive(), "window stayed modified (quit asked again)"
    with open(tmp_path / "t.c") as fh:
        assert fh.read() == seed, "disk changed after skipping every hunk"


# @scenario SC-ai-edit-26
def test_accept_reaches_buffer_but_disk_stays_identical_until_save(tmp_path):
    with WpeSession(str(tmp_path), ORIG, env_extra=_env(tmp_path, NEW), filename="t.c") as s:
        _start_edit(s)
        assert _wait_screen(s, BOX), "no review box"
        s.key("y", delay=0.7)
        s._drain(0.5)
        disp = "\n".join(s.display())
        assert "int b = 22;" in disp, "accepted hunk did not reach the buffer"
        with open(s.path, "rb") as fh:
            assert fh.read() == ORIG.encode("utf-8"), "disk changed before Save"
        s.save()
        assert s.text() == NEW, "save did not persist the accepted edit"


# @scenario SC-ai-edit-29
def test_reply_newline_variants_save_with_exactly_one_final_newline(tmp_path):
    replies = [NEW.rstrip("\n"), NEW + "\n\n\n"]
    for i, reply in enumerate(replies):
        case = tmp_path / ("case%d" % i)
        case.mkdir()
        with WpeSession(str(case), ORIG, env_extra=_env(case, reply), filename="t.c") as s:
            _start_edit(s)
            assert _wait_screen(s, BOX), "no review box"
            s.key("a", delay=0.7)
            s._drain(0.5)
            s.save()
            out = s.text()
        assert out.endswith("\n"), "saved text has no final newline"
        assert not out.endswith("\n\n"), "saved text kept multiple trailing blank lines"
        assert out == NEW, "saved text did not normalize to one final newline:\n" + repr(out)


# @scenario SC-ai-edit-30
def test_block_last_line_without_final_newline_replaces_cleanly(tmp_path):
    seed = "int a = 1;\nint b = 2;\nint c = 3"
    region_reply = "int c = 33;\n"
    with WpeSession(str(tmp_path), seed, env_extra=_env(tmp_path, region_reply), filename="t.c") as s:
        s.key("\033[B")
        s.key("\033[B")
        s.key(ALT.BLOCK, "b", delay=0.4)
        s.key("\033[C", delay=0.2)
        s.key(ALT.BLOCK, "e", delay=0.4)
        _start_edit(s, "edit last line")
        assert _wait_screen(s, BOX), "no review box for last-line block edit"
        s.key("y", delay=0.7)
        s._drain(0.5)
        s.save()
        out = s.text()
    assert out == "int a = 1;\nint b = 2;\nint c = 33;\n", \
        "last line replacement lost or duplicated content:\n" + repr(out)


# @scenario SC-ai-edit-36
def test_crlf_file_with_lf_reply_shows_real_change_and_saves_crlf(tmp_path):
    seed = "int a = 1;\r\nint b = 2;\r\nint c = 3;\r\n"
    reply = "int a = 1;\nint b = 22;\nint c = 3;\n"
    with WpeSession(str(tmp_path), seed, env_extra=_env(tmp_path, reply), filename="t.c") as s:
        _start_edit(s)
        assert _wait_screen(s, "Proposed change 1/1"), "expected one real hunk"
        disp = "\n".join(s.display())
        assert "int b = 22;" in disp, "changed line is not in the review box"
        assert "- int a = 1;" not in disp and "+ int a = 1;" not in disp, \
            "unchanged line a appears as a diff change"
        assert "- int c = 3;" not in disp and "+ int c = 3;" not in disp, \
            "unchanged line c appears as a diff change"
        s.key("y", delay=0.7)
        s._drain(0.5)
        s.save()
    with open(tmp_path / "t.c", "rb") as fh:
        assert fh.read() == b"int a = 1;\r\nint b = 22;\r\nint c = 3;\r\n", \
            "CRLF file was not saved back with CRLF line endings"
