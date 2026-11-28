"""Keys typed before a question appears are never its answer (terminal).

The assistant asks questions whose answer changes or destroys something: the
review of a run's changes (keep, revert, file by file, commit), the approval
of a command or a write.  They open from the end of a run, while the user may
still be typing the next request.  A question is answered only by a key
pressed after it was shown: what was typed before, and pasted text, is
dropped, and the question says so.

The message boxes the user opens with a key are questions of the same kind:
Close and Quit over a file with edits ("Do you want to save File ?"), Save As
over a file that exists ("already exists. Overwrite it?"), the boxes that only
say something (an error, About, System Info).  "Ctrl-W, then n" written in one
burst used to give a file's edits up before the question was on the screen.
They say nothing about a key they ignored: the box staying open is the sign.

Each row holds the editor at the point just before the question is drawn
(the test backend's named hold, a fifo: the editor stops, the row types the
burst, then opens the fifo), so the keys are queued before the question by
order of events and no row waits for a time.  The editor's dump says a
QUESTION waits (ctx=question:<what>), the traces say what was ignored.  A
message box keeps its own place in the dump (ctx=message) and carries what it
ignored there (dropped=, pasted=); its rows hold the editor with no fifo but
with one write, which the terminal delivers whole, so its keys are queued
before the box by order of events.  They are terminal-only: under X11 and
Wayland the key that opens a box and the key that would answer it are
separate events, and nothing holds the editor between them and the box's
first read.

Rows G1-G5 are guards: places to type or move keep their type-ahead.
Not covered headlessly: a key the mouse poll read ahead and gave back (the
drop takes that slot first).  The poll runs only inside a click or drag loop,
and every hold stops the editor inside the agent's run, where none is active.
Run: tests/.venv/bin/python -m pytest -q tests/test_question_typeahead.py
"""
import errno
import fcntl
import os
import struct
import subprocess
import signal
import stat
import termios

import pytest

pytest.importorskip("pyte")

from edit_model.oracle import ENTER, F2, EditSession, alt, ctrl  # noqa: E402
from gui_owner import wait_for  # noqa: E402
from host_tools import needs  # noqa: E402
from wpe_driver import WPE_BIN  # noqa: E402

ORIG = "int main(void){return 0;}\n"
RUN_REPLY = "TOOL write_file made.txt\nnew\n@@END@@TURN@@DONE made it"
APPROVE_CMD_REPLY = "TOOL run_command echo ran > ran.txt\n@@TURN@@DONE ran it"
APPROVE_WRITE_REPLY = "TOOL write_file w.txt\nwritten\n@@END@@TURN@@DONE wrote it"
WAIT_S = 20
END_WAIT_S = 10          # an editor told to end finishes within this
F4 = "\033OS"
PGDN = "\033[6~"
PASTE_BEGIN, PASTE_END = "\033[200~", "\033[201~"
# What the question says when it ignored keys, as the two Messages rows
# (the pane under it docks the Messages window to 38 columns at 80).
IGNORED_KEYS = ("[AI] keys typed before this", "question were ignored")
IGNORED_PASTE = ("[AI] pasted text ignored:", "answer the question first")
KEYS_FIRST_ROW = "answer the review first:"


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"AI changeset" in out
    except Exception:
        return False


pytestmark = [
    pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai"),
    needs("git"),
]


def _git_repo(folder):
    def g(*a):
        subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t",
                        "-c", "commit.gpgsign=false", *a],
                       cwd=folder, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    with open(os.path.join(folder, "t.c"), "w") as fh:
        fh.write(ORIG)
    g("init", "-q")
    g("add", "t.c")
    g("commit", "-q", "-m", "seed")


class Held:
    """A live editor stopped at a named point of the test backend."""

    def __init__(self, tmp_path, reply, point, policy="auto", git=False, extra=None):
        self.folder = str(tmp_path)
        self.aux = str(tmp_path.parent / (tmp_path.name + "-aux"))
        os.mkdir(self.aux)
        self.fifo = os.path.join(self.aux, "hold")
        os.mkfifo(self.fifo)
        self.ai_trace = os.path.join(self.aux, "ai.trace")
        self.point = point
        if git:
            _git_repo(self.folder)
        env = {
            "XWPE_AI_ENABLE": "1",
            "XWPE_AI_BACKEND": "mock",
            "XWPE_AI_POLICY": policy,
            "XWPE_AI_MOCK_REPLY": reply,
            "XWPE_AI_TRACE": self.ai_trace,
            "XWPE_AI_MOCK_HOLD": "%s=%s" % (point, self.fifo),
        }
        env.update(extra or {})
        self.s = EditSession(self.folder, ORIG, env_extra=env)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.s.close()

    # -- reading -------------------------------------------------------------
    def pump(self):
        self.s._pump([self.s.master_fd, self.s.dump_fd], 0.05,
                     {"pty": 0, "dump": 0})

    def ai(self):
        try:
            with open(self.ai_trace) as fh:
                return fh.read()
        except OSError:
            return ""

    def ui(self):
        try:
            with open(self.s.trace_path) as fh:
                return fh.read()
        except OSError:
            return ""

    def screen(self):
        return "\n".join(self.s.display())

    def wait(self, event, what, timeout=WAIT_S):
        def ev():
            self.pump()
            return event()
        return wait_for(ev, what, timeout)

    def next_dump(self, pred, what, timeout=WAIT_S):
        """The first dump the editor writes from now on that @pred accepts."""
        def ev():
            self.pump()
            while self.s.reader.dumps:
                d = self.s.reader.dumps.pop(0)
                if pred(d):
                    self.s.last = d
                    self.s._drain_screen_now()   # the screen of the dump's moment
                    return d
            return None
        return wait_for(ev, what, timeout)

    # -- driving -------------------------------------------------------------
    def start_run(self, request):
        """Ask the agent; the editor is held at the point when it gets there."""
        self.s.send([alt("g")])
        self.s.send(["g"])
        self.s.send(list(request))
        os.write(self.s.master_fd, b"\r")
        self.wait(lambda: "mock hold %s" % self.point in self.ai(),
                  "the editor to stop at the %s hold" % self.point)

    def type_ahead(self, text):
        """Type while the editor is held: queued, not read."""
        os.write(self.s.master_fd, text.encode("utf-8"))

    def press(self, text):
        """Type after the question is shown: an answer, not type-ahead."""
        os.write(self.s.master_fd, text.encode("utf-8"))

    def shows(self, text):
        """Wait until the screen shows @text."""
        self.wait(lambda: text in self.screen(), "the screen to show %r" % text)

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

    def question(self, what):
        """Wait for the dump that says a question of that name waits."""
        d = self.next_dump(lambda d: d.ctx == "question:%s" % what,
                           "the dump to say a %s question waits" % what)
        self.s.keys_sent = d.seq       # dropped keys were never counted
        return d

    def outcome(self):
        """The first thing that happens after the release: the question
        waits, the review took the typed keys as its answer, or a box the
        typed keys opened."""
        marks = ("changeset keep all", "changeset revert all",
                 "changeset file-by-file", "changeset commit")

        def ev():
            self.pump()
            while self.s.reader.dumps:
                d = self.s.reader.dumps.pop(0)
                self.s.last = d
                if d.ctx.startswith("question:"):
                    self.s.keys_sent = d.seq
                    self.s._drain_screen_now()
                    return "question"
                if d.ctx.startswith("dialog:"):
                    return d.ctx
            trace = self.ai()
            for m in marks:
                if m in trace:
                    return m
            return None
        return wait_for(ev, "the question, or what took the typed keys", WAIT_S)

    def made(self, name="made.txt"):
        return os.path.exists(os.path.join(self.folder, name))


# @scenario SC-question-typeahead-01
def test_run_the_tests_typed_in_the_gap_does_not_revert_the_run(tmp_path):
    """R1: 'run the tests' typed as the run ends holds an r; it must not be
    the review's answer.  The file stays, no revert is traced, the dump says
    the review waits as a question, and the paint was not pending."""
    with Held(tmp_path, RUN_REPLY, "review") as h:
        h.start_run("make it")
        h.type_ahead("run the tests")
        h.release()
        assert h.outcome() == "question", h.ai()
        assert h.made(), "the typed r reverted the run: made.txt is gone"
        assert "changeset revert all" not in h.ai()
        assert "ask review dropped=13" in h.ui(), h.ui()
        assert "paint pending" not in h.ui()
        assert h.s.last.ctx == "question:review"


# @scenario SC-question-typeahead-02
def test_the_answer_after_the_gap_is_taken_and_the_ignored_keys_are_said(tmp_path):
    """R2: after the burst, `a` keeps the changes and the review's lines say
    what was ignored, whole at 80 columns, and say it for the keys typed and
    for nothing else."""
    with Held(tmp_path, RUN_REPLY, "review") as h:
        h.start_run("make it")
        h.type_ahead("zz")
        h.release()
        assert h.outcome() == "question", h.ai()
        shown = h.screen()
        for row in IGNORED_KEYS + (KEYS_FIRST_ROW,):
            assert row in shown, "the review does not say %r:\n%s" % (row, shown)
        h.press("a")
        h.wait(lambda: "changeset keep all" in h.ai(), "the keep to be traced")
        assert h.made()
        h.shows("[AI] changes kept")
        h.press("hello")
        h.shows("hello")            # keys after the answer reach the chat input


# @scenario SC-question-typeahead-03
def test_nothing_typed_says_nothing_was_ignored(tmp_path):
    """R2b: no keys in the gap, no line about ignored keys."""
    with Held(tmp_path, RUN_REPLY, "review") as h:
        h.start_run("make it")
        h.release()
        assert h.outcome() == "question", h.ai()
        shown = h.screen()
        assert IGNORED_KEYS[0] not in shown and IGNORED_PASTE[0] not in shown, shown
        assert KEYS_FIRST_ROW in shown
        assert "ask review dropped=0" in h.ui(), h.ui()


# @scenario SC-question-typeahead-04
@pytest.mark.parametrize("burst,box", [
    ("second task", "no commit box"),
    ("fix it", "no file by file"),
    ("ink wish\r", "no keep all by Enter"),
])
def test_words_typed_in_the_gap_do_not_pick_a_review_key(tmp_path, burst, box):
    """R3: 'c' opens the commit box, 'f' goes file by file and Enter keeps
    all; typed before the review, none does."""
    with Held(tmp_path, RUN_REPLY, "review", git=True) as h:
        h.start_run("make it")
        h.type_ahead(burst)
        h.release()
        assert h.outcome() == "question", "%s: %s\n%s" % (box, h.ai(), h.screen())
        assert h.s.last.ctx == "question:review"


# @scenario SC-question-typeahead-05
def test_a_y_typed_before_a_command_approval_does_not_allow_it(tmp_path):
    """R4: under ask, a y typed in the gap before the approval must not run
    the command; a y after the question is shown does."""
    with Held(tmp_path, APPROVE_CMD_REPLY, "approve", policy="ask") as h:
        h.start_run("run it")
        h.type_ahead("y")
        h.release()
        d = h.next_dump(lambda d: d.ctx == "question:approve",
                        "the dump to say the approval waits")
        assert not h.made("ran.txt"), "the typed y allowed the command"
        assert "ask approve dropped=1" in h.ui(), h.ui()
        h.press("y")
        h.wait(lambda: h.made("ran.txt"), "the allowed command to run")


# @scenario SC-question-typeahead-06
def test_a_y_typed_before_a_write_approval_does_not_allow_it(tmp_path):
    """R4 (write): the write confirmation box is a question too."""
    with Held(tmp_path, APPROVE_WRITE_REPLY, "write", policy="ask") as h:
        h.start_run("write it")
        h.type_ahead("y")
        h.release()
        d = h.next_dump(lambda d: d.ctx == "question:diff-box",
                        "the dump to say the write box waits")
        assert not h.made("w.txt"), "the typed y allowed the write"
        assert "ask diff-box dropped=1" in h.ui(), h.ui()
        h.press("y")
        h.wait(lambda: h.made("w.txt"), "the allowed write to land")


# @scenario SC-question-typeahead-07
def test_keys_after_the_question_is_shown_all_act_in_one_write(tmp_path):
    """R5: PgDn and r written together after the dump says QUESTION: both
    are answers (two keys read), the revert happens."""
    with Held(tmp_path, RUN_REPLY, "review") as h:
        h.start_run("make it")
        h.release()
        h.question("review")
        h.press(PGDN + "r")
        h.wait(lambda: "changeset revert all" in h.ai(), "the revert to be traced")
        assert not h.made(), "r after the question did not revert"


# @scenario SC-question-typeahead-08
def test_a_paste_in_the_gap_is_not_an_answer_and_is_said(tmp_path):
    """R6: a bracketed paste holding r is ignored, the review says so, the
    next typed key is read as typing again."""
    with Held(tmp_path, RUN_REPLY, "review") as h:
        h.start_run("make it")
        h.type_ahead(PASTE_BEGIN + "r" + PASTE_END)
        h.release()
        assert h.outcome() == "question", h.ai()
        assert h.made()
        shown = h.screen()
        for row in IGNORED_PASTE:
            assert row in shown, "no paste line %r:\n%s" % (row, shown)
        assert "pasted=1" in h.ui(), h.ui()
        h.press("a")
        h.wait(lambda: "changeset keep all" in h.ai(), "the keep to be traced")
        h.press("ok")
        h.shows("ok")               # the paste flag did not swallow the typing


# @scenario SC-question-typeahead-20
def test_a_paste_after_the_question_is_shown_is_not_an_answer_either(tmp_path):
    """R6b: pasted text that arrives while the question waits is not its
    answer: the read goes on until the paste has ended, so the r in it does
    not revert, and the key typed after it does answer."""
    with Held(tmp_path, RUN_REPLY, "review") as h:
        h.start_run("make it")
        h.release()
        h.question("review")
        h.press(PASTE_BEGIN + "r" + PASTE_END + "a")
        h.wait(lambda: "changeset keep all" in h.ai() or "changeset revert all" in h.ai(),
               "the review to take an answer")
        assert "changeset revert all" not in h.ai(), "the pasted r reverted the run"
        assert h.made()


# @scenario SC-question-typeahead-09
def test_a_resize_in_the_gap_is_laid_out_and_the_review_still_waits(tmp_path):
    """R7: a resize while held is applied, not dropped, and the question
    still waits."""
    cols, rows = 100, 34
    with Held(tmp_path, RUN_REPLY, "review") as h:
        h.start_run("make it")
        fcntl.ioctl(h.s.master_fd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", rows, cols, 0, 0))
        h.s.screen.resize(rows, cols)
        h.release()
        assert h.outcome() == "question", h.ai()
        assert len(h.s.display()) == rows
        assert KEYS_FIRST_ROW in h.screen()
        assert h.made()


# @scenario SC-question-typeahead-19
def test_mouse_motion_in_the_gap_is_not_counted_as_typing(tmp_path):
    """The pointer moved while the run ended and nothing was typed: the
    question counts no key and says nothing was ignored."""
    motion = "\x1b[<35;10;10M"           # an SGR report: motion, no button
    with Held(tmp_path, RUN_REPLY, "review") as h:
        h.start_run("make it")
        h.type_ahead(motion)
        h.release()
        assert h.outcome() == "question", h.ai()
        assert "ask review dropped=0" in h.ui(), h.ui()
        assert IGNORED_KEYS[0] not in h.screen()
        assert h.made()


# @scenario SC-question-typeahead-21
def test_a_resize_in_the_gap_before_the_write_box_keeps_the_box_up(tmp_path):
    """The terminal drop hands a resize back to the reader.  A resize typed
    in the gap before the write box must be laid out and the box must stay on
    the screen at the new size, so the y that follows answers it."""
    cols, rows = 100, 34
    with Held(tmp_path, APPROVE_WRITE_REPLY, "write", policy="ask") as h:
        h.start_run("write it")
        fcntl.ioctl(h.s.master_fd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", rows, cols, 0, 0))
        h.s.screen.resize(rows, cols)
        h.release()
        h.question("diff-box")
        h.shows("Write w.txt")
        assert len(h.s.display()) == rows
        assert not h.made("w.txt")
        h.press("y")
        h.wait(lambda: h.made("w.txt"), "the allowed write to land")


# @scenario SC-question-typeahead-22
def test_scrolling_the_write_box_says_the_ignored_keys_once(tmp_path):
    """The box paints again at every PgDn; the line about the keys typed
    before it is said when it was shown, not at each paint."""
    with Held(tmp_path, APPROVE_WRITE_REPLY, "write", policy="ask") as h:
        h.start_run("write it")
        h.type_ahead("qq")
        h.release()
        h.question("diff-box")
        h.press(PGDN)
        h.press(PGDN)
        h.press("y")
        h.wait(lambda: h.made("w.txt"), "the allowed write to land")
        assert h.ui().count("ask diff-box dropped=") == 1, h.ui()
        assert h.screen().count(IGNORED_KEYS[0]) <= 1, h.screen()


# @scenario SC-question-typeahead-23
def test_a_held_editor_still_ends_on_sigterm(tmp_path):
    """A test hold must not make the editor deaf to SIGTERM: held just before
    the review, the editor ends on the signal within the usual bound."""
    with Held(tmp_path, RUN_REPLY, "review") as h:
        h.start_run("make it")
        h.s.proc.send_signal(signal.SIGTERM)
        assert h.s.ended_within(END_WAIT_S), \
            "the held editor should end on SIGTERM"


# MX5 (a key the mouse poll read ahead and gave back, taken by the drop) has
# no row here.  The poll runs only inside a click or drag loop of the editor,
# and the editor is stopped inside the agent's run at every hold, where no
# such loop is active; a button held across the run's end cannot be arranged
# headlessly without a second reader of the terminal.  The drop takes that
# slot first (e_t_take_unread_key) and the rows above cover the rest of it.

# ---- the message boxes the user opens with a key (terminal-only) -----------
#
# No fifo holds the editor here: a box opens from a key, not from a callback,
# and one write to the terminal is delivered whole, so the key that opens the
# box and the key that would answer it are queued before the box by order of
# events.  Under X11 and Wayland they are separate events with nothing to
# hold the editor between them and the box's first read, so these rows have no
# twins there (said in the twins' own docstrings).

CTRL_W = "\x17"                 # Close the active window
ESC = "\033"
SEED_TEXT = "alpha\n"
OLD_TEXT = "old text\n"
SAVE_ASK = "NOT saved"          # the close question's first line
OVERWRITE_ASK = "already exists. Overwrite it?"
ABOUT_LINE = "Created by Fred Kruse"
CONFIRM_ASK = "Replace this occurrence"


def _edited(tmp_path):
    """A session whose file has one unsaved edit."""
    s = EditSession(str(tmp_path), SEED_TEXT)
    s.send(["X"])
    return s


def _blob(s):
    return "\n".join(s.display())


def _next_dump(s, pred, what, timeout=WAIT_S):
    """The first dump from now on that @pred accepts, with the screen of its
    moment.  A key a question dropped was never counted, so the session's own
    count follows the dump."""
    def ev():
        s._pump([s.master_fd, s.dump_fd], 0.05, {"pty": 0, "dump": 0})
        while s.reader.dumps:
            d = s.reader.dumps.pop(0)
            if pred(d):
                s.last = d
                s.keys_sent = d.seq
                s._drain_screen_now()
                return d
        return None
    return wait_for(ev, what, timeout)


def _box_waiting_or_answered(d):
    """A box that ignored what was typed ahead of it, or the editor back at
    its own loop because a key typed ahead answered the box."""
    return (d.ctx == "message" and d.dropped > 0) or d.ctx not in ("wait", "message")


def _label_cell(s, label):
    """(col, row) of the first cell of @label on the screen, 0-based."""
    for row, text in enumerate(s.display()):
        col = text.find(label)
        if col >= 0:
            return col, row
    raise AssertionError("%r not on screen:\n%s" % (label, _blob(s)))


def _click_reports(col, row):
    """The press and release of button 1 on the 0-based cell, as one write
    delivers them."""
    return "\033[<0;%d;%dM\033[<0;%d;%dm" % (col + 1, row + 1, col + 1, row + 1)


# @scenario SC-question-typeahead-24
def test_a_key_typed_before_the_close_question_does_not_answer_it(tmp_path):
    """R9: Ctrl-W and the n that would give the edits up, written together.
    The n is queued before the question is shown, so the box still waits with
    the edit in the window and nothing written; the n typed after it does
    close the window."""
    with _edited(tmp_path) as s:
        os.write(s.master_fd, (CTRL_W + "n").encode())
        d = _next_dump(s, _box_waiting_or_answered,
                       "the close question, or what the typed n did")
        assert d.ctx == "message", \
            "the n typed ahead answered the close question:\n%s" % _blob(s)
        assert d.dropped == 1, d.raw
        assert s.disk() == SEED_TEXT.encode(), "the typed n gave the edits up"
        assert "Xalpha" in _blob(s), "the edit is not in the window:\n%s" % _blob(s)
        os.write(s.master_fd, b"n")
        _next_dump(s, lambda d: d.ctx != "message", "the question to close")
        assert SAVE_ASK not in _blob(s), "the question is still up:\n%s" % _blob(s)
        assert s.disk() == SEED_TEXT.encode()


# @scenario SC-question-typeahead-25
def test_a_click_typed_before_the_close_question_presses_no_button(tmp_path):
    """R8: the click on Yes, written together with the Ctrl-W that opens the
    question, is queued before the box is on the screen: it presses no button,
    the box waits and the file is not saved.  The cell is the one the same box
    showed when it was opened once before, and Cancel took away again."""
    with _edited(tmp_path) as s:
        os.write(s.master_fd, CTRL_W.encode())
        _next_dump(s, lambda d: d.ctx == "message", "the close question")
        col, row = _label_cell(s, "Yes")
        os.write(s.master_fd, ESC.encode())
        _next_dump(s, lambda d: d.ctx != "message", "the question to close")
        os.write(s.master_fd, (CTRL_W + _click_reports(col, row)).encode())
        d = _next_dump(s, _box_waiting_or_answered,
                       "the close question, or what the click did")
        assert d.ctx == "message", \
            "the click typed ahead pressed a button of the question:\n%s" % _blob(s)
        assert d.dropped == 1, d.raw
        assert s.disk() == SEED_TEXT.encode(), "the click saved the file"
        assert "Xalpha" in _blob(s), "the edit is not in the window:\n%s" % _blob(s)


# @scenario SC-question-typeahead-26
def test_a_y_typed_before_the_overwrite_question_does_not_overwrite(tmp_path):
    """R9b (the overwrite box): Save As over a file that exists asks first.
    The y written together with the key that submits the name is queued
    before the question is shown, so the other file keeps its text and the
    question waits; the y typed after it does replace the file."""
    work = str(tmp_path)
    target = os.path.join(work, "x.c")
    with open(target, "w") as fh:
        fh.write(OLD_TEXT)
    with EditSession(work, SEED_TEXT) as s:
        os.write(s.master_fd, (alt("f") + "a").encode())     # File > Save As
        _next_dump(s, lambda d: d.ctx == "filemanager", "the Save As manager")
        os.write(s.master_fd, ("x.c" + alt("s") + "y").encode())
        d = _next_dump(s, _box_waiting_or_answered,
                       "the overwrite question, or what the typed y did")
        assert d.ctx == "message", \
            "the y typed ahead answered the overwrite question:\n%s" % _blob(s)
        assert OVERWRITE_ASK in _blob(s), _blob(s)
        assert d.dropped == 1, d.raw
        with open(target) as fh:
            assert fh.read() == OLD_TEXT, "the typed y replaced the file"
        os.write(s.master_fd, b"y")
        _next_dump(s, lambda d: d.ctx != "message", "the question to close")
        with open(target) as fh:
            assert fh.read() == SEED_TEXT, "the y after the question did not save"


# @scenario SC-question-typeahead-27
def test_a_key_typed_before_an_information_box_does_not_dismiss_it(tmp_path):
    """R8b (a box that only says something): the Help dropdown keeps the
    letter typed ahead of it, because a menu is a place to move, and the
    About box it opens drops the key typed ahead of the box: the box is still
    there, and the key pressed after it takes the box away."""
    with _plain(tmp_path) as s:
        os.write(s.master_fd, (alt("#") + "a" + "z").encode())
        d = _next_dump(s, _box_waiting_or_answered,
                       "the About box, or what the typed z did")
        assert d.ctx == "message", \
            "the z typed ahead took the About box away:\n%s" % _blob(s)
        assert ABOUT_LINE in _blob(s), _blob(s)
        assert d.dropped == 1, d.raw
        os.write(s.master_fd, b"q")
        _next_dump(s, lambda d: d.ctx != "message", "the box to go away")
        assert ABOUT_LINE not in _blob(s), _blob(s)


NO_PROJECT = "No project open"     # the error box of Window > Project
F9 ="\033[20~"                     # Make: compile and link
RUN_IT = alt("r")                   # Run it, the folder's command question
FAKE_COMPILER = "#!/bin/sh\ntouch ran-fake\nexit 1\n"
FOLDER_COMPILER = "[Language/C]\nCompiler : ./fake-cc\n"


# @scenario SC-question-typeahead-28
def test_a_row_of_replace_confirmations_is_answered_by_one_write(tmp_path):
    """"Replace this occurrence?" is asked once per hit and the user
    answers a row of them on purpose, so each box keeps what was typed
    before it.  "Y Y Y" written at once answers the three of a 3-hit
    replace; every other message box ignores keys typed before it."""
    with EditSession(str(tmp_path), "a\na\na\n") as s:
        d = s.send([alt("s"), "r", "a", alt("n"), "b", alt("a")])
        assert d.ctx == "message" and CONFIRM_ASK in _blob(s), _blob(s)
        os.write(s.master_fd, b"YYY")
        s.keys_sent += 3
        d = s._wait_for(s.keys_sent, WAIT_S)
        assert d.ctx == "message" and d.dropped == 0, d.raw
        assert CONFIRM_ASK not in _blob(s), "a confirmation ignored a typed Y:\n" + _blob(s)
        s.send([ENTER, F2])
        assert s.disk() == b"b\nb\nb\n", s.disk()


# @scenario SC-question-typeahead-29
def test_a_key_typed_before_the_change_all_report_does_not_dismiss_it(tmp_path):
    """The report that ends Change All is a message box like any other: the
    Enter written with the Alt-A that starts the command is queued before
    the box, so the box stays up, says nothing, and the Enter pressed after
    it takes it away."""
    with EditSession(str(tmp_path), "a\na\n") as s:
        s.send([alt("s"), "r", "a", alt("n"), "b", alt("p")])
        os.write(s.master_fd, (alt("a") + ENTER).encode())
        d = _next_dump(s, _box_waiting_or_answered,
                       "the report, or what the typed Enter did")
        assert d.ctx == "message", \
            "the Enter typed ahead took the report away:\n%s" % _blob(s)
        assert d.dropped == 1, d.raw
        assert "Replaced 2" in _blob(s), _blob(s)
        os.write(s.master_fd, ENTER.encode())
        _next_dump(s, lambda d: d.ctx != "message", "the report to go away")
        assert "Replaced 2" not in _blob(s), _blob(s)


# @scenario SC-question-typeahead-30
def test_a_key_typed_before_an_error_box_does_not_dismiss_it(tmp_path):
    """The error box ("No project open" for Window > Project with none): the
    Esc written with the keys that raise the box is queued before it, so the
    user has the box to read; the box took the Esc away unread before."""
    with _plain(tmp_path) as s:
        os.write(s.master_fd, (alt("w") + "p" + ESC).encode())
        d = _next_dump(s, _box_waiting_or_answered,
                       "the error box, or what the typed Esc did")
        assert d.ctx == "message" and NO_PROJECT in _blob(s), \
            "the Esc typed ahead took the error box away:\n%s" % _blob(s)
        assert d.dropped == 1, d.raw
        os.write(s.master_fd, ESC.encode())
        _next_dump(s, lambda d: d.ctx != "message", "the box to go away")
        assert NO_PROJECT not in _blob(s), _blob(s)


# @scenario SC-question-typeahead-31
def test_a_key_typed_before_the_folder_command_question_does_not_run_it(tmp_path):
    """A folder that names its own compiler asks before F9 runs it.  The
    Alt-R (Run it) written with F9 is queued before the question: the
    question waits and the folder's program does not run."""
    fake = tmp_path / "fake-cc"
    fake.write_text(FAKE_COMPILER)
    fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
    (tmp_path / "xwperc").write_text(FOLDER_COMPILER)
    with EditSession(str(tmp_path), ORIG) as s:
        os.write(s.master_fd, (F9 + RUN_IT).encode())
        d = _next_dump(s, _box_waiting_or_answered,
                       "the folder's command question, or what the typed Alt-R did")
        assert d.ctx == "message" and "Command from" in _blob(s), \
            "the Alt-R typed ahead answered the question:\n%s" % _blob(s)
        assert d.dropped == 1, d.raw
        assert not (tmp_path / "ran-fake").exists(), "the folder's compiler ran"


# ---- guards: places to type or move keep their type-ahead ------------------

def _plain(tmp_path):
    return EditSession(str(tmp_path), "one\n")


# @scenario SC-question-typeahead-10
def test_a_menu_and_its_letter_in_one_write_still_open_the_dialog(tmp_path):
    """G3: Alt-O then e in one write opens Editor Options."""
    with _plain(tmp_path) as s:
        os.write(s.master_fd, (alt("o") + "e").encode())
        s.keys_sent = 2
        assert s._wait_for(2, WAIT_S).ctx == "dialog:Editor-Options"


# @scenario SC-question-typeahead-11
def test_ctrl_k_and_its_letter_in_one_write_still_mark_the_block(tmp_path):
    """G4: Ctrl-K b in one write sets the block begin."""
    with _plain(tmp_path) as s:
        before = s.last.win.mb
        os.write(s.master_fd, (ctrl("k") + "b").encode())
        s.keys_sent = 2
        d = s._wait_for(2, WAIT_S)
        assert d.ctx == "edit" and d.win.mb != before, \
            "the block begin was not set: %s -> %s" % (before, d.win.mb)


# @scenario SC-question-typeahead-12
def test_a_dialog_field_typed_with_the_key_that_opens_it(tmp_path):
    """G5: F4, the text and Enter in one write reach the Find field."""
    with _plain(tmp_path) as s:
        os.write(s.master_fd, (F4 + "abc" + "\r").encode())
        s.keys_sent = 5
        d = s._wait_for(5, WAIT_S)
        assert d.find is not None and d.find[0] == b"abc", d.find
