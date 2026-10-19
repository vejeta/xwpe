"""AI assistant -- checkpoint + changeset review in a git workspace.

An auto-policy agent rewrites a committed file; the changeset lands in
Messages as `path:line: [AI] +a -b` (registered for Alt-T / Alt-V) and the
review loop can revert everything back to the checkpoint or keep it.

The last case runs in a workspace that is NOT a git tree, where the checkpoint
is snapshot copies and the revert writes the file itself.
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
