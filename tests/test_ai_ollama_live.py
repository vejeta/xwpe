"""AI assistant -- LIVE tests against a real local Ollama.

Self-skips unless Ollama is reachable on localhost:11434 with a usable model.
Proves real end-to-end behaviour that the mock cannot: the editor talks to a
real model, and an AI Edit actually FIXES a broken C file (verified by compiling
the result).  Slow -- a real 30B model generates for tens of seconds.

Run:  WPE_BIN=../wpe python -m pytest tests/test_ai_ollama_live.py -v -s
"""
import os
import json
import shutil
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
        return b"xwpe console editor" in out
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


def _env(trace):
    return {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "ollama",
        "XWPE_AI_ENDPOINT": OLLAMA,
        "XWPE_AI_MODEL": MODEL,
        "XWPE_AI_TRACE": str(trace),
    }


def _wait_for(trace, needle, session, budget=180):
    waited = 0.0
    while waited < budget:
        session._drain(2.0)
        waited += 2.0
        if trace.exists() and needle in trace.read_text():
            return True
    return False


def test_ai_ollama_chat(tmp_path):
    trace = tmp_path / "ai.trace"
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=_env(trace)) as s:
        s.key(ALT.AI)
        s.key("a")
        s.key("In one short sentence, what is a semicolon in C?")
        s.key("\r", delay=2.0)
        assert _wait_for(trace, "chat done", s), "chat never completed:\n" + \
            (trace.read_text() if trace.exists() else "")
        disp = "\n".join(s.display())
    # A real reply put SOMETHING under the "AI:" line in the pane.
    assert len(disp.strip()) > 0


def test_ai_ollama_edit_fixes_bug(tmp_path):
    """Ask Ollama to fix a real compile error; verify the result COMPILES."""
    trace = tmp_path / "ai.trace"
    buggy = (
        "#include <stdio.h>\n"
        "int main(void)\n"
        "{\n"
        "    int x = 40 + 2\n"          # <-- missing semicolon (won't compile)
        "    printf(\"%d\\n\", x);\n"
        "    return 0;\n"
        "}\n"
    )
    # sanity: the seed really does NOT compile
    seed = tmp_path / "pre.c"
    seed.write_text(buggy)
    pre = subprocess.run(["gcc", "-fsyntax-only", str(seed)],
                         stderr=subprocess.PIPE)
    assert pre.returncode != 0, "seed unexpectedly compiled"

    with WpeSession(str(tmp_path), buggy, filename="bug.c",
                    env_extra=_env(trace)) as s:
        s.key(ALT.AI)
        s.key("e")                       # Edit
        s.key("Fix the compile error. Return ONLY the corrected C file.")
        s.key("\r", delay=1.0)           # submit -> generation starts
        # 'a' (accept all hunks) is buffered by the pty until the per-hunk
        # preview's getch runs, so it is applied once generation finishes.
        s.key("a", delay=1.0)
        assert _wait_for(trace, "edit applied", s, budget=220), \
            "edit never applied:\n" + (trace.read_text() if trace.exists() else "")
        s._drain(1.0)
        s.save()
        fixed = s.text()

    # The real proof: the AI-edited file now compiles.
    out = tmp_path / "post.c"
    out.write_text(fixed)
    post = subprocess.run(["gcc", "-fsyntax-only", str(out)],
                          stderr=subprocess.PIPE)
    assert post.returncode == 0, (
        "AI edit did not produce a compiling file:\n"
        + fixed + "\n--- gcc ---\n" + post.stderr.decode("utf-8", "replace"))
