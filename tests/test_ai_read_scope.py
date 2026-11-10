"""One workspace scope for every tool the assistant has: reads ask beyond it.

The assistant's reach is the workspace.  A path outside it, for a read as for
a write, is judged by one function (links followed), and the permission dial
decides only how much is asked about work inside the scope, never how wide the
scope is:

* read_file, list_dir, grep and glob run unasked inside the workspace, below
  the project's include directories, below the roots written in xwperc
  (AIReadRoots) and below folders the user opened in this session;
* anywhere else they ask with a box, at every dial, auto included: "y" lets
  that read through, "f" lets it through and opens the folder for the session
  (the Alt-G header shows it and the menu withdraws it), "n" gives the model an
  error that names the folder;
* a search never follows a link that leads out of the folder it started in,
  and its result says how many it left alone;
* the hosted engine's tools go through the same judge: a Read outside asks, a
  Write outside shows the marked box, whatever the dial, and the CLI is no
  longer launched with the permission bypass.

What the model reads is taken from a capturing OpenAI-compatible server on
127.0.0.1, so the rows assert on the TOOL RESULT message the editor really
sent.  Every home is the test's own workdir: nothing here reads the real one.
"""
import json
import os
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

SEED = "int main(void){return 0;}\n"
SECRET = "SECRET-IN-OUT-7141"
BOX_TITLE = "Allow this read?"
BOX_MARK = "Read outside the workspace"
DENIED_RESULT = "the user did not allow reading"
SKIPPED_NOTE = "leading outside the folder searched"
DIALS = ["ask", "edits", "auto"]
RUN_POLL_S = 25


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"autonomous coding agent" in out
    except Exception:
        return False


def _host_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"AIHostAdapter text needs AIHostCommand" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")
needs_host = pytest.mark.skipif(not _host_build(),
                                reason="built without --enable-ai-agent-host")


class Tree:
    """A workspace and the folders around it, in one temporary path.

    The editor starts in `ws` (its workspace and its HOME); `out` is a sibling
    holding a secret, `elsewhere` a second sibling; `vendor` inside the
    workspace is a link that leads to `out`."""

    def __init__(self, tmp_path):
        self.root = tmp_path
        self.ws = tmp_path / "ws"
        self.out = tmp_path / "out"
        self.other = tmp_path / "elsewhere"
        for d in (self.ws, self.out, self.other):
            d.mkdir()
        (self.ws / "in.txt").write_text("INSIDE-TEXT\n")
        (self.out / "a.txt").write_text(SECRET + " a\n")
        (self.out / "b.txt").write_text(SECRET + " b\n")
        (self.other / "e.txt").write_text("ELSEWHERE-TEXT\n")

    def link_vendor(self):
        os.symlink(str(self.out), str(self.ws / "vendor"))

    def seed_xwperc(self, lines):
        cfg = self.ws / ".config" / "xwpe"
        cfg.mkdir(parents=True, exist_ok=True)
        (cfg / "xwperc").write_text("[Programming]\n" + "".join(l + "\n" for l in lines))

    def trace(self):
        t = self.root / "ai.trace"
        return t.read_text() if t.exists() else ""


class Server:
    """A capturing OpenAI-compatible server: turn n answers replies[n], every
    later turn answers DONE.  `captured` holds each request body."""

    def __init__(self, replies):
        self.captured = []
        self.expected = len(replies) + 1
        owner = self

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
                owner.captured.append(self.rfile.read(n).decode("utf-8", "replace"))
                turn = len(owner.captured) - 1
                content = replies[turn] if turn < len(replies) else "DONE finished"
                body = (('data: {"choices":[{"delta":{"content":%s}}]}\n\n'
                         % json.dumps(content)).encode() + b'data: [DONE]\n\n')
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.end_headers()
                self.wfile.write(body)

        self.srv = HTTPServer(("127.0.0.1", 0), H)
        self.port = self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def done(self):
        return len(self.captured) >= self.expected

    def close(self):
        self.srv.shutdown()
        self.srv.server_close()

    def results(self):
        """Every TOOL RESULT message of the last request, in order."""
        msgs = json.loads(self.captured[-1])["messages"]
        return [m["content"] for m in msgs
                if m.get("role") == "user" and m["content"].startswith("TOOL RESULT:")]


def _screen(s):
    return "\n".join(s.display(drain=0.2))


def _env(tree, extra=None):
    env = {"XWPE_AI_ENABLE": "1", "OPENAI_API_KEY": "x",
           "XWPE_AI_TRACE": str(tree.root / "ai.trace")}
    env.update(extra or {})
    return env


def _config(tree, server, policy, extra_lines=()):
    tree.seed_xwperc(["AIBackend : 1",
                      "AIEndpoint : http://127.0.0.1:%d/v1" % server.port,
                      "AIModel : m", "AIPolicy : %s" % policy] + list(extra_lines))


def _answer_boxes(s, server, answers, poll=RUN_POLL_S):
    """Drive a run: press the next of `answers` whenever the read box shows,
    until the server has seen every turn.  A box with no answer left is
    denied and counted.  Returns the screens the box was seen on."""
    boxes = []
    end = time.time() + poll
    while time.time() < end:
        s._drain(0.3)
        scr = "\n".join(s.display(drain=0))
        if BOX_TITLE in scr:
            boxes.append(scr)
            s.key(answers.pop(0) if answers else "n", delay=0.8)
        elif server.done():
            break
    s._drain(1.0)
    return boxes


def _agent_run(tree, replies, policy, answers, extra_lines=(), filename="t.c",
               goal="inspect", then=None):
    """One agent run (Alt-G g) against the capturing server.  Returns the
    server, the boxes seen, the final screen and the trace.  `then(s, server)`
    may drive more of the same session before it closes."""
    server = Server(replies)
    try:
        _config(tree, server, policy, extra_lines)
        with WpeSession(str(tree.ws), SEED, env_extra=_env(tree), filename=filename) as s:
            s.key(ALT.AI, delay=0.6); s.key("g", delay=0.6)
            s.key(goal, delay=0.1); s.key("\r", delay=1.0)
            boxes = _answer_boxes(s, server, list(answers))
            if then:
                then(s, server)
            screen = _screen(s)
        trace = tree.trace()
        results = server.results() if server.captured else []
        return server, boxes, screen, trace, results
    finally:
        server.close()


# --- read_file outside asks at every dial ---------------------------------------

# @scenario SC-ai-agent-79
@pytest.mark.parametrize("policy", DIALS)
def test_read_file_outside_the_workspace_asks_at_every_dial(tmp_path, policy):
    tree = Tree(tmp_path)
    path = str(tree.out / "a.txt")
    server, boxes, screen, trace, results = _agent_run(
        tree, ["TOOL read_file %s" % path], policy, ["y"])
    assert boxes, "no read box for a file outside the workspace under %s:\n%s" % (policy, screen)
    assert BOX_MARK in boxes[0], "the box does not say the read is outside:\n" + boxes[0]
    assert "agent read outside workspace read_file %s -> once" % path in trace, trace
    assert results and SECRET in results[-1], \
        "after yes the model did not read the file:\n%s" % results


# @scenario SC-ai-agent-79
def test_yes_once_does_not_open_the_next_file(tmp_path):
    tree = Tree(tmp_path)
    a, b = str(tree.out / "a.txt"), str(tree.out / "b.txt")
    server, boxes, screen, trace, results = _agent_run(
        tree, ["TOOL read_file %s" % a, "TOOL read_file %s" % b], "auto", ["y", "n"])
    assert len(boxes) == 2, "the second file of the same folder was not asked about:\n%s" % boxes
    assert SECRET in results[-2], results
    assert DENIED_RESULT in results[-1] and SECRET not in results[-1], results


# --- a folder opened for the session --------------------------------------------

# @scenario SC-ai-agent-80
def test_yes_for_this_folder_opens_it_and_the_menu_withdraws_it(tmp_path):
    tree = Tree(tmp_path)
    a, b = str(tree.out / "a.txt"), str(tree.out / "b.txt")
    state = {}

    def withdraw(s, server):
        s.key(ALT.AI, delay=0.6)
        state["menu"] = _screen(s)
        s.key("r", delay=0.8)                       # Read folders
        state["picker"] = _screen(s)
        s.key("\r", delay=0.8)                      # Enter withdraws the folder
        s.key(ALT.AI, delay=0.6)
        state["menu_after"] = _screen(s)
        s.key("\033", delay=0.4)

    server, boxes, screen, trace, results = _agent_run(
        tree, ["TOOL read_file %s" % a, "TOOL read_file %s" % b], "ask", ["f"], then=withdraw)
    assert len(boxes) == 1, "the folder was asked about twice:\n%s" % boxes
    assert SECRET in results[-1] and SECRET in results[-2], results
    assert "agent read outside workspace read_file %s -> folder" % a in trace, trace
    assert "reads: 1 folder" in screen or "Read folders: 1" in state["menu"], \
        "the folder is not shown in the assistant's status:\n" + state["menu"]
    assert "Read folders" in state["menu"], state["menu"]
    assert str(tree.out) in state["picker"], \
        "the picker does not list the folder:\n" + state["picker"]
    assert "Read folders" not in state["menu_after"], \
        "the folder is still open after it was withdrawn:\n" + state["menu_after"]
    assert "agent read folder withdrawn %s" % tree.out in tree.trace()


# @scenario SC-ai-agent-80
def test_a_withdrawn_folder_asks_again(tmp_path):
    tree = Tree(tmp_path)
    a, b = str(tree.out / "a.txt"), str(tree.out / "b.txt")

    def withdraw_then_read(s, server):
        s.key(ALT.AI, delay=0.6); s.key("r", delay=0.8); s.key("\r", delay=0.8)
        server.expected += 2
        s.key(ALT.AI, delay=0.6); s.key("g", delay=0.6)
        s.key("again", delay=0.1); s.key("\r", delay=1.0)
        boxes = _answer_boxes(s, server, ["n"])
        withdraw_then_read.boxes = boxes

    server, boxes, screen, trace, results = _agent_run(
        tree, ["TOOL read_file %s" % a, "DONE first", "TOOL read_file %s" % b, "DONE second"],
        "ask", ["f"], then=withdraw_then_read)
    assert len(boxes) == 1, boxes
    assert getattr(withdraw_then_read, "boxes", []), \
        "after the folder was withdrawn a read in it did not ask:\n" + screen


# --- No gives the model the error -----------------------------------------------

# @scenario SC-ai-agent-81
def test_no_gives_the_model_an_error_naming_the_folder(tmp_path):
    tree = Tree(tmp_path)
    path = str(tree.out / "a.txt")
    server, boxes, screen, trace, results = _agent_run(
        tree, ["TOOL read_file %s" % path], "auto", ["n"])
    assert boxes, screen
    assert results, "no tool result reached the model"
    assert results[-1].startswith("TOOL RESULT:\nTOOL ERROR: ") and DENIED_RESULT in results[-1], results
    assert SECRET not in results[-1], "the file was read after no:\n%s" % results
    assert "[agent] denied: read_file %s" % path in screen or \
        "agent deny read_file %s" % path in trace, trace


# --- links a walk does not follow -----------------------------------------------

# @scenario SC-ai-agent-82
@pytest.mark.parametrize("tool,arg", [("grep", SECRET), ("glob", "*.txt"), ("list_dir", ".")])
def test_a_search_skips_a_link_leading_out_and_says_so(tmp_path, tool, arg):
    tree = Tree(tmp_path)
    tree.link_vendor()
    server, boxes, screen, trace, results = _agent_run(
        tree, ["TOOL %s %s" % (tool, arg)], "auto", [])
    assert not boxes, "a search inside the workspace asked:\n" + boxes[0]
    assert results, "no result for %s" % tool
    assert SECRET not in results[-1], \
        "%s followed the link out and read the secret:\n%s" % (tool, results[-1])
    assert "1 link was not followed" in results[-1] or SKIPPED_NOTE in results[-1], \
        "%s did not say it left a link alone:\n%s" % (tool, results[-1])


# @scenario SC-ai-agent-82
def test_a_search_inside_the_workspace_with_no_link_says_nothing_about_links(tmp_path):
    tree = Tree(tmp_path)
    server, boxes, screen, trace, results = _agent_run(
        tree, ["TOOL grep INSIDE-TEXT"], "auto", [])
    assert not boxes, boxes
    assert "in.txt" in results[-1], results
    assert SKIPPED_NOTE not in results[-1], results


# @scenario SC-ai-agent-82
@pytest.mark.parametrize("tool,arg", [("list_dir", "."), ("grep", "INSIDE-TEXT"), ("glob", "*.txt")])
def test_a_search_that_starts_above_the_workspace_asks(tmp_path, tool, arg):
    """The editor was started in a parent folder of the file it edits: the
    workspace is the file's folder, so a search from the working directory
    would read beyond it."""
    tree = Tree(tmp_path)
    (tree.ws / "sub").mkdir()
    server, boxes, screen, trace, results = _agent_run(
        tree, ["TOOL %s %s" % (tool, arg)], "auto", ["n"], filename="sub/t.c")
    assert boxes, "%s from a folder above the workspace did not ask:\n%s" % (tool, screen)
    assert DENIED_RESULT in results[-1], results


# --- inside the scope nothing is asked (guards) ----------------------------------

# @scenario SC-ai-agent-83
@pytest.mark.parametrize("policy", DIALS)
def test_reads_inside_the_workspace_are_not_asked_at_any_dial(tmp_path, policy):
    tree = Tree(tmp_path)
    inside = str(tree.ws / "in.txt")
    server, boxes, screen, trace, results = _agent_run(
        tree, ["TOOL read_file %s" % inside, "TOOL list_dir .", "TOOL grep INSIDE", "TOOL glob *.txt"],
        policy, [])
    assert not boxes, "a read inside the workspace asked under %s:\n%s" % (policy, boxes[0])
    assert "INSIDE-TEXT" in results[-4], results
    assert "in.txt" in results[-3] and "in.txt" in results[-1], results


# --- the project's include path and the roots in xwperc --------------------------

# @scenario SC-ai-agent-84
def test_include_directories_and_configured_roots_are_read_unasked(tmp_path):
    tree = Tree(tmp_path)
    inc = str(tree.out)
    root = str(tree.other)
    (tmp_path / "f.txt").write_text("BESIDE\n")
    server, boxes, screen, trace, results = _agent_run(
        tree,
        ["TOOL read_file %s/a.txt" % inc, "TOOL read_file %s/e.txt" % root,
         "TOOL read_file %s/f.txt" % tmp_path],
        "auto", ["n"],
        extra_lines=["IncludePath : %s" % inc, "AIReadRoots : %s" % root])
    assert SECRET in results[-3], "the project's include directory asked or was refused:\n%s" % results
    assert "ELSEWHERE-TEXT" in results[-2], "a root from xwperc asked or was refused:\n%s" % results
    assert len(boxes) == 1 and DENIED_RESULT in results[-1], \
        "a folder that is neither declared nor granted did not ask exactly once:\n%s" % boxes


# @scenario SC-ai-agent-84
def test_the_root_folder_is_never_a_grant(tmp_path):
    """A root of "/" in xwperc or in the include path would open every file:
    it opens nothing, and a read box about a path in "/" offers no folder."""
    tree = Tree(tmp_path)
    path = str(tree.out / "a.txt")
    server, boxes, screen, trace, results = _agent_run(
        tree, ["TOOL read_file %s" % path, "TOOL list_dir /"], "auto", ["n", "n"],
        extra_lines=["IncludePath : /", "AIReadRoots : /"])
    assert len(boxes) == 2, \
        "a root of / let a read outside the workspace through unasked:\n%s" % boxes
    assert DENIED_RESULT in results[-2] and SECRET not in results[-2], results
    assert "f this folder" in boxes[0], boxes[0]
    assert "f this folder" not in boxes[1] and "this folder: /" not in boxes[1], \
        "the box offered to open the whole disk:\n" + boxes[1]


# --- the chat and the plan study answer to the same scope ------------------------

def _chat_run(tree, reply, policy, answers):
    server = Server([reply])
    try:
        _config(tree, server, policy)
        with WpeSession(str(tree.ws), SEED, env_extra=_env(tree)) as s:
            s.key(ALT.AI, delay=0.6); s.key("a", delay=0.6)
            s.key("look at it", delay=0.1); s.key("\r", delay=1.0)
            boxes = _answer_boxes(s, server, list(answers))
            screen = _screen(s)
        results = server.results() if server.captured else []
        return boxes, screen, results
    finally:
        server.close()


# @scenario SC-ai-agent-85
def test_the_chat_reads_outside_the_workspace_only_after_asking(tmp_path):
    tree = Tree(tmp_path)
    path = str(tree.out / "a.txt")
    boxes, screen, results = _chat_run(tree, "TOOL read_file %s" % path, "auto", ["n"])
    assert boxes, "the chat read a file outside the workspace without asking:\n" + screen
    assert results and SECRET not in results[-1], results


def _plan_run(tree, reply, policy, answers):
    server = Server([reply])
    try:
        _config(tree, server, policy)
        with WpeSession(str(tree.ws), SEED, env_extra=_env(tree)) as s:
            s.key(ALT.AI, delay=0.6); s.key("f", delay=0.6)
            s.key("study", delay=0.1); s.key("\r", delay=1.0)
            boxes = _answer_boxes(s, server, list(answers))
            screen = _screen(s)
        results = server.results() if server.captured else []
        return boxes, screen, results
    finally:
        server.close()


# @scenario SC-ai-agent-85
def test_the_plan_study_reads_outside_the_workspace_only_after_asking(tmp_path):
    tree = Tree(tmp_path)
    path = str(tree.out / "a.txt")
    boxes, screen, results = _plan_run(tree, "TOOL read_file %s" % path, "auto", ["n"])
    assert boxes, "the plan study read a file outside the workspace without asking:\n" + screen
    assert results and SECRET not in results[-1], results


# --- the write box names the real place on a row of its own ----------------------

# @scenario SC-ai-agent-86
def test_the_write_box_names_the_real_place_on_its_own_row(tmp_path):
    tree = Tree(tmp_path)
    tree.link_vendor()
    rel = str(tree.ws / "vendor" / "w.txt")
    reply = "TOOL write_file %s\nbeyond\n@@END@@TURN@@DONE wrote it" % rel
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_MOCK_REPLY": reply,
           "XWPE_AI_POLICY": "auto", "XWPE_AI_TRACE": str(tree.root / "ai.trace")}
    with WpeSession(str(tree.ws), SEED, env_extra=env) as s:
        s.key(ALT.AI, delay=0.6); s.key("g", delay=0.6)
        s.key("write", delay=0.1); s.key("\r", delay=1.0)
        end = time.time() + 10
        box = ""
        while time.time() < end and " Write " not in box:
            s._drain(0.3)
            box = "\n".join(s.display(drain=0))
        s.key("n", delay=0.5)
    rows = [r for r in box.splitlines() if "lands at" in r]
    assert rows, "the box does not say where the file lands:\n" + box
    assert all("outside the workspace" not in r for r in rows), \
        "the place shares the mark's row, so a long path clips it:\n" + "\n".join(rows)


# --- the hosted engine ------------------------------------------------------------

def _host_perm(tree, tool, tool_input, policy, answers, filename="t.c", resp_dir=None):
    """One hosted turn in which the stub engine asks for `tool`; the editor's
    answer is read back from what the stub received, in the folder the stub
    ran in (`resp_dir`, the workspace by default)."""
    from test_ai_host_dial_and_interrupt import MOCK_PERM
    mock = tree.root / "mock_perm.py"
    mock.write_text(MOCK_PERM % {"tool": tool, "input": tool_input})
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_AGENT_ENGINE": "claude-code",
           "XWPE_AI_POLICY": policy, "XWPE_AI_HOST_CMD": "python3 %s" % mock,
           "XWPE_AI_WEB": "1", "XWPE_AI_TRACE": str(tree.root / "ai.trace")}
    screens = []
    resp = (resp_dir or tree.ws) / "perm_resp.txt"
    if resp.exists():
        resp.unlink()
    with WpeSession(str(tree.ws), SEED, env_extra=env, filename=filename) as s:
        s._drain(0.5)
        s.key(ALT.AI); s.key("g"); s._drain(0.5)
        s.key("use the tool"); s.key("\r", delay=0.8)
        pending = list(answers)
        end = time.time() + 15
        while time.time() < end:
            s._drain(0.4)
            scr = "\n".join(s.display(drain=0))
            if "PATH" in scr and ("ALLOWEDPATH" in scr or "DENIEDPATH" in scr):
                break
            if BOX_TITLE in scr or " Write " in scr or "APPROVE" in scr or \
               "outside the workspace:" in scr:
                screens.append(scr)
                s.key(pending.pop(0) if pending else "n", delay=0.6)
        screen = _screen(s)
    return screens, screen, (resp.read_text() if resp.exists() else ""), tree.trace()


def _allowed(resp):
    return '"behavior":"allow"' in resp.replace(" ", "")


# @scenario SC-ai-agent-87
@needs_host
@pytest.mark.parametrize("policy", DIALS)
def test_a_hosted_read_outside_the_workspace_asks_at_every_dial(tmp_path, policy):
    tree = Tree(tmp_path)
    path = str(tree.out / "a.txt")
    screens, screen, resp, trace = _host_perm(
        tree, "Read", '{"file_path": %r}' % path, policy, ["y"])
    assert screens and BOX_MARK in screens[0], \
        "no read box for a hosted Read outside under %s:\n%s" % (policy, screen)
    assert _allowed(resp), "the engine was not answered allow after yes:\n" + resp


# @scenario SC-ai-agent-87
@needs_host
def test_a_hosted_read_outside_refused_reaches_the_engine_as_a_denial(tmp_path):
    tree = Tree(tmp_path)
    path = str(tree.out / "a.txt")
    screens, screen, resp, trace = _host_perm(
        tree, "Read", '{"file_path": %r}' % path, "auto", ["n"])
    assert screens, screen
    assert '"behavior":"deny"' in resp.replace(" ", ""), resp


# @scenario SC-ai-agent-87
@needs_host
@pytest.mark.parametrize("policy", DIALS)
def test_a_hosted_read_inside_the_workspace_asks_nothing_at_any_dial(tmp_path, policy):
    """One judge, one answer: the editor's own read_file is unasked inside the
    workspace at every dial, so the hosted engine's Read is too."""
    tree = Tree(tmp_path)
    screens, screen, resp, trace = _host_perm(
        tree, "Read", '{"file_path": %r}' % str(tree.ws / "in.txt"), policy, [])
    assert not screens, "a hosted Read inside the workspace asked under %s:\n%s" % (policy, screens[0])
    assert _allowed(resp), resp


# @scenario SC-ai-agent-89
@needs_host
def test_a_hosted_tool_the_editor_does_not_know_is_judged_by_the_path_it_names(tmp_path):
    """A tool outside the editor's list that still names a file is held to the
    write rule: beyond the workspace it asks at every dial, auto included."""
    tree = Tree(tmp_path)
    path = str(tree.out / "a.txt")
    screens, screen, resp, trace = _host_perm(
        tree, "mcp__files__read_text", '{"path": %r}' % path, "auto", ["n"])
    assert screens and "outside the workspace" in screens[0], \
        "an unknown tool naming a path outside passed under auto:\n" + screen
    assert '"behavior":"deny"' in resp.replace(" ", ""), resp


# @scenario SC-ai-agent-90
@needs_host
def test_under_auto_a_hosted_command_still_runs_at_once(tmp_path):
    """Guard: auto now routes the engine's prompts to the editor instead of
    switching its checks off, and the editor answers a command at once, as
    the dial promises; only the paths beyond the workspace ask."""
    tree = Tree(tmp_path)
    screens, screen, resp, trace = _host_perm(tree, "Bash", '{"command": "true"}', "auto", [])
    assert not screens, "a command asked under auto:\n" + screens[0]
    assert _allowed(resp), resp
    assert "auto-approved (auto)" in screen, screen


# @scenario SC-ai-agent-89
@needs_host
def test_a_hosted_tool_the_editor_does_not_know_that_names_no_path_follows_the_dial(tmp_path):
    tree = Tree(tmp_path)
    screens, screen, resp, trace = _host_perm(tree, "TodoWrite", '{"todos": []}', "auto", [])
    assert not screens, screens
    assert _allowed(resp), resp


# @scenario SC-ai-agent-89
@needs_host
def test_the_hosted_engine_starts_in_the_workspace_and_judges_relative_paths_there(tmp_path):
    """The editor was started in a folder above the file it edits.  The engine
    runs in the workspace (the file's folder), so a search that names no path
    reads the workspace and asks nothing, and a relative path is the same place
    to both."""
    tree = Tree(tmp_path)
    (tree.ws / "sub").mkdir()
    screens, screen, resp, trace = _host_perm(
        tree, "Grep", '{"pattern": "x"}', "auto", [], filename="sub/t.c",
        resp_dir=tree.ws / "sub")
    assert resp, "the engine did not run in the workspace (no answer recorded there)"
    assert not screens, "a search from the workspace asked:\n" + screens[0]
    assert _allowed(resp), resp
    screens, screen, resp, trace = _host_perm(
        tree, "Read", '{"file_path": "t.c"}', "auto", [], filename="sub/t.c",
        resp_dir=tree.ws / "sub")
    assert not screens and _allowed(resp), \
        "a relative path to the workspace's own file was judged elsewhere:\n%s\n%s" % (screen, resp)
    screens, screen, resp, trace = _host_perm(
        tree, "Read", '{"file_path": "../in.txt"}', "auto", ["n"], filename="sub/t.c",
        resp_dir=tree.ws / "sub")
    assert screens and BOX_MARK in screens[0], \
        "a relative path climbing out of the workspace was not asked about:\n" + screen


# @scenario SC-ai-agent-88
@needs_host
@pytest.mark.parametrize("policy", ["edits", "auto"])
def test_a_hosted_write_outside_the_workspace_shows_the_marked_box(tmp_path, policy):
    tree = Tree(tmp_path)
    path = str(tree.out / "w.txt")
    screens, screen, resp, trace = _host_perm(
        tree, "Write", '{"file_path": %r}' % path, policy, ["n"])
    assert screens and " Write " in screens[0], \
        "no write box for a hosted Write outside under %s:\n%s" % (policy, screen)
    assert "outside the workspace" in screens[0], screens[0]
    assert '"behavior":"deny"' in resp.replace(" ", ""), resp


# @scenario SC-ai-agent-88
@needs_host
def test_a_hosted_write_inside_the_workspace_is_still_auto_approved_under_edits(tmp_path):
    tree = Tree(tmp_path)
    screens, screen, resp, trace = _host_perm(
        tree, "Write", '{"file_path": %r}' % str(tree.ws / "t.c"), "edits", [])
    assert not screens, screens
    assert _allowed(resp), resp


# @scenario SC-ai-agent-89
@needs_host
def test_a_hosted_file_tool_with_no_path_is_refused_never_passed_by_class(tmp_path):
    tree = Tree(tmp_path)
    screens, screen, resp, trace = _host_perm(tree, "Read", "{}", "auto", [])
    assert '"behavior":"deny"' in resp.replace(" ", ""), \
        "a Read with no path was passed:\n" + resp
    assert "host Read names no path: refused" in trace, trace


# @scenario SC-ai-agent-89
@needs_host
def test_a_hosted_search_with_no_path_is_judged_at_the_working_directory(tmp_path):
    tree = Tree(tmp_path)
    screens, screen, resp, trace = _host_perm(
        tree, "Grep", '{"pattern": "x"}', "auto", [])
    assert not screens, "a search from the workspace asked:\n" + screens[0]
    assert _allowed(resp), resp


# --- how the hosted engine is launched, per dial ---------------------------------

def _argv(tmp_path, policy, extra_lines=()):
    from test_ai_agent_engine_contracts import _host_run
    if extra_lines:
        cfg = tmp_path / ".config" / "xwpe"
        cfg.mkdir(parents=True, exist_ok=True)
        (cfg / "xwperc").write_text("[Programming]\n" + "".join(l + "\n" for l in extra_lines))
    rows, argv, hf, _, _ = _host_run(tmp_path, policy)
    return argv


def _value_after(argv, flag):
    return argv[argv.index(flag) + 1] if flag in argv else None


# @scenario SC-ai-agent-90
@needs_host
@pytest.mark.parametrize("policy,mode", [("ask", "manual"), ("edits", "acceptEdits"),
                                         ("auto", "acceptEdits")])
def test_the_hosted_engine_routes_its_prompts_to_the_editor_at_every_dial(tmp_path, policy, mode):
    argv = _argv(tmp_path, policy)
    assert _value_after(argv, "--permission-mode") == mode, argv
    assert "--dangerously-skip-permissions" not in argv, \
        "the permission bypass is on the command line under %s:\n%s" % (policy, argv)
    assert _value_after(argv, "--permission-prompts") == "host", argv
    assert _value_after(argv, "--permission-prompt-tool") == "stdio", argv


# @scenario SC-ai-agent-90
@needs_host
def test_the_hosted_engine_is_told_the_folders_the_user_already_allowed(tmp_path):
    tree = Tree(tmp_path)
    argv = _argv(tree.ws, "edits", ["AIReadRoots : %s:/" % tree.other])
    assert "--add-dir" in argv, argv
    assert str(tree.other) in argv[argv.index("--add-dir"):], argv
    assert "/" not in argv, "the whole disk was passed to the engine as a folder it may read:\n%s" % argv
