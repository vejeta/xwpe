"""What the model reads when a tool meets a name that is not a plain file.

The model names the file; the editor judges the place the name leads to, once,
and reads that place through the read owner.  The rows hold three things:

  - a pipe under the name is answered with what it is, for read_file and for
    apply_patch, and the run goes on (it used to wait for a writer, and
    apply_patch called the pipe "file not found");
  - a link retargeted between the scope check and the read is not followed:
    the model reads the place that was judged, never one nobody judged.

Run: WPE_BIN=$PWD/wpe pytest tests/test_ai_read_tools.py (as wpe, never we).
"""
import os

import pytest

from test_ai_read_scope import Tree, SECRET, _agent_run, _ai_build
from test_ai_changeset import _auto_run, _revert_all, _trace_text, WRITE_REPLY
from wpe_driver import pane_text_with_history

pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")

PATCH = "TOOL apply_patch %s\n<<<<<<< SEARCH\nAAA\n=======\nBBB\n>>>>>>> REPLACE\n@@END"
INSIDE_TEXT = "INSIDE-TEXT"      # what Tree writes into ws/in.txt
SWAP_HOOK = {"XWPE_TEST_FAIL": "ai-read-swap-after-gate"}


def _pipe_in_workspace(tree):
    path = tree.ws / "p.txt"
    os.mkfifo(str(path))
    return path


# @scenario SC-read-owner-32
def test_read_file_of_a_pipe_is_answered_with_what_it_is(tmp_path):
    tree = Tree(tmp_path)
    path = _pipe_in_workspace(tree)
    server, boxes, screen, trace, results = _agent_run(
        tree, ["TOOL read_file %s" % path], "auto", [])
    assert server.done(), "the run stopped on the pipe:\n" + screen
    assert results and "it is a pipe" in results[-1], results


# @scenario SC-read-owner-33
def test_apply_patch_of_a_pipe_does_not_stop_the_run(tmp_path):
    tree = Tree(tmp_path)
    path = _pipe_in_workspace(tree)
    server, boxes, screen, trace, results = _agent_run(
        tree, [PATCH % path], "auto", [])
    assert server.done(), "the run stopped on the pipe:\n" + screen


# @scenario SC-read-owner-34
def test_apply_patch_of_a_pipe_names_the_pipe(tmp_path):
    tree = Tree(tmp_path)
    path = _pipe_in_workspace(tree)
    server, boxes, screen, trace, results = _agent_run(
        tree, [PATCH % path], "auto", [])
    assert server.done(), screen
    assert results and "it is a pipe" in results[-1], results


# @scenario SC-read-owner-35
def test_a_link_retargeted_after_the_scope_check_is_not_followed(tmp_path):
    """ws/l.txt leads to ws/in.txt when the scope is checked; a program
    racing the editor then points it at a file outside.  The model reads the
    file that was judged, never the one the link leads to now."""
    tree = Tree(tmp_path)
    link = tree.ws / "l.txt"
    os.symlink(str(tree.ws / "in.txt"), str(link))
    env = dict(SWAP_HOOK)
    env.update({"XWPE_TEST_SWAP_DIR": str(link),
                "XWPE_TEST_SWAP_TO": str(tree.out / "a.txt")})
    server, boxes, screen, trace, results = _agent_run(
        tree, ["TOOL read_file %s" % link], "auto", [], env_extra=env)
    assert server.done(), screen
    assert results, "the model read nothing"
    assert SECRET not in results[-1], "the link was followed to the outside file:\n%s" % results


def _sub_swap(tree):
    """ws/sub holds a.txt; the hook swaps ws/sub for a link to the outside
    folder after the scope check.  Returns (ws/sub, the editor's environment)."""
    sub = tree.ws / "sub"
    sub.mkdir()
    (sub / "a.txt").write_text("SUB-INSIDE\n")
    return sub, {"XWPE_TEST_FAIL": "ai-read-swap-after-gate",
                 "XWPE_TEST_SWAP_DIR": str(sub), "XWPE_TEST_SWAP_TO": str(tree.out)}


# @scenario SC-read-owner-37
def test_read_file_under_a_folder_swapped_after_the_gate_reads_nothing_outside(tmp_path):
    """The folder on the way to the file is swapped for a link to the outside
    after the scope check.  The place judged is no longer the place the name
    leads to, so nothing is read and the model is told the place changed."""
    tree = Tree(tmp_path)
    sub, env = _sub_swap(tree)
    server, boxes, screen, trace, results = _agent_run(
        tree, ["TOOL read_file %s" % (sub / "a.txt")], "auto", [], env_extra=env)
    assert server.done(), screen
    assert results and SECRET not in results[-1] and "place changed" in results[-1], results


# @scenario SC-read-owner-38
def test_apply_patch_under_a_folder_swapped_after_the_gate_touches_nothing_outside(tmp_path):
    """The same swap for apply_patch: the outside file is neither read nor
    patched, and the model is told the place changed."""
    tree = Tree(tmp_path)
    sub, env = _sub_swap(tree)
    server, boxes, screen, trace, results = _agent_run(
        tree, [PATCH % (sub / "a.txt")], "auto", [], env_extra=env)
    assert server.done(), screen
    assert SECRET in (tree.out / "a.txt").read_text(), "the outside file was patched"
    assert results and "place changed" in results[-1], results


# @scenario SC-read-owner-39
def test_a_checkpoint_walk_does_not_enter_a_folder_swapped_for_a_link(tmp_path):
    """The checkpoint walks the workspace.  A folder it found is swapped for a
    link to the outside before the walk enters it; the walk refuses the link
    and the trace says so, instead of listing the outside files as the
    workspace's."""
    tree = Tree(tmp_path)
    sub = tree.ws / "sub"
    sub.mkdir()
    (sub / "inner.txt").write_text("INNER\n")
    env = {"XWPE_TEST_FAIL": "ai-walk-swap-before-enter",
           "XWPE_TEST_SWAP_DIR": str(sub), "XWPE_TEST_SWAP_TO": str(tree.out)}
    server, boxes, screen, trace, results = _agent_run(
        tree, ["TOOL read_file %s" % (tree.ws / "in.txt")], "auto", [], env_extra=env)
    assert server.done(), screen
    assert "walk refused a folder that became a link" in trace, trace


# @scenario SC-read-owner-41
def test_a_folder_the_walk_refused_keeps_its_files_on_revert(tmp_path):
    """sub/inner.txt was there before the run.  The checkpoint's walk finds
    sub as a folder, a program swaps it for a link before the walk enters it,
    and a command of the run puts the folder back.  The walk could not read it,
    so nobody can say its files are the run's: the review does not call
    sub/inner.txt new, the revert leaves it, and the pane says the folder was
    not watched."""
    sub = tmp_path / "sub"
    sub.mkdir()
    (sub / "inner.txt").write_text("USER FILE\n")
    outside = tmp_path.parent / (tmp_path.name + ".outside")
    outside.mkdir()
    env = {"XWPE_TEST_FAIL": "ai-walk-swap-before-enter",
           "XWPE_TEST_SWAP_DIR": str(sub), "XWPE_TEST_SWAP_TO": str(outside)}
    # sub.orig is made non-empty again so the hook cannot swap a second time
    restore = ("TOOL run_command rm sub && mv sub.orig sub && mkdir sub.orig && "
               "touch sub.orig/keep@@TURN@@")
    with _auto_run(tmp_path, "snapshot", restore + WRITE_REPLY % (tmp_path / "t.c"),
                   env_extra=env) as (s, t, trace):
        assert "walk refused a folder that became a link" in _trace_text(trace)
        _revert_all(s, trace)
        pane = "\n".join(pane_text_with_history(s, ["not watched", "it is a link"]))
    assert (sub / "inner.txt").exists(), "the revert removed a file that was there before the run"
    assert "not watched" in pane and "it is a link" in pane, pane


# @scenario SC-read-owner-42
@pytest.mark.skipif(os.geteuid() == 0, reason="root reads a folder with no permissions")
def test_a_folder_the_walk_could_not_read_keeps_its_files_on_revert(tmp_path):
    """The same guard for a folder the walk may not read: sub/inner.txt was
    there, the folder had no permissions at the checkpoint and a command of the
    run gave them back.  Nobody saw its files at the checkpoint, so the revert
    leaves them, and the pane names the folder."""
    sub = tmp_path / "sub"
    sub.mkdir()
    (sub / "inner.txt").write_text("USER FILE\n")
    sub.chmod(0)
    giveback = "TOOL run_command chmod 755 sub@@TURN@@"
    try:
        with _auto_run(tmp_path, "snapshot", giveback + WRITE_REPLY % (tmp_path / "t.c")) \
                as (s, t, trace):
            _revert_all(s, trace)
            pane = "\n".join(pane_text_with_history(s, ["not watched"]))
    finally:
        sub.chmod(0o755)
    assert (sub / "inner.txt").exists(), "the revert removed a file that was there before the run"
    assert "not watched" in pane, pane
