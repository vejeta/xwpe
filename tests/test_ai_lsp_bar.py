"""The AI assistant must NOT hide the LSP bottom-bar hint.

Before the fix, enabling AI made the "Alt-G AI" bar replace the "Alt-Q ? <server>"
LSP bar on every language-server file, so the LSP action menu lost its on-screen
hint (Alt-Q still worked, but was undiscoverable).  LSP and AI are meant to be
usable at the same time, so a file that has BOTH a language server AND AI enabled
must show BOTH hints on a combined bar.  These assert the bottom line for each
file-type x AI-toggle combination.

Uses the mock backend (no network) and relies on clangd being a CONFIGURED server
for C (it is, in the server table) -- the bar hint is config-based, not gated on
the server binary being installed, so this is deterministic on any box.
"""
import os
import subprocess
import pytest
from wpe_driver import WpeSession, WPE_BIN


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _bottom(tmp_path, ai_on, ext):
    env = {"XWPE_AI_ENABLE": "1" if ai_on else "0", "XWPE_AI_BACKEND": "mock"}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, filename="t." + ext) as s:
        s._drain(0.6)
        return s.display()[-1]


def test_lsp_file_with_ai_shows_both_hints(tmp_path):
    # A C file (clangd configured) with AI on: the LSP hint is NOT lost.
    bar = _bottom(tmp_path, ai_on=True, ext="c")
    assert "Alt-Q" in bar, "LSP hint lost when AI is on:\n" + bar
    assert "Alt-G" in bar, "AI hint missing on a combined bar:\n" + bar


def test_lsp_file_without_ai_shows_lsp_only(tmp_path):
    bar = _bottom(tmp_path, ai_on=False, ext="c")
    assert "Alt-Q" in bar, "LSP hint missing on an LSP file:\n" + bar
    assert "Alt-G" not in bar, "AI hint shown while AI is off:\n" + bar


def test_plain_file_with_ai_shows_ai_only(tmp_path):
    # No language server for .txt: AI hint only, no phantom LSP hint.
    bar = _bottom(tmp_path, ai_on=True, ext="txt")
    assert "Alt-G" in bar, "AI hint missing on a plain file:\n" + bar
    assert "Alt-Q" not in bar, "LSP hint shown for a non-LSP file:\n" + bar
