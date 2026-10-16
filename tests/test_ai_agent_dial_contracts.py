"""The permission dial end to end, and the review's per-run keys.

Under "edits" a write is auto-approved and a command or web tool still asks;
under "auto" a web tool is auto-approved too, and the search actually reaches
the server. Alt-G y cycles the dial with a flash that replaces the status
bar for one key, the choice is written to xwperc and shown in the next
header. Lowering the dial mid-run (auto -> ask) makes the very next write
ask, and the run is still checkpointed and reviewable at the end. In the
changeset review, "f" asks file by file (keep/revert each) and "c" commits
in a git worktree when one exists, leaving a plain directory untouched.
"""
import os
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN, pane_text_with_history

SEED = "int main(void){return 0;}\n"

FOLLOWUP_LINE = "type a follow-up"
REVIEW_PROMPT = "a = keep all"
TURN_POLL_S = 20
REVIEW_POLL_S = 25
BAR_WORD = "F1 Help"
# Pane lines the dial tests read back.  The end-of-run review opens Messages
# beside the pane, halving it: these lines then wrap over two rows or scroll
# above the view, so they are read as the pane's logical lines, paged back.
EDITS_WRITE_LINE = "[agent] auto-approved (edits): write a.txt (5 bytes)"
DENIED_WEB_LINE = "[agent] denied: web_search x"
AUTO_WEB_LINE = "[agent] auto-approved (auto): x y"
RUNNING_WEB_LINE = "[agent] running web_search ... (result follows)"
EDITS_HEADER_TAIL = "policy: edits]"
# The review's per-file question names the file by its full path in the
# Messages window, an ordinary editor window that is cut at its border, not
# wrapped; the question is read with the terminal widened, as a user with a
# long temporary path would widen it.
WIDE_COLS = 160
WIDE_ROWS = 30

# The driver scales its own key/drain waits by XWPE_TEST_WAIT_SCALE (the
# sanitizer build runs at 2, the Debian autopkgtest at 3); a fixed
# XWPE_AI_MOCK_DELAY_MS does not, so the dial-switch-mid-turn test below
# scales its own mock delay and the poll bounds that wait on it by hand to
# keep the same margin at every scale.
WAIT_SCALE = float(os.environ.get("XWPE_TEST_WAIT_SCALE", "1"))
DIAL_SWITCH_DELAY_MS = int(6000 * WAIT_SCALE)


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"autonomous coding agent" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _env(tmp_path, reply, policy, **more):
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_POLICY": policy,
        "XWPE_AI_TRACE": str(tmp_path / "ai.trace"),
    }
    env.update(more)
    return env


def _trace(tmp_path):
    t = tmp_path / "ai.trace"
    return t.read_text() if t.exists() else ""


def _screen(s, drain=0.2):
    return "\n".join(s.display(drain=drain))


def _seen_within(s, needle, timeout):
    end = time.time() + timeout
    while time.time() < end:
        s._drain(0.25)
        if needle in "\n".join(s.display(drain=0)):
            return True
    return False


def _traced_within(tmp_path, needle, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if needle in _trace(tmp_path):
            return True
        time.sleep(0.2)
    return False


def _read_xwperc(tmp_path):
    p = tmp_path / ".config" / "xwpe" / "xwperc"
    return p.read_text() if p.exists() else ""


def _start_agent(s, task, delay=1.0):
    s.key(ALT.AI); s.key("g")
    s.key(task); s.key("\r", delay=delay)


def _search_server():
    """A tiny HTTP server for the web_search tool: /find answers 200 and
    records every path hit."""
    paths = []

    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            paths.append(self.path)
            body = b"<html><body>result</body></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    srv = HTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1], paths


# @scenario SC-ai-agent-17
def test_edits_auto_approves_writes_and_asks_for_commands_and_web(tmp_path):
    srv, port, paths = _search_server()
    marker = tmp_path / "CMD_MARKER"
    turns = "@@TURN@@".join([
        "TOOL write_file a.txt\nalpha\n@@END",
        "TOOL run_command touch %s" % marker,
        "TOOL web_search x",
        "TOOL apply_patch a.txt\n<<<<<<< SEARCH\nalpha\n=======\nbeta\n>>>>>>> REPLACE\n@@END",
        "DONE all done"])
    # The web switch is answered on because the dial is what this asserts; an
    # unanswered switch counts as off for an agent task, which has nobody to
    # ask, and the search would be refused before the dial saw it.
    env = _env(tmp_path, turns, "edits", XWPE_AI_WEB="1",
               XWPE_AI_SEARCH_URL="http://127.0.0.1:%d/find?q=%%s" % port)
    try:
        with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
            _start_agent(s, "do several things", delay=1.0)
            assert _seen_within(s, "APPROVE?  touch", TURN_POLL_S), _screen(s)
            s.key("n", delay=1.0)
            assert _seen_within(s, "APPROVE?  x", TURN_POLL_S), _screen(s)
            s.key("n", delay=1.0)
            assert _seen_within(s, REVIEW_PROMPT, TURN_POLL_S), _screen(s)
            review = _screen(s)
            s.key("a", delay=1.0)
            assert _seen_within(s, FOLLOWUP_LINE, TURN_POLL_S), _screen(s)
            lines = pane_text_with_history(s, [EDITS_WRITE_LINE, DENIED_WEB_LINE])
    finally:
        srv.shutdown()
    txt = _trace(tmp_path)
    text = "\n".join(lines)
    # By the time the command box is up the header has scrolled off the pane
    # (ten lines in an eleven-row pane), so the dial is read from the trace.
    assert "agent policy=edits" in txt, txt
    assert "checkpoint snapshot" in txt or "checkpoint git" in txt, txt
    # write_file keeps the newline that ends its last body line, so
    # "alpha\n@@END" lands as 6 bytes; apply_patch then matches the literal
    # "alpha" substring and leaves the newline, writing "beta\n" (5 bytes).
    assert "agent auto-approve write a.txt (6 bytes)" in txt, txt     # scrolled off the pane by now
    assert EDITS_WRITE_LINE in text, text                            # apply_patch
    assert "agent deny run_command touch" in txt, txt       # its pane line scrolled off by the end
    assert DENIED_WEB_LINE in text, text
    assert not marker.exists()
    assert not paths, paths
    assert (tmp_path / "a.txt").read_text() == "beta\n"
    assert "AI changeset: 1 change" in review, review


# @scenario SC-ai-agent-17
def test_auto_web_search_is_auto_approved_and_reaches_the_server(tmp_path):
    srv, port, paths = _search_server()
    env = _env(tmp_path, "TOOL web_search x y@@TURN@@DONE found", "auto",
               XWPE_AI_WEB="1",   # the dial is the subject; the switch must be open
               XWPE_AI_SEARCH_URL="http://127.0.0.1:%d/find?q=%%s" % port)
    try:
        with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
            _start_agent(s, "search", delay=1.0)
            assert _seen_within(s, FOLLOWUP_LINE, TURN_POLL_S), _screen(s)
            lines = pane_text_with_history(s, [AUTO_WEB_LINE, RUNNING_WEB_LINE])
    finally:
        srv.shutdown()
    text = "\n".join(lines)
    assert AUTO_WEB_LINE in text, text
    assert RUNNING_WEB_LINE in text, text
    assert paths == ["/find?q=x%20y"], paths


# @scenario SC-ai-agent-21
def test_altg_y_flashes_persists_to_xwperc_and_shows_in_next_header(tmp_path):
    env = _env(tmp_path, "DONE ok", "ask")
    del env["XWPE_AI_POLICY"]
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        s.key(ALT.AI); s.key("y", delay=0.8)
        rows = s.display()
        bar = rows[-1]
        s.key("\033[B", delay=0.6)                 # any key restores the bar
        rows2 = s.display()
        rc = _read_xwperc(tmp_path)
        _start_agent(s, "x", delay=1.5)
        text = "\n".join(pane_text_with_history(s, [EDITS_HEADER_TAIL]))
    assert "Permissions:" in bar and "ask" in bar and "edits" in bar and "auto" in bar, bar
    assert "auto-accept edits, ask before commands" in bar, bar
    assert BAR_WORD not in bar
    assert BAR_WORD in rows2[-1], rows2[-1]
    assert "AIPolicy : edits" in rc, rc
    assert EDITS_HEADER_TAIL in text, text
    assert "policy set edits" in _trace(tmp_path)


# @scenario SC-ai-agent-22
def test_dial_lowered_mid_run_asks_for_the_next_write_and_still_reviews(tmp_path):
    turns = "TOOL write_file out.txt\nhello\n@@END@@TURN@@DONE wrote"
    env = _env(tmp_path, turns, "auto", XWPE_AI_MOCK_DELAY_MS=str(DIAL_SWITCH_DELAY_MS))
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _start_agent(s, "write", delay=0.3)
        s.key(ALT.AI); s.key("y")                  # auto -> ask
        assert _traced_within(tmp_path, "policy set ask", 6), _trace(tmp_path)
        got_box = _seen_within(s, " Write ", int(20 * WAIT_SCALE))
        if got_box:
            s.key("y", delay=1.0)
        assert _seen_within(s, REVIEW_PROMPT, int(20 * WAIT_SCALE)) or \
            _seen_within(s, FOLLOWUP_LINE, int(5 * WAIT_SCALE)), _screen(s)
        review = _screen(s)
        if REVIEW_PROMPT in review:
            s.key("a", delay=1.0)
    txt = _trace(tmp_path)
    assert got_box, "a write under a dial lowered to ask did not ask"
    assert "agent write confirm out.txt -> allow" in txt, txt
    assert REVIEW_PROMPT in review, "the checkpointed run did not open the review:\n" + review
    assert (tmp_path / "out.txt").read_text() == "hello\n"


# @scenario SC-ai-agent-33
def test_review_file_by_file_keeps_one_and_reverts_the_other(tmp_path):
    turns = "@@TURN@@".join(["TOOL write_file a.txt\nAAA\n@@END",
                             "TOOL write_file b.txt\nBBB\n@@END", "DONE two files"])
    env = _env(tmp_path, turns, "auto")
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _start_agent(s, "write two", delay=1.0)
        assert _seen_within(s, REVIEW_PROMPT, 20), _screen(s)
        s.resize(WIDE_COLS, WIDE_ROWS)       # the question's path fits the wider Messages
        s.key("f", delay=1.0)
        q1 = _screen(s)
        s.key("y", delay=1.0)
        q2 = _screen(s)
        s.key("n", delay=1.0)
        assert _seen_within(s, FOLLOWUP_LINE, TURN_POLL_S), _screen(s)
    txt = _trace(tmp_path)
    import re
    # The question's "?  y = keep  n = revert" tail can be cut at the
    # window's border when tmp_path is long, so only the "keep <path>"
    # prefix is matched.
    asked1 = re.findall(r"keep \S*/(\w\.txt)", q1)
    asked2 = re.findall(r"keep \S*/(\w\.txt)", q2)
    assert len(asked1) == 1, "no per-file question after f:\n" + q1
    assert len(asked2) == 2, "the second question did not appear:\n" + q2
    kept, reverted = asked2[0], asked2[1]
    assert (tmp_path / kept).exists(), "kept file vanished"
    assert not (tmp_path / reverted).exists(), "reverted new file still exists"
    assert "changeset revert" in txt


# @scenario SC-ai-agent-33
def test_review_commit_in_a_git_tree_makes_a_commit(tmp_path):
    # A throwaway repository in the test's own tmp_path.
    subprocess.run(["git", "init", "-q", str(tmp_path)], check=True)
    (tmp_path / "t.c").write_text(SEED)
    subprocess.run(["git", "-C", str(tmp_path), "-c", "user.name=p", "-c", "user.email=p@x",
                    "add", "t.c"], check=True)
    subprocess.run(["git", "-C", str(tmp_path), "-c", "user.name=p", "-c", "user.email=p@x",
                    "commit", "-q", "-m", "seed"], check=True)
    turns = "TOOL write_file a.txt\nAAA\n@@END@@TURN@@DONE one file"
    env = _env(tmp_path, turns, "auto", GIT_AUTHOR_NAME="p", GIT_AUTHOR_EMAIL="p@x",
               GIT_COMMITTER_NAME="p", GIT_COMMITTER_EMAIL="p@x")
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _start_agent(s, "write one", delay=1.0)
        assert _seen_within(s, REVIEW_PROMPT, 20), _screen(s)
        s.key("c", delay=1.0)
        prompt = _screen(s)
        s.key("\r", delay=1.5)
        assert _seen_within(s, FOLLOWUP_LINE, TURN_POLL_S), _screen(s)
    txt = _trace(tmp_path)
    log = subprocess.run(["git", "-C", str(tmp_path), "log", "--oneline"],
                         stdout=subprocess.PIPE, text=True).stdout
    assert "Commit message" in prompt, prompt
    assert "checkpoint git ref=" in txt, txt
    assert "changeset commit" in txt, txt
    assert "AI-assisted change" in log, log
