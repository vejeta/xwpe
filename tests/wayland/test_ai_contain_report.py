"""How far the assistant's commands are contained, said under the native
Wayland backend.

The header word, the line said once and the refusal under Strict are shared
UI and shared start gate.  The rows are those of
tests/x11/test_ai_contain_report.py, run here against this directory's native
Wayland fixture, so the two suites cannot drift.  Self-skips if the graphical
harness or --enable-ai is missing.
"""
from conftest import run_x11_module

run_x11_module(__name__, "test_ai_contain_report")
