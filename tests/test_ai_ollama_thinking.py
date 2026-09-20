"""Ollama reasoning models: a "thinking-only" turn must still show an answer.

Reasoning models (qwen3, deepseek-r1) stream their chain of thought in a separate
"thinking" field and can finish a turn with "content" empty.  xwpe renders
"content", so such a turn used to appear as a blank reply -- the Agent showed only
"working -> done" with no answer.  The stream now keeps the thinking as a fallback
and hands it over when a turn produced no content, so the reply is never blank; a
turn that DID produce content is untouched (the thinking stays hidden).

Deterministic: a tiny fake Ollama server (pointed at via XWPE_AI_ENDPOINT) emits a
scripted NDJSON stream, so no real model/network is needed.

Red/green: before the fallback, the thinking-only case shows "(no answer)" and the
marker never appears.
"""
import json
import os
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

THINK_MARKER = "THINKONLYMARKER42"
CONTENT_MARKER = "CONTENTMARKER99"
HIDDEN_THINK = "SHOULDBEHIDDEN77"


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _ndjson(chunks):
    return "".join(json.dumps(c) + "\n" for c in chunks)


class _FakeOllama(threading.Thread):
    """Serve one scripted /api/chat NDJSON body, then a done=true line."""
    def __init__(self, body):
        super().__init__(daemon=True)
        self._body = body.encode()
        handler_body = self._body

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_POST(self):
                ln = int(self.headers.get("Content-Length", 0))
                if ln:
                    self.rfile.read(ln)
                self.send_response(200)
                self.send_header("Content-Type", "application/x-ndjson")
                self.send_header("Content-Length", str(len(handler_body)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(handler_body)

        self._srv = HTTPServer(("127.0.0.1", 0), H)

    @property
    def url(self):
        return "http://127.0.0.1:%d" % self._srv.server_address[1]

    def run(self):
        self._srv.serve_forever()

    def stop(self):
        self._srv.shutdown()
        self._srv.server_close()


def _ask(tmp_path, body):
    srv = _FakeOllama(body)
    srv.start()
    try:
        env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "ollama",
               "XWPE_AI_ENDPOINT": srv.url, "XWPE_AI_MODEL": "qwen3-fake"}
        with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                        env_extra=env) as s:
            s.key(ALT.AI)
            s.key("a")
            s.key("hi")
            s.key("\r", delay=1.2)
            s._drain(1.5)
            return "\n".join(s.display())
    finally:
        srv.stop()


def test_thinking_only_turn_shows_the_thinking(tmp_path):
    # every chunk carries thinking and an EMPTY content -> the fallback fires
    body = _ndjson([
        {"message": {"role": "assistant", "content": "", "thinking": "The " + THINK_MARKER},
         "done": False},
        {"message": {"role": "assistant", "content": "", "thinking": " continues"},
         "done": False},
        {"message": {"role": "assistant", "content": ""}, "done": True},
    ])
    disp = _ask(tmp_path, body)
    assert THINK_MARKER in disp, \
        "a thinking-only reply was shown blank (no content fallback):\n" + disp
    assert "gathering the answer" not in disp, "liveness placeholder stuck:\n" + disp


def test_content_turn_hides_the_thinking(tmp_path):
    # content present -> it is shown and the thinking stays hidden (good path)
    body = _ndjson([
        {"message": {"role": "assistant", "content": "", "thinking": HIDDEN_THINK},
         "done": False},
        {"message": {"role": "assistant", "content": CONTENT_MARKER}, "done": False},
        {"message": {"role": "assistant", "content": ""}, "done": True},
    ])
    disp = _ask(tmp_path, body)
    assert CONTENT_MARKER in disp, "the content reply was not rendered:\n" + disp
    assert HIDDEN_THINK not in disp, \
        "the thinking leaked into the answer even though content was present:\n" + disp
