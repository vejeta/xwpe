"""Native-Wayland twin of tests/x11/test_keymap.py.

The test bodies are the X11 module's, run here under this directory's
native-wl_surface `xwpe` fixture (conftest.py) so the Wayland input path is
held to the same keys.  The row that needs a debug session skips under the
headless compositor; its compositor-free peer is the keysym selftest
(tests/test_wayland_keysym.py).  Single source of truth -- see run_x11_module."""
from conftest import run_x11_module

run_x11_module(__name__, "test_keymap")
