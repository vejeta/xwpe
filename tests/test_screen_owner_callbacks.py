"""The screen has one owner: callbacks and timers draw through it.

What arrives from the loop (a reply, a spinner's tick, a language server's
news, a program's run) is drawn only when no box, menu or run owns the
screen; what could not be drawn waits and is shown when the editor is its
own again.  These rows drive the real editor (wpe, terminal mode) with the
assistant's deterministic backend, a stand-in language server and a program
run by Ctrl-F9, and read the screen and the editor's trace (XWPE_UI_TRACE).
"""
import os
import re
import time

import pytest

import test_screen_owner as owner
from wpe_driver import WpeSession, ALT, WAIT_SCALE, assistant_build

ESC = "\033"
CTRL_F9 = "\033[20;5~"            # Run
POLL_S = 0.3
TRACE = "ui.trace"
AI_TRACE = "ai.trace"
HOLD_S = 2.0
WAITING_RE = re.compile(r"\[AI edit\] working \S\s+(\d+)s")
REVIEW_BOX = "Proposed change"
FIND_TITLE = "Find"

# The assistant's reply lands this long after the request, scaled like every
# wait of the driver.
REPLY_DELAY_MS = int(8000 * WAIT_SCALE)
RUN_REPLY_DELAY_MS = int(4500 * WAIT_SCALE)

SPINNER_SEED = "int a = 1;\nint b = 2;\nint c = 3;\n"
SPINNER_REPLY = "int a = 1;\nint b = 22;\nint c = 3;\n"
DEFERRED_SPINNER = "paint deferred the assistant's pane"
REPLAYED_SPINNER = "paint replay the assistant's pane"
WAITING_SEEN_S = 15
SPIN_AGAIN_S = 10


def _need_assistant():
    if not assistant_build():
        pytest.skip("wpe built without the assistant")


def _screen(w):
    return "\n".join(w.display(drain=0))


def _read(path):
    try:
        with open(path) as fh:
            return fh.read()
    except OSError:
        return ""


def _wait_for(w, predicate, timeout):
    deadline = time.time() + timeout * WAIT_SCALE
    while time.time() < deadline:
        w._drain(POLL_S)
        if predicate():
            return True
    return predicate()


def _waiting_seconds(w):
    """The seconds the assistant's waiting line shows, or -1."""
    found = WAITING_RE.search(_screen(w))
    if not found:
        return -1
    return int(found.group(1))


def _assistant_env(tmp_path, reply, delay_ms):
    return {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
            "XWPE_AI_MOCK_REPLY": reply,
            "XWPE_AI_MOCK_DELAY_MS": str(delay_ms),
            "XWPE_AI_TRACE": str(tmp_path / AI_TRACE),
            "XWPE_UI_TRACE": str(tmp_path / TRACE)}


def _ask_for_an_edit(w):
    w.key(ALT.AI)
    w.key("e")
    w._drain(0.4)
    w.key("change b")
    w.key("\r", delay=0.6)


# @scenario SC-screen-05
def test_a_spinner_ticking_under_a_box_resumes_after_it(tmp_path):
    """The assistant's waiting line turns every few ticks.  The Find dialog
    opens over it: the screen does not change while the dialog is up, the
    trace says the tick waited, and once Esc closes the dialog the waiting
    line turns again, its seconds past what they were."""
    _need_assistant()
    trace = tmp_path / TRACE
    env = _assistant_env(tmp_path, SPINNER_REPLY, REPLY_DELAY_MS)
    with WpeSession(str(tmp_path), SPINNER_SEED, filename="t.c", wait=1.5,
                    env_extra=env) as w:
        _ask_for_an_edit(w)
        assert _wait_for(w, lambda: _waiting_seconds(w) >= 0, WAITING_SEEN_S), \
            "the waiting line should show:\n%s" % _screen(w)
        w.menu(ALT.SEARCH, "f", delay=0.6)
        assert _wait_for(w, lambda: FIND_TITLE in _screen(w), WAITING_SEEN_S), \
            "Search > Find should open its dialog:\n%s" % _screen(w)
        w._drain(0.5)
        before = w.display(drain=0)
        seconds_before = _waiting_seconds(w)
        w._drain(HOLD_S)
        after = w.display(drain=0)
        assert after == before, \
            "nothing may be drawn while the dialog is up:\nbefore:\n%s\nafter:\n%s" \
            % ("\n".join(before), "\n".join(after))
        assert DEFERRED_SPINNER in _read(str(trace)), \
            "the tick that came under the dialog should wait:\n%s" \
            % _read(str(trace))
        w.key(ESC, delay=0.6)
        assert _wait_for(w, lambda: _waiting_seconds(w) > seconds_before,
                         SPIN_AGAIN_S), \
            "the waiting line should turn again once the dialog closes " \
            "(it showed %d s before):\n%s" % (seconds_before, _screen(w))
        assert REPLAYED_SPINNER in _read(str(trace)), \
            "the waiting tick should be drawn by the key loop:\n%s" \
            % _read(str(trace))
        assert w.alive(), "wpe died closing the dialog"


# @scenario SC-screen-01
def test_lsp_diagnostics_under_a_dropdown_show_when_it_closes(tmp_path):
    """A dropdown is open when the language server publishes an error for
    the file.  Nothing is drawn while it is up; once it closes the
    "LSP: 1 error(s)" line is in Messages, which the server's publication
    would otherwise never bring back."""
    if not assistant_build():
        pytest.skip("wpe built without the assistant")
    env = owner._stand_in_server(tmp_path)
    try:
        with WpeSession(str(tmp_path), owner.SEED, filename="a.c", wait=1.5,
                        env_extra=env) as w:
            w.key("\033q", delay=0.4)
            w.key("e", delay=0.2)
            assert _wait_for(w, lambda: "NOTE textDocument/didOpen"
                             in _read(str(tmp_path / owner.STUB_LOG)),
                             owner.LSP_STARTED_S), \
                "Alt-Q E should start the stand-in server:\n%s" % _screen(w)
            w._drain(1.0)
            w.key(ALT.SEARCH, delay=0.6)
            w._drain(0.5)
            before = w.display(drain=0)
            owner._release_the_stand_in(tmp_path)
            assert _wait_for(w, lambda: "PUBLISHED"
                             in _read(str(tmp_path / owner.STUB_LOG)),
                             owner.PUBLISHED_S), \
                "the stand-in should have published"
            w._drain(HOLD_S)
            after = w.display(drain=0)
            assert after == before, \
                "nothing may be drawn under the dropdown:\nbefore:\n%s\nafter:\n%s" \
                % ("\n".join(before), "\n".join(after))
            w.key(ESC, delay=0.8)
            assert _wait_for(w, lambda: "LSP: 1 error" in _screen(w),
                             owner.PUBLISHED_S), \
                "once the dropdown closes Messages should say the server " \
                "found 1 error:\n%s" % _screen(w)
            assert w.alive(), "wpe died closing the dropdown"
    finally:
        owner._end_the_stand_in(tmp_path)


SERVER_EXITED_LINE = "Language server stopped"


# @scenario SC-screen-01
def test_a_language_server_that_ends_under_a_dropdown_says_so_when_it_closes(tmp_path):
    """A dropdown is open when the language server exits.  Messages is not
    raised and written under it; once the dropdown closes the line that
    says the server stopped is there."""
    if not assistant_build():
        pytest.skip("wpe built without the assistant")
    env = owner._stand_in_server(tmp_path)
    try:
        with WpeSession(str(tmp_path), owner.SEED, filename="a.c", wait=1.5,
                        env_extra=env) as w:
            w.key("\033q", delay=0.4)
            w.key("e", delay=0.2)
            assert _wait_for(w, lambda: "NOTE textDocument/didOpen"
                             in _read(str(tmp_path / owner.STUB_LOG)),
                             owner.LSP_STARTED_S), \
                "Alt-Q E should start the stand-in server:\n%s" % _screen(w)
            w._drain(1.0)
            w.key(ALT.SEARCH, delay=0.6)
            w._drain(0.5)
            before = w.display(drain=0)
            owner._end_the_stand_in(tmp_path)
            w._drain(HOLD_S)
            after = w.display(drain=0)
            assert after == before, \
                "nothing may be drawn under the dropdown:\nbefore:\n%s\nafter:\n%s" \
                % ("\n".join(before), "\n".join(after))
            w.key(ESC, delay=0.8)
            assert _wait_for(w, lambda: SERVER_EXITED_LINE in _screen(w),
                             owner.PUBLISHED_S), \
                "once the dropdown closes Messages should say the server " \
                "stopped:\n%s" % _screen(w)
            assert w.alive(), "wpe died closing the dropdown"
    finally:
        owner._end_the_stand_in(tmp_path)


# The program outlives the reply however the waits are scaled.
RUN_SLEEP_S = int(10 * max(1.0, WAIT_SCALE))
RUN_SRC = (
    "#include <stdio.h>\n"
    "#include <unistd.h>\n"
    "int main(void)\n"
    "{\n"
    "  setvbuf(stdout, NULL, _IONBF, 0);\n"
    "  puts(\"RUNSTART\");\n"
    "  sleep(%d);\n" % RUN_SLEEP_S +
    "  printf(\"%s%s\\n\", \"RUN\", \"DONE\");\n"
    "  return 0;\n"
    "}\n"
)
RUN_REPLY = RUN_SRC.replace("sleep(%d)" % RUN_SLEEP_S, "sleep(%d)" % (RUN_SLEEP_S - 1))
RUN_BUILD_S = 25
RUN_END_S = 25
REPLY_PROBE_S = 1.5


# @scenario SC-screen-02
def test_a_reply_that_lands_during_a_run_is_shown_when_the_run_ends(tmp_path):
    """Ctrl-F9 builds the program and runs it in Messages; the program
    sleeps.  The assistant's reply lands during the run: no review box opens
    over the program (its keys would be the box's), the trace says the reply
    waited, and once the program has ended the box opens."""
    _need_assistant()
    trace = tmp_path / TRACE
    env = _assistant_env(tmp_path, RUN_REPLY, RUN_REPLY_DELAY_MS)
    with WpeSession(str(tmp_path), RUN_SRC, filename="r.c", wait=1.5,
                    env_extra=env) as w:
        _ask_for_an_edit(w)
        w.key(CTRL_F9, delay=0.5)
        assert _wait_for(w, lambda: "RUNSTART" in _screen(w), RUN_BUILD_S), \
            "Ctrl-F9 should build and start the program:\n%s" % _screen(w)
        assert _wait_for(w, lambda: "edit stream fd=" in
                         _read(str(tmp_path / AI_TRACE)), RUN_BUILD_S)
        assert _wait_for(w, lambda: "reply held" in
                         _read(str(tmp_path / AI_TRACE)), RUN_END_S), \
            "the reply that lands during the run should be held:\n%s\n%s" \
            % (_read(str(tmp_path / AI_TRACE)), _screen(w))
        w._drain(REPLY_PROBE_S)
        assert REVIEW_BOX not in _screen(w), \
            "the review box opened over the running program:\n%s" % _screen(w)
        assert "RUNDONE" not in _screen(w), "the program ended too early"
        assert _wait_for(w, lambda: REVIEW_BOX in _screen(w), RUN_END_S), \
            "once the program has ended the review should open:\n%s" \
            % _screen(w)
        assert "paint replay the assistant's reply" in _read(str(trace)), \
            _read(str(trace))
        assert w.alive(), "wpe died after the run"
