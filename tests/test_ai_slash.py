"""Chat slash commands: a leading /command expands to a full instruction.

Typing /explain, /fix, /test, /doc or /review sends the model a templated
instruction scoped to the current file, so a common ask is one token.  The
transcript still shows what the user typed; only the model receives the
expansion.  A capturing OpenAI-compatible server proves the expansion is sent.
"""
import os
import json
import time
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _server(captured):
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
            body = (b'data: {"choices":[{"delta":{"content":"ok"}}]}\n\n'
                    b'data: [DONE]\n\n')
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            self.wfile.write(body)

    srv = HTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]


def test_slash_explain_expands(tmp_path):
    captured = []
    srv, port = _server(captured)
    try:
        home = str(tmp_path / "home")
        cfg = os.path.join(home, ".config", "xwpe")
        os.makedirs(cfg)
        with open(os.path.join(cfg, "xwperc"), "w") as fh:
            fh.write("[Programming]\nAIBackend : 1\n"
                     "AIEndpoint : http://127.0.0.1:%d/v1\nAIModel : m\n"
                     "AIPolicy : ask\n" % port)
        env = {"XWPE_AI_ENABLE": "1", "HOME": home, "OPENAI_API_KEY": "x"}
        with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                        env_extra=dict(env), filename="t.c") as s:
            s.key(ALT.AI, delay=0.6); s.key("a", delay=0.6)
            s.key("/explain", delay=0.1)
            s.key("\r", delay=1.0)
            time.sleep(3)
            disp = "\n".join(s.display())
    finally:
        srv.shutdown()
    assert captured, "no request was sent"
    body = "\n".join(captured)
    # the model receives the expansion, not the bare slash command
    assert "Explain what the current file" in body, \
        "/explain was not expanded before sending:\n" + body[:1500]
    # the transcript still shows what the user typed
    assert "/explain" in disp, "the typed command was not echoed:\n" + disp
