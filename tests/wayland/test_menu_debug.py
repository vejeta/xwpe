"""Native-Wayland twin of tests/x11/test_menu_debug.py.

The test bodies are the X11 module's, run here under this directory's
native-wl_surface `xwpe` fixture (conftest.py).  Single source of truth -- see
run_x11_module.  The debugger rows run here too: the program's own pty is its
terminal, so a debug session starts although the headless harness gives the
editor no terminal.  The Break and Ctrl+Pause key translation, which is
Wayland's own, is also asserted without a compositor by
tests/test_wayland_keysym.py."""
from conftest import run_x11_module

run_x11_module(__name__, "test_menu_debug")
