"""Native-Wayland twin of tests/x11/test_selection_keys.py.

The test body is the X11 module's, run here under this directory's
native-wl_surface `xwpe` fixture (conftest.py).  Single source of truth -- see
run_x11_module.  Alt-Insert pastes and Alt-Delete copies in the editor, and
Alt-Insert or an Alt middle click pastes into a dialog's text field, without
ending the editor."""
from conftest import run_x11_module

run_x11_module(__name__, "test_selection_keys")
