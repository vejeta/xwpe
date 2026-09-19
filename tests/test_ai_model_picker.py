"""AI model picker -- radio list of the backend's available models.

Alt-G m opens a radio dialog of the models the active backend offers (queried
live for Ollama/OpenAI, the CLI aliases for Claude), with the model in use
pre-marked; picking one sets it.  Uses claudecli so the list is deterministic
(default/sonnet/opus/haiku) and needs no network.
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


def test_claudecli_model_picker(tmp_path):
    trace = tmp_path / "ai.trace"
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "claudecli",
        "XWPE_AI_TRACE": str(trace),
    }
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key(ALT.AI)                    # AI menu
        s.key("m")                       # Pick model
        s._drain(0.6)
        disp = "\n".join(s.display())
        for want in ("default", "sonnet", "opus", "haiku"):
            assert want in disp, "model %r not offered:\n%s" % (want, disp)
        # the current model ("default") is pre-marked as the active radio
        assert any("(*) default" in ln or "(*)default" in ln.replace(" ", "  ")
                   for ln in s.display()), "current model not pre-marked:\n" + disp
        s.key("\033[B", delay=0.3)       # down -> sonnet
        s.key("\r", delay=0.6)           # confirm
        s._drain(0.4)
    txt = trace.read_text() if trace.exists() else ""
    assert "model set sonnet" in txt, "picking a model did not set it:\n" + txt
