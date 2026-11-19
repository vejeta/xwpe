"""How far the assistant's commands are contained, said under the X11/Xft backend.

Backend parity: the pane header carries a containment word, and the first run
of a session says in the error colour what the commands still reach (their
files, the network).  Both are pane text, shared UI.  The pyte rows in
tests/test_ai_contain_credentials.py read them in the terminal; this drives a
real xwpe under the headless X server and asserts that the editor reached
each statement (its own trace says so), that the pane painted, that the line
is said once, and that Strict, which this system cannot yet honour, stops the
assistant's command before the fork.  The same module runs under the native
Wayland backend through tests/wayland/test_ai_contain_report.py.  Self-skips
if the X11 harness or --enable-ai is missing.
"""
import os
import subprocess
import pytest
from conftest import changed_pixels, wait_for, XWPE_BIN

TRACE_WAIT_S = 20.0
PANE_MIN_PIXELS = 1500
TRACE_NAME = "ai.trace"       # relative: the editor runs in the row's folder
UI_TRACE_NAME = "ui.trace"
MARKER_NAME = "ran.marker"


def _has_ai():
    try:
        out = subprocess.run(["strings", XWPE_BIN],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"Alt-G AI" in out
    except Exception:
        return False


def _env(reply, **extra):
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_POLICY": "auto",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_TRACE": TRACE_NAME,
        "XWPE_UI_TRACE": UI_TRACE_NAME,
    }
    env.update(extra)
    return env


PLAIN_ENV = _env("DONE nothing to run")
STRICT_ENV = _env("TOOL run_command touch " + MARKER_NAME + "@@TURN@@DONE tried",
                  XWPE_AI_CONTAIN="strict")


def _folder(xwpe):
    """The folder the editor started in, which is the assistant's workspace."""
    return os.path.dirname(xwpe.srcfile)


def _read_trace(xwpe, name):
    try:
        with open(os.path.join(_folder(xwpe), name)) as fh:
            return fh.read()
    except OSError:
        return ""


def _wait_trace(xwpe, name, needle):
    wait_for(lambda: needle in _read_trace(xwpe, name),
             "the editor's trace to say %r" % needle, TRACE_WAIT_S)


def _ask_the_agent(xwpe, request_text):
    xwpe.key("alt+g")                 # AI prefix
    xwpe.key("g", delay=0.6)          # g = Agent -> prompt dialog
    xwpe.type(request_text)
    xwpe.key("Return", delay=1.0)


# @scenario SC-ai-contain-31
@pytest.mark.skipif(not _has_ai(), reason="xwpe built without --enable-ai")
@pytest.mark.parametrize("xwpe", [PLAIN_ENV], indirect=True)
def test_the_header_word_and_the_line_are_said_once_under_x11(xwpe):
    before = xwpe.screenshot()
    _ask_the_agent(xwpe, "first task")
    _wait_trace(xwpe, TRACE_NAME, "agent containment=UNCONTAINED")
    _wait_trace(xwpe, TRACE_NAME, "agent containment notice said")
    painted = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died while saying how far the commands are held"
    assert changed_pixels(before, painted) > PANE_MIN_PIXELS, \
        "the pane with the header and the containment line did not paint under X11/Xft"
    wait_for(lambda: "agent done" in _read_trace(xwpe, TRACE_NAME),
             "the first run to end", TRACE_WAIT_S)
    _ask_the_agent(xwpe, "second task")
    wait_for(lambda: _read_trace(xwpe, TRACE_NAME).count("agent containment=UNCONTAINED") == 2,
             "the second run's header", TRACE_WAIT_S)
    assert _read_trace(xwpe, TRACE_NAME).count("agent containment notice said") == 1, \
        "the containment line was said again in the same session"


# @scenario SC-ai-contain-32
@pytest.mark.skipif(not _has_ai(), reason="xwpe built without --enable-ai")
@pytest.mark.parametrize("xwpe", [STRICT_ENV], indirect=True)
def test_strict_stops_the_assistants_command_before_the_fork_under_x11(xwpe):
    _ask_the_agent(xwpe, "touch the marker")
    _wait_trace(xwpe, UI_TRACE_NAME, "child uncontained")
    assert xwpe.proc.poll() is None, "xwpe died at the refused start"
    assert not os.path.exists(os.path.join(_folder(xwpe), MARKER_NAME)), \
        "the command ran although Strict was asked for"
