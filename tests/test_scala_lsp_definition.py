"""Go-to-definition through the editor bridge (Metals), in wpe.

This is the end-to-end counterpart to test_lsp_scala.c (which tests the engine):
it drives the real Alt-Q path with the cursor on a real symbol and checks the
editor actually JUMPS to the definition.  It is the regression guard for the
"No definition found on a freshly opened file" bug, where the bridge sent a
needless didChange before the request, bumping the document version so Metals'
symbol index (used by definition/implementation/typeDefinition) had nothing for
the new version yet.

Cursor positioning uses the editor's own Find (Alt-S, f) -- arrow-key escapes are
not decoded as movement under the pyte pty, so we never rely on them.

Requires metals + scala-cli.  Skips otherwise.  Slow (boots Metals).
"""
import shutil

import pytest

from wpe_driver import WpeSession

# use of `greeting` on line 1, its definition on line 2 (same object)
PROG = (
    "object Demo:\n"
    "  val z = greeting(\"x\")\n"
    "  def greeting(n: String): String = n\n"
)

ALT_Q = "\033q"
ALT_S = "\033s"
F6 = "\033[17~"    # switch window (Messages -> source)

pytestmark = pytest.mark.skipif(
    shutil.which("metals") is None or shutil.which("scala-cli") is None,
    reason="metals and scala-cli required")


def _find(w, term):
    """Position the cursor on the first match of `term` via Search > Find."""
    w.key(ALT_S, delay=0.6)            # Search menu
    w.key("f", delay=0.6)              # Find -> dialog, focus in the text field
    for ch in term:
        w.key(ch, delay=0.05)
    w.key("\r", delay=1.0)             # OK -> jump to the match


def test_scala_lsp_go_to_definition(tmp_path):
    """With the cursor on a use of `greeting`, Alt-Q D jumps to its def line."""
    # pin a JDK the Scala compiler supports (system default may be too new)
    (tmp_path / "project.scala").write_text("//> using jvm temurin:21\n")

    with WpeSession(str(tmp_path), PROG, filename="Demo.scala", wait=2.0) as w:
        w.key(ALT_Q, delay=0.4)
        w.key("e", delay=150.0)        # start Metals (cold) + first compile
        w._drain(3.0)
        assert w.alive(), "wpe died starting Metals"
        w.key(F6, delay=1.0)           # focus moved to Messages; back to source

        # search a prefix so the cursor ends INSIDE the identifier (Find leaves
        # the cursor just past the match); first match is the use on line 1
        _find(w, "greet")
        w.key(ALT_Q, delay=0.4)
        w.key("d", delay=4.0)          # go to definition
        w._drain(2.0)
        assert w.alive(), "wpe died on go-to-definition"

        cur = w.screen.cursor
        rows = w.display()
        row = rows[cur.y] if 0 <= cur.y < len(rows) else ""
        # the cursor must have landed on the DEFINITION line, not stayed on the use
        assert "def greeting" in row, (
            "Alt-Q D did not jump to the definition (cursor row=%r)\n%s"
            % (row, "\n".join(rows)))
