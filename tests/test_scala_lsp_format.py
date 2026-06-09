"""Scala formatting via the LSP bridge (Metals/scalafmt), in wpe.

This exercises the buffer rebuild (e_lsp_replace_buffer) that applies the
server's edits back into the editor -- the file-mutating path shared by format
and rename.  The edit-application logic itself is unit-tested against real
Metals in tests/test_lsp_scala.c; this checks the editor side rebuilds the
buffer correctly (no corruption).  Format needs no cursor movement, so it is a
clean way to trigger the rebuild.

Requires metals + scala-cli.  Skips otherwise.  Slow (boots Metals).
"""
import shutil
import time

import pytest

from wpe_driver import WpeSession

# Deliberately misformatted: extra spaces scalafmt collapses.
PROG = (
    "object Factorial:\n"
    "  def main(args: Array[String]): Unit =\n"
    "    var f     =     1L\n"
    "    println(f)\n"
)

ALT_Q = "\033q"

pytestmark = pytest.mark.skipif(
    shutil.which("metals") is None or shutil.which("scala-cli") is None,
    reason="metals and scala-cli required")


def test_scala_lsp_format_buffer(tmp_path):
    """Alt-Q F reformats the file; the editor buffer is rebuilt with the tidy
    result (the extra spaces are gone, the buffer is intact)."""
    # a scalafmt config so Metals actually formats (no prompt, deterministic)
    (tmp_path / ".scalafmt.conf").write_text(
        'version = "3.8.1"\nrunner.dialect = scala3\n')
    with WpeSession(str(tmp_path), PROG, filename="Factorial.scala", wait=2.0) as w:
        w.key(ALT_Q, delay=0.4)
        w.key("f", delay=160.0)            # cold Metals import + scalafmt + rebuild
        time.sleep(3.0)
        w._drain(4.0)
        assert w.alive(), "wpe died applying the format"
        text = "\n".join(w.display())
        assert "var f = 1L" in text, \
            "formatted line not in the buffer after rebuild:\n%s" % text
        assert "var f     =" not in text, \
            "the misformatted spacing survived (rebuild did not apply):\n%s" % text
        assert "object Factorial" in text and "println(f)" in text, \
            "buffer looks corrupted after the rebuild:\n%s" % text
