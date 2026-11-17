"""Agent write_file: the open file refreshes, and the changeset lists it once.

Two regressions this covers:
- After the agent writes a file that is open in a window, the editor showed the
  OLD contents (the buffer was never reloaded), so the change was invisible.
- The end-of-run changeset listed the same file twice with an inconsistent path
  (a doubled slash from a scope-list join).

Unattended (auto) policy so the write needs no approval and the changeset review
is produced; mock backend so no model/network is involved.
"""
import os
import stat
import time
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN, pane_text_with_history
from mode_premise import special_bits_settable, workdir_in_my_group


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def test_agent_write_refreshes_open_file_and_lists_once(tmp_path):
    turns = ("TOOL write_file t.c\nint main(void){return 7;}\n@@END"
             "@@TURN@@DONE changed t.c")
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_POLICY": "auto",
           "XWPE_AI_MOCK_REPLY": turns, "XWPE_AI_TRACE": str(tmp_path / "a.trace")}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, filename="t.c") as s:
        s.key(ALT.AI); s.key("g"); s._drain(0.5)
        s.key("change the return value in t.c"); s.key("\r", delay=0.8)
        end = time.time() + 15
        while time.time() < end:
            s._drain(0.5)
            t = (tmp_path / "a.trace").read_text() if (tmp_path / "a.trace").exists() else ""
            if "changeset" in t:
                break
        disp = "\n".join(s.display())
        # A0: the open buffer shows the agent's new content, not the stale text
        assert "return 7;" in disp, "open file was not refreshed after write:\n" + disp
        # A1: the file is listed exactly once, with a clean path (no doubled slash)
        assert "1 change" in disp, "changeset did not list the write once:\n" + disp
        assert "//t.c" not in disp, "changeset path has a doubled slash:\n" + disp
        s.key("a", delay=0.5)                     # keep all -> leave the review


# --- the assistant's write is a Save (it keeps the file's bits and its old text) ---

OLD = "int main(void){return 0;}\n"
NEW_REPLY = ("TOOL write_file t.c\nint main(void){return 7;}\n@@END"
             "@@TURN@@DONE changed t.c")
WAIT_S = 30


def _agent_env(trace, turns):
    """The mock backend under the unattended policy, tracing outside the workspace."""
    return {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_POLICY": "auto",
            "XWPE_AI_MOCK_REPLY": turns, "XWPE_AI_TRACE": str(trace)}


def _trace_of(tmp_path):
    """Where the run's trace goes: beside the workspace, so it is nobody's change."""
    return tmp_path.parent / (tmp_path.name + ".trace")


def _trace_text(trace):
    return trace.read_text() if trace.exists() else ""


def _wait_trace(s, trace, needle, secs=WAIT_S):
    """Drain the editor until the trace holds `needle` (the run's own record of a step)."""
    end = time.time() + secs
    while time.time() < end:
        s._drain(0.4)
        if needle in _trace_text(trace):
            return True
    return False


def _ask(s, prompt):
    s.key(ALT.AI); s.key("g"); s._drain(0.5)
    s.key(prompt); s.key("\r", delay=0.8)


def _setgid_or_skip(path):
    """Make `path` a setgid program, or say by name why this user cannot."""
    folder = os.path.dirname(path)
    workdir_in_my_group(folder)
    special_bits_settable(folder, stat.S_ISGID)
    os.chmod(path, 0o2755)


# @scenario SC-write-owner-A-06
def test_agent_write_keeps_the_files_setgid_bit(tmp_path):
    """An approved write is a Save: the program is still a program afterwards,
    and the text it had is in the .bak beside it (t.c becomes t.bak, as for F2)."""
    t = tmp_path / "t.c"
    trace = _trace_of(tmp_path)
    with WpeSession(str(tmp_path), OLD, env_extra=_agent_env(trace, NEW_REPLY),
                    filename="t.c") as s:
        _setgid_or_skip(t)
        _ask(s, "change the return value in t.c")
        assert _wait_trace(s, trace, "changeset"), _trace_text(trace)
        s.key("a", delay=0.5)
    assert "return 7" in t.read_text(), "the write did not happen:\n" + t.read_text()
    assert stat.S_IMODE(t.stat().st_mode) == 0o2755, \
        "the write took the setgid bit off: %o" % stat.S_IMODE(t.stat().st_mode)
    bak = tmp_path / "t.bak"
    assert bak.exists(), "the old text was not kept in a .bak: %r" % sorted(os.listdir(tmp_path))
    assert bak.read_text() == OLD, "the .bak does not hold the old text:\n" + bak.read_text()
    assert stat.S_IMODE(bak.stat().st_mode) & 0o7000 == 0, "the .bak is a program too"


# @scenario SC-write-owner-H2a-08
def test_agent_write_keeps_the_inode_and_the_other_names(tmp_path):
    """The write goes into the very file: a second name of it shows the new text.

    A guard, not a red row against truncation: a write that empties the file in
    place also keeps the inode and passes.  It turns red under the mutation "write
    a temporary file and rename it over the name", which makes a new inode and
    leaves the other name with the old text."""
    t = tmp_path / "t.c"
    h = tmp_path / "h.c"
    trace = _trace_of(tmp_path)
    with WpeSession(str(tmp_path), OLD, env_extra=_agent_env(trace, NEW_REPLY),
                    filename="t.c") as s:
        os.link(t, h)
        ino = os.stat(t).st_ino
        _ask(s, "change the return value in t.c")
        assert _wait_trace(s, trace, "changeset"), _trace_text(trace)
        s.key("a", delay=0.5)
    assert os.stat(t).st_ino == ino, "the write made a new file"
    assert os.stat(h).st_ino == ino and "return 7" in h.read_text(), \
        "the other name of the file does not show the new text"


# @scenario SC-write-owner-H2a-09
def test_agent_write_into_a_pipe_does_not_hang_the_editor(tmp_path):
    """A pipe the model names is refused from its own type: the editor does not
    wait for a reader that is never coming, writes nothing into it, and the
    sentence reaches the pane.  The pipe has no source-file extension, so the
    checkpoint's own read of the workspace's files does not meet it first."""
    fifo = tmp_path / "pipe.dat"
    trace = _trace_of(tmp_path)
    turns = "TOOL write_file pipe.dat\nint x;\n@@END@@TURN@@DONE tried"
    with WpeSession(str(tmp_path), OLD, env_extra=_agent_env(trace, turns),
                    filename="t.c") as s:
        os.mkfifo(fifo)
        _ask(s, "write pipe.dat")
        assert _wait_trace(s, trace, "agent write failed"), \
            "the write never gave up (the editor waits on the pipe?):\n" + _trace_text(trace)
        assert s.alive(), "the editor died on a write into a pipe"
        pane = "\n".join(pane_text_with_history(s, ["regular file"]))
        s.key("a", delay=0.5)
    assert stat.S_ISFIFO(os.stat(fifo).st_mode), "the pipe was replaced"
    assert "not a regular file" in pane.replace("\n", " "), \
        "the pane does not say why the write was refused:\n" + pane
