"""Every AI mode is handed the editor's open files as context.

The user should not have to tell the assistant which files xwpe has open (e.g. by
asking it to inspect running processes): the request must already carry a block
naming the open editor windows, with the focused one marked.  A fake Ollama
server captures the outgoing request body and we assert the block is present.
"""
import json
import os
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


class _CapturingOllama(threading.Thread):
    """Record every /api/chat request body; reply with a trivial NDJSON turn."""
    def __init__(self):
        super().__init__(daemon=True)
        self.bodies = []
        bodies = self.bodies
        reply = (json.dumps({"message": {"role": "assistant", "content": "ok"},
                             "done": True}) + "\n").encode()

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_POST(self):
                ln = int(self.headers.get("Content-Length", 0))
                bodies.append(self.rfile.read(ln).decode("utf-8", "replace") if ln else "")
                self.send_response(200)
                self.send_header("Content-Type", "application/x-ndjson")
                self.send_header("Content-Length", str(len(reply)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(reply)

        self._srv = HTTPServer(("127.0.0.1", 0), H)

    @property
    def url(self):
        return "http://127.0.0.1:%d" % self._srv.server_address[1]

    def run(self):
        self._srv.serve_forever()

    def stop(self):
        self._srv.shutdown()
        self._srv.server_close()


def _run(tmp_path, keys):
    srv = _CapturingOllama()
    srv.start()
    try:
        env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "ollama",
               "XWPE_AI_ENDPOINT": srv.url, "XWPE_AI_MODEL": "m"}
        with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                        env_extra=env, filename="widget.c") as s:
            for k, d in keys:
                s.key(k, delay=d)
            s._drain(1.2)
        return "\n".join(srv.bodies)
    finally:
        srv.stop()


def test_chat_sends_open_files(tmp_path):
    body = _run(tmp_path, [(ALT.AI, 0.4), ("a", 0.4), ("hi", 0.3), ("\r", 1.2)])
    assert "FILES OPEN IN THE EDITOR" in body, \
        "chat did not send the open-files context:\n" + body[:2000]
    assert "widget.c" in body, "the open file was not named:\n" + body[:2000]
    assert "focused" in body, "the focused file was not marked:\n" + body[:2000]


def test_agent_sends_open_files(tmp_path):
    body = _run(tmp_path, [(ALT.AI, 0.4), ("g", 0.4),
                           ("what is open?", 0.3), ("\r", 1.5)])
    assert "FILES OPEN IN THE EDITOR" in body, \
        "the agent did not send the open-files context:\n" + body[:2000]
    assert "widget.c" in body, "the open file was not named:\n" + body[:2000]
