"""AI assistant -- sessions tied to the workspace.

claudecli: the session_id from the CLI's JSON is persisted and passed back as
`--resume <id>` on the next run (a fake `claude` on PATH records its argv);
Alt-G n forgets it.  mock: the transcript (user/assistant turns) is persisted.

With no project open the session goes to the runtime state directory, the way
ws_session_path() builds it: $XDG_STATE_HOME/xwpe/ai/<hash>.session, or
$HOME/.local/state/xwpe/ai/<hash>.session when XDG_STATE_HOME is unset or
empty.  HOME is the test workdir and XDG_STATE_HOME is forced empty, so the
file lands inside the workdir.  <hash> comes from the directory of the file
being edited, so the tests glob the directory instead of naming the file.
"""
import os
import glob
import stat
import subprocess
from pathlib import Path
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN, pane_text_with_history


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


# Force the XDG state default regardless of the runner's environment, the way
# test_xdg.py does: an empty value makes the editor fall back to
# $HOME/.local/state/xwpe, and HOME is the workdir, so the session stays
# inside this test's own directory.
_NO_XDG = {"XDG_STATE_HOME": ""}


def _run_env(env, trace):
    """Environment for one editor run.  The XDG default comes first, so a
    caller can still point XDG_STATE_HOME somewhere else on purpose."""
    run = dict(_NO_XDG)
    run.update(env)
    run.update(XWPE_AI_ENABLE="1", XWPE_AI_TRACE=str(trace))
    return run


def _chat(tmp_path, env, prompt, wait=3.0):
    trace = tmp_path / "ai.trace"
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=_run_env(env, trace)) as s:
        s.key(ALT.AI)
        s.key("a")
        s.key(prompt)
        s.key("\r", delay=1.0)
        s._drain(wait)
    return trace.read_text() if trace.exists() else ""


def _state_dir(tmp_path, env=None):
    """The runtime state directory, following ws_state_dir():
    $XDG_STATE_HOME/xwpe, or $HOME/.local/state/xwpe when that variable is
    unset or empty.  HOME is the workdir."""
    state = (env or {}).get("XDG_STATE_HOME", "")
    if state:
        return Path(state) / "xwpe"
    return tmp_path / ".local" / "state" / "xwpe"


def _session_files(tmp_path, env=None):
    """Session files belonging to this workdir.  ws_session_path() names the
    file after a hash of the edited file's directory, so the name is not
    spelled out here."""
    return sorted(glob.glob(str(_state_dir(tmp_path, env) / "ai" / "*.session")))


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

    # Alt-G n forgets the workspace session
    trace = tmp_path / "ai.trace"
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=_run_env(env, trace)) as s:
        s.key(ALT.AI)
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


REPLY = "sure, noted"


def test_ai_session_save_failure_is_said_in_the_pane(tmp_path):
    """A conversation that could not be persisted is one the pane says so about.

    XDG_STATE_HOME points below a regular file, so the state directory cannot
    be created and the session file has nowhere to go.  The pane then names the
    path and the reason the system gave, and the trace says the save failed
    instead of naming a file that is not there.  The chat itself goes on: the
    turn on screen is still usable, only the persistence was lost.
    """
    blocker = tmp_path / "blocker"
    blocker.write_text("a regular file, where a directory was needed\n")
    state = str(blocker / "state")
    trace = tmp_path / "ai.trace"
    env = _run_env({"XWPE_AI_BACKEND": "mock", "XWPE_AI_MOCK_REPLY": REPLY,
                    "XDG_STATE_HOME": state}, trace)
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key(ALT.AI)
        s.key("a")
        s.key("remember the marker ZEBRA")
        s.key("\r", delay=1.0)
        s._drain(2.0)
        pane = pane_text_with_history(s, ["could not save the conversation",
                                          "Not a directory", "noted"])
        alive = s.alive()

    # ground truth: nothing was written, and nothing went to the default place
    assert not Path(state).exists(), "the state directory was created after all"
    assert not _session_files(tmp_path), "a session file was written elsewhere"

    text = "\n".join(pane)
    assert "could not save the conversation" in text, \
        "the pane does not say the conversation was not saved:\n" + text
    assert "Not a directory" in text, \
        "the pane does not say why it was not saved:\n" + text
    # the pane wraps a long path over rows, so the spaces a wrap added are
    # dropped before the path is looked for
    flat = text.replace(" ", "").replace("\n", "")
    assert (state + "/xwpe/ai") in flat, \
        "the pane does not name the path it could not write:\n" + text
    assert "noted" in text, "the chat stopped answering:\n" + text
    assert alive, "a session that could not be saved ended the editor"

    txt = trace.read_text() if trace.exists() else ""
    assert "session save failed" in txt, \
        "the trace does not say the save failed:\n" + txt
    lying = [l for l in txt.splitlines()
             if l.startswith("session save ") and "failed" not in l]
    assert not lying, "the trace names a session file that is not there: %r" % lying
