"""The AI pane re-wraps its transcript to whatever width it has.

The pane's lines were word-wrapped once, at the width of the moment they were
written.  When Messages opened beside the pane (a build with F9, a changeset
review) the pane went from the full width to half of it, and every earlier
line stayed cut at the new border with its right half gone; a terminal resize
did the same.  Now each buffer row remembers whether it continues the row
above, so the pane joins every wrapped line back together and wraps it again
at its present width before it is painted: every word stays readable over
more rows in a narrow pane, and a widened pane uses its room.  Because the
rows of one line are recorded as one line, a reply that wraps over two rows
and is marked and copied reaches the clipboard as the one line it is.

Red/green: on the unfixed editor the first test finds the reply's rows cut at
the half-width border (the words past column 37 of each row are gone).

Scenarios: SC-ai-chat-04, SC-ai-chat-06, SC-ai-chat-35.
"""
import os
import shutil
import subprocess
import time

import pytest
from wpe_driver import (WpeSession, ALT, WPE_BIN, title_row, pane_frame, pane_rows,
                        pane_rows_with_history)

KEY_F9 = "\033[20~"                 # Make (compile + link): opens Messages
SEED = "int main(void){return 0;}\n"
COLS_NORMAL = 80
COLS_WIDE = 120
COLS_NARROW = 40
ROWS = 30
# 30 words, 119 characters: two rows at the full width, four at the half.
LONG_REPLY = " ".join("w%02d" % i for i in range(30))
LONG_WORDS = LONG_REPLY.split()
# 19 words: "AI: " + 75 characters = 79, one row at the 80-column measure of
# a wide terminal, two rows at the 77 columns of an 80-column terminal.
RESIZE_REPLY = " ".join("w%02d" % i for i in range(19))
RESIZE_WORDS = RESIZE_REPLY.split()


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
            "XWPE_AI_MOCK_REPLY": reply, "XWPE_AI_TRACE": str(tmp_path / "ai.trace")}


def _seen_within(s, needle, timeout):
    end = time.time() + timeout
    while time.time() < end:
        s._drain(0.25)
        if needle in "\n".join(s.display(drain=0)):
            return True
    return False


def _pane_width(disp):
    """The columns a pane row may use: the interior minus the border columns."""
    _, x0, x1 = pane_frame(disp)
    return x1 - x0 - 1


def _rows_with_words(rows, words):
    return [r for r in rows if any(w in r.split() for w in words)]


def _missing(rows, words):
    present = set()
    for r in rows:
        present.update(r.split())
    return [w for w in words if w not in present]


def _ask(s, question, last_word):
    s.key(ALT.AI); s.key("a", delay=0.8)
    s.key(question); s.key("\r", delay=1.0)
    assert _seen_within(s, last_word, 8), \
        "the reply did not land in the pane:\n" + "\n".join(s.display(drain=0))


def _assert_whole_at_width(rows, words, width, what):
    assert not _missing(rows, words), \
        "%s: words cut away: %r\n%s" % (what, _missing(rows, words), "\n".join(rows))
    too_wide = [r for r in rows if len(r) > width]
    assert not too_wide, "%s: rows wider than the pane (%d):\n%s" \
        % (what, width, "\n".join(too_wide))


# @scenario SC-ai-chat-04
def test_reply_written_at_full_width_is_whole_after_messages_opens(tmp_path):
    if not shutil.which("gcc"):
        pytest.skip("gcc not installed")
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path, LONG_REPLY),
                    filename="t.c") as s:
        _ask(s, "q", "w29")
        before = pane_rows(s.display())
        rows_before = len(_rows_with_words(before, LONG_WORDS))
        s.key(KEY_F9, delay=3.0)
        s._drain(1.0)
        disp = s.display()
        msg_y = title_row(disp, "Messages")
        ai_y = title_row(disp, "AI")
        width = _pane_width(disp)
        after = pane_rows_with_history(s, LONG_WORDS)
    assert msg_y >= 0 and msg_y == ai_y, \
        "Messages did not open beside the pane:\n" + "\n".join(disp)
    assert width < COLS_NORMAL // 2, "the pane did not narrow: %d columns" % width
    _assert_whole_at_width(after, LONG_WORDS, width, "after F9")
    rows_after = len(_rows_with_words(after, LONG_WORDS))
    assert rows_after > rows_before, \
        "the reply should take more rows in the narrow pane (%d before, %d after):\n%s" \
        % (rows_before, rows_after, "\n".join(after))


# @scenario SC-ai-chat-06
def test_resize_rewraps_the_reply_and_the_row_count_follows_the_width(tmp_path):
    steps = [(COLS_WIDE, 1), (COLS_NORMAL, 2), (COLS_NARROW, 3), (COLS_NORMAL, 2)]
    found = []
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path, RESIZE_REPLY),
                    filename="t.c") as s:
        _ask(s, "q", "w18")
        rows = pane_rows(s.display())
        assert len(_rows_with_words(rows, RESIZE_WORDS)) == 2, \
            "the reply should start on two rows at 80 columns:\n" + "\n".join(rows)
        for cols, want in steps:
            s.resize(cols, ROWS, delay=0.8)
            s._drain(0.5)
            disp = s.display()
            width = _pane_width(disp)
            rows = pane_rows_with_history(s, RESIZE_WORDS)
            found.append((cols, width, len(_rows_with_words(rows, RESIZE_WORDS)), rows))
    for (cols, want), (_, width, got, rows) in zip(steps, found):
        _assert_whole_at_width(rows, RESIZE_WORDS, width, "at %d columns" % cols)
        assert got == want, \
            "at %d columns (pane width %d) the reply should take %d rows, takes %d:\n%s" \
            % (cols, width, want, got, "\n".join(rows))


# @scenario SC-ai-chat-35
def test_wrapped_reply_copies_as_one_line(tmp_path):
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path, LONG_REPLY),
                    filename="t.c") as s:
        _ask(s, "q", "w29")
        disp = s.display()
        y1 = next(i for i, r in enumerate(disp) if "AI: w00" in r)
        y2 = y1 + 1
        assert "w29" in disp[y2], \
            "the reply did not wrap onto the next row:\n" + "\n".join(disp)
        # Drag from the first character of the reply to one past the last
        # character of its second row (SGR mouse, 1-based columns), copy, and
        # read what reached the OS clipboard.
        end = len(disp[y2].rstrip()) + 1
        s.key("\033[<0;%d;%dM" % (2, y1 + 1), delay=0.2)
        s.key("\033[<32;%d;%dM" % (end, y2 + 1), delay=0.2)
        s.key("\033[<0;%d;%dm" % (end, y2 + 1), delay=0.3)
        s.key("\x03", delay=0.5)
        payload = s.osc52_payload()
    assert payload is not None, "nothing reached the OS clipboard (no OSC 52)"
    assert "w00" in payload and "w29" in payload, "the copy is incomplete: %r" % payload
    assert "\n" not in payload.rstrip("\n"), \
        "the wrapped reply copied as several lines: %r" % payload
    assert " ".join(payload.split()) == "AI: " + LONG_REPLY, \
        "the copy does not read as the one line written: %r" % payload
