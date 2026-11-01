"""The terminal editor keeps its keyboard modes after it runs a tool.

The editor reads every key itself: the terminal must not turn Ctrl-C,
Ctrl-Z or Ctrl-\\ into signals, nor Ctrl-S and Ctrl-Q into flow control.
Those modes are clear at start.  A compile (F9), a run (Ctrl-F9) or a
debugger start (Ctrl-G R) hands the terminal to a tool and takes it back,
and it used to take back the wrong modes: after one of them Ctrl-\\ ended
the editor with no save, Ctrl-Z stopped it, and Ctrl-S froze its output.

The modes are read with termios on the master side of the editor's pty,
which shows the modes of the terminal the editor runs on.  The keys are
then typed, and what each does is read from the screen.  The editor is the
leader of its own session here, so a Ctrl-Z that would stop it is thrown
away by the kernel rather than stopping it; the row for Ctrl-Z therefore
looks at the editing it should do (delete to the end of the line).
"""
import os
import signal
import termios
import time

import pytest

from wpe_driver import WpeSession, WAIT_SCALE
from test_debugger_integration import (C_PROG, CTRL_G, _need,
                                       _start_c_session, _wait_for)

F9 = "\033[20~"                 # Make: compile and link
CTRL_F9 = "\033[20;5~"          # Run: build, then run the program
RIGHT = "\033[C"
CTRL_BACKSLASH = "\x1c"
CTRL_S = "\x13"
CTRL_Z = "\x1a"
BUILD_TIMEOUT_S = 25            # a build, before scaling
SETTLE_S = 1.2                  # after a stop or the end of a debug session
# The Messages line that ends a build, and the one that ends a run.
BUILD_DONE = ("errors, ", "Return-Code")

# The input and local flags the editor must not leave on: signal keys
# (ISIG), the extended input keys (IEXTEN: Ctrl-V, Ctrl-O) and flow control.
SIGNAL_LFLAGS = termios.ISIG | termios.IEXTEN
FLOW_IFLAGS = termios.IXON

PROG = "int main(void)\n{\n  return 0;\n}\n"
LINE = "int main(void)"        # first line of PROG; Ctrl-Z cuts it short
KEPT = "int "                   # what stays after Ctrl-Z at column 4


def _tty_flags(w):
    """(c_iflag, c_lflag) of the editor's terminal."""
    mode = termios.tcgetattr(w.master_fd)
    return mode[0], mode[3]


def _assert_modes_clear(w, when):
    iflag, lflag = _tty_flags(w)
    assert not lflag & SIGNAL_LFLAGS, \
        "%s: the terminal sends signals for keys (c_lflag=%#o)" % (when, lflag)
    assert not iflag & FLOW_IFLAGS, \
        "%s: Ctrl-S and Ctrl-Q are flow control (c_iflag=%#o)" % (when, iflag)


def _text(w):
    return "\n".join(w.display())


def _build(w, key, product):
    """Press the build or run key and wait until the tool has run and the
    editor has taken the terminal back."""
    w.key(key, delay=0.5)
    deadline = time.time() + BUILD_TIMEOUT_S * WAIT_SCALE
    while time.time() < deadline and not os.path.exists(product):
        w._drain(0.25)
    assert os.path.exists(product), "the build never made %s" % product
    assert _wait_for(w, lambda: any(m in _text(w) for m in BUILD_DONE),
                     BUILD_TIMEOUT_S), \
        "the editor never showed the end of the tool:\n%s" % _text(w)


def _session(tmp_path, prog=PROG):
    return WpeSession(str(tmp_path), prog, filename="p.c", wait=1.5)


def _after_f9(w, tmp_path):
    _build(w, F9, str(tmp_path / "p.e"))


# @scenario SC-compile-03
def test_modes_are_clear_at_start(tmp_path):
    """Control row: a fresh editor has the signal and flow keys off."""
    with _session(tmp_path) as w:
        _assert_modes_clear(w, "at start")


# @scenario SC-compile-08
def test_modes_stay_clear_after_f9(tmp_path):
    """After a compile the keys are still keys."""
    _need("gcc")
    with _session(tmp_path) as w:
        _after_f9(w, tmp_path)
        _assert_modes_clear(w, "after F9")


# @scenario SC-compile-03
def test_modes_stay_clear_after_ctrl_f9(tmp_path):
    """After a run the keys are still keys."""
    _need("gcc")
    with _session(tmp_path) as w:
        _build(w, CTRL_F9, str(tmp_path / "p.e"))
        _assert_modes_clear(w, "after Ctrl-F9")


# @scenario SC-debug-17
def test_modes_stay_clear_after_a_debug_session(tmp_path):
    """After Ctrl-G R has built and started gdb, and after the session
    has ended, the keys are still keys."""
    _need("gcc", "gdb")
    with _session(tmp_path, C_PROG) as w:
        _start_c_session(w)
        _assert_modes_clear(w, "while gdb is stopped")
        w.key(CTRL_G, "q", delay=1.0)
        w._drain(SETTLE_S * WAIT_SCALE)
        _assert_modes_clear(w, "after the debug session")


# @scenario SC-compile-08
def test_ctrl_backslash_does_not_end_the_editor_after_f9(tmp_path):
    """Ctrl-\\ is a key (back tab).  With the signal mode on it ended the
    editor with a core dump and no save."""
    _need("gcc")
    with _session(tmp_path) as w:
        _after_f9(w, tmp_path)
        w.key(CTRL_BACKSLASH, delay=0.8)
        assert w.alive(), "Ctrl-\\ ended the editor (exit %r)" % w.exit_code()
        assert LINE in _text(w), "the editor does not show the file:\n%s" \
            % _text(w)


def _ctrl_z_cuts_the_line(w):
    """Ctrl-Z at column 4 of the first line deletes to the end of the
    line.  True when the line was cut."""
    for _ in range(len(KEPT)):
        w.key(RIGHT, delay=0.15)
    w.key(CTRL_Z, delay=0.8)
    return LINE not in _text(w) and KEPT.rstrip() in _text(w)


# @scenario SC-compile-03
def test_ctrl_z_edits_before_f9(tmp_path):
    """Control row for the next one: before any tool ran, Ctrl-Z deletes
    to the end of the line, so the probe measures what it claims to."""
    with _session(tmp_path) as w:
        assert _ctrl_z_cuts_the_line(w), \
            "Ctrl-Z did not cut the line:\n%s" % _text(w)


# @scenario SC-compile-08
def test_ctrl_z_edits_after_f9(tmp_path):
    """After a compile Ctrl-Z still deletes to the end of the line.  With
    the signal mode on the terminal took the key as a stop request and the
    editor never saw it."""
    _need("gcc")
    with _session(tmp_path) as w:
        _after_f9(w, tmp_path)
        assert _ctrl_z_cuts_the_line(w), \
            "Ctrl-Z did not cut the line after F9:\n%s" % _text(w)
        assert w.alive()


# @scenario SC-compile-08
def test_ctrl_s_does_not_freeze_the_screen_after_f9(tmp_path):
    """Ctrl-S is an ordinary key.  With flow control on it stopped the
    terminal's output, so what the user typed next never showed."""
    _need("gcc")
    with _session(tmp_path) as w:
        _after_f9(w, tmp_path)
        w.key(CTRL_S, delay=0.5)
        w.key("QQ", delay=0.8)
        assert _wait_for(w, lambda: "QQint main" in _text(w), 5), \
            "the screen stopped updating after Ctrl-S:\n%s" % _text(w)


ALT_F5 = "\033\033[15~"         # Alt-F5: the User Screen
USER_SCREEN_PROMPT = "press any key to return"
MARK = "Z"                      # typed to learn the editor handled a signal
RESUMED = b"\x1b[?1049h"        # ncurses' repaint on resume: alternate screen


# @scenario SC-compile-03
def test_modes_are_kept_when_the_editor_is_stopped_and_continued(tmp_path):
    """kill -TSTP: ncurses' own handler puts back the modes it saved when the
    editor resumes.  They must be the editor's own, not the ones cbreak()
    made, or Ctrl-\\ and Ctrl-Z become signals again.  (The editor leads its
    session here, so the stop itself is thrown away by the kernel; the
    handler runs and restores the modes all the same.)  A key typed
    afterwards and seen on the screen shows the signal was handled."""
    with _session(tmp_path) as w:
        seen = len(w.raw)
        os.kill(w.proc.pid, signal.SIGTSTP)   # no SIGCONT: it would cancel it
        # The handler throws away typeahead when it restores the modes; its
        # repaint enters the alternate screen again only after it has.
        assert _wait_for(w, lambda: RESUMED in bytes(w.raw[seen:]), 5), \
            "the editor did not repaint after the stop signal"
        w.key(MARK, delay=0.3)
        assert _wait_for(w, lambda: MARK + LINE in _text(w), 5), \
            "the editor did not take a key after the signals:\n%s" % _text(w)
        _assert_modes_clear(w, "after stop and continue")


def _open_user_screen(w, tmp_path):
    _build(w, CTRL_F9, str(tmp_path / "p.e"))
    w.key(ALT_F5, delay=0.8)
    assert USER_SCREEN_PROMPT in _text(w), \
        "Alt-F5 did not show the User Screen:\n%s" % _text(w)


# @scenario SC-compile-03
@pytest.mark.parametrize("key,name", [(CTRL_BACKSLASH, "Ctrl-\\"),
                                      (CTRL_Z, "Ctrl-Z")])
def test_a_signal_key_returns_from_the_user_screen(tmp_path, key, name):
    """On the User Screen any key goes back to the editor.  With the signal
    mode on, Ctrl-\\ ended the editor with no save, and Ctrl-Z was taken by
    the terminal and never read, so the screen stayed."""
    _need("gcc")
    with _session(tmp_path) as w:
        _open_user_screen(w, tmp_path)
        w.key(key, delay=1.0)
        assert w.alive(), "%s on the User Screen ended the editor (exit %r)" \
            % (name, w.exit_code())
        assert _wait_for(w, lambda: LINE in _text(w)
                         and USER_SCREEN_PROMPT not in _text(w), 5), \
            "%s did not return to the editor:\n%s" % (name, _text(w))
        _assert_modes_clear(w, "after the User Screen")
