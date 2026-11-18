"""The assistant's write asks about a file with several names, and about a file
replaced under its window, under the X11/Xft backend.

Backend parity: both questions are the marked write box, shared UI.  The
pyte rows in tests/test_ai_agent_write.py drive them in the terminal; this
drives a real xwpe under the headless X server and asserts the box painted,
that the editor reached the question (its own trace says so), and that
answering n leaves every name of the file as it was.  The same module runs
under the native Wayland backend through tests/wayland/test_ai_agent_write.py.
Self-skips if the X11 harness or --enable-ai is missing.
"""
import os
import subprocess
import pytest
from conftest import changed_pixels, wait_for, XWPE_BIN

TRACE_WAIT_S = 20.0
BOX_MIN_PIXELS = 1500
SECRET = "the user's other name for this file\n"
REPLACEMENT = "int main(void){return 3;}\n"
TRACE_NAME = "ai.trace"       # relative: the editor runs in the row's folder


def _has_ai():
    try:
        out = subprocess.run(["strings", XWPE_BIN],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"Alt-G AI" in out
    except Exception:
        return False


def _agent_env(reply):
    """The mock backend under the unattended dial, tracing into the row's folder."""
    return {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_POLICY": "auto",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_TRACE": TRACE_NAME,
    }


LINKED_ENV = _agent_env("TOOL write_file x\nplanted text\n@@END@@TURN@@DONE tried")
REPLACED_ENV = _agent_env("TOOL write_file t.c\nint main(void){return 7;}\n@@END@@TURN@@DONE tried")


def _folder(xwpe):
    """The folder the editor started in, which is the assistant's workspace."""
    return os.path.dirname(xwpe.srcfile)


def _trace_has(xwpe, needle):
    path = os.path.join(_folder(xwpe), TRACE_NAME)
    try:
        with open(path) as fh:
            return needle in fh.read()
    except OSError:
        return False


def _wait_trace(xwpe, needle):
    wait_for(lambda: _trace_has(xwpe, needle),
             "the editor's trace to say %r" % needle, TRACE_WAIT_S)


def _ask_the_agent(xwpe, request_text):
    xwpe.key("alt+g")                 # AI prefix
    xwpe.key("g", delay=0.6)          # g = Agent -> prompt dialog
    xwpe.type(request_text)
    xwpe.key("Return", delay=1.0)


def _read(path):
    with open(path) as fh:
        return fh.read()


# @scenario SC-write-owner-H2b-04
@pytest.mark.skipif(not _has_ai(), reason="xwpe built without --enable-ai")
@pytest.mark.parametrize("xwpe", [LINKED_ENV], indirect=True)
def test_write_to_a_hard_linked_file_asks_under_x11(xwpe):
    folder = _folder(xwpe)
    outside = folder + "-outside"
    os.mkdir(outside)
    secret = os.path.join(outside, "secret")
    with open(secret, "w") as fh:
        fh.write(SECRET)
    name = os.path.join(folder, "x")
    os.link(secret, name)
    before = xwpe.screenshot()
    _ask_the_agent(xwpe, "write x")
    _wait_trace(xwpe, "agent write linked x names=2")
    boxed = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died at the question"
    assert changed_pixels(before, boxed) > BOX_MIN_PIXELS, \
        "the question about a file with two names did not paint under X11/Xft"
    xwpe.key("n", delay=0.8)
    _wait_trace(xwpe, "agent deny write_file x")
    assert _read(secret) == SECRET and _read(name) == SECRET, \
        "a refused write changed a name of the file"


# @scenario SC-write-owner-H2b-09
@pytest.mark.skipif(not _has_ai(), reason="xwpe built without --enable-ai")
@pytest.mark.parametrize("xwpe", [REPLACED_ENV], indirect=True)
def test_write_after_the_file_was_replaced_asks_under_x11(xwpe):
    folder = _folder(xwpe)
    target = os.path.join(folder, "t.c")
    new = target + ".new"
    with open(new, "w") as fh:
        fh.write(REPLACEMENT)
    os.rename(new, target)            # another file under the window's name
    before = xwpe.screenshot()
    _ask_the_agent(xwpe, "change t.c")
    _wait_trace(xwpe, "agent write replaced t.c")
    boxed = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died at the question"
    assert changed_pixels(before, boxed) > BOX_MIN_PIXELS, \
        "the question about a replaced file did not paint under X11/Xft"
    xwpe.key("n", delay=0.8)
    _wait_trace(xwpe, "agent deny write_file t.c")
    assert _read(target) == REPLACEMENT, "a refused write changed the file"
