"""AI chat: the model is told which backend/model is serving THIS turn.

Every chat turn rebuilds a system prompt that names the backend now in use, so
an identity question ("which model are you?") is answered from the truth of the
moment -- not from an earlier turn left in the shared, backend-agnostic history.
Without this, a user who switches backend mid-chat gets the previous backend's
self-description parroted back by the new one.

The system prompt is not printed to screen, so we assert on the XWPE_AI_TRACE
"chat identity backend=... model=..." line, written as the prompt is built.  The
mock backend keeps this deterministic (no live Ollama/OpenAI/network); the
backend/model reported are the active globals, so it also proves the model name
is carried through -- the key to answering "which model are you?" correctly.
Cross-backend behaviour (switch mid-chat, identity follows the new backend) is
exercised live against Ollama in the manual switch check.
"""
import os
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


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


@pytest.mark.parametrize("model,marker", [
    ("",                 "chat identity backend=mock model="),
    ("some-local-model", "chat identity backend=mock model=some-local-model"),
])
def test_ai_identity_names_active_backend(tmp_path, model, marker):
    trace = tmp_path / "ai.trace"
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MODEL": model,
        "XWPE_AI_MOCK_REPLY": "OK",
        "XWPE_AI_TRACE": str(trace),
    }
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key(ALT.AI)
        s.key("a")
        s.key("which model are you?")
        s.key("\r", delay=1.0)
        s._drain(1.0)
    txt = trace.read_text() if trace.exists() else ""
    assert marker in txt, \
        "system prompt did not name the active backend:\n" + txt
