"""Contracts of the AI review box: line endings, wide rows, placement, viewport.

Three things a reviewer sees or does not see in the "Proposed change" box:

- a reply with CRLF line endings is normalised before the diff, so only the
  real change is shown and no carriage return reaches the buffer or the disk;
- a row wider than the box ends in a clip marker, so a change beyond the box's
  edge is never shown as two identical-looking rows;
- the box opens inside the file window, next to the hunk's first line, which
  is scrolled into view first (the cursor is parked on it, the way Search and
  Goto Line bring a line into view), never on the window's status row or over
  the assistant pane; after the hunk is accepted it is still on screen.

They run against the deterministic mock backend at the driver's 80x30 size,
where the file window body is 13 rows tall and the assistant pane sits below
the window's status row.
"""
import os
import re
import time
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

BOX = "Proposed change"
HINT = "y apply"
CLIP_MARK = ">"


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


def _wait_trace(s, tmp_path, needle, tmo=12):
    end = time.time() + tmo
    while time.time() < end:
        if needle in _trace(tmp_path):
            return True
        s._drain(0.3)
    return False


def _start_edit(s, instr="do it"):
    s.key(ALT.AI)
    s.key("e")
    s._drain(0.4)
    s.key(instr)
    s.key("\r", delay=0.8)


def _row_with(disp, needle):
    """Index of the first screen row containing needle, or -1."""
    return next((i for i, row in enumerate(disp) if needle in row), -1)


def _window_top_row(disp):
    """The file window's top border: the row that names the file."""
    return _row_with(disp, " t.c ")


def _status_row(disp):
    """The file window's status row: its bottom border carrying line:column."""
    for i, row in enumerate(disp):
        if row.startswith("m") and re.search(r"\s\d+:\d+\s", row):
            return i
    return -1


def _cursor_pos(disp):
    """(line, column) as the status row shows them."""
    i = _status_row(disp)
    assert i >= 0, "no status row on screen:\n" + "\n".join(disp)
    m = re.search(r"\s(\d+):(\d+)\s", disp[i])
    return int(m.group(1)), int(m.group(2))


def _gutter_rows(disp, text):
    """The box rows that show text behind a - or + gutter."""
    return [row for row in disp if re.search(r"[-+]" + re.escape(text), row)]


# @scenario SC-ai-edit-36
def test_crlf_reply_shows_only_the_real_change_and_saves_lf(tmp_path):
    seed = "int a = 1;\nint b = 2;\nint c = 3;\n"
    reply = "int a = 1;\r\nint b = 22;\r\nint c = 3;\r\n"
    with WpeSession(str(tmp_path), seed, env_extra=_env(tmp_path, reply), filename="t.c") as s:
        _start_edit(s)
        assert _wait_screen(s, "Proposed change 1/1"), "no review box"
        disp = s.display()
        shown = "\n".join(disp)
        assert _gutter_rows(disp, "int b = 22;"), "the changed line is not in the box:\n" + shown
        assert not _gutter_rows(disp, "int a = 1;"), \
            "an untouched line is shown as a change (the reply's CR survived):\n" + shown
        assert not _gutter_rows(disp, "int c = 3;"), \
            "an untouched line is shown as a change (the reply's CR survived):\n" + shown
        s.key("y", delay=0.7)
        assert _wait_trace(s, tmp_path, "edit applied"), "the hunk was not applied"
        s.save()
        assert s.alive(), "wpe died"
    with open(tmp_path / "t.c", "rb") as fh:
        data = fh.read()
    assert b"\r" not in data, "a carriage return reached the disk:\n" + repr(data)
    assert data == b"int a = 1;\nint b = 22;\nint c = 3;\n", repr(data)


# @scenario SC-ai-edit-16b
def test_row_wider_than_the_box_carries_a_clip_marker(tmp_path):
    long_line = "int x = 0; /*" + " " * 90 + "*/ // tail A"   # 107 columns
    seed = "int y = 1;\n" + long_line + "\n"
    reply = "int y = 1;\n" + long_line.replace("tail A", "tail B") + "\n"
    with WpeSession(str(tmp_path), seed, env_extra=_env(tmp_path, reply), filename="t.c") as s:
        _start_edit(s)
        assert _wait_screen(s, BOX), "no review box"
        disp = s.display()
        shown = "\n".join(disp)
        changed = _gutter_rows(disp, "int x = 0;")
        assert len(changed) == 2, "expected a - row and a + row for the long line:\n" + shown
        minus, plus = changed
        assert minus != plus, "the - and + rows read identical:\n" + shown
        assert CLIP_MARK in minus and CLIP_MARK in plus, \
            "a row wider than the box has no clip marker:\n" + shown
        assert minus.index(CLIP_MARK) == plus.index(CLIP_MARK), \
            "the clip marker is not at one clip column:\n" + shown
        assert minus.index(CLIP_MARK) > minus.index("int x = 0;") + len("int x = 0;"), \
            "the clip marker is inside the text instead of at the clip column:\n" + shown
        context = [row for row in disp if re.search(r"\d\s+int y = 1;", row)]
        assert context, "the short context row is missing:\n" + shown
        assert CLIP_MARK not in context[0], \
            "a row that fits the box carries the clip marker:\n" + shown
        s.key("q", delay=0.6)
        assert s.alive(), "wpe died"


# @scenario SC-ai-edit-18
# @scenario SC-ai-edit-28
def test_offscreen_hunk_is_scrolled_into_view_and_stays_after_accept(tmp_path):
    seed = "".join("int v%02d = %d;\n" % (i, i) for i in range(60))
    reply = seed.replace("int v50 = 50;", "int v50 = 999;")
    with WpeSession(str(tmp_path), seed, env_extra=_env(tmp_path, reply), filename="t.c") as s:
        _start_edit(s)
        assert _wait_screen(s, BOX), "no review box"
        disp = s.display()
        shown = "\n".join(disp)
        top = _window_top_row(disp)
        status = _status_row(disp)
        title = _row_with(disp, BOX)
        hint = _row_with(disp, HINT)
        assert top >= 0 and status > top, "file window borders not found:\n" + shown
        assert hint > title > top, "the box is not drawn top to bottom inside the window:\n" + shown
        # The window row the box sits next to (just above its title, or just
        # below its hint when it had to flip) must show the hunk's first line;
        # the box's own "-int v50 = 50;" row does not count.
        beside = [disp[title - 1], disp[hint + 1] if hint + 1 < len(disp) else ""]
        assert any(re.search(r"^x\s*int v50 = 50;", row) for row in beside), \
            "the hunk's line was not scrolled into view beside the box:\n" + shown
        assert hint < status, \
            "the box reaches the window's status row or the pane below it:\n" + shown
        assert _cursor_pos(disp) == (51, 1), \
            "the cursor is not parked on the hunk's first line while reviewing:\n" + shown
        s.key("y", delay=0.7)
        assert _wait_trace(s, tmp_path, "edit applied"), "the hunk was not applied"
        disp = s.display()
        shown = "\n".join(disp)
        assert _row_with(disp, "int v50 = 999;") >= 0, \
            "the accepted hunk is not on screen after accept:\n" + shown
        assert _cursor_pos(disp) == (51, 1), \
            "the cursor did not keep its row and column after accept:\n" + shown
        assert s.alive(), "wpe died"
