"""External agent host (Claude Code native): stream + on-disk edit -> buffer.

Alt-G h runs an external agent CLI as a persistent stream-json session.  The CLI
is replaced here by a deterministic mock (XWPE_AI_HOST_CMD) that, for each user
turn, writes the open file on disk and emits stream-json events (assistant text,
a Write tool_use, a result).  The host must render the text/tool lines in the
pane and, on the tool's file write, reload the open buffer -- so the editor shows
what the agent wrote, revertible with one Ctrl-U.

Needs a build with --enable-ai-agent-host (the menu entry and the code only exist
then); skips otherwise.
"""
import os
import time
import subprocess
import textwrap
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

MARK = "HOSTWROTETHIS"


def _host_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"AIHostAdapter text needs AIHostCommand" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _host_build(),
                                reason="built without --enable-ai-agent-host")

MOCK = textwrap.dedent('''\
    import sys, json
    def emit(o):
        sys.stdout.write(json.dumps(o) + "\\n"); sys.stdout.flush()
    while True:
        line = sys.stdin.readline()
        if not line:
            break
        with open("t.c", "w") as fh:
            fh.write("int main(void){return 0;} /* %s */\\n")
        emit({"type": "system", "subtype": "init", "session_id": "mock-123"})
        emit({"type": "assistant", "message": {"content": [
            {"type": "text", "text": "Editing t.c for you now."},
            {"type": "tool_use", "name": "Write", "input": {"file_path": "t.c"}}]}})
        emit({"type": "result", "subtype": "success", "is_error": False,
              "result": "edited t.c"})
''' % MARK)


def test_host_streams_and_reloads_edited_buffer(tmp_path):
    mock = tmp_path / "mock_host.py"
    mock.write_text(MOCK)
    trace = tmp_path / "h.trace"
    env = {"XWPE_AI_ENABLE": "1",
           "XWPE_AI_AGENT_ENGINE": "claude-code",     # Agent runs the hosted CLI
           "XWPE_AI_HOST_CMD": "python3 %s" % mock,
           "XWPE_AI_TRACE": str(trace)}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, filename="t.c") as s:
        s._drain(0.5)
        s.key(ALT.AI); s.key("g"); s._drain(0.5)      # Alt-G g -> host engine
        s.key("please edit t.c"); s.key("\r", delay=1.0)
        end = time.time() + 12
        while time.time() < end:
            s._drain(0.6)
            if "host result" in (trace.read_text() if trace.exists() else ""):
                break
        disp = "\n".join(s.display())
    txt = trace.read_text()
    assert "host send" in txt, "host turn was not sent:\n" + txt
    assert "host edit=" in txt, "the file-edit event was not handled:\n" + txt
    assert "host result" in txt, "the turn never completed:\n" + txt
    # the open buffer was reloaded from what the agent wrote (was not there before)
    assert MARK in disp, "the edited file was not reloaded into its buffer:\n" + disp
    # the tool activity was surfaced as a status line
    assert "host tool=Write" in txt, "the tool_use was not shown:\n" + txt


MOCK_THINK = textwrap.dedent('''\
    import sys, json
    def emit(o):
        sys.stdout.write(json.dumps(o) + "\\n"); sys.stdout.flush()
    while True:
        line = sys.stdin.readline()
        if not line:
            break
        for _ in range(6):                       # noisy thinking-token status events
            emit({"type": "system", "subtype": "thinking_tokens", "tokens": 42})
        emit({"type": "assistant", "message": {"content": [
            {"type": "text", "text": "UNIQUEANSWER from the agent"}]}})
        # result echoes the same text -- must NOT be printed a second time
        emit({"type": "result", "subtype": "success", "is_error": False,
              "result": "UNIQUEANSWER from the agent"})
''')


def test_host_suppresses_thinking_and_does_not_double_answer(tmp_path):
    mock = tmp_path / "mock_think.py"
    mock.write_text(MOCK_THINK)
    trace = tmp_path / "h.trace"
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_AGENT_ENGINE": "claude-code",
           "XWPE_AI_HOST_CMD": "python3 %s" % mock, "XWPE_AI_TRACE": str(trace)}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, filename="t.c") as s:
        s._drain(0.5)
        s.key(ALT.AI); s.key("g"); s._drain(0.5)
        s.key("hello"); s.key("\r", delay=1.0)
        end = time.time() + 12
        while time.time() < end:
            s._drain(0.6)
            if "host result" in (trace.read_text() if trace.exists() else ""):
                break
        disp = "\n".join(s.display())
    assert "thinking_tokens" not in disp, "thinking status events spammed the pane:\n" + disp
    assert disp.count("UNIQUEANSWER") == 1, \
        "the answer was printed %d times (expected 1):\n%s" % (disp.count("UNIQUEANSWER"), disp)


MOCK_PERM = textwrap.dedent('''\
    import sys, json
    def emit(o):
        sys.stdout.write(json.dumps(o) + "\\n"); sys.stdout.flush()
    sys.stdin.readline()                         # the user turn
    emit({"type": "system", "subtype": "init", "session_id": "m"})
    emit({"type": "control_request", "request_id": "r1", "request": {
        "subtype": "can_use_tool", "tool_name": "Bash",
        "input": {"command": "echo hi"}}})
    resp = sys.stdin.readline()                   # our control_response
    allowed = '"behavior":"allow"' in resp.replace(" ", "")
    emit({"type": "assistant", "message": {"content": [
        {"type": "text", "text": "ALLOWEDPATH" if allowed else "DENIEDPATH"}]}})
    emit({"type": "result", "subtype": "success", "is_error": False, "result": "done"})
''')


def _wait(s, needle, tmo=12):
    end = time.time() + tmo
    while time.time() < end:
        s._drain(0.5)
        if needle in "\n".join(s.display()):
            return True
    return False


def _perm_run(tmp_path, answer_key):
    mock = tmp_path / "mock_perm.py"
    mock.write_text(MOCK_PERM)
    trace = tmp_path / "h.trace"
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_AGENT_ENGINE": "claude-code",
           "XWPE_AI_POLICY": "ask", "XWPE_AI_HOST_CMD": "python3 %s" % mock,
           "XWPE_AI_TRACE": str(trace)}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, filename="t.c") as s:
        s._drain(0.5)
        s.key(ALT.AI); s.key("g"); s._drain(0.5)
        s.key("run echo hi"); s.key("\r", delay=0.8)
        assert _wait(s, "APPROVE"), "no per-tool permission prompt appeared"
        s.key(answer_key, delay=0.8)
        _wait(s, "done", tmo=8)
        disp = "\n".join(s.display())
    return trace.read_text(), disp


def test_host_permission_allow(tmp_path):
    txt, disp = _perm_run(tmp_path, "y")            # allow the tool
    assert "host permission tool=Bash allow=1" in txt, txt
    assert "ALLOWEDPATH" in disp, "allow was not sent to the agent:\n" + disp


def test_host_permission_deny(tmp_path):
    txt, disp = _perm_run(tmp_path, "n")            # deny the tool
    assert "host permission tool=Bash allow=0" in txt, txt
    assert "DENIEDPATH" in disp, "deny was not sent to the agent:\n" + disp


def test_host_second_turn_continues_same_session(tmp_path):
    mock = tmp_path / "mock_host.py"
    mock.write_text(MOCK)
    trace = tmp_path / "h.trace"
    env = {"XWPE_AI_ENABLE": "1",
           "XWPE_AI_AGENT_ENGINE": "claude-code",
           "XWPE_AI_HOST_CMD": "python3 %s" % mock,
           "XWPE_AI_TRACE": str(trace)}

    def _turn(s, text):
        s.key(ALT.AI); s.key("g"); s._drain(0.5)
        s.key(text); s.key("\r", delay=1.0)
        end = time.time() + 12
        while time.time() < end:
            s._drain(0.6)
            n = (trace.read_text() if trace.exists() else "").count("host result")
            if n >= _turn.want:
                break

    _turn.want = 0
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, filename="t.c") as s:
        s._drain(0.5)
        _turn.want = 1; _turn(s, "first change")
        _turn.want = 2; _turn(s, "second change")
    txt = trace.read_text()
    # both turns went to ONE persistent child (single "host start"), two sends/results
    assert txt.count("host start") == 1, "a new session was spawned per turn:\n" + txt
    assert txt.count("host send") == 2 and txt.count("host result") == 2, \
        "the second turn did not continue the session:\n" + txt
