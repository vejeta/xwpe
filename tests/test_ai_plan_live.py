"""AI assistant -- LIVE Multi-file edit (Alt-G f, the plan) against a real local Ollama model.

A bug that spans files: main.c prints add(2,3), add.h declares it, add.c
returns a - b.  The model must STUDY the workspace (read the other files),
propose a plan, and after apply-all the program must compile and print 5.
Self-skips unless the chosen provider is usable (Ollama with a model by
default).  Slow (minutes).

More time for a slow model:  XWPE_TEST_WAIT_SCALE=3 (multiplies every wait).
Against Claude instead of Ollama:
  XWPE_LIVE_AI=claude ANTHROPIC_API_KEY=... CLAUDE_MODEL=... WPE_BIN=$PWD/wpe tests/.venv/bin/python -m pytest -q tests/test_ai_plan_live.py
Provider choice and budgets live in tests/live_ai.py.
"""
import os
import subprocess
import pytest
import live_ai
from wpe_driver import WpeSession, ALT, WPE_BIN


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"PROPOSE" in out
    except Exception:
        return False


pytestmark = [
    pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai"),
    live_ai.SKIP_MARK,
]


def _wait_for(trace, needle, session, budget):
    return live_ai.wait_for_trace(trace, needle, session, budget, step=3.0)


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
    env = live_ai.session_env(trace)
    with WpeSession(str(tmp_path), main_c, filename="main.c",
                    env_extra=env) as s:
        s.key(ALT.AI)
        s.key("f")                       # Multi-file edit (the plan)
        s.key("This program must print 5 but it prints -1: add() in add.c "
              "returns a - b. Read the files, then fix add() to return the "
              "sum. Change only what is needed.")
        s.key("\r", delay=1.0)           # submit -> study turns -> plan pane
        # The plan runs in the background and the window refuses keys until
        # the review dialog is up, so "a" (apply all) waits for the proposals.
        assert _wait_for(trace, "plan proposals=", s, budget=480), \
            "the plan never proposed:\n" + (trace.read_text() if trace.exists() else "")
        s._drain(1.0)
        s.key("a", delay=1.0)
        assert _wait_for(trace, "plan done applied=", s, budget=30), \
            "plan never applied:\n" + (trace.read_text() if trace.exists() else "")
        # Save every candidate window (1 = main.c; 2 is the AI pane; 3.. = files
        # the plan opened): Alt-<n> switches, File->Save writes it.
        for n in "13456":
            s.key("\033" + n, delay=0.5)
            s.save()
        s._drain(0.8)

    txt = trace.read_text() if trace.exists() else ""
    assert "plan proposals=" in txt, txt
    assert "plan proposals=0" not in txt, "the model proposed nothing:\n" + txt
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
