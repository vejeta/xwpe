"""Project instructions are announced and cannot remove tools; a dial raised
mid-run gets a checkpoint.

The editor folds AGENTS.md (else .xwpe-ai.md) from the working directory into
the prompt.  It used to say so only in the trace, and a file longer than the
4095-byte buffer was cut silently: a user whose AGENTS.md told other coding
agents "do not open network connections" saw the model refuse a web search
with nothing on screen to explain it.  The pane now shows "[agent] project
instructions: <name> (<N> bytes)" right after the task line, with ", cut at
4095" when the file was longer, and the chat shows the same line on the first
turn of a session only.  The prompt states, in the same system message as the
project block, that the tool list above stands: the instructions govern how
the tools are used, never which tools exist.

The permission dial is read live at each approval and Alt-G y cycles it while
a run waits for the model, but the checkpoint was decided once at launch, so a
run started under ask and raised to auto wrote freely and ended with no
review.  The first write under a raised dial now takes the checkpoint the
launch would have taken, with the same pane line, and the review opens at the
end.

What the prompt carried is read from the stdin a stub `claude` saves when the
backend is the Claude CLI, as tests/test_ai_edit_request_body.py does; the
mock backend drives everything else.
"""
import os
import subprocess
import time

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

SEED = "int main(void){return 0;}\n"

MEMORY_USABLE = 4095                      # bytes of AGENTS.md the prompt keeps
SMALL_SIZE = 200
LARGE_SIZE = 6000

ANNOUNCE = "[agent] project instructions: AGENTS.md"
CUT_NOTE = "cut at %d" % MEMORY_USABLE
TOOLS_STAND = "The tool list above stands"
PROJECT_BLOCK = "PROJECT INSTRUCTIONS"
NETWORK_RULE = "do not open network connections"
CHECKPOINT_LINE = "[agent] checkpoint taken"
REVIEW_PROMPT = "a = keep all"
FOLLOWUP_LINE = "type a follow-up"
CHAT_REPLY = "The project keeps every function short."

# The driver scales its own key/drain waits by XWPE_TEST_WAIT_SCALE (the
# sanitizer build runs at 2, the Debian autopkgtest at 3), but a fixed
# XWPE_AI_MOCK_DELAY_MS does not: the mock's reply lands at the same
# wall-clock time regardless of that factor.  A slow first turn leaves room
# to press Alt-G y twice while the run waits for the model; the two presses
# take about four seconds at the sanitizer's wait scale, so the reply must be
# held back for longer than that at every scale, and the poll bounds that
# wait on the dial switch and the final review must grow with it too.
WAIT_SCALE = float(os.environ.get("XWPE_TEST_WAIT_SCALE", "1"))
SLOW_TURN_MS = int(8000 * WAIT_SCALE)
TURN_POLL_S = int(25 * WAIT_SCALE)
TRACE_POLL_S = int(6 * WAIT_SCALE)


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"autonomous coding agent" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _env(tmp_path, reply, policy="ask"):
    return {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_POLICY": policy,
        "XWPE_AI_TRACE": str(tmp_path / "ai.trace"),
    }


def _trace(tmp_path):
    trace = tmp_path / "ai.trace"
    return trace.read_text() if trace.exists() else ""


def _seen_within(s, needle, timeout):
    """Drain the screen until `needle` shows or `timeout` seconds pass."""
    end = time.time() + timeout
    while time.time() < end:
        s._drain(0.25)
        if needle in "\n".join(s.display(drain=0)):
            return True
    return False


def _traced_within(tmp_path, needle, timeout):
    """Wait until the trace holds `needle` or `timeout` seconds pass."""
    end = time.time() + timeout
    while time.time() < end:
        if needle in _trace(tmp_path):
            return True
        time.sleep(0.2)
    return False


def _write_agents_md(tmp_path, size):
    """An AGENTS.md of exactly `size` bytes: a heading, then filler lines."""
    head = "# Project rules\n- keep every function short\n"
    body = head + "x" * (size - len(head) - 1) + "\n"
    assert len(body.encode()) == size
    (tmp_path / "AGENTS.md").write_text(body)


def _row_index(rows, needle):
    return next((i for i, r in enumerate(rows) if needle in r), -1)


def _stub_claude_capturing(dir_, capture, reply):
    """A fake `claude` CLI that saves its stdin (the request body) to `capture`
    and answers with a success envelope whose result is `reply`."""
    d = os.path.join(dir_, "bin")
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, "claude")
    envelope = ('{"type":"result","subtype":"success","is_error":false,'
                '"result":"%s","session_id":"stub"}' % reply.replace("\n", "\\n"))
    with open(p, "w") as f:
        f.write("#!/bin/sh\n"
                "cat > '" + capture + "'\n"
                "printf '%s' '" + envelope + "'\n")
    os.chmod(p, 0o755)
    return d


def _agent_screen_after_done(tmp_path, env, task):
    """Run one agent task to its follow-up prompt and return the screen rows."""
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        s.key(ALT.AI); s.key("g")
        s.key(task); s.key("\r", delay=1.0)
        assert _seen_within(s, FOLLOWUP_LINE, TURN_POLL_S), \
            "the run did not finish:\n" + "\n".join(s.display())
        return s.display()


# @scenario SC-ai-agent-37
def test_project_instructions_announced_with_name_and_size(tmp_path):
    _write_agents_md(tmp_path, SMALL_SIZE)
    rows = _agent_screen_after_done(tmp_path, _env(tmp_path, "DONE looked"), "look")
    screen = "\n".join(rows)
    task_row = _row_index(rows, "[agent] task: look")
    ann_row = _row_index(rows, ANNOUNCE)
    assert task_row >= 0, "no task line:\n" + screen
    assert ann_row >= 0, "the pane does not say the project instructions were loaded:\n" + screen
    assert ann_row == task_row + 1, \
        "the announcement is not right after the task line (rows %d, %d):\n%s" \
        % (task_row, ann_row, screen)
    assert "(%d bytes)" % SMALL_SIZE in rows[ann_row], rows[ann_row]
    assert "cut at" not in rows[ann_row], rows[ann_row]
    assert "project memory AGENTS.md bytes=%d" % SMALL_SIZE in _trace(tmp_path)


# @scenario SC-ai-agent-37
def test_project_instructions_announce_the_cut(tmp_path):
    _write_agents_md(tmp_path, LARGE_SIZE)
    rows = _agent_screen_after_done(tmp_path, _env(tmp_path, "DONE looked"), "look")
    screen = "\n".join(rows)
    ann_row = _row_index(rows, ANNOUNCE)
    assert ann_row >= 0, "the pane does not say the project instructions were loaded:\n" + screen
    assert CUT_NOTE in rows[ann_row], \
        "a %d-byte file was cut to %d bytes and the pane did not say so:\n%s" \
        % (LARGE_SIZE, MEMORY_USABLE, rows[ann_row])
    assert "(%d bytes" % LARGE_SIZE in rows[ann_row], rows[ann_row]


# @scenario SC-ai-agent-37
def test_chat_announces_project_instructions_once_per_session(tmp_path):
    _write_agents_md(tmp_path, SMALL_SIZE)
    env = _env(tmp_path, CHAT_REPLY)
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        s.key(ALT.AI); s.key("a"); s._drain(0.5)
        s.key("hi"); s.key("\r", delay=1.0)
        assert _seen_within(s, "AI: " + CHAT_REPLY, TURN_POLL_S), "\n".join(s.display())
        s.key("again"); s.key("\r", delay=1.0)
        assert _traced_within(tmp_path, "chat prompt=again", TRACE_POLL_S)
        s._drain(1.5)
        rows = s.display()
    screen = "\n".join(rows)
    announced = [r for r in rows if ANNOUNCE in r]
    assert len(announced) == 1, \
        "the chat must announce the project instructions once per session, saw %d:\n%s" \
        % (len(announced), screen)
    assert "(%d bytes)" % SMALL_SIZE in announced[0], announced[0]
    # Both turns folded the file in; only the first said so.
    assert _trace(tmp_path).count("project memory AGENTS.md bytes=%d" % SMALL_SIZE) >= 2


# @scenario SC-ai-agent-36
# @scenario SC-ai-agent-38
def test_prompt_states_the_tool_list_stands_after_the_project_block(tmp_path):
    (tmp_path / "AGENTS.md").write_text(
        "# Rules for coding agents\n" + NETWORK_RULE + ".\n")
    capture = str(tmp_path / "request.txt")
    bindir = _stub_claude_capturing(str(tmp_path), capture, "DONE ok")
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "claudecli",
        "XWPE_AI_MODEL": "sonnet",
        "XWPE_AI_POLICY": "ask",
        "PATH": bindir + ":" + os.environ.get("PATH", ""),
        "XWPE_AI_TRACE": str(tmp_path / "ai.trace"),
    }
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        s.key(ALT.AI); s.key("g")
        s.key("search the web for the latest release"); s.key("\r", delay=1.0)
        assert _seen_within(s, FOLLOWUP_LINE, TURN_POLL_S), \
            "the run did not finish:\n" + "\n".join(s.display())
    body = open(capture).read()

    assert PROJECT_BLOCK in body, body
    assert NETWORK_RULE in body, body
    assert TOOLS_STAND in body, \
        "the prompt does not say the tool list stands over the project block:\n" + body
    i_tools = body.index("TOOL web_search")
    i_block = body.index(PROJECT_BLOCK)
    i_rule = body.index(NETWORK_RULE)
    i_stand = body.index(TOOLS_STAND)
    assert i_tools < i_block < i_rule < i_stand, (i_tools, i_block, i_rule, i_stand)
    # Same system message: no labelled turn opens between the block and the
    # sentence (the Claude CLI prompt labels user and assistant turns only).
    between = body[i_block:i_stand]
    assert "User: " not in between and "Assistant: " not in between, between
    assert "never which tools exist" in body, body


# @scenario SC-ai-agent-22
def test_dial_raised_mid_run_takes_checkpoint_before_the_write(tmp_path):
    reply = "TOOL write_file out.txt\nhello agent\n@@END@@TURN@@DONE wrote it"
    env = _env(tmp_path, reply, policy="ask")
    env["XWPE_AI_MOCK_DELAY_MS"] = str(SLOW_TURN_MS)
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        s.key(ALT.AI); s.key("g")
        s.key("write a file"); s.key("\r", delay=0.3)      # the first turn is slow
        # Alt-G y twice while the run waits: ask -> edits -> auto.
        s.key(ALT.AI); s.key("y")
        s.key(ALT.AI); s.key("y")
        assert _traced_within(tmp_path, "policy set auto", TRACE_POLL_S), \
            "Alt-G y did not reach the dial during the turn:\n" + _trace(tmp_path)
        assert _seen_within(s, REVIEW_PROMPT, TURN_POLL_S), \
            "no review at the end of a run raised to auto:\n" + "\n".join(s.display())
        review = "\n".join(s.display())
        assert "AI changeset: 1 change" in review, review
        assert "out.txt:" in review, "the Messages window does not list the written file:\n" + review
        s.key("a", delay=1.0)                                # keep all
        # The review's Messages window covered the pane; the follow-up prompt
        # brings the pane back, with the run's lines in the order they were shown.
        assert _seen_within(s, FOLLOWUP_LINE, TRACE_POLL_S), "\n".join(s.display())
        rows = s.display()
        screen = "\n".join(rows)
        ck_row = _row_index(rows, CHECKPOINT_LINE)
        ok_row = _row_index(rows, "[agent] auto-approved (auto): write out.txt")
        assert ck_row >= 0, "no checkpoint line in the pane:\n" + screen
        assert ok_row >= 0, "the write was not auto-approved under the raised dial:\n" + screen
        assert ck_row < ok_row, \
            "the checkpoint line must come before the write (rows %d, %d):\n%s" \
            % (ck_row, ok_row, screen)
    txt = _trace(tmp_path)
    out = tmp_path / "out.txt"
    assert out.exists() and "hello agent" in out.read_text()
    assert "agent write confirm" not in txt, "the write box opened under auto:\n" + txt
    order = ["policy set edits", "policy set auto", "checkpoint",
             "agent auto-approve write out.txt", "changeset n=1", "changeset keep all"]
    at = [txt.find(m) for m in order]
    assert all(i >= 0 for i in at), dict(zip(order, at))
    assert at == sorted(at), "trace order is wrong: %s\n%s" % (dict(zip(order, at)), txt)
