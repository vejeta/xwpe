"""The keys reach the editor while a debug adapter answers at a stop (X11).

dlv is held with SIGSTOP at a stop, after a watch was added, so the value
of the watch is owed for as long as the test needs.  Meanwhile the
display's own event loop must hand every key to the editor: Ctrl-G K is
refused instead of being asked of the adapter, typed text goes into the
file and File > Save writes it, all before dlv has answered.  The terminal
rows of the same scenario are in tests/test_debugger_integration.py.

The Wayland twin (tests/wayland/test_dap_answer_wait.py) runs the same
body.
"""
import os
import shutil
import signal
import sys
import time

import pytest

from conftest import _sleep

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import procinfo  # noqa: E402

START_TIMEOUT_S = 90            # dlv builds the package with a cold cache
ANSWER_TIMEOUT_S = 20
POLL_S = 0.3
STOP_GREEN = (0, 255, 0)        # the stop line's bar
# A stop bar fills its line's cells, some 2000 pixels at the fixture's font;
# letters of that green elsewhere make well under this.
MIN_BAR_PIXELS = 400
TYPED = "MARK"
WATCH = "x"
BREAK_LINE = 7
ADAPTER = "dlv"

PROG = (
    "package main\n"
    "\n"
    "import \"fmt\"\n"
    "\n"
    "func main() {\n"
    "\tx := 1\n"
    "\tfmt.Println(\"ready\", x)\n"
    "}\n"
)
GO_MOD = "module dapdemo\n\ngo 1.21\n"

DLV_ENV = {
    "XWPE_TEST_SOURCE": PROG,
    "XWPE_TEST_FILE": "main.go",
}


def _wait(predicate, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        _sleep(POLL_S)
    return predicate()


def _count(img, rgb):
    colours = img.getcolors(maxcolors=img.width * img.height) or []
    return sum(count for count, px in colours if px == rgb)


def _go_to_line(xwpe, line):
    xwpe.menu("s", "g", delay=0.6)
    xwpe.type(str(line), delay=0.4)
    xwpe.key("Return", delay=0.6)


def _drop_the_fixtures_c_file(home):
    """The fixture writes t.c next to the program; dlv builds the whole
    directory as one Go package, and a C file there would need cgo."""
    path = os.path.join(home, "t.c")
    if os.path.exists(path):
        os.remove(path)


def _let_go(pid):
    """Let the held adapter go on; nothing once it is gone."""
    if pid is None:
        return
    try:
        os.kill(pid, signal.SIGCONT)
    except ProcessLookupError:
        pass


# @scenario SC-debug-69
@pytest.mark.skipif(not shutil.which("dlv") or not shutil.which("go"),
                    reason="no dlv or go")
@pytest.mark.parametrize("xwpe", [DLV_ENV], indirect=True)
def test_dlv_keys_reach_the_editor_while_it_answers(xwpe):
    """dlv is held while it owes a watch's value at a stop.  Ctrl-G K,
    text and File > Save typed meanwhile all reach the editor: the file on
    disk gets the text before dlv answers.  Once it is let go the stop's
    green bar is still drawn."""
    home = os.path.dirname(xwpe.srcfile)
    with open(os.path.join(home, "go.mod"), "w") as fh:
        fh.write(GO_MOD)
    _drop_the_fixtures_c_file(home)
    _go_to_line(xwpe, BREAK_LINE)
    xwpe.key("ctrl+g", "b", delay=0.6)
    xwpe.key("ctrl+g", "r", delay=1.0)
    assert _wait(lambda: _count(xwpe.screenshot(), STOP_GREEN) >= MIN_BAR_PIXELS,
                 START_TIMEOUT_S), "Ctrl-G R should stop on the breakpoint"
    pids = procinfo.children_named(xwpe.proc.pid, ADAPTER)
    assert pids, "dlv should be a child of the editor"
    pid = pids[0]
    try:
        os.kill(pid, signal.SIGSTOP)
        xwpe.key("ctrl+g", "w", delay=0.8)
        xwpe.type(WATCH, delay=0.4)
        xwpe.key("Return", delay=0.6)
        xwpe.key("ctrl+g", "k", delay=0.6)
        xwpe.key("alt+1", delay=0.6)
        _go_to_line(xwpe, BREAK_LINE)
        xwpe.type(TYPED, delay=0.4)
        xwpe.save()
        assert _wait(lambda: TYPED in xwpe.saved_text(), ANSWER_TIMEOUT_S), \
            "keys typed while dlv answers should reach the editor " \
            "(file on disk:\n%r)" % xwpe.saved_text()
        assert procinfo.children_named(xwpe.proc.pid, ADAPTER) == [pid], \
            "the keys should be taken while dlv is still held"
        _let_go(pid)
        assert _wait(lambda: _count(xwpe.screenshot(), STOP_GREEN) >=
                     MIN_BAR_PIXELS, ANSWER_TIMEOUT_S), \
            "once dlv answers, the stop should still be drawn"
    finally:
        _let_go(pid)
