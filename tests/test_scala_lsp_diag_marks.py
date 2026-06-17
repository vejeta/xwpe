"""Inline diagnostic marks via the LSP bridge (Metals), in wpe.

publishDiagnostics ranges are not only listed in Messages -- the editor recolors
the offending cells (errors red) as the analogue of a red squiggle.  This drives
the same proven flow as test_scala_lsp_live.py (start Metals on a valid file,
type a hard parse error, let the keystroke polls drain diagnostics) and then
asserts that a source row actually carries the red error background.

Requires metals + scala-cli.  Skips otherwise.  Slow (boots Metals).
"""
import shutil

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

pytestmark = pytest.mark.skipif(
    shutil.which("metals") is None or shutil.which("scala-cli") is None,
    reason="metals and scala-cli required")


def test_scala_lsp_inline_diag_marks(tmp_path):
    """Typing a parse error makes the editor paint the offending cells red."""
    with WpeSession(str(tmp_path), PROG, filename="Factorial.scala", wait=2.0) as w:
        w.key(ALT_Q, delay=0.4)
        w.key("e", delay=150.0)            # start Metals (cold) -> "LSP: no problems"
        w._drain(2.0)
        assert w.alive()
        w.key(ALT_1, delay=1.0)            # focus the source (window 1)

        # a hard parse error at the very top of the file (-> didChange on newline)
        for ch in "@@@":
            w.key(ch, delay=0.15)
        w.key("\r", delay=2.0)             # recompile triggers here
        # keystroke polls drain the diagnostics; the summary swaps in the marks
        # and repaints the source window
        for _ in range(12):
            w.key(" ", delay=1.3)
        w._drain(2.0)
        assert w.alive(), "wpe died during inline diagnostics"

        # the "@@@" we typed sits on row 1 of the editor (row 0 is the menu bar,
        # the title border follows); scan the top source rows for a red cell.
        red_rows = [y for y in range(1, 12) if w.row_has_bg(y, "red")]
        assert red_rows, (
            "no red error cell appeared after a parse error:\n%s"
            % "\n".join(w.display()))
