"""Model fallback: a failing primary model retries once on the fallback model.

When the primary model errors (unavailable, or too slow/timed out), chat retries
the same turn on the configured AIModelFallback instead of dead-ending, and says
so.  The server here fails the primary model with an HTTP error and serves the
fallback model normally; the answer must still arrive, on the fallback.
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


def _server(models_seen):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            b = json.dumps({"object": "list",
                            "data": [{"id": "primary"}, {"id": "backup"}]}).encode()
            self.send_response(200)
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)

        def do_POST(self):
            n = int(self.headers.get("Content-Length", 0))
            raw = self.rfile.read(n).decode("utf-8", "replace")
            model = json.loads(raw).get("model")
            models_seen.append(model)
            if model == "primary":                       # primary is "down"
                b = json.dumps({"error": {"message": "model overloaded"}}).encode()
                self.send_response(503)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(b)))
                self.end_headers()
                self.wfile.write(b)
                return
            body = (b'data: {"choices":[{"delta":{"content":"FALLBACKWORKED"}}]}\n\n'
                    b'data: [DONE]\n\n')
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            self.wfile.write(body)

    srv = HTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]


def test_primary_failure_falls_back(tmp_path):
    seen = []
    srv, port = _server(seen)
    try:
        home = str(tmp_path / "home")
        cfg = os.path.join(home, ".config", "xwpe")
        os.makedirs(cfg)
        with open(os.path.join(cfg, "xwperc"), "w") as fh:
            fh.write("[Programming]\nAIBackend : 1\n"
                     "AIEndpoint : http://127.0.0.1:%d/v1\n"
                     "AIModel : primary\nAIModelFallback : backup\n"
                     "AIPolicy : ask\n" % port)
        env = {"XWPE_AI_ENABLE": "1", "HOME": home, "OPENAI_API_KEY": "x"}
        with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                        env_extra=dict(env), filename="t.c") as s:
            s.key(ALT.AI, delay=0.6); s.key("a", delay=0.6)
            s.key("hello", delay=0.1)
            s.key("\r", delay=1.0)
            time.sleep(4)
            disp = "\n".join(s.display())
    finally:
        srv.shutdown()
    assert seen[:2] == ["primary", "backup"], \
        "expected primary then backup, saw: %r" % seen
    assert "FALLBACKWORKED" in disp, \
        "the fallback model's answer did not arrive:\n" + disp
    assert "retrying with backup" in disp, \
        "the fallback was not announced:\n" + disp
