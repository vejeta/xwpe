"""Python IDE features via the LSP bridge (pyright or pylsp), in wpe.

The editor-bridge counterpart to tests/test_lsp_python.c (which tests the
engine): opening a .py is recognised as an LSP-backed language (the status bar
advertises the active Python server -- pyright preferred, pylsp fallback), and
Alt-Q E starts it and surfaces diagnostics in Messages.  Covers the we_debug.c
glue the engine test bypasses: extension -> language -> server selection, the
eager start, the status-bar label, and the Alt-Q dispatch.

Skips when neither pyright-langserver nor pylsp is on PATH.
"""
import shutil
import time

import pytest

from wpe_driver import WpeSession

# `undefined_name` guarantees a diagnostic from either server.
PROG = (
    "def add(a, b):\n"
    "    return a + b\n"
    "\n"
    "def main():\n"
    "    total = add(2, 3)\n"
    "    print(total)\n"
    "    return undefined_name\n"
    "\n"
    "main()\n"
)

ALT_Q = "\033q"     # the LSP prefix (Alt-Q)

pytestmark = pytest.mark.skipif(
    shutil.which("pyright-langserver") is None and shutil.which("pylsp") is None,
    reason="a Python language server (pyright or pylsp) is required")


def _server_label():
    return "pyright" if shutil.which("pyright-langserver") else "pylsp"


def _text(w):
    return "\n".join(w.display())


def test_python_lsp_bridge(tmp_path):
    """Opening a .py is recognised (the bar names the active server), and Alt-Q E
    starts it and surfaces diagnostics -- the editor bridge wired end to end."""
    label = _server_label()       # the editor prefers pyright; pylsp is fallback
    with WpeSession(str(tmp_path), PROG, filename="demo.py", wait=2.0) as w:
        time.sleep(1.0)
        w._drain(1.0)
        # extension -> language -> server: the bar advertises whichever Python
        # server backs this file (proves e_lsp_lang_for + e_lsp_server_for/label).
        assert label in _text(w), \
            "status bar should advertise the %s LSP for a .py file:\n%s" \
            % (label, _text(w))

        # Alt-Q E starts the server and reports diagnostics.
        w.key(ALT_Q, delay=0.4)
        w.key("e", delay=12.0)
        time.sleep(2.0)
        w._drain(3.0)
        assert w.alive(), "wpe died starting the Python language server"
        text = _text(w)
        assert ("LSP:" in text or "error" in text or "Language server" in text), \
            "no Python diagnostics/status in Messages:\n%s" % text
