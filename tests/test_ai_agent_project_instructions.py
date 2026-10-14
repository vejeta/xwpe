"""Project instructions are announced and cannot remove tools, are folded in
only where the assistant acts, prefer the editor's own file, and are bounded
by a key; a dial raised mid-run gets a checkpoint.

The editor folds .xwpe-ai.md (else AGENTS.md) from the working directory into
the prompt.  It used to say so only in the trace, and a file longer than the
4095-byte buffer was cut silently: a user whose AGENTS.md told other coding
agents "do not open network connections" saw the model refuse a web search
with nothing on screen to explain it.  The pane now shows "[agent] project
instructions: <name> (<N> bytes)" right after the task line, with ", cut at
4095; AIProjectInstructionsMax raises it" when the file was longer, and the
chat shows the same line once per session.  The prompt states, in the same
system message as the project block, that the tool list above stands: the
instructions govern how the tools are used, never which tools exist.

Where the block goes is decided by AIProjectInstructions (environment
XWPE_AI_PROJECT_INSTRUCTIONS).  Under the default, agent, the Agent, the
Multi-file edit and Build & fix carry it, and the chat carries it from the
turn in which it first uses a tool; a plain Ask does not, because the file
exists for work that acts on the repository and on the current file it is
context spent for nothing, a tenth of a local model's window.  all folds it
into every chat turn as well; off never folds it.  When both files exist,
.xwpe-ai.md wins, so a repository whose AGENTS.md serves other coding agents
can still give this editor its own instructions.  The bound is
AIProjectInstructionsMax (environment XWPE_AI_PROJECT_INSTRUCTIONS_MAX,
default 4095), and the cut line names the key.

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
ANNOUNCE_ANY = "[agent] project instructions:"
ANNOUNCE_SPECIFIC = "[agent] project instructions: .xwpe-ai.md"
CUT_NOTE = "cut at %d" % MEMORY_USABLE
BOUND_KEY = "AIProjectInstructionsMax"
SCOPE_KEY = "AIProjectInstructions"
BOUND_ENV = "XWPE_AI_PROJECT_INSTRUCTIONS_MAX"
SCOPE_ENV = "XWPE_AI_PROJECT_INSTRUCTIONS"
TOOLS_STAND = "The tool list above stands"
PROJECT_BLOCK = "PROJECT INSTRUCTIONS"
NETWORK_RULE = "do not open network connections"
CHECKPOINT_LINE = "[agent] checkpoint taken"
REVIEW_PROMPT = "a = keep all"
FOLLOWUP_LINE = "type a follow-up"
CHAT_REPLY = "The project keeps every function short."
# The two files, each with a sentence the other does not have, so the body
# shows which one was folded in.
SPECIFIC_SENTENCE = "Prefer the editor's own file XWPEAI_QUUX."
GENERIC_SENTENCE = "Serve the other coding agents AGENTS_ZORK."
# A 1000-byte file made of 200 numbered lines of 5 bytes; a bound of 300
# keeps exactly the first 60 lines.
BOUND_SMALL = 300
BOUNDED_FILE = "".join("%04d\n" % i for i in range(200))
BOUNDED_FILE_SIZE = len(BOUNDED_FILE.encode())
FIRST_LINE_PAST_BOUND = "%04d" % (BOUND_SMALL // 5)

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
    """Two plain chat turns fold the file in and the pane says so once.

    Plain chat turns carry the file only under AIProjectInstructions all (the
    default, agent, waits for the chat's first tool turn), so the scope is set
    to all here through its environment override; the once-per-session rule
    under the default is asserted by
    test_chat_carries_project_instructions_from_its_first_tool_turn.
    """
    _write_agents_md(tmp_path, SMALL_SIZE)
    env = _env(tmp_path, CHAT_REPLY)
    env[SCOPE_ENV] = "all"
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


# ----- where the block goes, which file wins, how long it may be -----------

def _claudecli_env(tmp_path, bindir):
    """The Claude CLI backend with a stub `claude` first on PATH, so the stub
    sees the request body the editor really built."""
    return {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "claudecli",
        "XWPE_AI_MODEL": "sonnet",
        "XWPE_AI_POLICY": "ask",
        "PATH": bindir + ":" + os.environ.get("PATH", ""),
        "XWPE_AI_TRACE": str(tmp_path / "ai.trace"),
        "XDG_CONFIG_HOME": str(tmp_path / "config"),
    }


def _write_xwperc(tmp_path, *lines):
    """An xwperc under the test's own XDG_CONFIG_HOME with the given
    [Programming] keys, the way a user would set them."""
    cfg = tmp_path / "config" / "xwpe"
    cfg.mkdir(parents=True, exist_ok=True)
    (cfg / "xwperc").write_text("[Programming]\n" + "".join(l + "\n" for l in lines))


def _stub_claude_sequenced(dir_, capture_prefix, replies):
    """A fake `claude` CLI for a conversation of several calls: the Nth call
    saves its stdin to `<capture_prefix>.N` and answers the Nth reply (the last
    one repeats), so every request of a chat can be read back afterwards."""
    d = os.path.join(dir_, "bin")
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, "claude")
    counter = os.path.join(dir_, "claude.calls")
    cases = ""
    for i, reply in enumerate(replies, start=1):
        envelope = ('{"type":"result","subtype":"success","is_error":false,'
                    '"result":"%s","session_id":"stub"}' % reply.replace("\n", "\\n"))
        pattern = "%d" % i if i < len(replies) else "*"
        cases += "  %s) printf '%%s' '%s' ;;\n" % (pattern, envelope)
    with open(p, "w") as f:
        f.write("#!/bin/sh\n"
                "n=$(cat '" + counter + "' 2>/dev/null || echo 0)\n"
                "n=$((n + 1))\n"
                "echo $n > '" + counter + "'\n"
                "cat > '" + capture_prefix + ".'$n\n"
                "case $n in\n" + cases + "esac\n")
    os.chmod(p, 0o755)
    return d


def _capture(tmp_path, n):
    return (tmp_path / ("request.%d" % n)).read_text()


def _pane_line(rows, start, count=2):
    """The pane line that begins at row `start`, unwrapped.

    The pane wraps a line longer than its width at a space onto the following
    row, and the window frame takes the first and last column of every row;
    the rows are stripped of the frame and joined with the space the wrap
    replaced, so an assertion can read the line as the editor wrote it."""
    return " ".join(r[1:-1].strip() for r in rows[start:start + count])


def _ask_rows_after_reply(tmp_path, env, prompt, reply):
    """Open the chat, send one prompt, wait for its reply; return the rows."""
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        s.key(ALT.AI); s.key("a"); s._drain(0.5)
        s.key(prompt); s.key("\r", delay=1.0)
        assert _seen_within(s, "AI: " + reply, TURN_POLL_S), "\n".join(s.display())
        s._drain(1.0)
        return s.display()


def _agent_rows_with_stub(tmp_path, env):
    """Run one agent task against the stub `claude` to its follow-up prompt."""
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        s.key(ALT.AI); s.key("g")
        s.key("look"); s.key("\r", delay=1.0)
        assert _seen_within(s, FOLLOWUP_LINE, TURN_POLL_S), \
            "the run did not finish:\n" + "\n".join(s.display())
        return s.display()


# @scenario SC-ai-agent-38b
def test_ask_plain_turn_carries_no_project_instructions(tmp_path):
    """Under the default scope a plain Ask neither carries the block nor
    announces the file: the file is for work that acts on the repository."""
    _write_agents_md(tmp_path, SMALL_SIZE)
    capture = str(tmp_path / "request.txt")
    bindir = _stub_claude_capturing(str(tmp_path), capture, CHAT_REPLY)
    rows = _ask_rows_after_reply(tmp_path, _claudecli_env(tmp_path, bindir), "hi", CHAT_REPLY)
    body = open(capture).read()
    assert PROJECT_BLOCK not in body, \
        "a plain Ask carried the project block under the default scope:\n" + body
    assert ANNOUNCE_ANY not in "\n".join(rows), \
        "the pane announced project instructions for a plain Ask:\n" + "\n".join(rows)
    assert "project memory" not in _trace(tmp_path), _trace(tmp_path)


# @scenario SC-ai-agent-38b
def test_agent_carries_project_instructions_under_the_default(tmp_path):
    _write_agents_md(tmp_path, SMALL_SIZE)
    capture = str(tmp_path / "request.txt")
    bindir = _stub_claude_capturing(str(tmp_path), capture, "DONE looked")
    rows = _agent_rows_with_stub(tmp_path, _claudecli_env(tmp_path, bindir))
    body = open(capture).read()
    assert PROJECT_BLOCK in body, body
    assert "keep every function short" in body, body
    assert _row_index(rows, ANNOUNCE) >= 0, \
        "the agent did not announce the project instructions:\n" + "\n".join(rows)


# @scenario SC-ai-agent-38b
def test_chat_carries_project_instructions_from_its_first_tool_turn(tmp_path):
    """The first chat turn is plain and carries nothing; the second answers
    with a tool line, and the request that follows the tool result carries the
    block, as does every later request of the session; the pane says so once."""
    _write_agents_md(tmp_path, SMALL_SIZE)
    first, second, third = "First answer.", "Second answer.", "Third answer."
    bindir = _stub_claude_sequenced(
        str(tmp_path), str(tmp_path / "request"),
        [first, "TOOL read_file t.c", second, third])
    env = _claudecli_env(tmp_path, bindir)
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        s.key(ALT.AI); s.key("a"); s._drain(0.5)
        s.key("hi"); s.key("\r", delay=1.0)
        assert _seen_within(s, "AI: " + first, TURN_POLL_S), "\n".join(s.display())
        s.key("read it"); s.key("\r", delay=1.0)
        assert _seen_within(s, "AI: " + second, TURN_POLL_S), "\n".join(s.display())
        s.key("more"); s.key("\r", delay=1.0)
        assert _seen_within(s, "AI: " + third, TURN_POLL_S), "\n".join(s.display())
        s._drain(1.0)
        rows = s.display()
    assert PROJECT_BLOCK not in _capture(tmp_path, 1), \
        "the first plain turn carried the block:\n" + _capture(tmp_path, 1)
    assert PROJECT_BLOCK not in _capture(tmp_path, 2), \
        "the request the tool line answered already carried the block:\n" + _capture(tmp_path, 2)
    assert PROJECT_BLOCK in _capture(tmp_path, 3), \
        "the request after the tool result does not carry the block:\n" + _capture(tmp_path, 3)
    assert TOOLS_STAND in _capture(tmp_path, 3), _capture(tmp_path, 3)
    assert PROJECT_BLOCK in _capture(tmp_path, 4), \
        "a later turn of the same session dropped the block:\n" + _capture(tmp_path, 4)
    announced = [r for r in rows if ANNOUNCE in r]
    assert len(announced) == 1, \
        "the chat must announce the project instructions once, saw %d:\n%s" \
        % (len(announced), "\n".join(rows))
    assert "(%d bytes)" % SMALL_SIZE in announced[0], announced[0]


# @scenario SC-ai-agent-38b
@pytest.mark.parametrize("how", ["xwperc", "env"])
def test_scope_all_folds_project_instructions_into_a_plain_ask(tmp_path, how):
    _write_agents_md(tmp_path, SMALL_SIZE)
    capture = str(tmp_path / "request.txt")
    bindir = _stub_claude_capturing(str(tmp_path), capture, CHAT_REPLY)
    env = _claudecli_env(tmp_path, bindir)
    if how == "xwperc":
        _write_xwperc(tmp_path, SCOPE_KEY + " : all")
    else:
        env[SCOPE_ENV] = "all"
    rows = _ask_rows_after_reply(tmp_path, env, "hi", CHAT_REPLY)
    body = open(capture).read()
    assert PROJECT_BLOCK in body, \
        "under %s all a plain Ask does not carry the block:\n%s" % (SCOPE_KEY, body)
    assert "keep every function short" in body, body
    assert _row_index(rows, ANNOUNCE) >= 0, \
        "the chat did not announce the project instructions under all:\n" + "\n".join(rows)


# @scenario SC-ai-agent-38b
def test_scope_off_keeps_project_instructions_out_of_the_agent(tmp_path):
    _write_agents_md(tmp_path, SMALL_SIZE)
    _write_xwperc(tmp_path, SCOPE_KEY + " : off")
    capture = str(tmp_path / "request.txt")
    bindir = _stub_claude_capturing(str(tmp_path), capture, "DONE looked")
    rows = _agent_rows_with_stub(tmp_path, _claudecli_env(tmp_path, bindir))
    body = open(capture).read()
    assert PROJECT_BLOCK not in body, \
        "under %s off the agent still carried the block:\n%s" % (SCOPE_KEY, body)
    assert ANNOUNCE_ANY not in "\n".join(rows), \
        "the pane announced project instructions under off:\n" + "\n".join(rows)


# @scenario SC-ai-agent-38c
def test_specific_file_wins_over_agents_md(tmp_path):
    (tmp_path / ".xwpe-ai.md").write_text("# For this editor\n" + SPECIFIC_SENTENCE + "\n")
    (tmp_path / "AGENTS.md").write_text("# For other agents\n" + GENERIC_SENTENCE + "\n")
    capture = str(tmp_path / "request.txt")
    bindir = _stub_claude_capturing(str(tmp_path), capture, "DONE looked")
    rows = _agent_rows_with_stub(tmp_path, _claudecli_env(tmp_path, bindir))
    body = open(capture).read()
    assert SPECIFIC_SENTENCE in body, \
        ".xwpe-ai.md was not the file folded in:\n" + body
    assert GENERIC_SENTENCE not in body, \
        "AGENTS.md was folded in although .xwpe-ai.md exists:\n" + body
    ann_row = _row_index(rows, ANNOUNCE_ANY)
    assert ann_row >= 0, "no announcement:\n" + "\n".join(rows)
    assert ANNOUNCE_SPECIFIC in rows[ann_row], \
        "the announcement does not name .xwpe-ai.md: " + rows[ann_row]
    assert "project memory .xwpe-ai.md" in _trace(tmp_path), _trace(tmp_path)


# @scenario SC-ai-agent-38d
@pytest.mark.parametrize("how", ["xwperc", "env"])
def test_bound_key_cuts_the_file_and_names_itself(tmp_path, how):
    (tmp_path / ".xwpe-ai.md").write_text(BOUNDED_FILE)
    capture = str(tmp_path / "request.txt")
    bindir = _stub_claude_capturing(str(tmp_path), capture, "DONE looked")
    env = _claudecli_env(tmp_path, bindir)
    if how == "xwperc":
        _write_xwperc(tmp_path, "%s : %d" % (BOUND_KEY, BOUND_SMALL))
    else:
        env[BOUND_ENV] = "%d" % BOUND_SMALL
    rows = _agent_rows_with_stub(tmp_path, env)
    body = open(capture).read()
    ann_row = _row_index(rows, ANNOUNCE_SPECIFIC)
    assert ann_row >= 0, "no announcement:\n" + "\n".join(rows)
    expected = "(%d bytes, cut at %d; %s raises it)" % (BOUNDED_FILE_SIZE, BOUND_SMALL, BOUND_KEY)
    line = _pane_line(rows, ann_row)
    assert expected in line, \
        "the cut line does not name the bound and its key: " + line
    first = BOUNDED_FILE[:BOUND_SMALL]
    assert first + "\n\n" in body, \
        "the body does not hold exactly the first %d bytes of the file:\n%s" % (BOUND_SMALL, body)
    assert FIRST_LINE_PAST_BOUND not in body, \
        "the body carries bytes past the bound:\n" + body
