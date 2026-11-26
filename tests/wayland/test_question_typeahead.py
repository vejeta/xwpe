"""Keys typed before a question appears are never its answer, under the native
Wayland backend.

The rows are those of tests/x11/test_question_typeahead.py, run here against
this directory's native Wayland fixture, so the two suites cannot drift.  The
paste and resize rows are terminal-only (see that module).  Self-skips if the
graphical harness or --enable-ai is missing.
"""
from conftest import run_x11_module

run_x11_module(__name__, "test_question_typeahead")
