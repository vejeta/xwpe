"""AI assistant -- LIVE PLAN mode (Alt-G p) against a real local Ollama model.

A bug that spans files: main.c prints add(2,3), add.h declares it, add.c
returns a - b.  The model must STUDY the workspace (read the other files),
propose a plan, and after apply-all the program must compile and print 5.
Self-skips unless Ollama is reachable with a usable model.  Slow (minutes).
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
        return b"PROPOSE" in out
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


def _wait_for(trace, needle, session, budget):
    waited = 0.0
    while waited < budget:
        session._drain(3.0)
        waited += 3.0
        if trace.exists() and needle in trace.read_text():
            return True
    return False


def test_ai_plan_live_fixes_cross_file_bug(tmp_path):
    (tmp_path / "add.h").write_text("int add(int a, int b);\n")
    (tmp_path / "add.c").write_text(
        '#include "add.h"\n'
        "int add(int a, int b)\n"
        "{\n"
        "    return a - b;\n"          # <-- the bug lives in a file that is NOT open
        "}\n")
    main_c = ('#include <stdio.h>\n'
              '#include "add.h"\n'
              "int main(void)\n"
              "{\n"
              '    printf("%d\\n", add(2, 3));\n'
              "    return 0;\n"
              "}\n")
    trace = tmp_path.parent / (tmp_path.name + ".trace")
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "ollama",
        "XWPE_AI_ENDPOINT": OLLAMA,
        "XWPE_AI_MODEL": MODEL,
        "XWPE_AI_TRACE": str(trace),
    }
    with WpeSession(str(tmp_path), main_c, filename="main.c",
                    env_extra=env) as s:
        s.key(ALT.AI)
        s.key("p")                       # Plan
        s.key("This program must print 5 but it prints -1: add() in add.c "
              "returns a - b. Read the files, then fix add() to return the "
              "sum. Change only what is needed.")
        s.key("\r", delay=1.0)           # submit -> study turns -> plan pane
        s.key("a", delay=1.0)            # buffered: apply all when the pane asks
        assert _wait_for(trace, "plan done applied=", s, budget=480), \
            "plan never applied:\n" + (trace.read_text() if trace.exists() else "")
        # Save every candidate window (1 = main.c; 2 is the AI pane; 3.. = files
        # the plan opened): Alt-<n> switches, File->Save writes it.
        for n in "13456":
            s.key("\033" + n, delay=0.5)
            s.save()
        s._drain(0.8)

    txt = trace.read_text() if trace.exists() else ""
    assert "plan proposals=" in txt, txt
    prog = tmp_path / "prog"
    r = subprocess.run(["gcc", "-o", str(prog), str(tmp_path / "main.c"),
                        str(tmp_path / "add.c")], stderr=subprocess.PIPE)
    assert r.returncode == 0, ("AI plan left the program uncompilable:\n"
                               + r.stderr.decode("utf-8", "replace")
                               + "\n--- add.c ---\n" + (tmp_path / "add.c").read_text())
    out = subprocess.run([str(prog)], stdout=subprocess.PIPE,
                         timeout=10).stdout.decode().strip()
    assert out == "5", ("program printed %r, not 5\n--- add.c ---\n%s"
                        % (out, (tmp_path / "add.c").read_text()))
