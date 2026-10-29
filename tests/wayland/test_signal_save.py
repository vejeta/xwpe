"""Native-Wayland twin of tests/x11/test_signal_save.py: SIGTERM ends the
Wayland window the same way, with the unsaved work written to .ESV.  The
test body is shared with the X11 module."""
from conftest import run_x11_module

run_x11_module(__name__, "test_signal_save")
