"""A debug start and a debug stop on native Wayland leave a line in the trace.

A Wayland client can neither take the keyboard nor raise its window, so a
debug start and a debug stop do nothing to the focus or the stacking (the
X11 rows of tests/x11/test_debug_focus.py have no twin here).  What the
editor does is say so in the trace file: "focus_editor" at a start and
"raise_editor" at a stop.  Someone chasing a debug stop that did not bring
the window forward reads there that the compositor, not the editor, decides.

The lines go to the file $XWPE_UI_TRACE names, which every build writes.
"""
import os

import pytest

from conftest import _sleep

TRACE = "wpe.trace"          # relative: the editor runs in the row's directory
START_LINE = "focus_editor: a client cannot take the keyboard"
STOP_LINE = "raise_editor: a client cannot raise its window"
BREAK_LINE = 14              # return 0;  after the line is read
RUN_TIMEOUT_S = 40
TRACE_TIMEOUT_S = 15

BLOCKED_PROG = (
    "#include <stdio.h>\n"
    "int main(void)\n"
    "{\n"
    "  char line[100];\n"
    "  FILE *o;\n"
    "  o = fopen(\"started.txt\", \"w\");\n"
    "  fputs(\"1\", o);\n"
    "  fclose(o);\n"
    "  if (fgets(line, sizeof line, stdin)) {\n"
    "    o = fopen(\"got.txt\", \"w\");\n"
    "    fputs(line, o);\n"
    "    fclose(o);\n"
    "  }\n"
    "  return 0;\n"
    "}\n"
)


def _wait(predicate, timeout):
    import time
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        _sleep(0.3)
    return predicate()


def _trace(xwpe):
    try:
        with open(os.path.join(os.path.dirname(xwpe.srcfile), TRACE)) as fh:
            return fh.read()
    except OSError:
        return ""


# @scenario SC-display-04
@pytest.mark.parametrize("xwpe", [{"XWPE_TEST_SOURCE": BLOCKED_PROG,
                                   "XWPE_UI_TRACE": TRACE}], indirect=True)
def test_a_debug_start_and_stop_say_the_compositor_decides(xwpe):
    """Breakpoint after the read, Ctrl-G R: the start writes the focus line.
    A line typed to the program makes it reach the breakpoint: the stop
    writes the raise line."""
    for _ in range(BREAK_LINE - 1):
        xwpe.key("Down", delay=0.1)
    xwpe.key("ctrl+g", "b")
    xwpe.key("ctrl+g", "r")
    started = os.path.join(os.path.dirname(xwpe.srcfile), "started.txt")
    assert _wait(lambda: os.path.exists(started), RUN_TIMEOUT_S), \
        "the program should build, start under gdb and run"
    assert _wait(lambda: START_LINE in _trace(xwpe), TRACE_TIMEOUT_S), \
        "a debug start should write the focus line:\n%s" % _trace(xwpe)
    assert STOP_LINE not in _trace(xwpe), \
        "no stop has happened yet:\n%s" % _trace(xwpe)
    xwpe.type("go")
    xwpe.key("Return")
    assert _wait(lambda: STOP_LINE in _trace(xwpe), TRACE_TIMEOUT_S), \
        "a debug stop should write the raise line:\n%s" % _trace(xwpe)
    assert xwpe.proc.poll() is None, "xwpe died at the debug stop"
