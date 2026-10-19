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
import shutil
import subprocess
import time
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN, pane_text_with_history


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
    return trace.read_text() if trace.exists() else "", t


def test_ai_changeset_revert_all(tmp_path):
    txt, t = _run(tmp_path, "r")
    assert "checkpoint git" in txt, txt
    assert "changeset n=1" in txt, txt
    assert "changeset revert all" in txt, txt
    assert t.read_text() == ORIG, "revert did not restore the file:\n" + t.read_text()


def test_ai_changeset_keep_all(tmp_path):
    txt, t = _run(tmp_path, "a")
    assert "changeset n=1" in txt, txt
    assert "changeset keep all" in txt, txt
    assert "return 99" in t.read_text()


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
