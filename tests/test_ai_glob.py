"""The chat/agent read tools include `glob`, which finds files by name pattern.

A capturing OpenAI-compatible server answers the first request with
`TOOL glob *.c` and the second with a plain answer; the glob output xwpe feeds
back on the second request must list the project's .c files, proving the tool ran
(rounding out read_file/grep/list_dir/glob, the Read/Grep/Glob triad).
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
            content = "TOOL glob *.c" if first else "found it: done"
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


def test_glob_tool_runs(tmp_path):
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
        # a second .c file so the glob result is unambiguous
        with open(os.path.join(str(tmp_path), "extra_unit.c"), "w") as fh:
            fh.write("int u(void){return 1;}\n")
        env = {"XWPE_AI_ENABLE": "1", "HOME": home, "OPENAI_API_KEY": "x"}
        with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                        env_extra=dict(env), filename="t.c") as s:
            s.key(ALT.AI, delay=0.6); s.key("a", delay=0.6)
            s.key("which c files exist", delay=0.1)
            s.key("\r", delay=1.0)
            time.sleep(4)
    finally:
        srv.shutdown()
    assert len(captured) >= 2, \
        "the glob tool call did not trigger a follow-up turn:\n" + "\n".join(captured)
    second = captured[1]
    assert "TOOL RESULT" in second and "extra_unit.c" in second, \
        "the glob result (the .c files) was not fed back to the model:\n" + second[:1500]
