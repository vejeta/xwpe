"""What runs underneath every assistant action: backends, errors, async, state.

Each test drives one runtime behaviour once, on the in-process mock backend
or on a throwaway HTTP server bound to 127.0.0.1, and asserts on what the
user sees (the pane, the waiting line) or on what reaches the disk or the
wire (the session file, the trace, the request body, open sockets).
"""
import glob
import json
import os
import re
import socket
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

import pytest
from wpe_driver import WpeSession, ALT, WAIT_SCALE, WPE_BIN, assistant_build, wait_screen

pytestmark = pytest.mark.skipif(not assistant_build(),
                                reason="wpe built without --enable-ai")

SEED = "int main(void){return 0;}\n"
REFUSED_ENDPOINT = "http://127.0.0.1:1"      # port 1: nothing listens, connect() is refused
MOCK_DELAY_MS = 3000                          # long enough to read the waiting line twice


# ---------------------------------------------------------------- helpers --

def screen(s, drain=0.3):
    return "\n".join(s.display(drain=drain))


def write_xwperc(config_home, lines):
    """Write xwperc under <config_home>/xwpe and return that directory."""
    cfg = os.path.join(config_home, "xwpe")
    os.makedirs(cfg, exist_ok=True)
    with open(os.path.join(cfg, "xwperc"), "w") as fh:
        fh.write("[Programming]\n" + "".join(l + "\n" for l in lines))
    return cfg


def open_chat_and_send(s, prompt):
    s.key(ALT.AI, delay=0.5); s.key("a", delay=0.8)
    s.key(prompt, delay=0.3); s.key("\r", delay=0.5)


class SilentServer:
    """Accepts every connection on 127.0.0.1 and never answers."""

    def __init__(self):
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(16)
        self.sock.settimeout(0.2)
        self.port = self.sock.getsockname()[1]
        self.conns = []
        self.running = True
        threading.Thread(target=self._accept, daemon=True).start()

    def _accept(self):
        while self.running:
            try:
                conn, _ = self.sock.accept()
                self.conns.append(conn)
            except OSError:
                pass

    def stop(self):
        self.running = False
        for conn in self.conns:
            conn.close()
        self.sock.close()


class OpenAIServer:
    """A local OpenAI-compatible server on 127.0.0.1 that records each chat
    request (path, Authorization header, body) and answers with a fixed
    status: 200 streams REPLY, anything else returns {"error":{"message"}}."""

    REPLY = "RUNTIMEREPLY"

    def __init__(self, status=200, message=""):
        self.requests = []
        requests, reply = self.requests, self.REPLY

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                b = json.dumps({"object": "list", "data": [{"id": "m1"}]}).encode()
                self.send_response(200)
                self.send_header("Content-Length", str(len(b)))
                self.end_headers()
                self.wfile.write(b)

            def do_POST(self):
                n = int(self.headers.get("Content-Length", 0))
                requests.append({"auth": self.headers.get("Authorization"),
                                 "body": self.rfile.read(n).decode("utf-8", "replace")})
                if status != 200:
                    b = json.dumps({"error": {"message": message}}).encode()
                    self.send_response(status)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(b)))
                    self.end_headers()
                    self.wfile.write(b)
                    return
                b = ('data: {"choices":[{"delta":{"content":"%s"}}]}\n\n'
                     'data: [DONE]\n\n' % reply).encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.end_headers()
                self.wfile.write(b)

        self.srv = HTTPServer(("127.0.0.1", 0), H)
        self.port = self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    @property
    def endpoint(self):
        return "http://127.0.0.1:%d/v1" % self.port

    def stop(self):
        self.srv.shutdown()
        self.srv.server_close()


def openai_env(endpoint, **extra):
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "openai",
           "XWPE_AI_ENDPOINT": endpoint, "XWPE_AI_MODEL": "m1",
           "OPENAI_API_KEY": "x"}
    env.update(extra)
    return env


def inet_sockets(pid):
    """TCP/UDP sockets the process holds: its socket fds matched by inode
    against /proc/<pid>/net/{tcp,tcp6,udp,udp6} (unix sockets do not count)."""
    inodes = set()
    for fd in os.listdir("/proc/%d/fd" % pid):
        try:
            link = os.readlink("/proc/%d/fd/%s" % (pid, fd))
        except OSError:
            continue
        if link.startswith("socket:["):
            inodes.add(link[len("socket:["):-1])
    found = []
    for table in ("tcp", "tcp6", "udp", "udp6"):
        with open("/proc/%d/net/%s" % (pid, table)) as fh:
            for line in fh.readlines()[1:]:
                fields = line.split()
                if fields[9] in inodes:
                    found.append((table, fields[1], fields[2]))
    return found


WAIT_SECONDS = re.compile(r"\(gathering the answer\.\.\.\) (\S)\s+(\d+)s")

FROZEN_LIMIT_S = 1.5         # longer than this without a painted byte reads as a frozen editor
LISTING_DEADLINE_S = 5       # the model listing's deadline: connect, handshake and first byte
ANSWER_DEADLINE_S = 3        # XWPE_AI_TIMEOUT given to a chat that must end by itself
ANSWER_KEY_S = 3             # the same wait, set by AIAnswerTimeout in xwperc


def first_paint_after(s, keys, limit=20.0):
    """Send keys; return the seconds until the editor writes its next byte."""
    s._drain(0.3)
    n = len(s.raw)
    t0 = time.time()
    os.write(s.master_fd, keys.encode())
    while time.time() - t0 < limit:
        s._drain(0.05)
        if len(s.raw) > n:
            return time.time() - t0
    return limit


def typed_echo_delay(s, ch="Q", limit=15.0):
    """Type one character into whatever has the focus; seconds until it shows."""
    before = screen(s).count(ch)
    t0 = time.time()
    os.write(s.master_fd, ch.encode())
    while time.time() - t0 < limit:
        s._drain(0.1)
        if "\n".join(s.display(drain=0)).count(ch) > before:
            return time.time() - t0
    return limit


def wait_trace(path, needle, tmo):
    """Poll a trace file for needle; return the line holding it, or ''."""
    end = time.time() + tmo
    while time.time() < end:
        if path.exists():
            for line in path.read_text().splitlines():
                if needle in line:
                    return line
        time.sleep(0.1)
    return ""


def options_ai_env(tmp_path, endpoint, **extra):
    """Environment that opens Options > AI on the OpenAI-compatible backend
    pointed at endpoint, with the session's xwperc under a private HOME."""
    home = tmp_path / "home"
    write_xwperc(str(home / ".config"), ["AIBackend : 1", "AIEndpoint : %s" % endpoint,
                                         "AIPolicy : ask"])
    env = {"XWPE_AI_ENABLE": "1", "HOME": str(home), "OPENAI_API_KEY": ""}
    env.update(extra)
    return env


def open_ai_options(s):
    s.key(ALT.OPTIONS, delay=0.5); s.key("i", delay=0.8)


def tls_build():
    try:
        out = subprocess.run(["ldd", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"libssl" in out or b"libtls" in out
    except Exception:
        return False


# ------------------------------------------------------------------ tests --

# @scenario SC-ai-runtime-01
def test_ollama_refused_names_the_endpoint_and_editor_stays_usable(tmp_path):
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "ollama",
           "XWPE_AI_ENDPOINT": REFUSED_ENDPOINT, "XWPE_AI_MODEL": "m"}
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        open_chat_and_send(s, "hi")
        wait_screen(s, "not reachable", 6)
        disp = screen(s)
        s.key("\033", delay=0.5)
        s.key(ALT.AI, delay=0.8)
        menu = screen(s)
        alive = s.alive()
    assert "Ollama not reachable at %s" % REFUSED_ENDPOINT in disp, \
        "the refusal does not name the endpoint:\n" + disp
    assert "ollama serve" in disp, "the refusal does not say what to do:\n" + disp
    assert alive and "Ask (chat)" in menu, \
        "the editor did not take Alt-G after the refusal:\n" + menu


# @scenario SC-ai-runtime-02
def test_chat_on_silent_endpoint_keeps_editor_live_and_esc_cancels(tmp_path):
    srv = SilentServer()
    try:
        env = openai_env("http://127.0.0.1:%d/v1" % srv.port)
        with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
            open_chat_and_send(s, "hi")
            assert wait_screen(s, "gathering the answer", 5), \
                "no waiting line:\n" + screen(s)
            s.key("QZ", delay=0.6)
            typed = screen(s)
            # The socket reader used by the no-socket test sees this request.
            held = inet_sockets(s.proc.pid)
            s.key("\033", delay=1.0)
            after = screen(s)
    finally:
        srv.stop()
    assert held, "no TCP socket seen for the request in flight"
    assert "> QZ" in typed, "the pane took no keys while the request hung:\n" + typed
    assert "[AI] cancelled" in after, "Esc did not cancel the hung request:\n" + after
    assert "gathering the answer" not in after, \
        "the waiting line outlived the cancel:\n" + after


# @scenario SC-ai-runtime-02
def test_edit_on_silent_endpoint_keeps_editor_live_and_esc_cancels(tmp_path):
    srv = SilentServer()
    try:
        env = openai_env("http://127.0.0.1:%d/v1" % srv.port)
        with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
            s.key(ALT.AI, delay=0.5); s.key("e", delay=0.8)
            s.key("add a comment", delay=0.3); s.key("\r", delay=1.0)
            busy = screen(s)
            t0 = time.time()
            s.key("\033", delay=1.0)
            esc_s = time.time() - t0
            after = screen(s)
            s.key(ALT.AI, delay=0.8)
            menu = screen(s)
    finally:
        srv.stop()
    assert "working" in busy, "the Edit did not start:\n" + busy
    assert "cancelled" in after, "Esc did not cancel the hung Edit:\n" + after
    assert esc_s < 3 * WAIT_SCALE, "Esc took %.1fs to be handled" % esc_s
    assert "Ask (chat)" in menu, "the editor did not take Alt-G after the cancel:\n" + menu


# @scenario SC-ai-runtime-03
def test_chat_http_401_shows_the_server_reason(tmp_path):
    srv = OpenAIServer(status=401, message="Incorrect API key provided")
    try:
        with WpeSession(str(tmp_path), SEED, env_extra=openai_env(srv.endpoint)) as s:
            open_chat_and_send(s, "hi")
            wait_screen(s, "Incorrect API key", 8)
            disp = screen(s)
    finally:
        srv.stop()
    assert "AI: error: Incorrect API key provided" in disp, \
        "the 401 reason was not shown on the reply line:\n" + disp
    assert "gathering the answer" not in disp, "the waiting line stayed:\n" + disp


# @scenario SC-ai-runtime-04
def test_chat_http_500_shows_the_server_reason(tmp_path):
    srv = OpenAIServer(status=500, message="upstream exploded")
    try:
        with WpeSession(str(tmp_path), SEED, env_extra=openai_env(srv.endpoint)) as s:
            open_chat_and_send(s, "hi")
            wait_screen(s, "upstream exploded", 8)
            disp = screen(s)
    finally:
        srv.stop()
    assert "AI: error: upstream exploded" in disp, \
        "the 500 reason was not shown on the reply line:\n" + disp


# @scenario SC-ai-runtime-10
def test_xdg_dirs_hold_config_and_state_and_empty_falls_back_to_home(tmp_path):
    work = tmp_path / "work"
    work.mkdir()
    home = tmp_path / "home"
    xdg_cfg, xdg_state = tmp_path / "cfg", tmp_path / "state"
    write_xwperc(str(xdg_cfg), ["AIBackend : 3", "AIModel : xdgmarker", "AIPolicy : ask"])
    write_xwperc(str(home / ".config"), ["AIBackend : 3", "AIModel : homemarker",
                                         "AIPolicy : ask"])
    base = {"XWPE_AI_ENABLE": "1", "XWPE_AI_MOCK_REPLY": "noted", "HOME": str(home)}

    def run(xdg_config, xdg_statedir):
        env = dict(base, XDG_CONFIG_HOME=xdg_config, XDG_STATE_HOME=xdg_statedir)
        with WpeSession(str(work), SEED, env_extra=env) as s:
            open_chat_and_send(s, "hi")
            wait_screen(s, "AI: noted", 6)
            return screen(s)

    with_xdg = run(str(xdg_cfg), str(xdg_state))
    assert "model: xdgmarker" in with_xdg, \
        "xwperc was not read from $XDG_CONFIG_HOME/xwpe:\n" + with_xdg
    assert glob.glob(str(xdg_state / "xwpe" / "ai" / "*.session")), \
        "no session file under $XDG_STATE_HOME/xwpe/ai"

    empty = run("", "")
    assert "model: homemarker" in empty, \
        "an empty XDG_CONFIG_HOME did not fall back to ~/.config/xwpe:\n" + empty
    assert glob.glob(str(home / ".local" / "state" / "xwpe" / "ai" / "*.session")), \
        "an empty XDG_STATE_HOME did not fall back to ~/.local/state/xwpe"


# @scenario SC-ai-runtime-11
def test_api_key_never_in_pane_trace_or_session(tmp_path):
    secret = "sk-runtime-secret-4711"
    srv = OpenAIServer()
    home = tmp_path / "home"
    cfg = write_xwperc(str(home / ".config"), ["AIPolicy : ask"])
    with open(os.path.join(cfg, "openai-api-key"), "w") as fh:
        fh.write(secret + "\n")
    trace = tmp_path / "ai.trace"
    try:
        env = openai_env(srv.endpoint, OPENAI_API_KEY="", HOME=str(home),
                         XWPE_AI_TRACE=str(trace))
        with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
            open_chat_and_send(s, "hi")
            wait_screen(s, OpenAIServer.REPLY, 8)
            disp = screen(s)
    finally:
        srv.stop()
    assert srv.requests and srv.requests[0]["auth"] == "Bearer " + secret, \
        "the key file was not used: %r" % srv.requests
    assert secret not in disp, "the key is on screen:\n" + disp
    assert secret not in trace.read_text(), "the key is in the trace"
    sessions = glob.glob(str(home / ".local" / "state" / "xwpe" / "ai" / "*.session"))
    assert sessions, "no session file was written"
    for path in sessions:
        assert secret not in open(path).read(), "the key is in " + path


# @scenario SC-ai-runtime-12
def test_no_network_socket_while_assistant_is_off(tmp_path):
    # Read with /proc/<pid>/fd socket inodes matched against /proc/<pid>/net/tcp*.
    with WpeSession(str(tmp_path), SEED, env_extra={}) as s:
        at_start = inet_sockets(s.proc.pid)
        s.key(ALT.AI, delay=0.8)
        disp = screen(s)
        after_altg = inet_sockets(s.proc.pid)
    assert "AI assistant is off" in disp, "the assistant was not off:\n" + disp
    assert at_start == [], "a network socket was open at startup: %r" % at_start
    assert after_altg == [], "Alt-G opened a network socket: %r" % after_altg


# @scenario SC-ai-runtime-13
def test_session_resumes_in_its_workspace_and_not_in_another(tmp_path):
    srv = OpenAIServer()
    home = tmp_path / "home"
    home.mkdir()
    ws_a, ws_b = tmp_path / "a", tmp_path / "b"
    ws_a.mkdir(); ws_b.mkdir()
    env = openai_env(srv.endpoint, HOME=str(home))

    def chat(workdir, prompt):
        with WpeSession(str(workdir), SEED, env_extra=env) as s:
            open_chat_and_send(s, prompt)
            wait_screen(s, OpenAIServer.REPLY, 8)
        return srv.requests[-1]["body"]

    try:
        chat(ws_a, "remember ZEBRA")
        resumed = chat(ws_a, "again")
        other = chat(ws_b, "third")
    finally:
        srv.stop()
    assert "ZEBRA" in resumed, "the next launch in the same workspace did not resume"
    assert "ZEBRA" not in other, "another workspace received the first one's talk"
    files = glob.glob(str(home / ".local" / "state" / "xwpe" / "ai" / "*.session"))
    assert len(files) == 2, "expected one session file per workspace: %r" % files


# @scenario SC-ai-runtime-15
def test_second_request_is_queued_by_name(tmp_path):
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_POLICY": "auto",
           "XWPE_AI_MOCK_REPLY": "DONE finished",
           "XWPE_AI_MOCK_DELAY_MS": str(int(MOCK_DELAY_MS * 2 * WAIT_SCALE))}

    def start_agent(s, task):
        s.key(ALT.AI); s._drain(0.5)
        s.key("g"); s._drain(0.6)
        s.key(task); s._drain(0.3)
        s.key("\r", delay=0.8)

    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        start_agent(s, "TASKONE")
        start_agent(s, "TASKTWO")
        queued = wait_screen(s, '[AI] queued "TASKTWO"', 4)
        disp = screen(s)
    assert queued, "the queued request was not named:\n" + disp


# @scenario SC-ai-runtime-16
def test_waiting_line_counts_seconds_and_is_replaced(tmp_path):
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": "DONEREPLY",
           "XWPE_AI_MOCK_DELAY_MS": str(int(MOCK_DELAY_MS * WAIT_SCALE))}
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        open_chat_and_send(s, "hi")
        first = WAIT_SECONDS.search(screen(s, 0.1))
        s._drain(1.2)
        second = WAIT_SECONDS.search(screen(s, 0.1))
        wait_screen(s, "AI: DONEREPLY", 8)
        done = screen(s)
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        open_chat_and_send(s, "hi")
        s.key("\033", delay=1.0)
        cancelled = screen(s)
    assert first and second, "no waiting line with a seconds count"
    assert int(second.group(2)) > int(first.group(2)), \
        "the seconds did not advance: %s -> %s" % (first.group(0), second.group(0))
    assert "(Esc cancels)" in first.string, "the waiting line does not say Esc cancels"
    assert "AI: DONEREPLY" in done and "gathering the answer" not in done, \
        "the reply did not replace the waiting line:\n" + done
    assert "[AI] cancelled" in cancelled and "gathering the answer" not in cancelled, \
        "the cancel did not replace the waiting line:\n" + cancelled


# @scenario SC-ai-runtime-18
def test_build_and_fix_checks_a_lone_c_file_with_the_compiler(tmp_path):
    trace = tmp_path / "ai.trace"
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_POLICY": "ask",
           "XWPE_AI_MOCK_REPLY": "DONE nothing to fix", "XWPE_AI_TRACE": str(trace)}
    with WpeSession(str(tmp_path), SEED, filename="lone.c", env_extra=env) as s:
        s.key(ALT.AI, delay=0.5); s.key("b", delay=1.0)
        wait_screen(s, "build command", 5)
        disp = screen(s)
    want = "gcc -fsyntax-only -Wall '%s'" % (tmp_path / "lone.c")
    assert "fix-build cmd=" + want in trace.read_text(), \
        "the build command for a lone C file is not a syntax check:\n" + trace.read_text()
    assert "[fix-build] build command: gcc -fsyntax-only -Wall" in disp, \
        "the pane does not show the build command:\n" + disp


# @scenario SC-ai-runtime-02
def test_listing_on_silent_endpoint_says_it_waits_and_ends_at_its_deadline(tmp_path):
    srv = SilentServer()
    trace = tmp_path / "ai.trace"
    host = "127.0.0.1:%d" % srv.port
    try:
        env = options_ai_env(tmp_path, "http://%s/v1" % host, XWPE_AI_TRACE=str(trace))
        with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
            open_ai_options(s)
            t0 = time.time()
            painted = first_paint_after(s, "\033m")
            waiting = screen(s, 0.2)
            ended = wait_trace(trace, "MODELS", LISTING_DEADLINE_S + 4)
            elapsed = time.time() - t0
    finally:
        srv.stop()
    assert painted < FROZEN_LIMIT_S, \
        "Alt-M froze the editor %.1fs on an endpoint that never answers" % painted
    assert "listing models from %s" % host in waiting, \
        "no waiting line while the listing waits:\n" + waiting
    assert "no answer from %s within %d s" % (host, LISTING_DEADLINE_S) in ended, \
        "the listing did not end at its deadline naming the endpoint: %r" % ended
    assert elapsed < LISTING_DEADLINE_S + 2.5, "the listing ran %.1fs" % elapsed


# @scenario SC-ai-runtime-02
def test_listing_on_silent_endpoint_is_abandoned_by_esc(tmp_path):
    srv = SilentServer()
    host = "127.0.0.1:%d" % srv.port
    try:
        env = options_ai_env(tmp_path, "http://%s/v1" % host)
        with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
            open_ai_options(s)
            s.key("\033m", delay=1.0)
            t0 = time.time()
            os.write(s.master_fd, b"\033")
            cancelled = wait_screen(s, "listing cancelled", 3)
            esc_s = time.time() - t0
            disp = screen(s)
    finally:
        srv.stop()
    assert cancelled, "Esc did not abandon the listing:\n" + disp
    assert esc_s < LISTING_DEADLINE_S - 1, "Esc took %.1fs to abandon the listing" % esc_s


# @scenario SC-ai-runtime-02
@pytest.mark.skipif(not tls_build(), reason="wpe built without TLS")
def test_chat_over_https_on_silent_endpoint_stays_live_and_ends_at_deadline(tmp_path):
    srv = SilentServer()
    host = "127.0.0.1:%d" % srv.port
    try:
        env = openai_env("https://%s/v1" % host, XWPE_AI_TIMEOUT=str(ANSWER_DEADLINE_S))
        with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
            open_chat_and_send(s, "hi")
            t0 = time.time()
            echo = typed_echo_delay(s, "Q", limit=ANSWER_DEADLINE_S + 10)
            ended = wait_screen(s, "no answer from %s within %d s" % (host, ANSWER_DEADLINE_S),
                                ANSWER_DEADLINE_S + 5)
            elapsed = time.time() - t0
            disp = screen(s)
    finally:
        srv.stop()
    assert echo < FROZEN_LIMIT_S, \
        "a chat on an https endpoint that never answers froze the editor %.1fs" % echo
    assert ended, "the chat did not end at its deadline naming the endpoint:\n" + disp
    assert "gathering the answer" not in disp, "the waiting line outlived the deadline:\n" + disp
    assert elapsed < ANSWER_DEADLINE_S + 4, "the chat ran %.1fs" % elapsed


# @scenario SC-ai-runtime-02
def test_answer_timeout_comes_from_xwperc_and_names_the_key(tmp_path):
    """AIAnswerTimeout in xwperc sets how long a request waits for the server's
    first byte, and the line that ends it names the key, so a user whose local
    model is still thinking learns from the message itself what to raise."""
    cfg = tmp_path / ".config" / "xwpe"
    cfg.mkdir(parents=True)
    (cfg / "xwperc").write_text("[Programming]\nAIAnswerTimeout : %d\n"
                                % ANSWER_KEY_S)
    srv = SilentServer()
    host = "127.0.0.1:%d" % srv.port
    try:
        env = openai_env("http://%s/v1" % host)
        with WpeSession(str(tmp_path), SEED, env_extra=env, cols=160) as s:
            open_chat_and_send(s, "hi")
            ended = wait_screen(s, "within %d s" % ANSWER_KEY_S,
                                ANSWER_KEY_S + 10)
            disp = screen(s)
    finally:
        srv.stop()
    # A narrow pane wraps this line, so compare it with the whitespace taken out
    # rather than assuming it lands on one unbroken row.
    flat = "".join(disp.split())
    want = "".join(("no answer from %s within %d s (AIAnswerTimeout)"
                    % (host, ANSWER_KEY_S)).split())
    assert ended, \
        "the chat did not end at the deadline xwperc gave it:\n" + disp
    assert want in flat, \
        "the line that ended the request does not name the endpoint, the wait " \
        "and the key that sets it:\n" + disp
    assert "gathering the answer" not in disp, \
        "the waiting line outlived the deadline:\n" + disp


class ListingServer:
    """Answers the model listing (GET) with a fixed status and JSON body."""

    def __init__(self, status, body):
        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                b = json.dumps(body).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(b)))
                self.end_headers()
                self.wfile.write(b)

        self.srv = HTTPServer(("127.0.0.1", 0), H)
        self.port = self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def stop(self):
        self.srv.shutdown()
        self.srv.server_close()


def listing_with_dialog_open(tmp_path, endpoint):
    """Alt-M in Options > AI; the screen while the dialog is open, then after
    one Esc (which closes whatever box is on top)."""
    with WpeSession(str(tmp_path), SEED, env_extra=options_ai_env(tmp_path, endpoint)) as s:
        open_ai_options(s)
        s.key("\033m", delay=2.0)
        during = screen(s, 0.5)
        s.key("\033", delay=0.8)
        after = screen(s, 0.5)
    return during, after


# @scenario SC-ai-runtime-03
def test_listing_http_401_shows_the_reason_over_the_dialog(tmp_path):
    srv = ListingServer(401, {"error": {"message": "Incorrect API key provided",
                                        "type": "invalid_request_error"}})
    host = "127.0.0.1:%d" % srv.port
    try:
        during, after = listing_with_dialog_open(tmp_path, "http://%s/v1" % host)
    finally:
        srv.stop()
    assert "HTTP 401 from %s: Incorrect API key provided" % host in during, \
        "the listing's 401 reason is not shown over the dialog:\n" + during
    assert "AI settings" in during, "the dialog is gone under the message:\n" + during
    assert "Incorrect API key" not in after, "Esc did not close the message:\n" + after
    assert "AI settings" in after and "Model (Alt-M)" in after, \
        "the dialog was not intact after the message closed:\n" + after


# @scenario SC-ai-runtime-07
def test_listing_error_is_shown_over_the_open_dialog(tmp_path):
    during, after = listing_with_dialog_open(tmp_path, REFUSED_ENDPOINT + "/v1")
    assert "cannot connect to 127.0.0.1:1" in during, \
        "the listing error was not visible while the dialog is open:\n" + during
    assert "AI settings" in after and "cannot connect" not in after, \
        "Esc did not leave the dialog intact without the message:\n" + after


def sse_server(payload):
    """POST answers HTTP 200 text/event-stream with payload, then closes."""
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_POST(self):
            self.rfile.read(int(self.headers.get("Content-Length", 0)))
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            self.wfile.write(payload)
            self.wfile.flush()
            self.close_connection = True

    srv = HTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


# @scenario SC-ai-runtime-04
def test_error_event_after_text_ends_the_turn_visibly(tmp_path):
    partial, reason = "PARTIALWORDS", "upstream overloaded mid-reply"
    srv = sse_server(('data: {"choices":[{"delta":{"content":"%s "}}]}\n\n'
                      'data: {"error":{"message":"%s"}}\n\n' % (partial, reason)).encode())
    home = tmp_path / "home"
    home.mkdir()
    try:
        env = openai_env("http://127.0.0.1:%d/v1" % srv.server_address[1], HOME=str(home))
        with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
            open_chat_and_send(s, "hi")
            wait_screen(s, "overloaded", 6)
            disp = screen(s, 1.0)
    finally:
        srv.shutdown()
    assert partial in disp, "the text before the error did not stay:\n" + disp
    assert reason in disp, "the error after the text was not shown:\n" + disp
    sessions = glob.glob(str(home / ".local" / "state" / "xwpe" / "ai" / "*.session"))
    assert sessions, "no session file was written"
    turns = json.load(open(sessions[0]))["messages"]
    answers = [m["content"] for m in turns if m["role"] == "assistant"]
    assert answers, "the partial turn was not kept in the session: %r" % turns
    assert all(a.strip() != partial for a in answers), \
        "the cut reply was saved as a finished answer: %r" % answers
    assert any(partial in a and reason in a for a in answers), \
        "the session does not keep the partial text with the error note: %r" % answers


def self_signed_tls_server(tmp_path):
    """An https server on 127.0.0.1 with a certificate nobody trusts."""
    import ssl
    cert, key = str(tmp_path / "c.pem"), str(tmp_path / "k.pem")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                    "-keyout", key, "-out", cert, "-days", "1", "-subj", "/CN=127.0.0.1",
                    "-addext", "subjectAltName=IP:127.0.0.1"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)

    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

    srv = HTTPServer(("127.0.0.1", 0), H)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


class ClosingServer(SilentServer):
    """Accepts every connection on 127.0.0.1 and closes it at once."""

    def _accept(self):
        while self.running:
            try:
                conn, _ = self.sock.accept()
                conn.close()
            except OSError:
                pass


def chat_line_after_tls_failure(tmp_path, port):
    env = openai_env("https://127.0.0.1:%d/v1" % port)
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        open_chat_and_send(s, "hi")
        wait_screen(s, "TLS", 8)
        return " ".join(screen(s, 0.5).split())


# @scenario SC-ai-runtime-05
@pytest.mark.skipif(not tls_build(), reason="wpe built without TLS")
def test_tls_untrusted_certificate_names_endpoint_reason_and_ca_advice(tmp_path):
    srv = self_signed_tls_server(tmp_path)
    port = srv.server_address[1]
    try:
        line = chat_line_after_tls_failure(tmp_path, port)
    finally:
        srv.shutdown()
    assert "127.0.0.1:%d" % port in line, "the TLS line lacks host:port:\n" + line
    assert "certificate verify failed" in line, "the TLS line lacks the reason:\n" + line
    assert "CA file" in line, "the TLS line does not point at the CA file:\n" + line
    assert "server/bridge" not in line, "a certificate problem got the down-server advice"


# @scenario SC-ai-runtime-05
@pytest.mark.skipif(not tls_build(), reason="wpe built without TLS")
def test_tls_peer_closing_in_handshake_names_endpoint_without_ca_advice(tmp_path):
    srv = ClosingServer()
    try:
        line = chat_line_after_tls_failure(tmp_path, srv.port)
    finally:
        srv.stop()
    reason = re.search(r"TLS handshake/verify failed with 127\.0\.0\.1:%d: (\w[^>]*)" % srv.port,
                       line)
    assert reason, "the TLS line lacks host:port and a reason:\n" + line
    assert "CA file" not in line, "a closed connection got the CA advice:\n" + line


# @scenario SC-ai-runtime-11
def test_rewritten_key_file_is_made_private(tmp_path):
    home = tmp_path / "home"
    cfg = write_xwperc(str(home / ".config"), ["AIBackend : 1",
                                               "AIEndpoint : %s/v1" % REFUSED_ENDPOINT,
                                               "AIPolicy : ask"])
    kf = os.path.join(cfg, "openai-api-key")
    with open(kf, "w") as fh:
        fh.write("old-key\n")
    os.chmod(kf, 0o644)
    env = {"XWPE_AI_ENABLE": "1", "HOME": str(home), "OPENAI_API_KEY": ""}
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        open_ai_options(s)
        s.key("\033k", delay=0.5)
        for ch in "new-key-999":
            s.key(ch, delay=0.03)
        s.key("\033o", delay=1.0)
    content = open(kf).read().strip()
    mode = os.stat(kf).st_mode & 0o777
    assert content == "new-key-999", "the typed key was not written: %r" % content
    assert mode == 0o600, "the key file holding the new key is mode %o" % mode


# @scenario SC-ai-runtime-14
def test_cut_current_file_is_marked_in_prompt_and_said_in_pane(tmp_path):
    lines = ["/* line %05d xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx */" % i
             for i in range(250)]
    srv = OpenAIServer()
    try:
        with WpeSession(str(tmp_path), "\n".join(lines) + "\n",
                        env_extra=openai_env(srv.endpoint)) as s:
            open_chat_and_send(s, "hi")
            wait_screen(s, OpenAIServer.REPLY, 8)
            disp = screen(s)
    finally:
        srv.stop()
    system = json.loads(srv.requests[0]["body"])["messages"][0]["content"]
    sent = sum(1 for l in lines if l in system)
    assert sent < len(lines), "the file was not cut; this test needs a cut"
    assert "(current file cut at line %d of %d)" % (sent, len(lines)) in system, \
        "the prompt carries no cut mark after line %d:\n%s" % (sent, system[-300:])
    assert "t.c: %d of %d lines sent" % (sent, len(lines)) in disp, \
        "the pane does not say how much of the file was sent:\n" + disp


# @scenario SC-ai-runtime-08
def test_stale_model_of_another_backend_is_named(tmp_path):
    srv = OpenAIServer(status=400, message="REPLYSEEN")
    try:
        home = tmp_path / "home"
        write_xwperc(str(home / ".config"), ["AIBackend : 2", "AIModel : qwen3:8b",
                                             "AIEndpoint : http://127.0.0.1:%d" % srv.port,
                                             "AIPolicy : ask"])
        env = {"XWPE_AI_ENABLE": "1", "HOME": str(home), "ANTHROPIC_API_KEY": "x"}
        with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
            open_chat_and_send(s, "hi")
            wait_screen(s, "REPLYSEEN", 8)
            disp = screen(s)
    finally:
        srv.stop()
    assert srv.requests and "qwen3:8b" not in srv.requests[0]["body"], \
        "the stale model was sent"
    assert "[AI] model qwen3:8b is not claude's; using" in disp, \
        "the stale model was dropped without a word:\n" + disp


# @scenario SC-ai-runtime-18
def test_build_and_fix_with_no_build_command_runs_nothing(tmp_path):
    trace = tmp_path / "ai.trace"
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_POLICY": "ask",
           "XWPE_AI_MOCK_REPLY": "DONE nothing", "XWPE_AI_TRACE": str(trace)}
    with WpeSession(str(tmp_path), "print('x')\n", filename="x.py", env_extra=env) as s:
        s.key(ALT.AI, delay=0.5); s.key("b", delay=1.0)
        wait_screen(s, "fix-build", 5)
        s._drain(1.5)
        disp = screen(s)
    log = trace.read_text() if trace.exists() else ""
    # The pane wraps the line at its width; each fact it carries sits whole on a row.
    assert "[fix-build] no build command for x.py: no makefile here" in disp, \
        "the pane does not say there is no build command:\n" + disp
    assert "only C and C++" in disp and "syntax check" in disp, \
        "the pane does not say which file types have a check:\n" + disp
    assert "fix-build cmd=" not in log, "a build command was run:\n" + log
    assert "DONE nothing" not in disp, "the agent ran without a build command:\n" + disp


# ------------------------------------------- waits on the model's server --

# How long the stand-in takes to list its models: long enough to type during
# it, and always inside the listing's own deadline, whatever the wait scale.
MODELS_DELAY_S = min(3 * WAIT_SCALE, LISTING_DEADLINE_S - 1.5)
ESC = "\033"
DOWN = "\033[B"


class SlowModelsServer:
    """Ollama or OpenAI-compatible on 127.0.0.1: the model listing answers
    after MODELS_DELAY_S with the given names; a chat request is recorded
    and answered at once with `reply` (Ollama NDJSON)."""

    def __init__(self, names, reply="REPLY"):
        self.chats = []
        chats = self.chats

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                time.sleep(MODELS_DELAY_S)
                if self.path.endswith("/models"):
                    body = {"object": "list", "data": [{"id": n} for n in names]}
                else:
                    body = {"models": [{"name": n} for n in names]}
                b = json.dumps(body).encode()
                try:
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(b)))
                    self.end_headers()
                    self.wfile.write(b)
                except OSError:
                    pass                    # the editor gave the listing up: expected

            def do_POST(self):
                n = int(self.headers.get("Content-Length", 0))
                chats.append(self.rfile.read(n).decode("utf-8", "replace"))
                b = (json.dumps({"message": {"role": "assistant", "content": reply},
                                 "done": False}) + "\n" +
                     json.dumps({"done": True}) + "\n").encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/x-ndjson")
                self.send_header("Content-Length", str(len(b)))
                self.end_headers()
                self.wfile.write(b)

        class Threaded(HTTPServer):
            daemon_threads = True

            def process_request(self, request, client_address):
                threading.Thread(target=self.process_request_thread,
                                 args=(request, client_address), daemon=True).start()

            def process_request_thread(self, request, client_address):
                try:
                    self.finish_request(request, client_address)
                finally:
                    self.shutdown_request(request)

        self.srv = Threaded(("127.0.0.1", 0), H)
        self.port = self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    @property
    def url(self):
        return "http://127.0.0.1:%d" % self.port

    def stop(self):
        self.srv.shutdown()
        self.srv.server_close()


def ollama_no_model_env(url, **extra):
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "ollama",
           "XWPE_AI_ENDPOINT": url, "XWPE_AI_MODEL": "", "XWPE_AI_POLICY": "ask"}
    env.update(extra)
    return env


def ask_for(s, letter, goal):
    s.key(ALT.AI, delay=0.5); s.key(letter, delay=0.8)
    s.key(goal, delay=0.3); s.key("\r", delay=0.3)


def seconds_until_on_screen(s, needle, limit):
    t0 = time.time()
    while time.time() - t0 < limit:
        s._drain(0.1)
        if needle in "\n".join(s.display(drain=0)):
            return time.time() - t0
    return limit


def first_menu_after(s, keys, needle, limit=MODELS_DELAY_S + 3):
    """Type keys; the seconds until needle (a menu row) is on the screen."""
    s._drain(0.1)
    os.write(s.master_fd, keys.encode())
    return seconds_until_on_screen(s, needle, limit)


# The model listing before a task: Edit, Multi-file edit and Agent each wait
# for it the same way.
TASK_LETTERS = [("edit", "e"), ("plan", "f"), ("agent", "g")]


# @scenario SC-ai-runtime-19
@pytest.mark.parametrize("mode,letter", TASK_LETTERS, ids=[m for m, _ in TASK_LETTERS])
def test_model_listing_before_a_task_takes_esc_and_sends_nothing(tmp_path, mode, letter):
    srv = SlowModelsServer(["m1"])
    try:
        with WpeSession(str(tmp_path), SEED, env_extra=ollama_no_model_env(srv.url)) as s:
            ask_for(s, letter, "do it")
            waiting = screen(s, 0.3)
            t0 = time.time()
            os.write(s.master_fd, ESC.encode())
            esc_s = seconds_until_on_screen(s, "[AI] cancelled", MODELS_DELAY_S + 3)
            s._drain(max(0.0, MODELS_DELAY_S + 1.0 - (time.time() - t0)))
            after = screen(s)
    finally:
        srv.stop()
    assert esc_s < FROZEN_LIMIT_S, \
        "Esc took %.1fs to cancel the model listing (the editor waited for it)" % esc_s
    assert not srv.chats, "the %s request went out after Esc:\n%r" % (mode, srv.chats)
    assert "[AI] cancelled" in after, "the cancel was not said:\n" + after
    assert "choosing a model from 127.0.0.1:%d" % srv.port in waiting, \
        "no waiting line while the model listing runs:\n" + waiting


# @scenario SC-ai-runtime-19
def test_model_listing_before_an_edit_takes_keys_and_the_edit_then_runs(tmp_path):
    new = "int main(void){return 7;}"
    srv = SlowModelsServer(["m1"], reply=new)
    try:
        with WpeSession(str(tmp_path), SEED, env_extra=ollama_no_model_env(srv.url)) as s:
            ask_for(s, "e", "return 7")
            menu_s = first_menu_after(s, ALT.AI, "Ask (chat)")
            s.key(ESC, delay=0.5)                      # closes the menu, not the task
            shown = wait_screen(s, "Proposed change", MODELS_DELAY_S + 8)
            s.key("a", delay=1.0)
            s.save()
            text = s.text()
    finally:
        srv.stop()
    assert menu_s < FROZEN_LIMIT_S, \
        "Alt-G took %.1fs to open while the models were listed" % menu_s
    assert srv.chats and '"model":"m1"' in srv.chats[0].replace(" ", ""), \
        "the Edit did not go out with the model the listing chose: %r" % srv.chats
    assert shown, "the Edit's review did not open once the model was chosen"
    assert text.startswith(new), "the reply was not applied:\n" + repr(text)


# @scenario SC-ai-runtime-19
def test_text_typed_while_the_model_is_chosen_lands_in_the_file(tmp_path):
    srv = SlowModelsServer(["m1"])
    try:
        with WpeSession(str(tmp_path), SEED, env_extra=ollama_no_model_env(srv.url)) as s:
            ask_for(s, "e", "do it")
            waiting = screen(s, 0.3)
            s.key("TYPED", delay=0.5)
            os.write(s.master_fd, ESC.encode())
            seconds_until_on_screen(s, "[AI] cancelled", MODELS_DELAY_S + 3)
            s.key("MORE", delay=0.5)                   # after Esc the file still has the keys
            s.save()
            text = s.text()
    finally:
        srv.stop()
    assert "choosing a model from" in waiting, "the check did not wait:\n" + waiting
    assert "TYPED" in text and "MORE" in text, \
        "keys typed while the model was chosen did not reach the file:\n" + repr(text)
    assert not srv.chats, "a request went out: %r" % srv.chats


# @scenario SC-ai-runtime-20
def test_a_down_typed_while_the_models_list_moves_the_picker_when_they_come(tmp_path):
    srv = SlowModelsServer(["m1", "m2", "m3"])
    try:
        env = options_ai_env(tmp_path, srv.url + "/v1")
        with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
            open_ai_options(s)
            s.key("\033m", delay=0.5)
            os.write(s.master_fd, DOWN.encode())
            listed = wait_screen(s, "Model (3) at", MODELS_DELAY_S + 5)
            s.key("\r", delay=1.0)
            dialog = screen(s)
    finally:
        srv.stop()
    assert listed, "the picker never showed the list:\n" + dialog
    assert "listing cancelled" not in dialog, "the typed key cancelled the listing:\n" + dialog
    assert re.search(r"\bm2\b", dialog), \
        "the Down typed while the list loaded was lost (m2 not chosen):\n" + dialog


def editor_children(pid):
    """The pids whose parent is the editor (its own threads' children lists)."""
    kids = set()
    for tid in os.listdir("/proc/%d/task" % pid):
        try:
            with open("/proc/%d/task/%s/children" % (pid, tid)) as fh:
                kids.update(int(k) for k in fh.read().split())
        except OSError:
            pass
    return kids


# @scenario SC-ai-runtime-21
def test_a_late_test_reply_starts_no_process(tmp_path):
    trace = tmp_path / "ai.trace"
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": "other text", "XWPE_AI_TRACE": str(trace),
           "XWPE_AI_MOCK_DELAY_MS": str(int(5000 * WAIT_SCALE))}
    with WpeSession(str(tmp_path), "plain text\n", filename="notes.txt", env_extra=env) as s:
        before = editor_children(s.proc.pid)
        ask_for(s, "e", "change it")
        started = wait_trace(trace, "edit stream fd=", 5)
        during = editor_children(s.proc.pid)
        reply = wait_screen(s, "Proposed change", 10)
        s.key(ESC, delay=0.5)
    assert started, "the Edit did not start"
    assert during == before, \
        "the late test reply started a process: %r" % sorted(during - before)
    assert reply, "the late reply never came"


PAGE_DELAY_S = 3 * WAIT_SCALE     # how long the stand-in takes to serve a web page
PAGE_WORDS = "SLOWPAGEWORDS"


class AgentWithSlowPageServer:
    """OpenAI-compatible on 127.0.0.1: the agent's first turn asks for a web
    page served by the same server after PAGE_DELAY_S, the second ends the
    run; every chat request body is recorded."""

    def __init__(self):
        self.chats = []
        chats, port = self.chats, {}

        class H(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.0"

            def log_message(self, *a):
                pass

            def do_GET(self):
                time.sleep(PAGE_DELAY_S)
                b = ("<html><body>%s</body></html>" % PAGE_WORDS).encode()
                try:
                    self.send_response(200)
                    self.send_header("Content-Type", "text/html")
                    self.send_header("Content-Length", str(len(b)))
                    self.end_headers()
                    self.wfile.write(b)
                except OSError:
                    pass

            def do_POST(self):
                n = int(self.headers.get("Content-Length", 0))
                chats.append(self.rfile.read(n).decode("utf-8", "replace"))
                if len(chats) == 1:
                    content = "TOOL web_fetch http://127.0.0.1:%d/page" % port["n"]
                else:
                    content = "DONE read it"
                b = (('data: {"choices":[{"delta":{"content":%s}}]}\n\n'
                      % json.dumps(content)).encode() + b'data: [DONE]\n\n')
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.end_headers()
                self.wfile.write(b)

        class Threaded(HTTPServer):
            def process_request(self, request, client_address):
                threading.Thread(target=self._one, args=(request, client_address),
                                 daemon=True).start()

            def _one(self, request, client_address):
                try:
                    self.finish_request(request, client_address)
                finally:
                    self.shutdown_request(request)

        self.srv = Threaded(("127.0.0.1", 0), H)
        self.port = port["n"] = self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def stop(self):
        self.srv.shutdown()
        self.srv.server_close()


# @scenario SC-ai-runtime-23
def test_an_agent_web_page_is_read_while_the_editor_takes_keys(tmp_path):
    srv = AgentWithSlowPageServer()
    try:
        env = openai_env("http://127.0.0.1:%d/v1" % srv.port,
                         XWPE_AI_POLICY="auto", XWPE_AI_WEB="1")
        with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
            ask_for(s, "g", "read the page")
            running = seconds_until_on_screen(s, "running web_fetch", 6)
            menu_s = first_menu_after(s, ALT.AI, "Ask (chat)")
            s.key(ESC, delay=0.5)                      # closes the menu, not the run
            end = time.time() + PAGE_DELAY_S + 8
            while time.time() < end and len(srv.chats) < 2:
                s._drain(0.3)
    finally:
        srv.stop()
    assert running < 6, "the agent never asked for the page"
    assert menu_s < FROZEN_LIMIT_S, \
        "Alt-G took %.1fs to open while the agent read a web page" % menu_s
    assert len(srv.chats) >= 2 and PAGE_WORDS in srv.chats[1], \
        "the page did not reach the model's next turn: %r" % srv.chats[1:]


STALE_ORIG = "int a = 1;\nint b = 2;\nint c = 3;\nint d = 4;\nint e = 5;\n"
STALE_REGION = "int b = 22;\nint c = 33;\n"
STALE_WANT = "\nint a = 1;\nint b = 22;\nint c = 33;\nint d = 4;\nint e = 5;\n"


# @scenario SC-ai-runtime-22
def test_an_edit_reply_is_not_spliced_after_edit_undo_moved_its_lines(tmp_path):
    trace = tmp_path / "ai.trace"
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": STALE_REGION, "XWPE_AI_TRACE": str(trace),
           "XWPE_AI_MOCK_DELAY_MS": str(int(6000 * WAIT_SCALE))}
    with WpeSession(str(tmp_path), STALE_ORIG, env_extra=env) as s:
        s.key("\r", delay=0.4)                         # a blank first line: one undo step
        s.key(DOWN)                                    # line 3, "int b = 2;"
        s.key(ALT.BLOCK, "b", delay=0.4)
        s.key(DOWN); s.key(DOWN)
        s.key(ALT.BLOCK, "e", delay=0.4)               # lines 3-4 marked
        ask_for(s, "e", "double them")
        started = wait_trace(trace, "sel=2..3", 6)
        s.menu(ALT.EDIT, "u")                          # Edit > Undo while the reply is out
        s._drain(0.6)
        shown = wait_screen(s, "Proposed change", 12)
        s.key("a", delay=1.0)
        s.save()
        text = s.text()
    assert started, "the block Edit did not start:\n" + (trace.read_text() if trace.exists() else "")
    assert shown, "the review did not open"
    assert text == STALE_WANT, \
        "the reply was spliced at lines that had moved:\n" + repr(text)


SLOW_CHAT_S = 5 * WAIT_SCALE      # how long the stand-in takes to answer a request
SLOW_CHAT_REPLY = "int main(void){return 9;}"


class SlowChatServer:
    """Ollama on 127.0.0.1 with a model: every chat request is answered after
    SLOW_CHAT_S with SLOW_CHAT_REPLY."""

    def __init__(self):
        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_POST(self):
                self.rfile.read(int(self.headers.get("Content-Length", 0)))
                time.sleep(SLOW_CHAT_S)
                b = (json.dumps({"message": {"role": "assistant", "content": SLOW_CHAT_REPLY},
                                 "done": False}) + "\n" +
                     json.dumps({"done": True}) + "\n").encode()
                try:
                    self.send_response(200)
                    self.send_header("Content-Length", str(len(b)))
                    self.end_headers()
                    self.wfile.write(b)
                except OSError:
                    pass                    # the editor gave the request up: expected

        class Threaded(HTTPServer):
            def process_request(self, request, client_address):
                threading.Thread(target=self._one, args=(request, client_address),
                                 daemon=True).start()

            def _one(self, request, client_address):
                try:
                    self.finish_request(request, client_address)
                finally:
                    self.shutdown_request(request)

        self.srv = Threaded(("127.0.0.1", 0), H)
        self.port = self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def stop(self):
        self.srv.shutdown()
        self.srv.server_close()


# @scenario SC-ai-runtime-24
def test_opening_the_chat_keeps_the_running_task_cancellable(tmp_path):
    srv = SlowChatServer()
    trace = tmp_path / "ai.trace"
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "ollama",
           "XWPE_AI_ENDPOINT": "http://127.0.0.1:%d" % srv.port,
           "XWPE_AI_MODEL": "m1", "XWPE_AI_POLICY": "ask",
           "XWPE_AI_TRACE": str(trace)}
    try:
        with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
            ask_for(s, "e", "return 9")
            started = wait_trace(trace, "edit stream fd=", 5)
            s.key(ALT.AI, delay=0.5); s.key("a", delay=1.5)   # the chat opens
            s.key(ESC, delay=0.8); s.key(ESC, delay=0.8)
            after_esc = screen(s)
            offered = wait_screen(s, "Proposed change", SLOW_CHAT_S + 3)
    finally:
        srv.stop()
    assert started, "the Edit did not start"
    assert "[AI] cancelled" in after_esc, \
        "Esc no longer cancels the running Edit once the chat opened:\n" + after_esc
    assert not offered, "the cancelled Edit's reply was still offered"
