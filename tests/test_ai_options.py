"""Options -> AI: the AI settings dialog (enable + backend/model/policy radios).

The dialog is the config home the user chose.  It shows radios for the backend,
the backend's live model list and the permission policy -- each marking the
value in use -- plus an Enable checkbox.  Ok applies the choices to the session
(Save Options persists).  Uses claudecli so the model list is deterministic.
"""
import os
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"AI settings" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")

ENV = {
    "XWPE_AI_ENABLE": "1",
    "XWPE_AI_BACKEND": "claudecli",
    "XWPE_AI_MODEL": "sonnet",
    "XWPE_AI_POLICY": "ask",
}


def test_ai_options_dialog_renders_current(tmp_path):
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=dict(ENV)) as s:
        s.key("\033o", delay=0.5)        # Options menu
        assert "AI" in "\n".join(s.display()), "no AI entry in Options"
        s.key("i", delay=0.6)            # AI -> dialog
        disp = "\n".join(s.display())
        s.key("\033", delay=0.3)
    for want in ("AI settings", "Backend", "Model", "Permission",
                 "Claude CLI", "Ollama", "sonnet", "opus"):
        assert want in disp, "AI dialog missing %r:\n%s" % (want, disp)
    # current backend/model/policy are the marked radios
    assert "(*) Claude CLI" in disp.replace("  ", " ").replace(" (", " (") \
        or "(*)Claude CLI" in disp.replace(" ", "") \
        or any("(*)" in ln and "Claude CLI" in ln for ln in disp.splitlines()), \
        "current backend not pre-marked:\n" + disp


def test_ai_options_ok_reads_radios(tmp_path):
    trace = tmp_path / "ai.trace"
    env = dict(ENV); env["XWPE_AI_TRACE"] = str(trace)
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key("\033o", delay=0.5)
        s.key("i", delay=0.6)
        s.key(" ", delay=0.3)            # toggle Enable (focused first)
        s.key("\033o", delay=0.6)        # Ok
        s._drain(0.4)
    txt = trace.read_text() if trace.exists() else ""
    line = next((l for l in txt.splitlines() if l.startswith("options ")), "")
    assert "backend=claudecli" in line and "model=sonnet" in line \
        and "policy=ask" in line and "enable=1" in line, \
        "AI dialog did not apply the settings:\n" + txt
