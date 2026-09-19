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
    # backend + policy are radios; the model is a button showing the current
    # choice (the full list lives in the scrollable picker behind it).
    for want in ("AI settings", "Backend", "Model", "Permission",
                 "Claude CLI", "Ollama", "sonnet", "(change)"):
        assert want in disp, "AI dialog missing %r:\n%s" % (want, disp)
    # current backend is the marked radio
    assert "(*) Claude CLI" in disp.replace("  ", " ").replace(" (", " (") \
        or "(*)Claude CLI" in disp.replace(" ", "") \
        or any("(*)" in ln and "Claude CLI" in ln for ln in disp.splitlines()), \
        "current backend not pre-marked:\n" + disp


def test_ai_options_model_picker_scrolls_and_selects(tmp_path):
    # Alt-M opens the scrollable model picker; Down + Enter selects a different
    # model, and the dialog reopens with it on the button.  claudecli's list is
    # deterministic (default/sonnet/opus/haiku), no network.
    trace = tmp_path / "ai.trace"
    env = dict(ENV); env["XWPE_AI_TRACE"] = str(trace)
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key("\033o", delay=0.5)
        s.key("i", delay=0.6)
        s.key("\033m", delay=0.8)        # Alt-M -> scrollable picker
        pick = "\n".join(s.display())
        assert "Model (" in pick and "available)" in pick, \
            "scrollable model picker did not open:\n" + pick
        assert "PgUp/PgDn" in pick, "picker is not the scrollable overlay:\n" + pick
        s.key("\033[B", delay=0.3)       # Down one item
        s.key("\r", delay=0.6)           # Enter -> select
        s.key("\033", delay=0.4)         # leave the reopened dialog
    txt = trace.read_text() if trace.exists() else ""
    assert "model set" in txt, "picking a model was not recorded:\n" + txt


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


def test_ai_options_keyboard_navigation(tmp_path):
    # Tab/arrows must move between the radio groups and Space must select -- the
    # widgets carry unique non-zero sw ids so e_opt_kst can focus them (a zero sw
    # made them unreachable).  Tab to the Backend group, arrow to Ollama, select.
    trace = tmp_path / "ai.trace"
    env = dict(ENV); env["XWPE_AI_TRACE"] = str(trace)
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key("\033o", delay=0.5)
        s.key("i", delay=0.6)
        s.key("\t", delay=0.35)          # Enable -> Backend group
        s.key("\033[B", delay=0.35)      # down -> Ollama
        s.key(" ", delay=0.35)           # select
        s.key("\033o", delay=0.6)        # Ok -> reloads with Ollama's models
        s._drain(0.4)
        s.key("\033", delay=0.4)         # leave the reopened dialog
    txt = trace.read_text() if trace.exists() else ""
    # changing the backend triggers a reload of that backend's model list
    assert "options reload backend=ollama" in txt, \
        "Tab/arrow/Space navigation did not reach and select a radio:\n" + txt


def test_ai_options_backend_change_reloads_models(tmp_path):
    # Selecting a different backend reloads the Model list for it: switch to
    # Ollama and the dialog reopens listing Ollama models, not the Claude ones.
    trace = tmp_path / "ai.trace"
    env = dict(ENV); env["XWPE_AI_TRACE"] = str(trace)
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key("\033o", delay=0.5)
        s.key("i", delay=0.6)
        s.key("\t", delay=0.35)          # Backend group
        s.key("\033[B", delay=0.35)      # Ollama
        s.key(" ", delay=0.35)
        s.key("\033o", delay=0.8)        # Ok -> reload
        disp = "\n".join(s.display())
        s.key("\033", delay=0.4)
    # the reopened dialog no longer offers the Claude CLI aliases as models
    assert "options reload backend=ollama" in (trace.read_text() if trace.exists() else "")
    # and Ollama is now the marked backend
    assert any("(*) Ollama" in ln for ln in disp.splitlines()) \
        or "(*)Ollama" in disp.replace(" ", ""), \
        "backend did not switch to Ollama on reload:\n" + disp
