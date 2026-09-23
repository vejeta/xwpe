"""The built-in agent can search the web with the web_search tool.

web_search builds a query URL from a configurable template (AISearchURL /
XWPE_AI_SEARCH_URL, "%s" = the URL-encoded query) and fetches it as text, so any
built-in-agent backend can find pages, not only fetch known URLs.

Driven with the mock backend emitting `TOOL web_search <query>` and a template
pointing at a local server that records the encoded query it receives.
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
    paths = []

    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            paths.append(self.path)
            body = b"<html><body><a href='http://x'>result one</a></body></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    srv = HTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, paths


def test_agent_web_search_encodes_and_fetches(tmp_path):
    srv, paths = _server()
    port = srv.server_address[1]
    template = "http://127.0.0.1:%d/find?q=%%s" % port   # %%s -> literal %s
    try:
        env = {
            "XWPE_AI_ENABLE": "1",
            "XWPE_AI_BACKEND": "mock",
            "XWPE_AI_POLICY": "auto",
            "XWPE_AI_SEARCH_URL": template,
            "XWPE_AI_MOCK_REPLY": "TOOL web_search hello world@@TURN@@Done.",
        }
        with WpeSession(tempfile.mkdtemp(), "int main(void){return 0;}\n",
                        env_extra=env, filename="a.c") as s:
            s.key(ALT.AI, delay=0.5); s.key("g", delay=0.7)
            s.key("search please", delay=0.4)
            s.key("\033s", delay=2.0)                       # Alt-S = Send
            waited = 0.0
            while waited < 20 and not paths:
                s._drain(1.0)
                waited += 1.0
    finally:
        srv.shutdown()
        srv.server_close()

    assert paths, "web_search never issued a query to the search server"
    # The query must be substituted into the template and URL-encoded.
    assert "/find?q=hello%20world" in paths[0], "bad search path: %r" % paths
