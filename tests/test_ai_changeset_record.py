"""AI assistant -- what the checkpoint knows about each path, at both moments.

The review after a run is built from a record of the workspace taken at the
checkpoint and again when the review opens (we_ai_ckrec.c).  A name missing
from a list has four causes: it was not there, the walk was refused, the walk
was cut, or the object is there under another name.  Every row below makes the
run do one of those things and reads what the user is told and what `r` does,
on disk first:

  - a folder the walk could not read after the run is a sentence, not silence;
  - a file the run removed or made unreadable is a row, not "unchanged";
  - a file of the user's that the run renamed (alone or with its folder, in a
    plain folder or a git workspace) is the same object under its new name, and
    `r` does not delete it;
  - new files beyond the walk's bound are found, and the checkpoint's own bound
    is said at the review, where PgUp reaches it before the answer;
  - "ALL changes reverted" is said only when nothing was left unseen.

Rows whose assertion needs the revert actions of the next pass (the wastebasket,
undoing a rename) are skipped here and say so.

Run: WPE_BIN=$PWD/wpe pytest tests/test_ai_changeset_record.py (as wpe).
"""
import os
import re
import shutil

import pytest
from host_tools import needs
from wpe_driver import KEY_PGUP, pane_rows, pane_text_with_history, title_row
from test_ai_changeset import (ORIG, WRITE_REPLY, _ai_build, _auto_run, _revert_all,
                               _trace_text)

pytestmark = [
    pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai"),
]

ROOT0 = pytest.mark.skipif(os.geteuid() == 0, reason="root reads a folder with no permissions")
NEEDS_GIT = needs("git")
PASS_B = "the revert actions of the next pass (wastebasket, undo of a rename)"

NOT_SEEN_NOW = "not seen after the run"
NOT_WATCHED = "not watched"
ALL_REVERTED = "ALL changes reverted"
WATCH_BOUND = 2000
USER_DATA = "USER DATA\n"


def _messages(s):
    """The Messages window's rows, read through its frame."""
    return "\n".join(r.rstrip() for r in pane_rows(s.display(), "Messages"))


def _pane(s, words):
    """The pane's lines, paged back until every phrase of `words` is in view."""
    return "\n".join(pane_text_with_history(s, words))


def _flat(text):
    """A text with its wrapping undone, so a long path is found whole."""
    return "".join(text.split())


def _screen(s):
    return "\n".join(s.display(drain=0))


def _run_cmd(command, then=WRITE_REPLY, tmp=None):
    """The mock reply of a run: one shell command, then the usual write of t.c."""
    return "TOOL run_command %s@@TURN@@%s" % (command, then % (tmp / "t.c") if tmp else then)


def _fill(folder, n):
    """`n` small files in `folder`: a workspace as large as the walk's bound."""
    for i in range(n):
        (folder / ("a%04d.txt" % i)).write_text("x\n")


def _user_file(path, text=USER_DATA):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return path


# ---- places the run closed, files it removed or made unreadable -------------

# @scenario SC-ai-checkpoint-01
@ROOT0
def test_a_file_made_in_a_folder_the_run_closed_is_said_not_lost_in_silence(tmp_path):
    """The run makes d/new.c and closes d (chmod 0).  The walk after the run
    cannot read d: the pane says so, the trace has the note, and `r` leaves
    d/new.c alone because nobody saw it."""
    d = tmp_path / "d"
    make = _run_cmd("mkdir d && echo x > d/new.c && chmod 0 d", tmp=tmp_path)
    try:
        with _auto_run(tmp_path, "snapshot", make) as (s, t, trace):
            pane = _pane(s, [NOT_SEEN_NOW])
            txt = _trace_text(trace)
            _revert_all(s, trace)
    finally:
        if d.exists():
            d.chmod(0o755)
    assert NOT_SEEN_NOW in pane and _flat(str(d)) in _flat(pane), \
        "the pane does not say d could not be read after the run:\n" + pane
    assert "changeset note refused" in txt, txt
    assert (d / "new.c").exists(), "r removed a file nobody saw"


# @scenario SC-ai-checkpoint-02
@ROOT0
def test_an_edited_file_made_unreadable_after_the_run_is_a_row(tmp_path):
    """The run rewrites t.c and closes it (chmod 0).  The review lists t.c as
    not readable now, and the pane gives the system's reason, instead of
    treating the file as unchanged."""
    t = tmp_path / "t.c"
    reply = ("TOOL run_command printf 'int main(void){return 7;}\\n' > t.c && echo x > other.c "
             "&& chmod 0 t.c@@TURN@@DONE ok")
    try:
        with _auto_run(tmp_path, "snapshot", reply) as (s, tt, trace):
            pane = _pane(s, ["Permission denied"])
            rows = _messages(s)
    finally:
        if t.exists():
            t.chmod(0o644)
    assert (tmp_path / "other.c").exists(), "the command did not run"
    assert re.search(r"t\.c:\d+: \[AI\] not readable now", rows), rows
    assert "Permission denied" in pane, pane


# @scenario SC-ai-checkpoint-03
def test_a_file_the_run_removed_is_a_row_and_r_puts_it_back(tmp_path):
    """The run removes t.c, which the checkpoint holds a copy of.  The review
    lists it as removed (it used to be left out because it could not be read),
    and `r` writes the checkpoint's copy back."""
    reply = "TOOL run_command rm t.c && echo x > other.c@@TURN@@DONE ok"
    with _auto_run(tmp_path, "snapshot", reply) as (s, t, trace):
        rows = _messages(s)
        _revert_all(s, trace)
    assert re.search(r"t\.c:\d+: \[AI\] removed", rows), rows
    assert t.exists() and t.read_text() == ORIG, "r did not put the removed file back"


# ---- a rename is not a creation ---------------------------------------------

# @scenario SC-ai-checkpoint-04
def test_r_after_a_folder_rename_keeps_the_user_file(tmp_path):
    """d/f.c is the user's and is not in the scope.  The run renames d to e.
    The review calls e/f.c moved (it is the object the checkpoint saw), never
    new, and `r` keeps it.  It used to list it as new and delete the only copy."""
    _user_file(tmp_path / "d" / "f.c")
    reply = _run_cmd("mv d e", tmp=tmp_path)
    with _auto_run(tmp_path, "snapshot", reply) as (s, t, trace):
        rows = _messages(s)
        _revert_all(s, trace)
    kept = [p for p in (tmp_path / "d" / "f.c", tmp_path / "e" / "f.c") if p.exists()]
    assert kept and kept[0].read_text() == USER_DATA, \
        "the revert removed the only copy of d/f.c; the review said:\n" + rows
    assert re.search(r"e/f\.c:\d+: \[AI\] moved here", rows), rows
    assert "[AI] new file" not in rows, rows


# @scenario SC-ai-checkpoint-05
def test_r_after_a_single_file_rename_keeps_the_user_file(tmp_path):
    """sub/notes.txt (no copy held) is renamed to sub/n2.txt by the run."""
    _user_file(tmp_path / "sub" / "notes.txt")
    with _auto_run(tmp_path, "snapshot", _run_cmd("mv sub/notes.txt sub/n2.txt", tmp=tmp_path)) \
            as (s, t, trace):
        rows = _messages(s)
        _revert_all(s, trace)
    n2, old = tmp_path / "sub" / "n2.txt", tmp_path / "sub" / "notes.txt"
    assert n2.exists() or old.exists(), "r removed the only copy of notes.txt:\n" + rows
    assert re.search(r"sub/n2\.txt:\d+: \[AI\] moved here", rows), rows


# @scenario SC-ai-checkpoint-06
def test_r_after_a_rename_of_a_file_with_a_copy_puts_the_copy_back_and_keeps_the_new_name(tmp_path):
    """t.c has a before image.  The run renames it to u.c.  The review lists
    t.c as removed and u.c as moved; `r` writes t.c back from the image and
    leaves u.c where the run put it.  It used to say nothing about t.c, and
    removed u.c: the file was gone."""
    with _auto_run(tmp_path, "snapshot", "TOOL run_command mv t.c u.c@@TURN@@DONE ok") \
            as (s, t, trace):
        rows = _messages(s)
        _revert_all(s, trace)
    assert re.search(r"t\.c:\d+: \[AI\] removed", rows), rows
    assert (tmp_path / "t.c").read_text() == ORIG, "t.c was not put back"
    assert (tmp_path / "u.c").exists(), "r took away the file the run moved"


# @scenario SC-ai-checkpoint-07
def test_a_copy_followed_by_a_removal_is_kept_and_names_the_removed_file(tmp_path):
    """d/notes.txt is copied to d/n2.txt and removed.  The new file has the old
    one's size and the checkpoint holds no copy of notes.txt, so n2.txt may hold
    the user's bytes: it is kept, and the pane names notes.txt."""
    _user_file(tmp_path / "d" / "notes.txt")
    cmd = "cp d/notes.txt d/n2.txt && rm d/notes.txt"
    with _auto_run(tmp_path, "snapshot", _run_cmd(cmd, tmp=tmp_path)) as (s, t, trace):
        rows = _messages(s)
        pane = _pane(s, ["which the run removed and the checkpoint holds no copy of. r keeps it."])
        _revert_all(s, trace)
    assert (tmp_path / "d" / "n2.txt").read_text() == USER_DATA, "r removed the copy:\n" + rows
    assert re.search(r"d/n2\.txt:\d+: \[AI\] new, kept", rows), rows
    said = " ".join(pane.split())
    assert "notes.txt, which the run removed and the checkpoint holds no copy of. r keeps it." in said, pane


# @scenario SC-ai-checkpoint-08
def test_a_hidden_file_renamed_to_a_visible_name_is_not_new(tmp_path):
    """The user's .env is renamed env.txt.  The walk never lists hidden files,
    but it knows their identity: env.txt is the .env the checkpoint saw."""
    _user_file(tmp_path / ".env", "SECRET=1\n")
    with _auto_run(tmp_path, "snapshot", _run_cmd("mv .env env.txt", tmp=tmp_path)) as (s, t, trace):
        _revert_all(s, trace)
    assert (tmp_path / "env.txt").exists() or (tmp_path / ".env").exists(), \
        "r removed the only copy of .env"


# @scenario SC-ai-checkpoint-09
@pytest.mark.skip(reason=PASS_B)
def test_a_file_moved_out_of_a_hidden_folder_is_in_the_wastebasket_after_r(tmp_path):
    """.keep/x.c is moved to x.c by the run.  The walk cannot see into .keep, so
    the file looks new; the revert moves it to the wastebasket and says where."""


# ---- the walk after the run has the bound the walk before it has -------------

# @scenario SC-ai-checkpoint-10
def test_new_files_beyond_the_bound_after_the_run_are_all_listed_and_reverted(tmp_path):
    """The workspace holds 1980 files, just under the walk's bound.  The run
    makes s/g1.txt to s/g30.txt.  The files the checkpoint knew by name do not
    count against the bound after the run, so all 30 are found: n is 31 (the
    30 and t.c), none is left after `r`, and no bound is said."""
    _fill(tmp_path, 1980)
    make = _run_cmd("mkdir s && for i in $(seq 1 30); do echo x > s/g$i.txt; done", tmp=tmp_path)
    with _auto_run(tmp_path, "snapshot", make) as (s, t, trace):
        txt = _trace_text(trace)
        pane = _pane(s, ["the review saw"])
        _revert_all(s, trace)
    left = sorted(p.name for p in tmp_path.glob("s/g*.txt"))
    assert "changeset n=31" in txt, "the review listed fewer than 30 new files and t.c:\n" + txt
    assert not left, "files the run made were kept by r: %s" % left
    assert "the review saw" not in pane, pane


def _bound_run(tmp_path):
    """A workspace of 2100 files, past the checkpoint's bound, and a run that edits t.c."""
    _fill(tmp_path, WATCH_BOUND + 100)
    return _auto_run(tmp_path, "snapshot", WRITE_REPLY % (tmp_path / "t.c"))


# @scenario SC-ai-checkpoint-11
def test_the_checkpoint_bound_is_in_the_trace_and_counted_at_the_review(tmp_path):
    """2100 files: the checkpoint stops at its bound and the trace says so.  The
    review says it again: the trace, the Messages count, the header."""
    with _bound_run(tmp_path) as (s, t, trace):
        txt = _trace_text(trace)
        rows = _messages(s)
    assert "checkpoint note bound seen=%d" % WATCH_BOUND in txt, txt
    assert "changeset note bound" in txt, txt
    assert "not seen (the pane says why)" in rows, rows


# @scenario SC-ai-checkpoint-12
def test_the_bound_sentence_is_in_reach_of_page_up_before_the_answer(tmp_path):
    """The review waits for a key with the pane on screen.  PgUp scrolls the
    pane to the sentence that says what the bound costs, before `a` or `r`."""
    watches = "the snapshot watches %d files under %s" % (WATCH_BOUND, tmp_path)
    beyond = "a file the run creates beyond them is unseen"
    with _bound_run(tmp_path) as (s, t, trace):
        pane = _pane(s, [watches, beyond])
    text = " ".join(" ".join(pane.split()).split())
    assert watches in text and beyond in text, \
        "the pane does not show the bound before the answer:\n" + pane


# @scenario SC-ai-checkpoint-12
def test_page_up_scrolls_the_pane_while_the_review_waits_for_its_key(tmp_path):
    """The review's own key loop used to drop PgUp.  With it pressed once, and
    the review still waiting, the pane shows other rows than before."""
    with _bound_run(tmp_path) as (s, t, trace):
        before = [r.rstrip() for r in pane_rows(s.display(), "AI")]
        s.key(KEY_PGUP, delay=0.8)
        after = [r.rstrip() for r in pane_rows(s.display(), "AI")]
        waiting = "changeset keep all" not in _trace_text(trace)
    assert waiting, "the key ended the review"
    assert before != after, "PgUp did not scroll the pane:\n" + "\n".join(after)


# ---- the review never says everything is back while something was unseen ----

# @scenario SC-ai-checkpoint-13
@ROOT0
def test_a_file_removed_in_a_folder_nobody_could_read_is_not_all_reverted(tmp_path):
    """sub has no permissions at the checkpoint, and the run opens it and
    removes sub/x.c.  The pane repeats at the review that sub was not watched,
    the trace has the note, and the last line does not claim every change was
    reverted."""
    sub = tmp_path / "sub"
    _user_file(sub / "x.c")
    sub.chmod(0)
    reply = _run_cmd("chmod 755 sub && rm sub/x.c", tmp=tmp_path)
    try:
        with _auto_run(tmp_path, "snapshot", reply) as (s, t, trace):
            pane = _pane(s, [NOT_WATCHED])
            txt = _trace_text(trace)
            _revert_all(s, trace)
            shown = _screen(s)
    finally:
        sub.chmod(0o755)
    assert NOT_WATCHED in pane and _flat(str(sub)) in _flat(pane), pane
    assert "changeset note refused" in txt, txt
    assert ALL_REVERTED not in shown, "the last line claims a full revert:\n" + shown


# @scenario SC-ai-checkpoint-14
def test_a_workspace_folder_replaced_by_the_run_is_said(tmp_path):
    """The run moves the workspace folder aside and makes another under its
    name.  The review does not list the new folder's files: it says the folder
    was replaced, and `r` does not claim a full revert."""
    w = tmp_path / "w"
    reply = ("TOOL run_command mv %s %s.old && mkdir %s@@TURN@@DONE ok" % (w, w, w))
    with _auto_run(tmp_path, "snapshot", reply, sub="w") as (s, t, trace):
        pane = _pane(s, ["the workspace folder was replaced since the checkpoint"])
        rows = _messages(s)
        _revert_all(s, trace)
        shown = _screen(s)
    assert "the workspace folder was replaced since the checkpoint" in " ".join(pane.split()), pane
    assert "not seen" in rows, rows
    assert ALL_REVERTED not in shown, shown


# @scenario SC-ai-checkpoint-15
def test_a_file_the_run_made_in_a_new_folder_is_listed_and_r_removes_it(tmp_path):
    """The control: a folder that was not there at the checkpoint is the run's,
    so its file is new, and `r` takes it away and says everything is back."""
    reply = _run_cmd("mkdir out && echo x > out/y.c", tmp=tmp_path)
    with _auto_run(tmp_path, "snapshot", reply) as (s, t, trace):
        rows = _messages(s)
        _revert_all(s, trace)
        shown = _screen(s)
    assert re.search(r"out/y\.c:1: \[AI\] new file", rows), rows
    assert not (tmp_path / "out" / "y.c").exists(), "r left the file the run made"
    assert ALL_REVERTED in shown, shown


# @scenario SC-ai-checkpoint-16
@pytest.mark.skip(reason=PASS_B)
def test_a_file_the_run_made_goes_to_the_wastebasket_and_the_pane_names_it(tmp_path):
    """The control of the next pass: other.c goes to the wastebasket, not away."""


# ---- git workspaces ----------------------------------------------------------

def _git_untracked(path, text=USER_DATA):
    """A file the user has in the workspace, not tracked: made before the run."""
    def prepare(_t):
        _user_file(path, text)
    return prepare


# @scenario SC-ai-checkpoint-17
@NEEDS_GIT
def test_git_r_after_an_untracked_file_is_renamed_keeps_it(tmp_path):
    """The git twin of the rename row: the user's untracked notes.txt is
    renamed n2.txt.  It is untracked now and was not in the list at the
    checkpoint, which used to make it new; it is the same object."""
    with _auto_run(tmp_path, "git", _run_cmd("mv notes.txt n2.txt", tmp=tmp_path),
                   prepare=_git_untracked(tmp_path / "notes.txt")) as (s, t, trace):
        rows = _messages(s)
        _revert_all(s, trace)
    survivors = [p for p in (tmp_path / "notes.txt", tmp_path / "n2.txt") if p.exists()]
    assert survivors and survivors[0].read_text() == USER_DATA, \
        "r removed the user's only copy of notes.txt:\n" + rows
    assert re.search(r"n2\.txt:\d+: \[AI\] moved here", rows), rows


# @scenario SC-ai-checkpoint-18
@NEEDS_GIT
def test_git_r_after_an_ignored_folder_is_renamed_keeps_its_files(tmp_path):
    """build/ is ignored by git.  The run renames it to out, which git does not
    ignore: out/o.bin is untracked now and was in no list.  It is inside the
    folder the checkpoint knew, so nobody can say it is the run's, and `r`
    keeps it."""
    def prepare(_t):
        _user_file(tmp_path / ".gitignore", "build/\n")
        _user_file(tmp_path / "build" / "o.bin", "OBJECT\n")
    with _auto_run(tmp_path, "git", _run_cmd("mv build out", tmp=tmp_path), prepare=prepare) \
            as (s, t, trace):
        _revert_all(s, trace)
    survivors = [p for p in (tmp_path / "build" / "o.bin", tmp_path / "out" / "o.bin") if p.exists()]
    assert survivors and survivors[0].read_text() == "OBJECT\n", \
        "r removed the user's only copy of build/o.bin"


# @scenario SC-ai-checkpoint-21
@NEEDS_GIT
def test_git_an_ignored_file_is_not_removed_and_all_is_reverted(tmp_path):
    """.gitignore holds *.o and the user has foo.o.  The run writes t.c and
    other.c only.  Ignored files are listed at the checkpoint and after the
    run alike, so foo.o is neither a row nor "removed, no copy", and `r` can
    say everything is back."""
    def prepare(_t):
        _user_file(tmp_path / ".gitignore", "*.o\n")
        _user_file(tmp_path / "foo.o", "OBJ\n")
    with _auto_run(tmp_path, "git", _run_cmd("echo x > other.c", tmp=tmp_path),
                   prepare=prepare) as (s, t, trace):
        rows = _messages(s)
        _revert_all(s, trace)
        shown = _screen(s)
    assert "foo.o" not in rows, rows
    assert (tmp_path / "foo.o").read_text() == "OBJ\n"
    assert ALL_REVERTED in shown, shown


# @scenario SC-ai-checkpoint-22
@NEEDS_GIT
def test_git_a_replaced_ignored_folder_keeps_the_user_file(tmp_path):
    """build/ is ignored and holds the user's x.  The run rewrites .gitignore
    and puts a new build/ in its place with x moved into it.  Nobody read what
    was in the ignored folder, so build/x is not the run's and `r` keeps it."""
    def prepare(_t):
        _user_file(tmp_path / ".gitignore", "build/\n")
        _user_file(tmp_path / "build" / "x", "USER\n")
    cmd = "printf 'z\\n' > .gitignore && mv build b2 && mkdir build && mv b2/x build/x"
    with _auto_run(tmp_path, "git", _run_cmd(cmd, tmp=tmp_path), prepare=prepare) as (s, t, trace):
        rows = _messages(s)
        _revert_all(s, trace)
    assert (tmp_path / "build" / "x").read_text() == "USER\n", "r removed the user's build/x:\n" + rows
    assert "build/x:1: [AI] new file" not in rows, rows


# @scenario SC-ai-checkpoint-23
def test_a_file_renamed_to_a_hidden_name_is_a_row_on_its_old_name(tmp_path):
    """d/notes.txt becomes d/.notes.  The review never lists hidden names, so
    the old name gets the row, and `r` keeps the file."""
    _user_file(tmp_path / "d" / "notes.txt")
    with _auto_run(tmp_path, "snapshot", _run_cmd("mv d/notes.txt d/.notes", tmp=tmp_path)) \
            as (s, t, trace):
        rows = _messages(s)
        _revert_all(s, trace)
    assert re.search(r"d/notes\.txt:\d+: \[AI\] now hidden", rows), rows
    assert (tmp_path / "d" / ".notes").read_text() == USER_DATA


# @scenario SC-ai-checkpoint-24
def test_alt_t_does_not_open_a_window_under_a_name_that_is_gone(tmp_path):
    """The row of a file the run renamed to a hidden name is about the old
    name, which is not there now.  It is in Messages but not in the list
    Alt-T steps (the trace says how long that list is), so Alt-T opens no
    empty window under that name."""
    _user_file(tmp_path / "d" / "notes.txt")
    with _auto_run(tmp_path, "snapshot", "TOOL run_command mv d/notes.txt d/.notes@@TURN@@DONE ok") \
            as (s, t, trace):
        rows = _messages(s)
        txt = _trace_text(trace)
        s.key("a", delay=1.0)                # the review answers; Alt-T steps its list
        s.key("\033t", delay=1.5)
        opened = title_row(s.display(drain=0), "notes.txt")
    assert re.search(r"d/notes\.txt:\d+: \[AI\] now hidden", rows), rows
    assert opened < 0, "Alt-T opened a window under the name that is gone"
    assert "changeset jump list n=0" in txt, txt


# @scenario SC-ai-checkpoint-25
def test_a_file_outside_the_workspace_under_a_replaced_root_is_not_restored(tmp_path):
    """out/x.c is open outside the workspace, so it has a before image and out
    is watched.  The run moves out aside and puts a link to docs in its place;
    docs/x.c is the user's own.  The review cannot say what became of out/x.c
    (the folder was replaced): no row, nothing restored, and `r` leaves docs/x.c
    as it was.  The review counts the place as not seen."""
    out = tmp_path.parent / (tmp_path.name + ".out")
    docs = tmp_path.parent / (tmp_path.name + ".docs")
    out.mkdir()
    docs.mkdir()
    (out / "x.c").write_text("SEED X\n")
    (docs / "x.c").write_text("USER X\n")
    cmd = "mv %s %s.real && ln -s %s %s" % (out, out, docs, out)
    with _auto_run(tmp_path, "snapshot", _run_cmd(cmd, tmp=tmp_path), args=[str(out / "x.c")]) \
            as (s, t, trace):
        rows = _messages(s)
        _revert_all(s, trace)
        shown = _screen(s)
    assert (docs / "x.c").read_text() == "USER X\n", "r wrote the checkpoint's copy over docs/x.c"
    assert "1 not seen" in rows, rows
    assert ALL_REVERTED not in shown, shown


def _git_that_fails_after(tmp_path, marker):
    """A `git` ahead on PATH that fails `ls-files` once `marker` exists."""
    real = shutil.which("git")
    bindir = tmp_path.parent / (tmp_path.name + ".bin")
    bindir.mkdir()
    script = bindir / "git"
    script.write_text('#!/bin/sh\nif [ -e "%s" ]; then case "$*" in *ls-files*) exit 1;; esac; fi\n'
                      'exec "%s" "$@"\n' % (marker, real))
    script.chmod(0o755)
    return {"PATH": "%s:%s" % (bindir, os.environ.get("PATH", ""))}


# @scenario SC-ai-checkpoint-19
@NEEDS_GIT
def test_git_that_cannot_list_after_the_run_is_said_and_r_claims_no_full_revert(tmp_path):
    """git answers at the checkpoint and not after the run.  The review cannot
    list the files the run made: it says so in the pane and the trace, and the
    last line of `r` does not claim every change was reverted."""
    marker = tmp_path.parent / (tmp_path.name + ".mark")
    env = _git_that_fails_after(tmp_path, marker)
    reply = _run_cmd("touch %s && echo x > other.c" % marker, tmp=tmp_path)
    with _auto_run(tmp_path, "git", reply, env_extra=env) as (s, t, trace):
        pane = _pane(s, ["git could not list"])
        txt = _trace_text(trace)
        _revert_all(s, trace)
        shown = _screen(s)
    assert "git could not list" in pane, pane
    assert "changeset note tool" in txt, txt
    assert ALL_REVERTED not in shown, shown
