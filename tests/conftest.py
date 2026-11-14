"""Settings every test session shares.

A test starts the editor in an empty HOME, and an empty HOME is a new profile,
which the editor asks which keys it wants.  Nobody answers there, so a session
runs the Borland keys unless a row says otherwise: XWPE_KEYS=borland is in the
environment of every editor a test starts.  The two drivers (wpe_driver.py,
gui_owner.py) also set it themselves; this covers the tests that start the
editor on their own.  A row that asks the first-run question, or runs another
style, removes or changes the variable in its own environment (test_first_run.py,
test_keys_session.py).
"""
import os

os.environ.setdefault("XWPE_KEYS", "borland")
