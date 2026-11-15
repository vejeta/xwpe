"""A window closed to the user's changes says why, from one owner.

we_closed.c answers "may this window be changed" and spells the one line said
for each reason.  This file holds the rows of its first pass: the owner's
assistant line is counted by the state dump like every other refusal, and the
windows that are closed by their nature carry the read-only flag whoever runs
the editor.

Every row drives the editor through ``EditSession`` (tests/edit_model/oracle.py),
so each key answers with a dump: ``says`` counts the status lines said since
the dump before, ``ro`` is the window's lock as the padlock shows it.
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

from build_config import requires  # noqa: E402
from edit_model.oracle import ENTER, ESC, EditSession, alt  # noqa: E402
from host_tools import needs  # noqa: E402

READ_ONLY_LINE = "Read-only window: the text is unchanged"
ASSISTANT_LINE = "The assistant is working on this window (Esc cancels)"

# The reply must not arrive while the rows run.
MOCK_DELAY_MS = 30000


def _status_row(s):
    return s.display()[-1]


def _window_named(d, name):
    for w in d.windows.values():
        if w.name.endswith(name):
            return w
    raise AssertionError("no window %r in the dump:\n%s" % (name, d.raw))


def _assistant_env():
    return {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": "int a = 1;\n",
        "XWPE_AI_MOCK_DELAY_MS": str(MOCK_DELAY_MS),
    }


# @scenario SC-window-closed-11
@requires("enable-ai")
def test_a_typed_letter_in_a_window_waiting_for_the_assistant_says_so_once(tmp_path):
    """While an Edit waits for its reply the window's text must stay as sent:
    a typed letter changes nothing and the status row says why, counted as one
    line said (the old refusal painted the bar without being counted)."""
    seed = "int a = 1;\nint b = 2;\n"
    with EditSession(str(tmp_path), seed, filename="t.c",
                     env_extra=_assistant_env()) as s:
        s.send([alt("g"), "e"])
        s.send(list("do it"))
        s.send([ENTER])
        before = _window_named(s.last, "t.c").text_rows()
        d = s.send(["Q"])
        assert d.says == 1, "a typed letter said %d lines:\n%s" % (d.says, d.raw)
        assert ASSISTANT_LINE in _status_row(s), s.display()
        assert _window_named(d, "t.c").text_rows() == before, d.raw
        assert d.ctx == "edit", d.raw


# @scenario SC-window-closed-11
@requires("enable-ai")
def test_each_refused_letter_in_a_waiting_window_says_its_line(tmp_path):
    """One line per key, not one per wait: the second refused letter says
    its line again, so the latch that keeps a command to one line is
    cleared when the next key is read."""
    seed = "int a = 1;\nint b = 2;\n"
    with EditSession(str(tmp_path), seed, filename="t.c",
                     env_extra=_assistant_env()) as s:
        s.send([alt("g"), "e"])
        s.send(list("do it"))
        s.send([ENTER])
        assert s.send(["Q"]).says == 1, s.last.raw
        d = s.send(["R"])
        assert d.says == 1, "the second letter said %d lines:\n%s" % (d.says, d.raw)
        assert ASSISTANT_LINE in _status_row(s), s.display()


# @scenario SC-window-closed-11
@requires("enable-ai")
def test_the_assistant_line_goes_when_the_reply_lands(tmp_path):
    """The line said for a refused letter names a wait that is over once the
    reply is in: when the review of the change is up, the status row no
    longer says the assistant is working (its Esc would now cancel the
    review, not the task)."""
    seed = "int a = 1;\nint b = 2;\n"
    env = _assistant_env()
    env["XWPE_AI_MOCK_REPLY"] = "int a = 1;\nint b = 3;\n"
    env["XWPE_AI_MOCK_DELAY_MS"] = "3000"
    with EditSession(str(tmp_path), seed, filename="t.c", env_extra=env) as s:
        s.send([alt("g"), "e"])
        s.send(list("do it"))
        s.send([ENTER])
        s.send(["Q"])
        deadline = time.monotonic() + 20
        while not any("Proposed change" in r for r in s.display()):
            assert time.monotonic() < deadline, s.display()
            time.sleep(0.2)
            s._drain_screen_now()      # the reply paints with no key sent
        assert ASSISTANT_LINE not in _status_row(s), s.display()


PASTE_BEG = ESC + "[200~"


# @scenario SC-window-closed-11
@requires("enable-ai")
def test_a_paste_after_a_refused_letter_in_a_waiting_window_says_so(tmp_path):
    """A refused letter, then a paste into the same waiting window: the paste
    is refused too and its line is on the status row while it streams.
    The paste's first character is a key of its own, so the line said for the
    letter before must not keep the paste silent.  The paste is left open: the
    reader of its end marker waits for the next key, so no dump answers it."""
    seed = "int a = 1;\nint b = 2;\n"
    with EditSession(str(tmp_path), seed, filename="t.c",
                     env_extra=_assistant_env()) as s:
        s.send([alt("g"), "e"])
        s.send(list("do it"))
        s.send([ENTER])
        before = _window_named(s.last, "t.c").text_rows()
        d = s.send(["Q"])
        assert d.says == 1, d.raw
        d = s.send([PASTE_BEG + "x", "y", "z"])
        assert ASSISTANT_LINE in _status_row(s), s.display()
        assert _window_named(d, "t.c").text_rows() == before, d.raw


# @scenario SC-window-closed-11
@requires("enable-ai")
def test_a_paste_after_a_refused_letter_says_its_own_line_once(tmp_path):
    """The paste's own line, counted: its first character says once and the
    rest of the paste says nothing.  The row above sees the letter's line
    kept on the row and cannot tell it from the paste's."""
    seed = "int a = 1;\nint b = 2;\n"
    with EditSession(str(tmp_path), seed, filename="t.c",
                     env_extra=_assistant_env()) as s:
        s.send([alt("g"), "e"])
        s.send(list("do it"))
        s.send([ENTER])
        assert s.send(["Q"]).says == 1, s.last.raw
        d = s.send([PASTE_BEG + "x"])
        assert d.says == 1, "the paste's first character said %d lines:\n%s" % (
            d.says, d.raw)
        rest = sum(s.send(["y"]).says for _ in range(50))
        assert rest == 0, "the rest of the paste said %d lines" % rest
        assert ASSISTANT_LINE in _status_row(s), s.display()


# @scenario SC-window-closed-11
def test_a_typed_letter_in_the_same_window_after_esc_changes_the_text(tmp_path):
    """The control: Esc frees the window and the letter is text again, with
    nothing said."""
    seed = "int a = 1;\nint b = 2;\n"
    with EditSession(str(tmp_path), seed, filename="t.c",
                     env_extra=_assistant_env()) as s:
        s.send([alt("g"), "e"])
        s.send(list("do it"))
        s.send([ENTER])
        s.send([ESC])
        s.send([ESC])                  # the second Esc leaves the armed chat input
        d = s.send(["Q"])
        assert d.says == 0, d.raw
        assert any(b"Q" in data for _, data in _window_named(d, "t.c").lines), d.raw


# @scenario SC-window-closed-14
@needs("man")
def test_a_man_page_window_is_read_only_by_its_nature(tmp_path):
    """The man page is closed because the editor marks it so, not because its
    temporary file happened to be written 0400 (a process that may write
    anything, such as root, would otherwise find it open)."""
    workdir = str(tmp_path)
    mandir = os.path.join(workdir, "man")
    os.makedirs(os.path.join(mandir, "man1"))
    with open(os.path.join(mandir, "man1", "wcpage.1"), "w") as fh:
        fh.write(".TH WCPAGE 1\n.SH NAME\nwcpage \\- page of a test\n"
                 ".SH DESCRIPTION\nWC-BODY-MARKER\n")
    env = {"MANPATH": mandir, "MANWIDTH": "80"}
    with EditSession(workdir, "wcpage\n", filename="t.txt", env_extra=env) as s:
        d = s.send([alt("h"), "t"], timeout=30)
        assert any("WC-BODY-MARKER" in r for r in s.display()), s.display()
        assert d.win is not None and d.win.ro == 1, d.raw
