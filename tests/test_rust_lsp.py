"""Rust IDE features via the LSP bridge (rust-analyzer), in wpe.

The editor-bridge counterpart to tests/test_lsp_rust.c (which tests the engine):
opening a .rs is recognised as an LSP-backed language (the status bar advertises
the rust server) and Alt-Q E starts it.  Also the regression guard for the bar
layout: "rust-analyzer" is too wide for the 80-column button row, so a concise
"Alt-Q ? rust" label is used and the row repacks to keep every button (Quit
included) whole.

A Cargo.toml + src/main.rs are written so rust-analyzer loads a crate.
Skips when rust-analyzer (or cargo) is absent.  rust-analyzer indexes std on
first run, so this is the slowest bridge test.
"""
import shutil
import time

import pytest

from wpe_driver import WpeSession

PROG = (
    "fn add(a: i32, b: i32) -> i32 {\n"
    "    a + b\n"
    "}\n"
    "\n"
    "fn main() {\n"
    "    let total = add(2, 3);\n"
    "    println!(\"{}\", total);\n"
    "}\n"
)

ALT_Q = "\033q"     # the LSP prefix (Alt-Q)

pytestmark = pytest.mark.skipif(
    shutil.which("rust-analyzer") is None or shutil.which("cargo") is None,
    reason="rust-analyzer and cargo are required")


def _text(w):
    return "\n".join(w.display())


def test_rust_lsp_bridge(tmp_path):
    """Opening a .rs (in a crate) names rust-analyzer on the bar -- and the long
    label keeps the whole button row on screen -- and Alt-Q E starts it."""
    (tmp_path / "Cargo.toml").write_text(
        "[package]\nname = \"demo\"\nversion = \"0.1.0\"\nedition = \"2021\"\n")
    src = tmp_path / "src"
    src.mkdir()
    (src / "main.rs").write_text(PROG)

    # open the file from inside the crate (filename relative to workdir=tmp_path)
    with WpeSession(str(tmp_path), PROG, filename="src/main.rs", wait=2.0) as w:
        time.sleep(1.0)
        w._drain(1.0)
        # extension -> language -> server: the bar advertises the rust server.
        # The bar label is kept concise ("Alt-Q ? rust"); "rust-analyzer" itself
        # is too wide for the 80-column button row, so a short label is used and
        # the row repacks to keep every button -- including Quit -- whole.
        bar = w.display()[-1]
        assert "Alt-Q ? rust" in bar, \
            "status bar should name the rust server for a .rs file:\n%r" % bar
        assert "Alt-X Quit" in bar, \
            "the button row clipped a button (Quit not whole):\n%r" % bar

        # Alt-Q E starts rust-analyzer (cold start indexes std -- give it time).
        w.key(ALT_Q, delay=0.4)
        w.key("e", delay=20.0)
        time.sleep(3.0)
        w._drain(3.0)
        assert w.alive(), "wpe died starting rust-analyzer"
        text = _text(w)
        assert ("LSP:" in text or "error" in text or "Language server" in text), \
            "no rust-analyzer status in Messages:\n%s" % text
