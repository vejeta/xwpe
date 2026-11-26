"""A tool step that waits for the loop feeds the model once, and no more.

The assistant's web and command tools do not answer at once: the page is read,
or the command runs, on the editor's loop, and the task goes on when it is
done.  The result must then reach the model exactly one time, whatever flags
the editor was built with: a build that treated a waiting step as a finished
one fed the model twice (or ended the editor with a double free).

The rows read the behaviour log, not the screen: "web page read" or "tool
command read" once, one "TOOL RESULT" turn sent to the model, one end.  The
chat rows read the sources line the answer ends with: a search names its URL
once, and a page that did not come names nothing.

The model's side is the mock backend; the pages come from a loopback server
and no test touches the network.
"""
import os
import re
import socket
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN, wait_until

SEED = "int main(void){return 0;}\n"
TURN_POLL_S = 20      # seconds on an idle machine; the driver stretches it
PAGE_WORD = "STEP_PAGE_WORD"
RESULT_TURN = "last=TOOL RESULT:"
SOURCES_TRACE = "chat sources "


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"autonomous coding agent" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _trace(tmp_path):
    t = tmp_path / "ai.trace"
    return t.read_text() if t.exists() else ""


def _traced_within(s, tmp_path, needle, timeout):
    """Wait, through the driver's owner of waits, until the log holds ``needle``."""
    return wait_until(s, lambda: needle in _trace(tmp_path), timeout, what=needle)


def _env(tmp_path, reply, **more):
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_POLICY": "auto",
        "XWPE_AI_TRACE": str(tmp_path / "ai.trace"),
    }
    env.update(more)
    return env


def _server():
    """A loopback site: every path answers a page holding PAGE_WORD."""
    paths = []

    class H(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def log_message(self, *a):
            pass

        def do_GET(self):
            paths.append(self.path)
            body = ("<html><body><p>%s</p></body></html>" % PAGE_WORD).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    srv = HTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, paths


def _closed_port():
    """A port nothing listens on: a fetch to it fails at once."""
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def _agent_run(tmp_path, reply, **more):
    """Run one agent task to its end and return the behaviour log."""
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path, reply, **more)) as s:
        s.key(ALT.AI); s.key("g")
        s.key("go"); s.key("\r", delay=1.0)
        assert _traced_within(s, tmp_path, "agent done", TURN_POLL_S), _trace(tmp_path)
        s._drain(0.8)
    return _trace(tmp_path)


def _plan_run(tmp_path, reply, **more):
    """Run one plan (the multi-file edit) until the model has been asked twice
    and return the behaviour log."""
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path, reply, **more)) as s:
        s.key(ALT.AI); s.key("f")
        s.key("study"); s.key("\r", delay=1.0)
        assert _traced_within(s, tmp_path, "mock history tool-results=1", TURN_POLL_S), \
            _trace(tmp_path)
        s._drain(0.8)
    return _trace(tmp_path)


def _chat_run(tmp_path, reply, **more):
    """Ask the chat one question and return the behaviour log once it ended."""
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path, reply, **more)) as s:
        s.key(ALT.AI); s.key("a", delay=0.6)
        s.key("news?"); s.key("\r", delay=1.0)
        assert _traced_within(s, tmp_path, "chat done", TURN_POLL_S), _trace(tmp_path)
        s._drain(0.8)
    return _trace(tmp_path)


def _tool_results_sent(trace):
    """How many tool results each request to the model carried, in order."""
    return [int(n) for n in re.findall(r"mock history tool-results=(\d+)", trace)]


def _assert_fed_once(trace, read_line):
    assert trace.count(read_line) == 1, "the result was read %d times:\n%s" % (
        trace.count(read_line), trace)
    assert trace.count(RESULT_TURN) == 1, "the model got %d results:\n%s" % (
        trace.count(RESULT_TURN), trace)
    assert _tool_results_sent(trace) == [0, 1], \
        "the requests carried %r tool results, want [0, 1]:\n%s" % (_tool_results_sent(trace), trace)
    assert trace.count("agent done") == 1, trace


# @scenario SC-ai-agent-107
def test_a_web_fetch_waits_and_the_model_reads_its_page_once(tmp_path):
    srv, paths = _server()
    page = "http://127.0.0.1:%d/page" % srv.server_address[1]
    try:
        trace = _agent_run(tmp_path, "TOOL web_fetch %s@@TURN@@DONE ok" % page, XWPE_AI_WEB="1")
    finally:
        srv.shutdown()
        srv.server_close()
    _assert_fed_once(trace, "web page read")
    assert paths == ["/page"], paths
    assert PAGE_WORD in trace, trace


# @scenario SC-ai-agent-107
def test_a_run_command_waits_and_the_model_reads_its_output_once(tmp_path):
    trace = _agent_run(tmp_path, "TOOL run_command echo STEP_CMD_WORD@@TURN@@DONE ok")
    _assert_fed_once(trace, "tool command read status=0")
    assert "STEP_CMD_WORD" in trace.split(RESULT_TURN)[1], trace


# @scenario SC-ai-agent-107
def test_a_grep_waits_and_the_model_reads_its_matches_once(tmp_path):
    (tmp_path / "found.txt").write_text("a STEP_GREP_WORD here\n")
    trace = _agent_run(tmp_path, "TOOL grep STEP_GREP_WORD@@TURN@@DONE ok")
    _assert_fed_once(trace, "tool command read status=0")
    assert "found.txt" in trace.split(RESULT_TURN)[1], trace


# @scenario SC-ai-agent-107
def test_a_plan_study_grep_waits_and_the_model_reads_its_matches_once(tmp_path):
    (tmp_path / "found.txt").write_text("a STEP_PLAN_WORD here\n")
    trace = _plan_run(tmp_path, "TOOL grep STEP_PLAN_WORD@@TURN@@@@PLAN-DONE nothing to change")
    # the grep also matches the log itself, which holds this line: count log lines
    assert len([l for l in trace.splitlines() if l.startswith("plan tool=grep")]) == 1, trace
    assert trace.count("tool command read status=0") == 1, trace
    assert trace.count(RESULT_TURN) == 1, trace
    assert _tool_results_sent(trace) == [0, 1], trace
    assert "found.txt" in trace.split(RESULT_TURN)[1], trace


# @scenario SC-ai-agent-107
def test_the_chat_names_a_search_url_once_in_its_sources(tmp_path):
    srv, paths = _server()
    port = srv.server_address[1]
    try:
        trace = _chat_run(tmp_path, "TOOL web_search xwpe editor@@TURN@@ANSWER",
                          XWPE_AI_WEB="1",
                          XWPE_AI_SEARCH_URL="http://127.0.0.1:%d/find?q=%%s" % port)
    finally:
        srv.shutdown()
        srv.server_close()
    assert paths == ["/find?q=xwpe%20editor"], paths
    lines = [l for l in trace.splitlines() if SOURCES_TRACE in l]
    assert len(lines) == 1, trace
    sources = lines[0].split(SOURCES_TRACE, 1)[1]
    assert sources == "http://127.0.0.1:%d/find?q=xwpe%%20editor" % port, sources
    assert trace.count(RESULT_TURN) == 1, trace
    assert _tool_results_sent(trace) == [0, 1], trace


# @scenario SC-ai-agent-107
def test_a_chat_read_with_no_text_is_a_result_and_the_turn_goes_on(tmp_path):
    """Out of memory while a file is read is a result with a note, not the
    chat's answer: the model is asked again and answers."""
    (tmp_path / "f.txt").write_text("body\n")
    trace = _chat_run(tmp_path, "TOOL read_file f.txt@@TURN@@ANSWER",
                      XWPE_TEST_FAIL="ai-chat-read-no-text")
    assert _tool_results_sent(trace) == [0, 1], trace
    assert "(no output" in trace.split(RESULT_TURN)[1], trace
    assert trace.count("chat done") == 1, trace


# @scenario SC-ai-agent-107
def test_the_chat_names_no_source_for_a_search_that_did_not_come(tmp_path):
    port = _closed_port()
    trace = _chat_run(tmp_path, "TOOL web_search xwpe editor@@TURN@@ANSWER",
                      XWPE_AI_WEB="1",
                      XWPE_AI_SEARCH_URL="http://127.0.0.1:%d/find?q=%%s" % port)
    assert SOURCES_TRACE not in trace, trace
    assert "TOOL ERROR" in trace.split(RESULT_TURN)[1], trace
