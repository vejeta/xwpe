"""The AI assistant sees the current file's live diagnostics.

So "fix this" works without pasting the error: the language server's diagnostics
for the open file are folded into the prompt (Chat/Edit/Agent).  This opens a C
file with syntax errors, starts clangd (Alt-Q H), waits for its diagnostics, then
opens the chat and asserts the prompt carried them (XWPE_AI_TRACE
"prompt diagnostics=N").  Uses the mock AI backend -- the diagnostics are added
regardless of backend -- so no model/network is needed.  Skips without clangd.
"""
import os
import time
import shutil
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = [
    pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai"),
    pytest.mark.skipif(shutil.which("clangd") is None, reason="no clangd"),
]


def test_chat_prompt_includes_diagnostics(tmp_path):
    (tmp_path / "bad.c").write_text("int main(void){ int x = ; return 0 }\n")
    ai = tmp_path / "ai.trace"
    lsp = tmp_path / "lsp.trace"
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_MOCK_REPLY": "ok",
           "XWPE_AI_TRACE": str(ai), "XWPE_LSP_TRACE": str(lsp)}
    with WpeSession(str(tmp_path), (tmp_path / "bad.c").read_text(),
                    env_extra=env, filename="bad.c") as s:
        s._drain(0.6)
        s.key("\033q", delay=0.6); s.key("h", delay=0.6)   # Alt-Q H: start clangd
        end = time.time() + 45
        while time.time() < end:                            # wait for diagnostics
            s._drain(2.0)
            if lsp.exists() and "diagnostic" in lsp.read_text().lower():
                break
        if not (lsp.exists() and "diagnostic" in lsp.read_text().lower()):
            pytest.skip("clangd delivered no diagnostics in time")
        s.key("\033", delay=0.4)                            # close any hover popup
        s.key(ALT.AI); s.key("a", delay=0.6)
        s.key("fix the errors"); s.key("\r", delay=1.5); s._drain(1.5)
    txt = ai.read_text() if ai.exists() else ""
    assert "prompt diagnostics=" in txt, \
        "the chat prompt did not include the file's diagnostics:\n" + txt
