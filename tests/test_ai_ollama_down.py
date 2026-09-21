"""First-run friction: when Ollama is not usable, say exactly what to do.

Two distinct cases must each surface an actionable message in the pane rather than
a silent failure or a cryptic error:
  - Ollama not running  -> "try `ollama serve`"
  - Ollama up, no models -> "ollama pull qwen2.5-coder"
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


def test_ollama_not_running_message(tmp_path):
    # point at a port nothing listens on -> the preflight must explain it
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "ollama",
           "XWPE_AI_ENDPOINT": "http://127.0.0.1:1", "XWPE_AI_MODEL": "m"}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, filename="a.c") as s:
        s.key(ALT.AI); s.key("a")
        s.key("hi"); s.key("\r", delay=1.0)
        s._drain(1.0)
        disp = "\n".join(s.display())
    assert "ollama serve" in disp, \
        "no actionable 'is Ollama running?' message:\n" + disp


class _EmptyOllama(threading.Thread):
    """Reachable Ollama with no models installed."""
    def __init__(self):
        super().__init__(daemon=True)
        tags = json.dumps({"models": []}).encode()

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

        self._srv = HTTPServer(("127.0.0.1", 0), H)

    @property
    def url(self):
        return "http://127.0.0.1:%d" % self._srv.server_address[1]

    def run(self):
        self._srv.serve_forever()

    def stop(self):
        self._srv.shutdown()
        self._srv.server_close()


def test_ollama_no_models_message(tmp_path):
    srv = _EmptyOllama()
    srv.start()
    try:
        env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "ollama",
               "XWPE_AI_ENDPOINT": srv.url, "XWPE_AI_MODEL": ""}  # unset -> must resolve
        with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                        env_extra=env, filename="a.c") as s:
            s.key(ALT.AI); s.key("a")
            s.key("hi"); s.key("\r", delay=1.0)
            s._drain(1.0)
            disp = "\n".join(s.display())
    finally:
        srv.stop()
    assert "ollama pull" in disp, \
        "no actionable 'install a model' message when the server has none:\n" + disp
