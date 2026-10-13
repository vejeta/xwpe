"""The agent's basic run contracts with the deterministic mock backend.

Alt-G g starts a run with a header naming the backend/model/dial and a task
line right under it; Esc or an empty prompt sends nothing; a reply with no
DONE/TOOL marker is shown as the answer, not run as a tool; a model that
keeps asking for tools is stopped after 24 turns with a named line; a dial
name the editor does not recognise (env var or xwperc) reads as ask, the
safest choice; Esc on the command box denies the tool exactly like pressing
n; the ask dial never opens the changeset review (nothing to revert, since
every write there was confirmed one at a time); and the "working" line
carries a spinner and an advancing seconds count so a long run does not look
stalled.
"""
import os
import re
import subprocess
import time

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

SEED = "int main(void){return 0;}\n"
ESC = "\033"

FOLLOWUP_LINE = "type a follow-up"
WRITE_BOX = " Write "
STEP_LIMIT_LINE = "[AI] reached the step limit - Alt-G g to continue the task"
CHECKPOINT_LINE = "[agent] checkpoint taken - changes are reviewable/revertible at the end"
TURN_POLL_S = 15
LOOP_POLL_S = 60

# The driver scales its own key/drain waits by XWPE_TEST_WAIT_SCALE (the
# sanitizer build runs at 2, the Debian autopkgtest at 3), but a fixed
# XWPE_AI_MOCK_DELAY_MS does not: the mock's reply still lands at the same
# wall-clock time regardless of how the harness wait factor is set.  A window
# measured straight off time.time(), rather than through a driver drain, must
# be scaled by hand to stay in the same proportion to that delay.
WAIT_SCALE = float(os.environ.get("XWPE_TEST_WAIT_SCALE", "1"))
SPINNER_DELAY_MS = int(4500 * WAIT_SCALE)


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"autonomous coding agent" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _env(tmp_path, reply, policy, **more):
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_POLICY": policy,
        "XWPE_AI_TRACE": str(tmp_path / "ai.trace"),
    }
    env.update(more)
    return env


def _trace(tmp_path):
    t = tmp_path / "ai.trace"
    return t.read_text() if t.exists() else ""


def _screen(s, drain=0.2):
    return "\n".join(s.display(drain=drain))


def _seen_within(s, needle, timeout):
    """Drain the screen until `needle` shows or `timeout` seconds pass."""
    end = time.time() + timeout
    while time.time() < end:
        s._drain(0.25)
        if needle in "\n".join(s.display(drain=0)):
            return True
    return False


def _seed_xwperc(tmp_path, lines):
    """Write a [Programming] section into the test HOME's xwperc.  The driver
    sets HOME to the workdir, so this is the configuration the editor reads."""
    cfg = tmp_path / ".config" / "xwpe"
    cfg.mkdir(parents=True, exist_ok=True)
    (cfg / "xwperc").write_text("[Programming]\n" + "".join(l + "\n" for l in lines))


def _start_agent(s, task, delay=1.0):
    s.key(ALT.AI); s.key("g")
    s.key(task); s.key("\r", delay=delay)


def _row_index(rows, needle):
    return next((i for i, r in enumerate(rows) if needle in r), -1)


# @scenario SC-ai-agent-01
def test_header_and_task_line_are_adjacent(tmp_path):
    env = _env(tmp_path, "DONE all good", "ask")
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _start_agent(s, "tidy the code", delay=1.5)
        s._drain(1.0)
        rows = s.display()
    text = "\n".join(rows)
    assert "[AI: mock | model: mock-model | policy: ask]" in text, text
    assert "[agent] task: tidy the code" in text, text
    hdr = next(i for i, r in enumerate(rows) if "[AI: mock" in r)
    task = next(i for i, r in enumerate(rows) if "[agent] task:" in r)
    assert task == hdr + 1, "the task line is not right under the header:\n" + text


# @scenario SC-ai-agent-02
def test_esc_and_empty_prompt_send_nothing(tmp_path):
    env = _env(tmp_path, "DONE never", "ask")
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        s.key(ALT.AI); s.key("g"); s._drain(0.5)
        s.key(ESC, delay=0.8)
        s.key(ALT.AI); s.key("g"); s._drain(0.5)
        s.key("\r", delay=0.8)
        s._drain(1.0)
        rows = s.display()
    txt = _trace(tmp_path)
    text = "\n".join(rows)
    assert "stream start" not in txt, txt
    assert "agent tool=" not in txt, txt
    assert "agent task=" not in txt, txt
    assert "[agent]" not in text, text


# @scenario SC-ai-agent-07
def test_no_marker_reply_is_shown_as_the_answer(tmp_path):
    reply = "Here is my view.\nSecond line of prose."
    env = _env(tmp_path, reply, "ask")
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _start_agent(s, "opine", delay=1.5)
        assert _seen_within(s, FOLLOWUP_LINE, 10), _screen(s)
        rows = s.display()
    txt = _trace(tmp_path)
    text = "\n".join(rows)
    assert "Here is my view." in text and "Second line of prose." in text, text
    assert "agent answer (no marker)" in txt, txt
    assert "agent tool=" not in txt, txt


# @scenario SC-ai-agent-10
def test_step_limit_stops_after_24_tool_turns(tmp_path):
    turns = "@@TURN@@".join(["TOOL list_dir ."] * 40 + ["DONE never reached"])
    env = _env(tmp_path, turns, "ask")
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _start_agent(s, "loop", delay=1.0)
        assert _seen_within(s, FOLLOWUP_LINE, LOOP_POLL_S), _screen(s)
        rows = s.display()
    txt = _trace(tmp_path)
    n = txt.count("agent tool=list_dir")
    text = "\n".join(rows)
    assert STEP_LIMIT_LINE in text, text
    assert "agent done" not in txt, txt
    # The 25th tool request is refused, so 24 turns actually ran.
    assert n == 24, "expected 24 tool turns before the limit, saw %d" % n


# @scenario SC-ai-agent-16
def test_bogus_policy_env_reads_ask(tmp_path):
    env = _env(tmp_path, "DONE ok", "bogus")
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _start_agent(s, "x", delay=1.5)
        text = _screen(s)
    assert "policy: ask]" in text, text
    assert "agent policy=ask" in _trace(tmp_path)


# @scenario SC-ai-agent-16
def test_bogus_policy_xwperc_reads_ask(tmp_path):
    _seed_xwperc(tmp_path, ["AIPolicy : sometimes"])
    env = _env(tmp_path, "DONE ok", "ask")
    del env["XWPE_AI_POLICY"]
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _start_agent(s, "x", delay=1.5)
        text = _screen(s)
    assert "policy: ask]" in text, text


# @scenario SC-ai-agent-18
def test_esc_on_the_command_box_denies(tmp_path):
    marker = tmp_path / "SHOULD_NOT_EXIST"
    env = _env(tmp_path, "TOOL run_command touch %s@@TURN@@DONE stopped" % marker, "ask")
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _start_agent(s, "run", delay=1.0)
        assert _seen_within(s, "APPROVE?", TURN_POLL_S), _screen(s)
        box = _screen(s)
        s.key(ESC, delay=1.2)
        s._drain(1.0)
        rows = s.display()
    txt = _trace(tmp_path)
    text = "\n".join(rows)
    assert ">> APPROVE?  touch" in box and ">> y = allow    n / Esc = deny" in box, box
    assert not marker.exists(), "Esc on the command box still ran the command"
    assert "[agent] denied: run_command touch" in text, text
    assert "agent deny run_command touch" in txt, txt
    assert "agent done" in txt, txt


# @scenario SC-ai-agent-35
def test_no_review_opens_under_ask(tmp_path):
    env = _env(tmp_path, "TOOL write_file out.txt\nhello\n@@END@@TURN@@DONE wrote", "ask")
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _start_agent(s, "write", delay=1.0)
        assert _seen_within(s, WRITE_BOX, TURN_POLL_S), _screen(s)
        s.key("y", delay=1.2)
        assert _seen_within(s, FOLLOWUP_LINE, TURN_POLL_S), _screen(s)
        rows = s.display()
    txt = _trace(tmp_path)
    text = "\n".join(rows)
    assert (tmp_path / "out.txt").read_text() == "hello\n"
    assert "a = keep all" not in text, text
    assert "checkpoint" not in text and "checkpoint" not in txt, txt
    assert "changeset" not in txt, txt


# @scenario SC-ai-agent-39
def test_spinner_glyph_and_seconds_advance(tmp_path):
    env = _env(tmp_path, "DONE slow", "ask", XWPE_AI_MOCK_DELAY_MS=str(SPINNER_DELAY_MS))
    samples = []
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _start_agent(s, "wait", delay=0.3)
        end = time.time() + 4.0 * WAIT_SCALE
        while time.time() < end:
            s._drain(0.3)
            line = next((r for r in s.display(drain=0) if "[agent] working" in r), None)
            if line:
                samples.append(line.strip())
        s._drain(1.5)
    uniq = sorted(set(samples))
    assert samples, "no working line was ever on screen"
    assert any("(Esc cancels)" in x for x in samples), uniq
    assert len(uniq) >= 3, "the working line did not change over 4 s: %r" % uniq
    secs = sorted({int(m.group(1)) for x in samples for m in [re.search(r"(\d+)s", x)] if m})
    assert len(secs) >= 2, "the seconds counter did not advance: %r" % secs


# @scenario SC-ai-agent-03
def test_launch_checkpoint_shown_before_the_first_tool_line_under_auto(tmp_path):
    env = _env(tmp_path, "TOOL list_dir .@@TURN@@DONE done", "auto")
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _start_agent(s, "list it", delay=1.0)
        assert _seen_within(s, FOLLOWUP_LINE, TURN_POLL_S), _screen(s)
        rows = s.display()
    text = "\n".join(rows)
    ck_row = _row_index(rows, CHECKPOINT_LINE)
    tool_row = _row_index(rows, "[agent] list_dir .")
    assert ck_row >= 0, "no checkpoint line in the pane under auto:\n" + text
    assert tool_row >= 0, "no tool line in the pane:\n" + text
    assert ck_row < tool_row, \
        "the checkpoint line did not come before the first tool line:\n" + text


# @scenario SC-ai-agent-03
def test_launch_takes_no_checkpoint_under_ask(tmp_path):
    env = _env(tmp_path, "TOOL list_dir .@@TURN@@DONE done", "ask")
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _start_agent(s, "list it", delay=1.0)
        assert _seen_within(s, FOLLOWUP_LINE, TURN_POLL_S), _screen(s)
        rows = s.display()
    text = "\n".join(rows)
    assert _row_index(rows, CHECKPOINT_LINE) < 0, \
        "a checkpoint line appeared under ask, where nothing is taken:\n" + text
