"""AI assistant -- a command it runs must not be able to paint the editor.

A child process inherits the editor's controlling terminal, so a command that
opens /dev/tty writes straight onto the screen, around ncurses: the display
goes black, the child's lines scroll over the editor and the layout only comes
back at the next full repaint.  Redirecting the child's standard output and
error does not stop it; only giving the child a session of its own does, and
then opening /dev/tty fails.

The scenario drives one approved run_command turn whose command tries exactly
that.  The marker lives in a file the command reads, never in the command text:
the pane echoes the command before running it, so a marker spelled inside the
command would reach the terminal stream legitimately and the assertion would
pass on a tree that still has the defect.

Red/green: on a tree whose children keep the editor's terminal the marker bytes
arrive on the pty and the first assertion fails.
"""
import os
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

ESC = "\033"
SEED = "int a = 1;\nint b = 2;\nint c = 3;\n"

# Spelled only in the file the command reads, so finding it in the terminal
# stream can mean one thing: the child wrote onto the editor's terminal.
INTRUDER = "QQINTRUDERQQ"
PAYLOAD = "payload.txt"
TTY_COMMAND = "cat %s > /dev/tty" % PAYLOAD

# Text typed into the file after the turn, saved and read back from disk: the
# editor is not merely running, it still accepts keys and edits the document.
TYPED_AFTER = "STILLTAKESKEYS"

AGENT_SETTLE_POLLS = 20
AGENT_POLL_SECONDS = 0.5


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"autonomous coding agent" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _agent_env(trace):
    """Environment for one auto-approved run_command turn, then DONE."""
    return {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_POLICY": "auto",
        "XWPE_AI_MOCK_REPLY": "TOOL run_command %s\n@@TURN@@DONE ran it" % TTY_COMMAND,
        "XWPE_AI_TRACE": str(trace),
    }


def _wait_for_agent(session, trace):
    """Drain the pty until the trace says the agent run ended."""
    for _ in range(AGENT_SETTLE_POLLS):
        session._drain(AGENT_POLL_SECONDS)
        txt = trace.read_text() if trace.exists() else ""
        if "agent done" in txt or "agent error" in txt:
            return txt
    return trace.read_text() if trace.exists() else ""


# @scenario SC-agent-tty-01
def test_tool_command_cannot_write_to_the_editor_terminal(tmp_path):
    trace = tmp_path / "ai.trace"
    (tmp_path / PAYLOAD).write_text("I/O error: %s\n" % INTRUDER)
    with WpeSession(str(tmp_path), SEED, filename="t.c",
                    env_extra=_agent_env(trace)) as s:
        s.key(ALT.AI)
        s.key("g")                            # aGent
        s.key("run it")
        s.key("\r", delay=1.6)
        txt = _wait_for_agent(s, trace)
        screen = "\n".join(s.display(drain=1.0))
        stream = bytes(s.raw)
        alive = s.alive()
        s.key(ESC, delay=0.8)                 # close the pane, back to the file
        s.key(TYPED_AFTER, delay=0.4)
        s.save()
        on_disk = s.text()

    assert "agent tool=run_command" in txt, \
        "the command never ran, so nothing was proved about the terminal:\n" + txt
    assert INTRUDER.encode() not in stream, \
        "a command the assistant ran wrote onto the editor's terminal"
    assert INTRUDER not in screen, \
        "a command the assistant ran painted over the editor:\n" + screen
    assert alive, "the editor died during the turn"
    assert TYPED_AFTER in on_disk, \
        "the editor stopped accepting keys after the turn; file on disk:\n" + on_disk
