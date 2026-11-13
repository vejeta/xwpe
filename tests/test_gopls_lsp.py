"""Go IDE features via the LSP bridge (gopls), in wpe.

The editor-bridge counterpart to tests/test_lsp_gopls.c (which tests the engine):
opening a .go is recognised as an LSP-backed language (the status bar advertises
"gopls"), and Alt-Q E starts gopls and surfaces its status/diagnostics.  Covers
the we_debug.c glue the engine test bypasses: extension -> language -> server,
the eager start, the status-bar label, and the Alt-Q dispatch.

A go.mod is written so gopls loads the file as a module (full features).
Skips when gopls (or go) is absent.
"""
import shutil
import time

import pytest

from wpe_driver import WpeSession

# `undefinedName` guarantees a gopls diagnostic.
PROG = (
    "package main\n"
    "\n"
    "import \"fmt\"\n"
    "\n"
    "func add(a, b int) int {\n"
    "\treturn a + b\n"
    "}\n"
    "\n"
    "func main() {\n"
    "\ttotal := add(2, 3)\n"
    "\tfmt.Println(total)\n"
    "\t_ = undefinedName\n"
    "}\n"
)

ALT_Q = "\033q"     # the LSP prefix (Alt-Q)

pytestmark = pytest.mark.skipif(
    shutil.which("gopls") is None or shutil.which("go") is None,
    reason="gopls and go are required")


def _text(w):
    return "\n".join(w.display())


def test_gopls_lsp_bridge(tmp_path):
    """Opening a .go is recognised (the bar names gopls), and Alt-Q E starts
    gopls -- the editor bridge wired end to end."""
    # gopls wants a module: give it a go.mod so it loads with full features.
    (tmp_path / "go.mod").write_text("module demo\n\ngo 1.21\n")

    with WpeSession(str(tmp_path), PROG, filename="main.go", wait=2.0) as w:
        time.sleep(1.0)
        w._drain(1.0)
        # extension -> language -> server: the bar advertises gopls.
        assert "gopls" in _text(w), \
            "status bar should advertise the gopls LSP for a .go file:\n%s" % _text(w)

        # Alt-Q E starts gopls and reports its status (cold start can index stdlib).
        w.key(ALT_Q, delay=0.4)
        w.key("e", delay=15.0)
        time.sleep(2.0)
        w._drain(3.0)
        assert w.alive(), "wpe died starting gopls"
        text = _text(w)
        assert ("LSP:" in text or "error" in text or "Language server" in text), \
            "no gopls diagnostics/status in Messages:\n%s" % text


WIDE_PROG = PROG.replace("\t_ = undefinedName\n",
                         "    _ = \"caf\u00e9 \U0001f600\" + undefinedName\n")
WIDE_CURSOR = (21, 13)      # screen column and row of `undefinedName`


def test_gopls_keeps_utf16_and_the_jump_lands_on_the_symbol(tmp_path):
    """The editor offers utf-8 and utf-16; gopls ignores the offer and counts
    in utf-16 (the trace says so), and Alt-Q . still puts the cursor on
    `undefinedName` after an accented letter and an emoji on its line."""
    (tmp_path / "go.mod").write_text("module demo\n\ngo 1.21\n")
    trace = tmp_path / "lsp-trace.log"
    with WpeSession(str(tmp_path), WIDE_PROG, filename="main.go", wait=2.0,
                    env_extra={"XWPE_LSP_TRACE": str(trace)}) as w:
        time.sleep(1.0)
        w._drain(1.0)
        w.key(ALT_Q, delay=0.4)
        w.key("e", delay=15.0)
        time.sleep(2.0)
        w._drain(3.0)
        assert w.alive(), "wpe died starting gopls"
        w.key(ALT_Q, delay=0.4)
        w.key(".", delay=2.5)
        w._drain(0.8)
        text = trace.read_text(errors="replace")
        assert '"positionEncodings": [ "utf-8", "utf-16" ]' in text, \
            "initialize should offer the encodings:\n%s" % text[:2000]
        assert "positionEncoding=absent -> utf-16" in text, \
            "gopls should stay on utf-16:\n%s" % text[:2000]
        assert (w.screen.cursor.x, w.screen.cursor.y) == WIDE_CURSOR, \
            "the cursor should be on `undefinedName` at %s, is at %s:\n%s" \
            % (WIDE_CURSOR, (w.screen.cursor.x, w.screen.cursor.y), _text(w))
