"""Contracts of the post-edit hook (AIEditHook) and of Ctrl-U after an assistant change.

Three behaviours a user relies on once a hook such as "clang-format -i" is set:

- an agent write followed by the hook's rewrite is ONE Ctrl-U step, and the
  pane says the undo is "buffer only" (the disk keeps the agent's text until
  Save or a changeset revert);
- the hook also runs on a file changed by an accepted Edit, with the same single
  undo step, and without writing the file to disk before Save;
- the path reaches the hook intact (quoted), a non-zero exit is reported with
  its status, and the buffer is not reloaded from a failed hook.

The hook here is a small shell script that appends a marker to the path it is
given, so every test can see whether it ran and on which file.  The mock
backend drives the agent and the Edit; no model or network is involved.
"""
import os
import stat
import time
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

CTRL_U = "\x15"
ESC = "\033"
MARK = "HOOKRAN99"
SEED = "int main(void){return 0;}\n"
AGENT_TEXT = "int main(void){return 1;}\n"
EDIT_ORIG = "int a = 1;\nint b = 2;\nint c = 3;\n"
EDIT_NEW = "int a = 1;\nint b = 22;\nint c = 3;\n"
REVIEW_BOX = "Proposed change"


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _hook_script(tmp_path, body):
    """Write an executable hook that receives the file path as $1."""
    script = tmp_path / "hook.sh"
    script.write_text("#!/bin/sh\n" + body + "\n")
    script.chmod(script.stat().st_mode | stat.S_IXUSR)
    return str(script)


def _home_with_hook(tmp_path, hook_cmd):
    """A HOME whose xwperc sets AIEditHook to `hook_cmd`."""
    home = str(tmp_path / "home")
    cfg = os.path.join(home, ".config", "xwpe")
    os.makedirs(cfg)
    with open(os.path.join(cfg, "xwperc"), "w") as fh:
        fh.write("[Programming]\nAIEditHook : %s\n" % hook_cmd)
    return home


def _trace(tmp_path, name):
    p = tmp_path / name
    return p.read_text() if p.exists() else ""


def _wait_trace(s, tmp_path, name, needle, tmo=15):
    end = time.time() + tmo
    while time.time() < end:
        s._drain(0.5)
        if needle in _trace(tmp_path, name):
            return True
    return False


def _wait_screen(s, needle, tmo=12):
    end = time.time() + tmo
    while time.time() < end:
        s._drain(0.4)
        if needle in "\n".join(s.display()):
            return True
    return False


def _agent_env(home, filename, content, trace):
    turns = ("TOOL write_file %s\n%s@@END@@TURN@@DONE wrote it" % (filename, content))
    return {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_POLICY": "auto",
            "XWPE_AI_MOCK_REPLY": turns, "XWPE_AI_TRACE": str(trace), "HOME": home}


def _run_agent_write(s, tmp_path, trace_name):
    """Ask the agent to rewrite the file, wait for the end-of-run changeset,
    keep everything, and leave the pane so the file window has the focus."""
    s.key(ALT.AI); s.key("g"); s._drain(0.5)
    s.key("rewrite the file"); s.key("\r", delay=0.8)
    assert _wait_trace(s, tmp_path, trace_name, "changeset"), \
        "the agent run did not reach the changeset:\n" + _trace(tmp_path, trace_name)
    s.key("a", delay=0.6)                      # keep all -> leave the review
    s.key(ESC, delay=0.6)                      # leave the pane: focus back on the file
    s._drain(0.5)


def _edit_env(home, reply, trace):
    return {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
            "XWPE_AI_MOCK_REPLY": reply, "XWPE_AI_TRACE": str(trace), "HOME": home}


def _accept_edit(s, tmp_path, trace_name):
    s.key(ALT.AI); s.key("e"); s._drain(0.4)
    s.key("change b"); s.key("\r", delay=0.8)
    assert _wait_screen(s, REVIEW_BOX), "no review box"
    s.key("y", delay=0.7)
    assert _wait_trace(s, tmp_path, trace_name, "edit applied", tmo=10), \
        "the accepted edit was not applied:\n" + _trace(tmp_path, trace_name)
    s._drain(0.5)


# @scenario SC-ai-edit-48
def test_agent_write_with_hook_is_one_undo_step_and_disk_keeps_the_write(tmp_path):
    hook = _hook_script(tmp_path, 'printf %s >> "$1"' % MARK)
    home = _home_with_hook(tmp_path, hook)
    env = _agent_env(home, "t.c", AGENT_TEXT, tmp_path / "a.trace")
    with WpeSession(str(tmp_path), SEED, env_extra=env, filename="t.c") as s:
        _run_agent_write(s, tmp_path, "a.trace")
        shown = "\n".join(s.display())
        assert "return 1;" in shown, "the agent write did not reach the buffer:\n" + shown
        assert MARK in shown, "the hook's rewrite did not reach the buffer:\n" + shown
        s.key(CTRL_U, delay=0.6); s._drain(0.6)
        undone = "\n".join(s.display())
        assert "return 0;" in undone, \
            "one Ctrl-U did not restore the text before the agent write:\n" + undone
        assert "return 1;" not in undone and MARK not in undone, \
            "one Ctrl-U left part of the write or of the hook's rewrite:\n" + undone
    with open(os.path.join(str(tmp_path), "t.c")) as fh:
        disk = fh.read()
    assert "return 1;" in disk and MARK in disk, \
        "Ctrl-U must leave the disk with the agent's text and the hook's rewrite:\n" + disk


# @scenario SC-ai-edit-48
def test_agent_write_pane_says_the_undo_is_buffer_only(tmp_path):
    hook = _hook_script(tmp_path, 'printf %s >> "$1"' % MARK)
    home = _home_with_hook(tmp_path, hook)
    env = _agent_env(home, "t.c", AGENT_TEXT, tmp_path / "a.trace")
    with WpeSession(str(tmp_path), SEED, env_extra=env, filename="t.c") as s:
        _run_agent_write(s, tmp_path, "a.trace")
        shown = "\n".join(s.display())
        assert "Ctrl-U to undo (buffer only)" in shown, \
            "the pane did not say the undo is buffer only:\n" + shown


# @scenario SC-ai-edit-50
def test_accepted_edit_runs_the_hook_and_one_undo_reverts_both(tmp_path):
    hook = _hook_script(tmp_path, 'printf %s >> "$1"' % MARK)
    home = _home_with_hook(tmp_path, hook)
    env = _edit_env(home, EDIT_NEW, tmp_path / "e.trace")
    with WpeSession(str(tmp_path), EDIT_ORIG, env_extra=env, filename="t.c") as s:
        _accept_edit(s, tmp_path, "e.trace")
        shown = "\n".join(s.display())
        assert "int b = 22;" in shown, "the accepted hunk did not reach the buffer:\n" + shown
        assert MARK in shown, "the hook did not run on the accepted edit:\n" + shown
        with open(s.path, "rb") as fh:
            assert fh.read() == EDIT_ORIG.encode("utf-8"), "disk changed before Save"
        leftovers = [n for n in os.listdir(str(tmp_path)) if "hook" in n and n != "hook.sh"]
        assert leftovers == [], "the hook left a temporary copy behind: %r" % (leftovers,)
        s.key(CTRL_U, delay=0.6); s._drain(0.6)
        undone = "\n".join(s.display())
        assert "int b = 2;" in undone, \
            "one Ctrl-U did not restore the text before the edit:\n" + undone
        assert "int b = 22;" not in undone and MARK not in undone, \
            "one Ctrl-U left part of the edit or of the hook's rewrite:\n" + undone


# @scenario SC-ai-edit-51
def test_hook_receives_a_path_with_spaces_intact(tmp_path):
    hook = _hook_script(tmp_path, 'printf %s >> "$1"' % MARK)
    home = _home_with_hook(tmp_path, hook)
    env = _agent_env(home, "a b.c", AGENT_TEXT, tmp_path / "a.trace")
    with WpeSession(str(tmp_path), SEED, env_extra=env, filename="a b.c") as s:
        _run_agent_write(s, tmp_path, "a.trace")
    with open(os.path.join(str(tmp_path), "a b.c")) as fh:
        disk = fh.read()
    assert MARK in disk, "the hook did not touch the file named 'a b.c':\n" + disk
    assert not os.path.exists(os.path.join(str(tmp_path), "a")), \
        "the hook was handed the path split at the space (a file named 'a' appeared)"


# @scenario SC-ai-edit-51
def test_failed_hook_is_reported_with_its_status_and_the_buffer_is_not_reloaded(tmp_path):
    hook = _hook_script(tmp_path, 'printf %s >> "$1"; exit 3' % MARK)
    home = _home_with_hook(tmp_path, hook)
    env = _agent_env(home, "t.c", AGENT_TEXT, tmp_path / "a.trace")
    with WpeSession(str(tmp_path), SEED, env_extra=env, filename="t.c") as s:
        _run_agent_write(s, tmp_path, "a.trace")
        shown = "\n".join(s.display())
        assert "status 3" in shown, "the pane did not report the hook's exit status:\n" + shown
        assert "return 1;" in shown, "the agent write did not reach the buffer:\n" + shown
        assert MARK not in shown, \
            "the buffer was reloaded from the output of a failed hook:\n" + shown
