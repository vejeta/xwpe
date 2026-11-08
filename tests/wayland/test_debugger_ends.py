"""Native-Wayland twin of tests/x11/test_debugger_ends.py.

The test body is the X11 module's, run here under this directory's
native-wl_surface `xwpe` fixture (conftest.py): the editor's end during a
debug session must leave no debugger behind under Wayland as under X11.
The row needs a debug session and skips here while the headless compositor
harness cannot start one, as the other debugger rows do.  Single source of
truth -- see run_x11_module."""
from conftest import run_x11_module

run_x11_module(__name__, "test_debugger_ends")
