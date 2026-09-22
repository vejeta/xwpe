"""Options > AI > Advanced...: the less-common settings have a dialog now.

AIModelFallback, AIEditHook (and, in a host build, AIHostCommand) are config
keys; the main AI dialog is full on a 24-line terminal, so they live behind an
"Advanced (Alt-A)..." button in their own dialog.  This opens it, types a
fallback model, confirms, and checks it was applied and saved to xwperc.
"""
import os
import subprocess
import pytest
from wpe_driver import WpeSession, WPE_BIN


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"Advanced AI settings" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def test_advanced_dialog_sets_fallback(tmp_path):
    home = str(tmp_path / "home")
    cfg = os.path.join(home, ".config", "xwpe")
    os.makedirs(cfg)
    with open(os.path.join(cfg, "xwperc"), "w") as fh:
        fh.write("[Programming]\nAIBackend : 4\n")     # claudecli: no model-list network
    env = {"XWPE_AI_ENABLE": "1", "HOME": home}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, filename="t.c") as s:
        s.key("\033o", delay=0.5); s.key("i", delay=0.7)    # Options > AI
        s.key("\033a", delay=0.7)                           # Alt-A -> Advanced
        assert "Advanced AI settings" in "\n".join(s.display()), \
            "the Advanced dialog did not open:\n" + "\n".join(s.display())
        s.key("\033m", delay=0.4)                           # focus Model fallback field
        for ch in "BACKUPMODEL9":
            s.key(ch, delay=0.03)
        s.key("\033o", delay=0.7)                           # Ok (advanced) -> apply + save
        s.key("\033", delay=0.4)                            # leave the main dialog
    txt = open(os.path.join(cfg, "xwperc")).read()
    assert "AIModelFallback : BACKUPMODEL9" in txt, \
        "the fallback model was not applied/saved:\n" + txt
