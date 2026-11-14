"""Native-Wayland twin of tests/x11/test_terminal_owner.py: the Wayland window
writes nothing to the terminal the editor was started from, whether the end
comes from SIGTERM or from Alt-X.  The test body is shared with the X11
module."""
from conftest import run_x11_module

run_x11_module(__name__, "test_terminal_owner")
