"""The assistant's write asks about a file with several names, and about a file
replaced under its window, under the native Wayland backend.

The two questions are the marked write box, shared UI.  The rows are those of
tests/x11/test_ai_agent_write.py, run here against this directory's native
Wayland fixture, so the two suites cannot drift.  Self-skips if the graphical
harness or --enable-ai is missing.
"""
from conftest import run_x11_module

run_x11_module(__name__, "test_ai_agent_write")
