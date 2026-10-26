"""Hosted engine: permission requests classed like the built-in tools, Esc interrupts.

Alt-G g with the external engine runs an agent CLI as a persistent stream-json
session.  The CLI is replaced here by a deterministic fake (XWPE_AI_HOST_CMD)
that asks for a tool through a can_use_tool control request, or streams a first
text block and then waits inside its turn so the test can press Esc.

Two contracts are checked.  Under the dial position "edits" the CLI's requests
follow the same classes as the editor's own tools: Bash and the web tools ask
with the command box, the file-editing tools are auto-approved, anything else
asks; a denial reaches the CLI and is shown in the pane.  Esc during a hosted
turn sends the engine its interrupt request and the turn ends with the session
alive; an engine that ignores the request is ended at AIToolTimeout.

Needs a build with --enable-ai-agent-host; skips otherwise.
"""
import os
import time
import subprocess
import textwrap
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN


def _host_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"AIHostAdapter text needs AIHostCommand" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _host_build(),
                                reason="built without --enable-ai-agent-host")

# The fake asks for one tool and records the editor's answer on disk, so the
# test reads what the CLI received rather than what the pane claims.
MOCK_PERM = textwrap.dedent('''\
    import sys, json
    def emit(o):
        sys.stdout.write(json.dumps(o) + "\\n"); sys.stdout.flush()
    sys.stdin.readline()                         # the user turn
    emit({"type": "system", "subtype": "init", "session_id": "m"})
    emit({"type": "control_request", "request_id": "r1", "request": {
        "subtype": "can_use_tool", "tool_name": %(tool)r, "input": %(input)s}})
    resp = sys.stdin.readline()                   # the editor's control_response
    with open("perm_resp.txt", "w") as fh:
        fh.write(resp)
    allowed = '"behavior":"allow"' in resp.replace(" ", "")
    emit({"type": "assistant", "message": {"content": [
        {"type": "text", "text": "ALLOWEDPATH" if allowed else "DENIEDPATH"}]}})
    emit({"type": "result", "subtype": "success", "is_error": False, "result": "done"})
''')

WEB_URL = "https://example.org/page"


def _wait(s, needle, tmo=12):
    end = time.time() + tmo
    while time.time() < end:
        s._drain(0.5)
        if needle in "\n".join(s.display()):
            return True
    return False


def _perm_env(tmp_path, mock, trace, policy="edits"):
    # The dial is the subject, so the web switch is answered: the switch is
    # checked before the dial, and an unanswered one would refuse the hosted
    # engine's WebFetch before the permission box could appear.
    return {"XWPE_AI_ENABLE": "1", "XWPE_AI_AGENT_ENGINE": "claude-code",
            "XWPE_AI_POLICY": policy, "XWPE_AI_HOST_CMD": "python3 %s" % mock,
            "XWPE_AI_WEB": "1",
            "XWPE_AI_TRACE": str(trace)}


def _perm_run(tmp_path, tool, tool_input, answer_key):
    """One hosted turn in which the fake asks for `tool` under the edits dial.
    `answer_key` is pressed if the command box appears (None = expect no box).
    Returns (trace text, screen, what the fake received)."""
    mock = tmp_path / "mock_perm.py"
    mock.write_text(MOCK_PERM % {"tool": tool, "input": tool_input})
    trace = tmp_path / "h.trace"
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=_perm_env(tmp_path, mock, trace), filename="t.c") as s:
        s._drain(0.5)
        s.key(ALT.AI); s.key("g"); s._drain(0.5)
        s.key("use the tool"); s.key("\r", delay=0.8)
        asked = _wait(s, "APPROVE", tmo=6)
        if answer_key is not None:
            assert asked, "no command box for %s under edits:\n%s" % (
                tool, "\n".join(s.display()))
            s.key(answer_key, delay=0.8)
        _wait(s, "PATH", tmo=8)
        disp = "\n".join(s.display())
    resp_file = tmp_path / "perm_resp.txt"
    resp = resp_file.read_text() if resp_file.exists() else ""
    return trace.read_text(), disp, asked, resp


# @scenario SC-ai-agent-51
def test_host_edits_webfetch_asks_and_denial_is_visible(tmp_path):
    txt, disp, asked, resp = _perm_run(tmp_path, "WebFetch", '{"url": %r}' % WEB_URL, "n")
    assert asked, "WebFetch was not asked for under edits:\n" + disp
    assert "host permission tool=WebFetch allow=0" in txt, txt
    assert '"behavior":"deny"' in resp.replace(" ", ""), \
        "the CLI did not receive the deny response:\n" + resp
    assert "DENIEDPATH" in disp, "deny was not sent to the agent:\n" + disp
    assert "denied: WebFetch" in disp, "no denied line in the pane:\n" + disp
    # The box has to say what would leave the machine.  It read "APPROVE?
    # WebFetch" with nothing after it, so the answer was a guess, and the
    # denied line ended in a dangling colon.
    assert WEB_URL in disp, "the box did not say which page it would fetch:\n" + disp


# @scenario SC-ai-agent-51
def test_host_edits_write_is_auto_approved_without_a_box(tmp_path):
    txt, disp, asked, resp = _perm_run(tmp_path, "Write", '{"file_path": "t.c"}', None)
    assert not asked, "a command box appeared for Write under edits:\n" + disp
    assert "auto-approved (edits)" in disp, "no auto-approved line for Write:\n" + disp
    assert "host permission tool=Write allow=1" in txt, txt
    assert '"behavior":"allow"' in resp.replace(" ", ""), \
        "the CLI did not receive the allow response:\n" + resp
    assert "ALLOWEDPATH" in disp, "allow was not sent to the agent:\n" + disp


# @scenario SC-ai-agent-51
def test_host_edits_read_asks(tmp_path):
    txt, disp, asked, resp = _perm_run(tmp_path, "Read", '{"file_path": "t.c"}', "y")
    assert asked, "Read was auto-approved under edits (anything else asks):\n" + disp
    assert "host permission tool=Read allow=1" in txt, txt
    assert "ALLOWEDPATH" in disp, "allow was not sent to the agent:\n" + disp


# The fake streams a first text block, then waits inside the turn for a line
# on stdin.  An interrupt control request ends the turn with the normal result
# and leaves the session alive for the next user turn, which is answered at
# once.  The request it received is recorded on disk.
MOCK_INTERRUPT = textwrap.dedent('''\
    import sys, json, select
    def emit(o):
        sys.stdout.write(json.dumps(o) + "\\n"); sys.stdout.flush()
    turn = 0
    while True:
        line = sys.stdin.readline()
        if not line:
            break
        if json.loads(line).get("type") != "user":
            continue
        turn += 1
        emit({"type": "system", "subtype": "init", "session_id": "mock-int"})
        if turn == 1:
            emit({"type": "assistant", "message": {"content": [
                {"type": "text", "text": "FIRSTBLOCK streamed before the wait"}]}})
            r, _, _ = select.select([sys.stdin], [], [], 40)
            if not r:
                emit({"type": "result", "subtype": "success", "is_error": False,
                      "result": "finished without an interrupt"})
                continue
            req = sys.stdin.readline()
            with open("interrupt.txt", "w") as fh:
                fh.write(req)
            o = json.loads(req)
            if o.get("type") == "control_request" and \\
               o.get("request", {}).get("subtype") == "interrupt":
                emit({"type": "result", "subtype": "success", "is_error": False,
                      "result": "interrupted"})
            continue
        emit({"type": "assistant", "message": {"content": [
            {"type": "text", "text": "SECONDTURN answered by the same session"}]}})
        emit({"type": "result", "subtype": "success", "is_error": False,
              "result": "done"})
''')

# The fake streams a first text block and then ignores everything on stdin,
# never ending the turn: the editor's bound has to end the session.  What it
# received is recorded OUTSIDE the workdir, so the record is not a file the
# session's checkpoint would list as a change (that path is the review's).
MOCK_IGNORE = textwrap.dedent('''\
    import sys, json
    def emit(o):
        sys.stdout.write(json.dumps(o) + "\\n"); sys.stdout.flush()
    sys.stdin.readline()                         # the user turn
    emit({"type": "system", "subtype": "init", "session_id": "mock-deaf"})
    emit({"type": "assistant", "message": {"content": [
        {"type": "text", "text": "FIRSTBLOCK streamed before the wait"}]}})
    while True:
        line = sys.stdin.readline()
        if not line:
            break
        with open(%(record)r, "a") as fh:
            fh.write(line)
''')

TOOL_TIMEOUT_S = 2


def _seed_xwperc(tmp_path, lines):
    """Write a [Programming] section into the test HOME's xwperc.  The driver
    sets HOME to the workdir, so this is the configuration the editor reads."""
    cfg = tmp_path / ".config" / "xwpe"
    cfg.mkdir(parents=True, exist_ok=True)
    (cfg / "xwperc").write_text("[Programming]\n" + "".join(l + "\n" for l in lines))


def _host_env(mock, trace):
    return {"XWPE_AI_ENABLE": "1", "XWPE_AI_AGENT_ENGINE": "claude-code",
            "XWPE_AI_HOST_CMD": "python3 %s" % mock, "XWPE_AI_TRACE": str(trace)}


# @scenario SC-ai-agent-53b
def test_host_esc_interrupts_turn_and_session_continues(tmp_path):
    mock = tmp_path / "mock_interrupt.py"
    mock.write_text(MOCK_INTERRUPT)
    trace = tmp_path / "h.trace"
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=_host_env(mock, trace), filename="t.c") as s:
        s._drain(0.5)
        s.key(ALT.AI); s.key("g"); s._drain(0.5)
        s.key("think for a while"); s.key("\r", delay=0.8)
        assert _wait(s, "FIRSTBLOCK", tmo=8), "the first text block never streamed"
        s.key("\033"); s._drain(0.8)                              # Esc interrupts
        assert _wait(s, "interrupting", tmo=4), \
            "Esc during the hosted turn did not say it was interrupting:\n" + \
            "\n".join(s.display())
        assert _wait(s, "turn interrupted", tmo=8), \
            "the pane never showed the interrupted line:\n" + "\n".join(s.display())
        # a following turn is answered by the same session
        s.key(ALT.AI); s.key("g"); s._drain(0.5)
        s.key("again"); s.key("\r", delay=0.8)
        assert _wait(s, "SECONDTURN", tmo=8), \
            "the session did not answer a turn after the interrupt:\n" + \
            "\n".join(s.display())
        disp = "\n".join(s.display())
    req_file = tmp_path / "interrupt.txt"
    assert req_file.exists(), "the fake never received a line after Esc"
    req = req_file.read_text().replace(" ", "")
    assert '"type":"control_request"' in req and '"subtype":"interrupt"' in req \
        and '"request_id":"' in req, "not an interrupt control request:\n" + req
    txt = trace.read_text()
    assert "host interrupt" in txt, "the interrupt was not traced:\n" + txt
    assert txt.count("host start") == 1, "a new session was spawned:\n" + txt
    assert txt.count("host send") == 2 and txt.count("host result") == 2, \
        "the second turn did not continue the session:\n" + txt
    assert "session ended" not in disp, "the session was ended by the interrupt:\n" + disp


# @scenario SC-ai-agent-53b
def test_host_esc_ignored_ends_session_at_tool_timeout(tmp_path):
    mock = tmp_path / "mock_ignore.py"
    ignored = tmp_path.parent / (tmp_path.name + ".ignored.txt")   # beside, not inside
    mock.write_text(MOCK_IGNORE % {"record": str(ignored)})
    trace = tmp_path / "h.trace"
    _seed_xwperc(tmp_path, ["AIToolTimeout : %d" % TOOL_TIMEOUT_S])
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=_host_env(mock, trace), filename="t.c") as s:
        s._drain(0.5)
        s.key(ALT.AI); s.key("g"); s._drain(0.5)
        s.key("think for a while"); s.key("\r", delay=0.8)
        assert _wait(s, "FIRSTBLOCK", tmo=8), "the first text block never streamed"
        t0 = time.time()
        s.key("\033"); s._drain(0.8)                              # Esc interrupts
        # The session ends as at the prompt: the changeset review runs first
        # (its Messages window says "no files changed" here), then the pane
        # comes to the front with the session-ended line.
        assert _wait(s, "session ended", tmo=TOOL_TIMEOUT_S + 6), \
            "the deaf engine was not ended at AIToolTimeout:\n" + "\n".join(s.display())
        took = time.time() - t0
        disp = "\n".join(s.display())
        assert "interrupting" in disp, \
            "Esc during the hosted turn did not say it was interrupting:\n" + disp
        # the editor takes keys again: a menu opens and closes
        s.key(ALT.FILE, delay=0.6)
        assert "Save" in "\n".join(s.display()), "the editor did not take a key:\n" + \
            "\n".join(s.display())
        s.key("\033", delay=0.5)
    txt = trace.read_text()
    assert "host interrupt sent" in txt, "the interrupt was not traced:\n" + txt
    assert took < TOOL_TIMEOUT_S + 6, "the bound took %.1fs" % took
    assert "host interrupt deadline" in txt, "the bound did not end the session:\n" + txt
    assert ignored.exists() and "interrupt" in ignored.read_text(), \
        "the fake never received the interrupt request"


# The fake streams a first text block and waits.  On the interrupt request it
# does not end the turn: it asks for a tool, so the editor's permission box is
# up when the bound falls due, then records the answer and goes silent.
MOCK_ASK_AFTER_INTERRUPT = textwrap.dedent('''\
    import sys, json
    def emit(o):
        sys.stdout.write(json.dumps(o) + "\\n"); sys.stdout.flush()
    sys.stdin.readline()                         # the user turn
    emit({"type": "system", "subtype": "init", "session_id": "mock-modal"})
    emit({"type": "assistant", "message": {"content": [
        {"type": "text", "text": "FIRSTBLOCK streamed before the wait"}]}})
    sys.stdin.readline()                         # the interrupt request
    emit({"type": "control_request", "request_id": "r9", "request": {
        "subtype": "can_use_tool", "tool_name": "Read",
        "input": {"file_path": "t.c"}}})
    resp = sys.stdin.readline()                  # the editor's answer
    with open(%(record)r, "a") as fh:
        fh.write(resp)
    while sys.stdin.readline():
        pass
''')


# @scenario SC-ai-agent-53b
def test_host_deadline_under_permission_modal_waits_for_the_answer(tmp_path):
    mock = tmp_path / "mock_modal.py"
    record = tmp_path.parent / (tmp_path.name + ".modal.txt")      # beside, not inside
    mock.write_text(MOCK_ASK_AFTER_INTERRUPT % {"record": str(record)})
    trace = tmp_path / "h.trace"
    _seed_xwperc(tmp_path, ["AIToolTimeout : %d" % TOOL_TIMEOUT_S])
    env = _host_env(mock, trace)
    env["XWPE_AI_POLICY"] = "edits"
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, filename="t.c") as s:
        s._drain(0.5)
        s.key(ALT.AI); s.key("g"); s._drain(0.5)
        s.key("think for a while"); s.key("\r", delay=0.8)
        assert _wait(s, "FIRSTBLOCK", tmo=8), "the first text block never streamed"
        s.key("\033"); s._drain(0.3)                              # Esc interrupts
        assert _wait(s, "APPROVE", tmo=6), \
            "the permission box did not appear after the interrupt:\n" + \
            "\n".join(s.display())
        # Let the bound fall due (AIToolTimeout) while the box is up, and then
        # some: the turn must neither end nor re-enter, and the box stays.
        s._drain(TOOL_TIMEOUT_S + 2)
        disp = "\n".join(s.display())
        mid = trace.read_text()
        assert "APPROVE" in disp, "the box went away before it was answered:\n" + disp
        assert "session ended" not in disp, \
            "the session ended under the permission box:\n" + disp
        assert "host interrupt deadline" not in mid, \
            "the turn ended while the box was up:\n" + mid
        assert "host permission" not in mid, "the box was answered by itself:\n" + mid
        assert not record.exists(), "the engine got an answer nobody gave"
        # The box still reads its key, and the answer ends the turn once.
        s.key("y", delay=0.8)
        assert _wait(s, "session ended", tmo=TOOL_TIMEOUT_S + 6), \
            "the turn did not end once the box was answered:\n" + "\n".join(s.display())
        s._drain(1.0)
    txt = trace.read_text()
    assert "host permission tool=Read allow=1" in txt, \
        "the box did not take its key:\n" + txt
    assert txt.count("host interrupt deadline") == 1, \
        "the bound did not end the turn exactly once:\n" + txt
    assert txt.index("host permission tool=Read allow=1") < \
        txt.index("host interrupt deadline"), "the turn ended before the answer:\n" + txt
    assert txt.count("host end notice") == 1,"the session ended more than once:\n" + txt
    assert record.exists() and '"behavior":"allow"' in record.read_text().replace(" ", ""), \
        "the engine did not receive the answer"
