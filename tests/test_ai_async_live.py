"""AI assistant -- Edit runs in the BACKGROUND (needs a real, slow-ish model).

Proves the anti-clunky contract: while the model generates an Edit reply the
editor is NOT blocked -- the spinner animates, typing lands in the file, and
Alt-B cancels the running task.  Self-skips unless Ollama is reachable.
"""
import os
import re
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
        return b"(async)" in out and b"Alt-B AI" in out
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


def _spin_secs(disp):
    m = re.search(r"working .  (\d+)s", disp)
    return int(m.group(1)) if m else -1


def test_ai_edit_is_background(tmp_path):
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "ollama",
           "XWPE_AI_ENDPOINT": OLLAMA, "XWPE_AI_MODEL": MODEL}
    with WpeSession(str(tmp_path), "AAA\n", filename="stack.c",
                    env_extra=env) as s:
        s.key(ALT.BLOCK); s.key("e", delay=0.8)
        s.key("append a trailing comment line")
        s.key("\r", delay=2.0)                 # async edit starts; model warming
        sp0 = _spin_secs("\n".join(s.display()))
        # the editor must accept keystrokes while the model generates
        s.key("Z"); s.key("Z"); s.key("Z")
        s._drain(2.0)
        disp = "\n".join(s.display())
        sp1 = _spin_secs(disp)
        assert sp1 > sp0 >= 0, "spinner did not advance (editor loop stalled):\n" + disp
        assert "ZZZ" in disp.replace(" ", ""), \
            "typing did not land while the AI generated (editor was blocked):\n" + disp
        # one-key cancel, no waiting out the whole 32B generation
        s.key(ALT.BLOCK); s._drain(1.0)
        assert "cancelled" in "\n".join(s.display())
