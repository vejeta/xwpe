"""A stale non-Claude model must not break the Claude CLI backend.

If the config still holds a model from a previous backend (e.g. Ollama's
"qwen3:8b") while the backend is now Claude CLI, xwpe must NOT run
`claude --model qwen3:8b` -- claude rejects it with "the selected model may not
exist / you may not have access".  The model is dropped and claude uses its
login default instead.
"""
import os
import subprocess
import tempfile

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def test_stale_ollama_model_not_forwarded(tmp_path):
    argv_file = tmp_path / "claude_argv.txt"
    bindir = tmp_path / "bin"
    bindir.mkdir()
    fake = bindir / "claude"
    fake.write_text(
        "#!/bin/sh\n"
        'echo "$*" > ' + str(argv_file) + "\n"
        "echo '{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,\"result\":\"ok\"}'\n"
    )
    fake.chmod(0o755)

    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "claudecli",
        "XWPE_AI_MODEL": "qwen3.8:27b",     # stale, from an earlier Ollama session
        "PATH": str(bindir) + ":" + os.environ.get("PATH", ""),
    }
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key(ALT.AI); s.key("a")
        s.key("hi"); s.key("\r", delay=1.5)
        waited = 0.0
        while waited < 15 and not argv_file.exists():
            s._drain(1.0)
            waited += 1.0

    assert argv_file.exists(), "the claudecli backend never launched `claude`"
    argv = argv_file.read_text()
    assert "qwen3.8:27b" not in argv, "the stale non-Claude model was forwarded:\n" + argv
