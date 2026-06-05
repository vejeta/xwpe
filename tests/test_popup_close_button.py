"""Popup title-bar buttons for wpe (terminal mode).

Borland message/dialog boxes never had a zoom/maximize box -- only a close
box.  xwpe used to draw BOTH a maximize glyph and a close glyph on every
popup, including a "String NOT found" message that cannot meaningfully be
resized.  The fix keeps only the close glyph on popups; document (editor)
windows still carry their maximize box.

We assert on the glyphs in the popup's title row: close present, maximize
absent.  Cross-checks the editor window still has both.
"""
import time

from wpe_driver import WpeSession, ALT

CLOSE = "✕"      # the close glyph drawn by e_draw_titlebar_buttons
MAXIMIZE = "□"   # the box-maximize glyph (terminal), must NOT be on popups


def _row_with(rows, needle):
    for r in rows:
        if needle in r:
            return r
    return ""


def test_message_popup_has_close_but_no_maximize(tmp_path):
    """A "String NOT found" popup shows a close box and no maximize box."""
    with WpeSession(str(tmp_path), "int main(){\n  return 0;\n}\n") as w:
        time.sleep(0.4)
        w.menu(ALT.SEARCH, "f")          # Search -> Find
        time.sleep(0.5)
        w.key("zzqq")                    # a string not in the file
        time.sleep(0.2)
        w.key("\r")                      # run search -> popup
        time.sleep(0.6)
        rows = w.display()
        popup = _row_with(rows, "Message")
        assert popup, "expected the 'Message' popup; screen:\n%s" % "\n".join(rows)
        assert CLOSE in popup, "popup must keep its close box, got: %r" % popup
        assert MAXIMIZE not in popup, \
            "popup must NOT have a maximize box (not Borland), got: %r" % popup
        w.key("\r")                      # dismiss


def test_editor_window_keeps_both_buttons(tmp_path):
    """The document window is not a popup: it keeps maximize AND close."""
    with WpeSession(str(tmp_path), "int main(){\n  return 0;\n}\n") as w:
        time.sleep(0.4)
        title = _row_with(w.display(), " t.c ")
        assert CLOSE in title and MAXIMIZE in title, \
            "editor window should keep both zoom and close boxes, got: %r" % title
