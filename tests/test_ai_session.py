"""AI assistant -- sessions tied to the workspace.

claudecli: the session_id from the CLI's JSON is persisted and passed back as
`--resume <id>` on the next run (a fake `claude` on PATH records its argv);
Alt-B n forgets it.  mock: the transcript (user/assistant turns) is persisted.
HOME is the test workdir, so the session file lives under <workdir>/.xwpe/ai/.
"""
import os
import glob
import stat
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"session reset" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _fake_claude(tmp_path):
    bindir = tmp_path / "bin"
    bindir.mkdir()
    log = tmp_path / "argv.log"
    script = bindir / "claude"
    script.write_text(
        "#!/bin/sh\n"
        "cat >/dev/null\n"
        'echo "$@" >> "%s"\n' % log
        + "printf '{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,"
          "\"result\":\"PONG\",\"session_id\":\"sess-123\"}'\n")
    script.chmod(script.stat().st_mode | stat.S_IEXEC)
    return bindir, log


def _chat(tmp_path, env, prompt, wait=3.0):
    trace = tmp_path / "ai.trace"
    env = dict(env, XWPE_AI_ENABLE="1", XWPE_AI_TRACE=str(trace))
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key(ALT.BLOCK)
        s.key("a")
        s.key(prompt)
        s.key("\r", delay=1.0)
        s._drain(wait)
    return trace.read_text() if trace.exists() else ""


def _session_files(tmp_path):
    return glob.glob(str(tmp_path / ".xwpe" / "ai" / "*.session"))


def test_ai_session_claudecli_resume(tmp_path):
    bindir, log = _fake_claude(tmp_path)
    env = {"XWPE_AI_BACKEND": "claudecli",
           "PATH": str(bindir) + os.pathsep + os.environ.get("PATH", "")}

    txt = _chat(tmp_path, env, "hello")
    assert "backend=claudecli" in txt and "chat done" in txt, txt
    files = _session_files(tmp_path)
    assert files, "no session file written"
    assert "sess-123" in open(files[0]).read()
    assert "--resume" not in log.read_text(), "first run must not resume"

    txt = _chat(tmp_path, env, "again")
    assert "--resume sess-123" in log.read_text(), log.read_text()

    # Alt-B n forgets the workspace session
    trace = tmp_path / "ai.trace"
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=dict(env, XWPE_AI_ENABLE="1", XWPE_AI_TRACE=str(trace))) as s:
        s.key(ALT.BLOCK)
        s.key("n")
        s._drain(0.8)
    assert "session reset" in trace.read_text()
    assert not _session_files(tmp_path), "session file should be gone after reset"


def test_ai_session_mock_transcript(tmp_path):
    env = {"XWPE_AI_BACKEND": "mock", "XWPE_AI_MOCK_REPLY": "sure, noted"}
    txt = _chat(tmp_path, env, "remember the marker ZEBRA")
    assert "chat done" in txt, txt
    files = _session_files(tmp_path)
    assert files, "no session file written"
    body = open(files[0]).read()
    assert "ZEBRA" in body and "sure, noted" in body, body
