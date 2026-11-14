"""The Wayland twin of tests/x11/test_debugger_chain_outcome.py: the same body,
run on the native Wayland backend.  See run_x11_module."""
from conftest import run_x11_module

run_x11_module(__name__, "test_debugger_chain_outcome")
