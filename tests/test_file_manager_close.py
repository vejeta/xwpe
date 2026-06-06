"""File-manager dialog chrome + close box for wpe (terminal mode).

The File-Manager (Open / Save As / search pickers) is a fixed-size dialog, so
like a Borland TDialog it must carry ONLY a close box -- no maximize/zoom box --
and clicking that close [X] must dismiss it.

Two regressions are guarded:
  1. The picker was drawn with e_draw_window_buttons (close + maximize □); it
     now uses e_draw_dialog_close_button (close only).
  2. WpeMngMouseInFileManager hit-tested the close box at the OLD top-left
     position (a.x+3) while the glyph is drawn top-right (e.x-2), so clicking
     the visible ✕ started a window move instead of closing.  It now hit-tests
     e_hit_close_button (e.x-2).

Run: tests/.venv/bin/python -m pytest -v tests/test_file_manager_close.py
"""
from test_window_close_button import _Wpe, CLOSE_GLYPH, MAX_GLYPH

F3 = '\033OR'          # opens the File-Manager (xterm-256color kf3)
FM_TITLE = 'File-Manager'


def _open_fm(w):
    w.drain(1.3)
    w.send(F3)
    w.drain(1.0)


def test_file_manager_has_close_box_no_maximize(tmp_path):
    """The File-Manager title bar shows a close ✕ but NO maximize box."""
    w = _Wpe(str(tmp_path))
    try:
        _open_fm(w)
        disp = '\n'.join(w.screen.display)
        assert FM_TITLE in disp, "File-Manager did not open:\n%s" % disp
        assert CLOSE_GLYPH in disp, "File-Manager has no close box:\n%s" % disp
        # The only window on screen is the FM (over the editor, whose own title
        # bar is covered), so a maximize glyph here would be the FM's.
        assert MAX_GLYPH not in disp, \
            "File-Manager must not carry a maximize box:\n%s" % disp
    finally:
        w.close()


def test_file_manager_close_box_dismisses(tmp_path):
    """Clicking the File-Manager close ✕ dismisses the dialog."""
    w = _Wpe(str(tmp_path))
    try:
        _open_fm(w)
        assert any(FM_TITLE in ln for ln in w.screen.display), "FM did not open"
        spot = w.find_glyph(CLOSE_GLYPH)
        assert spot, "close glyph not found in FM title bar"
        w.click(*spot)
        assert w.alive(), "wpe died clicking the FM close box"
        disp = '\n'.join(w.screen.display)
        assert FM_TITLE not in disp, \
            "clicking ✕ should dismiss the File-Manager, but it is still open:\n%s" % disp
    finally:
        w.close()
