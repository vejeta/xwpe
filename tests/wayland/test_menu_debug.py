"""Native-Wayland twin of the Ctrl-Break row of tests/x11/test_menu_debug.py.

The test body is the X11 module's, run here under this directory's
native-wl_surface `xwpe` fixture (conftest.py).  Single source of truth -- see
run_x11_module.  Only the Pause row is taken: the module's other debugger rows
need a debug session, which the headless compositor harness cannot start, and
this one skips for the same reason (the fixture's can_debug is False).  What
is Wayland's own, the Break and Ctrl+Pause key translation, is asserted
without a compositor by tests/test_wayland_keysym.py; the live check is a line
in WAYLAND-TESTING.txt."""
from conftest import run_x11_module

run_x11_module(__name__, "test_menu_debug")

for _name in [n for n in list(globals())
              if n.startswith("test_") and n != "test_ctrl_break_pauses"]:
    del globals()[_name]
