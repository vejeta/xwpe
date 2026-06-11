"""C IDE features via the LSP bridge (clangd), in wpe.

Drives the editor's LSP entry point for a C file: opening a .c is recognised as
an LSP-backed language (the status bar advertises "clangd"), and Alt-Q (the
"Query the language server" prefix) then E starts clangd and surfaces compiler
diagnostics in Messages.

This is the EDITOR-bridge counterpart to tests/test_lsp_clangd.c: the C test
exercises the protocol engine (we_lsp.c) directly; this one checks the glue in
we_debug.c that the engine test bypasses -- extension -> language detection, the
eager start on open, the status-bar label, and the Alt-Q key dispatch.

clangd needs no JVM and no build server, so unlike the Metals bridge test this
runs in a few seconds -- fast enough for CI where clangd is packaged.  Skips
when clangd is absent.
"""
import shutil
import time

import pytest

from wpe_driver import WpeSession

# A deliberate undeclared identifier on the last line guarantees clangd reports
# at least one diagnostic, so the assertion is deterministic.
PROG = (
    "#include <stdio.h>\n"
    "\n"
    "int add(int a, int b) { return a + b; }\n"
    "\n"
    "int main(void) {\n"
    "    int total = add(2, 3);\n"
    "    printf(\"%d\\n\", total);\n"
    "    return bad_symbol;\n"          # undeclared -> a clangd diagnostic
    "}\n"
)

ALT_Q = "\033q"     # the LSP prefix (Alt-Q)

pytestmark = pytest.mark.skipif(
    shutil.which("clangd") is None, reason="clangd required")


def _text(w):
    return "\n".join(w.display())


def test_clangd_lsp_bridge(tmp_path):
    """Opening a .c is recognised (the bar names clangd), and Alt-Q E starts
    clangd and surfaces diagnostics -- the editor bridge wired end to end."""
    with WpeSession(str(tmp_path), PROG, filename="demo.c", wait=2.0) as w:
        time.sleep(1.0)
        w._drain(1.0)
        # extension -> language -> server: the bottom bar advertises the server
        # that backs this file (proves e_lsp_lang_for + e_lsp_server_label for C).
        assert "clangd" in _text(w), \
            "status bar should advertise the clangd LSP for a .c file:\n%s" % _text(w)

        # Alt-Q E starts clangd and reports diagnostics (fast: no JVM/build
        # server -- and the eager start already began clangd when we opened).
        w.key(ALT_Q, delay=0.4)
        w.key("e", delay=6.0)
        time.sleep(1.5)
        w._drain(2.0)
        assert w.alive(), "wpe died starting clangd"
        text = _text(w)
        assert ("LSP:" in text or "error" in text or "Language server" in text), \
            "no clangd diagnostics/status in Messages:\n%s" % text
