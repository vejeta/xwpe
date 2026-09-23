"""The built-in agent can read a web page with the web_fetch tool.

Models reached over the plain OpenAI protocol (Ollama, a Groq/Lumo endpoint)
have no web tool of their own, so they reported "no internet". The agent now
exposes web_fetch, which GETs a URL over the built-in TLS HTTP client and feeds
the (de-marked-up) page back to the model.

Driven with the mock backend emitting `TOOL web_fetch <url>` against a local
server that records the hit, with the permission dial on Auto so the tool is
not held at an approval prompt.
"""
import os
import subprocess
import tempfile
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


def _server():
    hits = []

    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            hits.append(self.path)
            body = (b"<html><body><h1>MARKER_ABC</h1>"
                    b"<script>var x = 1 < 2;</script>"
                    b"<p>hello&nbsp;world</p></body></html>")
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    srv = HTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, hits


def test_agent_web_fetch_hits_server(tmp_path):
    srv, hits = _server()
    port = srv.server_address[1]
    url = "http://127.0.0.1:%d/" % port
    try:
        env = {
            "XWPE_AI_ENABLE": "1",
            "XWPE_AI_BACKEND": "mock",
            "XWPE_AI_POLICY": "auto",     # auto-approve the tool (no prompt)
            "XWPE_AI_MOCK_REPLY": "TOOL web_fetch " + url + "@@TURN@@Done.",
        }
        with WpeSession(tempfile.mkdtemp(), "int main(void){return 0;}\n",
                        env_extra=env, filename="a.c") as s:
            s.key(ALT.AI, delay=0.5); s.key("g", delay=0.7)   # Agent -> prompt popup
            s.key("read the page", delay=0.4)
            s.key("\033s", delay=2.0)                          # Alt-S = Send -> agent runs
            waited = 0.0
            while waited < 20 and not hits:
                s._drain(1.0)
                waited += 1.0
    finally:
        srv.shutdown()
        srv.server_close()

    assert hits, "web_fetch never issued a GET to the server"
    assert hits[0] == "/", "unexpected fetched path: %r" % hits
