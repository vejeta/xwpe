"""Scala completion via the LSP bridge (Metals), in wpe.

Alt-Q C offers completion candidates for the word under the cursor in a popup
(the dialog radio list) and inserts the chosen one.  Protocol correctness is
covered headless by tests/test_lsp_scala.c; this checks the popup appears in the
editor with candidates.

Requires metals + scala-cli.  Skips otherwise.  Slow (boots Metals).
"""
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

ALT_Q = "\033q"
CTRL_N = "\x0e"     # down a line
ARROW_R = "\033[C"  # right one character (arrow key escape: xwpe's Ctrl-D
                    #  is DELETE, not WordStar-right, so use the canonical
                    #  cursor-right sequence to avoid corrupting the buffer)

pytestmark = pytest.mark.skipif(
    shutil.which("metals") is None or shutil.which("scala-cli") is None,
    reason="metals and scala-cli required")


def test_scala_lsp_completion_popup(tmp_path):
    """Alt-Q C on the `println` line shows the completion popup with candidates."""
    with WpeSession(str(tmp_path), PROG, filename="Factorial.scala", wait=2.0) as w:
        w.key(ALT_Q, delay=0.4)
        w.key("e", delay=90.0)             # start Metals + import + compile
        time.sleep(2.0)
        w._drain(2.0)
        assert w.alive(), "wpe died starting the language server"

        # cursor to the println line (0-based line 7) and into the identifier
        for _ in range(7):
            w.key(CTRL_N, delay=0.12)
        for _ in range(11):
            w.key(ARROW_R, delay=0.05)

        w.key(ALT_Q, delay=0.4)
        w.key("c", delay=8.0)              # completion request -> popup
        w._drain(2.0)
        text = "\n".join(w.display())
        assert "Completion" in text or "println" in text, \
            "no completion popup/candidates after Alt-Q C:\n%s" % text
        w.key("\033", delay=0.5)           # dismiss the popup
