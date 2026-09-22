"""An empty tool result is fed back as an explicit note, not a blank.

When grep finds nothing (or a directory is empty, or a command prints nothing),
the model used to receive a blank TOOL RESULT that reads like a broken tool -- so
it could stall or repeat.  It now gets an explicit "nothing matched" note so it
adjusts.  A scripted OpenAI-compatible server asks for a grep with no match on
turn 1; the request xwpe sends on turn 2 must carry the note.
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
    turn = {"n": 0}

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
            first = turn["n"] == 0
            turn["n"] += 1
            content = "TOOL grep ZZZNOMATCH99XYZ" if first else "not present: done"
            body = (('data: {"choices":[{"delta":{"content":%s}}]}\n\n'
                     % json.dumps(content)).encode()
                    + b'data: [DONE]\n\n')
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            self.wfile.write(body)

    srv = HTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]


def test_empty_grep_feeds_back_a_note(tmp_path):
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
            s.key("does ZZZNOMATCH99XYZ exist", delay=0.1)
            s.key("\r", delay=1.0)
            time.sleep(4)
    finally:
        srv.shutdown()
    assert len(captured) >= 2, \
        "the grep tool call did not trigger a follow-up turn:\n" + "\n".join(captured)
    second = captured[1]
    assert "nothing matched" in second and "TOOL RESULT" in second, \
        "the empty grep result was not fed back as a note:\n" + second[:1500]
