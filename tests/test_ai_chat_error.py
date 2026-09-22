"""Chat surfaces a backend HTTP error instead of a blank "(no answer)".

When the OpenAI-compatible endpoint rejects the request -- most commonly a model
the key cannot access (HTTP 404 "model_not_found") -- the chat used to show only
"(no answer)", which told the user nothing.  It now shows the server's error
message so the fix (pick an accessible model) is obvious.
"""
import os
import json
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

import pytest
from wpe_driver import WpeSession, WPE_BIN


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"Alt-G AI" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _erroring_openai_server():
    """/v1/models lists one model; POST /v1/chat/completions returns 404 with the
    OpenAI-shaped error body a real server sends for an inaccessible model."""
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            if self.path.endswith("/models"):
                b = json.dumps({"object": "list",
                                "data": [{"id": "badmodel"}]}).encode()
                self.send_response(200)
                self.send_header("Content-Length", str(len(b)))
                self.end_headers()
                self.wfile.write(b)
            else:
                self.send_response(404)
                self.end_headers()

        def do_POST(self):
            self.rfile.read(int(self.headers.get("Content-Length", 0)))
            b = json.dumps({"error": {
                "message": "The model `badmodel` does not exist or you do not "
                           "have access to it.",
                "type": "invalid_request_error",
                "code": "model_not_found"}}).encode()
            self.send_response(404)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)

    srv = HTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]


def test_chat_shows_backend_error(tmp_path):
    import time
    srv, port = _erroring_openai_server()
    try:
        home = str(tmp_path / "home")
        cfg = os.path.join(home, ".config", "xwpe")
        os.makedirs(cfg)
        with open(os.path.join(cfg, "xwperc"), "w") as fh:
            fh.write("[Programming]\nAIBackend : 1\n"
                     "AIEndpoint : http://127.0.0.1:%d/v1\nAIModel : badmodel\n"
                     "AIPolicy : ask\n" % port)
        env = {"XWPE_AI_ENABLE": "1", "HOME": home, "OPENAI_API_KEY": "x"}
        with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                        env_extra=dict(env), filename="t.c") as s:
            s.key("\033g", delay=0.6); s.key("a", delay=0.8)   # Alt-G a: Ask
            for ch in "what model":
                s.key(ch, delay=0.02)
            s.key("\r", delay=0.5)
            time.sleep(4)
            disp = "\n".join(s.display())
    finally:
        srv.shutdown()
    assert "does not exist" in disp, \
        "the chat did not surface the backend error:\n" + disp
    assert "(no answer)" not in disp, \
        "the chat still showed a blank answer instead of the error:\n" + disp
