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
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

import pytest
from wpe_driver import WpeSession, ALT, WAIT_SCALE, assistant_build, wait_screen

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
