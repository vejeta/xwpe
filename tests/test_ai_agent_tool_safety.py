"""Read tools take their argument as one shell word; a write beyond the tree asks.

list_dir, grep and glob run without asking, in the agent loop and in the chat
loop alike, so the text the model writes after the tool name must reach the
shell as exactly one word: ``TOOL list_dir . ; touch x`` is a listing of an
odd name and never a touch, and ``TOOL grep two words`` searches for the
two-word phrase instead of "two" in a file called "words".  A page fetched
with web_fetch can plant such a line in the model's context, which is why the
dial does not matter here.

write_file and apply_patch to a path outside the workspace (absolute beyond
the working tree, or climbing out with ``..``) show the write box carrying the
outside-the-workspace mark whatever the dial, so an unattended run cannot
reach past the project silently; denied, nothing is written.
"""
import json
import os
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

OUTSIDE_WORD = "outside the workspace"
OUTSIDE_MARK = "!"


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"autonomous coding agent" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _env(tmp_path, reply, policy):
    return {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_POLICY": policy,
        "XWPE_AI_TRACE": str(tmp_path / "ai.trace"),
    }


def _trace(tmp_path):
    trace = tmp_path / "ai.trace"
    return trace.read_text() if trace.exists() else ""


def _wait_for(s, needle, timeout=15):
    """Drain the screen until `needle` shows or `timeout` seconds pass."""
    end = time.time() + timeout
    while time.time() < end:
        s._drain(0.5)
        if needle in "\n".join(s.display()):
            return True
    return False


def _injection_arg(marker):
    """The argument shape a planted line would use: a valid value, then a
    second command, then a comment so whatever the tool appends after the
    argument cannot stop the touch from running when the shell sees two
    commands."""
    return ". ; touch %s #" % marker


def _agent_tool_injection(tmp_path, tool):
    """Run `TOOL <tool> <injection>` under auto and return (marker, trace)."""
    marker = tmp_path / ("marker-%s" % tool)
    arg = _injection_arg(marker)
    reply = "TOOL %s %s@@TURN@@DONE looked" % (tool, arg)
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=_env(tmp_path, reply, "auto")) as s:
        s.key(ALT.AI); s.key("g")
        s.key("inspect"); s.key("\r", delay=1.6)
        s._drain(2.0)
    return marker, arg, _trace(tmp_path)


def _assert_one_word(tool, marker, arg, txt):
    assert "agent tool=%s arg=%s" % (tool, arg) in txt, \
        "the trace does not carry the whole text as the one argument:\n" + txt
    assert not marker.exists(), \
        "%s ran the text after ';' as a second command (marker created)" % tool
    assert "agent done" in txt, txt


# @scenario SC-ai-agent-12
def test_agent_list_dir_argument_is_one_word(tmp_path):
    marker, arg, txt = _agent_tool_injection(tmp_path, "list_dir")
    _assert_one_word("list_dir", marker, arg, txt)


# @scenario SC-ai-agent-12
def test_agent_grep_argument_is_one_word(tmp_path):
    marker, arg, txt = _agent_tool_injection(tmp_path, "grep")
    _assert_one_word("grep", marker, arg, txt)


# @scenario SC-ai-agent-12
def test_agent_glob_argument_is_one_word(tmp_path):
    marker, arg, txt = _agent_tool_injection(tmp_path, "glob")
    _assert_one_word("glob", marker, arg, txt)


# @scenario SC-ai-agent-12
def test_chat_list_dir_argument_is_one_word(tmp_path):
    # The chat loop (Alt-G a) runs the same read tools with no dial at all, so
    # it is the easier target for a planted line.
    marker = tmp_path / "marker-chat"
    arg = _injection_arg(marker)
    reply = "TOOL list_dir %s@@TURN@@CHATANSWER77 nothing odd there" % arg
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=_env(tmp_path, reply, "ask")) as s:
        s.key(ALT.AI); s.key("a", delay=0.6)
        s.key("what is in this directory", delay=0.1)
        s.key("\r", delay=1.0)
        s._drain(3.0)
        disp = "\n".join(s.display())
    assert "CHATANSWER77" in disp, \
        "the chat did not run the tool turn and reach the answer:\n" + disp
    assert not marker.exists(), \
        "chat list_dir ran the text after ';' as a second command (marker created)"


def _agent_server(captured, first_reply):
    """A capturing OpenAI-compatible server: turn 1 answers `first_reply`,
    every later turn answers DONE.  What the editor fed back as the TOOL
    RESULT is read from the second request's body."""
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
            content = first_reply if first else "DONE found it"
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


def _seed_openai_xwperc(tmp_path, port, policy):
    home = str(tmp_path / "home")
    cfg = os.path.join(home, ".config", "xwpe")
    os.makedirs(cfg)
    with open(os.path.join(cfg, "xwperc"), "w") as fh:
        fh.write("[Programming]\nAIBackend : 1\n"
                 "AIEndpoint : http://127.0.0.1:%d/v1\nAIModel : m\n"
                 "AIPolicy : %s\n" % (port, policy))
    return home


# @scenario SC-ai-agent-13
def test_agent_grep_pattern_with_a_space_is_one_pattern(tmp_path):
    # The mock backend cannot show what was fed back, so a capturing server
    # stands in for the model: its second request carries the TOOL RESULT.
    (tmp_path / "phrase.txt").write_text("alpha two words beta\n")
    (tmp_path / "words").write_text("two\n")
    captured = []
    srv, port = _agent_server(captured, "TOOL grep two words")
    try:
        home = _seed_openai_xwperc(tmp_path, port, "auto")
        env = {"XWPE_AI_ENABLE": "1", "HOME": home, "OPENAI_API_KEY": "x",
               "XWPE_AI_TRACE": str(tmp_path / "ai.trace")}
        with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                        env_extra=env, filename="t.c") as s:
            s.key(ALT.AI, delay=0.6); s.key("g", delay=0.6)
            s.key("where is the phrase", delay=0.1)
            s.key("\r", delay=1.0)
            end = time.time() + 15
            while time.time() < end and len(captured) < 2:
                s._drain(0.5)
    finally:
        srv.shutdown()
    txt = _trace(tmp_path)
    assert "agent tool=grep arg=two words" in txt, txt
    assert len(captured) >= 2, \
        "the grep tool call did not trigger a follow-up turn:\n" + "\n".join(captured)
    second = captured[1]
    assert "TOOL RESULT" in second, second[:1500]
    assert "phrase.txt:1:alpha two words beta" in second, \
        "the two-word phrase was not searched as one pattern:\n" + second[:1500]
    assert "words:1:two" not in second, \
        "the pattern was split: 'two' was searched in the file 'words':\n" + second[:1500]


def _outside_path(tmp_path):
    """A path that climbs out of the workspace with `..`, named per process so
    two runs never collide; the test removes it."""
    name = "outside-%d.txt" % os.getpid()
    return str(tmp_path) + "/../" + name, tmp_path.parent / name


def _agent_outside_write(tmp_path, answer):
    """Under auto, ask the agent to write beyond the workspace and answer the
    write box with `answer`.  Returns (box screen, screen after, trace)."""
    rel, real = _outside_path(tmp_path)
    reply = "TOOL write_file %s\nbeyond the tree\n@@END@@TURN@@DONE wrote it" % rel
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=_env(tmp_path, reply, "auto")) as s:
        s.key(ALT.AI); s.key("g")
        s.key("write outside"); s.key("\r", delay=1.0)
        _wait_for(s, " Write ")
        box = "\n".join(s.display())
        s.key(answer, delay=0.8); s._drain(1.5)
        after = "\n".join(s.display())
    return rel, real, box, after, _trace(tmp_path)


# @scenario SC-ai-agent-32
def test_agent_write_outside_workspace_asks_under_auto_and_deny(tmp_path):
    rel, real, box, after, txt = _agent_outside_write(tmp_path, "n")
    try:
        assert " Write " in box, \
            "no write box under auto for a path outside the workspace:\n" + box
        assert OUTSIDE_WORD in box, "the write box carries no outside mark:\n" + box
        assert OUTSIDE_MARK + " " in box, "the write box carries no gutter mark:\n" + box
        assert not real.exists(), "a denied write outside the workspace still landed"
        assert "agent write confirm %s -> deny" % rel in txt, txt
        assert "agent done" in txt, txt
    finally:
        if real.exists():
            real.unlink()


# @scenario SC-ai-agent-32
def test_agent_write_outside_workspace_asks_under_auto_and_approve(tmp_path):
    rel, real, box, after, txt = _agent_outside_write(tmp_path, "y")
    try:
        assert " Write " in box, \
            "no write box under auto for a path outside the workspace:\n" + box
        assert real.exists(), "an approved write outside the workspace did not land"
        assert "beyond the tree" in real.read_text()
        assert "agent write confirm %s -> allow" % rel in txt, txt
        assert "agent wrote outside workspace %s" % rel in txt, txt
        assert OUTSIDE_WORD in after, \
            "the pane does not report the file as outside the workspace:\n" + after
    finally:
        if real.exists():
            real.unlink()
