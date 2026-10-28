"""Native-Wayland twin of tests/x11/test_signal_threads.py: the Wayland
window renders with the same Pango font map, so its font thread must keep
SIGCHLD blocked too.  The test body is shared with the X11 module."""
from conftest import run_x11_module

run_x11_module(__name__, "test_signal_threads")
