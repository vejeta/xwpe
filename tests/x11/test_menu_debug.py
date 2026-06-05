"""Debug-menu (Alt-D) tests for xwpe (X11/Xft).

Full gdb debug sessions (Trace/Step/breakpoints/watches, Ctrl-G ...) are
covered by the wpe debug tests.  Here we cover the one purely-visual Debug
action that needs no running inferior: Toggle Breakpoint (F5) marks the
current line, which we detect as a screenshot change.
"""
import time

from conftest import changed_pixels


def test_toggle_breakpoint_marks_the_line(xwpe):
    """Debug -> Toggle Breakpoint (F5) paints a breakpoint marker on the line."""
    xwpe.key("Down"); xwpe.key("Down")   # move onto an interior code line
    before = xwpe.screenshot()
    xwpe.key("F5")                       # Debug -> Toggle Breakpoint
    time.sleep(0.3)
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died toggling a breakpoint"
    assert changed_pixels(before, after) > 200, \
        "Toggle Breakpoint should mark the line (screen did not change)"
