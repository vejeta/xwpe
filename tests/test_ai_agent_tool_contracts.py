"""What a built-in tool feeds back to the model, and the web_fetch limits.

A tool's output becomes the next turn's "TOOL RESULT:" user message; a
search/grep with no hits is not an empty string but a named "no output"
note, so the model can tell "ran, found nothing" from "never ran". A tool
name the model invents is answered "(unknown tool)" and the loop goes on
rather than stopping cold. web_fetch bounds both ends: the GET itself stops
at 65536 bytes (so markup past that point cannot smuggle text in), and the
de-marked-up text handed to the model is cut at 12000 characters with a
trailing "...(truncated)" note. An HTTP error is a result, not a crash
("(HTTP 404)" plus the server's own body), and a fetch that hangs is given
up on after 20 seconds with "TOOL ERROR: ... timed out".

The mock backend cannot show what was fed back to the model, so these tests
stand up a capturing OpenAI-compatible server on the loopback and read the
second request's body, as the run-visibility tests do.
"""
import json
import os
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN, pane_text_with_history

SEED = "int main(void){return 0;}\n"
TURN_POLL_S = 20
RUNNING_WEB_FETCH = "[agent] running web_fetch ... (result follows)"


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


def _screen(s, drain=0.2):
    return "\n".join(s.display(drain=drain))


def _seen_within(s, needle, timeout):
    end = time.time() + timeout
    while time.time() < end:
        s._drain(0.25)
        if needle in "\n".join(s.display(drain=0)):
            return True
    return False


def _seed_xwperc(tmp_path, lines):
    cfg = tmp_path / ".config" / "xwpe"
    cfg.mkdir(parents=True, exist_ok=True)
    (cfg / "xwperc").write_text("[Programming]\n" + "".join(l + "\n" for l in lines))


def _multi_turn_server(captured, replies):
    """An OpenAI-compatible server that records every request body and
    answers turn n with `replies[n]` (the last reply repeats)."""
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


def _run_openai(tmp_path, replies, policy, want_requests, timeout=TURN_POLL_S,
                pane_needles=()):
    """Drive one agent run against the capturing server, auto-approving
    (policy auto, so nothing asks).  Returns the captured requests, the
    pane's logical lines at the end (paged back with PgUp while any of
    `pane_needles` is missing from the view: the end-of-run review halves
    the pane and its oldest lines scroll above the view) and the trace."""
    captured = []
    srv, port = _multi_turn_server(captured, replies)
    try:
        _seed_openai_xwperc(tmp_path, port, policy)
        with WpeSession(str(tmp_path), SEED, env_extra=_openai_env(tmp_path)) as s:
            s.key(ALT.AI, delay=0.6); s.key("g", delay=0.6)
            s.key("do it", delay=0.1); s.key("\r", delay=1.0)
            end = time.time() + timeout
            while time.time() < end and len(captured) < want_requests:
                s._drain(0.5)
            _seen_within(s, "type a follow-up", 8)
            s._drain(1.0)
            rows = pane_text_with_history(s, pane_needles)
    finally:
        srv.shutdown()
        srv.server_close()
    return captured, rows, _trace(tmp_path)


# @scenario SC-ai-agent-08
def test_tool_result_and_a_blank_result_are_both_fed_back(tmp_path):
    (tmp_path / "hay.txt").write_text("needle in hay\n")
    # The trace file lives in the workdir and quotes every pattern, so the
    # "empty" pattern is a regex whose literal spelling does not match itself.
    captured, rows, txt = _run_openai(
        tmp_path, ["TOOL grep needle", "TOOL grep zz[q]qxxnothing", "DONE ok"], "auto", 3)
    assert len(captured) >= 3, "\n".join(captured)
    r1 = _tool_results(captured[1])
    r2 = _tool_results(captured[2])
    assert r1 and r1[-1].startswith("TOOL RESULT:\n") and "hay.txt:1:needle in hay" in r1[-1], r1
    assert r2 and r2[-1] == "TOOL RESULT:\n(no output -- nothing matched; try a different path or pattern)", r2


# @scenario SC-ai-agent-09
def test_unknown_tool_is_reported_and_the_loop_continues(tmp_path):
    captured, rows, txt = _run_openai(tmp_path, ["TOOL frobnicate the widget", "DONE ok"], "auto", 2)
    text = "\n".join(rows)
    assert "[agent] frobnicate the widget" in text, text
    assert len(captured) >= 2, "the loop did not continue after an unknown tool"
    r = _tool_results(captured[1])
    assert r and r[-1] == "TOOL RESULT:\n(unknown tool)", r
    assert "agent done" in txt


# @scenario SC-ai-agent-29
def test_apply_patch_tool_errors_are_fed_back_distinctly(tmp_path):
    """Each of the three ways apply_patch can fail reads back to the model as
    its own named TOOL ERROR, never a generic failure the model cannot act
    on: an unmatched SEARCH, a path that does not exist, and a reply with no
    SEARCH/REPLACE body at all."""
    unmatched = tmp_path / "unmatched"; unmatched.mkdir()
    missing = tmp_path / "missing"; missing.mkdir()
    no_blocks = tmp_path / "no-blocks"; no_blocks.mkdir()

    captured, _, _ = _run_openai(
        unmatched,
        ["TOOL apply_patch t.c\n<<<<<<< SEARCH\nnonexistent line\n"
         "=======\nnew line\n>>>>>>> REPLACE\n@@END", "DONE done"], "auto", 2)
    r = _tool_results(captured[1])
    assert r and r[-1].startswith("TOOL RESULT:\nTOOL ERROR: SEARCH text not found"), r

    captured, _, _ = _run_openai(
        missing,
        ["TOOL apply_patch does-not-exist.txt\n<<<<<<< SEARCH\nx\n"
         "=======\ny\n>>>>>>> REPLACE\n@@END", "DONE done"], "auto", 2)
    r = _tool_results(captured[1])
    assert r and r[-1] == ("TOOL RESULT:\nTOOL ERROR: file not found "
                            "(use write_file to create it)"), r

    # No newline at all after the TOOL line: the model gave no body, not even
    # an empty one, so this is the "no body" branch, not "no blocks in body".
    captured, _, _ = _run_openai(no_blocks, ["TOOL apply_patch t.c", "DONE done"], "auto", 2)
    r = _tool_results(captured[1])
    assert r and r[-1] == "TOOL RESULT:\nTOOL ERROR: apply_patch needs SEARCH/REPLACE blocks", r


# @scenario SC-ai-agent-24
def test_web_fetch_bounds_the_get_and_the_text_handed_to_the_model(tmp_path):
    # 65 000 bytes of markup with no text, then text: a 65536-byte GET bound
    # keeps at most ~500 bytes of the text; a longer bound shows LATE_WORD.
    comment = b"<!--" + b"x" * 65000 + b"-->"
    tail = b"<p>" + b"EARLY_WORD " * 2000 + b"</p><p>LATE_WORD_BEYOND_64K</p>"
    big_text = b"<p>" + b"word " * 10000 + b"</p>"           # 50 000 chars of text
    paths = []

    class H(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def log_message(self, *a):
            pass

        def do_GET(self):
            paths.append(self.path)
            body = comment + tail if self.path == "/bound" else big_text
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    srv = HTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    port = srv.server_address[1]
    try:
        captured, rows, txt = _run_openai(
            tmp_path, ["TOOL web_fetch http://127.0.0.1:%d/bound" % port,
                       "TOOL web_fetch http://127.0.0.1:%d/big" % port, "DONE ok"], "auto", 3,
            timeout=40, pane_needles=[RUNNING_WEB_FETCH])
    finally:
        srv.shutdown()
    assert paths[:2] == ["/bound", "/big"], paths
    r1 = _tool_results(captured[1])[-1]
    r2 = _tool_results(captured[2])[-1]
    assert "LATE_WORD_BEYOND_64K" not in r1, "the GET read past 65536 bytes"
    assert "EARLY_WORD" in r1, r1[:300]
    body2 = r2[len("TOOL RESULT:\n"):]
    assert body2.endswith("...(truncated)"), body2[-100:]
    assert len(body2) <= 12000 + len("\n...(truncated)") + 8, len(body2)
    assert RUNNING_WEB_FETCH in "\n".join(rows), "\n".join(rows)


def _one_page_server(pages):
    """pages[path] = (status, body); "/slow" sleeps 25 s before answering."""
    class H(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def log_message(self, *a):
            pass

        def do_GET(self):
            if self.path.startswith("/slow"):
                time.sleep(25)
            st, body = pages.get(self.path, (404, b"<html><body>no such page here</body></html>"))
            self.send_response(st)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    srv = HTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


# @scenario SC-ai-agent-25
def test_web_fetch_http_404_is_a_result_not_a_crash(tmp_path):
    srv = _one_page_server({})
    try:
        port = srv.server_address[1]
        captured, rows, txt = _run_openai(
            tmp_path, ["TOOL web_fetch http://127.0.0.1:%d/missing" % port, "DONE ok"], "auto", 2)
    finally:
        srv.shutdown()
    r = _tool_results(captured[1])[-1]
    assert r.startswith("TOOL RESULT:\n(HTTP 404)"), r
    assert "no such page here" in r, r
    assert "agent done" in txt


# @scenario SC-ai-agent-25
def test_web_fetch_gives_up_after_20s_and_reports_a_timeout(tmp_path):
    srv = _one_page_server({})
    try:
        port = srv.server_address[1]
        t0 = time.time()
        captured, rows, txt = _run_openai(
            tmp_path, ["TOOL web_fetch http://127.0.0.1:%d/slow" % port, "DONE ok"], "auto", 2,
            timeout=45, pane_needles=[RUNNING_WEB_FETCH])
    finally:
        srv.shutdown()
    dt = time.time() - t0
    assert len(captured) >= 2, "the run did not continue after the slow fetch"
    r = _tool_results(captured[1])[-1]
    assert "TOOL ERROR:" in r and "timed out" in r, r
    assert "agent done" in txt
    assert RUNNING_WEB_FETCH in "\n".join(rows), "\n".join(rows)
    assert dt < 45, "the 20 s bound was not honoured: took %.1fs" % dt
