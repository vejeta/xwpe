"""The chat answers every tool line, and reaches the web behind one switch.

A reply that asks for a tool the chat cannot run (a web tool with the switch
off, a name the model invented) is answered: the model reads a tool result
naming the tools it may use, the pane shows one line naming the refused tool
and why, and the turn goes on to an answer -- it never ends in silence.

The web lives behind the saved switch ``AIWeb`` (off by default).  With it
off or unanswered the chat's prompt lists only the four workspace tools and
names web_search and web_fetch as the Agent's.  The first turn that wants the
web while the switch is unanswered shows a notice (what leaves the machine,
to which engine, and that Options > AI turns it off); ``y`` writes
``AIWeb : on`` to xwperc at once, ``n`` leaves the file without the key and
the turn answers without the web.  With the web on, each call goes through
the permission dial as the agent's web calls do, the transcript shows the
query, the engine's host and every URL read, the answer ends with a
``sources:`` line, and the pane header gains `` | web``.

The model's side is the mock backend.  No test touches the network: every
session that may search is pointed at a loopback server through
``AISearchURL`` in the test HOME's xwperc, the server records every request
it gets, and the tests that keep the web off assert it got none.
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
ESC = "\033"

WAIT_SCALE = float(os.environ.get("XWPE_TEST_WAIT_SCALE", "1") or 1)
TURN_POLL_S = 20 * WAIT_SCALE

HEADER_ASK = "[AI: mock | model: mock-model | policy: ask]"
HEADER_ASK_WEB = "[AI: mock | model: mock-model | policy: ask | web]"
NOTICE_FIRST = "Web access -- first use"
NOTICE_KEYS = "n / Esc = not now"
NOTICE_ENABLE = "y = enable web access"
APPROVE_KEYS = "n / Esc = deny"
RUNNING_SEARCH = "[agent] running web_search ... (result follows)"
RUNNING_FETCH = "[agent] running web_fetch ... (result follows)"
PAGE_WORD = "PAGE_WORD_42"


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


def _xwperc_path(tmp_path):
    return tmp_path / ".config" / "xwpe" / "xwperc"


def _xwperc(tmp_path):
    p = _xwperc_path(tmp_path)
    return p.read_text() if p.exists() else ""


def _screen(s, drain=0.2):
    return "\n".join(s.display(drain=drain))


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


def _ask(s, question, delay=0.5):
    s.key(question); s.key("\r", delay=delay)


def _seed_xwperc(tmp_path, lines):
    cfg = tmp_path / ".config" / "xwpe"
    cfg.mkdir(parents=True, exist_ok=True)
    _xwperc_path(tmp_path).write_text(
        "[Programming]\n" + "".join(l + "\n" for l in lines))


def _web_server(requests):
    """A loopback site: /search?q=... answers with a link to /page, /page
    holds PAGE_WORD; every request path is appended to `requests`."""
    class H(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def log_message(self, *a):
            pass

        def do_GET(self):
            requests.append(self.path)
            if self.path.startswith("/page"):
                body = ("<html><body><p>%s</p></body></html>" % PAGE_WORD).encode()
            else:
                body = ("<html><body><a href=\"http://127.0.0.1:%d/page\">page"
                        "</a></body></html>" % self.server.server_address[1]).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    srv = HTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]


def _search_url(port):
    return "http://127.0.0.1:%d/search?q=%%s" % port


def _mock_env(tmp_path, reply, policy="ask", **more):
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_POLICY": policy,
        "XWPE_AI_TRACE": str(tmp_path / "ai.trace"),
    }
    env.update(more)
    return env


def _chat(tmp_path, env, question, needles, keys_after=(), key_wait=None):
    """Open the chat, ask `question`, press `keys_after` once `key_wait`
    shows on screen (a notice or an approval line), wait for the turn to
    end, and return the pane's logical lines (paged back for `needles`)."""
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        s.key(ALT.AI); s.key("a", delay=0.6)
        _ask(s, question, delay=1.0)
        if key_wait is not None:
            assert _seen_within(s, key_wait, TURN_POLL_S), \
                "the pane never showed %r:\n%s" % (key_wait, _screen(s))
            for k in keys_after:
                s.key(k, delay=0.6)
        assert _traced_within(tmp_path, "chat done", TURN_POLL_S), \
            "the turn never ended:\n" + _trace(tmp_path) + "\n" + _screen(s)
        s._drain(0.8)
        return pane_text_with_history(s, needles)


# @scenario SC-ai-chat-51
def test_unavailable_tool_is_answered_never_silent(tmp_path):
    """THEN the model is given a tool result naming the tools it may use
    instead, the pane shows one line saying which tool was refused and why,
    and the turn goes on to an answer."""
    requests = []
    srv, port = _web_server(requests)
    try:
        off = tmp_path / "off"; off.mkdir()
        _seed_xwperc(off, ["AISearchURL : " + _search_url(port)])
        env = _mock_env(off, "TOOL web_search what is new@@TURN@@ANSWER FROM WORKSPACE",
                        XWPE_AI_WEB="0")
        rows = _chat(off, env, "news?", [". web_search what is new", "AI: ANSWER"])
        text = "\n".join(rows)
        trace = _trace(off)
        assert ". web_search what is new" in text, text
        refused = [r for r in rows if r.startswith("[AI] web_search")]
        assert refused and "web access is off" in refused[0] and "Options > AI" in refused[0], \
            "no line names the refused tool and why:\n" + text
        assert "AI: ANSWER FROM WORKSPACE" in text, "the turn ended without an answer:\n" + text
        assert "chat tool refused web_search" in trace, trace
        assert "you may use read_file, grep, list_dir, glob" in trace, \
            "the model was not told which tools it may use:\n" + trace
        assert "web_search, web_fetch" not in trace.split("chat tool refused")[1].split("\n")[0], \
            "the refusal offered the web tools while the web is off:\n" + trace
        assert requests == [], "a refused search reached the server: %r" % requests

        bogus = tmp_path / "bogus"; bogus.mkdir()
        _seed_xwperc(bogus, ["AISearchURL : " + _search_url(port)])
        env = _mock_env(bogus, "TOOL frobnicate the widget@@TURN@@ANSWER TWO", XWPE_AI_WEB="0")
        rows = _chat(bogus, env, "do it", [". frobnicate the widget", "AI: ANSWER TWO"])
        text = "\n".join(rows)
        refused = [r for r in rows if r.startswith("[AI] frobnicate")]
        assert refused and "no such tool" in refused[0], \
            "an invented tool was not named as refused:\n" + text
        assert "AI: ANSWER TWO" in text, "the turn ended without an answer:\n" + text
        assert "chat tool refused frobnicate" in _trace(bogus), _trace(bogus)
    finally:
        srv.shutdown()
        srv.server_close()


def _capturing_model(captured):
    """An OpenAI-compatible server that records every request body and
    answers "ok", so the system prompt the chat built can be read."""
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
            body = (b'data: {"choices":[{"delta":{"content":"ok"}}]}\n\n'
                    b'data: [DONE]\n\n')
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            self.wfile.write(body)

    srv = HTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]


def _system_prompt(tmp_path, web):
    """The system message of the first chat request, with XWPE_AI_WEB=web."""
    captured = []
    srv, port = _capturing_model(captured)
    try:
        _seed_xwperc(tmp_path, ["AIBackend : 1",
                                "AIEndpoint : http://127.0.0.1:%d/v1" % port,
                                "AIModel : m", "AIPolicy : ask"])
        env = {"XWPE_AI_ENABLE": "1", "OPENAI_API_KEY": "x", "XWPE_AI_WEB": web,
               "XWPE_AI_TRACE": str(tmp_path / "ai.trace")}
        with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
            s.key(ALT.AI, delay=0.6); s.key("a", delay=0.6)
            _ask(s, "hi", delay=1.0)
            end = time.time() + TURN_POLL_S
            while time.time() < end and not captured:
                s._drain(0.5)
    finally:
        srv.shutdown()
        srv.server_close()
    assert captured, "the chat sent no request"
    msgs = json.loads(captured[0])["messages"]
    return next(m["content"] for m in msgs if m.get("role") == "system")


# @scenario SC-ai-chat-53
def test_with_the_web_off_the_model_is_not_offered_it(tmp_path):
    """THEN the request's system prompt lists only read_file, grep, list_dir
    and glob, and the capability paragraph names web_search and web_fetch
    as the Agent's tools, not the chat's."""
    off = tmp_path / "off"; off.mkdir()
    sys_off = _system_prompt(off, "0")
    tools = sys_off.split("I will reply with the result")[0]
    for name in ("TOOL read_file", "TOOL grep", "TOOL list_dir", "TOOL glob"):
        assert name in tools, "the workspace tool list lost %r:\n%s" % (name, tools)
    assert "TOOL web_search" not in tools and "TOOL web_fetch" not in tools, \
        "the web tools were offered with the web off:\n" + tools
    assert "web_search and web_fetch" in sys_off and "the Agent's" in sys_off, \
        "the capability paragraph does not name the web tools as the Agent's:\n" + sys_off

    on = tmp_path / "on"; on.mkdir()
    sys_on = _system_prompt(on, "1")
    tools = sys_on.split("I will reply with the result")[0]
    assert "TOOL web_search <query>" in tools and "TOOL web_fetch <url>" in tools, \
        "the web tools were not offered with the web on:\n" + tools
    assert "workspace first" in tools.lower(), \
        "no word on when to use the web:\n" + tools


# @scenario SC-ai-chat-52
def test_first_need_asks_once_and_the_answer_is_remembered(tmp_path):
    """THEN the pane shows the first-use notice (what leaves the machine: the
    query, to the engine named in Options > AI; and that the model stays
    where it is) and one attention line, "Enter / y = enable web access
    n / Esc = not now"; AND y writes `AIWeb : on` to xwperc at once and the
    turn goes on; n leaves xwperc without an `AIWeb` line, tells the model
    the web is not enabled, and the turn answers without it."""
    requests = []
    srv, port = _web_server(requests)
    reply = "TOOL web_search xwpe editor@@TURN@@ANSWER AFTER NOTICE"
    try:
        no = tmp_path / "no"; no.mkdir()
        _seed_xwperc(no, ["AISearchURL : " + _search_url(port)])
        rows = _chat(no, _mock_env(no, reply, policy="auto"), "what is new?",
                     ["Web access", "AI: ANSWER"], keys_after=["n"], key_wait=NOTICE_FIRST)
        text = "\n".join(rows)
        assert "127.0.0.1" in text and "Options > AI" in text, \
            "the notice does not name the engine and Options > AI:\n" + text
        assert "model stays where it is" in text, "the notice does not say the model stays:\n" + text
        assert NOTICE_ENABLE in text and NOTICE_KEYS in text, "no attention line:\n" + text
        assert "[AI] web access not enabled" in text, "n was not acknowledged:\n" + text
        assert "AI: ANSWER AFTER NOTICE" in text, "the turn did not answer after n:\n" + text
        assert "AIWeb" not in _xwperc(no), "n wrote an AIWeb line:\n" + _xwperc(no)
        assert "web access not enabled" in _trace(no), _trace(no)
        assert requests == [], "a search ran after n: %r" % requests

        yes = tmp_path / "yes"; yes.mkdir()
        _seed_xwperc(yes, ["AISearchURL : " + _search_url(port)])
        rows = _chat(yes, _mock_env(yes, reply, policy="auto"), "what is new?",
                     ["Web access", RUNNING_SEARCH, "AI: ANSWER"],
                     keys_after=["y"], key_wait=NOTICE_FIRST)
        text = "\n".join(rows)
        assert "AIWeb : on" in _xwperc(yes), "y did not save AIWeb : on:\n" + _xwperc(yes)
        assert RUNNING_SEARCH in text, "the turn did not go on to the search:\n" + text
        assert "AI: ANSWER AFTER NOTICE" in text, text
        assert requests and requests[0].startswith("/search?q=xwpe%20editor"), requests
        assert _trace(yes).count("chat web access enabled") == 1, \
            "the notice was not answered exactly once:\n" + _trace(yes)
    finally:
        srv.shutdown()
        srv.server_close()


# @scenario SC-ai-chat-54
def test_each_web_call_is_gated_by_the_dial(tmp_path):
    """THEN the pane asks "APPROVE?  <query>" with the query shown in full,
    y runs it and n denies it and tells the model so; AND with the dial at
    auto the search runs unasked."""
    requests = []
    srv, port = _web_server(requests)
    reply = "TOOL web_search xwpe borland editor@@TURN@@ANSWER AFTER DIAL"
    try:
        deny = tmp_path / "deny"; deny.mkdir()
        _seed_xwperc(deny, ["AISearchURL : " + _search_url(port), "AIWeb : on"])
        rows = _chat(deny, _mock_env(deny, reply, policy="ask"), "news?",
                     ["APPROVE?", "AI: ANSWER"], keys_after=["n"], key_wait=APPROVE_KEYS)
        text = "\n".join(rows)
        assert "APPROVE? xwpe borland editor" in text, "no approval line with the query:\n" + text
        assert "[agent] denied: web_search xwpe borland editor" in text, text
        assert "agent deny web_search xwpe borland editor" in _trace(deny), _trace(deny)
        assert "AI: ANSWER AFTER DIAL" in text, "the turn did not answer after n:\n" + text
        assert requests == [], "a denied search reached the server: %r" % requests

        allow = tmp_path / "allow"; allow.mkdir()
        _seed_xwperc(allow, ["AISearchURL : " + _search_url(port), "AIWeb : on"])
        rows = _chat(allow, _mock_env(allow, reply, policy="ask"), "news?",
                     ["APPROVE?", RUNNING_SEARCH, "AI: ANSWER"],
                     keys_after=["y"], key_wait=APPROVE_KEYS)
        text = "\n".join(rows)
        assert RUNNING_SEARCH in text, "y did not run the search:\n" + text
        assert requests and requests[0].startswith("/search?q=xwpe%20borland%20editor"), requests
        del requests[:]

        auto = tmp_path / "auto"; auto.mkdir()
        _seed_xwperc(auto, ["AISearchURL : " + _search_url(port), "AIWeb : on"])
        rows = _chat(auto, _mock_env(auto, reply, policy="auto"), "news?",
                     ["auto-approved", RUNNING_SEARCH, "AI: ANSWER"])
        text = "\n".join(rows)
        assert "APPROVE?" not in text, "auto asked:\n" + text
        assert "[agent] auto-approved (auto): xwpe borland editor" in text, text
        assert requests and requests[0].startswith("/search?q=xwpe%20borland%20editor"), requests
        assert "AI: ANSWER AFTER DIAL" in text, text
    finally:
        srv.shutdown()
        srv.server_close()


# @scenario SC-ai-chat-54
def test_agent_web_tools_refuse_while_the_switch_is_off(tmp_path):
    """The same switch governs the Agent's web tools: with AIWeb off they
    refuse with one line naming the switch, and nothing is fetched."""
    requests = []
    srv, port = _web_server(requests)
    page = "http://127.0.0.1:%d/page" % port
    try:
        _seed_xwperc(tmp_path, ["AISearchURL : " + _search_url(port), "AIWeb : off"])
        env = _mock_env(tmp_path, "TOOL web_fetch %s@@TURN@@DONE ok" % page, policy="auto")
        with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
            s.key(ALT.AI); s.key("g", delay=0.6)
            s.key("do it", delay=0.1); s.key("\r", delay=1.0)
            assert _traced_within(tmp_path, "agent done", TURN_POLL_S), _trace(tmp_path)
            s._drain(0.8)
            rows = pane_text_with_history(s, ["refused"])
        text = "\n".join(rows)
        assert "[agent] web_fetch refused: web access is off (Options > AI turns it on)" in text, text
        assert "agent web off web_fetch " + page in _trace(tmp_path), _trace(tmp_path)
        assert requests == [], "the agent fetched with the web off: %r" % requests
    finally:
        srv.shutdown()
        srv.server_close()


# @scenario SC-ai-chat-54
def test_agent_web_tools_refuse_while_the_switch_is_unanswered(tmp_path):
    """An agent task runs with nobody to ask: the chat can show its notice
    because a question is waiting for an answer, but at the auto dial a task
    would reach the network with nothing on screen.  So an unanswered switch
    counts as off here, and the file is still left unanswered for the notice."""
    requests = []
    srv, port = _web_server(requests)
    page = "http://127.0.0.1:%d/page" % port
    try:
        _seed_xwperc(tmp_path, ["AISearchURL : " + _search_url(port)])
        env = _mock_env(tmp_path, "TOOL web_fetch %s@@TURN@@DONE ok" % page, policy="auto")
        with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
            s.key(ALT.AI); s.key("g", delay=0.6)
            s.key("do it", delay=0.1); s.key("\r", delay=1.0)
            assert _traced_within(tmp_path, "agent done", TURN_POLL_S), _trace(tmp_path)
            s._drain(0.8)
            rows = pane_text_with_history(s, ["refused"])
        text = "\n".join(rows)
        assert "[agent] web_fetch refused: web access is off (Options > AI turns it on)" in text, text
        assert requests == [], "the agent fetched with the switch unanswered: %r" % requests
        assert "AIWeb" not in _xwperc(tmp_path), \
            "the refusal answered the switch: %r" % _xwperc(tmp_path)
    finally:
        srv.shutdown()
        srv.server_close()


# @scenario SC-ai-chat-55
def test_every_query_and_page_is_visible_and_the_answer_names_its_sources(tmp_path):
    """THEN the transcript shows the query, the engine it went to and every
    URL read, and the answer ends with the sources it used."""
    requests = []
    srv, port = _web_server(requests)
    page = "http://127.0.0.1:%d/page" % port
    reply = ("TOOL web_search xwpe editor@@TURN@@TOOL web_fetch %s@@TURN@@"
             "ANSWER FROM THE PAGE" % page)
    try:
        _seed_xwperc(tmp_path, ["AISearchURL : " + _search_url(port), "AIWeb : on"])
        rows = _chat(tmp_path, _mock_env(tmp_path, reply, policy="auto"), "news?",
                     ["xwpe editor", "search engine", RUNNING_FETCH, "sources:"])
        text = "\n".join(rows)
        assert "[agent] auto-approved (auto): xwpe editor" in text, "the query is not shown:\n" + text
        assert "[agent] search engine: 127.0.0.1" in text, "the engine is not named:\n" + text
        assert "[agent] auto-approved (auto): " + page in text, "the URL read is not shown:\n" + text
        answer = next((i for i, r in enumerate(rows) if "AI: ANSWER FROM THE PAGE" in r), -1)
        sources = next((i for i, r in enumerate(rows) if r.startswith("sources:")), -1)
        assert answer >= 0, "no answer:\n" + text
        assert sources > answer, "no sources line after the answer:\n" + text
        assert page in rows[sources] and "/search?q=xwpe%20editor" in rows[sources], rows[sources]
        assert requests == ["/search?q=xwpe%20editor", "/page"], requests
    finally:
        srv.shutdown()
        srv.server_close()


# @scenario SC-ai-chat-56
def test_header_names_the_web_when_it_is_on(tmp_path):
    """THEN it reads "[AI: <backend> | model: <m> | policy: <p> | web]", and
    without the switch it reads as before."""
    on = tmp_path / "on"; on.mkdir()
    with WpeSession(str(on), SEED, env_extra=_mock_env(on, "ok", XWPE_AI_WEB="1")) as s:
        s.key(ALT.AI); s.key("a", delay=0.8)
        text = _screen(s)
    assert HEADER_ASK_WEB in text, "the header does not name the web:\n" + text

    off = tmp_path / "off"; off.mkdir()
    with WpeSession(str(off), SEED, env_extra=_mock_env(off, "ok", XWPE_AI_WEB="0")) as s:
        s.key(ALT.AI); s.key("a", delay=0.8)
        text = _screen(s)
    assert HEADER_ASK in text and "| web" not in text, "the header changed with the web off:\n" + text
