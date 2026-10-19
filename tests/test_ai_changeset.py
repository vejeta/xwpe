"""AI assistant -- checkpoint + changeset review in a git workspace.

An auto-policy agent rewrites a committed file; the changeset lands in
Messages as `path:line: [AI] +a -b` (registered for Alt-T / Alt-V) and the
review loop can revert everything back to the checkpoint or keep it.

The later cases run in a workspace that is NOT a git tree, where the
checkpoint is snapshot copies and the revert writes the file itself.  The last
three are the ways that promise can break: a restore the workspace refuses, a
file the run created in a directory that will not let it go, and a checkpoint
that could not save a single before image.  Each one asserts the ground truth
on disk first, then that the user is told, then that neither the pane, the
review's summary nor the trace claims work that did not happen.
"""
import os
import re
import shutil
import subprocess
import time
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN, pane_rows, pane_text_with_history


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"AI changeset" in out
    except Exception:
        return False


pytestmark = [
    pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai"),
    pytest.mark.skipif(shutil.which("git") is None, reason="no git"),
]

ORIG = "int main(void){return 0;}\n"


def _git_repo(tmp_path):
    def g(*a):
        # commit.gpgsign=false: the host's ~/.gitconfig may force signed commits,
        # which fail non-interactively (no pinentry) -- the test must not depend
        # on the developer's GPG setup.
        subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t",
                        "-c", "commit.gpgsign=false", *a],
                       cwd=tmp_path, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    g("init", "-q")
    (tmp_path / "t.c").write_text(ORIG)
    g("add", "t.c")
    g("commit", "-q", "-m", "seed")


def _run(tmp_path, review_key):
    _git_repo(tmp_path)
    t = tmp_path / "t.c"
    # keep the trace OUTSIDE the git workspace so it is not part of the changeset
    trace = tmp_path.parent / (tmp_path.name + ".trace")
    reply = ("TOOL write_file %s\nint main(void){return 99;}\n@@END" % t
             + "@@TURN@@DONE rewrote it")
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_POLICY": "auto",
        "XWPE_AI_TRACE": str(trace),
    }
    with WpeSession(str(tmp_path), ORIG, env_extra=env) as s:
        s.key(ALT.AI)
        s.key("g")
        s.key("rewrite it")
        s.key("\r", delay=2.0)           # submit -> checkpoint -> run -> review
        s._drain(1.5)
        s.key(review_key, delay=1.5)     # a = keep all / r = revert all
        s._drain(1.0)
        shown = s.display(drain=0)
    return trace.read_text() if trace.exists() else "", t, shown


def test_ai_changeset_revert_all(tmp_path):
    txt, t, shown = _run(tmp_path, "r")
    assert "checkpoint git" in txt, txt
    assert "changeset n=1" in txt, txt
    assert "changeset revert all" in txt, txt
    assert t.read_text() == ORIG, "revert did not restore the file:\n" + t.read_text()
    # the window the user is looking at is reloaded from the restored file, so
    # the screen says what the disk holds
    window = "\n".join(pane_rows(shown, "t.c"))
    assert ORIG.strip() in window, \
        "the open window still shows the text the revert took away:\n" + window


def test_ai_changeset_keep_all(tmp_path):
    txt, t, _shown = _run(tmp_path, "a")
    assert "changeset n=1" in txt, txt
    assert "changeset keep all" in txt, txt
    assert "return 99" in t.read_text()


# @scenario SC-ai-agent-61
def test_ai_changeset_esc_at_the_top_keeps_all(tmp_path):
    """Esc at the review's prompt keeps every change, the same as `a`.

    When the review opens the files are already written, so leaving them as
    they are is keeping them: Esc backs out of the review and touches nothing,
    and the trace records it as the keep it is.
    """
    txt, t, _shown = _run(tmp_path, "\033")
    assert "changeset n=1" in txt, txt
    assert "changeset keep all" in txt, txt
    assert "changeset revert" not in txt, "Esc reverted the change:\n" + txt
    assert "return 99" in t.read_text(), \
        "Esc at the review took the change away:\n" + t.read_text()


def _trace_text(path):
    return path.read_text() if path.exists() else ""


def _wait_trace(path, needle, tmo):
    """Poll the trace until it holds `needle`.

    The trace is the run's own record of having reached a step, so it says when
    the review is waiting for its key without depending on which window the
    screen has room for.
    """
    end = time.time() + tmo
    while time.time() < end:
        if needle in _trace_text(path):
            return True
        time.sleep(0.2)
    return False


def test_ai_changeset_revert_reports_a_file_it_could_not_restore(tmp_path):
    """A revert that could not write the file says so instead of claiming it did.

    The workspace is not a git tree, so the checkpoint is snapshot copies and
    the revert writes the file itself.  The file is made read-only once the
    review is waiting for its key: the restore is refused, the file keeps the
    change, and the pane names the file and the reason the system gave.  The
    review's own summary must not say everything went back.
    """
    t = tmp_path / "t.c"
    # keep the trace OUTSIDE the workspace so it is not part of the changeset
    trace = tmp_path.parent / (tmp_path.name + ".trace")
    reply = ("TOOL write_file %s\nint main(void){return 99;}\n@@END" % t
             + "@@TURN@@DONE rewrote it")
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_POLICY": "auto",
        "XWPE_AI_TRACE": str(trace),
        "XDG_STATE_HOME": str(tmp_path / "state"),   # where the copies go
    }
    with WpeSession(str(tmp_path), ORIG, env_extra=env) as s:
        s.key(ALT.AI)
        s.key("g")
        s.key("rewrite it")
        s.key("\r", delay=2.0)          # submit -> checkpoint -> run -> review
        assert _wait_trace(trace, "changeset n=", 60), \
            "the review never opened:\n" + _trace_text(trace)
        t.chmod(0o444)                  # the restore cannot write the file now
        s.key("r", delay=1.0)           # r = revert all
        assert _wait_trace(trace, "changeset revert", 30), \
            "the revert never ran:\n" + _trace_text(trace)
        pane = pane_text_with_history(s, ["could not restore",
                                          "Permission denied"])
        disp = "\n".join(s.display(drain=0))

    # ground truth: the write really was refused, the change is still there
    assert "return 99" in t.read_text(), \
        "the file was restored after all:\n" + t.read_text()

    text = "\n".join(pane)
    assert "could not restore" in text, \
        "the pane does not say the file kept the change:\n" + text
    assert "Permission denied" in text, \
        "the pane does not say why the file kept the change:\n" + text
    # the pane wraps a long path over rows, so the spaces a wrap added are
    # dropped before the path is looked for
    assert str(t) in text.replace(" ", "").replace("\n", ""), \
        "the pane does not name the file it could not restore:\n" + text

    txt = _trace_text(trace)
    assert "changeset n=1" in txt, txt
    assert "changeset revert failed" in txt, \
        "the trace does not say the revert failed:\n" + txt
    lying = [l for l in txt.splitlines()
             if l.startswith("changeset revert ") and "failed" not in l]
    assert not lying, "the trace claims a revert that did not happen: %r" % lying

    assert "ALL changes reverted" not in disp, \
        "the review still says everything went back:\n" + disp
    assert "kept the change" in disp, \
        "the review does not say the file kept the change:\n" + disp


def _pane_lines_with(pane, needle):
    """The pane's logical lines holding `needle`, with the spaces a wrap added
    dropped from each.

    Scoped to the lines that say the failure, so the line announcing the write
    (which names the same file) cannot stand in for them.  Flattened because
    the review's Messages window halves the pane and the wrapper then cuts a
    long path over rows; a path cut in two is still whole once the spaces go.
    """
    return "\n".join(l.replace(" ", "") for l in pane if needle in l)


def _lying_revert_lines(txt):
    """Trace lines that claim one file went back, with no failure on them."""
    return [l for l in txt.splitlines()
            if l.startswith("changeset revert ") and "failed" not in l]


def test_ai_changeset_revert_reports_a_git_restore_the_workspace_refused(tmp_path):
    """A git restore the workspace refused is said, not swallowed.

    The workspace is a git tree, so the checkpoint is a git ref and putting a
    file back means handing `git checkout <ref> -- <file>` to a shell.  The
    workspace directory is made read-only once the review waits for its key:
    git cannot take the old file away, it refuses in its own words, and the
    file keeps the change.  The pane must name the file and carry that reason,
    and the review's summary must count the file instead of claiming that
    everything went back.

    A read-only FILE does not provoke this: git unlinks and recreates it, so
    the restore succeeds and the mode comes back from the repository.  The
    directory is what has to refuse.
    """
    _git_repo(tmp_path)
    t = tmp_path / "t.c"
    # keep the trace OUTSIDE the git workspace so it is not part of the changeset
    trace = tmp_path.parent / (tmp_path.name + ".trace")
    reply = ("TOOL write_file %s\nint main(void){return 99;}\n@@END" % t
             + "@@TURN@@DONE rewrote it")
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_POLICY": "auto",
        "XWPE_AI_TRACE": str(trace),
    }
    try:
        with WpeSession(str(tmp_path), ORIG, env_extra=env) as s:
            s.key(ALT.AI)
            s.key("g")
            s.key("rewrite it")
            s.key("\r", delay=2.0)           # submit -> checkpoint -> run -> review
            assert _wait_trace(trace, "changeset n=", 60), \
                "the review never opened:\n" + _trace_text(trace)
            os.chmod(tmp_path, 0o555)        # git cannot unlink the file now
            s.key("r", delay=1.0)            # r = revert all
            assert _wait_trace(trace, "changeset revert", 30), \
                "the revert never ran:\n" + _trace_text(trace)
            pane = pane_text_with_history(s, ["could not restore",
                                              "Permission denied"])
            disp = "\n".join(s.display(drain=0))
            alive = s.alive()
    finally:
        os.chmod(tmp_path, 0o755)            # pytest has to clean this up

    # ground truth: the restore really was refused, the change is still there
    assert "return 99" in t.read_text(), \
        "the file was restored after all:\n" + t.read_text()

    text = "\n".join(pane)
    assert "could not restore" in text, \
        "the pane does not say the file kept the change:\n" + text
    assert "Permission denied" in text, \
        "the pane does not carry the reason the workspace gave:\n" + text
    # matched against the failure line itself, so the pane line that announced
    # the write cannot stand in for it
    said = _pane_lines_with(pane, "could not restore")
    assert str(t) in said, \
        "the failure line does not name the file it could not restore:\n" + text

    txt = _trace_text(trace)
    assert "checkpoint git" in txt, txt
    assert "changeset n=1" in txt, txt
    assert "changeset revert failed" in txt, \
        "the trace does not say the revert failed:\n" + txt
    assert not _lying_revert_lines(txt), \
        "the trace claims a revert that did not happen:\n" + txt

    assert "ALL changes reverted" not in disp, \
        "the review still says everything went back:\n" + disp
    assert "kept the change" in disp, \
        "the review does not say the file kept the change:\n" + disp
    assert alive, "a restore the workspace refused ended the editor"


def test_ai_changeset_revert_reports_a_created_file_it_could_not_remove(tmp_path):
    """A file the run created, in a directory that will not let it go.

    The workspace is not a git tree, so the checkpoint is snapshot copies and
    putting back a file the run created means taking it away.  The directory is
    made read-only once the review waits for its key: the delete is refused and
    the file is still there.  The user must be told that, instead of reading a
    summary that says every change went back over a file that did not.
    """
    new = tmp_path / "out.txt"
    # keep the trace and the state folder OUTSIDE the workspace: it is about to
    # become read-only, and neither belongs in the changeset
    trace = tmp_path.parent / (tmp_path.name + ".trace")
    state = tmp_path.parent / (tmp_path.name + ".state")
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": "TOOL write_file out.txt\nhello agent\n@@END"
                              "@@TURN@@DONE wrote it",
        "XWPE_AI_POLICY": "auto",
        "XWPE_AI_TRACE": str(trace),
        "XDG_STATE_HOME": str(state),
    }
    try:
        with WpeSession(str(tmp_path), ORIG, env_extra=env) as s:
            s.key(ALT.AI)
            s.key("g")
            s.key("write a file")
            s.key("\r", delay=2.0)           # submit -> checkpoint -> run -> review
            assert _wait_trace(trace, "changeset n=", 60), \
                "the review never opened:\n" + _trace_text(trace)
            os.chmod(tmp_path, 0o555)        # the file cannot be unlinked now
            s.key("r", delay=1.0)            # r = revert all
            assert _wait_trace(trace, "changeset revert", 30), \
                "the revert never ran:\n" + _trace_text(trace)
            pane = pane_text_with_history(s, ["could not remove",
                                              "Permission denied"])
            disp = "\n".join(s.display(drain=0))
            alive = s.alive()
    finally:
        os.chmod(tmp_path, 0o755)            # pytest has to clean this up

    # ground truth: the delete really was refused, the file is still there
    assert new.exists(), "the file the run created was removed after all"
    assert "hello agent" in new.read_text(), new.read_text()

    text = "\n".join(pane)
    assert "could not remove" in text, \
        "the pane does not say the file is still there:\n" + text
    assert "Permission denied" in text, \
        "the pane does not say why the file is still there:\n" + text
    said = _pane_lines_with(pane, "could not remove")
    assert str(new) in said, \
        "the failure line does not name the file it could not remove:\n" + text

    txt = _trace_text(trace)
    assert "changeset n=1" in txt, txt
    assert "changeset revert failed" in txt, \
        "the trace does not say the revert failed:\n" + txt
    assert not _lying_revert_lines(txt), \
        "the trace claims a revert that did not happen:\n" + txt

    assert "ALL changes reverted" not in disp, \
        "the review still says everything went back:\n" + disp
    assert "kept the change" in disp, \
        "the review does not say the file kept the change:\n" + disp
    assert alive, "a delete the directory refused ended the editor"


def test_ai_checkpoint_without_a_before_image_offers_no_revert(tmp_path):
    """No checkpoint is better than one that cannot put anything back.

    XDG_STATE_HOME points below a regular file, so the folder the snapshot
    copies would go into cannot be created.  The checkpoint used to activate
    anyway, with no before image in it: the review then opened over a file the
    run had changed and offered a revert that could only restore nothing, and
    the pane had already promised the changes were revertible.  Now the
    checkpoint stays off, the pane says the folder could not be created, and
    nothing on screen offers or claims a revert.
    """
    blocker = tmp_path / "blocker"
    blocker.write_text("a regular file, where a directory was needed\n")
    state = blocker / "state"
    t = tmp_path / "t.c"
    # keep the trace OUTSIDE the workspace so it is not part of the changeset
    trace = tmp_path.parent / (tmp_path.name + ".trace")
    reply = ("TOOL write_file %s\nint main(void){return 99;}\n@@END" % t
             + "@@TURN@@DONE rewrote it")
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_POLICY": "auto",
        "XWPE_AI_TRACE": str(trace),
        "XDG_STATE_HOME": str(state),
    }
    with WpeSession(str(tmp_path), ORIG, env_extra=env) as s:
        s.key(ALT.AI)
        s.key("g")
        s.key("rewrite it")
        s.key("\r", delay=2.0)               # submit -> checkpoint -> run
        assert _wait_trace(trace, "agent done", 60), \
            "the run never finished:\n" + _trace_text(trace)
        pane = pane_text_with_history(s, ["could not create the folder",
                                          "Not a directory", "revertible"])
        disp = "\n".join(s.display(drain=0))
        alive = s.alive()

    # ground truth: the change is on disk and no before image exists anywhere
    assert "return 99" in t.read_text(), t.read_text()
    assert not state.exists(), "the snapshot folder was created after all"

    text = "\n".join(pane)
    assert "could not create the folder" in text, \
        "the pane does not say the checkpoint folder is missing:\n" + text
    assert "Not a directory" in text, \
        "the pane does not say why it is missing:\n" + text
    assert "revertible" not in text, \
        "the pane still promises a revert that cannot happen:\n" + text
    assert "ALL changes reverted" not in disp, \
        "the screen claims a revert that cannot happen:\n" + disp

    txt = _trace_text(trace)
    assert "changeset" not in txt, \
        "a review opened over a checkpoint that holds no before image:\n" + txt
    assert not _lying_revert_lines(txt), \
        "the trace claims a revert that did not happen:\n" + txt
    assert alive, "a checkpoint that could not be saved ended the editor"


# ---- where a row points, and where its jump lands ---------------------------
#
# A twenty-line file whose line 10 the run rewrites, wide enough that the
# Messages window shows a row whole.  Both checkpoint modes go through the same
# helper, because the row and the jump are two views of one place and only a
# test that reads both can see them disagree.

TWENTY = "\n".join("int l%02d = %d;" % (i, i) for i in range(1, 21)) + "\n"
TEN_CHANGED = TWENTY.replace("int l10 = 10;", "int l10 = 1010;")
CHANGED_LINE = 10
ALT_T = "\033t"
WIDE_COLS, WIDE_ROWS = 200, 40


def _git(tmp_path, *args):
    """Run git in the workspace under the test's own identity.

    `commit.gpgsign=false` for the reason `_git_repo` gives: a host that signs
    by default must not decide whether these tests can run.
    """
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t",
                    "-c", "commit.gpgsign=false", *args],
                   cwd=tmp_path, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def _git_commit(tmp_path, message):
    """Commit the workspace as it stands, a file it has not seen included."""
    _git(tmp_path, "add", "-A")
    _git(tmp_path, "commit", "-qm", message)


def _row_line(rows):
    """The line a changeset row names, read from `<path>:<line>: [AI] ...`.

    None when no row is on screen, so a missing row fails as a missing row
    instead of as a line number that happens to be wrong.
    """
    for r in rows:
        m = re.search(r":(\d+): \[AI\]", r)
        if m:
            return int(m.group(1))
    return None


def _status_row(rows):
    """The file window's status row: its bottom border carrying line:column.

    Read the way the other AI contract files read it, so a jump is judged by
    the editor's own answer to "where is the cursor" and not by the text on
    screen, which a narrow window scrolls and a wide one shows twice.  The
    Messages and pane borders carry no line:column field, so they cannot pass
    for it.
    """
    for i, row in enumerate(rows):
        if row.startswith("m") and re.search(r"\s\d+:\d+\s", row):
            return i
    return -1


def _cursor_line(rows):
    """The 1-based line the status row shows, or None when there is no such row."""
    i = _status_row(rows)
    if i < 0:
        return None
    return int(re.search(r"\s(\d+):(\d+)\s", rows[i]).group(1))


def _run_changed_line(tmp_path, git, cols=WIDE_COLS, rows=WIDE_ROWS):
    """Rewrite line 10 of a twenty-line file under a run, keep it, then jump.

    Returns (the review's screen rows, the rows after Alt-T, the trace text,
    whether the editor is still alive).  The trace and the snapshot folder are
    kept outside the workspace in both modes, so neither is counted as a change
    of the run and the changeset stays the one file the run rewrote.
    """
    t = tmp_path / "t.c"
    if git:
        _git_repo(tmp_path)
        t.write_text(TWENTY)
        _git_commit(tmp_path, "twenty")
    trace = tmp_path.parent / (tmp_path.name + ".trace")
    state = tmp_path.parent / (tmp_path.name + ".state")
    reply = ("TOOL write_file %s\n%s@@END@@TURN@@DONE changed one line"
             % (t, TEN_CHANGED))
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_POLICY": "auto",
        "XWPE_AI_TRACE": str(trace),
        "XDG_STATE_HOME": str(state),
    }
    with WpeSession(str(tmp_path), TWENTY, env_extra=env,
                    cols=cols, rows=rows) as s:
        s.key(ALT.AI)
        s.key("g")
        s.key("change line ten")
        s.key("\r", delay=1.0)               # submit -> checkpoint -> run -> review
        assert _wait_trace(trace, "changeset n=", 60), \
            "the review never opened:\n" + _trace_text(trace)
        review = [r.rstrip() for r in s.display(drain=0)]
        s.key("a", delay=0.8)                # keep all, the review closes
        s.key(ALT_T, delay=0.8)              # the jump the row promises
        jumped = [r.rstrip() for r in s.display(drain=0)]
        alive = s.alive()
    return review, jumped, _trace_text(trace), alive


# @scenario SC-ai-agent-58
@pytest.mark.parametrize("git", [True, False], ids=["git", "snapshot"])
def test_ai_changeset_row_points_at_the_first_changed_line(tmp_path, git):
    """The row names the first changed line, and Alt-T goes to that line.

    A run rewrote line 10 of a twenty-line file.  In a git workspace the row
    used to name line 7 and the jump landed there, because the diff was read
    with its three lines of context and a hunk starts at the first of them; a
    snapshot workspace named 10 and landed on 10.  The two modes are one
    contract, and the row and the jump are read together so they cannot each
    be right about a different place.
    """
    review, jumped, txt, alive = _run_changed_line(tmp_path, git)
    row = _row_line(review)
    at = _cursor_line(jumped)
    assert row == CHANGED_LINE, \
        "the row names line %r, the change is on %d:\n%s" % (
            row, CHANGED_LINE, "\n".join(review))
    assert at == CHANGED_LINE, \
        "Alt-T put the cursor on line %r, the change is on %d:\n%s" % (
            at, CHANGED_LINE, "\n".join(jumped))
    assert "changeset n=1" in txt, txt
    assert alive, "stepping the changeset ended the editor"


# ---- a row the default layout can show -------------------------------------
#
# At 80 columns the pane is docked beside Messages, which is then 38 columns
# wide and cuts a row at its border instead of wrapping it.  A row that names
# the file by its absolute path is all path by then: no line, no counts.

def _messages_rows(rows):
    """The Messages window's own rows, read through its frame.

    The window is read rather than the screen, because at 80 columns a screen
    row carries a Messages row and a pane row side by side, and the pane's text
    would answer a question asked of Messages.
    """
    return [r.rstrip() for r in pane_rows(rows, "Messages")]


def _changeset_row(rows):
    """The row Messages shows for one change, "" when it shows none.

    Found as the row under the review's header, not as the row carrying the
    "[AI]" mark: a row the window's width cut can have lost the mark, the line
    and the counts, and losing them is one of the things this is read for.
    """
    messages = _messages_rows(rows)
    for i, r in enumerate(messages):
        if r.startswith("=== AI changeset") and i + 1 < len(messages):
            return _row_text(messages[i + 1])
    return ""


def _row_text(row):
    """A Messages row without the cursor block and frame edge the window may
    paint after its text when it is the active window."""
    return re.sub(r"(\s|▮)+x?$", "", row)


# @scenario SC-ai-agent-60
def test_ai_changeset_row_fits_the_messages_window_at_80_columns(tmp_path):
    """A row shows the line and the counts in the default layout, and still jumps.

    80 columns with the pane docked, so Messages is 38 columns wide and cuts a
    row at its border.  Naming the file by its path under the workspace root,
    the way a compiler run from the root reports one, is what leaves room for
    the line and the counts; the absolute path filled the window on its own.
    The jump is asserted with the row, because the row is what the user reads
    and the list behind Alt-T is what the editor opens: making the row relative
    must not move the jump.
    """
    review, jumped, txt, alive = _run_changed_line(tmp_path, git=True,
                                                   cols=80, rows=30)
    row = _changeset_row(review)
    want = "t.c:%d: [AI] +1 -1" % CHANGED_LINE
    assert row == want, \
        "Messages shows %r, the row should read %r:\n%s" % (
            row, want, "\n".join(_messages_rows(review)))
    at = _cursor_line(jumped)
    assert at == CHANGED_LINE, \
        "Alt-T put the cursor on line %r, the row named %d:\n%s" % (
            at, CHANGED_LINE, "\n".join(jumped))
    assert "changeset n=1" in txt, txt
    assert alive, "stepping the changeset ended the editor"


# ---- Esc inside the file-by-file prompt ------------------------------------

T_NEW = "int main(void){return 99;}\n"
U_ORIG = "int u(void){return 0;}\n"
U_NEW = "int u(void){return 9;}\n"
ESC = "\033"


def _two_file_repo(tmp_path):
    """A git workspace with two committed files, so the review has two rows."""
    _git_repo(tmp_path)
    (tmp_path / "u.c").write_text(U_ORIG)
    _git_commit(tmp_path, "two files")


def _review_two_files(tmp_path, keys):
    """Rewrite two committed files under a run, then answer the review with `keys`.

    Returns (the Messages rows the review left, the trace text, whether the
    editor is alive).  The trace and the snapshot folder stay outside the
    workspace so neither joins the changeset, which is then exactly the two
    files the run rewrote.
    """
    t, u = tmp_path / "t.c", tmp_path / "u.c"
    _two_file_repo(tmp_path)
    trace = tmp_path.parent / (tmp_path.name + ".trace")
    state = tmp_path.parent / (tmp_path.name + ".state")
    reply = "@@TURN@@".join([
        "TOOL write_file %s\n%s@@END" % (t, T_NEW),
        "TOOL write_file %s\n%s@@END" % (u, U_NEW),
        "DONE changed two files"])
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_POLICY": "auto",
        "XWPE_AI_TRACE": str(trace),
        "XDG_STATE_HOME": str(state),
    }
    with WpeSession(str(tmp_path), ORIG, env_extra=env) as s:
        s.key(ALT.AI)
        s.key("g")
        s.key("change two files")
        s.key("\r", delay=1.0)               # submit -> checkpoint -> run -> review
        assert _wait_trace(trace, "changeset n=", 60), \
            "the review never opened:\n" + _trace_text(trace)
        for k in keys:
            s.key(k, delay=0.8)
        ended = _wait_trace(trace, "changeset file-by-file", 20)
        messages = _messages_rows(s.display(drain=0))
        alive = s.alive()
    return messages, _trace_text(trace), alive, ended


# @scenario SC-ai-agent-62
def test_ai_changeset_esc_in_file_by_file_keeps_the_files_it_never_asked_about(tmp_path):
    """Esc at a per-file question keeps the rest and ends the review.

    Two files changed, `f` asks about the first, Esc backs out.  Esc means the
    same here as at the top of the review: leave the files as the run wrote
    them.  The prompt used to swallow the key and wait, so backing out of one
    question was impossible; now the review ends, both files still hold the
    change, nothing was reverted, and Messages says how many files were left
    unreviewed, which is the one thing the user cannot work out afterwards.
    """
    t, u = tmp_path / "t.c", tmp_path / "u.c"
    messages, txt, alive, ended = _review_two_files(tmp_path, ["f", ESC])
    shown = "\n".join(messages)

    # ground truth: both files kept the change, and neither was put back
    assert t.read_text() == T_NEW, "the first file lost the change:\n" + t.read_text()
    assert u.read_text() == U_NEW, "the file never asked about lost the change:\n" + u.read_text()

    assert ended, "Esc did not end the review, it is still waiting:\n" + shown
    assert "changeset n=2" in txt, txt
    assert "changeset revert" not in txt, \
        "Esc reverted a file it was asked to keep:\n" + txt
    assert not _lying_revert_lines(txt), \
        "the trace claims a revert that did not happen:\n" + txt

    assert "[AI] Esc: 2 files kept unseen" in shown, \
        "Messages does not say how many files were left unreviewed:\n" + shown
    # the question names the file the way its row does: at 80 columns the
    # Messages window is 38 wide, and an absolute path there hid the y/n hint
    assert "keep t.c ?  y = keep  n = revert" in shown, \
        "the file-by-file question does not fit the Messages window:\n" + shown
    assert alive, "backing out of a per-file question ended the editor"


# ---- what `c` commits -------------------------------------------------------

HAND_ORIG = "int hand(void){return 0;}\n"
HAND_EDIT = "int hand(void){return 7;}\n"
A_TXT = "AAA\n"


def _git_out(tmp_path, *args):
    """git's own answer as text: what a commit holds is read from git, not from
    the screen, which shows only what the review chose to say about it."""
    return subprocess.run(["git", *args], cwd=tmp_path, check=True,
                          stdout=subprocess.PIPE, text=True).stdout


def _committed_paths(tmp_path):
    """The paths in HEAD's commit, sorted."""
    return sorted(_git_out(tmp_path, "show", "--pretty=format:", "--name-only",
                           "HEAD").split())


# @scenario SC-ai-agent-63
def test_ai_changeset_commit_covers_the_reviewed_files_only(tmp_path):
    """`c` commits the files the review listed, and leaves the user's own edit out.

    The workspace holds an edit the user made before the run and never
    committed.  Staging everything in the workspace put that edit into the
    commit as well, so a commit that should read as one run's change carried a
    change of the user's that they had deliberately left uncommitted.  The
    changeset's own files are staged instead: the file the run created and the
    one it rewrote go in, and the user's edit stays unstaged on disk.
    """
    t = tmp_path / "t.c"
    hand = tmp_path / "hand.c"
    _git_repo(tmp_path)                       # commits t.c holding ORIG
    hand.write_text(HAND_ORIG)
    _git_commit(tmp_path, "two files")
    hand.write_text(HAND_EDIT)                # the user's own edit, uncommitted
    trace = tmp_path.parent / (tmp_path.name + ".trace")
    state = tmp_path.parent / (tmp_path.name + ".state")
    reply = "@@TURN@@".join([
        "TOOL write_file a.txt\n%s@@END" % A_TXT,
        "TOOL write_file %s\n%s@@END" % (t, T_NEW),
        "DONE wrote two files"])
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_POLICY": "auto",
        "XWPE_AI_TRACE": str(trace),
        "XDG_STATE_HOME": str(state),
        "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t",
        "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t",
    }
    with WpeSession(str(tmp_path), ORIG, env_extra=env) as s:
        s.key(ALT.AI)
        s.key("g")
        s.key("write two files")
        s.key("\r", delay=1.0)               # submit -> checkpoint -> run -> review
        assert _wait_trace(trace, "changeset n=", 60), \
            "the review never opened:\n" + _trace_text(trace)
        s.key("c", delay=0.8)                # commit, then take the default message
        s.key("\r", delay=1.0)
        made = _wait_trace(trace, "changeset commit", 30)
        messages = _messages_rows(s.display(drain=0))
        alive = s.alive()

    assert made, "the commit never ran:\n%s\ntrace:\n%s" % (
        "\n".join(messages), _trace_text(trace))
    txt = _trace_text(trace)
    assert "changeset n=2" in txt, txt
    assert _committed_paths(tmp_path) == ["a.txt", "t.c"], \
        "the commit holds the wrong files:\n%s" % _git_out(tmp_path, "show",
                                                           "--stat", "HEAD")
    status = _git_out(tmp_path, "status", "--porcelain")
    assert " M hand.c" in status, \
        "the user's own edit was staged or committed:\n" + status
    assert hand.read_text() == HAND_EDIT, \
        "the user's edit did not survive the commit:\n" + hand.read_text()
    assert (tmp_path / "a.txt").read_text() == A_TXT
    assert alive, "committing the changeset ended the editor"


# @scenario SC-ai-agent-63
def test_ai_changeset_commit_leaves_the_users_staged_edit_in_the_index(tmp_path):
    """`c` commits the listed files even when the user had staged other work.

    Staging the listed files and then committing the whole index carried
    whatever the user had already staged into the agent's commit.  The commit
    now names the same paths it staged, so the user's staged edit stays in the
    index, uncommitted, exactly as they left it.
    """
    t = tmp_path / "t.c"
    hand = tmp_path / "hand.c"
    _git_repo(tmp_path)                       # commits t.c holding ORIG
    hand.write_text(HAND_ORIG)
    _git_commit(tmp_path, "two files")
    hand.write_text(HAND_EDIT)
    _git(tmp_path, "add", "hand.c")           # the user's own edit, staged
    trace = tmp_path.parent / (tmp_path.name + ".trace")
    state = tmp_path.parent / (tmp_path.name + ".state")
    reply = "TOOL write_file %s\n%s@@END@@TURN@@DONE rewrote it" % (t, T_NEW)
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_POLICY": "auto",
        "XWPE_AI_TRACE": str(trace),
        "XDG_STATE_HOME": str(state),
        "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t",
        "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t",
    }
    with WpeSession(str(tmp_path), ORIG, env_extra=env) as s:
        s.key(ALT.AI)
        s.key("g")
        s.key("rewrite it")
        s.key("\r", delay=1.0)
        assert _wait_trace(trace, "changeset n=", 60), \
            "the review never opened:\n" + _trace_text(trace)
        s.key("c", delay=0.8)
        s.key("\r", delay=1.0)
        made = _wait_trace(trace, "changeset commit", 30)
        messages = _messages_rows(s.display(drain=0))
        alive = s.alive()

    assert made, "the commit never ran:\n%s\ntrace:\n%s" % (
        "\n".join(messages), _trace_text(trace))
    assert _committed_paths(tmp_path) == ["t.c"], \
        "the commit holds the wrong files:\n%s" % _git_out(tmp_path, "show",
                                                           "--stat", "HEAD")
    status = _git_out(tmp_path, "status", "--porcelain")
    assert "M  hand.c" in status, \
        "the user's staged edit is no longer staged and uncommitted:\n" + status
    assert hand.read_text() == HAND_EDIT
    assert alive, "committing beside a staged edit ended the editor"
