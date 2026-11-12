"""Native-Wayland twin of tests/x11/test_dialog_field_utf8.py: not drivable.

These rows type characters above Latin-1.  xdotool reaches them by remapping
a spare keycode of the X server, and weston's x11 backend keeps its own xkb
keymap, so such a key arrives as keysym 0 with no text and nothing is typed.
The Wayland side is held instead by tests/test_wayland_keysym.py: the decoder
returns the codepoint of a typed multibyte character flagged as a character,
which is all the shared dialog field needs from the backend.
"""
import pytest

pytest.skip("weston's xkb keymap drops the keysyms xdotool remaps for "
            "non-ASCII text", allow_module_level=True)
