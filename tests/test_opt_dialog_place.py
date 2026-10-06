"""Where a modal dialog lands, and what it does when the screen cannot hold it.

The option-dialog engine (e_opt_kst) centres a box on the active window, except
over a docked tool pane (the AI chat strip, ins == 8), where it centres on the
whole screen.  A box that does not fit is clamped into the top-left corner,
only the widgets whose origin falls inside the frame are drawn, and every key
is swallowed until Esc or a resize.  While a box is up, asynchronous painters
(the AI pane's streamed reply) must leave the screen alone.

The resizes here are real resizes.  WpeSession.resize() alone never reaches
the ncurses backend: the child runs in its own session without a controlling
tty, so TIOCSWINSZ raises no SIGWINCH, and the harness exports LINES/COLUMNS,
which ncurses prefers over the pty size.  The tests therefore run wpe through
a two-line wrapper that unsets both variables and send SIGWINCH themselves.

Driven on the AI settings dialog (Options > AI), 64x21 cells.
"""
import os
import signal
import stat
import subprocess

import pytest

import wpe_driver
from wpe_driver import WpeSession, ALT, WPE_BIN, incoherence


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"AI settings" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")

ENV = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock"}
SEED = "int main(void){return 0;}\n"
TITLE = "AI settings"
ESC = "\033"
F2_SAVE = "\033OQ"
# pyte shows the DEC line-drawing set as its ASCII code points: the frame
# corners come out as l (top-left), k (top-right), m (bottom-left), the
# vertical border as x.
CORNER_TL, CORNER_TR, CORNER_BL, V_BORDER = "l", "k", "m", "x"
# Labels of the AI settings widgets whose row lies past a 16-row screen.
BELOW_16_ROWS = ("CA file", "API key", "Advanced", " Ok ", "Cancel")


def _open_ai_settings(s):
    s.key(ALT.OPTIONS, delay=0.5)
    s.key("i", delay=0.7)


def _open_chat_pane(s):
    """Alt-G a, one prompt: the docked AI pane opens and keeps the focus."""
    s.key(ALT.AI)
    s.key("a")
    s._drain(0.4)
    s.key("hi")
    s.key("\r", delay=0.5)


def _row_of(s, needle):
    for i, row in enumerate(s.display()):
        if needle in row:
            return i
    return None


def _frame(s):
    """(top, bottom, left, right) rows/cols of the dialog frame, from the
    title row: the corners around the title and the bottom-left corner below."""
    rows = s.display()
    top = _row_of(s, TITLE)
    assert top is not None, "no dialog title on screen:\n" + "\n".join(rows)
    at = rows[top].index(TITLE)
    left = rows[top].rfind(CORNER_TL, 0, at)
    right = rows[top].find(CORNER_TR, at)
    bottom = next((i for i in range(top + 1, len(rows)) if rows[i][left] == CORNER_BL), -1)
    assert left >= 0 and right > left and bottom > top, \
        "dialog frame not intact (%d,%d,%d,%d):\n%s" % (top, bottom, left, right, "\n".join(rows))
    return top, bottom, left, right


def _inside(rows, frame):
    top, bottom, left, right = frame
    return [rows[y][left:right + 1] for y in range(top, bottom + 1)]


def _dismiss(s):
    """Esc closes the dialog.  From the Enable checkbox, which has the initial
    focus, the engine spends the first Esc moving the focus to Cancel; press it
    again when the box is still up."""
    s.key(ESC, delay=0.6)
    if _row_of(s, TITLE) is not None:
        s.key(ESC, delay=0.6)


@pytest.fixture
def pty_sized_wpe(tmp_path, monkeypatch):
    """Run wpe without the harness's LINES/COLUMNS so ncurses sizes itself
    from the pty, which is what a real terminal resize changes."""
    wrapper = tmp_path / "wpe"
    wrapper.write_text("#!/bin/sh\nunset LINES COLUMNS\nexec \"%s\" \"$@\"\n"
                       % os.path.abspath(WPE_BIN))
    wrapper.chmod(stat.S_IRWXU)
    monkeypatch.setattr(wpe_driver, "WPE_BIN", str(wrapper))


def _real_resize(s, cols, rows):
    """Resize the pty and deliver the SIGWINCH the kernel would send to a
    foreground process group; drain the relayout."""
    s.resize(cols, rows, delay=0.2)
    os.kill(s.proc.pid, signal.SIGWINCH)
    s._drain(1.0)


# @scenario SC-dialog-engine-02
def test_dialog_over_docked_pane_centres_on_screen(tmp_path):
    env = dict(ENV, XWPE_AI_MOCK_REPLY="PANEREPLY")
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _open_chat_pane(s)
        s._drain(1.0)
        pane_top = _row_of(s, "q AI q")
        assert pane_top is not None, \
            "the docked AI pane did not open:\n" + "\n".join(s.display())
        _open_ai_settings(s)
        top, bottom, _, _ = _frame(s)
        rows = s.display()
    screen_centre = wpe_driver.ROWS / 2.0
    box_centre = (top + bottom) / 2.0
    assert abs(box_centre - screen_centre) <= 2, \
        "dialog rows %d..%d are not centred on the %d-row screen:\n%s" % (
            top, bottom, wpe_driver.ROWS, "\n".join(rows))
    assert top < pane_top, \
        "the dialog was squeezed into the pane strip (pane title row %d):\n%s" % (
            pane_top, "\n".join(rows))


# @scenario SC-dialog-engine-03
def test_degenerate_geometry_falls_back_to_corner(tmp_path):
    # A 20x6 screen cannot hold a 64x21 box: the frame is clamped to column 0,
    # row 1 (menu bar above, status line below) and, because it is clipped,
    # the engine also takes the too-small path: keys are swallowed until Esc.
    # Both are asserted; the corner is the placement, the swallow is the
    # consequence of the clipping.
    with WpeSession(str(tmp_path), SEED, cols=20, rows=6, env_extra=ENV) as s:
        _open_ai_settings(s)
        assert s.alive(), "xwpe died opening a dialog on a 20x6 screen"
        rows = s.display()
        assert rows[1][0] == CORNER_TL and rows[1][-1] == CORNER_TR, \
            "frame top is not at column 0 / row 1:\n" + "\n".join(rows)
        assert rows[4][0] == CORNER_BL, \
            "frame bottom is not clamped to row 4 (rows-2):\n" + "\n".join(rows)
        s.key("zz", delay=0.4)
        assert s.display() == rows, \
            "keys were not swallowed on the clipped dialog:\n" + "\n".join(s.display())
        s.key(ESC, delay=0.6)
        assert s.display()[1][0] != CORNER_TL, \
            "Esc did not close the clipped dialog:\n" + "\n".join(s.display())
        s.key("QQ", delay=0.3)
        s.key(F2_SAVE, delay=0.6)
        saved = s.text()
    assert saved.startswith("QQ"), \
        "the editor did not take input after the clipped dialog: %r" % saved


# @scenario SC-dialog-engine-29
def test_screen_too_small_swallows_keys_until_esc(tmp_path, pty_sized_wpe):
    with WpeSession(str(tmp_path), SEED, env_extra=ENV) as s:
        _open_ai_settings(s)
        _real_resize(s, 40, 12)
        assert s.alive(), "xwpe died on the shrink to 40x12"
        shrunk = s.display()
        assert _row_of(s, TITLE) is not None, \
            "the dialog vanished on the shrink:\n" + "\n".join(shrunk)
        s.key("zqx", delay=0.5)
        typed = s.display()
        assert typed == shrunk, \
            "letters reached the clipped dialog:\n" + "\n".join(typed)
        s.key(ESC, delay=0.6)
        assert _row_of(s, TITLE) is None, \
            "Esc did not close the clipped dialog:\n" + "\n".join(s.display())
        s.key("MARK", delay=0.3)
        s.key(F2_SAVE, delay=0.6)
        saved = s.text()
    assert saved.startswith("MARK"), \
        "the editor was not usable after the clipped dialog: %r" % saved


# @scenario SC-dialog-engine-30
def test_clipped_widgets_are_not_drawn(tmp_path, pty_sized_wpe):
    with WpeSession(str(tmp_path), SEED, cols=60, rows=30, env_extra=ENV) as s:
        _open_ai_settings(s)
        _real_resize(s, 50, 16)
        rows = s.display()
        top, bottom, left, right = _frame(s)
    joined = "\n".join(rows)
    assert (top, left) == (1, 0) and bottom <= 14 and right <= 49, \
        "the clipped frame is not clamped into the 50x16 screen: %r\n%s" % (
            (top, bottom, left, right), joined)
    assert "Enable AI" in joined, "the first widget is missing:\n" + joined
    for label in BELOW_16_ROWS:
        assert label not in joined, \
            "widget %r lies below the frame yet was drawn:\n%s" % (label, joined)


# @scenario SC-dialog-engine-30
@incoherence("a widget whose origin is inside the frame is drawn through the right "
             "border: e_opt_element_visible checks the origin only, so at 50x16 the "
             "Permission radios and the Endpoint field overwrite the border column")
def test_clipped_widgets_do_not_bleed_through_the_border(tmp_path, pty_sized_wpe):
    with WpeSession(str(tmp_path), SEED, cols=60, rows=30, env_extra=ENV) as s:
        _open_ai_settings(s)
        _real_resize(s, 50, 16)
        rows = s.display()
        top, bottom, left, right = _frame(s)
    broken = [y for y in range(top + 1, bottom) if rows[y][right] != V_BORDER]
    assert not broken, \
        "widget text overwrote the right border on rows %r:\n%s" % (broken, "\n".join(rows))


# @scenario SC-dialog-engine-35
def test_async_update_does_not_paint_over_dialog(tmp_path):
    marker = "ASYNCMARKER77"
    env = dict(ENV, XWPE_AI_MOCK_REPLY=marker, XWPE_AI_MOCK_DELAY_MS="3000")
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _open_chat_pane(s)
        _open_ai_settings(s)                    # the reply is still pending
        frame = _frame(s)
        before = _inside(s.display(), frame)
        s._drain(3.5)                           # the mock reply lands meanwhile
        after = _inside(s.display(), frame)
        _dismiss(s)
        s._drain(0.5)
        released = "\n".join(s.display())
    assert not any(marker in line for line in after), \
        "the reply was painted over the open dialog:\n" + "\n".join(after)
    assert after == before, \
        "the dialog box changed under an async update:\nBEFORE:\n%s\nAFTER:\n%s" % (
            "\n".join(before), "\n".join(after))
    assert marker in released, \
        "the deferred reply did not appear once the dialog closed:\n" + released
