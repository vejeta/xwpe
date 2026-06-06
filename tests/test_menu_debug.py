"""Debug-menu (Alt-D) regression tests for wpe (terminal mode).

Debug items (we_menue.c): Toggle Breakpoint -> e_breakpoint, Trace, Step,
Make Watch -> e_make_watches, etc.  Full gdb sessions (Trace/Step over a
running inferior) are timing-heavy; here we cover the one Debug action that
needs no inferior and is visible on the text screen: Toggle Breakpoint paints
the current line as a breakpoint (red background marker).

Per the project's pyte rule, we assert on the CELL COLORS, not the glyphs:
the marker is a background-colour change, invisible in a plain text dump.
"""
import time

from wpe_driver import WpeSession, ALT


def _has_red_line(session):
    """True if any editor row carries a red-background cell (breakpoint mark)."""
    for y in range(2, 10):
        row = session.screen.buffer[y]
        if any(row[x].bg == "red" for x in range(1, 40)):
            return True
    return False


def test_toggle_breakpoint_marks_the_line(tmp_path):
    """Debug -> Toggle Breakpoint paints the current line red (and back)."""
    seed = "int main(){\n  int x=1;\n  return 0;\n}\n"
    with WpeSession(str(tmp_path), seed) as w:
        time.sleep(0.3)
        assert not _has_red_line(w), "no breakpoint marker expected before toggling"
        w.menu(ALT.DEBUG, "b")           # Debug -> Toggle Breakpoint
        time.sleep(0.4)
        assert w.alive(), "wpe died toggling a breakpoint"
        assert _has_red_line(w), \
            "Toggle Breakpoint should paint the line red:\n%s" % \
            "\n".join(w.display()[1:8])
        w.menu(ALT.DEBUG, "b")           # toggle off again
        time.sleep(0.4)
        assert not _has_red_line(w), "second toggle should clear the breakpoint"


# --- shortcut path (#159): same action via the advertised accelerator ---
#
# Toggle Breakpoint is advertised as "^F8 / ^G B" in the default key style.
# ^G B (the debugger prefix Ctrl-G then B) is build-independent and decodes in
# any terminal, so it is the reliable accelerator to assert.  (F5 is the Window
# "Zoom" key in the default build, not a breakpoint key -- a reminder that the
# advertised accelerator differs by key style, exactly what #159 guards.)
CTRL_G = "\x07"          # debugger command prefix


def test_toggle_breakpoint_via_ctrl_g_b(tmp_path):
    """^G B (debugger prefix) toggles the breakpoint marker on the line."""
    seed = "int main(){\n  int x=1;\n  return 0;\n}\n"
    with WpeSession(str(tmp_path), seed) as w:
        time.sleep(0.3)
        assert not _has_red_line(w), "no breakpoint expected before ^G B"
        w.key(CTRL_G, "b")
        time.sleep(0.4)
        assert w.alive(), "wpe died on ^G B"
        assert _has_red_line(w), \
            "^G B should paint the breakpoint line red:\n%s" % \
            "\n".join(w.display()[1:8])
