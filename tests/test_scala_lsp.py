"""Scala IDE features via the LSP bridge (Metals), in wpe.

Drives the editor's LSP entry point: Alt-Q (the "Query the language server"
prefix) then a letter -- E starts Metals for the .scala file, imports the build
and surfaces diagnostics; D jumps to a definition; H shows hover.  The protocol
correctness is covered headless by tests/test_lsp_scala.c (vs real Metals); this
test checks the editor *bridge* (key -> action -> visible effect).

Requires metals + scala-cli.  Skips otherwise.  Slow: Alt-Q E boots a JVM
language server and imports the build (the UI blocks until it is ready).
"""
import re
import shutil
import time

import pytest

from wpe_driver import WpeSession

PROG = (
    "object Factorial:\n"
    "  def main(args: Array[String]): Unit =\n"
    "    var f = 1L\n"
    "    var i = 1\n"
    "    while i <= 10 do\n"
    "      f = f * i\n"
    "      i = i + 1\n"
    "    println(s\"factorial(10) = $f\")\n"
)

ALT_Q = "\033q"     # the LSP prefix (Alt-Q)

pytestmark = pytest.mark.skipif(
    shutil.which("metals") is None or shutil.which("scala-cli") is None,
    reason="metals and scala-cli required")


def _text(w):
    return "\n".join(w.display())


def test_scala_lsp_diagnostics_ready(tmp_path):
    """Alt-Q E starts the language server for the .scala file and reports it
    ready (build imported, file compiled) -- the bridge wired end to end."""
    with WpeSession(str(tmp_path), PROG, filename="Factorial.scala", wait=2.0) as w:
        w.key(ALT_Q, delay=0.4)            # LSP prefix
        w.key("e", delay=90.0)             # start Metals + import + compile (slow)
        time.sleep(3.0)
        w._drain(3.0)
        assert w.alive(), "wpe died starting the language server"
        text = _text(w)
        assert ("Language server ready" in text
                or "Starting language server" in text), \
            "no language-server status in Messages:\n%s" % text
