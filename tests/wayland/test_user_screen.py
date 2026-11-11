"""Native-Wayland twin of tests/x11/test_user_screen.py.

The test body is the X11 module's, run here under this directory's
native-wl_surface `xwpe` fixture (conftest.py).  Single source of truth -- see
run_x11_module.  Alt-F5 shows the program's colours in the palette the
Wayland renderer paints with, a key returns to the editor, and no scrollbar
bleeds onto the program's screen."""
from conftest import run_x11_module

run_x11_module(__name__, "test_user_screen")
