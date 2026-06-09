"""Live (as-you-type) diagnostics via the LSP bridge (Metals), in wpe.

Each editor change is pushed to the server (didChange, debounced on newline) and
e_lsp_poll drains diagnostics non-blocking on each keystroke, so the
"LSP: N error(s)" status updates live without an explicit action or a save.

Requires metals + scala-cli.  Skips otherwise.  Slow (boots Metals).
"""
import shutil
import time

import pytest

from wpe_driver import WpeSession

PROG = (
    "object Factorial:\n"
    "  def main(args: Array[String]): Unit =\n"
    "    println(1)\n"
)

ALT_Q = "\033q"
F6 = "\033[17~"    # switch window (Messages -> source)

pytestmark = pytest.mark.skipif(
    shutil.which("metals") is None or shutil.which("scala-cli") is None,
    reason="metals and scala-cli required")


def test_scala_lsp_live_diagnostics(tmp_path):
    """After starting the server on a valid file, typing an error makes the
    live LSP status report it -- no explicit diagnostics action."""
    with WpeSession(str(tmp_path), PROG, filename="Factorial.scala", wait=2.0) as w:
        w.key(ALT_Q, delay=0.4)
        w.key("e", delay=150.0)            # start Metals (cold) -> "LSP: no problems"
        w._drain(2.0)
        assert w.alive()
        w.key(F6, delay=1.0)               # focus moved to Messages; go back to source

        # type a broken token at the top of the file, then a newline (-> didChange)
        for ch in "@@@":
            w.key(ch, delay=0.15)
        w.key("\r", delay=2.0)             # recompile is triggered here
        # subsequent keystrokes poll & drain the new diagnostics (non-blocking);
        # the summary is written to Messages WITHOUT stealing focus (sw=0)
        for _ in range(10):
            w.key(" ", delay=1.3)
        w.key(F6, delay=1.5)               # raise Messages to view the live status
        w._drain(2.0)
        assert w.alive(), "wpe died during live diagnostics"
        text = "\n".join(w.display())
        assert "LSP:" in text and "error(s)" in text, \
            "live error status did not appear after typing an error:\n%s" % text
