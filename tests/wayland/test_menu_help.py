"""Native-Wayland twin of tests/x11/test_menu_help.py.

The test bodies are the X11 module's, run here under this directory's
native-wl_surface `xwpe` fixture (conftest.py) so the Wayland input/render path
is held to identical behaviour.  Single source of truth -- see run_x11_module."""
from conftest import run_x11_module

run_x11_module(__name__, "test_menu_help")
