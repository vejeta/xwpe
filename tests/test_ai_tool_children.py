"""The assistant's children run beside the editor, and end when they should.

An agent's tool command runs on the editor's loop: the editor takes keys
while it runs, Esc ends it with the task, and one that outlives AIToolTimeout
gets SIGTERM, then SIGKILL after the children's grace, so a command that
ignores SIGTERM still ends and nothing of it is left.  It used to run inside
a wait of its own, so the editor took no key, Esc included, until the command
was over or its bound and grace had passed.

The command-line backend reads its prompt from stdin.  The prompt used to be
written with a blocking loop, so the editor froze while `claude -p` was slow
to read it, and a write interrupted by a signal cut it short without a word.
It now goes through the editor's writer, and the end of file follows its
last byte.

The hosted engine is one of the editor's children too: the editor's own end
ends it, as it ends a debugger or a build.

Stand-ins only: the mock backend for the agent, a fake `claude` on PATH that
reads slowly, and a fake hosted engine that ignores SIGTERM.  Every process a
test starts is named by the PID file it writes, and only that PID or its
group is checked or ended.
"""
import os
import re
import signal
import time

import pytest
from wpe_driver import WpeSession, ALT, WAIT_SCALE, WPE_BIN, assistant_build, wait_screen

pytestmark = pytest.mark.skipif(not assistant_build(),
                                reason="wpe built without --enable-ai")

SEED = "int main(void){return 0;}\n"
ESC = "\033"
TOOL_TIMEOUT_S = 2
CHILD_GRACE_S = 2              # the children's owner: SIGTERM, then SIGKILL after this
FROZEN_LIMIT_S = 1.5           # longer than this without the menu reads as a frozen editor
MENU_ROW = "Ask (chat)"        # a row of the Alt-G menu
TIMEOUT_RESULT = "TOOL ERROR: timed out after %d s" % TOOL_TIMEOUT_S
# The trace line of the pane's "timed out" report: the pane itself is halved
# by the end-of-run review and wraps the line, the trace does not.
TIMED_OUT_TRACE = "agent tool timed out:"
START_LIMIT_S = 8 * WAIT_SCALE
GONE_LIMIT_S = CHILD_GRACE_S + 3 * WAIT_SCALE


# ---------------------------------------------------------------- helpers --

def _trace(path):
    return path.read_text() if path.exists() else ""


def _wait_trace(s, path, needle, limit):
    end = time.time() + limit
    while time.time() < end:
        if needle in _trace(path):
            return True
        s._drain(0.1)
    return needle in _trace(path)


def _wait_file(s, path, limit):
    end = time.time() + limit
    while time.time() < end:
        if path.exists() and path.read_text().strip():
            return True
        s._drain(0.1)
    return False


def _pids_from(path):
    """The numbers a stand-in wrote: its PID, and its group when it wrote one."""
    return [int(w) for w in path.read_text().split()]


def _alive(pid):
    """Is the process `pid` there?  Only ever asked for a PID a stand-in wrote."""
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def _group_alive(pgid):
    """Has the group `pgid` any member left?  Asked only for a stand-in's group."""
    try:
        os.killpg(pgid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def _gone_within(s, pid, limit):
    end = time.time() + limit
    while time.time() < end:
        if not _alive(pid) and not _group_alive(pid):
            return True
        if s is not None:
            s._drain(0.1)
        else:
            time.sleep(0.1)
    return not _alive(pid) and not _group_alive(pid)


def _end_group(pgid):
    """Clean up after a red run: SIGKILL to the group the stand-in leads."""
    if _group_alive(pgid):
        try:
            os.killpg(pgid, signal.SIGKILL)
        except OSError:
            pass


def _seconds_until_menu(s):
    """Type Alt-G; the seconds until its menu is on the screen."""
    s._drain(0.1)
    t0 = time.time()
    os.write(s.master_fd, ALT.AI.encode())
    end = t0 + 12 * WAIT_SCALE
    while time.time() < end:
        s._drain(0.1)
        if MENU_ROW in "\n".join(s.display(drain=0)):
            return time.time() - t0
    return 12 * WAIT_SCALE


def _seed_xwperc(tmp_path, lines):
    cfg = tmp_path / ".config" / "xwpe"
    cfg.mkdir(parents=True, exist_ok=True)
    (cfg / "xwperc").write_text("[Programming]\n" + "".join(l + "\n" for l in lines))


def _agent_env(tmp_path, reply):
    return {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
            "XWPE_AI_MOCK_REPLY": reply, "XWPE_AI_POLICY": "auto",
            "XWPE_AI_TRACE": str(tmp_path / "ai.trace")}


# ------------------------------------------------------- the tool command --

# @scenario SC-ai-runtime-25
def test_a_tool_command_that_ignores_sigterm_ends_at_the_deadline_plus_the_grace(tmp_path):
    pidf = tmp_path / "cmd.pid"
    trace = tmp_path / "ai.trace"
    _seed_xwperc(tmp_path, ["AIToolTimeout : %d" % TOOL_TIMEOUT_S])
    reply = ("TOOL run_command trap '' TERM; echo $$ > %s; sleep 30"
             "@@TURN@@DONE gave up waiting" % pidf)
    pid = None
    try:
        with WpeSession(str(tmp_path), SEED, env_extra=_agent_env(tmp_path, reply)) as s:
            s.key(ALT.AI); s.key("g")
            s.key("wait for it"); s.key("\r", delay=0.2)
            started = _wait_file(s, pidf, START_LIMIT_S)
            t0 = time.time()
            pid = _pids_from(pidf)[0] if started else None
            menu_s = _seconds_until_menu(s)
            s.key(ESC, delay=0.4)                      # closes the menu, not the run
            told = _wait_trace(s, trace, TIMED_OUT_TRACE,
                               TOOL_TIMEOUT_S + CHILD_GRACE_S + 10)
            elapsed = time.time() - t0
            gone = pid is not None and _gone_within(s, pid, 1.0)
            s._drain(1.0)
            txt = _trace(trace)
    finally:
        if pid:
            _end_group(pid)
    assert started, "the command never ran:\n" + _trace(trace)
    assert menu_s < FROZEN_LIMIT_S, \
        "Alt-G took %.1fs to open while the tool command ran" % menu_s
    assert told, "the command was never reported timed out:\n" + txt
    assert elapsed >= TOOL_TIMEOUT_S + CHILD_GRACE_S - 0.5, \
        "the command was given no grace after SIGTERM (%.1fs)" % elapsed
    assert TIMEOUT_RESULT in txt, "the model was not told:\n" + txt
    assert "agent done" in txt, "the run did not go on after the timeout:\n" + txt
    assert gone, "the command (pid %d) or its group outlived the deadline" % pid


# @scenario SC-ai-runtime-26
def test_esc_during_a_tool_command_ends_it_and_the_turn(tmp_path):
    pidf = tmp_path / "cmd.pid"
    trace = tmp_path / "ai.trace"
    reply = ("TOOL run_command echo $$ > %s; sleep 30"
             "@@TURN@@DONE the second turn must never start" % pidf)
    pid = None
    try:
        with WpeSession(str(tmp_path), SEED, env_extra=_agent_env(tmp_path, reply)) as s:
            s.key(ALT.AI); s.key("g")
            s.key("wait for it"); s.key("\r", delay=0.2)
            started = _wait_file(s, pidf, START_LIMIT_S)
            pid = _pids_from(pidf)[0] if started else None
            menu_s = _seconds_until_menu(s)
            s.key(ESC, delay=0.4)                      # closes the menu
            s._drain(0.2)
            t0 = time.time()
            os.write(s.master_fd, ESC.encode())        # ends the run
            cancelled = wait_screen(s, "[AI] cancelled", 4 * WAIT_SCALE)
            cancel_s = time.time() - t0
            gone = pid is not None and _gone_within(s, pid, GONE_LIMIT_S)
            s._drain(1.0)
            txt = _trace(trace)
    finally:
        if pid:
            _end_group(pid)
    assert started, "the command never ran:\n" + _trace(trace)
    assert menu_s < FROZEN_LIMIT_S, \
        "Alt-G took %.1fs to open while the tool command ran" % menu_s
    assert cancelled and cancel_s < FROZEN_LIMIT_S + 1, \
        "Esc did not end the run while its command ran (%.1fs)" % cancel_s
    assert gone, "the command (pid %d) or its group outlived Esc" % pid
    assert "agent done" not in txt, "the run went on after Esc:\n" + txt
    assert txt.count("stream start backend=mock") == 1, \
        "a second turn started after Esc:\n" + txt


# ------------------------------------------------- the command-line backend --

CLI_READ_DELAY_S = 3 * WAIT_SCALE    # the stand-in reads nothing for this long
# The instructions file, near the 65536 bytes the prompt may carry of it, so
# with the rest of the prompt more than a pipe holds (64 KiB on Linux).
PROMPT_FILE_BYTES = 65000
PROMPT_END_MARK = "PROMPT-END-MARK-7Q"
CLI_ANSWER = "DONE got the prompt"


def _slow_claude(bindir, received):
    """A fake `claude` that waits, then reads its whole stdin into `received`."""
    bindir.mkdir()
    fake = bindir / "claude"
    fake.write_text(
        "#!/bin/sh\n"
        "sleep %s\n"
        "cat > %s\n"
        "echo '{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,"
        "\"result\":\"%s\"}'\n" % (CLI_READ_DELAY_S, received, CLI_ANSWER))
    fake.chmod(0o755)


def _long_instructions(tmp_path):
    """A project instructions file that makes the prompt longer than a pipe."""
    line = "keep every line of this file in the prompt, it is a test.\n"
    body = line * (PROMPT_FILE_BYTES // len(line))
    (tmp_path / ".xwpe-ai.md").write_text(body + PROMPT_END_MARK + "\n")


# @scenario SC-ai-runtime-27
def test_a_long_prompt_reaches_the_cli_whole(tmp_path):
    received = tmp_path / "received.txt"
    trace = tmp_path / "ai.trace"
    _slow_claude(tmp_path / "bin", received)
    _long_instructions(tmp_path)
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "claudecli",
           "XWPE_AI_MODEL": "default", "XWPE_AI_POLICY": "auto",
           "XWPE_AI_TRACE": str(trace),
           "XWPE_AI_PROJECT_INSTRUCTIONS_MAX": "65536",
           "PATH": str(tmp_path / "bin") + ":" + os.environ.get("PATH", "")}
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        s.key(ALT.AI); s.key("g", delay=0.5)           # the agent folds the instructions in
        s.key("follow the instructions"); s.key("\r", delay=0.3)
        menu_s = _seconds_until_menu(s)
        s.key(ESC, delay=0.4)                          # closes the menu, not the run
        answered = _wait_trace(s, trace, "agent done", CLI_READ_DELAY_S + 12)
        txt = _trace(trace)
    got = received.read_text() if received.exists() else ""
    m = re.search(r"claudecli prompt bytes=(\d+)", txt)
    assert menu_s < FROZEN_LIMIT_S, \
        "Alt-G took %.1fs to open while the CLI was slow to read its prompt" % menu_s
    assert answered, "the CLI's answer never reached the pane"
    assert PROMPT_END_MARK in got, \
        "the prompt was cut: %d bytes reached the CLI" % len(got.encode())
    assert m and int(m.group(1)) == len(got.encode()), \
        "the CLI read %d bytes of a prompt the trace sized as %s" % (
            len(got.encode()), m.group(1) if m else "?")


# ------------------------------------------------------- the hosted engine --

HOST_STAND_IN = '''\
import json, os, signal, sys, time
signal.signal(signal.SIGTERM, signal.SIG_IGN)
signal.signal(signal.SIGHUP, signal.SIG_IGN)
with open(%r, "w") as fh:
    fh.write("%%d %%d\\n" %% (os.getpid(), os.getpgid(0)))
for line in sys.stdin:
    sys.stdout.write(json.dumps({"type": "result", "subtype": "success",
                                 "is_error": False, "result": "HOST-ANSWERED"}) + "\\n")
    sys.stdout.flush()
while True:
    time.sleep(1)
'''


def _host_env(tmp_path, pidf):
    stand = tmp_path / "host_stand_in.py"
    stand.write_text(HOST_STAND_IN % str(pidf))
    return {"XWPE_AI_ENABLE": "1", "XWPE_AI_AGENT_ENGINE": "claude-code",
            "XWPE_AI_HOST_CMD": "exec python3 %s" % stand,
            "XWPE_AI_TRACE": str(tmp_path / "ai.trace")}


def _host_build():
    return b"AIHostAdapter text needs AIHostCommand" in open(
        os.path.abspath(WPE_BIN), "rb").read()


def _start_host_turn(s, pidf):
    s.key(ALT.AI); s.key("g", delay=0.5)
    s.key("hello"); s.key("\r", delay=0.3)
    return _wait_file(s, pidf, START_LIMIT_S)


# @scenario SC-ai-runtime-28
@pytest.mark.skipif(not _host_build(), reason="built without --enable-ai-agent-host")
def test_the_editors_end_leaves_no_hosted_engine(tmp_path):
    pidf = tmp_path / "host.pid"
    pid = pgid = None
    gone = False
    try:
        with WpeSession(str(tmp_path), SEED, env_extra=_host_env(tmp_path, pidf),
                        filename="t.c") as s:
            started = _start_host_turn(s, pidf)
            if started:
                pid, pgid = _pids_from(pidf)
            os.kill(s.proc.pid, signal.SIGTERM)        # the editor, by its own PID
            gone = pid is not None and _gone_within(s, pgid, GONE_LIMIT_S)
    finally:
        if pgid:
            _end_group(pgid)
    assert started, "the hosted engine never started"
    assert gone, "the hosted engine (pid %d) outlived the editor" % pid


# @scenario SC-ai-runtime-29
@pytest.mark.skipif(not _host_build(), reason="built without --enable-ai-agent-host")
def test_ending_a_hosted_session_ends_an_engine_deaf_to_sigterm(tmp_path):
    pidf = tmp_path / "host.pid"
    trace = tmp_path / "ai.trace"
    pid = pgid = None
    try:
        with WpeSession(str(tmp_path), SEED, env_extra=_host_env(tmp_path, pidf),
                        filename="t.c") as s:
            started = _start_host_turn(s, pidf)
            if started:
                pid, pgid = _pids_from(pidf)
            answered = wait_screen(s, "turn complete", 8 * WAIT_SCALE)
            s.key(ALT.AI); s.key("g", delay=0.8)
            s.key("\r", delay=0.8)                     # an empty task ends the session
            s.key("a", delay=0.5)                      # the review: keep everything
            ended = wait_screen(s, "session ended", 4 * WAIT_SCALE)
            gone = pid is not None and _gone_within(s, pgid, GONE_LIMIT_S)
            txt = _trace(trace)
    finally:
        if pgid:
            _end_group(pgid)
    assert started, "the hosted engine never started"
    assert answered, "the engine's turn never completed:\n" + txt
    assert ended, "the pane never said the session ended:\n" + txt
    assert gone, "an engine deaf to SIGTERM (pid %d) outlived its session" % pid
