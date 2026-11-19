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
from host_tools import needs
from wpe_driver import WpeSession, ALT, WPE_BIN, pane_rows, pane_text_with_history
from mode_premise import special_bits_settable, workdir_in_my_group
from seen_premise import reports_birth_time
from seen_reuse import fs_type, make_folder_with_number


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"AI changeset" in out
    except Exception:
        return False


pytestmark = [
    pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai"),
    needs("git"),
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
    file back means reading its old content from git (`git show <ref>:<file>`)
    and writing it into the very file, which keeps its inode, its other names
    and its permission bits.  The file is made read-only once the review waits
    for its key: the write is refused in the system's own words, and the file
    keeps the change.  The pane must name the file and carry that reason, and
    the review's summary must count the file instead of claiming that
    everything went back.

    A read-only DIRECTORY no longer provokes this: nothing is unlinked or
    recreated, so the write into the file goes through.  The file is what has
    to refuse.
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
            t.chmod(0o444)                   # the restore cannot write the file now
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


NO_CHECKPOINT_LINE = "[AI] no checkpoint: this run cannot be reverted from the review"


def _run_without_a_before_image(tmp_path):
    """Run a rewrite with XDG_STATE_HOME below a regular file.

    The folder the snapshot copies would go into cannot be created, so the
    checkpoint is refused.  Returns (the pane's lines, the screen, whether the
    editor is alive, the trace text) and leaves the written file and the
    blocked state path for the caller to read.
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
                                          "Not a directory", "revertible",
                                          "no checkpoint"])
        disp = "\n".join(s.display(drain=0))
        alive = s.alive()
    return pane, disp, alive, _trace_text(trace)


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
    pane, disp, alive, txt = _run_without_a_before_image(tmp_path)
    t = tmp_path / "t.c"
    state = tmp_path / "blocker" / "state"

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

    assert "changeset" not in txt, \
        "a review opened over a checkpoint that holds no before image:\n" + txt
    assert not _lying_revert_lines(txt), \
        "the trace claims a revert that did not happen:\n" + txt
    assert alive, "a checkpoint that could not be saved ended the editor"


def _holds_folder_open(s, folder):
    """Does the editor the session started have a descriptor on `folder`?
    Read from the editor's own /proc entry: the session started it."""
    fds = "/proc/%d/fd" % s.proc.pid
    for name in os.listdir(fds):
        try:
            if os.readlink(os.path.join(fds, name)) == str(folder):
                return True
        except OSError:
            continue                  # a descriptor that closed while listing
    return False


# @scenario SC-name-identity-29
def test_a_checkpoint_holds_its_workspace_folder_until_it_is_refused_or_ends(tmp_path):
    """While a checkpoint is live the editor keeps the workspace folder open, so
    the folder cannot be removed and made again with its inode number.  A
    checkpoint that is refused after its folder was taken (here no before image
    could be saved) lets the folder go at once: no descriptor outlives it."""
    with _auto_run(tmp_path, "snapshot", WRITE_REPLY % (tmp_path / "t.c")) as (s, t, trace):
        assert _holds_folder_open(s, tmp_path), "a live checkpoint does not hold its folder open"
    blocker = tmp_path.parent / (tmp_path.name + "-refused")
    blocker.mkdir()
    (blocker / "regular").write_text("a regular file, where a directory was needed\n")
    refused_trace = tmp_path.parent / (tmp_path.name + "-refused.trace")
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_POLICY": "auto",
           "XWPE_AI_MOCK_REPLY": WRITE_REPLY % (blocker / "t.c"),
           "XWPE_AI_TRACE": str(refused_trace), "XDG_STATE_HOME": str(blocker / "regular" / "state")}
    with WpeSession(str(blocker), ORIG, env_extra=env) as s:
        s.key(ALT.AI)
        s.key("g")
        s.key("rewrite it")
        s.key("\r", delay=2.0)
        assert _wait_trace(refused_trace, "checkpoint refused", 60), \
            "the checkpoint was not refused:\n" + _trace_text(refused_trace)
        assert not _holds_folder_open(s, blocker), \
            "a refused checkpoint still holds its workspace folder open"
    assert "checkpoint holds workspace folder" in _trace_text(refused_trace), \
        "the refused checkpoint never held its folder, so this row proves nothing:\n" + \
        _trace_text(refused_trace)


# @scenario SC-ai-agent-65
def test_a_run_without_a_checkpoint_says_it_cannot_be_reverted(tmp_path):
    """The pane says what a refused checkpoint costs, not only why it failed.

    The folder error told the user what to fix; nothing connected it to the
    consequence, that the edits this run makes cannot be undone through the
    review.  The pane now says so in its own line, after the folder error and
    before the first write, so the user can still stop the run or lower the
    dial.
    """
    pane, disp, alive, txt = _run_without_a_before_image(tmp_path)
    text = "\n".join(pane)
    assert "could not create the folder" in text, \
        "the pane does not say the checkpoint folder is missing:\n" + text
    assert NO_CHECKPOINT_LINE in text, \
        "the pane does not say the run cannot be reverted:\n" + text
    assert text.index("could not create the folder") < text.index(NO_CHECKPOINT_LINE), \
        "the consequence came before its cause:\n" + text
    assert "checkpoint taken" not in text, \
        "the pane promises a checkpoint it refused:\n" + text
    assert "checkpoint refused" in txt, txt
    assert alive, "a refused checkpoint ended the editor"


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


# ---- the keys the review offers ---------------------------------------------
#
# The review's key line is read as Messages rows, because that window is where
# the user reads it, and at 80 columns with the pane docked it is 38 columns
# wide and cuts a row at its border.

KEYS_ROW_1 = "   a = keep all   r = REVERT all"
KEYS_ROW_2_GIT = "   f = file by file   c = commit"
KEYS_ROW_2_SNAPSHOT = "   f = file by file"


def _key_rows(review):
    """The Messages rows after the changeset rows: the review's key rows, as the
    window shows them, cursor block and frame edge removed."""
    messages = _messages_rows(review)
    rows = []
    for i, r in enumerate(messages):
        if r.startswith("=== AI changeset"):
            rows = [_row_text(x) for x in messages[i + 1:]]
            break
    return [r for r in rows if r.strip().startswith(("a = ", "f = "))]


# @scenario SC-ai-agent-70
@pytest.mark.parametrize("git", [True, False], ids=["git", "snapshot"])
def test_the_review_keys_fit_the_messages_window(tmp_path, git):
    """The review's keys are two rows the narrowest Messages window shows whole,
    and `c = commit` is offered only where a commit can be made.

    One 72-character row was cut at 38 columns before `c = commit (git)`, so
    the one key the manual presents as the point of a git workspace was
    invisible in the default layout.  Two rows of the same words fit; and in a
    snapshot workspace, where `c` does nothing, the second row ends at
    `f = file by file`, so no key that does nothing is shown.  The git case is
    read at 80 columns with the pane docked, the snapshot case at 200, where a
    stray `c = commit` would have room to appear.
    """
    if git:
        review, _, txt, alive = _run_changed_line(tmp_path, git=True,
                                                  cols=80, rows=30)
        want = [KEYS_ROW_1, KEYS_ROW_2_GIT]
    else:
        review, _, txt, alive = _run_changed_line(tmp_path, git=False)
        want = [KEYS_ROW_1, KEYS_ROW_2_SNAPSHOT]
    rows = _key_rows(review)
    assert rows == want, \
        "the review's key rows read %r, expected %r:\n%s" % (
            rows, want, "\n".join(_messages_rows(review)))
    assert "changeset n=1" in txt, txt
    assert alive, "the review ended the editor"


# ---- a commit git refused after the add -------------------------------------

HOOK_SAYS_NO = "#!/bin/sh\necho 'pre-commit hook says no' >&2\nexit 1\n"
STAY_STAGED_LINE = "the listed files stay staged"


# @scenario SC-ai-agent-63
def test_a_refused_commit_says_the_files_stay_staged(tmp_path):
    """When a hook refuses the commit, the staged files stay and the pane says so.

    The add had succeeded, so the listed files sat in the index with no
    commit; the pane gave git's line and Messages said no commit was made,
    but nothing said where the files were left, and the user's next commit
    would have carried them.  The pane now says the files stay staged, which
    is also what a retry after fixing the hook's complaint needs, and the
    index is not touched further.
    """
    t = tmp_path / "t.c"
    _git_repo(tmp_path)                       # commits t.c holding ORIG
    hook = tmp_path / ".git" / "hooks" / "pre-commit"
    hook.write_text(HOOK_SAYS_NO)
    hook.chmod(0o755)
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
        refused = _wait_trace(trace, "changeset not committed", 30)
        pane = pane_text_with_history(s, ["could not commit", "stay staged"])
        messages = _messages_rows(s.display(drain=0))
        alive = s.alive()

    assert refused, "the refusal never reached the trace:\n" + _trace_text(trace)
    text = "\n".join(pane)
    assert "could not commit the changeset: pre-commit hook says no" in text, \
        "the pane does not give git's reason:\n" + text
    assert STAY_STAGED_LINE in text, \
        "the pane does not say where the files were left:\n" + text
    assert any("[AI] not committed" in r for r in messages), \
        "Messages does not say the commit was not made:\n" + "\n".join(messages)
    assert _git_out(tmp_path, "log", "--oneline").count("\n") == 1, \
        "a commit was made after all:\n" + _git_out(tmp_path, "log", "--oneline")
    assert "M  t.c" in _git_out(tmp_path, "status", "--porcelain"), \
        "the listed file is not staged:\n" + _git_out(tmp_path, "status",
                                                      "--porcelain")
    assert alive, "a refused commit ended the editor"


# --- the assistant's write is a Save; the revert puts back what the file was ---

import contextlib
import stat

WORKSPACES = ["snapshot", "git"]
REVERT_WAIT_S = 30
# The hook of the owner of "is this the file I saw": every look drops the birth
# time, as on OpenBSD, ext3 or most FUSE.
NO_BIRTH_ENV = {"XWPE_TEST_FAIL": "seen-no-birth"}
CHANGED_WORDS = "was changed or replaced since this window read it"
REPLACED_WORDS = "was replaced on disk since this window read it"
REFUSED_SETGID_WORDS = ("it was changed or replaced since the window showed it, and it has a "
                        "setuid, setgid or sticky bit")


@contextlib.contextmanager
def _auto_run(tmp_path, workspace, reply, seed=ORIG, prepare=None, answer=None, sub=None,
              env_extra=None, review=True, answers=()):
    """An unattended run over t.c, stopped at the review's prompt.

    Yields (session, t.c, trace).  `workspace` is "git" (a committed t.c, so the
    checkpoint is a git ref) or "snapshot" (a plain folder, so the checkpoint is
    copies).  `prepare(t)` runs once the editor has read the file and before the
    run starts: where a row sets up the file's bits or another name for it.  The
    trace and the state folder stay outside the workspace, so neither is a change.
    `answer` is a key for the question the write asks first (a file with several
    names asks at every dial); it is pressed once the question is on screen.
    `sub` names a folder under tmp_path that is the workspace, with the editor
    started one folder above it (a row that removes the workspace folder must
    not leave the editor standing in it, which would keep its inode alive).
    `env_extra` adds to the editor's environment (a row that plays a system
    with no birth time).  `review` False is a run whose write is refused, so the
    review has nothing to offer: the row waits for the empty review's trace line
    instead of its prompt.  `answers` is a pair of keys for a file with several
    names that the window cannot prove its own: the names question, then the
    "changed or replaced" box (see _answer_linked_then_changed).
    """
    t = tmp_path / "t.c" if sub is None else tmp_path / sub / "t.c"
    if sub is not None:
        (tmp_path / sub).mkdir()
    trace = tmp_path.parent / (tmp_path.name + ".trace")
    state = tmp_path.parent / (tmp_path.name + ".state")
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_MOCK_REPLY": reply,
           "XWPE_AI_POLICY": "auto", "XWPE_AI_TRACE": str(trace),
           "XDG_STATE_HOME": str(state)}
    env.update(env_extra or {})
    if workspace == "git":
        _git_repo(tmp_path)
        if isinstance(seed, bytes):
            raise ValueError("a git row seeds its file with the commit's own text")
    filename = "t.c" if sub is None else sub + "/t.c"
    with WpeSession(str(tmp_path), seed, env_extra=env, filename=filename) as s:
        if prepare:
            prepare(t)
        s.key(ALT.AI)
        s.key("g")
        s.key("rewrite it")
        s.key("\r", delay=2.0)
        if answer:
            assert _wait_trace(trace, "agent write linked", 60), \
                "the write never asked about the file's names:\n" + _trace_text(trace)
            s.key(answer, delay=0.8)
        if answers:
            _answer_linked_then_changed(s, trace, answers)
        if review:
            assert _wait_trace(trace, "changeset n=", 60), \
                "the review never opened:\n" + _trace_text(trace)
        else:
            assert _wait_trace(trace, "changeset empty", 60), \
                "the run did not end with an empty review:\n" + _trace_text(trace)
        yield s, t, trace


def _answer_linked_then_changed(s, trace, answers):
    """Answer the two questions of a file with several names whose status the
    window cannot prove (no birth time): the names question, then the box that
    says the file was "changed or replaced since this window read it" and never
    "replaced on disk"."""
    first, second = answers
    assert _wait_trace(trace, "agent write linked", 60), \
        "the write never asked about the file's names:\n" + _trace_text(trace)
    s.key(first, delay=0.8)
    assert _wait_trace(trace, "agent write replaced", 60), \
        "no second question about the file the window read:\n" + _trace_text(trace)
    assert _wait_screen_text(s, CHANGED_WORDS), \
        "the second box does not say changed or replaced:\n" + "\n".join(s.display())
    assert REPLACED_WORDS not in "\n".join(s.display()), \
        "the second box claims a replacement it cannot know"
    s.key(second, delay=0.8)


def _wait_screen_text(s, needle, secs=30):
    """Drain the editor until `needle` is on the screen."""
    end = time.time() + secs
    while time.time() < end:
        s._drain(0.4)
        if needle in "\n".join(s.display()):
            return True
    return False


def _revert_all(s, trace):
    s.key("r", delay=1.0)
    assert _wait_trace(trace, "changeset revert", REVERT_WAIT_S), \
        "the revert never ran:\n" + _trace_text(trace)
    s._drain(1.0)


def _set_setgid(t):
    """Make `t` a setgid program, or say by name why this user cannot."""
    folder = os.path.dirname(t)
    workdir_in_my_group(folder)
    special_bits_settable(folder, stat.S_ISGID)
    os.chmod(t, 0o2755)


WRITE_REPLY = ("TOOL write_file %s\nint main(void){return 99;}\n@@END"
               "@@TURN@@DONE rewrote it")


# @scenario SC-write-owner-A-06
@pytest.mark.parametrize("workspace", WORKSPACES)
def test_revert_puts_back_the_bits_the_file_had(tmp_path, workspace):
    """A program stays a program through the run and its revert.

    The file is setgid before the run.  The assistant's write keeps the bit (it
    is a Save); the revert puts the before image back wearing the bits recorded
    before the first write, in the very same file, in a git workspace (the
    content comes from git) as in a plain folder (from the checkpoint's copy).

    Where the system keeps no birth time the file, made setgid after the window
    read it, is not proven to be the window's own: the write is refused, there
    is nothing to review and nothing to revert (_refused_setgid_run).
    """
    if not reports_birth_time(tmp_path.parent, {}):
        _refused_setgid_run(tmp_path, workspace, WRITE_REPLY % (tmp_path / "t.c"), 0o2755)
        return
    ino = []

    def prepare(t):
        _set_setgid(t)
        ino.append(os.stat(t).st_ino)

    with _auto_run(tmp_path, workspace, WRITE_REPLY % (tmp_path / "t.c"),
                   prepare=prepare) as (s, t, trace):
        assert stat.S_IMODE(t.stat().st_mode) == 0o2755, \
            "the write took the setgid bit off before the review: %o" % \
            stat.S_IMODE(t.stat().st_mode)
        _revert_all(s, trace)
    assert t.read_text() == ORIG, "the revert did not restore the text:\n" + t.read_text()
    assert stat.S_IMODE(t.stat().st_mode) == 0o2755, \
        "the revert did not put the file's bits back: %o" % stat.S_IMODE(t.stat().st_mode)
    assert t.stat().st_ino == ino[0], "the revert made a new file"


# @scenario SC-write-owner-A-06
@pytest.mark.parametrize("workspace", WORKSPACES)
def test_revert_puts_back_the_recorded_bits_after_a_command_took_them_off(tmp_path, workspace):
    """The bits come from the record made before the first write, not from what
    the file has when the revert runs.

    The run writes the setgid file (the record is made then) and then a command
    of the run takes every special bit off.  The revert puts the text back and
    the record's bits with it.

    Where the system keeps no birth time the write is refused (the file, made
    setgid after the window read it, is not proven the window's own), so the
    command only takes the bits off a file whose text never changed
    (_refused_setgid_run).
    """
    t = tmp_path / "t.c"
    reply = (WRITE_REPLY % t).replace("@@TURN@@DONE rewrote it",
                                      "@@TURN@@TOOL run_command chmod 0755 %s@@TURN@@DONE rewrote it" % t)
    if not reports_birth_time(tmp_path.parent, {}):
        _refused_setgid_run(tmp_path, workspace, reply, 0o755)
        return
    with _auto_run(tmp_path, workspace, reply, prepare=_set_setgid) as (s, t, trace):
        assert stat.S_IMODE(t.stat().st_mode) == 0o755, \
            "the command did not take the bits off: %o" % stat.S_IMODE(t.stat().st_mode)
        _revert_all(s, trace)
    assert t.read_text() == ORIG
    assert stat.S_IMODE(t.stat().st_mode) == 0o2755, \
        "the revert did not give back the recorded bits: %o" % stat.S_IMODE(t.stat().st_mode)


# @scenario SC-write-owner-H2a-12
@pytest.mark.parametrize("workspace", WORKSPACES)
def test_revert_keeps_the_bits_of_a_file_only_a_command_changed(tmp_path, workspace):
    """A file the run never wrote has no record: the revert leaves its bits as
    they are (here 0600, which git's 100644 would have widened).  The command
    changes the file's text; the assistant's own write goes to another file.

    Where the system keeps no birth time the file, chmod'ed after the window
    read it, is not proven the window's own, so the revert is refused
    (_revert_refused_for_a_touched_window_file)."""
    if not reports_birth_time(tmp_path.parent, {}):
        _revert_refused_for_a_touched_window_file(tmp_path, workspace, {})
        return
    t = tmp_path / "t.c"
    reply = ("TOOL run_command printf 'int main(void){return 5;}\\n' > %s@@TURN@@DONE changed it" % t)
    with _auto_run(tmp_path, workspace, reply, prepare=lambda p: os.chmod(p, 0o600)) as (s, t, trace):
        assert "return 5" in t.read_text(), "the command did not change the file"
        _revert_all(s, trace)
    assert t.read_text() == ORIG
    assert stat.S_IMODE(t.stat().st_mode) == 0o600, \
        "the revert gave the file another file's bits: %o" % stat.S_IMODE(t.stat().st_mode)


def _refused_setgid_run(tmp_path, workspace, reply, mode_after):
    """A run over a setgid t.c that the window read before it was made setgid,
    on a system with no birth time: the write is refused with the reason, the
    review is empty and there is no revert.  The text and the .bak are as they
    were, the file's mode is `mode_after` (the bit the file had, or what a
    command left)."""
    ino = []

    def prepare(t):
        _set_setgid(t)
        ino.append(os.stat(t).st_ino)

    with _auto_run(tmp_path, workspace, reply, prepare=prepare, review=False) as (s, t, trace):
        txt = _trace_text(trace)
        assert "agent write failed" in txt and REFUSED_SETGID_WORDS in txt, \
            "the refusal is not counted and named:\n" + txt
    assert t.read_text() == ORIG, "the write changed a file it could not prove the window's"
    assert stat.S_IMODE(t.stat().st_mode) == mode_after, \
        "the file's mode is not %o: %o" % (mode_after, stat.S_IMODE(t.stat().st_mode))
    assert t.stat().st_ino == ino[0], "the run made a new file"
    assert not (tmp_path / "t.bak").exists(), "a refused write left a .bak"


def _revert_refused_for_a_touched_window_file(tmp_path, workspace, env_extra):
    """On a system that records no time of making, a file the window read and a
    program then touched in place (here its mode) cannot be told from a new file
    with its number.  The revert puts nothing back into it, says it was changed
    or replaced, and leaves the file as the command left it.  The outcome of
    test_revert_keeps_the_bits_of_a_file_only_a_command_changed with no birth time."""
    t = tmp_path / "t.c"
    reply = ("TOOL run_command printf 'int main(void){return 5;}\\n' > %s@@TURN@@DONE changed it" % t)
    with _auto_run(tmp_path, workspace, reply, prepare=lambda p: os.chmod(p, 0o600),
                   env_extra=env_extra) as (s, t, trace):
        assert "return 5" in t.read_text(), "the command did not change the file"
        _revert_all(s, trace)
        pane = " ".join(pane_text_with_history(s, ["changed or replaced"]))
    txt = _trace_text(trace)
    assert "return 5" in t.read_text(), "the revert wrote into a file it could not prove the window's"
    assert "changeset revert failed" in txt and "changed or replaced" in txt, \
        "the refused revert is not counted and named:\n" + txt
    assert "changed or replaced" in pane, "the pane does not name the refusal:\n" + pane


# @scenario SC-name-identity-25
@pytest.mark.parametrize("workspace", WORKSPACES)
def test_revert_of_a_window_file_touched_since_the_read_is_refused_without_a_birth_time(
        tmp_path, workspace):
    """The no-birth outcome of test_revert_keeps_the_bits_of_a_file_only_a_command_changed,
    forced on a host that does have a birth time."""
    _revert_refused_for_a_touched_window_file(tmp_path, workspace, NO_BIRTH_ENV)


# @scenario SC-write-owner-A-07
@pytest.mark.parametrize("workspace", WORKSPACES)
def test_the_backup_of_an_agent_write_is_not_reviewed_and_survives_a_revert(tmp_path, workspace):
    """The .bak a write makes (t.c becomes t.bak) is the price of a write being
    a Save, not the run's work: the review lists one change (t.c) and never
    t.bak, and a revert neither removes it nor restores it."""
    with _auto_run(tmp_path, workspace, WRITE_REPLY % (tmp_path / "t.c")) as (s, t, trace):
        bak = tmp_path / "t.bak"
        assert bak.read_text() == ORIG, "the write kept no .bak of the old text"
        txt = _trace_text(trace)
        assert "changeset n=1" in txt, "the review lists more than the one file:\n" + txt
        shown = "\n".join(s.display(drain=0))
        assert "t.bak" not in shown, "the review lists the .bak:\n" + shown
        _revert_all(s, trace)
    assert t.read_text() == ORIG
    assert bak.exists(), "the revert removed the .bak"
    assert bak.read_text() == ORIG, "the revert changed the .bak:\n" + bak.read_text()


# @scenario SC-write-owner-H2a-10
def test_snapshot_revert_puts_back_a_before_image_that_holds_a_nul_byte(tmp_path):
    """A before image with a NUL byte comes back whole: the checkpoint copied it
    byte for byte and the revert writes all of it, never up to the first NUL."""
    image = b"a\x00b\nint main(void){return 0;}\n"
    with _auto_run(tmp_path, "snapshot", WRITE_REPLY % (tmp_path / "t.c"), seed=image) as (s, t, trace):
        assert b"return 99" in t.read_bytes(), "the run did not write the file"
        _revert_all(s, trace)
    assert t.read_bytes() == image, "the revert cut the before image short: %r" % t.read_bytes()


def _commit_all(tmp_path, message):
    """Commit what a row set up after the workspace was made."""
    for args in (("add", "-A"), ("commit", "-q", "-m", message)):
        subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t",
                        "-c", "commit.gpgsign=false", *args],
                       cwd=tmp_path, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def _victim(tmp_path):
    """A file of the user's outside the workspace, which no revert may touch."""
    victim = tmp_path.parent / (tmp_path.name + ".victim")
    victim.write_text("USER DATA\n")
    return victim


# @scenario SC-write-owner-H2a-16
def test_tracked_file_replaced_by_link_to_outside(tmp_path):
    """A command of the run replaces the tracked t.c by a link to a file outside.
    The revert must not write through the link: the outside file keeps its bytes
    and t.c is a regular file with the committed text again."""
    victim = _victim(tmp_path)
    reply = "TOOL run_command ln -sf %s t.c@@TURN@@DONE ok" % victim
    with _auto_run(tmp_path, "git", reply) as (s, t, trace):
        _revert_all(s, trace)
        s._drain(1.0)
    assert victim.read_text() == "USER DATA\n", \
        "the revert wrote into a file outside the workspace:\n" + victim.read_text()
    assert not os.path.islink(t) and t.read_text() == ORIG, \
        "t.c is not the committed file again"
    assert "changeset revert failed" not in _trace_text(trace), _trace_text(trace)


# @scenario SC-write-owner-H2a-17
def test_tracked_symlink_retargeted(tmp_path):
    """A tracked link retargeted by a command comes back as the link it was; the
    file it was retargeted to is not written."""
    def prepare(t):
        (tmp_path / "other.c").write_text("OTHER\n")
        os.symlink("t.c", tmp_path / "l.c")
        _commit_all(tmp_path, "link")

    reply = "TOOL run_command ln -sfn other.c l.c@@TURN@@DONE ok"
    with _auto_run(tmp_path, "git", reply, prepare=prepare) as (s, t, trace):
        _revert_all(s, trace)
        s._drain(1.0)
    assert (tmp_path / "other.c").read_text() == "OTHER\n", "the revert wrote into other.c"
    link = tmp_path / "l.c"
    assert os.path.islink(link) and os.readlink(link) == "t.c", \
        "the link did not come back where it was"


# @scenario SC-write-owner-H2a-18
def test_tracked_symlink_replaced_by_file(tmp_path):
    """A tracked link replaced by a regular file comes back as the link, not as a
    file holding the link's target string."""
    def prepare(t):
        os.symlink("t.c", tmp_path / "l.c")
        _commit_all(tmp_path, "link")

    reply = "TOOL run_command rm l.c && printf 'x\\n' > l.c@@TURN@@DONE ok"
    with _auto_run(tmp_path, "git", reply, prepare=prepare) as (s, t, trace):
        _revert_all(s, trace)
        s._drain(1.0)
    link = tmp_path / "l.c"
    assert os.path.islink(link) and os.readlink(link) == "t.c", \
        "the link did not come back: %r" % (link.read_text() if link.exists() else None)


# @scenario SC-write-owner-H2a-19
def test_snapshot_revert_refuses_a_link_planted_at_the_name(tmp_path):
    """In a plain folder a command plants a link to a file outside at t.c.  The
    revert refuses it, says so by file and reason, counts the file as kept, and
    the outside file keeps its bytes."""
    victim = _victim(tmp_path)
    reply = "TOOL run_command ln -sf %s t.c@@TURN@@DONE ok" % victim
    with _auto_run(tmp_path, "snapshot", reply) as (s, t, trace):
        s.key("r", delay=1.0)
        assert _wait_trace(trace, "changeset revert", REVERT_WAIT_S), _trace_text(trace)
        pane = "\n".join(pane_text_with_history(s, ["could not restore", "link"]))
        shown = "\n".join(s.display(drain=0))
    assert victim.read_text() == "USER DATA\n", \
        "the revert wrote into a file outside the workspace:\n" + victim.read_text()
    assert os.path.islink(t), "the planted link was replaced after all"
    txt = _trace_text(trace)
    assert "changeset revert failed" in txt, "the refused restore is not counted:\n" + txt
    assert "could not restore" in pane and "link" in pane, \
        "the pane does not name the refusal:\n" + pane
    assert "ALL changes reverted" not in shown, \
        "the review claims everything went back:\n" + shown


FOLDER_LINK_WORDS = "link has taken the place of a folder"


def _outside_folder(tmp_path):
    """A folder of the user's outside the workspace, which no revert may touch."""
    out = tmp_path.parent / (tmp_path.name + ".out")
    out.mkdir()
    return out


def _seed_sub(tmp_path):
    """A tracked sub/t.c, committed."""
    (tmp_path / "sub").mkdir()
    (tmp_path / "sub" / "t.c").write_text(ORIG)
    _commit_all(tmp_path, "sub")


def _plant_folder_link(out):
    """The run's command: sub moves aside and a link to `out` takes its name."""
    return "TOOL run_command mv sub sub.x && ln -s %s sub@@TURN@@DONE ok" % out


# @scenario SC-write-owner-H2a-20
def test_git_parent_link_regular(tmp_path):
    """A command replaces the tracked folder sub by a link to a folder outside the
    workspace, which holds its own t.c.  The revert of sub/t.c must not write
    there: the outside file keeps its bytes, and the failure is counted and
    named (git refuses a path beyond a link)."""
    out = _outside_folder(tmp_path)
    (out / "t.c").write_text("USER DATA\n")
    with _auto_run(tmp_path, "git", _plant_folder_link(out),
                   prepare=lambda t: _seed_sub(tmp_path)) as (s, t, trace):
        _revert_all(s, trace)
        pane = " ".join(pane_text_with_history(s, [FOLDER_LINK_WORDS]))
    assert (out / "t.c").read_text() == "USER DATA\n", \
        "the revert wrote through a link planted in place of a folder"
    txt = _trace_text(trace)
    assert "changeset revert failed %s" % (tmp_path / "sub" / "t.c") in txt, \
        "the refused restore of sub/t.c is not counted:\n" + txt
    assert FOLDER_LINK_WORDS in pane, "the pane does not name the refusal:\n" + pane


# @scenario SC-write-owner-H2a-21
def test_git_parent_link_leaf_link(tmp_path):
    """The folder link leads to a folder where t.c is itself a link to a user's
    file.  Neither the file nor the link outside is touched."""
    out = _outside_folder(tmp_path)
    (out / "victim").write_text("USER DATA\n")
    os.symlink(out / "victim", out / "t.c")
    with _auto_run(tmp_path, "git", _plant_folder_link(out),
                   prepare=lambda t: _seed_sub(tmp_path)) as (s, t, trace):
        _revert_all(s, trace)
        pane = " ".join(pane_text_with_history(s, [FOLDER_LINK_WORDS]))
    assert (out / "victim").read_text() == "USER DATA\n"
    assert os.path.islink(out / "t.c"), "the revert replaced a file outside the workspace"
    assert "changeset revert failed %s" % (tmp_path / "sub" / "t.c") in _trace_text(trace), \
        _trace_text(trace)
    assert FOLDER_LINK_WORDS in pane, pane


ROOT_LINK_WORDS = "workspace folder was replaced by a link"
ROOT_LINK_PANE_WORDS = "workspace folder was"


def _plant_root_link(tmp_path, out):
    """The run's command: the workspace folder moves aside and a link to `out`
    takes its name."""
    name = tmp_path.name
    return "TOOL run_command cd .. && mv %s %s.x && ln -s %s %s@@TURN@@DONE ok" % (name, name, out, name)


def _outside_with_files(tmp_path):
    """A folder of the user's outside the workspace: a t.c with their bytes and a
    v.txt that no revert may remove."""
    out = _outside_folder(tmp_path)
    (out / "t.c").write_text("USER DATA\n")
    (out / "v.txt").write_text("VICTIM\n")
    return out


def _assert_root_swap_refused(out, trace, pane):
    assert (out / "t.c").read_text() == "USER DATA\n", \
        "the revert wrote into the folder the link leads to:\n" + (out / "t.c").read_text()
    assert (out / "v.txt").exists(), "the revert removed a file of the folder the link leads to"
    txt = _trace_text(trace)
    assert "changeset revert failed" in txt, "the refused revert is not counted:\n" + txt
    assert ROOT_LINK_WORDS in txt, "the trace does not name the refusal:\n" + txt
    # the pane wraps the sentence at the window's width, so only its first
    # words are looked for there
    assert ROOT_LINK_PANE_WORDS in pane, "the pane does not name the refusal:\n" + pane


# @scenario SC-write-owner-H2a-23
def test_snapshot_workspace_root_swapped_for_link(tmp_path):
    """Plain folder.  The run's command moves the workspace folder aside and puts
    a link to a folder of the user's in its place.  The revert writes nothing
    and removes nothing there, and says why."""
    out = _outside_with_files(tmp_path)
    with _auto_run(tmp_path, "snapshot", _plant_root_link(tmp_path, out)) as (s, t, trace):
        _revert_all(s, trace)
        pane = " ".join(pane_text_with_history(s, [ROOT_LINK_WORDS]))
    _assert_root_swap_refused(out, trace, pane)


# @scenario SC-write-owner-H2a-24
def test_git_workspace_root_swapped_for_link(tmp_path):
    """The same in a git workspace, where the folder the link leads to is a
    repository of its own with a changed t.c and a new v.txt."""
    out = _outside_with_files(tmp_path)
    (out / "v.txt").unlink()
    _git_repo(out)
    (out / "t.c").write_text("USER DATA\n")
    (out / "v.txt").write_text("VICTIM\n")
    with _auto_run(tmp_path, "git", _plant_root_link(tmp_path, out)) as (s, t, trace):
        _revert_all(s, trace)
        pane = " ".join(pane_text_with_history(s, [ROOT_LINK_WORDS]))
    _assert_root_swap_refused(out, trace, pane)


# @scenario SC-write-owner-H2a-25
def test_snapshot_workspace_link_retargeted(tmp_path):
    """The workspace is opened through the user's own link ln -> real, on the
    absolute name ln/t.c.  The run retargets ln to a folder of the user's
    outside.  The workspace's real folder has not changed, so only the check
    that every folder on the way still leads where the review listed it stands
    between the revert and that folder: its t.c keeps its bytes, its v.txt is
    not removed, and both refusals are counted and named."""
    real = tmp_path / "real"
    real.mkdir()
    ln = tmp_path / "ln"
    os.symlink(real, ln)
    out = _outside_with_files(tmp_path)
    reply = "TOOL run_command ln -sfn %s %s@@TURN@@DONE ok" % (out, ln)
    trace = tmp_path / "retarget.trace"
    state = tmp_path / "retarget.state"
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_MOCK_REPLY": reply,
           "XWPE_AI_POLICY": "auto", "XWPE_AI_TRACE": str(trace),
           "XDG_STATE_HOME": str(state)}
    with WpeSession(str(ln), ORIG, filename=str(ln / "t.c"), env_extra=env) as s:
        s.key(ALT.AI)
        s.key("g")
        s.key("rewrite it")
        s.key("\r", delay=2.0)
        assert _wait_trace(trace, "changeset n=", 60), \
            "the review never opened:\n" + _trace_text(trace)
        _revert_all(s, trace)
        pane = " ".join(pane_text_with_history(s, [FOLDER_LINK_WORDS]))
    assert (out / "t.c").read_text() == "USER DATA\n", \
        "the revert wrote into the folder the retargeted link leads to"
    assert (out / "v.txt").exists(), "the revert removed a file of the folder the link leads to"
    txt = _trace_text(trace)
    assert "changeset revert failed %s" % (ln / "t.c") in txt, txt
    assert "changeset revert failed %s" % (ln / "v.txt") in txt, txt
    assert FOLDER_LINK_WORDS in pane, "the pane does not name the refusal:\n" + pane


# @scenario SC-write-owner-H2a-22
def test_snapshot_parent_link(tmp_path):
    """The same in a plain folder: the outside file keeps its bytes."""
    out = _outside_folder(tmp_path)
    (out / "t.c").write_text("USER DATA\n")
    (tmp_path / "sub").mkdir()
    (tmp_path / "sub" / "t.c").write_text(ORIG)
    with _auto_run(tmp_path, "snapshot", _plant_folder_link(out)) as (s, t, trace):
        _revert_all(s, trace)
    assert (out / "t.c").read_text() == "USER DATA\n", \
        "the revert wrote through a link planted in place of a folder"


# @scenario SC-write-owner-H2a-11
def test_git_revert_keeps_the_inode_and_the_other_names(tmp_path):
    """The git revert writes git's copy into the very file: the inode is the same
    after it, and a second name of the file shows the restored text too (a
    checkout would have made a new file and left the other name with the change).

    Where the system keeps no birth time the name made after the window read the
    file moves its status time, so the write asks a second question, "changed or
    replaced", and the row answers both."""
    ino = []

    def prepare(t):
        os.link(t, tmp_path / "h.c")
        ino.append(os.stat(t).st_ino)

    # one more key than before the question existed: the write to a file with
    # two names asks first, and this row is about the revert that follows a yes
    asking = {"answer": "y"}
    if not reports_birth_time(tmp_path.parent, {}):
        asking = {"answers": ("y", "y")}
    with _auto_run(tmp_path, "git", WRITE_REPLY % (tmp_path / "t.c"), prepare=prepare,
                   **asking) as (s, t, trace):
        _revert_all(s, trace)
    assert t.read_text() == ORIG
    assert t.stat().st_ino == ino[0], "the revert made a new file"
    h = tmp_path / "h.c"
    assert h.stat().st_ino == ino[0] and h.read_text() == ORIG, \
        "the other name of the file does not show the restored text:\n" + h.read_text()


OTHERS_TEXT = "somebody else's t.c\n"


REMADE_WORKSPACE = "ws"
REMADE_FILE = "n.c"     # a file the run creates, which no window has open


def _remake_the_folder_with_its_number(folder):
    """With the review open: remove the workspace folder and make another under
    its name, keeping every folder that lands below the old number aside until
    one has it, and put an n.c of somebody else's in it.  Returns whether the new
    folder has the old folder's number (it cannot while the checkpoint holds the
    old folder open)."""
    old = os.stat(folder).st_ino
    shutil.rmtree(folder)
    fillers, reached = make_folder_with_number(folder, old)
    (folder / REMADE_FILE).write_text(OTHERS_TEXT)
    for filler in fillers:
        os.rmdir(filler)
    return reached


# @scenario SC-name-identity-10
def test_a_workspace_folder_made_again_with_its_number_is_not_the_checkpoints(tmp_path):
    """The review is open.  The folder the checkpoint saw is removed and another
    is made under its name, with the old folder's inode number when the file
    system gives it back.  The revert touches nothing in the new folder: the
    checkpoint holds the old one open, so no other folder can take its number
    (the new folder has another), and the name no longer leads to the held
    folder.

    Red on the tree before the checkpoint held its folder: nothing held the old
    folder, the file system gave its number to the new one on ext4, the number
    alone said "the same folder" and the revert removed the n.c it found there
    as "the file the run created".  The editor stands one folder above the
    workspace, so the editor itself does not keep the old inode from being
    given back, and the file is one no window has open (a window's own identity
    check would refuse the revert by itself)."""
    ws = tmp_path / REMADE_WORKSPACE
    n = ws / REMADE_FILE
    print("row SC-name-identity-10 ran on %s" % fs_type(tmp_path.parent))
    with _auto_run(tmp_path, "snapshot", WRITE_REPLY % n, sub=REMADE_WORKSPACE) as (s, t, trace):
        assert "checkpoint holds workspace folder %s" % ws in _trace_text(trace), \
            "the checkpoint does not hold its workspace folder:\n" + _trace_text(trace)
        _remake_the_folder_with_its_number(ws)
        _revert_all(s, trace)
    assert n.exists() and n.read_text() == OTHERS_TEXT, \
        "the revert removed a file of a folder that only had the checkpoint's number (%s):\n%s" \
        % (fs_type(tmp_path.parent), _trace_text(trace))
