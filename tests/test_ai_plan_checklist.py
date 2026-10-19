"""The agent's plan checklist: what it loads, how it is drawn, and what it says.

Each test drives one reply through the mock backend and reads what the Messages
window and the pane hold afterwards.

Two things about the harness shape the assertions.  The pane's lines come back
through pane_logical_lines, which normalises each line to single spaces, so the
two spaces the pane writes before "ok:" read back as one.  And the Messages
window wraps a row wider than itself over several screen rows, so a checklist
is read as the rows under its own header and a long step is read by joining
them back.

The tests run under the ask dial on purpose.  Under auto a checkpoint is taken
at launch, so the review opens when the run ends and writes to the same
Messages window.  Loading and ticking a checklist is not gated by the dial, so
ask isolates the behaviour under test.

Scenarios: SC-ai-agent-66, 67, 68, 69.
"""
import glob
import os
import re
import shutil
import subprocess
import time

import pytest
from wpe_driver import (ALT, WPE_BIN, WpeSession, pane_rows,
                        pane_text_with_history, title_row)
from test_ai_changeset import _messages_rows, _trace_text, _wait_trace


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")

SEED = "int main(void){return 0;}\n"
LAID_OUT = ("[plan] laid out - the checklist is in the Messages window "
            "(updates as steps finish)")
LAID_OUT_RESULT = "(plan shown"
HEADER = "=== Agent plan: %d/%d done ==="
STEP_ROW = re.compile(r"\s*\[( |x)\] (.*)$")
CHECK_COLS, CHECK_ROWS = 160, 40
# The pane writes "[plan] n/N done  ok: <step>"; read back through
# pane_logical_lines the run of spaces is one.
PROGRESS = "[plan] %d/%d done ok: %s"


def _trace_path(tmp_path):
    """The trace's path, outside the workspace so it is not one of the run's own
    changes."""
    return tmp_path.parent / (tmp_path.name + ".trace")


def _state_path(tmp_path):
    return tmp_path.parent / (tmp_path.name + ".state")


def _env(tmp_path, reply, policy="ask"):
    return {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_POLICY": policy,
        "XWPE_AI_TRACE": str(_trace_path(tmp_path)),
        "XDG_STATE_HOME": str(_state_path(tmp_path)),
    }


def _start_agent(s, task):
    s.key(ALT.AI)
    s.key("g")
    s.key(task)
    s.key("\r", delay=1.0)


def _screen(s):
    return [r.rstrip() for r in s.display(drain=0)]


def _run_plan(tmp_path, reply, needles, policy="ask",
              cols=CHECK_COLS, rows=CHECK_ROWS):
    """Run one plan reply and read what both windows hold once the run has ended.

    Returns (the Messages rows, the pane's logical lines, the trace text, the
    whole screen, whether the editor is alive).  Messages is read before the pane
    is paged back, since paging clicks into the pane and repaints.
    """
    trace = _trace_path(tmp_path)
    env = _env(tmp_path, reply, policy)
    with WpeSession(str(tmp_path), SEED, env_extra=env, cols=cols, rows=rows) as s:
        _start_agent(s, "lay out a plan")
        assert _wait_trace(trace, "agent done", 90), \
            "the run never finished:\n" + _trace_text(trace)
        s._drain(1.5)
        scr = _screen(s)
        messages = _messages_rows(scr) if title_row(scr, "Messages") >= 0 else []
        pane = pane_text_with_history(s, needles)
        alive = s.alive()
    return messages, pane, _trace_text(trace), scr, alive


def _evidence(messages, pane, txt):
    return "Messages:\n%s\npane:\n%s\ntrace:\n%s" % (
        "\n".join(messages), "\n".join(pane), txt)


def _checklists(messages):
    """Every checklist Messages holds, as (header row, [(tick, text), ...]).

    One entry per draw, in the order drawn: the window keeps what was written to
    it, so a run that draws the checklist twice leaves two entries and a test
    has to say which one it is reading.
    """
    out = []
    for r in messages:
        if r.startswith("=== Agent plan"):
            out.append((r, []))
            continue
        m = STEP_ROW.match(r)
        if m and out:
            out[-1][1].append((m.group(1), m.group(2)))
    return out


def _last_checklist(messages):
    """The most recently drawn checklist, ("", []) when Messages holds none."""
    lists = _checklists(messages)
    return lists[-1] if lists else ("", [])


def _session_text(tmp_path):
    """What the workspace session file holds, so a test can say whether a string
    the model reads was persisted anywhere it can reach."""
    out = []
    for p in glob.glob(str(_state_path(tmp_path) / "xwpe" / "ai" / "*.session")):
        with open(p, errors="replace") as fh:
            out.append(fh.read())
    return "\n".join(out)


PLAN_THREE = "TOOL plan\n- alpha step\n- beta step\n- gamma step\n@@END"


# @scenario SC-ai-agent-66
def test_bullets_blank_lines_and_plain_lines_all_become_steps(tmp_path):
    """A body of `- build it`, `  * test it`, a blank line and `ship it`.

    The contract says the three become `[ ] build it`, `[ ] test it` and
    `[ ] ship it` under "=== Agent plan: 0/3 done ===" and that the pane says the
    checklist was laid out.  The blank line is not a step, and the bullets and the
    indentation are not part of one.

    The contract also says the model reads "(plan shown -- now do the steps ...)".
    That clause is not asserted, because nothing outside the process carries it:
    the string is AI_PLAN_LAID_OUT_RESULT (we_ai_ui.c:10065), returned by
    ai_agent_tool_plan and handed to ai_agent_feed_result, which puts it in the
    turn's message list only.  No trace line covers an agent turn's prompt, the
    pane never shows it, and the session file is appended for the goal and the
    assistant turns but not for a tool result.  The test looks in all three and
    prints what it found rather than failing on a clause it cannot observe.
    """
    reply = ("TOOL plan\n- build it\n  * test it\n\nship it\n@@END"
             "@@TURN@@DONE laid out")
    messages, pane, txt, scr, alive = _run_plan(tmp_path, reply, [LAID_OUT])

    lists = _checklists(messages)
    assert len(lists) == 1, \
        "Messages holds %d checklists for one plan:\n%s" % (
            len(lists), _evidence(messages, pane, txt))
    header, steps = lists[0]
    assert header == HEADER % (0, 3), \
        "the header reads %r, the contract says %r:\n%s" % (
            header, HEADER % (0, 3), _evidence(messages, pane, txt))
    assert steps == [(" ", "build it"), (" ", "test it"), (" ", "ship it")], \
        "the checklist holds %r:\n%s" % (steps, _evidence(messages, pane, txt))
    assert LAID_OUT in "\n".join(pane), \
        "the pane does not say the checklist was laid out:\n%s" % _evidence(
            messages, pane, txt)

    sess = _session_text(tmp_path)
    print("the line the model reads, looked for in the trace (%r), on the screen "
          "(%r) and in the session file (%r)"
          % (LAID_OUT_RESULT in txt, LAID_OUT_RESULT in "\n".join(scr),
             LAID_OUT_RESULT in sess))
    assert alive, "laying out a plan ended the editor"


# @scenario SC-ai-agent-67
def test_a_plan_with_no_body_shows_an_empty_checklist(tmp_path):
    """`TOOL plan` with no body.

    Messages shows "=== Agent plan: 0/0 done ===" and "  (no steps given)", and
    the pane's laid-out line still appears.  Both spellings of an empty body
    are measured: the end marker on its own line, which is how a write_file
    body says "empty content", and no body at all, the TOOL line being the
    whole request.  The marker used to be read as a step named "@@END" when it
    was the body's first line.
    """
    spellings = [
        ("end marker on its own line",
         "TOOL plan\n@@END@@TURN@@DONE nothing to do"),
        ("no body at all",
         "TOOL plan@@TURN@@DONE nothing to do"),
    ]
    results = []
    for name, reply in spellings:
        messages, pane, txt, _scr, alive = _run_plan(tmp_path, reply, [LAID_OUT])
        lists = _checklists(messages)
        header, steps = lists[-1] if lists else ("", [])
        results.append((name, messages, pane, txt, alive, header, steps))
        print("%s: header=%r steps=%r laid_out=%r alive=%r"
              % (name, header, steps, LAID_OUT in "\n".join(pane), alive))

    # one failure carries every spelling's measurement, so a spelling the
    # contract gets right is not hidden behind one it does not
    wrong = []
    for name, messages, pane, txt, alive, header, steps in results:
        assert alive, "%s: an empty plan ended the editor" % name
        bad = []
        if header != HEADER % (0, 0):
            bad.append("the header reads %r, the contract says %r"
                       % (header, HEADER % (0, 0)))
        if steps:
            bad.append("the checklist holds the steps %r" % (steps,))
        if "  (no steps given)" not in messages:
            bad.append("Messages does not say no steps were given")
        if LAID_OUT not in "\n".join(pane):
            bad.append("the pane does not say the checklist was laid out")
        if bad:
            wrong.append("%s: %s\n%s" % (name, "; ".join(bad),
                                         _evidence(messages, pane, txt)))
    assert not wrong, "\n\n".join(wrong)


# @scenario SC-ai-agent-68
def test_a_step_done_that_matches_nothing_ticks_the_earliest_pending(tmp_path):
    """`TOOL step_done nothing-like-this` against a three-step plan.

    The contract says the earliest pending step is ticked when no step matches,
    and that the pane names the text the tool was given rather than a step it
    invented.  Only the first tick is asked about here, so the run ends after it
    and the checklist is read with two steps still pending.  The checklist read is
    the last one drawn; whether the earlier draw is still in the window is the
    next test's question.
    """
    reply = "@@TURN@@".join([PLAN_THREE, "TOOL step_done zzz unmatched",
                             "DONE stopped here"])
    want = PROGRESS % (1, 3, "zzz unmatched")
    messages, pane, txt, _scr, alive = _run_plan(tmp_path, reply, [want])

    header, steps = _last_checklist(messages)
    assert steps == [("x", "alpha step"), (" ", "beta step"), (" ", "gamma step")], \
        "the checklist holds %r:\n%s" % (steps, _evidence(messages, pane, txt))
    assert header == HEADER % (1, 3), \
        "the header reads %r:\n%s" % (header, _evidence(messages, pane, txt))
    assert want in "\n".join(pane), \
        "the pane's progress line does not name the text it was given:\n%s" % (
            _evidence(messages, pane, txt))
    assert alive, "ticking a step ended the editor"


# @scenario SC-ai-agent-68
def test_a_step_done_with_every_step_ticked_changes_nothing(tmp_path):
    """A `step_done` that arrives after the last step is already ticked.

    The contract says nothing changes and no progress line is printed.  The first
    three ticks match no step, so each falls back to the earliest pending one; the
    fourth names text of its own, so a progress line printed for it would carry
    that text and could not be mistaken for the third tick's.
    """
    reply = "@@TURN@@".join(
        [PLAN_THREE] + ["TOOL step_done zzz unmatched"] * 3
        + ["TOOL step_done omega final", "DONE finished"])
    messages, pane, txt, _scr, alive = _run_plan(
        tmp_path, reply, [PROGRESS % (3, 3, "zzz unmatched"), LAID_OUT])

    text = "\n".join(pane)
    header, steps = _last_checklist(messages)
    assert steps == [("x", "alpha step"), ("x", "beta step"),
                     ("x", "gamma step")], \
        "the checklist holds %r:\n%s" % (steps, _evidence(messages, pane, txt))
    assert header == HEADER % (3, 3), \
        "the header reads %r:\n%s" % (header, _evidence(messages, pane, txt))
    for n in (1, 2, 3):
        want = "[plan] %d/3 done" % n
        assert want in text, \
            "the pane is missing the progress line %r:\n%s" % (
                want, _evidence(messages, pane, txt))
    extra = [l for l in pane if "done ok:" in l and "omega final" in l]
    assert not extra, \
        "a step_done with nothing left to tick still printed a progress line: " \
        "%r\n%s" % (extra, _evidence(messages, pane, txt))
    assert alive, "a step_done past the last step ended the editor"


# @scenario SC-ai-agent-68
def test_a_tick_redraws_the_checklist_in_place(tmp_path):
    """One plan, one tick: how many checklists does the Messages window hold?

    The requirement says the checklist is drawn in the Messages window and
    REDRAWN IN PLACE as steps finish, and this scenario says each tick redraws it.
    In place reads as one checklist that changes, not a new copy under the old
    one, so the test counts the headers the window holds after a single tick.
    Each tick used to append a fresh copy, so the window read "0/3" and "1/3"
    at once.
    """
    reply = "@@TURN@@".join([PLAN_THREE, "TOOL step_done alpha step",
                             "DONE finished"])
    messages, pane, txt, _scr, alive = _run_plan(
        tmp_path, reply, [PROGRESS % (1, 3, "alpha step")])

    lists = _checklists(messages)
    assert len(lists) == 1, \
        "Messages holds %d checklists after one tick, so each draw was appended " \
        "rather than redrawn in place:\n%s" % (
            len(lists), _evidence(messages, pane, txt))
    assert alive, "ticking a step ended the editor"


# @scenario SC-ai-agent-69
def test_the_checklist_stops_at_thirty_two_steps(tmp_path):
    """A plan body of thirty-four lines.

    The contract says the 33rd and later lines are not loaded.  It does not say
    the user is told, so the test pins the count and then records whether
    anything anywhere mentions the two lines that did not make it.
    """
    body = "".join("- step %02d\n" % i for i in range(1, 35))
    reply = "TOOL plan\n" + body + "@@END@@TURN@@DONE laid out"
    messages, pane, txt, scr, alive = _run_plan(
        tmp_path, reply, [LAID_OUT], cols=120, rows=80)

    lists = _checklists(messages)
    assert len(lists) == 1, \
        "Messages holds %d checklists for one plan:\n%s" % (
            len(lists), _evidence(messages, pane, txt))
    header, steps = lists[0]
    assert header == HEADER % (0, 32), \
        "the header reads %r, the contract's bound is 32:\n%s" % (
            header, _evidence(messages, pane, txt))
    assert len(steps) == 32, \
        "the checklist holds %d steps, the last %r:\n%s" % (
            len(steps), steps[-1:], _evidence(messages, pane, txt))
    assert steps[-1][1] == "step 32", \
        "the last step loaded is %r:\n%s" % (
            steps[-1][1], _evidence(messages, pane, txt))
    assert "step 33" not in "\n".join(messages), \
        "a line past the limit was loaded:\n%s" % _evidence(messages, pane, txt)

    said = [l for l in list(pane) + list(scr) + txt.splitlines()
            if re.search(r"drop|truncat|too many|limit|\b34\b", l, re.I)]
    print("anything mentioning the two dropped lines: %r" % (said,))
    assert alive, "a long plan ended the editor"


# @scenario SC-ai-agent-69
def test_a_step_longer_than_its_room_is_cut(tmp_path):
    """A plan line of 250 bytes.

    The contract says a longer line is cut, and the room for one step is 200 bytes
    with the last left for the terminator.  Messages wraps a row wider than itself
    over several screen rows, so the step is read by joining the rows under its
    header back together; the terminal is opened wide enough that the join is the
    checklist's own cut and not a window border.  The step carries no space, so
    the join cannot lose one.
    """
    long = "abcdefghij" * 25                     # 250 bytes
    assert len(long) == 250
    reply = "TOOL plan\n- " + long + "\n@@END@@TURN@@DONE laid out"
    messages, pane, txt, _scr, alive = _run_plan(
        tmp_path, reply, [LAID_OUT], cols=460, rows=30)

    at = next((i for i, r in enumerate(messages)
               if r.startswith("=== Agent plan")), -1)
    assert at >= 0, "Messages holds no checklist:\n%s" % _evidence(
        messages, pane, txt)
    got = "".join(r for r in messages[at + 1:] if r)
    want = "  [ ] " + long[:199]
    print("the 250-byte step is shown as %d bytes over %d screen row(s)"
          % (len(got) - 6, len([r for r in messages[at + 1:] if r])))
    assert got == want, \
        "the step is shown as %d bytes, the contract's cut leaves 199:\n" \
        "got  %r\nwant %r\n%s" % (len(got) - 6, got, want,
                                  _evidence(messages, pane, txt))
    assert alive, "a long step ended the editor"


# @scenario SC-ai-agent-69
def test_a_second_plan_replaces_the_checklist(tmp_path):
    """A second `TOOL plan` in the same run.

    The contract says a new plan replaces the old checklist, so the window is
    asked for the old plan's steps as well as the new one's.  It does not say
    the user is told; the test records whether any line anywhere mentions it.
    The old checklist used to stay in the window above the new one.
    """
    reply = "@@TURN@@".join([
        "TOOL plan\n- first plan alpha\n- first plan beta\n@@END",
        "TOOL plan\n- second plan only\n@@END",
        "DONE finished"])
    messages, pane, txt, scr, alive = _run_plan(tmp_path, reply, [LAID_OUT])

    header, steps = _last_checklist(messages)
    assert header == HEADER % (0, 1), \
        "the new checklist's header reads %r:\n%s" % (
            header, _evidence(messages, pane, txt))
    assert steps == [(" ", "second plan only")], \
        "the new checklist holds %r:\n%s" % (
            steps, _evidence(messages, pane, txt))
    said = [l for l in list(pane) + list(scr) + txt.splitlines()
            if re.search(r"replac|previous plan|old checklist", l, re.I)]
    print("anything saying the checklist was replaced: %r" % (said,))
    assert "first plan" not in "\n".join(messages), \
        "the old checklist is still in the Messages window:\n%s" % _evidence(
            messages, pane, txt)
    assert alive, "a second plan ended the editor"


def _wait_agent_done(trace, count, tmo):
    """Wait until the trace holds `count` finished runs."""
    for _ in range(int(tmo * 10)):
        if _trace_text(trace).count("agent done") >= count:
            return True
        time.sleep(0.1)
    return False


def _row_index(messages, lead):
    """The index of the first Messages row that starts with `lead`, -1 if none."""
    return next((i for i, r in enumerate(messages) if r.startswith(lead)), -1)


# @scenario SC-ai-agent-69
def test_a_run_keeps_its_checklist_and_review_together_and_the_next_run_starts_clean(tmp_path):
    """Within a run the checklist and the review share the Messages window;
    a new run empties it first, the way a compile does.

    Under auto a first run lays out a plan, ticks a step and writes a file, so
    the review's rows land below the checklist: one record of that run, and
    the rows Alt-T steps.  A second run (Alt-G g) lays out a new plan.  The
    first run's checklist and review are gone and the second run's checklist
    is the window's first content, as the Messages window after F9 holds the
    last compile only.  Runs used to pile up: the first review's rows stayed
    above the second checklist.
    """
    t = tmp_path / "t.c"
    reply = "@@TURN@@".join([
        PLAN_THREE, "TOOL step_done alpha step",
        "TOOL write_file %s\nint main(void){return 7;}\n@@END" % t,
        "DONE first run",
        "TOOL plan\n- second plan only\n@@END", "DONE second run"])
    trace = _trace_path(tmp_path)
    env = _env(tmp_path, reply, policy="auto")
    with WpeSession(str(tmp_path), SEED, env_extra=env,
                    cols=CHECK_COLS, rows=CHECK_ROWS) as s:
        _start_agent(s, "lay out a plan")
        assert _wait_trace(trace, "changeset n=", 90), \
            "the first run's review never opened:\n" + _trace_text(trace)
        first = _messages_rows(_screen(s))      # the review is waiting
        s.key("a", delay=1.0)                   # keep
        _start_agent(s, "lay out another plan")  # a second run, Alt-G g
        assert _wait_agent_done(trace, 2, 90), \
            "the second run never finished:\n" + _trace_text(trace)
        s._drain(1.5)
        second = _messages_rows(_screen(s))
        alive = s.alive()

    shown_1 = "\n".join(first)
    plan_at = _row_index(first, "=== Agent plan")
    review_at = _row_index(first, "=== AI changeset")
    assert plan_at >= 0 and review_at > plan_at, \
        "within the first run the review should follow its checklist:\n" + shown_1
    assert _checklists(first)[0][0] == HEADER % (1, 3), \
        "the first run's checklist should read 1/3 done:\n" + shown_1

    shown_2 = "\n".join(second)
    lists = _checklists(second)
    assert len(lists) == 1 and lists[0][1] == [(" ", "second plan only")], \
        "Messages should hold the second plan's checklist alone:\n" + shown_2
    assert "alpha step" not in shown_2, "the old checklist is still there:\n" + shown_2
    # the second run's own review ("no files changed") may follow its
    # checklist; the first run's review, its row and its summary may not
    assert "=== AI changeset: 1 change" not in shown_2 \
        and "t.c:1:" not in shown_2 \
        and "[AI] changes kept" not in shown_2, \
        "the first run's review is still in the window:\n" + shown_2
    content = [r for r in second if r.strip()]
    assert content and content[0].startswith("=== Agent plan"), \
        "the second run's checklist should be the window's first content:\n" + shown_2
    assert alive, "a second run ended the editor"
