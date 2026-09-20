"""AI assistant -- Agent mode (deterministic mock backend).

The mock can script several turns (XWPE_AI_MOCK_REPLY split on @@TURN@@), so we
drive the full tool loop: a DONE, an approved write_file, and a denied
run_command.  Read-only tools run automatically; write/run need y/n approval.
"""
import os
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"autonomous coding agent" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _agent(tmp_path, reply, task="do the task", approve=None):
    trace = tmp_path / "ai.trace"
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_TRACE": str(trace),
    }
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key(ALT.AI)
        s.key("g")                       # aGent
        s.key(task)
        s.key("\r", delay=1.6)           # submit -> agent loop (policy = ask)
        s._drain(1.0)
        if approve is not None:
            s.key(approve, delay=1.2)    # answer the approval prompt
            s._drain(1.2)
    return trace.read_text() if trace.exists() else ""


def test_ai_agent_done(tmp_path):
    txt = _agent(tmp_path, "DONE agent completed the task", task="tidy up")
    assert "agent task=tidy up" in txt, txt
    assert "agent done" in txt, txt


def test_ai_agent_write_file_approved(tmp_path):
    reply = "TOOL write_file agent_out.txt\nhello agent\n@@END@@TURN@@DONE wrote it"
    txt = _agent(tmp_path, reply, task="write a file", approve="y")
    assert "agent tool=write_file" in txt, txt
    out = tmp_path / "agent_out.txt"
    assert out.exists(), "agent did not create the file"
    assert "hello agent" in out.read_text()
    assert "agent done" in txt, txt


def test_ai_agent_run_command_denied(tmp_path):
    sentinel = tmp_path / "SHOULD_NOT_EXIST"
    reply = ("TOOL run_command touch %s\n@@TURN@@DONE stopped" % sentinel)
    txt = _agent(tmp_path, reply, task="run something", approve="n")
    assert "agent tool=run_command" in txt, txt
    assert not sentinel.exists(), "denied command still ran!"
    assert "agent done" in txt, txt


def test_ai_agent_tool_after_preamble(tmp_path):
    # A model (a local one especially) often writes a sentence of reasoning
    # BEFORE its "TOOL ..." line.  The agent must find the tool line anyway and
    # run it -- not mistake the preamble for the final answer and stop.
    reply = ("Let me read the files first so I do not invent details.\n"
             "TOOL list_dir .@@TURN@@DONE it is C")
    txt = _agent(tmp_path, reply, task="inspect")
    assert "agent tool=list_dir" in txt, \
        "the preamble before TOOL was mistaken for the answer; tool never ran:\n" + txt
    assert "agent done" in txt, txt


def test_agent_pane_docks_at_bottom(tmp_path):
    # The agent's output pane must dock at the bottom like the chat pane, leaving
    # the edited file visible above it -- not open as a full window over the file.
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": "TOOL list_dir .@@TURN@@DONE looked around"}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, filename="prog.c") as s:
        s.key(ALT.AI); s.key("g")
        s.key("look around"); s.key("\r", delay=1.2)
        s._drain(1.5)
        disp = s.display()
    text = "\n".join(disp)
    file_row = next((y for y, ln in enumerate(disp) if "int main(void)" in ln), -1)
    pane_row = next((y for y, ln in enumerate(disp) if "[agent]" in ln), -1)
    assert file_row >= 0, "the edited file is not visible -- pane took the whole screen:\n" + text
    assert pane_row >= 0, "the agent pane did not render:\n" + text
    assert file_row < pane_row, \
        "the agent pane is not docked below the file (file row %d, pane row %d):\n%s" \
        % (file_row, pane_row, text)


def test_agent_offers_followup_when_done(tmp_path):
    # When the agent finishes it arms the chat input on the same pane so the
    # user can type a follow-up (with the agent's context) instead of a dead log.
    turns = ("TOOL list_dir .@@TURN@@DONE looked around"
             "@@TURN@@FOLLOWUP_REPLY_MARKER")
    trace = tmp_path / "ai.trace"
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_POLICY": "ask",
           "XWPE_AI_MOCK_REPLY": turns, "XWPE_AI_TRACE": str(trace)}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, filename="t.c") as s:
        s.key(ALT.AI); s.key("g")
        s.key("look around"); s.key("\r", delay=1.0); s._drain(1.5)
        armed = "\n".join(s.display())
        assert "type a follow-up" in armed, "no follow-up prompt after the agent:\n" + armed
        # a follow-up typed in the pane is sent as a chat turn (proving the input
        # row was armed and consumes keys -- including the FIRST key, which used to
        # leak into the file because the editor loop passed a stale focused window)
        s.key("CONTINUEWORK"); s.key("\r", delay=1.0); s._drain(0.8)
        after = "\n".join(s.display())
    txt = trace.read_text() if trace.exists() else ""
    assert "chat prompt=CONTINUEWORK" in txt, \
        "the follow-up lost its first key(s) -- stale focus:\n" + txt
    assert "FOLLOWUP_REPLY_MARKER" in after, \
        "the in-pane follow-up did not send / get a reply:\n" + after
