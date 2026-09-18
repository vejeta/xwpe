"""AI assistant -- Chat INVESTIGATES the workspace (read-only tools).

The chat can read files it is not currently looking at, so "what does file X
say?" works instead of "I only have the open file".  Needs a real model that
can follow the tool protocol; self-skips unless Ollama is reachable.
"""
import os
import json
import subprocess
import urllib.request
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

OLLAMA = "http://localhost:11434"
PREFERRED = ["qwen2.5-coder:32b", "deepseek-coder:33b", "codellama:34b",
             "qwen3.8:27b", "gemma4:31b"]


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"WORKSPACE FILES:" in out
    except Exception:
        return False


def _ollama_model():
    try:
        with urllib.request.urlopen(OLLAMA + "/api/tags", timeout=5) as r:
            names = [m["name"] for m in json.load(r).get("models", [])]
        for want in PREFERRED:
            if want in names:
                return want
        return names[0] if names else None
    except Exception:
        return None


MODEL = _ollama_model()
pytestmark = [
    pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai"),
    pytest.mark.skipif(MODEL is None, reason="no local Ollama/model"),
]


def test_chat_reads_another_file(tmp_path):
    # a marker only discoverable by READING a file the chat is not looking at
    (tmp_path / "notes.md").write_text("project notes\nSECRETWORD=BANANA42\nend\n")
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "ollama",
           "XWPE_AI_ENDPOINT": OLLAMA, "XWPE_AI_MODEL": MODEL}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    filename="stack.c", env_extra=env) as s:
        s.key(ALT.AI); s.key("a", delay=0.8)
        s.key("Read the file notes.md and tell me the value of SECRETWORD.")
        s.key("\r", delay=2.0)
        found = False
        for _ in range(40):
            s._drain(2.0)
            if "BANANA42" in "\n".join(s.display()):
                found = True
                break
    assert found, "chat could not read the other file / find its secret"
