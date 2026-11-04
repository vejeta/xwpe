"""The keys reach the editor while gdb answers at a stop (X11).

tests/stubs/gdb stands in front of the real gdb and holds back its `where`
until the test writes to its release pipe, so the editor waits for gdb's
answer at the first stop exactly as long as the test needs.  Meanwhile the
display's own event loop must hand every key to the editor: Ctrl-G K is
refused instead of being read behind the editor's back, typed text goes
into the file and File > Save writes it, all before gdb has answered.  The
terminal rows of the same scenario are in tests/test_debugger_integration.py.

The Wayland twin (tests/wayland/test_debug_answer_wait.py) runs the same
body; it skips there, as the other rows that need a debug session do (the
headless compositor harness cannot start one).
"""
import os
import shutil
import time

import pytest

from conftest import _sleep

HERE = os.path.dirname(os.path.abspath(__file__))
STUBS_DIR = os.path.join(HERE, "..", "stubs")
STUB_LOG = "gdb-stub.log"
STUB_RELEASE = "gdb-stub.release"
HOLD_TIMEOUT_S = 40
STOP_TIMEOUT_S = 20
POLL_S = 0.3
STOP_GREEN = (0, 255, 0)        # the stop line's bar
MIN_BAR_PIXELS = 60             # a bar is a dozen cells wide
TYPED = "MARK"
BREAK_LINE = 5
# The first column of BREAK_LINE in the source window, at the fixture's
# pinned font size: a click there brings the source back on top (the run
# put Messages there, for the program's input) and puts the cursor there.
BREAK_LINE_X = 12
BREAK_LINE_Y = 110

PROG = (
    "#include <stdio.h>\n"
    "int g = 42;\n"
    "int main(void)\n"
    "{\n"
    "  printf(\"READY\\n\");\n"
    "  return g;\n"
    "}\n"
)
PROG_WITH_TYPED = PROG.replace("  printf", TYPED + "  printf", 1)

STUB_ENV = {
    "PATH": os.path.abspath(STUBS_DIR) + ":" + os.environ.get("PATH", ""),
    "XWPE_STUB_REAL_GDB": shutil.which("gdb") or "",
    "XWPE_STUB_STALL": "where",
    "XWPE_STUB_STALL_NTH": "1",
    "XWPE_TEST_SOURCE": PROG,
}


def _wait(predicate, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        _sleep(POLL_S)
    return predicate()


def _stub_log(home):
    try:
        with open(os.path.join(home, STUB_LOG)) as fh:
            return fh.read()
    except OSError:
        return ""


def _count(img, rgb):
    colours = img.getcolors(maxcolors=img.width * img.height) or []
    return sum(count for count, px in colours if px == rgb)


def _go_to_line(xwpe, line):
    xwpe.menu("s", "g", delay=0.6)
    xwpe.type(str(line), delay=0.4)
    xwpe.key("Return", delay=0.6)


# @scenario SC-debug-49
@pytest.mark.skipif(not shutil.which("gdb") or not shutil.which("gcc"),
                    reason="no gdb or gcc")
@pytest.mark.parametrize("xwpe", [STUB_ENV], indirect=True)
def test_gdb_keys_reach_the_editor_while_it_answers(xwpe):
    """gdb holds back its answer to `where` at the first stop.  Ctrl-G K,
    text and File > Save typed meanwhile all reach the editor: the file on
    disk gets the text and no "k", before gdb answers.  Once it answers the
    stop's green bar is drawn."""
    if not getattr(xwpe, "can_debug", True):
        pytest.skip("no debug session under the headless compositor harness")
    home = os.path.dirname(xwpe.srcfile)
    _go_to_line(xwpe, BREAK_LINE)
    xwpe.key("ctrl+g", "b", delay=0.6)
    xwpe.key("ctrl+g", "r", delay=1.0)
    assert _wait(lambda: "STALL where" in _stub_log(home), HOLD_TIMEOUT_S), \
        "the editor should ask gdb `where` at the stop:\n%s" % _stub_log(home)
    xwpe.click(BREAK_LINE_X, BREAK_LINE_Y, delay=0.6)
    xwpe.key("ctrl+g", "k", delay=0.6)
    xwpe.type(TYPED, delay=0.4)
    xwpe.save()
    assert _wait(lambda: xwpe.saved_text() == PROG_WITH_TYPED, STOP_TIMEOUT_S), \
        "keys typed while gdb answers should reach the editor, and Ctrl-G K " \
        "should type nothing (file on disk:\n%r)" % xwpe.saved_text()
    with open(os.path.join(home, STUB_RELEASE), "w") as fh:
        fh.write("go\n")
    assert _wait(lambda: _count(xwpe.screenshot(), STOP_GREEN) >= MIN_BAR_PIXELS,
                 STOP_TIMEOUT_S), \
        "once gdb answers, the stop should be drawn as the green bar"
