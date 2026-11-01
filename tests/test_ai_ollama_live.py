"""AI assistant -- LIVE tests against a real local Ollama.

Self-skips unless Ollama is reachable on localhost:11434 with a usable model.
Proves real end-to-end behaviour that the mock cannot: the editor talks to a
real model, and an AI Edit actually FIXES a broken C file (verified by compiling
the result).  Slow -- a real 30B model generates for tens of seconds.

Run:  WPE_BIN=../wpe python -m pytest tests/test_ai_ollama_live.py -v -s
More time for a slow model:  XWPE_TEST_WAIT_SCALE=3 (multiplies every wait).
Against Claude instead of Ollama:
  XWPE_LIVE_AI=claude ANTHROPIC_API_KEY=... CLAUDE_MODEL=... WPE_BIN=$PWD/wpe tests/.venv/bin/python -m pytest -q tests/test_ai_ollama_live.py
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
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = [
    pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai"),
    live_ai.SKIP_MARK,
]


def _env(trace):
    return live_ai.session_env(trace)


def _wait_for(trace, needle, session, budget=180):
    return live_ai.wait_for_trace(trace, needle, session, budget)


CHAT_PROMPT = live_ai.CHAT_PROMPT
_reply_shows_answer = live_ai.reply_shows_answer


def test_ai_ollama_chat(tmp_path):
    trace = tmp_path / "ai.trace"
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=_env(trace)) as s:
        s.key(ALT.AI)
        s.key("a")
        s.key(CHAT_PROMPT)
        s.key("\r", delay=2.0)
        assert _wait_for(trace, "chat done", s), "chat never completed:\n" + \
            (trace.read_text() if trace.exists() else "")
        disp = "\n".join(s.display())
    # A backend error leaves a non-empty screen too, so it fails the test.
    assert not live_ai.has_backend_error(disp, trace), \
        "the provider returned an error:\n" + disp
    # The expected word is not in the prompt: only a real reply can show it.
    assert _reply_shows_answer(disp), "no answer under the AI: line:\n" + disp


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
        # The Edit runs in the background and the file window refuses keys
        # until the reply lands, so "a" (accept all) is typed only once the
        # review box is up.
        assert _wait_for(trace, "edit review", s, budget=220), \
            "the review never opened:\n" + (trace.read_text() if trace.exists() else "")
        assert not live_ai.has_backend_error("\n".join(s.display()), trace), \
            "the provider returned an error:\n" + "\n".join(s.display())
        s.key("a", delay=1.0)
        assert _wait_for(trace, "edit applied", s, budget=20), \
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
