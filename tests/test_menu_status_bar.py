"""Contextual bottom status/hint bar tests for wpe (terminal mode).

The bottom line (e_pr_uul, we_main.c) is not fixed: it shows a different set
of function-key hints depending on which window has the focus --
  - editor window  -> eblst_o:  F2 Save / F3 Files / F4 Search / Alt-X Quit
  - Messages window -> mblst_o:  PreV/NexT Err / Compile / Make / RUn

A user relies on this bar to know which keys do what *right now*; if it stops
tracking the focused window, the editor silently lies about its shortcuts.
These tests pin the two main contexts and that the bar actually changes
between them.
"""
import time

from wpe_driver import WpeSession, ALT


def _bar(session):
    return session.display()[-1]


def test_editor_context_shows_editor_hints(tmp_path):
    """With the editor focused, the hint bar offers Save/Files/Search/Quit."""
    with WpeSession(str(tmp_path), "int main(){\n  return 0;\n}\n") as w:
        time.sleep(0.4)
        bar = _bar(w)
        assert "Save" in bar and "Search" in bar and "Quit" in bar, \
            "editor hint bar should show Save/Search/Quit, got: %r" % bar
        assert "Compile" not in bar and "Make" not in bar, \
            "editor hint bar must not show the Messages hints, got: %r" % bar


def test_messages_context_shows_message_hints(tmp_path):
    """Focusing the Messages window swaps in Compile/Make/RUn/Err hints."""
    with WpeSession(str(tmp_path), "int main(){\n  return 0;\n}\n") as w:
        time.sleep(0.4)
        editor_bar = _bar(w)
        w.menu(ALT.RUN, "m")             # Make -> opens the Messages window
        time.sleep(2.5)
        w.menu(ALT.WINDOW, "x")          # Window -> Next : focus Messages
        time.sleep(0.6)
        msg_bar = _bar(w)
        assert w.alive(), "wpe died switching focus to Messages"
        assert "Compile" in msg_bar and "Make" in msg_bar and "RUn" in msg_bar, \
            "Messages hint bar should show Compile/Make/RUn, got: %r" % msg_bar
        assert "Err" in msg_bar, \
            "Messages hint bar should offer error navigation, got: %r" % msg_bar
        assert msg_bar != editor_bar, \
            "the hint bar must change with the focused window (editor==messages)"
