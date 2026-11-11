"""Native-Wayland twin of tests/x11/test_screen_owner.py.

The test body is the X11 module's, run here under this directory's
native-wl_surface `xwpe` fixture (conftest.py).  Single source of truth -- see
run_x11_module.  The row starts a debug session, which works without a
terminal on the editor's input because the program's own pty is its
terminal; the live check is a line in WAYLAND-TESTING.txt."""
from conftest import run_x11_module

run_x11_module(__name__, "test_screen_owner")
