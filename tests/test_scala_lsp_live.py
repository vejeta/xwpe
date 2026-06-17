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
ALT_1 = "\0331"    # jump to window 1 (the source: deterministic across the
                   #  number of background windows Metals may have opened --
                   #  e.g. the Doctor -- which F6's cycle would skip past)
ALT_2 = "\0332"    # jump to window 2 (Messages)

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
        w.key(ALT_1, delay=1.0)            # focus the source (window 1)

        # type a broken token at the top of the file, then a newline (-> didChange)
        for ch in "@@@":
            w.key(ch, delay=0.15)
        w.key("\r", delay=2.0)             # recompile is triggered here
        # subsequent keystrokes poll & drain the new diagnostics (non-blocking);
        # the summary is written to Messages WITHOUT stealing focus (sw=0)
        for _ in range(10):
            w.key(" ", delay=1.3)
        w.key(ALT_2, delay=1.5)            # raise Messages (window 2) to view the live status
        w._drain(2.0)
        assert w.alive(), "wpe died during live diagnostics"
        text = "\n".join(w.display())
        assert "LSP:" in text and "error(s)" in text, \
            "live error status did not appear after typing an error:\n%s" % text
