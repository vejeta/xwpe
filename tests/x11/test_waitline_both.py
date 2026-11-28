"""A build and a starting debug adapter keep both waiting lines (X11).

The two lines are rows of the Messages text, one under the other, the line
the next Esc ends at the bottom.  The terminal row is G18 of
tests/test_esc_owner.py and tests/test_waitline_owner.py, which read the
screen's text.  A graphical display shows pixels, so this row reads two
things: what the owner of the lines says in the editor's trace (a line is
laid with the row it took), and the screen itself, where the seconds of both
lines turn together, in two rows that touch (a line not painted, or the two
sharing one row, leaves one band of changed pixels or none).  Then it checks
that the editor takes keys and ends both waits with Esc: the build first,
then the adapter's start.  Repaint is the display's own, so each backend
runs the row.

The Wayland twin runs this same body.
"""
import os
import stat
import sys

import pytest

from PIL import ImageChops

import gui_owner as owner_api
from conftest import wait_for

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))

TRACE = "ui.trace"
BOTH_WAIT_S = 40
POLL_S = 0.3
DLV_LAID = "waitline laid the debugger start row="
BUILD_LAID = "waitline laid the build row="
CANCELLED = "e_cancel_key what="
PIXEL_THRESHOLD = 40            # grey levels that count as a changed pixel
MIN_CHANGED_PIXELS = 4          # a digit turning changes more than this
PROG = "package main\n\nfunc main() {\n\tx := 3\n\t_ = x\n}\n"
SESSION = {
    "XWPE_TEST_SOURCE": PROG,
    "XWPE_TEST_FILE": "main.go",
    "XWPE_UI_TRACE": TRACE,
    "XDG_STATE_HOME": "",
    **owner_api.product_env("XWPE_DEBUG_START_TIMEOUT", 600),
    "PATH": "bin:" + os.environ.get("PATH", ""),
}
# A dlv that never answers, and a build that waits on a fifo the row owns.
FAKE_DLV = "#!/bin/sh\nexec 3>&1\nexec cat 3>&3 > adapter-stdin.log\n"
# The build says nothing until the row releases it: it has printed no row
# of its own that could move the lines while the row reads the screen.
SLOW_MAKE = ("all:\n\t@echo $$PPID > build.pid\n"
             "\t@read _ < build.release\n\t@echo BUILD_DONE\n\t@touch prog\n")


def _wait(predicate, what, timeout=BOTH_WAIT_S):
    """Wait, through the owner's clock, until @predicate is true; fail naming
    @what when it never is."""
    return wait_for(predicate, what, timeout, poll=POLL_S)


def _changed_bands(before, after):
    """The bands of pixel rows that differ between two screenshots.

    Return: a list of (first row, last row), top to bottom; a row differs when
    more than MIN_CHANGED_PIXELS of its pixels changed by more than
    PIXEL_THRESHOLD grey levels.  One text row of a window that changed is
    one band."""
    diff = ImageChops.difference(before, after).convert("L")
    mask = diff.point(lambda p: 255 if p > PIXEL_THRESHOLD else 0)
    width, height = mask.size
    bands, start = [], None
    for y in range(height):
        changed = mask.crop((0, y, width, y + 1)).histogram()[255] >= MIN_CHANGED_PIXELS
        if changed and start is None:
            start = y
        if not changed and start is not None:
            bands.append((start, y - 1))
            start = None
    if start is not None:
        bands.append((start, height - 1))
    return bands


def _quiet_screen(xwpe):
    """A screenshot of the screen once it has stopped changing.

    Two screenshots in a row that show the same pixels: the layout the lines
    were laid into has settled, and what changes after that is the lines
    turning.  Return: that screenshot."""
    last = {"shot": xwpe.screenshot(), "quiet": None}

    def settled():
        shot = xwpe.screenshot()
        if not _changed_bands(last["shot"], shot):
            last["quiet"] = shot
        last["shot"] = shot
        return last["quiet"]
    _wait(settled, "the screen to settle once both lines are laid")
    return last["quiet"]


def _last_two_touch(bands):
    """True when the last two bands are one text row each and touch: the gap
    between them is smaller than the band's own height."""
    if len(bands) < 2:
        return False
    (a0, a1), (b0, b1) = bands[-2], bands[-1]
    return b0 - a1 - 1 < max(a1 - a0 + 1, b1 - b0 + 1)


def _trace(home):
    try:
        with open(os.path.join(home, TRACE), errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def _row_of(line):
    return int(line.rsplit("row=", 1)[1])


def _foot_is_two_rows(home):
    """True once the owner laid the debugger's line and, right after it, the
    build's, one row below."""
    lines = [l for l in _trace(home).splitlines() if "waitline laid " in l]
    for first, second in zip(lines, lines[1:]):
        if DLV_LAID in first and BUILD_LAID in second:
            if _row_of(second) == _row_of(first) + 1:
                return True
    return False


def _cancels(home):
    return [l for l in _trace(home).splitlines() if l.startswith(CANCELLED)]


def _make_stand_ins(home):
    os.mkdir(os.path.join(home, "bin"))
    dlv = os.path.join(home, "bin", "dlv")
    with open(dlv, "w") as fh:
        fh.write(FAKE_DLV)
    os.chmod(dlv, os.stat(dlv).st_mode | stat.S_IXUSR)
    with open(os.path.join(home, "Makefile"), "w") as fh:
        fh.write(SLOW_MAKE)
    os.mkfifo(os.path.join(home, "build.release"))


def _release_build(home):
    try:
        fd = os.open(os.path.join(home, "build.release"),
                     os.O_WRONLY | os.O_NONBLOCK)
    except OSError:
        return
    try:
        os.write(fd, b"go\n")
    finally:
        os.close(fd)


# @scenario SC-waitline-09
@pytest.mark.parametrize("xwpe", [SESSION], indirect=True)
def test_both_waiting_lines_are_laid_and_each_esc_ends_one(xwpe):
    """The adapter starts, then a build: the owner lays two adjacent rows, the
    build's at the bottom.  The first Esc ends the build (the owner's line says
    which claim took it) and the second ends the adapter's start."""
    home = os.path.dirname(xwpe.srcfile)
    _make_stand_ins(home)
    try:
        xwpe.key("ctrl+g", "r", delay=1.5)
        _wait(lambda: DLV_LAID in _trace(home), "the adapter's waiting line laid")
        xwpe.key("alt+a", delay=1.0)
        _wait(lambda: _foot_is_two_rows(home), "the two lines laid in two rows")
        _wait(lambda: os.path.exists(os.path.join(home, "build.pid")),
              "the build's recipe to run")
        first = _quiet_screen(xwpe)
        # The rows of the build's own output may still settle after the lines
        # are laid.  Once the screen has changed and settled again, only the
        # seconds can change it: both lines turning is the second change.
        _wait(lambda: _changed_bands(first, xwpe.screenshot()),
              "the screen to change once more")
        quiet = _quiet_screen(xwpe)
        _wait(lambda: _last_two_touch(_changed_bands(quiet, xwpe.screenshot())),
              "the seconds of both lines turning in two touching rows")
        xwpe.key("Escape", delay=1.0)
        _wait(lambda: len(_cancels(home)) >= 1, "the first Esc taken by a claim")
        assert "what=the build" in _cancels(home)[0], \
            "the first Esc did not end the build: %r" % _cancels(home)
        xwpe.key("Escape", delay=1.0)
        _wait(lambda: len(_cancels(home)) >= 2, "the second Esc taken by a claim")
        assert "what=the debugger start" in _cancels(home)[1], \
            "the second Esc did not end the debugger's start: %r" % _cancels(home)
    finally:
        _release_build(home)
