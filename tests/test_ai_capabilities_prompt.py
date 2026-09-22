"""The chat system prompt carries the xwpe capability manifest.

So the model stops re-proposing already-built features (Edit, Agent, streaming,
diff review, ...) and points the user to the shortcut instead, the chat system
prompt lists what the xwpe AI integration already does.  This stands up a local
OpenAI-compatible server, captures the request xwpe sends, and asserts the
manifest and the mode shortcuts are actually in the system message.
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


def _capturing_server(captured):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            b = json.dumps({"object": "list",
                            "data": [{"id": "m"}]}).encode()
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


def test_manifest_in_system_prompt(tmp_path):
    captured = []
    srv, port = _capturing_server(captured)
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
            s.key("hi", delay=0.1)
            s.key("\r", delay=1.0)
            time.sleep(3)
    finally:
        srv.shutdown()

    assert captured, "xwpe sent no request to the endpoint"
    body = "\n".join(captured)
    assert "XWPE AI CAPABILITIES" in body, \
        "the capability manifest was not in the system prompt:\n" + body[:2000]
    # the mode shortcuts the model should point users to
    for needle in ("Alt-G e", "Alt-G g", "Alt-G b"):
        assert needle in body, "manifest missing shortcut %r" % needle
