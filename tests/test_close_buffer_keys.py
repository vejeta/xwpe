"""Modernised window-control keys (wpe terminal mode).

Two rebindings, verified against the SAME real key sequences a user types:

  - Ctrl-W  -> Close active window   (browser/VSCode/IntelliJ "close tab")
              was: Show Buffer
  - Alt-Y   -> Show Buffer           (emacs M-y, browse the paste buffer)
              the buffer window's title bar reads " Buffer " (BUFFER_NAME)

Red-green note: on the OLD binding Ctrl-W opened the Buffer window instead
of closing c.c (so test_ctrl_w_closes_active_window fails), and Alt-Y was
unbound globally (so test_alt_y_shows_buffer fails).  Both pass only with the
new bindings.

Run: tests/.venv/bin/python -m pytest -v tests/test_close_buffer_keys.py
"""
from test_window_menu import run_windows, visible_titles

CTRL_W = '\x17'      # Ctrl-W -> close window
ALT_Y = '\033y'      # Alt-Y  -> Show Buffer (Clipboard)


def test_ctrl_w_closes_active_window(tmp_path):
    """Ctrl-W closes the active window (c.c), leaving the others open -- it no
    longer opens the Clipboard."""
    screen, code = run_windows(str(tmp_path), [CTRL_W])
    assert code is None, 'wpe died on Ctrl-W (exit=%r)' % code
    titles = visible_titles(screen)
    assert 'c.c' not in titles, \
        'Ctrl-W should close the active window (c.c), saw %s' % titles
    assert {'a.c', 'b.c'} <= titles, \
        'Ctrl-W should keep the other windows open, saw %s' % titles
    blob = '\n'.join(screen.display)
    assert ' Buffer ' not in blob, \
        'Ctrl-W must no longer open the Buffer window (that moved to Alt-Y)'


def test_alt_y_shows_buffer(tmp_path):
    """Alt-Y opens the Show Buffer window (title bar reads " Buffer ")."""
    screen, code = run_windows(str(tmp_path), [ALT_Y])
    assert code is None, 'wpe died on Alt-Y (exit=%r)' % code
    blob = '\n'.join(screen.display)
    assert ' Buffer ' in blob, \
        'Alt-Y should open the Buffer window:\n%s' % blob
