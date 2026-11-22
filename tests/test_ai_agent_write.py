"""Agent write_file: the open file refreshes, and the changeset lists it once.

Two regressions this covers:
- After the agent writes a file that is open in a window, the editor showed the
  OLD contents (the buffer was never reloaded), so the change was invisible.
- The end-of-run changeset listed the same file twice with an inconsistent path
  (a doubled slash from a scope-list join).

Unattended (auto) policy so the write needs no approval and the changeset review
is produced; mock backend so no model/network is involved.
"""
import functools
import os
import stat
import time
import subprocess
import pytest
import gui_owner
from wpe_driver import WpeSession, ALT, WPE_BIN, pane_text_with_history, \
    wait_deadline, wait_screen
from mode_premise import special_bits_settable, workdir_in_my_group
from seen_premise import reports_birth_time
from seen_reuse import fs_type, make_file_with_number, reached_or_say


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


def _agent_env(trace, turns, policy="auto"):
    """The mock backend under the unattended policy (or the one given), tracing
    outside the workspace."""
    return {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_POLICY": policy,
            "XWPE_AI_MOCK_REPLY": turns, "XWPE_AI_TRACE": str(trace)}


def _trace_of(tmp_path):
    """Where the run's trace goes: beside the workspace, so it is nobody's change."""
    return tmp_path.parent / (tmp_path.name + ".trace")


def _trace_text(trace):
    return trace.read_text() if trace.exists() else ""


def _wait_trace(s, trace, needle, secs=WAIT_S):
    """Drain the editor until the trace holds `needle` (the run's own record of a step)."""
    end = wait_deadline(secs)
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
    and the text it had is in the .bak beside it (t.c becomes t.bak, as for F2).

    Where the system keeps no birth time the file, made setgid after the window
    read it, is not proven to be the window's own: the write is refused with the
    reason (_refused_setgid_write)."""
    if not reports_birth_time(tmp_path.parent, {}):
        _refused_setgid_write(tmp_path, {})
        return
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
    leaves the other name with the old text.

    Where the system keeps no birth time the name made after the window read the
    file moves its status time, so a second question follows the first
    (_added_name_asks_twice)."""
    if not reports_birth_time(tmp_path.parent, {}):
        _added_name_asks_twice(tmp_path, {})
        return
    t = tmp_path / "t.c"
    h = tmp_path / "h.c"
    trace = _trace_of(tmp_path)
    with WpeSession(str(tmp_path), OLD, env_extra=_agent_env(trace, NEW_REPLY),
                    filename="t.c") as s:
        os.link(t, h)
        ino = os.stat(t).st_ino
        _ask(s, "change the return value in t.c")
        # one more key than before the question existed: a file with two names
        # asks at every dial, and this row is about the write that follows a yes
        assert _wait_screen(s, LINKED_BOX), "no question for a file with two names"
        s.key("y", delay=0.8)
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
        pane = "\n".join(pane_text_with_history(s, ["not a regular file"]))
        s.key("a", delay=0.5)
    assert stat.S_ISFIFO(os.stat(fifo).st_mode), "the pipe was replaced"
    assert "not a regular file" in pane.replace("\n", " "), \
        "the pane does not say why the write was refused:\n" + pane


# --- a file with several names, and a file replaced under its window, ask ---
# The user's own F2 never asks about a hard link (a Save keeps the inode, so
# every name changes, which is what a hard link means); the assistant's write
# asks at every dial, auto included, and names how many names the file has.

SECRET = "the user's other name for this file\n"
PLANTED = "planted text\n"
LINKED_BOX = "names on disk"
SLOW_TURN_BASE_MS = 4000    # a turn is held back so a row can act between two turns
SLOW_TURN_MS = int(SLOW_TURN_BASE_MS * gui_owner.wait_scale())
F2 = "\033OQ"               # kf2 -> Save
WRITE_X = "TOOL write_file x\n%s@@END" % PLANTED
PATCH_X = ("TOOL apply_patch x\n<<<<<<< SEARCH\nalpha\n=======\nbeta\n"
           ">>>>>>> REPLACE\n@@END")


def _linked_pair(tmp_path, text=SECRET):
    """A file outside the workspace, with a second name `x` inside it.  Returns
    (the outside file, the workspace name).  The scope judge sees `x` as an
    ordinary file of the workspace: only the link count tells."""
    outside = tmp_path.parent / (tmp_path.name + "-outside")
    outside.mkdir()
    secret = outside / "secret"
    secret.write_text(text)
    name = tmp_path / "x"
    os.link(secret, name)
    return secret, name


# Drain the editor until `needle` is on the screen: the owner's wait, with
# this file's own length (WAIT_S) as its default.
_wait_screen = functools.partial(wait_screen, tmo=WAIT_S)


def _row_after(s, needle):
    """The screen row under the first row that holds `needle` ("" when there is
    none): inside a box, the row the box puts after its heading, not a pane
    line elsewhere that repeats the same words."""
    rows = s.display()
    for i, row in enumerate(rows):
        if needle in row and i + 1 < len(rows):
            return rows[i + 1]
    return ""


def _count(trace, needle):
    """How many times the run's trace says `needle`."""
    return _trace_text(trace).count(needle)


# @scenario SC-write-owner-H2b-04
@pytest.mark.parametrize("policy", ["auto", "edits", "ask"])
def test_write_to_a_hard_linked_file_asks_under_auto(tmp_path, policy):
    """At every dial (the row keeps its name, auto being the one that wrote the
    outside file) the assistant is asked about a file with two names, and the
    box says how many.  n leaves both names byte-identical, and the trace has
    the denial.  Red before the question existed: auto wrote the outside file."""
    secret, x = _linked_pair(tmp_path)
    trace = _trace_of(tmp_path)
    with WpeSession(str(tmp_path), OLD,
                    env_extra=_agent_env(trace, WRITE_X + "@@TURN@@DONE tried", policy),
                    filename="t.c") as s:
        _ask(s, "write x")
        assert _wait_screen(s, LINKED_BOX), "no question for a file with two names:\n" + \
            "\n".join(s.display()) + "\n" + _trace_text(trace)
        box = "\n".join(s.display())
        s.key("n", delay=0.8)
        assert _wait_trace(s, trace, "agent deny write_file x"), _trace_text(trace)
    assert "This file has 2 names on disk" in box, "the box does not name the 2 names:\n" + box
    assert "Writing changes all of them" in box, box
    assert secret.read_text() == SECRET and x.read_text() == SECRET, \
        "a refused write changed a name of the file"
    assert os.stat(secret).st_ino == os.stat(x).st_ino


# @scenario SC-write-owner-H2b-05
def test_apply_patch_on_a_hard_linked_file_asks_under_auto(tmp_path):
    """The same through apply_patch: the patched result is shown in the same
    question and n leaves both names as they were."""
    secret, x = _linked_pair(tmp_path, "alpha\n")
    trace = _trace_of(tmp_path)
    with WpeSession(str(tmp_path), OLD, env_extra=_agent_env(trace, PATCH_X + "@@TURN@@DONE tried"),
                    filename="t.c") as s:
        _ask(s, "patch x")
        assert _wait_screen(s, LINKED_BOX), "no question for a patch of a file with two names:\n" + \
            "\n".join(s.display()) + "\n" + _trace_text(trace)
        s.key("n", delay=0.8)
        assert _wait_trace(s, trace, "agent deny apply_patch x"), _trace_text(trace)
    assert secret.read_text() == "alpha\n" and x.read_text() == "alpha\n", \
        "a refused patch changed a name of the file"


def _answer_and_finish(s, trace, answer):
    """Answer the open box, wait for the run's review and leave it with `a`."""
    s.key(answer, delay=0.8)
    assert _wait_trace(s, trace, "changeset"), _trace_text(trace)
    s.key("a", delay=0.5)


# @scenario SC-write-owner-H2b-06
def test_yes_to_a_linked_write_keeps_the_inode(tmp_path):
    """y writes the file itself: both names hold the new text, the old text is
    in x.bak, and the inode is the same, so the user who said yes got a Save."""
    secret, x = _linked_pair(tmp_path)
    trace = _trace_of(tmp_path)
    ino = os.stat(x).st_ino
    with WpeSession(str(tmp_path), OLD, env_extra=_agent_env(trace, WRITE_X + "@@TURN@@DONE ok"),
                    filename="t.c") as s:
        _ask(s, "write x")
        assert _wait_screen(s, LINKED_BOX), _trace_text(trace)
        _answer_and_finish(s, trace, "y")
    assert x.read_text() == PLANTED and secret.read_text() == PLANTED, \
        "a name of the file did not get the new text"
    assert os.stat(x).st_ino == ino and os.stat(secret).st_ino == ino, "the write made a new file"
    bak = tmp_path / "x.bak"
    assert bak.exists() and bak.read_text() == SECRET, \
        "the old text is not in x.bak: %r" % sorted(os.listdir(tmp_path))


# The system the editor runs on has a birth time for its files, or plays one
# that has none (the hook of the owner of "is this the file I saw": OpenBSD,
# ext3, most FUSE): the same rows hold on both.
BIRTH_MODES = ["with-birth", "no-birth"]
NO_BIRTH_ENV = {"XWPE_TEST_FAIL": "seen-no-birth"}
# A row whose outcome depends on the birth time runs as the host is
# ("observed": reports_birth_time says which outcome) and again forced to play a
# host with none, so both outcomes are asserted on every host.
OBSERVED_OR_FORCED = ["observed", "no-birth"]


def _birth_env(env, birth):
    """`env` with the no-birth hook set when the row runs as a system with none."""
    if birth == "no-birth":
        env.update(NO_BIRTH_ENV)
    return env


# @scenario SC-write-owner-H2b-07
# @scenario SC-name-identity-23
@pytest.mark.parametrize("birth", BIRTH_MODES)
def test_a_yes_to_a_linked_write_is_kept_until_the_name_count_changes(tmp_path, birth):
    """The second write of the same run to the same file does not ask again.
    A name added between the two writes is another question: the yes was for
    two names, not for three.

    With no birth time the first write moves the file's status time, so the
    yes kept from before it is refreshed from the file as the write left it:
    without that the second write would ask again (red before the refresh)."""
    secret, x = _linked_pair(tmp_path)
    trace = _trace_of(tmp_path)
    turns = ("TOOL write_file x\none\n@@END@@TURN@@TOOL write_file x\ntwo\n@@END"
             "@@TURN@@TOOL write_file x\nthree\n@@END@@TURN@@DONE ok")
    env = _birth_env(_agent_env(trace, turns), birth)
    env["XWPE_AI_MOCK_DELAY_MS"] = str(SLOW_TURN_MS)
    with WpeSession(str(tmp_path), OLD, env_extra=env, filename="t.c") as s:
        _ask(s, "write x three times")
        assert _wait_screen(s, "This file has 2 names"), _trace_text(trace)
        s.key("y", delay=0.5)
        assert _wait_trace(s, trace, "agent write confirm x -> allow"), _trace_text(trace)
        end = wait_deadline(WAIT_S)
        while time.time() < end and x.read_text() != "two\n":
            s._drain(0.4)
        assert x.read_text() == "two\n", "the second write was not made:\n" + _trace_text(trace)
        assert _count(trace, "agent write linked x") == 1, \
            "a yes kept for this run asked again:\n" + _trace_text(trace)
        extra = tmp_path / "x3"
        os.link(x, extra)                      # a third name, before turn three lands
        assert _wait_screen(s, "This file has 3 names"), \
            "a new name did not ask again:\n" + "\n".join(s.display())
        s.key("n", delay=0.8)
        assert _wait_trace(s, trace, "agent deny write_file x"), _trace_text(trace)
        s.key("a", delay=0.5)
    assert x.read_text() == "two\n" and extra.read_text() == "two\n", \
        "the refused third write changed the file"


# @scenario SC-name-identity-11
def test_a_yes_to_a_linked_write_is_not_for_another_file_with_its_number(tmp_path):
    """The user said yes for a file with two names.  Before the run's next
    write both names are removed and another file with two names is made, and
    the file system gives it the first file's inode number.  The yes was for
    the first file: the second write asks again, and n leaves the new file as
    it is.

    A guard once the remembered yes is judged by the owner of "is this the
    file I saw" (red under the mutation that compares the number alone)."""
    secret, x = _linked_pair(tmp_path)
    trace = _trace_of(tmp_path)
    ino = os.stat(x).st_ino
    turns = ("TOOL write_file x\none\n@@END@@TURN@@TOOL write_file x\ntwo\n@@END"
             "@@TURN@@DONE ok")
    env = _agent_env(trace, turns)
    env["XWPE_AI_MOCK_DELAY_MS"] = str(SLOW_TURN_MS)
    with WpeSession(str(tmp_path), OLD, env_extra=env, filename="t.c") as s:
        _ask(s, "write x twice")
        assert _wait_screen(s, "This file has 2 names"), _trace_text(trace)
        s.key("y", delay=0.5)
        end = wait_deadline(WAIT_S)
        while time.time() < end and x.read_text() != "one\n":
            s._drain(0.4)
        assert x.read_text() == "one\n", "the first write was not made:\n" + _trace_text(trace)
        os.unlink(x)
        os.unlink(secret)
        fillers, reached = make_file_with_number(secret.parent, "secret", ino, text=PLANTED)
        reached_or_say(secret.parent, os.stat(secret).st_ino if reached else -1, ino)
        os.link(secret, x)
        assert _wait_screen(s, "This file has 2 names"), \
            "a file with the first one's number was written without asking (%s):\n%s" \
            % (fs_type(tmp_path), _trace_text(trace))
        s.key("n", delay=0.8)
        assert _wait_trace(s, trace, "agent deny write_file x"), _trace_text(trace)
        s.key("a", delay=0.5)
    assert x.read_text() == PLANTED and secret.read_text() == PLANTED, \
        "the refused write changed the other file"


# @scenario SC-name-identity-24
@pytest.mark.parametrize("hook", OBSERVED_OR_FORCED)
def test_write_after_the_windows_file_was_touched_in_place(tmp_path, hook):
    """The window read t.c; a program then changed it in place (its times, as
    touch does).  With a birth time the file is proven to be the one the window
    read, so the write is a Save with no question.  With none it cannot be told
    from a new file: the box says "changed or replaced", never "replaced on
    disk", and a yes writes the text into the file.  The row branches on what
    this run observes (the system, the build, the hook); the forced parameter
    plays a system with no birth time on any host."""
    t = tmp_path / "t.c"
    trace = _trace_of(tmp_path)
    env = _birth_env(_agent_env(trace, NEW_REPLY + "@@TURN@@DONE ok"), hook)
    with WpeSession(str(tmp_path), OLD, env_extra=env, filename="t.c") as s:
        os.utime(t, (1000000000, 1000000000))
        _ask(s, "change the return value in t.c")
        if not reports_birth_time(tmp_path.parent, env):
            assert _wait_screen(s, CHANGED_BOX), "no question for a file that may be another:\n" + \
                "\n".join(s.display()) + "\n" + _trace_text(trace)
            assert REPLACED_BOX not in "\n".join(s.display()), "the box claims a replacement it cannot know"
            _answer_and_finish(s, trace, "y")
        else:
            assert _wait_trace(s, trace, "changeset"), _trace_text(trace)
            assert _count(trace, "agent write confirm") == 0, \
                "a file proven to be the window's own was asked about:\n" + _trace_text(trace)
            s.key("a", delay=0.5)
    assert "return 7" in t.read_text(), "the text was not written:\n" + t.read_text()


# @scenario SC-name-identity-30
def test_under_ask_the_one_box_says_changed_or_replaced_without_a_birth_time(tmp_path):
    """Under ask the diff box itself carries the warning, before any write is
    tried.  For a file the window read and a program then touched, on a system
    with no birth time, it says "changed or replaced" and shows the diff, never
    "replaced on disk"; n leaves the file alone."""
    t = tmp_path / "t.c"
    trace = _trace_of(tmp_path)
    env = _birth_env(_agent_env(trace, NEW_REPLY + "@@TURN@@DONE no", "ask"), "no-birth")
    with WpeSession(str(tmp_path), OLD, env_extra=env, filename="t.c") as s:
        os.utime(t, (1000000000, 1000000000))
        _ask(s, "change the return value in t.c")
        assert _wait_screen(s, CHANGED_BOX), "no warning in the box:\n" + "\n".join(s.display()) + \
            "\n" + _trace_text(trace)
        shown = "\n".join(s.display())
        assert REPLACED_BOX not in shown, "the box claims a replacement it cannot know"
        assert "return 7" in shown, "under ask the warning box carries no diff"
        s.key("n", delay=0.8)
        assert _wait_trace(s, trace, "agent deny write_file t.c"), _trace_text(trace)
    assert _count(trace, "agent write confirm") == 1, "more than one box:\n" + _trace_text(trace)
    assert t.read_text() == OLD, "a refused write changed the file"


def _refused_setgid_write(tmp_path, env_extra):
    """With no birth time the window's setgid file, made a program after the
    window read it, cannot be told from a new file with its number.  A copy
    would lose the bit, so the owner refuses (it does not ask) and says why:
    the file keeps its text and its bit, and there is no .bak.  The outcome of
    test_agent_write_keeps_the_files_setgid_bit on a system with no birth time."""
    t = tmp_path / "t.c"
    trace = _trace_of(tmp_path)
    env = _agent_env(trace, NEW_REPLY + "@@TURN@@DONE no")
    env.update(env_extra)
    with WpeSession(str(tmp_path), OLD, env_extra=env, filename="t.c") as s:
        _setgid_or_skip(t)
        _ask(s, "change the return value in t.c")
        assert _wait_trace(s, trace, "agent write failed"), _trace_text(trace)
        s.key("a", delay=0.5)
    txt = _trace_text(trace)
    assert "changed or replaced since the window showed it" in txt, \
        "the refusal does not say the file may be another:\n" + txt
    assert t.read_text() == OLD, "the write changed a file it could not prove the window's"
    assert stat.S_IMODE(t.stat().st_mode) == 0o2755, "the file lost its bit"
    assert not (tmp_path / "t.bak").exists(), "a refused write left a .bak"


# @scenario SC-name-identity-26
def test_agent_write_to_a_setgid_file_touched_since_the_read_is_refused_without_a_birth_time(tmp_path):
    """The no-birth outcome of test_agent_write_keeps_the_files_setgid_bit,
    forced on a host that does have a birth time."""
    _refused_setgid_write(tmp_path, NO_BIRTH_ENV)


def _added_name_asks_twice(tmp_path, env_extra):
    """The window read t.c; a second name was made for it afterwards (the file's
    status time moved).  With a birth time the file is the window's own: one
    question, about the names.  With none it cannot be told from a new file: the
    second question says "changed or replaced", and two yes writes the text into
    the very file, so the other name shows it.  The outcome of
    test_agent_write_keeps_the_inode_and_the_other_names with no birth time."""
    t = tmp_path / "t.c"
    h = tmp_path / "h.c"
    trace = _trace_of(tmp_path)
    env = _agent_env(trace, NEW_REPLY)
    env.update(env_extra)
    with WpeSession(str(tmp_path), OLD, env_extra=env, filename="t.c") as s:
        os.link(t, h)
        ino = os.stat(t).st_ino
        _ask(s, "change the return value in t.c")
        assert _wait_screen(s, LINKED_BOX), "no question for a file with two names"
        s.key("y", delay=0.8)
        assert _wait_screen(s, CHANGED_BOX), "no second question:\n" + "\n".join(s.display())
        assert REPLACED_BOX not in "\n".join(s.display()), "the box claims a replacement it cannot know"
        s.key("y", delay=0.8)
        assert _wait_trace(s, trace, "changeset"), _trace_text(trace)
        s.key("a", delay=0.5)
    assert os.stat(t).st_ino == ino, "the write made a new file"
    assert os.stat(h).st_ino == ino and "return 7" in h.read_text(), \
        "the other name of the file does not show the new text"


# @scenario SC-name-identity-27
def test_agent_write_after_a_name_was_added_since_the_read_asks_twice_without_a_birth_time(tmp_path):
    """The no-birth outcome of test_agent_write_keeps_the_inode_and_the_other_names,
    forced on a host that does have a birth time."""
    _added_name_asks_twice(tmp_path, NO_BIRTH_ENV)


# @scenario SC-write-owner-H2b-08
@pytest.mark.parametrize("key", ["\r", "\n"])
def test_enter_is_not_yes_to_a_linked_write(tmp_path, key):
    """Only y allows: a stray Enter at the question writes nothing."""
    secret, x = _linked_pair(tmp_path)
    trace = _trace_of(tmp_path)
    with WpeSession(str(tmp_path), OLD, env_extra=_agent_env(trace, WRITE_X + "@@TURN@@DONE tried"),
                    filename="t.c") as s:
        _ask(s, "write x")
        assert _wait_screen(s, LINKED_BOX), _trace_text(trace)
        s.key(key, delay=0.8)
        assert _wait_trace(s, trace, "agent deny write_file x"), \
            "Enter did not end the question as a refusal:\n" + _trace_text(trace)
    assert secret.read_text() == SECRET and x.read_text() == SECRET, "Enter wrote the file"


# --- a file replaced on disk under an open window (D117) --------------------

REPLACEMENT = "int main(void){return 3;}\n"
REPLACED_BOX = "was replaced on disk since this window read it"
CHANGED_BOX = "was changed or replaced since this window read it"


def _replace_on_disk(tmp_path, name, text):
    """Replace `name` by another regular file, as a program that saves by
    renaming does: a new inode under the same name."""
    new = tmp_path / (name + ".new")
    new.write_text(text)
    os.rename(new, tmp_path / name)


# @scenario SC-write-owner-H2b-09
def test_write_after_the_file_was_replaced_asks_and_yes_writes_a_copy(tmp_path):
    """The window read one file; a program replaced it.  The assistant's write
    asks as Save does.  y writes the text into the file now there, as a copy
    (the replacement's own inode, no special bit), and the window follows: a
    later F2 from the window does not ask again."""
    t = tmp_path / "t.c"
    trace = _trace_of(tmp_path)
    with WpeSession(str(tmp_path), OLD, env_extra=_agent_env(trace, NEW_REPLY), filename="t.c") as s:
        _replace_on_disk(tmp_path, "t.c", REPLACEMENT)
        ino = os.stat(t).st_ino
        _ask(s, "change the return value in t.c")
        assert _wait_screen(s, REPLACED_BOX), "no question for a replaced file:\n" + \
            "\n".join(s.display()) + "\n" + _trace_text(trace)
        _answer_and_finish(s, trace, "y")
        s.key("\033", delay=0.8)               # Esc leaves the pane for the window
        s.key("Z"); s.key(F2, delay=1.0)
        screen = "\n".join(s.display())
        s._drain(0.5)
    assert "return 7" in t.read_text(), "the text was not written:\n" + t.read_text()
    assert os.stat(t).st_ino == ino, "the text went into another file than the one asked about"
    assert stat.S_IMODE(t.stat().st_mode) & 0o7000 == 0, "a copy got a special bit"
    assert "Z" in t.read_text(), "F2 from the window did not write (it asked again?):\n" + screen
    assert "Overwrite" not in screen and REPLACED_BOX not in screen, \
        "the window did not follow the new file:\n" + screen


# @scenario SC-write-owner-H2b-10
@pytest.mark.parametrize("policy", ["auto", "edits", "ask"])
def test_write_after_the_file_was_replaced_and_n_writes_nothing(tmp_path, policy):
    """n at the replaced-file question leaves the file there untouched, at every
    dial.  Under ask the diff box itself carries the warning (one box, with the
    diff under it); under edits and auto the question has its own box."""
    t = tmp_path / "t.c"
    trace = _trace_of(tmp_path)
    with WpeSession(str(tmp_path), OLD,
                    env_extra=_agent_env(trace, NEW_REPLY + "@@TURN@@DONE no", policy),
                    filename="t.c") as s:
        _replace_on_disk(tmp_path, "t.c", REPLACEMENT)
        _ask(s, "change the return value in t.c")
        assert _wait_screen(s, REPLACED_BOX), _trace_text(trace)
        if policy == "ask":
            assert "return 7" in "\n".join(s.display()), "under ask the warning box carries no diff"
        s.key("n", delay=0.8)
        assert _wait_trace(s, trace, "agent deny write_file t.c"), _trace_text(trace)
    assert _count(trace, "agent write confirm") == 1, "more than one box:\n" + _trace_text(trace)
    assert t.read_text() == REPLACEMENT, "a refused write changed the file:\n" + t.read_text()


# @scenario SC-write-owner-H2b-15
@pytest.mark.parametrize("key", ["\r", "y"])
def test_under_ask_one_box_carries_the_replaced_warning_and_only_y_writes(tmp_path, key):
    """One box under ask: the warning is in the diff box, Enter does not write,
    y writes the text into the file now there as a copy.  There is no second
    box after the y."""
    t = tmp_path / "t.c"
    trace = _trace_of(tmp_path)
    with WpeSession(str(tmp_path), OLD,
                    env_extra=_agent_env(trace, NEW_REPLY + "@@TURN@@DONE ok", "ask"),
                    filename="t.c") as s:
        _replace_on_disk(tmp_path, "t.c", REPLACEMENT)
        ino = os.stat(t).st_ino
        _ask(s, "change the return value in t.c")
        assert _wait_screen(s, REPLACED_BOX), _trace_text(trace)
        assert "the text goes into the file now there, as a copy" in "\n".join(s.display())
        assert "t.c" in _row_after(s, "was replaced on disk since this window read it"), \
            "the path is not on the row after the heading:\n" + "\n".join(s.display())
        s.key(key, delay=1.0)
        if key == "y":
            assert _wait_trace(s, trace, "agent done"), _trace_text(trace)
        else:
            assert _wait_trace(s, trace, "agent deny write_file t.c"), _trace_text(trace)
        s._drain(0.5)
    assert _count(trace, "agent write confirm") == 1, "a second box was shown:\n" + _trace_text(trace)
    if key == "y":
        assert "return 7" in t.read_text() and os.stat(t).st_ino == ino, t.read_text()
        assert stat.S_IMODE(t.stat().st_mode) & 0o7000 == 0
    else:
        assert t.read_text() == REPLACEMENT, "Enter wrote the file"


# @scenario SC-write-owner-H2b-18
def test_under_ask_a_replacement_that_is_a_program_gets_no_box_and_is_refused(tmp_path):
    """The file that took the window's name is a setgid program.  The owner
    refuses it whatever the user says, so under ask there is no box to say yes
    to: the refusal's sentence comes at once, as under edits and auto, and the
    file is as it was."""
    t = tmp_path / "t.c"
    trace = _trace_of(tmp_path)
    with WpeSession(str(tmp_path), OLD,
                    env_extra=_agent_env(trace, NEW_REPLY + "@@TURN@@DONE no", "ask"),
                    filename="t.c") as s:
        _replace_on_disk(tmp_path, "t.c", REPLACEMENT)
        _setgid_or_skip(t)
        _ask(s, "change the return value in t.c")
        assert _wait_trace(s, trace, "agent write failed t.c"), \
            "no refusal:\n" + _trace_text(trace) + "\n".join(s.display())
        pane = " ".join(pane_text_with_history(
            s, ["Cannot write", "Nothing was written"]))
    assert _count(trace, "agent write confirm") == 0 and "agent write replaced" not in _trace_text(trace), \
        "a box asked about a file the owner refuses:\n" + _trace_text(trace)
    assert "Cannot write" in pane and "Nothing was written" in pane, pane
    assert t.read_text() == REPLACEMENT and stat.S_IMODE(t.stat().st_mode) == 0o2755


# @scenario SC-write-owner-H2b-19
def test_under_ask_a_file_that_changes_after_the_check_is_refused_not_asked(tmp_path):
    """The check says the owner would refuse the replacement, so no box is shown
    and the other questions (here the file's second name) are skipped with it.
    The owner then finds the file askable after all (the test hook makes the two
    looks disagree).  It must not ask a lone replaced question that leaves the
    names warning unshown: it refuses, says the file changed, and writes
    nothing."""
    t = tmp_path / "t.c"
    h = tmp_path / "h.c"
    trace = _trace_of(tmp_path)
    env = _agent_env(trace, NEW_REPLY + "@@TURN@@DONE no", "ask")
    env["XWPE_TEST_FAIL"] = "write-replacement-check-no"
    with WpeSession(str(tmp_path), OLD, env_extra=env, filename="t.c") as s:
        _replace_on_disk(tmp_path, "t.c", REPLACEMENT)
        os.link(t, h)
        _ask(s, "change the return value in t.c")
        assert _wait_trace(s, trace, "agent write failed t.c"), \
            "no refusal:\n" + _trace_text(trace) + "\n".join(s.display())
        pane = " ".join(pane_text_with_history(
            s, ["the file changed while it was checked"]))
    assert _count(trace, "agent write confirm") == 0 and REPLACED_BOX not in pane, \
        "a box was shown:\n" + _trace_text(trace)
    assert "the file changed while it was checked" in pane, pane
    assert t.read_text() == REPLACEMENT and h.read_text() == REPLACEMENT, "the file was written"


# @scenario SC-write-owner-H2b-17
def test_under_ask_a_replaced_and_linked_file_shows_both_warnings_in_one_box(tmp_path):
    """The file under the window's name is a replacement that also has a second
    name: the one box carries both warnings, and y writes all names."""
    t = tmp_path / "t.c"
    h = tmp_path / "h.c"
    trace = _trace_of(tmp_path)
    with WpeSession(str(tmp_path), OLD,
                    env_extra=_agent_env(trace, NEW_REPLY + "@@TURN@@DONE ok", "ask"),
                    filename="t.c") as s:
        _replace_on_disk(tmp_path, "t.c", REPLACEMENT)
        os.link(t, h)
        _ask(s, "change the return value in t.c")
        assert _wait_screen(s, REPLACED_BOX), _trace_text(trace)
        box = "\n".join(s.display())
        assert "This file has 2 names on disk" in box, box
        s.key("y", delay=1.0)
        assert _wait_trace(s, trace, "agent done"), _trace_text(trace)
    assert _count(trace, "agent write confirm") == 1, "more than one box:\n" + _trace_text(trace)
    assert "return 7" in t.read_text() and "return 7" in h.read_text(), "not all names written"


# @scenario SC-write-owner-H2b-16
@pytest.mark.parametrize("key", ["\r", "y"])
def test_under_ask_one_box_carries_the_linked_warning_and_only_y_writes(tmp_path, key):
    """One box under ask for a file with two names: the count and the full path
    are in the diff box, Enter does not write, y writes all names."""
    deep = "d" * 50
    ws_dir = tmp_path / deep / "a"
    ws_dir.mkdir(parents=True)
    outside = tmp_path.parent / (tmp_path.name + "-outside")
    outside.mkdir()
    secret = outside / "secret"
    secret.write_text(SECRET)
    x = ws_dir / "x"
    os.link(secret, x)
    rel = "%s/a/x" % deep
    trace = _trace_of(tmp_path)
    turns = "TOOL write_file %s\n%s@@END@@TURN@@DONE ok" % (rel, PLANTED)
    with WpeSession(str(tmp_path), OLD, env_extra=_agent_env(trace, turns, "ask"), filename="t.c") as s:
        _ask(s, "write the linked file")
        assert _wait_screen(s, LINKED_BOX), _trace_text(trace)
        box = "\n".join(s.display())
        assert "This file has 2 names on disk" in box, box
        assert rel in _row_after(s, "This file has 2 names on disk"), \
            "the whole path is not on the row after the count:\n" + box
        s.key(key, delay=1.0)
        if key == "y":
            assert _wait_trace(s, trace, "agent done"), _trace_text(trace)
        else:
            assert _wait_trace(s, trace, "agent deny write_file"), _trace_text(trace)
        s._drain(0.5)
    assert _count(trace, "agent write confirm") == 1, "a second box was shown:\n" + _trace_text(trace)
    if key == "y":
        assert x.read_text() == PLANTED and secret.read_text() == PLANTED, "not all names written"
    else:
        assert x.read_text() == SECRET and secret.read_text() == SECRET, "Enter wrote the file"


# @scenario SC-write-owner-H2b-11
def test_enter_is_not_yes_to_the_replaced_file_question(tmp_path):
    """Only y allows here too."""
    t = tmp_path / "t.c"
    trace = _trace_of(tmp_path)
    with WpeSession(str(tmp_path), OLD, env_extra=_agent_env(trace, NEW_REPLY + "@@TURN@@DONE no"),
                    filename="t.c") as s:
        _replace_on_disk(tmp_path, "t.c", REPLACEMENT)
        _ask(s, "change the return value in t.c")
        assert _wait_screen(s, REPLACED_BOX), _trace_text(trace)
        s.key("\r", delay=0.8)
        assert _wait_trace(s, trace, "agent deny write_file t.c"), _trace_text(trace)
    assert t.read_text() == REPLACEMENT, "Enter wrote the file"


# @scenario SC-write-owner-H2b-12
def test_a_revert_that_finds_another_file_under_the_window_says_so(tmp_path):
    """The run wrote t.c; before the review's revert another file took the
    name.  The revert puts nothing into it and says what happened in the
    words of a revert, not with advice to use Save As, which belongs to a
    Save of the user's own text."""
    t = tmp_path / "t.c"
    trace = _trace_of(tmp_path)
    with WpeSession(str(tmp_path), OLD, env_extra=_agent_env(trace, NEW_REPLY), filename="t.c") as s:
        _ask(s, "change the return value in t.c")
        assert _wait_trace(s, trace, "changeset"), _trace_text(trace)
        _replace_on_disk(tmp_path, "t.c", REPLACEMENT)
        s.key("r", delay=1.5)
        pane = " ".join(pane_text_with_history(
            s, ["Cannot put back", "Nothing was changed."]))
        s._drain(0.5)
    assert t.read_text() == REPLACEMENT, "the revert wrote into the file that took the name"
    assert "Cannot put back" in pane and "Nothing was changed." in pane, \
        "the revert does not say it put nothing back:\n" + pane
    assert "Save As" not in pane, "a revert advises Save As:\n" + pane


# @scenario SC-write-owner-H2b-13
def test_a_folder_swapped_for_a_link_after_the_look_is_refused(tmp_path):
    """d/f is a file of the workspace when the place is looked at.  Before the
    scope is judged, d is replaced by a link to a folder outside (the test hook
    does what a racing program would).  The judge is given the place the look
    resolved, so it finds a link on its way and the write is refused with the
    words for it, nothing asked and nothing written; the outside file keeps its
    bytes.  Red if the judge rules on the model's name instead: the outside
    folder is then judged as outside and the box asks, and nothing is refused."""
    outside = tmp_path.parent / (tmp_path.name + "-outside")
    outside.mkdir()
    (outside / "f").write_text(SECRET)
    d = tmp_path / "d"
    d.mkdir()
    (d / "f").write_text(SECRET)
    trace = _trace_of(tmp_path)
    turns = "TOOL write_file d/f\n%s@@END@@TURN@@DONE tried" % PLANTED
    env = _agent_env(trace, turns)
    env.update({"XWPE_TEST_FAIL": "ai-swap-after-look", "XWPE_TEST_SWAP_DIR": str(d),
                "XWPE_TEST_SWAP_TO": str(outside)})
    with WpeSession(str(tmp_path), OLD, env_extra=env, filename="t.c") as s:
        _ask(s, "write d/f")
        assert _wait_trace(s, trace, "agent write failed d/f"), \
            "the write was not refused:\n" + _trace_text(trace) + "\n".join(s.display())
        pane = " ".join(pane_text_with_history(
            s, ["a link was put on its way while it was checked"]))
    assert "a link was put on its way while it was checked" in pane, pane
    assert (outside / "f").read_text() == SECRET, "the write reached the outside file"
    assert "agent write confirm" not in _trace_text(trace), "a question was asked"


# @scenario SC-write-owner-H2b-14
def test_an_outside_file_with_several_names_asks_the_outside_question_and_then_the_names(tmp_path):
    """x in the workspace is a second name of outside/secret.  The model names
    the outside path: the box for a file outside the workspace comes first, and
    after a y the box about the file's names comes too, at the same dial.  n at
    the second leaves the file as it was.  Red if the outside branch drops the
    names question: the y then writes the file."""
    secret, x = _linked_pair(tmp_path)
    trace = _trace_of(tmp_path)
    turns = "TOOL write_file %s\n%s@@END@@TURN@@DONE tried" % (secret, PLANTED)
    with WpeSession(str(tmp_path), OLD, env_extra=_agent_env(trace, turns), filename="t.c") as s:
        _ask(s, "write the outside file")
        assert _wait_screen(s, "outside the workspace"), _trace_text(trace)
        assert LINKED_BOX not in "\n".join(s.display()), "both questions at once"
        s.key("y", delay=0.8)
        assert _wait_screen(s, LINKED_BOX), \
            "no question about the names after the outside box:\n" + _trace_text(trace)
        assert "2 names on disk" in "\n".join(s.display())
        s.key("n", delay=0.8)
        assert _wait_trace(s, trace, "agent deny write_file"), _trace_text(trace)
    assert secret.read_text() == SECRET and x.read_text() == SECRET, "the file was written"
