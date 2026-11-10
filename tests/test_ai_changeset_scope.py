"""AI assistant -- the change review holds exactly what the run changed.

The review after a run is built from a checkpoint taken before it, and `r`
puts the workspace back by it.  The files a user had lying around before the
run (notes, build output git does not ignore), the rest of a repository the
workspace is only a part of, and an outer repository that ignores the
workspace folder are none of the run's: listing them offers to revert changes
the run never made, and reverting a "new file" deletes it.  Each case below
asserts the ground truth on disk first, then what the review listed.
"""
import os
import shutil
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN
from test_ai_changeset import (_ai_build, _git, _git_commit, _wait_trace,
                               _trace_text, _messages_rows, _cursor_line,
                               TWENTY, TEN_CHANGED, CHANGED_LINE, ALT_T,
                               WIDE_COLS, WIDE_ROWS)

pytestmark = [
    pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai"),
    pytest.mark.skipif(shutil.which("git") is None, reason="no git"),
]

# names that break a line-oriented parse: a leading blank and a newline
AWKWARD_NAMES = ["notes.txt", " spaced.txt", "line\nbreak.txt"]


def _repo_with_t(root, workdir):
    """A repository at `root` whose workspace `workdir` holds a committed t.c."""
    _git(root, "init", "-q")
    workdir.mkdir(exist_ok=True)
    (workdir / "t.c").write_text(TWENTY)


def _drive(tmp_path, workdir, reply, review_keys, wait_revert=False, jump=False):
    """Run the mock assistant in `workdir`, answer the review with `review_keys`.

    Returns (Messages rows the review showed, rows after the jump or None, the
    trace text).  The trace and the state folder are kept beside `tmp_path`,
    outside every folder the test builds, so neither is counted as a change.
    """
    trace = tmp_path.parent / (tmp_path.name + ".trace")
    state = tmp_path.parent / (tmp_path.name + ".state")
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_POLICY": "auto",
        "XWPE_AI_TRACE": str(trace),
        "XDG_STATE_HOME": str(state),
    }
    jumped = None
    with WpeSession(str(workdir), TWENTY, env_extra=env,
                    cols=WIDE_COLS, rows=WIDE_ROWS) as s:
        s.key(ALT.AI)
        s.key("g")
        s.key("change it")
        s.key("\r", delay=1.0)
        assert _wait_trace(trace, "changeset n=", 60), \
            "the review never opened:\n" + _trace_text(trace)
        review = _messages_rows(s.display(drain=0))
        for k in review_keys:
            s.key(k, delay=0.8)
        if wait_revert:
            assert _wait_trace(trace, "changeset revert", 20), \
                "no revert happened:\n" + _trace_text(trace)
        if jump:
            s.key(ALT_T, delay=0.8)
            jumped = [r.rstrip() for r in s.display(drain=0)]
    return review, jumped, _trace_text(trace)


def _edit_reply(path):
    """A run that rewrites line 10 of `path`."""
    return "TOOL write_file %s\n%s@@END@@TURN@@DONE changed one line" % (path, TEN_CHANGED)


# @scenario SC-ai-agent-92
def test_untracked_files_from_before_the_run_are_not_listed_and_survive_r(tmp_path):
    """A file the user had before the run is never the run's, whatever its name.

    The workspace is a plain repository with three untracked files (one with a
    leading blank, one with a newline in its name).  The run edits t.c.  The
    review must hold t.c alone, and `r` must leave all three on disk: git
    records no untracked file in the checkpoint, so they used to be listed as
    "new file" and removed.
    """
    _repo_with_t(tmp_path, tmp_path)
    _git_commit(tmp_path, "seed")
    for name in AWKWARD_NAMES:
        (tmp_path / name).write_text("mine\n")
    t = tmp_path / "t.c"
    review, _, txt = _drive(tmp_path, tmp_path, _edit_reply(t), ["r"], wait_revert=True)
    shown = "\n".join(review)
    for name in AWKWARD_NAMES:
        assert (tmp_path / name).read_text() == "mine\n", \
            "r removed the user's file %r:\n%s" % (name, txt)
    assert t.read_text() == TWENTY, "r did not put t.c back"
    assert "changeset n=1" in txt, txt
    assert "notes.txt" not in shown and "spaced" not in shown and "break" not in shown, \
        "the review lists a file the run never touched:\n" + shown


# @scenario SC-ai-agent-93
def test_a_workspace_inside_a_repository_lists_only_its_own_changes(tmp_path):
    """A subfolder workspace: the repository's other changes are not the run's.

    The repository has a modified tracked file and an untracked file outside
    the workspace.  The run edits ws/t.c.  The review holds that one file, at
    its own path and line, the jump lands on that line, and nothing outside
    the workspace is touched.
    """
    ws = tmp_path / "ws"
    _repo_with_t(tmp_path, ws)
    (tmp_path / "outer.c").write_text("int o;\n")
    _git_commit(tmp_path, "seed")
    (tmp_path / "outer.c").write_text("int o2;\n")
    (tmp_path / "outer_new.txt").write_text("mine\n")
    t = ws / "t.c"
    review, jumped, txt = _drive(tmp_path, ws, _edit_reply(t), ["a"], jump=True)
    shown = "\n".join(review)
    assert "outer" not in shown, "the review lists the repository's own files:\n" + shown
    assert "t.c:%d: [AI] +1 -1" % CHANGED_LINE in shown, shown
    assert "changeset n=1" in txt, txt
    assert _cursor_line(jumped) == CHANGED_LINE, "\n".join(jumped)
    assert (tmp_path / "outer.c").read_text() == "int o2;\n"
    assert (tmp_path / "outer_new.txt").read_text() == "mine\n"


# @scenario SC-ai-agent-94
def test_a_workspace_in_an_ignored_folder_of_another_repository_is_not_that_repository(tmp_path):
    """The workspace is ignored by the repository around it: snapshot, not git.

    git says "inside a work tree" there yet holds nothing of the workspace, so
    the review used to list the outer repository's untracked files.  Now the
    checkpoint is the snapshot copies: the run's edit is listed, `r` puts it
    back, and the outer repository is untouched.
    """
    _git(tmp_path, "init", "-q")
    (tmp_path / ".gitignore").write_text("ign/\n")
    (tmp_path / "kept.c").write_text("int k;\n")
    _git_commit(tmp_path, "seed")
    (tmp_path / "outer_untracked.txt").write_text("mine\n")
    ws = tmp_path / "ign"
    ws.mkdir()
    (ws / "t.c").write_text(TWENTY)
    t = ws / "t.c"
    review, _, txt = _drive(tmp_path, ws, _edit_reply(t), ["r"], wait_revert=True)
    shown = "\n".join(review)
    assert "checkpoint snapshot" in txt, txt
    assert "outer_untracked" not in shown and "kept.c" not in shown, \
        "the review lists the outer repository:\n" + shown
    assert "changeset n=1" in txt, txt
    assert t.read_text() == TWENTY, "r did not put t.c back"
    assert (tmp_path / "outer_untracked.txt").read_text() == "mine\n"


# @scenario SC-ai-agent-95
def test_a_file_the_run_creates_is_listed_new_and_r_removes_only_it(tmp_path):
    """The run's own new file goes on `r`; the user's untracked file stays."""
    _repo_with_t(tmp_path, tmp_path)
    _git_commit(tmp_path, "seed")
    (tmp_path / "notes.txt").write_text("mine\n")
    made = tmp_path / "made.txt"
    reply = "TOOL write_file %s\nnew\n@@END@@TURN@@DONE made a file" % made
    review, _, txt = _drive(tmp_path, tmp_path, reply, ["r"], wait_revert=True)
    shown = "\n".join(review)
    assert "[AI] new file" in shown and "made.txt" in shown, shown
    assert "notes.txt" not in shown, shown
    assert not made.exists(), "r did not remove the file the run created"
    assert (tmp_path / "notes.txt").read_text() == "mine\n", \
        "r removed a file the run did not create"
    assert "changeset n=1" in txt, txt


def _git_status(root, *args):
    """Run git in `root` and return its exit status, without failing on it."""
    return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t",
                           "-c", "commit.gpgsign=false", *args],
                          cwd=root, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode


def _repo_in_a_merge_conflict(root):
    """A repository halfway through a merge that stopped on a conflict.

    In that state git cannot save the worktree as a stash, so the checkpoint
    has no git image of the user's uncommitted edits.
    """
    _repo_with_t(root, root)
    (root / "a.c").write_text("int a;\n")
    (root / "mine.c").write_text("int mine;\n")
    _git_commit(root, "seed")
    _git(root, "checkout", "-qb", "side")
    _git(root, "mv", "a.c", "a2.c")
    _git(root, "commit", "-qm", "side")
    _git(root, "checkout", "-q", "-")
    _git(root, "rm", "-q", "a.c")
    _git(root, "commit", "-qm", "main")
    assert _git_status(root, "merge", "side") != 0, "the merge did not stop on a conflict"


def _repo_with_a_held_index(root):
    """A repository whose index another git process holds while the checkpoint is taken."""
    _repo_with_t(root, root)
    (root / "mine.c").write_text("int mine;\n")
    _git_commit(root, "seed")
    (root / ".git" / "index.lock").write_text("")


# @scenario SC-ai-agent-96
@pytest.mark.parametrize("why", ["merge-conflict", "index-held"])
def test_an_edit_the_user_had_before_the_run_survives_r_when_git_cannot_stash(tmp_path, why):
    """git cannot save the worktree as a stash: snapshot, never HEAD.

    In the middle of a merge, or while another git process holds the index,
    `git stash create` fails.  The checkpoint used to take HEAD (or git's
    complaint) as its reference: every uncommitted edit of the user's was then
    listed as the run's and `r` checked HEAD's copy out over it, or the review
    could list nothing at all.  The run's edit must be listed and put back,
    and the user's edit to mine.c must survive.  In the held-index case the
    run's first command ends the other process's hold, as its exit would.
    """
    t = tmp_path / "t.c"
    if why == "merge-conflict":
        _repo_in_a_merge_conflict(tmp_path)
        reply = _edit_reply(t)
    else:
        _repo_with_a_held_index(tmp_path)
        reply = "TOOL run_command rm -f .git/index.lock@@TURN@@" + _edit_reply(t)
    mine = tmp_path / "mine.c"
    mine.write_text("int mine_edited_by_hand;\n")
    review, _, txt = _drive(tmp_path, tmp_path, reply, ["r"], wait_revert=True)
    assert mine.read_text() == "int mine_edited_by_hand;\n", \
        "r threw away the user's uncommitted edit:\n" + "\n".join(review) + "\n" + txt
    assert t.read_text() == TWENTY, "r did not put t.c back"
    assert "mine.c" not in "\n".join(review), "\n".join(review)


# @scenario SC-ai-agent-97
def test_a_file_the_run_stops_ignoring_is_not_new_and_survives_r(tmp_path):
    """An ignored file the user had is the user's even when the run un-ignores it.

    The run rewrites .gitignore so debug.log is no longer ignored.  debug.log
    was there before the run, so it is never listed as new and `r` keeps it.
    """
    _repo_with_t(tmp_path, tmp_path)
    (tmp_path / ".gitignore").write_text("*.log\n")
    _git_commit(tmp_path, "seed")
    (tmp_path / "debug.log").write_text("mine\n")
    t = tmp_path / "t.c"
    reply = ("TOOL write_file %s\n# nothing ignored\n@@END@@TURN@@"
             "TOOL write_file %s\n%s@@END@@TURN@@DONE changed two files"
             % (tmp_path / ".gitignore", t, TEN_CHANGED))
    review, _, txt = _drive(tmp_path, tmp_path, reply, ["r"], wait_revert=True)
    assert (tmp_path / "debug.log").read_text() == "mine\n", \
        "r removed a file the user had before the run:\n" + "\n".join(review) + "\n" + txt
    assert "debug.log" not in "\n".join(review), "\n".join(review)


# the snapshot's bound on the files it watches, as test_ai_changeset_bounds has it
WATCH_BOUND = 2000


# @scenario SC-ai-agent-98
def test_files_beyond_the_snapshot_bound_are_not_new_when_the_run_deletes_others(tmp_path):
    """Snapshot mode in a workspace larger than its bound: unseen is not new.

    The checkpoint watched WATCH_BOUND of the workspace's files.  The run
    deletes every other file, so the review's walk, now within the bound,
    reaches files the checkpoint never saw, whatever order the directory
    lists them in; those were there before the run and must be neither listed
    nor removed.
    """
    bulk = tmp_path / "bulk"
    bulk.mkdir()
    names = ["f%04d" % i for i in range(WATCH_BOUND + WATCH_BOUND // 2)]
    for name in names:
        (bulk / name).write_text("mine\n")
    (tmp_path / "t.c").write_text(TWENTY)
    reply = "TOOL run_command rm -f bulk/f???[02468]@@TURN@@" + _edit_reply(tmp_path / "t.c")
    review, _, txt = _drive(tmp_path, tmp_path, reply, ["r"], wait_revert=True)
    kept = [n for n in names if int(n[1:]) % 2]
    gone = [n for n in kept if not (bulk / n).exists()]
    assert not gone, "r removed %d files the user had before the run, e.g. %s:\n%s" % (
        len(gone), gone[:3], txt)
    assert "[AI] new file" not in "\n".join(review), "\n".join(review)


# @scenario SC-ai-agent-99
def test_a_created_file_with_a_long_quoted_name_never_removes_a_shorter_one(tmp_path):
    """A name too long to quote whole is refused, never cut to another name.

    The run creates a file whose shell-quoted path does not fit the command
    buffer; cut short, the quoted path named the user's file beside it, whose
    name is a prefix of the created one.  `r` must leave the user's file.
    """
    _repo_with_t(tmp_path, tmp_path)
    _git_commit(tmp_path, "seed")
    deep = tmp_path / ("'" * 255)
    deep.mkdir()
    mine = deep / ("'" * 19)
    mine.write_text("mine\n")
    made = deep / ("'" * 255)
    reply = "TOOL write_file %s\nnew\n@@END@@TURN@@DONE made a file" % made
    review, _, txt = _drive(tmp_path, tmp_path, reply, ["r"], wait_revert=True)
    assert mine.exists() and mine.read_text() == "mine\n", \
        "r removed the user's file through a cut name:\n" + txt
