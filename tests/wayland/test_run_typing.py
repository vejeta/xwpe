"""Native-Wayland twin of tests/x11/test_run_typing.py.

The test body is the X11 module's, run here under this directory's
native-wl_surface `xwpe` fixture (conftest.py).  Single source of truth -- see
run_x11_module."""
from conftest import run_x11_module

run_x11_module(__name__, "test_run_typing")
