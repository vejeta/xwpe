"""A tool run is named and bounded, every denial is visible, a cut result says so.

While the agent runs a command or a web tool the editor reads no keys, so the
pane must say what is happening: its last line reads "[agent] running <tool>
... (result follows)" from before the run starts until the result lands.  A
command that never ends used to hang the editor with Esc unread; it is now
ended after AIToolTimeout seconds (default 60, here 2 through a seeded xwperc),
the model is told "TOOL ERROR: timed out after N s", the pane says so, no child
of the run is left behind and the next key reaches the editor.

A denied command, web tool or write used to answer the model "(denied by user)"
and show the user nothing; the pane now reads "[agent] denied: <tool> <arg>"
and the trace records the denial.

A command result or a file over 6000 bytes used to be cut silently, so the
model took the first 6000 bytes for the whole; the result now ends with a line
naming the cut and the size, so it knows to ask for less.

The mock backend cannot show what was fed back to the model, so the tests that
assert on the TOOL RESULT stand up a capturing OpenAI-compatible server and
read the second request's body, as the tool-safety tests do.
"""
import json
import os
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

import subprocess

SEED = "int main(void){return 0;}\n"
ESC = "\033"
TYPED_AFTER = "Z9"

TOOL_TIMEOUT_S = 2
RUNNING_LINE = "[agent] running run_command ... (result follows)"
TIMEOUT_RESULT = "TOOL ERROR: timed out after %d s" % TOOL_TIMEOUT_S
TIMEOUT_PANE = "timed out after %d s" % TOOL_TIMEOUT_S
CUT_NOTE = "(output cut at 6000 bytes)"
RESULT_MAX = 6000
OVERSIZE = 7000

# Waits are bounded inside the test (the virtual environment carries no pytest
# timeout plugin): on an unfixed tree a hung command never shows these lines
# and the polls below simply run out, so the test fails instead of hanging.
RUNNING_POLL_S = 1.5
TIMEOUT_POLL_S = 12
TURN_POLL_S = 15


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"autonomous coding agent" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _env(tmp_path, reply, policy):
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


def _screen(s):
    return "\n".join(s.display(drain=0.2))


def _seen_within(s, needle, timeout):
    """Drain the screen until `needle` shows or `timeout` seconds pass."""
    end = time.time() + timeout
    while time.time() < end:
        s._drain(0.25)
        if needle in "\n".join(s.display(drain=0)):
            return True
    return False


def _seed_xwperc(tmp_path, lines):
    """Write a [Programming] section into the test HOME's xwperc.  The driver
    sets HOME to the workdir, so this is the configuration the editor reads."""
    cfg = tmp_path / ".config" / "xwpe"
    cfg.mkdir(parents=True, exist_ok=True)
    (cfg / "xwperc").write_text("[Programming]\n" + "".join(l + "\n" for l in lines))


def _traced_tool_pid(txt):
    m = re.search(r"agent tool pid=(\d+)", txt)
    return int(m.group(1)) if m else None


def _proc_comm(pid):
    """The command name of `pid` from /proc, or None when the process is gone.
    Only ever called for a pid read from the editor's own trace."""
    try:
        with open("/proc/%d/stat" % pid) as fh:
            stat = fh.read()
    except OSError:
        return None
    return stat[stat.index("(") + 1:stat.rindex(")")]


# @scenario SC-ai-agent-40
def test_agent_slow_command_shows_running_line_and_times_out(tmp_path):
    _seed_xwperc(tmp_path, ["AIToolTimeout : %d" % TOOL_TIMEOUT_S])
    reply = "TOOL run_command sleep 30@@TURN@@DONE gave up waiting"
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path, reply, "auto")) as s:
        s.key(ALT.AI); s.key("g")
        s.key("wait for it"); s.key("\r", delay=0.3)
        running = _seen_within(s, RUNNING_LINE, RUNNING_POLL_S)
        timed_out = _seen_within(s, TIMEOUT_PANE, TIMEOUT_POLL_S)
        s._drain(1.0)
        txt = _trace(tmp_path)
        pid = _traced_tool_pid(txt)
        comm = _proc_comm(pid) if pid else None
        s.key(ESC, delay=0.8)                 # leave the pane, back to the file
        s.key(TYPED_AFTER, delay=0.4)
        screen_after = _screen(s)
        s.save()
        on_disk = s.text()

    assert running, "the pane never said the command was running"
    assert timed_out, "the pane never said the command timed out:\n" + screen_after
    assert TIMEOUT_RESULT in txt, \
        "the trace does not carry the timeout fed back to the model:\n" + txt
    assert "agent done" in txt, "the run did not go on after the timeout:\n" + txt
    assert pid is not None, "the trace does not name the tool child's pid:\n" + txt
    assert comm not in ("sleep", "sh", "bash", "dash"), \
        "the timed-out command is still running as pid %d (%s)" % (pid, comm)
    assert TYPED_AFTER in screen_after, \
        "a key typed after the timeout did not reach the editor:\n" + screen_after
    assert TYPED_AFTER in on_disk, \
        "the editor did not take keys after the timeout; file on disk:\n" + on_disk


# @scenario SC-ai-agent-40
def test_agent_quick_command_shows_running_line_and_completes(tmp_path):
    reply = "TOOL run_command true@@TURN@@DONE it ran"
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path, reply, "auto")) as s:
        s.key(ALT.AI); s.key("g")
        s.key("run it"); s.key("\r", delay=1.6)
        s._drain(2.0)
        screen = _screen(s)
    txt = _trace(tmp_path)
    assert RUNNING_LINE in screen, \
        "the pane never said the command was running:\n" + screen
    assert "agent tool pid=" in txt, "the trace does not name the tool child:\n" + txt
    assert "agent done" in txt, txt
    assert "timed out" not in txt, "the timeout fired on an instant command:\n" + txt
    assert "timed out" not in screen, screen


def _agent_server(captured, first_reply):
    """A capturing OpenAI-compatible server: turn 1 answers `first_reply`,
    every later turn answers DONE.  What the editor fed back as the TOOL
    RESULT is read from the second request's body."""
    turn = {"n": 0}

    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            b = json.dumps({"object": "list", "data": [{"id": "m"}]}).encode()
            self.send_response(200)
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)

        def do_POST(self):
            n = int(self.headers.get("Content-Length", 0))
            captured.append(self.rfile.read(n).decode("utf-8", "replace"))
            first = turn["n"] == 0
            turn["n"] += 1
            content = first_reply if first else "DONE finished"
            body = (('data: {"choices":[{"delta":{"content":%s}}]}\n\n'
                     % json.dumps(content)).encode()
                    + b'data: [DONE]\n\n')
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            self.wfile.write(body)

    srv = HTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]


def _seed_openai_xwperc(tmp_path, port, policy):
    _seed_xwperc(tmp_path, ["AIBackend : 1",
                            "AIEndpoint : http://127.0.0.1:%d/v1" % port,
                            "AIModel : m",
                            "AIPolicy : %s" % policy])


def _tool_result_message(request_body):
    """The TOOL RESULT the editor fed back, read from a captured request."""
    msgs = json.loads(request_body)["messages"]
    results = [m["content"] for m in msgs
               if m.get("role") == "user" and m["content"].startswith("TOOL RESULT:")]
    assert results, "no TOOL RESULT message in the request:\n" + request_body[:1500]
    return results[-1]


def _cut_payload(result):
    """The bytes a cut result carries between its header and the cut note."""
    assert result.startswith("TOOL RESULT:\n"), result[:80]
    assert result.endswith("\n" + CUT_NOTE), \
        "a cut result does not end with the cut line:\n...%s" % result[-200:]
    return result[len("TOOL RESULT:\n"):-len("\n" + CUT_NOTE)]


def _run_openai_agent(tmp_path, first_reply, policy, answer=None, goal="do it"):
    """Drive one agent run against the capturing server.  `answer` is the key
    pressed once the approval box shows (None: nothing asks).  Returns the
    captured requests, the screen at the end and the trace."""
    captured = []
    srv, port = _agent_server(captured, first_reply)
    try:
        _seed_openai_xwperc(tmp_path, port, policy)
        env = {"XWPE_AI_ENABLE": "1", "OPENAI_API_KEY": "x",
               "XWPE_AI_TRACE": str(tmp_path / "ai.trace")}
        with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
            s.key(ALT.AI, delay=0.6); s.key("g", delay=0.6)
            s.key(goal, delay=0.1)
            s.key("\r", delay=1.0)
            if answer is not None:
                assert _seen_within(s, "APPROVE?", TURN_POLL_S), \
                    "no approval box under ask:\n" + _screen(s)
                s.key(answer, delay=0.8)
            end = time.time() + TURN_POLL_S
            while time.time() < end and len(captured) < 2:
                s._drain(0.5)
            s._drain(1.0)
            screen = _screen(s)
    finally:
        srv.shutdown()
        srv.server_close()
    return captured, screen, _trace(tmp_path)


# @scenario SC-ai-agent-20
def test_agent_denied_command_is_visible_and_nothing_ran(tmp_path):
    captured, screen, txt = _run_openai_agent(tmp_path, "TOOL run_command echo hi",
                                              "ask", answer="n")
    assert "[agent] denied: run_command echo hi" in screen, \
        "the pane does not show the denial:\n" + screen
    assert "agent deny run_command echo hi" in txt, \
        "the trace does not record the denial:\n" + txt
    assert len(captured) >= 2, \
        "the denied tool did not trigger a follow-up turn:\n" + "\n".join(captured)
    result = _tool_result_message(captured[1])
    assert result == "TOOL RESULT:\n(denied by user)", \
        "the model was told something other than the denial:\n" + result
    assert "agent done" in txt, txt


def _search_server():
    paths = []

    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            paths.append(self.path)
            body = b"<html><body>result</body></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    srv = HTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, paths


# @scenario SC-ai-agent-20
def test_agent_denied_web_search_is_visible_and_nothing_fetched(tmp_path):
    srv, paths = _search_server()
    try:
        env = _env(tmp_path, "TOOL web_search x@@TURN@@DONE nothing to say", "ask")
        env["XWPE_AI_SEARCH_URL"] = "http://127.0.0.1:%d/find?q=%%s" % srv.server_address[1]
        with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
            s.key(ALT.AI); s.key("g")
            s.key("look it up"); s.key("\r", delay=1.0)
            assert _seen_within(s, "APPROVE?", TURN_POLL_S), \
                "no approval box for web_search under ask:\n" + _screen(s)
            s.key("n", delay=1.0)
            s._drain(1.5)
            screen = _screen(s)
    finally:
        srv.shutdown()
        srv.server_close()
    txt = _trace(tmp_path)
    assert "[agent] denied: web_search x" in screen, \
        "the pane does not show the denial:\n" + screen
    assert "agent deny web_search x" in txt, \
        "the trace does not record the denial:\n" + txt
    assert not paths, "a denied web_search still reached the search server: %r" % paths
    assert "agent done" in txt, txt


# @scenario SC-ai-agent-15
def test_agent_cut_command_output_ends_with_the_cut_line(tmp_path):
    cmd = "head -c %d /dev/zero | tr '\\0' x" % OVERSIZE
    captured, screen, txt = _run_openai_agent(tmp_path, "TOOL run_command " + cmd, "auto")
    assert len(captured) >= 2, \
        "the command did not trigger a follow-up turn:\n" + "\n".join(captured)
    payload = _cut_payload(_tool_result_message(captured[1]))
    assert payload == "x" * RESULT_MAX, \
        "the result carries %d bytes of output, not %d" % (len(payload), RESULT_MAX)


# @scenario SC-ai-agent-14
def test_agent_cut_file_ends_with_the_cut_line(tmp_path):
    (tmp_path / "big.txt").write_text("y" * OVERSIZE)
    captured, screen, txt = _run_openai_agent(tmp_path, "TOOL read_file big.txt", "auto")
    assert len(captured) >= 2, \
        "read_file did not trigger a follow-up turn:\n" + "\n".join(captured)
    payload = _cut_payload(_tool_result_message(captured[1]))
    assert payload == "y" * RESULT_MAX, \
        "the result carries %d bytes of the file, not %d" % (len(payload), RESULT_MAX)
