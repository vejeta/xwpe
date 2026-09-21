"""Zero-config: auto-select a CODE model, not just the first one listed.

When no model is chosen, a fresh Ollama setup should answer coding questions
well.  Picking the first model the server happens to return can land on a small
general chat model that rambles; auto-selection must prefer a code-tuned model
(qwen2.5-coder, deepseek-coder, ...) when one is installed.

Deterministic: a fake Ollama serves /api/tags (a general model listed FIRST, a
coder model after) and captures the /api/chat request so we can read which model
xwpe actually used.

Red/green: before the fix, the first-listed general model is used and the coder
is ignored.
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


class _FakeOllama(threading.Thread):
    """Serve /api/tags with the given model names; capture /api/chat bodies."""
    def __init__(self, model_names):
        super().__init__(daemon=True)
        self.chat_bodies = []
        bodies = self.chat_bodies
        tags = (json.dumps({"models": [{"name": m} for m in model_names]})).encode()
        reply = (json.dumps({"message": {"role": "assistant", "content": "ok"},
                             "done": True}) + "\n").encode()

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(tags)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(tags)

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


def _autopick(tmp_path, model_names):
    srv = _FakeOllama(model_names)
    srv.start()
    try:
        env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "ollama",
               "XWPE_AI_ENDPOINT": srv.url, "XWPE_AI_MODEL": ""}  # unset -> auto
        with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                        env_extra=env, filename="a.c") as s:
            s.key(ALT.AI); s.key("a")
            s.key("hi"); s.key("\r", delay=1.2)
            s._drain(1.2)
        return "\n".join(srv.chat_bodies)
    finally:
        srv.stop()


def test_autopick_prefers_a_code_model(tmp_path):
    # a code model wins over a general one even when the general one is listed
    # first and is larger -- code-tuning matters more for an editor.
    body = _autopick(tmp_path, ["llama3:70b", "qwen2.5-coder:7b", "gemma:2b"])
    assert '"qwen2.5-coder:7b"' in body, \
        "auto-select did not prefer the code model:\n" + body[:600]
    assert '"llama3:70b"' not in body, \
        "auto-select used a general model instead of the coder:\n" + body[:600]


def test_autopick_prefers_the_stronger_code_model(tmp_path):
    # among code models, the larger (stronger) one the user installed wins.
    body = _autopick(tmp_path, ["qwen2.5-coder:7b", "deepseek-coder:33b"])
    assert '"deepseek-coder:33b"' in body, \
        "auto-select did not take the larger code model:\n" + body[:600]
    assert '"qwen2.5-coder:7b"' not in body, \
        "auto-select took the smaller code model:\n" + body[:600]
