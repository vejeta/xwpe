"""Native-Wayland twin of tests/x11/test_screen_owner.py.

The test body is the X11 module's, run here under this directory's
native-wl_surface `xwpe` fixture (conftest.py).  Single source of truth -- see
run_x11_module.  The row starts a debug session, which works without a
terminal on the editor's input because the program's own pty is its
terminal; the live check is a line in WAYLAND-TESTING.txt."""
from conftest import incoherence, run_x11_module

run_x11_module(__name__, "test_screen_owner")

# The User Screen on Wayland: Alt-F5 with program output ends the editor
# (SIGSEGV), because the screen's colours are read from an X display that a
# Wayland editor does not have.  The row stays, to turn green with the fix.
test_stop_under_the_user_screen_waits_for_the_key = incoherence(
    "Alt-F5 ends the editor on Wayland: the User Screen reads its colours "
    "from the X display")(test_stop_under_the_user_screen_waits_for_the_key)
