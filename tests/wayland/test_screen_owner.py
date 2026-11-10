"""Native-Wayland twin of tests/x11/test_screen_owner.py.

The test body is the X11 module's, run here under this directory's
native-wl_surface `xwpe` fixture (conftest.py).  Single source of truth -- see
run_x11_module.  The row needs a debug session, which the headless compositor
harness cannot start (the fixture's can_debug is False), so it skips here, as
the debugger rows of test_menu_debug.py do; the live check is a line in
WAYLAND-TESTING.txt."""
from conftest import run_x11_module

run_x11_module(__name__, "test_screen_owner")
