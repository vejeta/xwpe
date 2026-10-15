"""Errors are errors, the final newline is content, a failed write and a
hosted checkpoint are told, and a closed window repaints at once.

A server that closed the connection with no bytes, or answered something that
was not HTTP, ended an agent run as a finished turn: "[agent] done" with the
trace "agent answer (no marker)".  The user could not tell a dead server from
a finished task.  The stream layer now counts a turn as finished only when the
protocol said so (the backend's end marker, a complete HTTP body or an error
status); a peer that merely hangs up is a transport error, the pane says
"[AI] transport error", the run stops and nothing is applied.  An HTTP error
with a JSON message used to show "backend error: no result" because the pane
line was built after the stream that held the message was freed; the message
is now kept and shown.

write_file and apply_patch took the body up to the newline before @@END, so
"hello" followed by a newline and the marker landed as five bytes and every
file the agent wrote lacked its final newline.  The newline that ends the
last body line is content, as the spec decided.

"(write failed)" reached the model only; the pane's last word was
"auto-approved" and the user believed the file existed.  The pane now says
"[agent] write failed: <path>" and the trace records it.

Ctrl-W on the run's window during a turn dropped the run with the right line,
but the screen was repainted only when the stream's child exited, because
freeing the stream waited for that child; the closed window stayed painted
for the whole reply delay.  The child is ended first, so the close repaints
at once.

The hosted engine took its checkpoint at session start and said nothing; it
now shows the same "[agent] checkpoint taken" line as the built-in loop.

The mock backend cannot fail below HTTP or show what was fed back, so the
transport, backend-error and failed-write tests stand up a fault-injecting
OpenAI-compatible server on the loopback and read the editor's requests, as
the run-visibility tests do.
"""
import json
import os
import socket
import subprocess
import textwrap
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN, pane_rows, pane_logical_lines

SEED = "int main(void){return 0;}\n"
CTRL_W = "\x17"

TRANSPORT_LINE = "[AI] transport error"
BACKEND_ERROR_LINE = "[agent] working: backend error:"
SERVER_MESSAGE = "model m is on fire"
CHECKPOINT_LINE = "[agent] checkpoint taken"
ENGINE_LINE = "[agent] engine:"
FOLLOWUP_LINE = "type a follow-up"
REVIEW_PROMPT = "a = keep all"
WRITE_BOX = " Write "
DROP_LINE = "[AI agent] dropped - its window was closed"
WRITE_FAILED_PATH = "nodir/sub/x.txt"

TURN_POLL_S = 20
REVIEW_POLL_S = 25
TRACE_POLL_S = 15

# The driver scales its own key/drain waits by XWPE_TEST_WAIT_SCALE (the
# sanitizer build runs at 2, the Debian autopkgtest at 3); a fixed
# XWPE_AI_MOCK_DELAY_MS does not, since the mock's reply lands at the same
# wall-clock time regardless of that factor.  The reply is held back long
# enough for the close to be sampled well before the stream's child would
# have exited on its own; the unfixed tree repaints only at that exit, so the
# bound below separates the two by seconds, scaled by hand so the separation
# holds at every scale.
WAIT_SCALE = float(os.environ.get("XWPE_TEST_WAIT_SCALE", "1"))
SLOW_REPLY_MS = int(9000 * WAIT_SCALE)
REPAINT_BOUND_S = 2.0 * WAIT_SCALE
TIMELINE_S = 11
APPLY_NOTHING_DELAY_MS = int(5000 * WAIT_SCALE)


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"autonomous coding agent" in out
    except Exception:
        return False


def _host_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"AIHostAdapter text needs AIHostCommand" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _env(tmp_path, reply, policy="ask", **more):
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_POLICY": policy,
        "XWPE_AI_TRACE": str(tmp_path / "ai.trace"),
    }
    env.update(more)
    return env


def _trace(tmp_path):
    trace = tmp_path / "ai.trace"
    return trace.read_text() if trace.exists() else ""


def _screen(s, drain=0.2):
    return "\n".join(s.display(drain=drain))


def _pane_text(disp):
    """The pane's logical lines on a screen snapshot, as one searchable
    text.  The review's Messages window halves the pane, so the server's
    message wraps over two rows; the rows are joined back first."""
    return "\n".join(pane_logical_lines(pane_rows(disp)))


def _pane_text_with_review(rows, review):
    """The pane's lines at the end of the run and, when a review opened,
    the pane's lines while it was up: the message is shown before the
    review and kept after it."""
    text = _pane_text(rows)
    if review is not None:
        text = text + "\n" + _pane_text(review)
    return text


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


def _seed_xwperc(tmp_path, lines):
    """Write a [Programming] section into the test HOME's xwperc.  The driver
    sets HOME to the workdir, so this is the configuration the editor reads."""
    cfg = tmp_path / ".config" / "xwpe"
    cfg.mkdir(parents=True, exist_ok=True)
    (cfg / "xwperc").write_text("[Programming]\n" + "".join(l + "\n" for l in lines))


def _start_agent(s, task, delay=1.0):
    s.key(ALT.AI); s.key("g")
    s.key(task); s.key("\r", delay=delay)


def _row_index(rows, needle):
    return next((i for i, r in enumerate(rows) if needle in r), -1)


# ---------------------------------------------------------------- fault server
def _fault_server(captured, replies, status_for_turn=None, fault_for_turn=None):
    """An OpenAI-compatible server that records every request body and can
    misbehave on a chosen turn.  `replies[n]` answers request n (the last one
    repeats).  `status_for_turn(n)` may return an HTTP status other than 200
    for request n; the body is then a JSON error carrying SERVER_MESSAGE.
    `fault_for_turn(n)` may return "close" (accept the request, answer with no
    bytes, close), "garbage" (bytes that are not HTTP) or "error_event" (HTTP
    200, then one SSE error event carrying SERVER_MESSAGE and no end marker,
    the shape a failing bridge sends) for request n."""
    turn = {"n": 0}

    class H(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

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
            i = turn["n"]
            turn["n"] += 1
            fault = fault_for_turn(i) if fault_for_turn else None
            if fault == "close":
                self.close_connection = True
                self.wfile.flush()
                self.connection.shutdown(socket.SHUT_RDWR)
                return
            if fault == "garbage":
                self.wfile.write(b"\x00\xff this is not http at all\r\n\r\n")
                self.close_connection = True
                return
            if fault == "error_event":
                event = json.dumps({"error": {"message": "%s (turn %d)" % (SERVER_MESSAGE, i),
                                              "type": "server_error"}})
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.end_headers()
                self.wfile.write(("data: %s\n\n" % event).encode())
                self.close_connection = True
                return
            status = status_for_turn(i) if status_for_turn else 200
            if status != 200:
                body = json.dumps({"error": {"message": "%s (turn %d)" % (SERVER_MESSAGE, i),
                                             "type": "server_error"}}).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            content = replies[i] if i < len(replies) else replies[-1]
            body = (('data: {"choices":[{"delta":{"content":%s}}]}\n\n'
                     % json.dumps(content)).encode() + b'data: [DONE]\n\n')
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


def _openai_env(tmp_path):
    return {"XWPE_AI_ENABLE": "1", "OPENAI_API_KEY": "x",
            "XWPE_AI_TRACE": str(tmp_path / "ai.trace")}


def _tool_results(request_body):
    """Every TOOL RESULT user message in a captured request, in order."""
    msgs = json.loads(request_body)["messages"]
    return [m["content"] for m in msgs
            if m.get("role") == "user" and m["content"].startswith("TOOL RESULT:")]


def _run_openai(tmp_path, replies, policy, want_requests, status_for_turn=None,
                fault_for_turn=None, timeout=TURN_POLL_S):
    """Drive one agent run against the fault server.  Waits for `want_requests`
    requests, then for the run to settle; a review that opens is kept whole
    (keys "a") so the screen after it is read too.  Returns the captured
    requests, the final screen rows, the review rows (or None) and the trace."""
    captured = []
    srv, port = _fault_server(captured, replies, status_for_turn, fault_for_turn)
    try:
        _seed_openai_xwperc(tmp_path, port, policy)
        with WpeSession(str(tmp_path), SEED, env_extra=_openai_env(tmp_path)) as s:
            s.key(ALT.AI, delay=0.6); s.key("g", delay=0.6)
            s.key("do it", delay=0.1); s.key("\r", delay=1.0)
            end = time.time() + timeout
            while time.time() < end and len(captured) < want_requests:
                s._drain(0.5)
            _seen_within(s, FOLLOWUP_LINE, 8)
            s._drain(1.0)
            rows = s.display()
            review = None
            if REVIEW_PROMPT in "\n".join(rows):
                review = list(rows)
                s.key("a", delay=1.0)
                _seen_within(s, FOLLOWUP_LINE, 5)
                rows = s.display()
    finally:
        srv.shutdown()
        srv.server_close()
    return captured, rows, review, _trace(tmp_path)


def _assert_transport_error_ended_the_run(rows, txt):
    text = "\n".join(rows)
    assert TRANSPORT_LINE in text, \
        "a dead connection was not reported as a transport error:\n" + text
    assert "[agent] done" not in text, "the run was shown as finished:\n" + text
    assert "agent answer (no marker)" not in txt, \
        "the trace took the empty answer for a finished turn:\n" + txt
    assert "agent done" not in txt, txt
    assert FOLLOWUP_LINE not in text, \
        "the follow-up line was armed after a transport error:\n" + text


# @scenario SC-ai-agent-55
def test_connection_closed_with_no_bytes_is_a_transport_error(tmp_path):
    captured, rows, review, txt = _run_openai(
        tmp_path, ["TOOL list_dir .", "DONE never"], "ask", 2,
        fault_for_turn=lambda n: "close" if n == 1 else None)
    assert len(captured) >= 2, "the second turn was never requested:\n" + txt
    _assert_transport_error_ended_the_run(rows, txt)


# @scenario SC-ai-agent-55
def test_non_http_answer_is_a_transport_error(tmp_path):
    captured, rows, review, txt = _run_openai(
        tmp_path, ["TOOL list_dir .", "DONE never"], "ask", 2,
        fault_for_turn=lambda n: "garbage" if n == 1 else None)
    assert len(captured) >= 2, "the second turn was never requested:\n" + txt
    _assert_transport_error_ended_the_run(rows, txt)


# @scenario SC-ai-agent-54
def test_backend_error_mid_loop_shows_the_servers_message(tmp_path):
    captured, rows, review, txt = _run_openai(
        tmp_path, ["TOOL write_file a.txt\nAAA\n@@END", "DONE never"], "auto", 2,
        status_for_turn=lambda n: 500 if n == 1 else 200)
    text = _pane_text_with_review(rows, review)
    assert BACKEND_ERROR_LINE in text, text
    assert SERVER_MESSAGE in text, \
        "the pane does not show the server's own message:\n" + text
    assert "conv error (stopped)" in txt, txt
    assert "session save" in txt, "the session was not saved after the error:\n" + txt
    assert review is not None, "no review although a.txt was written under auto:\n" + text
    assert (tmp_path / "a.txt").exists()
    assert "agent done" not in txt, txt


# @scenario SC-ai-agent-54
def test_in_stream_error_event_mid_loop_shows_the_servers_message(tmp_path):
    """The server said what went wrong inside the stream: HTTP 200, one SSE
    error event with its message, then the connection closed with no end
    marker.  That is a backend error answered in full, not a dead connection:
    the pane shows the server's words, not the transport line, and the run
    stops without acting on the turn."""
    captured, rows, review, txt = _run_openai(
        tmp_path, ["TOOL write_file a.txt\nAAA\n@@END", "DONE never"], "auto", 2,
        fault_for_turn=lambda n: "error_event" if n == 1 else None)
    text = _pane_text_with_review(rows, review)
    assert len(captured) >= 2, "the second turn was never requested:\n" + txt
    assert TRANSPORT_LINE not in text, \
        "an error event the server sent in full was shown as a transport error:\n" + text
    assert BACKEND_ERROR_LINE in text, text
    assert SERVER_MESSAGE in text, \
        "the pane does not show the server's own message:\n" + text
    assert "conv error (stopped)" in txt, txt
    assert "conv transport error" not in txt, txt
    assert review is not None, "no review although a.txt was written under auto:\n" + text
    assert "agent done" not in txt, txt


# @scenario SC-ai-agent-28
def test_write_file_keeps_the_final_newline(tmp_path):
    env = _env(tmp_path, "TOOL write_file out.txt\nhello\n@@END@@TURN@@DONE wrote", "ask")
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _start_agent(s, "write")
        assert _seen_within(s, WRITE_BOX, TURN_POLL_S), _screen(s)
        s.key("y", delay=1.2)
        assert _seen_within(s, FOLLOWUP_LINE, TURN_POLL_S), _screen(s)
    data = (tmp_path / "out.txt").read_bytes()
    assert data == b"hello\n", \
        "the newline that ends the last body line was dropped: %r" % data


# @scenario SC-ai-agent-28
def test_write_file_keeps_crlf_lines_and_the_final_newline(tmp_path):
    env = _env(tmp_path, "TOOL write_file crlf.txt\nline one\r\nline two\r\n@@END"
                         "@@TURN@@DONE wrote", "ask")
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _start_agent(s, "write")
        assert _seen_within(s, WRITE_BOX, TURN_POLL_S), _screen(s)
        s.key("y", delay=1.2)
        assert _seen_within(s, FOLLOWUP_LINE, TURN_POLL_S), _screen(s)
    data = (tmp_path / "crlf.txt").read_bytes()
    assert data == b"line one\r\nline two\r\n", \
        "CRLF content was not written verbatim: %r" % data


# @scenario SC-ai-agent-28
def test_apply_patch_inherits_the_final_newline(tmp_path):
    turns = "@@TURN@@".join([
        "TOOL write_file a.txt\nAAA\n@@END",
        "TOOL apply_patch a.txt\n<<<<<<< SEARCH\nAAA\n=======\nBBB\n>>>>>>> REPLACE\n@@END",
        "DONE done"])
    env = _env(tmp_path, turns, "auto")
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _start_agent(s, "auto run", delay=0.5)
        assert _seen_within(s, REVIEW_PROMPT, REVIEW_POLL_S), \
            "no review at the end of the auto run:\n" + _screen(s)
        s.key("a", delay=1.0)
        assert _seen_within(s, FOLLOWUP_LINE, TURN_POLL_S), _screen(s)
    data = (tmp_path / "a.txt").read_bytes()
    assert data == b"BBB\n", \
        "the patched file lost the newline the write gave it: %r" % data


# @scenario SC-ai-agent-31
def test_failed_write_is_told_to_the_user(tmp_path):
    captured, rows, review, txt = _run_openai(
        tmp_path, ["TOOL write_file %s\nhello\n@@END" % WRITE_FAILED_PATH, "DONE ok"],
        "auto", 2)
    text = "\n".join(rows)
    assert len(captured) >= 2, "the run did not continue after the failed write:\n" + txt
    result = _tool_results(captured[1])
    assert result and result[-1] == "TOOL RESULT:\n(write failed)", result
    assert not (tmp_path / "nodir").exists(), "a directory was created for the failed write"
    assert "[agent] write failed: %s" % WRITE_FAILED_PATH in text, \
        "the pane does not say the write failed:\n" + text
    assert "agent write failed %s" % WRITE_FAILED_PATH in txt, \
        "the trace does not record the failed write:\n" + txt
    assert "agent done" in txt, "the run did not go on after the failed write:\n" + txt


# @scenario SC-ai-agent-44
def test_window_closed_during_run_repaints_at_once(tmp_path):
    env = _env(tmp_path, "DONE late", "ask", XWPE_AI_MOCK_DELAY_MS=str(SLOW_REPLY_MS))
    timeline = []
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _start_agent(s, "slow", delay=0.5)
        t0 = time.time()
        s.key(CTRL_W, delay=0.2)
        while time.time() - t0 < TIMELINE_S:
            s._drain(0.2)
            rows = s.display(drain=0)
            timeline.append((round(time.time() - t0, 1),
                             "t.c" in rows[1],
                             DROP_LINE in "\n".join(rows),
                             "agent target gone" in _trace(tmp_path)))
    gone_at = next((t for t, title, _, _ in timeline if not title), None)
    drop_at = next((t for t, _, dropped, _ in timeline if dropped), None)
    assert timeline[0][3], "the trace does not record the drop at Ctrl-W time"
    assert gone_at is not None and gone_at < REPAINT_BOUND_S, \
        "the closed window stayed painted for %s s after Ctrl-W" % gone_at
    assert drop_at is not None and drop_at < REPAINT_BOUND_S, \
        "the drop line took %s s to show" % drop_at


# @scenario SC-ai-agent-44
def test_window_closed_during_run_applies_nothing(tmp_path):
    env = _env(tmp_path, "TOOL write_file t.c\nSHOULD NOT LAND\n@@END@@TURN@@DONE x", "auto",
               XWPE_AI_MOCK_DELAY_MS=str(APPLY_NOTHING_DELAY_MS))
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _start_agent(s, "slow", delay=0.5)
        s.key(CTRL_W, delay=1.0)
        s._drain(6.0)
        rows = s.display()
    txt = _trace(tmp_path)
    text = "\n".join(rows)
    disk = (tmp_path / "t.c").read_text()
    assert "SHOULD NOT LAND" not in disk, "the reply was applied after the window closed"
    assert "agent target gone" in txt, txt
    assert DROP_LINE in text, text


HOST_MOCK = textwrap.dedent('''\
    import sys, json
    def emit(o):
        sys.stdout.write(json.dumps(o) + "\\n"); sys.stdout.flush()
    while True:
        line = sys.stdin.readline()
        if not line:
            break
        emit({"type": "system", "subtype": "init", "session_id": "mock-1"})
        emit({"type": "assistant", "message": {"content": [
            {"type": "text", "text": "Looking at t.c."}]}})
        emit({"type": "result", "subtype": "success", "is_error": False,
              "result": "looked at t.c"})
''')


# @scenario SC-ai-agent-49
@pytest.mark.skipif(not _host_build(), reason="built without --enable-ai-agent-host")
def test_hosted_session_announces_its_checkpoint(tmp_path):
    mock = tmp_path / "mock_host.py"
    mock.write_text(HOST_MOCK)
    env = {"XWPE_AI_ENABLE": "1",
           "XWPE_AI_AGENT_ENGINE": "claude-code",
           "XWPE_AI_HOST_CMD": "python3 %s" % mock,
           "XWPE_AI_POLICY": "ask",
           "XWPE_AI_TRACE": str(tmp_path / "ai.trace")}
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        s._drain(0.5)
        s.key(ALT.AI); s.key("g"); s._drain(0.5)
        s.key("look at t.c"); s.key("\r", delay=1.0)
        assert _traced_within(tmp_path, "host result", TRACE_POLL_S), _trace(tmp_path)
        s._drain(1.0)
        rows = s.display()
    txt = _trace(tmp_path)
    text = "\n".join(rows)
    assert ENGINE_LINE in text, text
    assert "checkpoint" in txt, "no checkpoint was taken for the hosted session:\n" + txt
    ck_row = _row_index(rows, CHECKPOINT_LINE)
    you_row = _row_index(rows, "[agent] you:")
    assert ck_row >= 0, \
        "the pane never says the hosted session took a checkpoint:\n" + text
    assert you_row >= 0, text
    assert ck_row < you_row, \
        "the checkpoint line must come before the first turn (rows %d, %d):\n%s" \
        % (ck_row, you_row, text)
