"""Project instructions (AGENTS.md) are injected into the AI system prompt.

Following the cross-tool per-repo agent-instructions convention, xwpe reads
.xwpe-ai.md (or AGENTS.md) from the working directory and folds it into the
prompt of the modes that act on the repository, so the model follows the
repo's standing instructions without the user restating them.  A plain chat
turn carries the file only under AIProjectInstructions all (the default,
agent, waits for the chat's first tool turn; tests/test_ai_agent_project_instructions.py
covers that), so the positive case here seeds that key in xwperc.  This stands
up a local OpenAI-compatible server, captures the request, and asserts the
file's content reached the system message -- and that with no such file,
nothing is injected.
"""
import os
import json
import time
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

MARK = "PROJECTRULE_XYZZY use four-space indent and never touch the vendor dir"


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
            b = json.dumps({"object": "list", "data": [{"id": "m"}]}).encode()
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


def _ask_capture(tmp_path, write_agents, scope_all=False):
    """One plain chat turn against the capturing server; `scope_all` seeds
    AIProjectInstructions : all, the scope under which a plain turn folds the
    file in."""
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
            if scope_all:
                fh.write("AIProjectInstructions : all\n")
        if write_agents:
            with open(os.path.join(str(tmp_path), "AGENTS.md"), "w") as fh:
                fh.write(MARK + "\n")
        env = {"XWPE_AI_ENABLE": "1", "HOME": home, "OPENAI_API_KEY": "x"}
        with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                        env_extra=dict(env), filename="t.c") as s:
            s.key(ALT.AI, delay=0.6); s.key("a", delay=0.6)
            s.key("hi", delay=0.1)
            s.key("\r", delay=1.0)
            time.sleep(3)
    finally:
        srv.shutdown()
    return "\n".join(captured)


def test_agents_md_injected(tmp_path):
    """With AIProjectInstructions : all seeded in xwperc, a plain chat turn
    carries the AGENTS.md block (under the default scope it would not)."""
    body = _ask_capture(tmp_path, write_agents=True, scope_all=True)
    assert body, "xwpe sent no request"
    assert MARK in body, \
        "AGENTS.md instructions were not injected into the prompt:\n" + body[:2000]
    assert "PROJECT INSTRUCTIONS" in body


def test_no_agents_md_no_block(tmp_path):
    body = _ask_capture(tmp_path, write_agents=False)
    assert body, "xwpe sent no request"
    assert "PROJECT INSTRUCTIONS" not in body, \
        "a project-instructions block appeared with no AGENTS.md present"
