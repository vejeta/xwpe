"""A link inside the workspace that leads outside it does not hide a write.

The agent's write policy asks, with the marked box, before any write that
lands outside the workspace, whatever the permission dial.  The check used to
compare path text only, so a directory link inside the tree (a vendored
directory, a link the model made with a command) let an unattended run write
beyond it without asking.  The check now follows links before it decides:

* a file under a linked directory, new or existing, and a dangling link as the
  last component are judged by where the link leads;
* a link loop is outside;
* a link that stays inside the tree is inside (no box where none is due);
* ".." after a link goes up from where the link leads, not from where it sits.

The test driver no longer links the machine's login into every session's HOME,
which is also the workspace: a test that needs the real login says so.  The
two rows at the end use a fake home, so nothing of the user's is ever read or
written.
"""
import os
import time

import pytest
from wpe_driver import WpeSession, ALT, pane_text_with_history
from test_ai_agent_tool_safety import _ai_build, _env, _trace, _wait_for, OUTSIDE_WORD

pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")

LANDS_WORD = "lands at"
RUN_DONE = "agent done"
WAIT_RUN_SECONDS = 20


class Tree:
    """A workspace and a sibling directory beside it, in one temporary path.

    The editor starts in `ws` (its workspace and its HOME); `out` is where the
    links lead.  `make` places a link or a file below the temporary path."""

    def __init__(self, tmp_path):
        self.root = tmp_path
        self.ws = tmp_path / "ws"
        self.out = tmp_path / "out"
        self.ws.mkdir()
        self.out.mkdir()
        (self.ws / "sub").mkdir()

    def link(self, name, target):
        os.symlink(str(target), str(self.ws / name))


def _wait_run_done(tmp_path):
    """Wait until the trace says the run ended; return the trace."""
    end = time.time() + WAIT_RUN_SECONDS
    while time.time() < end:
        txt = _trace(tmp_path)
        if RUN_DONE in txt:
            return txt
        time.sleep(0.3)
    return _trace(tmp_path)


def _agent_write(tree, rel, answer):
    """Under auto, have the agent write `rel` (as the model would name it) and
    answer the write box with `answer`, or take no box when none opens.

    Returns (the screen with the box or "", the pane's lines after, the
    trace)."""
    reply = "TOOL write_file %s\nbeyond the tree\n@@END@@TURN@@DONE wrote it" % rel
    env = _env(tree.root, reply, "auto")
    with WpeSession(str(tree.ws), "int main(void){return 0;}\n", env_extra=env) as s:
        s.key(ALT.AI); s.key("g")
        s.key("write through a link"); s.key("\r", delay=1.0)
        boxed = _wait_for(s, " Write ", timeout=8)
        box = "\n".join(s.display()) if boxed else ""
        if boxed:
            s.key(answer, delay=0.8)
        s._drain(1.5)
        txt = _wait_run_done(tree.root)
        after = "\n".join(pane_text_with_history(s, [OUTSIDE_WORD]))
    return box, after, txt


def _assert_asked(box, txt, rel):
    assert " Write " in box, "no write box under auto for a link leading out:\n" + box
    assert OUTSIDE_WORD in box, "the write box carries no outside mark:\n" + box
    assert "agent write outside workspace %s" % rel in txt, txt


# @scenario SC-ai-agent-71
def test_write_through_a_directory_link_leading_out_asks_and_names_the_place(tmp_path):
    tree = Tree(tmp_path)
    (tree.out / "f.txt").write_text("before\n")
    tree.link("vendor", tree.out)
    rel = str(tree.ws / "vendor" / "f.txt")
    box, after, txt = _agent_write(tree, rel, "y")
    _assert_asked(box, txt, rel)
    assert (tree.out / "f.txt").read_text() == "beyond the tree\n"
    assert OUTSIDE_WORD in after and LANDS_WORD in after, after
    assert str(tree.out.resolve() / "f.txt") in after, \
        "the pane does not say where the file really went:\n" + after


# @scenario SC-ai-agent-72
def test_new_file_under_a_directory_link_leading_out_asks(tmp_path):
    tree = Tree(tmp_path)
    tree.link("vendor", tree.out)
    rel = str(tree.ws / "vendor" / "new.txt")
    box, after, txt = _agent_write(tree, rel, "n")
    _assert_asked(box, txt, rel)
    assert not (tree.out / "new.txt").exists(), "a denied write still landed"
    assert "agent write confirm %s -> deny" % rel in txt, txt


# @scenario SC-ai-agent-73
def test_dangling_link_leading_out_as_the_last_component_asks(tmp_path):
    tree = Tree(tmp_path)
    tree.link("dang", tree.out / "made.txt")
    rel = str(tree.ws / "dang")
    box, after, txt = _agent_write(tree, rel, "n")
    _assert_asked(box, txt, rel)
    assert not (tree.out / "made.txt").exists(), "a denied write still landed"


# @scenario SC-ai-agent-74
def test_link_loop_counts_as_outside_and_asks(tmp_path):
    tree = Tree(tmp_path)
    tree.link("loopa", "loopb")
    tree.link("loopb", "loopa")
    rel = str(tree.ws / "loopa")
    box, after, txt = _agent_write(tree, rel, "n")
    _assert_asked(box, txt, rel)
    assert os.path.islink(str(tree.ws / "loopa")), "the loop link was replaced"


# @scenario SC-ai-agent-75
def test_link_that_stays_inside_the_tree_asks_nothing_under_auto(tmp_path):
    tree = Tree(tmp_path)
    tree.link("inlink", tree.ws / "sub")
    rel = str(tree.ws / "inlink" / "x.txt")
    box, after, txt = _agent_write(tree, rel, "n")
    assert box == "", "a write through a link that stays inside asked:\n" + box
    assert "outside workspace" not in txt, txt
    assert (tree.ws / "sub" / "x.txt").read_text() == "beyond the tree\n"


# @scenario SC-ai-agent-76
def test_dots_after_a_link_go_up_from_where_the_link_leads(tmp_path):
    tree = Tree(tmp_path)
    tree.link("vendor", tree.out)
    rel = str(tree.ws / "vendor" / ".." / "x.txt")      # the temporary path, not ws
    box, after, txt = _agent_write(tree, rel, "n")
    _assert_asked(box, txt, rel)
    assert not (tree.root / "x.txt").exists(), "a denied write still landed"
    assert not (tree.ws / "x.txt").exists()


LONG_DIR_NAME = "w" * 120         # a deep project directory, as users have
LONG_PATH_ROOM = 1060             # near the longest path a tool line carries


def _long_climb(room):
    """A relative path of about `room` bytes that climbs one level out:
    "./" repeated, then "../out.txt"."""
    tail = "../out.txt"
    return "./" * ((room - len(tail)) // 2) + tail


# @scenario SC-ai-agent-32
def test_a_long_relative_path_that_climbs_out_still_asks(tmp_path):
    """The check made the path absolute in a buffer shorter than a tool line
    plus a deep working directory; when it did not fit, the path counted as
    inside and an unattended run wrote it without the box."""
    ws = tmp_path / LONG_DIR_NAME
    ws.mkdir()
    rel = _long_climb(LONG_PATH_ROOM)
    reply = "TOOL write_file %s\nbeyond the tree\n@@END@@TURN@@DONE wrote it" % rel
    env = _env(tmp_path, reply, "auto")
    with WpeSession(str(ws), "int main(void){return 0;}\n", env_extra=env) as s:
        s.key(ALT.AI); s.key("g")
        s.key("write a long path"); s.key("\r", delay=1.0)
        boxed = _wait_for(s, " Write ", timeout=8)
        box = "\n".join(s.display()) if boxed else ""
        if boxed:
            s.key("n", delay=0.8)
        s._drain(1.5)
        _wait_run_done(tmp_path)
    assert " Write " in box, "a long path climbing out wrote without the box"
    assert OUTSIDE_WORD in box, "the write box carries no outside mark:\n" + box
    assert not (tmp_path / "out.txt").exists(), "the write landed outside unasked"


MARKER = "login marker, must stay as it is\n"
WRITE_SETTINGS = ".claude/settings.json"


def _fake_home(tmp_path, monkeypatch):
    """A home of its own for the driver's idea of "the machine's login": the
    test never goes near the real one.  Returns the marker file."""
    home = tmp_path / "fakehome"
    (home / ".claude").mkdir(parents=True)
    marker = home / ".claude" / "settings.json"
    marker.write_text(MARKER)
    (home / ".claude.json").write_text("{}\n")
    monkeypatch.setenv("HOME", str(home))
    return marker


def _agent_write_in(workdir, root, rel, answer, **session_args):
    """As _agent_write, with the session opened by the caller's arguments."""
    reply = "TOOL write_file %s\nPWNED\n@@END@@TURN@@DONE wrote it" % rel
    env = _env(root, reply, "auto")
    with WpeSession(str(workdir), "int main(void){return 0;}\n", env_extra=env,
                    **session_args) as s:
        s.key(ALT.AI); s.key("g")
        s.key("write the settings"); s.key("\r", delay=1.0)
        boxed = _wait_for(s, " Write ", timeout=8)
        box = "\n".join(s.display()) if boxed else ""
        if boxed:
            s.key(answer, delay=0.8)
        s._drain(1.5)
        txt = _wait_run_done(root)
    return box, txt


# @scenario SC-ai-agent-77
def test_the_logins_link_a_test_asks_for_is_outside_and_asks(tmp_path, monkeypatch):
    marker = _fake_home(tmp_path, monkeypatch)
    ws = tmp_path / "ws"
    ws.mkdir()
    box, txt = _agent_write_in(ws, tmp_path, WRITE_SETTINGS, "n", real_login=True)
    assert os.path.islink(str(ws / ".claude")), "real_login did not link the login in"
    _assert_asked(box, txt, WRITE_SETTINGS)
    assert marker.read_text() == MARKER, "a write through the login link landed"


# @scenario SC-ai-agent-78
def test_a_session_has_no_link_to_the_machines_login_unless_it_asks(tmp_path, monkeypatch):
    marker = _fake_home(tmp_path, monkeypatch)
    ws = tmp_path / "ws"
    ws.mkdir()
    box, txt = _agent_write_in(ws, tmp_path, WRITE_SETTINGS, "n")
    for name in (".claude", ".claude.json"):
        assert not os.path.lexists(str(ws / name)), \
            "the session's HOME holds a link to the machine's %s" % name
    assert marker.read_text() == MARKER, "a mock write reached the machine's home"
