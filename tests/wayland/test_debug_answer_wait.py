"""Native-Wayland twin of tests/x11/test_debug_answer_wait.py.

The test body is the X11 module's, run here under this directory's
native-wl_surface `xwpe` fixture (conftest.py): the keys typed while gdb
answers at a stop must reach the editor through the Wayland event loop as
they do through X11's.  Single source of truth -- see run_x11_module."""
from conftest import run_x11_module

run_x11_module(__name__, "test_debug_answer_wait")
