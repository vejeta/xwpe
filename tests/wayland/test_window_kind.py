"""Native-Wayland twin of tests/x11/test_window_kind.py.

The test bodies are the X11 module's, run here under this directory's
native-wl_surface `xwpe` fixture (conftest.py) so the Wayland key wait is
held to the same behaviour: a build that ends while the editor waits for a
key says so with a code of its own, and the key that follows belongs to the
window the user is looking at.  Single source of truth -- see
run_x11_module."""
from conftest import run_x11_module

run_x11_module(__name__, "test_window_kind")
