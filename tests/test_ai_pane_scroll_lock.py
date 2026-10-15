"""AI pane -- the browsed view holds while the transcript grows (deterministic mock).

PgUp/PgDn, the arrows and the mouse wheel browse the transcript; from then on
the view is locked where the user left it until a key snaps it back to the
input row.  The lock must survive the pane losing focus: a user who paged two
pages up and then clicked into the file keeps reading the same lines while the
reply lands below them.  The wheel goes through the same scroller as the keys,
three lines per step, so it locks the view the same way.

Red/green: before the fix the pane honoured the lock only while it was the
focused window -- the heartbeat and the landing reply re-pinned an unfocused
pane to the bottom -- and the terminal never delivered the wheel as a scroll
key at all (it reached the editor as a buttonless mouse event and merely moved
the caret to the pointer), so the wheel did nothing over the pane.
"""
import os
import subprocess
import time

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN, title_row, pane_rows

SEED = "int main(void){return 0;}\n"
PGUP = "\033[5~"
GATHERING = "gathering the answer"
LONG = "\n".join("line%02d" % i for i in range(30))

# The mock's reply delay is fixed wall-clock time, so it is scaled by hand to
# stay "during the turn" under the slower sanitizer binary; the driver scales
# its own drains itself.
WAIT_SCALE = float(os.environ.get("XWPE_TEST_WAIT_SCALE", "1") or 1)
DELAY_MS = 2500
LANDED_S = DELAY_MS / 1000.0 + 1.5          # the reply has landed by then (driver-scaled)
REPLY_POLL_S = DELAY_MS / 1000.0 + 5
TURN_POLL_S = 2
WHEEL_LINES = 3

# SGR (1006) mouse reports at 1-based screen cells: the wheel over the pane,
# and a click (press + release) on the file window.
WHEEL_UP_OVER_PANE = "\033[<64;20;22M"
PANE_LINE_PREFIX = "line"


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _scaled_ms(ms):
    return str(int(ms * WAIT_SCALE))


def _env(tmp_path, reply, **more):
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_POLICY": "ask",
        "XWPE_AI_TRACE": str(tmp_path / "ai.trace"),
    }
    env.update(more)
    return env


def _trace(tmp_path):
    t = tmp_path / "ai.trace"
    return t.read_text() if t.exists() else ""


def _dump(s, drain=0.2):
    rows = s.display(drain=drain)
    return "\n".join("%2d|%s|" % (i, r) for i, r in enumerate(rows))


def _seen_within(s, needle, timeout):
    end = time.time() + timeout * WAIT_SCALE
    while time.time() < end:
        s._drain(0.25)
        if needle in "\n".join(s.display(drain=0)):
            return True
    return False


def _top_line(s):
    """The first transcript row of the AI pane, as the user sees it now;
    None while the pane is not open."""
    disp = s.display(drain=0)
    if title_row(disp, "AI") < 0:
        return None
    return pane_rows(disp)[0].rstrip()


def _line_number(top):
    """The number of a 'lineNN' pane row, None for anything else."""
    if top and top.startswith(PANE_LINE_PREFIX):
        return int(top[len(PANE_LINE_PREFIX):len(PANE_LINE_PREFIX) + 2])
    return None


def _click(s, col, row):
    s.key("\033[<0;%d;%dM" % (col, row), delay=0.2)
    s.key("\033[<0;%d;%dm" % (col, row), delay=0.3)


def _arm(s):
    s.key(ALT.AI)
    s.key("a", delay=0.6)


def _ask(s, question, delay=0.3):
    s.key(question)
    s.key("\r", delay=delay)


def _start_late_turn(s, tmp_path):
    """Arm the chat, land one long reply, then start a turn whose reply is
    late enough to browse while it is still being gathered."""
    _arm(s)
    _ask(s, "one")
    assert _seen_within(s, "line29", REPLY_POLL_S), _dump(s)
    _ask(s, "two")
    assert _seen_within(s, GATHERING, TURN_POLL_S), _dump(s)


# @scenario SC-ai-chat-32
def test_reply_arriving_while_browsing_keeps_the_view_focused(tmp_path):
    env = _env(tmp_path, LONG, XWPE_AI_MOCK_DELAY_MS=_scaled_ms(DELAY_MS))
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _start_late_turn(s, tmp_path)
        s.key(PGUP, delay=0.2)
        s.key(PGUP, delay=0.2)
        top_before = _top_line(s)
        s._drain(LANDED_S)
        assert "chat done" in _trace(tmp_path), _trace(tmp_path)
        top_after = _top_line(s)
        s.key("k", delay=0.4)
        snapped = s.display()
    assert _line_number(top_before) is not None, "PgUp did not browse: %r" % top_before
    assert top_after == top_before, \
        "the view moved when the reply landed: %r -> %r" % (top_before, top_after)
    assert any("> k" in r for r in snapped) and any("line29" in r for r in snapped), \
        "typing did not snap back to the input:\n" + "\n".join(snapped)


# @scenario SC-ai-chat-32
def test_reply_arriving_while_browsing_keeps_the_view_unfocused(tmp_path):
    env = _env(tmp_path, LONG, XWPE_AI_MOCK_DELAY_MS=_scaled_ms(DELAY_MS))
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _start_late_turn(s, tmp_path)
        s.key(PGUP, delay=0.2)
        s.key(PGUP, delay=0.2)
        top_before = _top_line(s)
        _click(s, 10, 5)                                # focus the file window
        focused_file = "1q" in s.display(drain=0)[1]
        top_clicked = _top_line(s)
        s._drain(LANDED_S)
        assert "chat done" in _trace(tmp_path), _trace(tmp_path)
        top_after = _top_line(s)
        after = _dump(s, drain=0)
    assert _line_number(top_before) is not None, "PgUp did not browse: %r" % top_before
    assert focused_file, "the click did not focus the file window"
    assert top_clicked == top_before, \
        "focusing the file re-pinned the pane: %r -> %r" % (top_before, top_clicked)
    assert top_after == top_before, \
        "the unfocused pane re-pinned when the reply landed: %r -> %r\n%s" \
        % (top_before, top_after, after)


# @scenario SC-ai-chat-33
def test_wheel_scrolls_three_lines_and_locks(tmp_path):
    env = _env(tmp_path, LONG, XWPE_AI_MOCK_DELAY_MS=_scaled_ms(DELAY_MS))
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _arm(s)
        _ask(s, "one")
        assert _seen_within(s, "line29", REPLY_POLL_S), _dump(s)
        top0 = _top_line(s)
        s.key(WHEEL_UP_OVER_PANE, delay=0.4)
        top1 = _top_line(s)
        _ask(s, "two")                                  # typing snaps back; a turn starts
        assert _seen_within(s, GATHERING, TURN_POLL_S), _dump(s)
        s.key(WHEEL_UP_OVER_PANE, delay=0.4)
        s.key(WHEEL_UP_OVER_PANE, delay=0.4)
        top2 = _top_line(s)
        s._drain(LANDED_S)
        assert "chat done" in _trace(tmp_path), _trace(tmp_path)
        top3 = _top_line(s)
        after = _dump(s, drain=0)
    assert top1 != top0, "the wheel did not scroll the pane: %r" % top0
    n0 = _line_number(top0)
    n1 = _line_number(top1)
    assert n0 is not None and n1 == n0 - WHEEL_LINES, \
        "the wheel should move three lines: %r -> %r" % (top0, top1)
    assert top3 == top2, \
        "the wheel's view did not lock: the paint re-pinned %r -> %r\n%s" % (top2, top3, after)
