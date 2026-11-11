"""Native-Wayland twin of tests/x11/test_breakpoint_hollow.py.

The test bodies are the X11 module's, run here under this directory's
native-wl_surface `xwpe` fixture (conftest.py): the unconfirmed-breakpoint
colour is one entry of the colour scheme, drawn by the same line painter for
every backend, and the Wayland renderer must show it the same way.  The row
that starts a debug session runs too, as does the colour dialog's sample row.
Single source of truth -- see run_x11_module."""
from conftest import run_x11_module

run_x11_module(__name__, "test_breakpoint_hollow")
