"""Keys typed before a question appears are never its answer, under X11/Xft.

The terminal rows are in tests/test_question_typeahead.py (they read the
editor's dump).  The graphical launcher passes no dump descriptor, so these
rows read what the editor did: the files it left and its two traces.  The
editor is held, by the test backend's named hold (a fifo), at the point just
before a question is drawn; the row types the burst with the server, then
opens the fifo, so the keys are queued before the question by order of
events and nothing waits for a time.  The same module runs under the native
Wayland backend through tests/wayland/test_question_typeahead.py.

Declared terminal-only, with the reason: a bracketed paste exists only on the
terminal (a graphical paste is a key the question drops like any other), and
the resize row needs the terminal's ncurses resize to give a pending resize
back, which the graphical displays handle in their frame owner.

The message boxes (the close question, the overwrite question, the About box)
open from a key, and the key that opens a box and the key that would answer it
are two injected events.  The editor is held at the owner's settle of the box,
by $XWPE_UI_HOLD ("<question>=<fifo>", honoured only with $XWPE_UI_TRACE set),
which stops it once just before the box's first read and wakes on the fifo or
on SIGTERM.  The row types while it is held and then opens the fifo, so the key
is queued before the box by order of events here too.  The click row of the
terminal suite has no twin: it needs the pixel of a button of a box, which the
graphical harness cannot read without a screenshot search.
Self-skips if the graphical harness is missing; the assistant rows also need
--enable-ai.
"""
import errno
import os
import re
import subprocess

import pytest
from conftest import wait_for, XWPE_BIN

WAIT_S = 20.0
AI_TRACE = "ai.trace"            # relative: the editor runs in the row's folder
UI_TRACE = "ui.trace"
HOLD_FIFO = "../qt-hold.fifo"    # beside the row's folder, not inside the workspace
RUN_REPLY = "TOOL write_file made.txt\nnew\n@@END@@TURN@@DONE made it"
CMD_REPLY = "TOOL run_command echo ran > ran.txt\n@@TURN@@DONE ran it"
WRITE_REPLY = "TOOL write_file w.txt\nwritten\n@@END@@TURN@@DONE wrote it"


def _has_ai():
    try:
        out = subprocess.run(["strings", XWPE_BIN],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"Alt-G AI" in out
    except Exception:
        return False


def _env(reply, point, policy):
    return {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_POLICY": policy,
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_TRACE": AI_TRACE,
        "XWPE_UI_TRACE": UI_TRACE,
        "XWPE_AI_MOCK_HOLD": "%s=%s" % (point, HOLD_FIFO),
        "XDG_STATE_HOME": "",
    }


REVIEW_ENV = _env(RUN_REPLY, "review", "auto")
APPROVE_ENV = _env(CMD_REPLY, "approve", "ask")
WRITE_ENV = _env(WRITE_REPLY, "write", "ask")


class Held:
    """The editor stopped at a named point; the row types, then lets it go."""

    def __init__(self, xwpe):
        self.x = xwpe
        self.folder = os.path.dirname(xwpe.srcfile)
        self.fifo = os.path.normpath(os.path.join(self.folder, HOLD_FIFO))
        if os.path.exists(self.fifo):
            os.unlink(self.fifo)
        os.mkfifo(self.fifo)

    def _read(self, name):
        try:
            with open(os.path.join(self.folder, name)) as fh:
                return fh.read()
        except OSError:
            return ""

    def ai(self):
        return self._read(AI_TRACE)

    def ui(self):
        return self._read(UI_TRACE)

    def made(self, name):
        return os.path.exists(os.path.join(self.folder, name))

    def start_run(self, request, point):
        self.x.key("alt+g")
        self.x.key("g", delay=0.6)
        self.x.type(request)
        self.x.key("Return", delay=0.2)
        wait_for(lambda: "mock hold %s" % point in self.ai(),
                 "the editor to stop at the %s hold" % point, WAIT_S)

    def release(self):
        def open_writer():
            try:
                return os.open(self.fifo, os.O_WRONLY | os.O_NONBLOCK)
            except OSError as e:
                if e.errno == errno.ENXIO:
                    return None
                raise
        fd = wait_for(open_writer, "the held editor to read its fifo", WAIT_S)
        os.close(fd)

    def dropped(self, what):
        """How many presses the question @what ignored, once it settled."""
        found = wait_for(
            lambda: re.search(r"ask %s dropped=(\d+)" % what, self.ui()),
            "the %s question to settle" % what, WAIT_S)
        return int(found.group(1))

    def review_is_over(self):
        return any(m in self.ai() for m in ("changeset keep all",
                                            "changeset revert all",
                                            "changeset file-by-file",
                                            "changeset commit"))


def _settled_or_taken(h, what):
    """Wait until the question settled or the typed keys took the review
    away; a row that finds the second fails on its assertions at once."""
    wait_for(lambda: re.search(r"ask %s dropped=" % what, h.ui()) or h.review_is_over(),
             "the %s question, or what took the typed keys" % what, WAIT_S)


# @scenario SC-question-typeahead-13
@pytest.mark.skipif(not _has_ai(), reason="xwpe built without --enable-ai")
@pytest.mark.parametrize("xwpe", [REVIEW_ENV], indirect=True)
def test_run_the_tests_typed_in_the_gap_does_not_revert_the_run(xwpe):
    """R1: the r of 'run the tests' typed as the run ends is not the review's
    answer: the file stays, no revert is traced, the question dropped the 13
    presses, and its words were on the screen when it settled."""
    h = Held(xwpe)
    h.start_run("make it", "review")
    xwpe.type("run the tests")
    h.release()
    _settled_or_taken(h, "review")
    assert h.made("made.txt"), "the typed r reverted the run: made.txt is gone"
    assert "changeset revert all" not in h.ai()
    assert h.dropped("review") == 13, h.ui()
    assert "paint pending" not in h.ui()


# @scenario SC-question-typeahead-14
@pytest.mark.skipif(not _has_ai(), reason="xwpe built without --enable-ai")
@pytest.mark.parametrize("xwpe", [REVIEW_ENV], indirect=True)
def test_an_answer_after_the_gap_is_taken_and_nothing_typed_drops_nothing(xwpe):
    """R2: keys typed before are dropped, `a` after the question keeps all;
    the editor then takes typing again."""
    h = Held(xwpe)
    h.start_run("make it", "review")
    xwpe.type("zz")
    h.release()
    _settled_or_taken(h, "review")
    assert h.dropped("review") == 2, h.ui()
    assert not h.review_is_over(), "the review ended with no answer:\n" + h.ai()
    xwpe.key("a", delay=0.5)
    wait_for(lambda: "changeset keep all" in h.ai(), "the keep to be traced", WAIT_S)
    assert h.made("made.txt")


# @scenario SC-question-typeahead-15
@pytest.mark.skipif(not _has_ai(), reason="xwpe built without --enable-ai")
@pytest.mark.parametrize("xwpe", [REVIEW_ENV], indirect=True)
def test_words_typed_in_the_gap_do_not_pick_a_review_key(xwpe):
    """R3: 'second task' (c), 'fix it' (f) and Enter all hit a review key
    when typed before it; none may."""
    h = Held(xwpe)
    h.start_run("make it", "review")
    xwpe.type("fix it")
    xwpe.key("Return", delay=0.2)
    h.release()
    _settled_or_taken(h, "review")
    assert not h.review_is_over(), "a typed key answered the review:\n" + h.ai()
    assert h.made("made.txt")
    assert h.dropped("review") == 7, h.ui()


# @scenario SC-question-typeahead-16
@pytest.mark.skipif(not _has_ai(), reason="xwpe built without --enable-ai")
@pytest.mark.parametrize("xwpe", [APPROVE_ENV], indirect=True)
def test_a_y_typed_before_a_command_approval_does_not_allow_it(xwpe):
    """R4: under ask, the y typed before the approval does not run the
    command; a y after the question is shown does."""
    h = Held(xwpe)
    h.start_run("run it", "approve")
    xwpe.type("y")
    h.release()
    assert h.dropped("approve") == 1, h.ui()
    assert not h.made("ran.txt"), "the typed y allowed the command"
    xwpe.key("y", delay=0.5)
    wait_for(lambda: h.made("ran.txt"), "the allowed command to run", WAIT_S)


# @scenario SC-question-typeahead-17
@pytest.mark.skipif(not _has_ai(), reason="xwpe built without --enable-ai")
@pytest.mark.parametrize("xwpe", [WRITE_ENV], indirect=True)
def test_a_y_typed_before_a_write_approval_does_not_allow_it(xwpe):
    """R4 (write): the write confirmation box is a question too."""
    h = Held(xwpe)
    h.start_run("write it", "write")
    xwpe.type("y")
    h.release()
    assert h.dropped("diff-box") == 1, h.ui()
    assert not h.made("w.txt"), "the typed y allowed the write"
    xwpe.key("y", delay=0.5)
    wait_for(lambda: h.made("w.txt"), "the allowed write to land", WAIT_S)


# @scenario SC-question-typeahead-18
@pytest.mark.skipif(not _has_ai(), reason="xwpe built without --enable-ai")
@pytest.mark.parametrize("xwpe", [REVIEW_ENV], indirect=True)
def test_keys_after_the_question_is_shown_all_act(xwpe):
    """R5: the settle drops once per question; PgDn and r pressed after it
    both act (r reverts)."""
    h = Held(xwpe)
    h.start_run("make it", "review")
    h.release()
    assert h.dropped("review") == 0, h.ui()
    xwpe.key("Next", delay=0.3)
    xwpe.key("r", delay=0.5)
    wait_for(lambda: "changeset revert all" in h.ai(), "the revert to be traced", WAIT_S)
    assert not h.made("made.txt"), "r after the question did not revert"


# ---- the message boxes, held at the owner's settle -------------------------

def _hold_env(question):
    return {
        "XWPE_UI_TRACE": UI_TRACE,
        "XWPE_UI_HOLD": "%s=%s" % (question, HOLD_FIFO),
        "XDG_STATE_HOME": "",
    }


MESSAGE_ENV = _hold_env("Message")
ABOUT_ENV = _hold_env("the About box")
OLD_TEXT = "old text\n"
SCREEN_CHANGED = 2000          # pixels a box that appears or goes changes at least
BOX_GONE_SHARE = 5             # a box taken away leaves less than 1/5 of its pixels


class BoxHeld(Held):
    """The editor held at the settle of a message box."""

    def wait_hold(self, question):
        wait_for(lambda: "hold %s" % question in self.ui(),
                 "the editor to stop at the hold of %r" % question, WAIT_S)

    def saved(self, name="t.c"):
        try:
            with open(os.path.join(self.folder, name)) as fh:
                return fh.read()
        except OSError:
            return ""


# @scenario SC-question-typeahead-32
@pytest.mark.parametrize("xwpe", [MESSAGE_ENV], indirect=True)
def test_a_key_typed_before_the_close_question_does_not_answer_it(xwpe):
    """The n typed while the close question is about to be read gives nothing
    up: the question ignores it, and the y typed after it saves the edit."""
    h = BoxHeld(xwpe)
    xwpe.type("X")
    xwpe.key("ctrl+w", delay=0.2)
    h.wait_hold("Message")
    xwpe.type("n")
    h.release()
    assert h.dropped("Message") == 1, h.ui()
    xwpe.key("y", delay=0.5)
    wait_for(lambda: h.saved().startswith("Xint main"),
             "the y after the question to save the edit", WAIT_S)


# @scenario SC-question-typeahead-33
@pytest.mark.parametrize("xwpe", [MESSAGE_ENV], indirect=True)
def test_a_y_typed_before_the_overwrite_question_does_not_overwrite(xwpe):
    """Save As over a file that exists: the y typed before the question is
    read replaces nothing, and the y after it does."""
    h = BoxHeld(xwpe)
    with open(os.path.join(h.folder, "x.c"), "w") as fh:
        fh.write(OLD_TEXT)
    xwpe.key("alt+f", delay=0.4)
    xwpe.key("a", delay=0.8)
    xwpe.type("x.c")
    xwpe.key("alt+s", delay=0.2)
    h.wait_hold("Message")
    xwpe.type("y")
    h.release()
    assert h.dropped("Message") == 1, h.ui()
    assert h.saved("x.c") == OLD_TEXT, "the typed y replaced the file"
    xwpe.key("y", delay=0.5)
    wait_for(lambda: h.saved("x.c") != OLD_TEXT,
             "the y after the question to replace the file", WAIT_S)


# @scenario SC-question-typeahead-34
@pytest.mark.parametrize("xwpe", [ABOUT_ENV], indirect=True)
def test_a_key_typed_before_an_information_box_does_not_dismiss_it(xwpe):
    """The About box stays when a key was typed before it was read, and the
    key pressed after it takes it away."""
    from conftest import changed_pixels
    h = BoxHeld(xwpe)
    before = xwpe.screenshot()
    xwpe.esc_menu("#", "a", delay=0.2)
    h.wait_hold("the About box")
    xwpe.type("z")
    h.release()
    assert h.dropped("the About box") == 1, h.ui()
    shown = xwpe.screenshot()
    box = changed_pixels(before, shown)
    assert box > SCREEN_CHANGED, "the About box is not on the screen (%d)" % box
    xwpe.key("q", delay=0.6)
    gone = changed_pixels(before, xwpe.screenshot())
    assert gone < box // BOX_GONE_SHARE, "the key after the box did not take it away"
