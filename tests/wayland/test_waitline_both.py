"""Native-Wayland twin of tests/x11/test_waitline_both.py.

The test body is the X11 module's, run here under this directory's
native-wl_surface `xwpe` fixture (conftest.py): a build and a starting debug
adapter keep both waiting lines, and each Esc ends one, through the Wayland
event loop and repaint as through X11's.  Single source of truth -- see
run_x11_module."""
from conftest import run_x11_module

run_x11_module(__name__, "test_waitline_both")
