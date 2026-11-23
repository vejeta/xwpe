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
from wpe_driver import WpeSession, ALT, WPE_BIN, pane_text_with_history

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


def _ask_capture(tmp_path, write_agents, scope_all=False, plant=None, pane_needles=None):
    """One plain chat turn against the capturing server; `scope_all` seeds
    AIProjectInstructions : all, the scope under which a plain turn folds the
    file in; `plant(folder)` puts anything else at the names the editor reads
    (a pipe, a link) before it starts.  With `pane_needles` (the phrases the
    row asserts on the pane) the result is (request body, the pane's text)."""
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
        if plant:
            plant(tmp_path)
        env = {"XWPE_AI_ENABLE": "1", "HOME": home, "OPENAI_API_KEY": "x"}
        with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                        env_extra=dict(env), filename="t.c") as s:
            s.key(ALT.AI, delay=0.6); s.key("a", delay=0.6)
            s.key("hi", delay=0.1)
            s.key("\r", delay=1.0)
            time.sleep(3)
            pane = ""
            if pane_needles:
                pane = "\n".join(pane_text_with_history(s, pane_needles))
    finally:
        srv.shutdown()
    if pane_needles:
        return "\n".join(captured), pane
    return "\n".join(captured)


def _ask_capture_pane(tmp_path, plant, needles):
    """A chat turn over a folder `plant` has prepared, with the pane read for
    `needles`: (the request body, the pane's text)."""
    return _ask_capture(tmp_path, write_agents=False, scope_all=True, plant=plant,
                        pane_needles=needles)


def _link(folder, target):
    """AGENTS.md in `folder` as a link to `target`."""
    os.symlink(str(target), os.path.join(str(folder), "AGENTS.md"))


# @scenario SC-ai-runtime-14
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


# @scenario SC-read-owner-25
def test_agents_md_that_is_a_pipe_does_not_stop_the_editor(tmp_path):
    """A cloned folder with a pipe called AGENTS.md.  The editor used to open
    it and wait for a writer for ever, so the turn was never sent; now the pipe
    is refused by what it is, the turn goes out and carries no instructions."""
    body = _ask_capture(tmp_path, write_agents=False, scope_all=True,
                        plant=lambda d: os.mkfifo(os.path.join(str(d), "AGENTS.md")))
    assert body, "xwpe sent no request: it is waiting on the pipe"
    assert "PROJECT INSTRUCTIONS" not in body, \
        "instructions were folded in from a pipe:\n" + body[:2000]


# @scenario SC-read-owner-26
def test_a_pipe_as_the_editors_own_file_leaves_agents_md_in_use(tmp_path):
    """A pipe called .xwpe-ai.md, the file looked for first, beside a regular
    AGENTS.md: the pipe is refused and the next file of the list is read."""
    body = _ask_capture(tmp_path, write_agents=True, scope_all=True,
                        plant=lambda d: os.mkfifo(os.path.join(str(d), ".xwpe-ai.md")))
    assert body, "xwpe sent no request: it is waiting on the pipe"
    assert MARK in body, \
        "AGENTS.md was not read after the pipe was refused:\n" + body[:2000]


# @scenario SC-read-owner-27
def test_agents_md_linked_out_of_the_workspace_is_refused_and_named(tmp_path):
    """AGENTS.md is a link to a file of the user's outside the folder.  A cloned
    folder can point it anywhere, so it is not followed: the turn carries no
    instructions from it and the pane names the file and says why."""
    out = tmp_path.parent / (tmp_path.name + ".out.md")
    out.write_text(MARK + "\n")
    body, pane = _ask_capture_pane(tmp_path, lambda d: _link(d, out),
                                   ["AGENTS.md", "outside the workspace"])
    assert body and MARK not in body, "the link was followed into the prompt:\n" + body[:2000]
    assert "AGENTS.md" in pane and "outside the workspace" in pane, pane


# @scenario SC-read-owner-29
def test_agents_md_linked_inside_the_workspace_is_read(tmp_path):
    """AGENTS.md kept as a link to another file of the project is read."""
    def plant(d):
        (d / "rules.txt").write_text(MARK + "\n")
        _link(d, d / "rules.txt")

    body, pane = _ask_capture_pane(tmp_path, plant, ["AGENTS.md"])
    assert MARK in body, "the link inside the workspace was not read:\n" + body[:2000]


# @scenario SC-read-owner-30
def test_agents_md_linked_to_a_pipe_inside_is_refused_and_named(tmp_path):
    """AGENTS.md is a link to a pipe inside the folder: refused by its kind,
    never waited for, and named in the pane."""
    def plant(d):
        os.mkfifo(str(d / "pipe.md"))
        _link(d, d / "pipe.md")

    body, pane = _ask_capture_pane(tmp_path, plant, ["AGENTS.md", "it is a pipe"])
    assert body, "the turn never went out: the editor waits on the pipe"
    assert "AGENTS.md" in pane and "it is a pipe" in pane, pane


# @scenario SC-read-owner-36
def test_agents_md_that_is_a_dangling_link_is_named(tmp_path):
    """AGENTS.md is a link to a file that is not there.  It is not dropped in
    silence: the pane names the file and says it is a link to a missing file."""
    body, pane = _ask_capture_pane(tmp_path, lambda d: _link(d, d / "gone.md"),
                                   ["AGENTS.md", "a link to a missing file"])
    assert body, "xwpe sent no request"
    assert "PROJECT INSTRUCTIONS" not in body, body[:2000]
    assert "AGENTS.md" in pane and "a link to a missing file" in pane, pane


# @scenario SC-read-owner-31
def test_a_chain_of_two_links_that_leaves_the_workspace_is_refused(tmp_path):
    """AGENTS.md -> hop.md -> a file outside: the whole chain is followed and
    the place it ends at is outside, so it is refused and named."""
    out = tmp_path.parent / (tmp_path.name + ".out.md")
    out.write_text(MARK + "\n")

    def plant(d):
        os.symlink(str(out), str(d / "hop.md"))
        _link(d, d / "hop.md")

    body, pane = _ask_capture_pane(tmp_path, plant, ["AGENTS.md", "outside the workspace"])
    assert body and MARK not in body, body[:2000]
    assert "outside the workspace" in pane, pane
