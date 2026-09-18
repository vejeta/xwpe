"""AI assistant -- checkpoint + changeset review in a git workspace.

An auto-policy agent rewrites a committed file; the changeset lands in
Messages as `path:line: [AI] +a -b` (registered for Alt-T / Alt-V) and the
review loop can revert everything back to the checkpoint or keep it.
"""
import os
import shutil
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN


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
