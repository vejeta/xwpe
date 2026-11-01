"""The Claude backend sends the workspace ID when one is configured.

A Console key that belongs to the organization, not to one workspace, is
refused by the API unless each request names the workspace.  The editor reads
the ID the way it reads the key (environment, then <NAME>_FILE, then the
config file) and sends it as the anthropic-workspace-id header; a workspace
key needs nothing and no header may appear.  A local plain-http listener
records the headers of the request the editor makes.
"""
import http.server
import os
import subprocess
import threading

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

HEADER = "anthropic-workspace-id"
WAIT_ROUNDS = 10


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(),
                                reason="wpe built without --enable-ai")


class _Recorder(http.server.BaseHTTPRequestHandler):
    """Remember the headers of every request and answer with an error body."""

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        if length:
            self.rfile.read(length)
        self.server.seen.append({k.lower(): v for k, v in self.headers.items()})
        body = b'{"error":{"message":"recorded"}}'
        self.send_response(400)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def _ask_once(tmp_path, env_more):
    """Run one chat turn against the recorder; return the headers it saw."""
    srv = http.server.HTTPServer(("127.0.0.1", 0), _Recorder)
    srv.seen = []
    th = threading.Thread(target=srv.serve_forever, daemon=True)
    th.start()
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "claude",
           "XWPE_AI_ENDPOINT": "http://127.0.0.1:%d" % srv.server_address[1],
           "XWPE_AI_MODEL": "m", "ANTHROPIC_API_KEY": "test-key-not-real",
           "ANTHROPIC_WORKSPACE_ID": "", "ANTHROPIC_WORKSPACE_ID_FILE": ""}
    env.update(env_more)
    try:
        with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                        env_extra=env, filename="a.c") as s:
            s.key(ALT.AI)
            s.key("a")
            s.key("hi")
            s.key("\r", delay=1.0)
            for _ in range(WAIT_ROUNDS):
                s._drain(0.5)
                if srv.seen:
                    break
    finally:
        srv.shutdown()
        srv.server_close()
    assert srv.seen, "the editor never reached the listener"
    return srv.seen[0]


def test_workspace_id_from_environment_is_sent(tmp_path):
    seen = _ask_once(tmp_path, {"ANTHROPIC_WORKSPACE_ID": "wrkspc_test_1"})
    assert seen.get(HEADER) == "wrkspc_test_1", seen
    assert seen.get("x-api-key") == "test-key-not-real", seen


def test_workspace_id_from_file_is_sent(tmp_path):
    idfile = tmp_path / "ws-id"
    idfile.write_text("wrkspc_test_2\n")
    seen = _ask_once(tmp_path, {"ANTHROPIC_WORKSPACE_ID_FILE": str(idfile)})
    assert seen.get(HEADER) == "wrkspc_test_2", seen


def test_no_workspace_id_sends_no_header(tmp_path):
    seen = _ask_once(tmp_path, {})
    assert HEADER not in seen, seen


def test_workspace_id_with_line_break_is_not_sent(tmp_path):
    # A pasted value that carries a line break must not end the header line
    # and add a header of its own to the request.
    seen = _ask_once(tmp_path,
                     {"ANTHROPIC_WORKSPACE_ID": "wrkspc_3\r\nX-Injected: yes"})
    assert "x-injected" not in seen, seen
    assert HEADER not in seen, seen
    assert seen.get("x-api-key") == "test-key-not-real", seen
