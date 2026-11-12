"""A project folder cannot make the assistant run code behind the user's back.

Two ways a folder the user opens, a cloned repository or one the assistant
itself wrote into, used to reach past the permission dial:

* An ``xwperc`` in the folder the editor is started in is read as the config
  file.  It could name a post-edit hook, a hosted engine command, the dial,
  the web switch, the endpoints and the folders the assistant may read.
  Those keys are now taken only from the user's own config file and from the
  environment; the folder's file keeps every other key, and one status line
  names the keys it could not set.
* Files inside the workspace that git, the editor or the hosted engine run or
  obey (``.git/**``, ``xwperc``, ``.xwpe-ai.md``, ``AGENTS.md``,
  ``.claude/**``, ``.mcp.json``, ``.qwen/**``) were ordinary workspace files,
  written without a question under ``auto``.  A write there now asks at
  every dial, and the box says why.
"""
import os
import time

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN
from test_ai_agent_tool_safety import _ai_build, _env, _trace, _wait_for
from test_ai_read_scope import Tree as ScopeTree, _host_perm, needs_host

pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")

IGNORED_LINE = "./xwperc may not set"
PROTECTED_WORD = "changes how git, the editor or the engine runs code"
RUN_DONE = "agent done"
WAIT_RUN_SECONDS = 20
SEED = "int main(void){return 0;}\n"
EDIT_REPLY = ("TOOL write_file t.c\nint main(void){return 1;}\n@@END"
              "@@TURN@@DONE wrote it")


def _wait_run_done(tmp_path):
    """Wait until the trace says the run ended; return the trace."""
    end = time.time() + WAIT_RUN_SECONDS
    while time.time() < end:
        txt = _trace(tmp_path)
        if RUN_DONE in txt or "changeset" in txt:
            return txt
        time.sleep(0.3)
    return _trace(tmp_path)


def _project(tmp_path, xwperc_text):
    """A project folder holding an xwperc, and a HOME of the user's own beside it."""
    ws = tmp_path / "ws"
    home = tmp_path / "home"
    ws.mkdir()
    home.mkdir()
    (ws / "xwperc").write_text(xwperc_text)
    return ws, home


def _user_config(home, text):
    """The user's own config file, under the HOME the session runs with."""
    cfg = home / ".config" / "xwpe"
    cfg.mkdir(parents=True, exist_ok=True)
    (cfg / "xwperc").write_text(text)


def _agent_edit(ws, home, tmp_path):
    """Start in `ws`, take the screen as it opens, and have the agent rewrite
    t.c under auto through the mock backend.  Returns (the first screen, the
    text of t.c after the run)."""
    env = _env(tmp_path, EDIT_REPLY, "auto")
    env["HOME"] = str(home)
    with WpeSession(str(ws), SEED, env_extra=env, filename="t.c") as s:
        first = "\n".join(s.display())
        s.key(ALT.AI); s.key("g"); s._drain(0.5)
        s.key("rewrite t.c"); s.key("\r", delay=0.8)
        _wait_run_done(tmp_path)
        s.key("a", delay=0.5)                      # keep all, leave the review
        s._drain(0.5)
    return first, (ws / "t.c").read_text()


# @scenario SC-ai-agent-104
def test_a_start_folder_xwperc_cannot_set_the_post_edit_hook(tmp_path):
    """The folder's xwperc names `touch <marker>` as the post-edit hook: the
    agent's edit lands, the hook does not run, and the status line the user
    sees at start names the key."""
    marker = tmp_path / "pwned"
    ws, home = _project(tmp_path, "[Programming]\nAIEditHook : touch %s\n" % marker)
    first, text = _agent_edit(ws, home, tmp_path)
    assert "return 1;" in text, "the agent's edit did not land:\n" + text
    assert not marker.exists(), "a hook named by the start folder's xwperc ran"
    bottom = first.splitlines()[-1]
    assert IGNORED_LINE in bottom and "AIEditHook" in bottom, \
        "no status line names the key the folder could not set:\n" + first


# @scenario SC-ai-agent-104
def test_the_users_own_hook_still_runs_in_a_folder_with_its_own_xwperc(tmp_path):
    """The user's own config sets the hook; the folder's xwperc sets only a
    tab width.  The user's hook runs there as everywhere, and no status line
    complains."""
    marker = tmp_path / "ownhook"
    ws, home = _project(tmp_path, "[General]\nTab : 3\n")
    _user_config(home, "[Programming]\nAIEditHook : touch %s\n" % marker)
    first, text = _agent_edit(ws, home, tmp_path)
    assert "return 1;" in text, "the agent's edit did not land:\n" + text
    assert marker.exists(), "the user's own hook did not run in a folder with an xwperc"
    assert IGNORED_LINE not in first, first


# @scenario SC-ai-agent-104
def test_a_dial_change_in_a_folder_with_its_own_xwperc_goes_to_the_users_file(tmp_path):
    """Alt-G y saves the options at once.  In a folder whose xwperc is the
    option file, the new dial goes to the user's own file, where the next
    start reads it, and the user's own hook is not copied into the project's
    file; the folder's own tab width stays in the folder's file."""
    marker = tmp_path / "ownhook"
    ws, home = _project(tmp_path, "[General]\nTab : 3\n")
    _user_config(home, "[General]\nAutosave : 7\n[Programming]\nAIEditHook : touch %s\n" % marker)
    env = _env(tmp_path, "DONE ok", "ask")
    del env["XWPE_AI_POLICY"]
    env["HOME"] = str(home)
    with WpeSession(str(ws), SEED, env_extra=env, filename="t.c") as s:
        s.key(ALT.AI); s.key("y", delay=0.8)
        s.key("\033[B", delay=0.6)
    folder = (ws / "xwperc").read_text()
    own = (home / ".config" / "xwpe" / "xwperc").read_text()
    assert "AIEditHook" not in folder and "AIPolicy" not in folder, \
        "the user's own keys were written into the project's xwperc:\n" + folder
    assert "Tab : 3" in folder, "the folder's own key was lost:\n" + folder
    assert "AIPolicy : edits" in own, "the new dial did not reach the user's file:\n" + own
    assert "AIEditHook : touch %s" % marker in own, "the user's hook was lost:\n" + own
    assert "Autosave : 7" in own, "the user's other keys were not kept:\n" + own


# @scenario SC-ai-agent-104
def test_a_save_leaves_the_users_keys_out_of_the_folders_file_when_his_own_cannot_be_written(tmp_path):
    """The user's own config cannot be written (read-only).  Alt-G y saves
    the options: the dial and his hook still leave the project's xwperc,
    and the status line says they were not saved."""
    marker = tmp_path / "ownhook"
    ws, home = _project(tmp_path, "[General]\nTab : 3\n")
    _user_config(home, "[Programming]\nAIEditHook : touch %s\n" % marker)
    own = home / ".config" / "xwpe" / "xwperc"
    own.chmod(0o400)
    env = _env(tmp_path, "DONE ok", "ask")
    del env["XWPE_AI_POLICY"]
    env["HOME"] = str(home)
    env["XWPE_UI_TRACE"] = str(tmp_path / "ui.trace")
    try:
        with WpeSession(str(ws), SEED, env_extra=env, filename="t.c") as s:
            s.key(ALT.AI); s.key("y", delay=0.8)
            s.key("\033[B", delay=0.6)
    finally:
        own.chmod(0o600)
    folder = (ws / "xwperc").read_text()
    assert "AIEditHook" not in folder and "AIPolicy" not in folder, \
        "the user's own keys stayed in the project's xwperc:\n" + folder
    assert "Tab : 3" in folder, "the folder's own key was lost:\n" + folder
    said = (tmp_path / "ui.trace").read_text()
    assert "could not write your own config" in said, said


# @scenario SC-ai-agent-104
def test_save_options_as_a_third_file_writes_every_key_and_leaves_the_users_file(tmp_path):
    """Options > Save Options into a file of the user's choosing, while the
    options come from the folder's xwperc: that file gets every key, his
    hook included, and his own config is not rewritten."""
    marker = tmp_path / "ownhook"
    ws, home = _project(tmp_path, "[General]\nTab : 3\n")
    _user_config(home, "[Programming]\nAIEditHook : touch %s\n" % marker)
    own = home / ".config" / "xwpe" / "xwperc"
    before = own.read_text()
    if len(str(ws / "xwperc")) > SAVE_FIELD_ROOM:
        pytest.skip("the folder's xwperc path is longer than the Save field "
                    "(%d bytes): Save Options then shows it whole and offers "
                    "no other name (tests/test_startcmd.py)" % len(str(ws / "xwperc")))
    third = ws / "third.rc"
    env = {"HOME": str(home)}
    with WpeSession(str(ws), SEED, env_extra=env, filename="t.c") as s:
        s.key(ALT.OPTIONS); s.key("s", delay=0.8)
        s.key("\033[F", delay=0.4)                       # End of the field
        s.key("\x7f" * (len(str(ws / "xwperc")) + 8), delay=0.8)
        s.key(third.name, delay=0.6)
        typed = "\n".join(s.display())
        s.key("\r", delay=1.0)
    assert third.exists(), "Save Options did not write the file named:\n" + typed
    saved = third.read_text()
    assert "AIEditHook : touch %s" % marker in saved, \
        "the file the user saved to lost his hook:\n" + saved
    assert "Tab : 3" in saved, saved
    assert own.read_text() == before, "the user's own config was rewritten by a Save As elsewhere"


def _x_column(ws, home, seed):
    """The screen column of the X in `seed`'s first line, as the editor shows it."""
    with WpeSession(str(ws), seed, env_extra={"HOME": str(home)}, filename="t.c") as s:
        rows = s.display()
    for row in rows[1:-1]:                       # below the menu bar, above the status row
        if "X" in row:
            return row.index("X")
    return -1


# @scenario SC-ai-agent-104
def test_a_start_folder_xwperc_still_sets_ordinary_keys(tmp_path):
    """The same folder file that names a hook also sets a tab width: the tab
    width applies, as it always did."""
    seed = "\tX\n"
    plain = tmp_path / "plain"
    plain_home = tmp_path / "plain_home"
    plain.mkdir()
    plain_home.mkdir()
    default_col = _x_column(plain, plain_home, seed)
    ws, home = _project(tmp_path, "[General]\nTab : 3\n[Programming]\nAIEditHook : true\n")
    narrow_col = _x_column(ws, home, seed)
    assert default_col > 0 and narrow_col > 0, (default_col, narrow_col)
    assert narrow_col < default_col, \
        "the folder's Tab : 3 did not apply (X at %d, default %d)" % (narrow_col, default_col)


def _agent_write_protected(tmp_path, rel, answer):
    """Under auto, have the agent write `rel` below a fresh workspace and
    answer the write box with `answer`.  Returns (the box screen or "", the
    workspace, the trace)."""
    ws = tmp_path / "ws"
    ws.mkdir()
    target = ws / rel
    target.parent.mkdir(parents=True, exist_ok=True)
    reply = "TOOL write_file %s\nplanted\n@@END@@TURN@@DONE wrote it" % target
    env = _env(tmp_path, reply, "auto")
    with WpeSession(str(ws), SEED, env_extra=env) as s:
        s.key(ALT.AI); s.key("g")
        s.key("write the file"); s.key("\r", delay=1.0)
        boxed = _wait_for(s, " Write ", timeout=8)
        box = "\n".join(s.display()) if boxed else ""
        if boxed:
            s.key(answer, delay=0.8)
        s._drain(1.5)
        txt = _wait_run_done(tmp_path)
    return box, target, txt


# @scenario SC-ai-agent-105
@pytest.mark.parametrize("rel", [".git/hooks/pre-commit", ".claude/settings.json",
                                 "CLAUDE.md", "QWEN.md", ".config/xwpe/xwperc"])
def test_under_auto_a_write_that_runs_code_asks_and_n_leaves_nothing(tmp_path, rel):
    """The session's HOME is the workspace, so `.config/xwpe/xwperc` is the
    user's own config file: the one file trusted with the hook and the dial."""
    box, target, txt = _agent_write_protected(tmp_path, rel, "n")
    assert " Write " in box, "no write box under auto for %s:\n%s" % (rel, box)
    assert PROTECTED_WORD in box, "the box does not say why it asks:\n" + box
    assert "agent write protected %s" % target in txt, txt
    assert not target.exists(), "a refused write to %s landed" % rel


def _agent_write_from(tmp_path, start, filename, target, answer):
    """Start the editor in `start` on `filename`, have the agent write
    `target` under auto and answer the write box with `answer`.  HOME is a
    folder of its own.  Returns (the box screen or "", the trace)."""
    home = tmp_path / "home"
    home.mkdir(exist_ok=True)
    target.parent.mkdir(parents=True, exist_ok=True)
    reply = "TOOL write_file %s\nplanted\n@@END@@TURN@@DONE wrote it" % target
    env = _env(tmp_path, reply, "auto")
    env["HOME"] = str(home)
    with WpeSession(str(start), SEED, env_extra=env, filename=filename) as s:
        s.key(ALT.AI); s.key("g")
        s.key("write the file"); s.key("\r", delay=1.0)
        boxed = _wait_for(s, " Write ", timeout=8)
        box = "\n".join(s.display()) if boxed else ""
        if boxed:
            s.key(answer, delay=0.8)
        s._drain(1.5)
        txt = _wait_run_done(tmp_path)
    return box, txt


# @scenario SC-ai-agent-105
def test_a_protected_name_in_the_start_folder_below_the_workspace_asks(tmp_path):
    """Started in proj/src, editing proj/t.c: the workspace is proj, and the
    editor reads ./xwperc and the assistant ./.xwpe-ai.md from proj/src.  An
    auto write of proj/src/.xwpe-ai.md asks, as it would at proj's top."""
    proj = tmp_path / "proj"
    src = proj / "src"
    src.mkdir(parents=True)
    target = src / ".xwpe-ai.md"
    box, txt = _agent_write_from(tmp_path, src, "../t.c", target, "n")
    assert " Write " in box, "no write box for the start folder's instructions:\n" + box
    assert PROTECTED_WORD in box, "the box does not say why it asks:\n" + box
    assert not target.exists(), "a refused write to the start folder's instructions landed"


F3 = "\033OR"              # the File-Manager
SAVE_FIELD_ROOM = 128      # the name field of Options > Save Options


def _editor_cwd(s):
    """The working folder of the editor this session started."""
    return os.readlink("/proc/%d/cwd" % s.proc.pid)


# @scenario SC-ai-agent-105
@pytest.mark.skipif(not os.path.isdir("/proc/self"), reason="needs /proc to see the editor's folder")
def test_the_start_folder_stays_protected_after_the_file_manager_moves_the_editor(tmp_path):
    """Started in proj/src, the user opens proj/u.c through the File-Manager,
    which moves the editor's working folder to proj.  The start folder's
    instructions file, read from proj/src at start, still asks under auto."""
    proj = tmp_path / "proj"
    src = proj / "src"
    src.mkdir(parents=True)
    (proj / "u.c").write_text(SEED)
    home = tmp_path / "home"
    home.mkdir()
    target = src / ".xwpe-ai.md"
    reply = "TOOL write_file %s\nplanted\n@@END@@TURN@@DONE wrote it" % target
    env = _env(tmp_path, reply, "auto")
    env["HOME"] = str(home)
    with WpeSession(str(src), SEED, env_extra=env, filename="../t.c") as s:
        s.key(F3, delay=1.0)
        s.key("../u.c", delay=0.6)
        s.key("\r", delay=1.2)                     # the manager moves to proj
        s.key("\r", delay=1.2)                     # and opens u.c there
        moved = _editor_cwd(s)
        opened = "\n".join(s.display())
        assert " u.c q" in opened, "u.c did not open:\n" + opened
        s.key(ALT.AI); s.key("g")
        s.key("write the file"); s.key("\r", delay=1.0)
        boxed = _wait_for(s, " Write ", timeout=8)
        box = "\n".join(s.display()) if boxed else ""
        if boxed:
            s.key("n", delay=0.8)
        s._drain(1.5)
        _wait_run_done(tmp_path)
    assert os.path.realpath(moved) == os.path.realpath(str(proj)), \
        "the File-Manager did not move the editor (it is in %s): the row proves nothing" % moved
    assert PROTECTED_WORD in box, "the start folder's instructions were written unasked:\n" + box
    assert not target.exists(), "a refused write to the start folder's instructions landed"


# @scenario SC-ai-agent-105
@pytest.mark.parametrize("rel", [".husky/pre-commit", ".pre-commit-config.yaml",
                                 "tools/hooks/pre-commit"])
def test_under_auto_a_hook_a_commit_framework_runs_asks(tmp_path, rel):
    """husky's hook folder, pre-commit's config, and the folder the
    repository's core.hooksPath names (read from .git/config, no git run)
    make git run a program on the next commit: each auto write asks."""
    ws = tmp_path / "ws"
    (ws / ".git").mkdir(parents=True)
    (ws / ".git" / "config").write_text("[core]\n\thooksPath = tools/hooks\n")
    target = ws / rel
    box, txt = _agent_write_from(tmp_path, ws, "t.c", target, "n")
    assert " Write " in box, "no write box under auto for %s:\n%s" % (rel, box)
    assert PROTECTED_WORD in box, "the box does not say why it asks:\n" + box
    assert not target.exists(), "a refused write to %s landed" % rel


# @scenario SC-ai-agent-105
def test_under_auto_a_write_to_the_folders_xwperc_lands_after_yes(tmp_path):
    box, target, txt = _agent_write_protected(tmp_path, "xwperc", "y")
    assert PROTECTED_WORD in box, "no marked box for the folder's xwperc:\n" + box
    assert target.read_text() == "planted\n", "the allowed write did not land"


# @scenario SC-ai-agent-105
def test_a_multi_file_proposal_for_a_git_hook_is_marked_in_the_approval_box(tmp_path):
    """The Multi-file edit always asks; its row for a git hook carries the
    same mark as a file outside the workspace, so it is not approved as an
    ordinary change."""
    ws = tmp_path / "ws"
    (ws / ".git" / "hooks").mkdir(parents=True)
    plan = "PROPOSE .git/hooks/pre-commit\nplanted\n@@END\n@@PLAN-DONE add a hook\n"
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_MOCK_REPLY": plan,
           "XWPE_AI_TRACE": str(tmp_path / "ai.trace")}
    with WpeSession(str(ws), SEED, env_extra=env, filename="t.c") as s:
        s.key(ALT.AI); s.key("f"); s._drain(0.5)
        s.key("add a hook"); s.key("\r", delay=0.8)
        assert _wait_for(s, "Multi-file edit -", timeout=12), "the approval box did not open"
        rows = [r for r in s.display() if "pre-commit" in r and "(+" in r]
        s.key("\033", delay=0.8)
    assert rows, "the proposal row is missing"
    assert "! .git/hooks/pre-commit" in rows[0], "the row carries no mark:\n" + rows[0]
    assert "changes how git" in rows[0], "the row does not say why:\n" + rows[0]
    assert not (ws / ".git" / "hooks" / "pre-commit").exists(), "the cancelled hook landed"


# @scenario SC-ai-agent-106
@needs_host
def test_a_hosted_write_to_a_git_hook_asks_under_auto(tmp_path):
    tree = ScopeTree(tmp_path)
    (tree.ws / ".git" / "hooks").mkdir(parents=True)
    path = str(tree.ws / ".git" / "hooks" / "pre-commit")
    screens, screen, resp, trace = _host_perm(
        tree, "Write", '{"file_path": %r}' % path, "auto", ["n"])
    assert screens and " Write " in screens[0], \
        "no write box for a hosted Write to a git hook under auto:\n" + screen
    assert PROTECTED_WORD in screens[0], screens[0]
    assert '"behavior":"deny"' in resp.replace(" ", ""), resp
    assert "host write protected Write %s -> deny" % path in trace, trace
