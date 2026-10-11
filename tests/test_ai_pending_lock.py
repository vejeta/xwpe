"""A window waiting for an assistant reply is read-only, and a second request queues.

The reply of an Edit, a block Edit or a Complete is spliced over the lines that
were sent, at the line numbers captured when the request left.  While the reply
is in flight the target window therefore must not move underneath it: a key that
would insert, delete or replace text is refused and the status row says the
assistant is working and that Esc cancels.  Everything else keeps working --
cursor keys in that window, other windows, the menus, and Esc, which cancels the
task and releases the window at once.

Without the lock, three things went wrong, and each has a test here: a character
typed during a whole-file Edit came back as a second hunk ("revert what you just
typed"); a line inserted above a marked block made the splice land on the wrong
lines; and a completion rebuilt the file from the before/after captured at
request time, silently dropping what had been typed meanwhile.

The second half covers the other half of the same contract: a request started
while a task runs is queued behind it instead of refused, at every entry point
(Edit, Complete, Multi-file), announced on a pane line of its own that the
progress indicator does not overwrite, and launched when the running task ends.

Red/green: on the unfixed tree the typed characters reach the buffer (the review
then offers 2 hunks instead of 1, and the typed text is on screen), and a second
Alt-G e answers "a task is already running" with no queued notice and never runs;
with the fix the buffer is untouched, the status row explains why, and the second
request is announced as queued and runs after the first.
"""
import os
import subprocess
import time
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

ORIG = "int a = 1;\nint b = 2;\nint c = 3;\nint d = 4;\nint e = 5;\n"
NEW = "int a = 1;\nint b = 2;\nint c = 3;\nint d = 4;\nint e = 55;\n"
REGION = "int b = 22;\nint c = 33;\n"          # a block edit answers the region only
SPLICED = "int a = 1;\nint b = 22;\nint c = 33;\nint d = 4;\nint e = 5;\n"
SUGGESTION = "/*done*/"                        # what Complete inserts at the cursor

MOCK_DELAY_MS = 5000           # the reply lands well after the keys below
LONG_DELAY_MS = 9000           # room for a menu, a second window and typing in it
QUEUE_DELAY_MS = 6000          # long enough to type a second request and watch it
READABLE_S = 2.0               # the queued notice must survive this much spinning

BOX = "Proposed change"        # the hunk review box title
WORKING = "assistant is working"
CTRL_HOME = "\033[1;5H"        # cursor to the top of the file (line 1, column 1)
ESC = "\033"


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _trace(path):
    return path.read_text() if path.exists() else ""


def _wait_trace(s, path, needle, tmo):
    end = time.time() + tmo
    while time.time() < end:
        s._drain(0.5)
        if needle in _trace(path):
            return True
    return False


def _wait_screen(s, needle, tmo):
    end = time.time() + tmo
    while time.time() < end:
        s._drain(0.5)
        if any(needle in r for r in s.display(drain=0)):
            return True
    return False


def _screen(s):
    return "\n".join(s.display())


def _on_screen(s, needle):
    return any(needle in r for r in s.display())


def _status_row(s):
    return s.display()[-1]


def _env(trace, reply, delay_ms=MOCK_DELAY_MS):
    return {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_MOCK_DELAY_MS": str(delay_ms),
        "XWPE_AI_TRACE": str(trace),
    }


def _start_edit(s, trace, instr="do it"):
    s.key(ALT.AI); s.key("e"); s._drain(0.5)
    s.key(instr); s.key("\r", delay=0.8)
    assert _wait_trace(s, trace, "edit stream fd=", 10), \
        "the edit never started:\n" + _trace(trace)


def _mark_lines_two_and_three(s):
    s.key("\033[B")                         # cursor -> line 2 ("int b = 2;")
    s.key(ALT.BLOCK, "b", delay=0.4)        # Block -> Begin Mark
    s.key("\033[B"); s.key("\033[B")        # cursor -> line 4, column 1
    s.key(ALT.BLOCK, "e", delay=0.4)        # Block -> End Mark => lines 2-3


# @scenario SC-ai-edit-08
def test_typing_during_pending_edit_leaves_the_file_alone(tmp_path):
    trace = tmp_path / "e.trace"
    with WpeSession(str(tmp_path), ORIG, env_extra=_env(trace, NEW), filename="t.c") as s:
        _start_edit(s, trace)

        s.key("Z"); s.key("Z"); s.key("Z")
        s._drain(0.6)
        assert "ZZZ" not in _screen(s).replace(" ", ""), \
            "typing reached the window waiting for its reply:\n" + _screen(s)

        # The review that follows offers the model's change and nothing else:
        # a second hunk would be "revert what you just typed".
        assert _wait_screen(s, BOX, 12), \
            "the review did not open:\n" + _screen(s)
        s.key("a", delay=1.0)                      # apply every hunk offered
        assert _wait_trace(s, trace, "edit applied", 8), \
            "the edit was not applied:\n" + _trace(trace)
        assert "edit applied 1/1" in _trace(trace), \
            "the review offered more than the model's own change:\n" + _trace(trace)
        s._drain(0.6)
        s.save()
        assert s.text() == NEW, "the file on disk is not the reviewed change:\n" + repr(s.text())


# @scenario SC-ai-edit-08
def test_status_row_says_the_assistant_is_working(tmp_path):
    trace = tmp_path / "e.trace"
    with WpeSession(str(tmp_path), ORIG, env_extra=_env(trace, NEW), filename="t.c") as s:
        _start_edit(s, trace)
        row = _status_row(s)
        assert WORKING in row, \
            "the status row does not say the assistant is working:\n%r\n%s" % (row, _screen(s))
        assert "Esc" in row, \
            "the status row does not offer Esc to cancel:\n%r" % row


# @scenario SC-ai-edit-08
def test_another_window_stays_editable_while_the_first_waits(tmp_path):
    trace = tmp_path / "e.trace"
    env = _env(trace, NEW, LONG_DELAY_MS)
    with WpeSession(str(tmp_path), ORIG, env_extra=env, filename="t.c") as s:
        _start_edit(s, trace)

        s.key(ALT.FILE, "n", delay=0.9)            # File -> New: an unrelated window
        s.key("QQQ"); s._drain(0.6)
        assert "QQQ" in _screen(s).replace(" ", ""), \
            "the lock spread to a window the task knows nothing about:\n" + _screen(s)
        assert s.alive(), "wpe died while typing into the second window"


# @scenario SC-ai-edit-08
def test_escape_cancels_and_the_window_takes_text_again(tmp_path):
    trace = tmp_path / "e.trace"
    with WpeSession(str(tmp_path), ORIG, env_extra=_env(trace, NEW), filename="t.c") as s:
        _start_edit(s, trace)

        s.key("Z"); s.key("Z"); s.key("Z")
        s._drain(0.5)
        assert "ZZZ" not in _screen(s).replace(" ", ""), \
            "the window was not locked during the wait:\n" + _screen(s)

        # Esc cancels the task.  The mock's reply comes from a child process
        # that sleeps out its delay, and tearing the stream down reaps it, so
        # give the cancel that long to come back.
        s.key(ESC, delay=0.8)
        s._drain(MOCK_DELAY_MS / 1000.0)
        assert _on_screen(s, "cancelled"), \
            "Esc did not cancel the pending edit:\n" + _screen(s)

        # The cancel arms the chat input on the pane; a second Esc leaves it and
        # the file window is focused again -- and takes text, as it did before.
        s.key(ESC, delay=0.8)
        s.key("QQQ"); s._drain(0.6)
        s.save()
        assert "QQQ" in s.text(), \
            "the window did not take text after the cancel:\n%s\n%s" % (
                repr(s.text()), _screen(s))
        assert "ZZZ" not in s.text(), \
            "the keys refused during the wait landed after all:\n" + repr(s.text())


# @scenario SC-ai-edit-31
def test_block_edit_splice_lands_on_the_marked_lines(tmp_path):
    trace = tmp_path / "e.trace"
    with WpeSession(str(tmp_path), ORIG, env_extra=_env(trace, REGION), filename="t.c") as s:
        _mark_lines_two_and_three(s)
        s.key(ALT.AI); s.key("e"); s._drain(0.5)
        s.key("double them"); s.key("\r", delay=0.8)
        assert _wait_trace(s, trace, "sel=1..2", 10), \
            "the block edit never started on lines 1..2:\n" + _trace(trace)

        # A line inserted above the block would shift the lines the reply is
        # spliced over.  It cannot happen: the window is locked.
        s.key(CTRL_HOME, delay=0.5)
        s.key("\r"); s.key("\r")
        s._drain(0.6)
        assert _wait_screen(s, BOX, 12), \
            "the review did not open:\n" + _screen(s)
        s.key("a", delay=1.0)
        assert _wait_trace(s, trace, "edit applied", 8), \
            "the block edit was not applied:\n" + _trace(trace)
        s._drain(0.6)
        s.save()
        assert s.text() == SPLICED, \
            "the splice did not land on the marked lines:\n" + repr(s.text())


# @scenario SC-ai-edit-39
def test_completion_is_not_raced_by_typing(tmp_path):
    trace = tmp_path / "e.trace"
    with WpeSession(str(tmp_path), ORIG, env_extra=_env(trace, SUGGESTION), filename="t.c") as s:
        s.key(ALT.AI); s.key("c"); s._drain(0.5)
        assert _wait_trace(s, trace, "complete stream fd=", 10), \
            "the completion never started:\n" + _trace(trace)

        s.key("Y"); s.key("Y")
        s._drain(0.6)
        assert "YY" not in _screen(s).replace(" ", ""), \
            "typing reached the window waiting for its completion:\n" + _screen(s)

        # The suggestion is offered in the review box first; y takes it.
        assert _wait_screen(s, "Proposed completion", 12), \
            "the completion was not offered for review:\n" + _trace(trace) + "\n" + _screen(s)
        s.key("y", delay=0.8)
        assert _wait_screen(s, "completed at cursor", 12), \
            "the completion never landed:\n" + _trace(trace) + "\n" + _screen(s)
        s._drain(0.6)
        s.save()
        txt = s.text()
        assert "YY" not in txt, \
            "the refused keys reached the file after all:\n" + repr(txt)
        assert txt.startswith(SUGGESTION), \
            "the suggestion did not land at the cursor:\n" + repr(txt)
        assert txt.endswith(ORIG), \
            "the completion lost part of the file:\n" + repr(txt)


# @scenario SC-ai-edit-04
def test_second_edit_is_queued_and_runs_after_the_first(tmp_path):
    trace = tmp_path / "e.trace"
    env = _env(trace, NEW, QUEUE_DELAY_MS)
    with WpeSession(str(tmp_path), ORIG, env_extra=env, filename="t.c") as s:
        _start_edit(s, trace, "FIRSTEDIT")

        s.key(ALT.AI); s._drain(0.5); s.key("e"); s._drain(0.6)
        s.key("SECONDEDIT"); s.key("\r", delay=0.8)
        assert _on_screen(s, "queued"), \
            "the second edit was not queued:\n" + _screen(s)
        s._drain(READABLE_S)
        assert _on_screen(s, "queued"), \
            "the progress indicator overwrote the queued notice:\n" + _screen(s)

        assert _wait_screen(s, BOX, 15), \
            "the first edit's review did not open:\n" + _screen(s)
        s.key("q", delay=1.0)                      # discard it; the queue runs next
        assert _wait_trace(s, trace, "edit instr=SECONDEDIT", 15), \
            "the queued edit never ran:\n" + _trace(trace)


# @scenario SC-ai-edit-04
def test_complete_is_queued_behind_a_running_edit(tmp_path):
    trace = tmp_path / "e.trace"
    env = _env(trace, NEW, QUEUE_DELAY_MS)
    with WpeSession(str(tmp_path), ORIG, env_extra=env, filename="t.c") as s:
        _start_edit(s, trace, "FIRSTEDIT")

        s.key(ALT.AI); s._drain(0.5); s.key("c"); s._drain(0.6)
        assert _on_screen(s, "queued"), \
            "Complete was not queued behind the running edit:\n" + _screen(s)
        s._drain(READABLE_S)
        assert _on_screen(s, "queued"), \
            "the progress indicator overwrote the queued notice:\n" + _screen(s)

        assert _wait_screen(s, BOX, 15), \
            "the edit's review did not open:\n" + _screen(s)
        s.key("q", delay=1.0)
        assert _wait_trace(s, trace, "complete stream fd=", 15), \
            "the queued completion never ran:\n" + _trace(trace)


# @scenario SC-ai-edit-04
def test_multifile_is_queued_behind_a_running_edit(tmp_path):
    trace = tmp_path / "e.trace"
    env = _env(trace, NEW, QUEUE_DELAY_MS)
    with WpeSession(str(tmp_path), ORIG, env_extra=env, filename="t.c") as s:
        _start_edit(s, trace, "FIRSTEDIT")

        s.key(ALT.AI); s._drain(0.5); s.key("f"); s._drain(0.6)
        s.key("PLANTASK"); s.key("\r", delay=0.8)
        assert _on_screen(s, "queued"), \
            "Multi-file was not queued behind the running edit:\n" + _screen(s)
        s._drain(READABLE_S)
        assert _on_screen(s, "queued"), \
            "the progress indicator overwrote the queued notice:\n" + _screen(s)

        assert _wait_screen(s, BOX, 15), \
            "the edit's review did not open:\n" + _screen(s)
        s.key("q", delay=1.0)
        assert _wait_trace(s, trace, "plan task=PLANTASK", 15), \
            "the queued multi-file task never ran:\n" + _trace(trace)
