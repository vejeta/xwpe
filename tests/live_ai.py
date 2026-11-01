"""Shared provider selection and time budgets for the LIVE assistant tests.

The live tests (test_ai_ollama_live.py, test_ai_async_live.py,
test_ai_plan_live.py) run against a real model.  This module picks which one,
so the same three tests can be run against a local Ollama (the default) or
against Claude, and it scales every wait by XWPE_TEST_WAIT_SCALE so a slow
machine can give a slow model more time.

  XWPE_LIVE_AI=ollama|claude   provider (default ollama)
  CLAUDE_MODEL=<id>            Claude model (default claude-sonnet-5-5)
  ANTHROPIC_API_KEY            read by the editor itself from the environment

The API key is never read, logged or copied here: WpeSession starts the editor
with the caller's environment, and the editor looks the key up on its own.
"""
import json
import os
import urllib.request

import pytest
from wpe_driver import WAIT_SCALE

OLLAMA_ENDPOINT = "http://localhost:11434"
CLAUDE_ENDPOINT = "https://api.anthropic.com"
CLAUDE_DEFAULT_MODEL = "claude-sonnet-5-5"
PREFERRED = ["qwen2.5-coder:32b", "deepseek-coder:33b", "codellama:34b",
             "qwen3.8:27b", "gemma4:31b"]

PROVIDER = (os.environ.get("XWPE_LIVE_AI") or "ollama").strip().lower()


def _ollama_model():
    """Return the best installed Ollama model name, or None if unreachable."""
    try:
        with urllib.request.urlopen(OLLAMA_ENDPOINT + "/api/tags",
                                    timeout=5) as r:
            names = [m["name"] for m in json.load(r).get("models", [])]
        for want in PREFERRED:
            if want in names:
                return want
        return names[0] if names else None
    except Exception:
        return None


def _resolve():
    """Return (backend, endpoint, model, skip_reason) for the chosen provider.

    skip_reason is None when the provider can be used.
    """
    if PROVIDER == "claude":
        model = os.environ.get("CLAUDE_MODEL") or CLAUDE_DEFAULT_MODEL
        reason = None
        if not os.environ.get("ANTHROPIC_API_KEY"):
            reason = "ANTHROPIC_API_KEY not set"
        return "claude", CLAUDE_ENDPOINT, model, reason
    if PROVIDER != "ollama":
        return "ollama", OLLAMA_ENDPOINT, None, (
            "unknown XWPE_LIVE_AI=%r (use ollama or claude)" % PROVIDER)
    model = _ollama_model()
    reason = None if model else "no local Ollama/model"
    return "ollama", OLLAMA_ENDPOINT, model, reason


def openai_compatible():
    """Return (endpoint, model, skip_reason) for the OpenAI-compatible test.

    The endpoint comes from OPENAI_BASE_URL and the model from OPENAI_MODEL,
    the convention for pointing OpenAI clients at a local server such as
    Ollama (OPENAI_BASE_URL=http://localhost:11434).  There is no default
    endpoint: when either variable is unset the reason says so, so the test
    can never send a request to a host the user did not name.  A base with or
    without a trailing /v1 is passed on unchanged; the editor builds
    <base>/chat/completions from a base that carries a path, and
    /v1/chat/completions from a bare host.
    """
    base = (os.environ.get("OPENAI_BASE_URL") or "").strip()
    model = (os.environ.get("OPENAI_MODEL") or "").strip()
    if not base:
        return None, model, "OPENAI_BASE_URL not set (no default endpoint)"
    if not model:
        return base, None, "OPENAI_MODEL not set"
    return base, model, None


def endpoint_needs_tls(endpoint):
    """True when the endpoint is https, so the build must have TLS."""
    return bool(endpoint) and endpoint.lower().startswith("https:")


BACKEND, ENDPOINT, MODEL, SKIP_REASON = _resolve()

# Apply as a module-level mark: pytestmark = [..., live_ai.SKIP_MARK]
SKIP_MARK = pytest.mark.skipif(SKIP_REASON is not None,
                               reason=SKIP_REASON or "")


def session_env(trace=None):
    """Return the env_extra dict that points the editor at the chosen model.

    trace is an optional path for XWPE_AI_TRACE.
    """
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": BACKEND,
        "XWPE_AI_ENDPOINT": ENDPOINT,
        "XWPE_AI_MODEL": MODEL or "",
    }
    if trace is not None:
        env["XWPE_AI_TRACE"] = str(trace)
    return env


def scaled(seconds):
    """Return a live-wait budget stretched by XWPE_TEST_WAIT_SCALE."""
    return seconds * WAIT_SCALE


def wait_for_trace(trace, needle, session, budget, step=2.0):
    """Wait until the trace file contains needle; True if it did in time.

    budget is in seconds at scale 1; the real wait is budget * WAIT_SCALE.
    session._drain already stretches each step by the scale, so the deadline
    is measured on the clock, not by counting steps.
    """
    import time
    deadline = time.monotonic() + scaled(budget)
    while time.monotonic() < deadline:
        session._drain(step)
        if trace.exists() and needle in trace.read_text():
            return True
    return False


# What the editor paints or traces when a provider refuses or cannot be
# reached: a chat reply cut by an error ("AI: error: ..."), a transport
# failure, an edit or agent turn that ended in a backend error.
PANE_ERROR_MARKS = ("AI: error:", "transport error", "backend error",
                    "cannot connect")
TRACE_ERROR_MARKS = ("transport error", "cut by an error", "edit error")


def has_backend_error(disp, trace=None):
    """True when the pane text or the trace shows the provider's failure.

    A live test passes only if the model really answered, so a refused
    request (wrong key scope, bad model, no network) must fail it instead of
    leaving a screen that still holds the typed prompt.
    """
    for mark in PANE_ERROR_MARKS:
        if mark in disp:
            return True
    if trace is not None and trace.exists():
        txt = trace.read_text()
        for mark in TRACE_ERROR_MARKS:
            if mark in txt:
                return True
    return False


# The chat question asks for a word it does not contain, so the question
# echoed on the screen cannot satisfy the check: only a real reply can.
CHAT_PROMPT = "Reply with the word that is the reverse of GNOP"
CHAT_ANSWER = "pong"
CHAT_PROMPT_START = "reply with the word"   # identifies the echoed question


def reply_shows_answer(disp):
    """True when a line other than the echoed question holds the answer.

    A reply that repeats the question ("The reverse of GNOP is PONG") still
    counts; only the line holding the typed question is left out.
    """
    for line in disp.splitlines():
        low = line.lower()
        if CHAT_PROMPT_START in low:
            continue
        if CHAT_ANSWER in low:
            return True
    return False
