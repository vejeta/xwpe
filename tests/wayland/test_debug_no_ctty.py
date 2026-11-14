"""Native-Wayland twin of tests/x11/test_debug_no_ctty.py.

The test body is the X11 module's, run here under this directory's
native-wl_surface `xwpe` fixture (conftest.py): a debugger session must work
whether or not the editor has a controlling terminal.  Single source of
truth -- see run_x11_module."""
from conftest import run_x11_module

run_x11_module(__name__, "test_debug_no_ctty")
