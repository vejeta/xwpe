"""The terminal's private modes: set when the editor owns the terminal, given
back whenever it does not.

The editor asks the terminal for xterm's modifyOtherKeys and kitty's keyboard
protocol (so Ctrl-Shift-letter and Esc arrive as unambiguous reports), beside
the mouse reports and bracketed paste it always asked for.  A program the
editor hands the terminal to, the shell after a quit, and the user's prompt
after a crash or a stop must see none of them, or a key pressed there prints
a report instead of acting.  These rows read the bytes the editor writes to
its own pty (not through a screen emulator, which would hide them) and check
the order: on after the alternate screen is entered, off before it is left.
The kitty entry is popped on the editor's own screen: the alternate and the
main screen keep separate stacks, so a pop after leaving would remove the
shell's entry.

Run: WPE_BIN=$PWD/wpe tests/.venv/bin/python -m pytest -q tests/test_private_modes.py
"""
import os
import shutil
import signal
import time

import pytest

from wpe_driver import WpeSession, ALT, wait_screen
from test_file_shell import SHELL_MARK, SHELL_ENTRY, write_marker_shell
from test_terminal_owner import release_string

# What the editor writes, byte for byte (unixkeys.h).
KEYS_ON = b"\x1b[>4;2m\x1b[>5u"
KEYS_OFF = b"\x1b[<u\x1b[>4m"
KITTY_QUERY = b"\x1b[?u"
MOK_QUERY = b"\x1b[?4m"
PASTE_OFF = b"\x1b[?2004l"
MOUSE_OFF = b"\x1b[?1002l\x1b[?1006l\x1b[?1000l"
# What the editor's own exit writes first, and the signal handlers' one
# constant string (the same modes in a fixed order).
MODES_OFF = KEYS_OFF + PASTE_OFF
CRASH_OFF = KEYS_OFF + PASTE_OFF + MOUSE_OFF
ALT_SCREEN_ON = b"\x1b[?1049h"
ALT_SCREEN_OFF = b"\x1b[?1049l"

SEED = "AAAAA\nBBBBB\n"
EXIT_WAIT_S = 8
SETTLE_S = 1.0


def _end_of(raw, marker):
    return raw.rfind(marker) + len(marker)


def test_the_editor_asks_for_the_protocols_once_after_the_screen_switch(tmp_path):
    """On start: the alternate screen is entered, then the protocols are
    switched on once, and the terminal is asked which it speaks."""
    with WpeSession(str(tmp_path), SEED) as w:
        raw = bytes(w.raw)
    screen = raw.find(ALT_SCREEN_ON)
    assert screen >= 0, "the editor never entered the alternate screen"
    assert raw.count(KEYS_ON) == 1, \
        "the protocols are asked for %d times, not once:\n%r" \
        % (raw.count(KEYS_ON), raw[:300])
    assert raw.find(KEYS_ON) > screen, \
        "the protocols were switched on before the alternate screen"
    assert KITTY_QUERY in raw and MOK_QUERY in raw, \
        "the terminal was not asked which protocol it speaks"
    assert KEYS_OFF not in raw, "the protocols were switched off at start"


def test_the_protocols_are_off_before_a_shell_and_on_after_it(tmp_path):
    """File > Shell: the kitty entry is popped and modifyOtherKeys reset
    before the screen is left, nothing switches them on until the shell has
    run, and they are on again when the editor has the terminal back."""
    workdir = str(tmp_path)
    shell = write_marker_shell(workdir)
    with WpeSession(workdir, SEED, env_extra={"SHELL": shell}) as w:
        del w.raw[:]
        w.menu(ALT.FILE, SHELL_ENTRY, delay=1.0)
        raw = bytes(w.raw)
        assert w.alive(), "wpe died around File > Shell"
    marker = raw.find(SHELL_MARK)
    assert marker >= 0, "the shell never ran"
    before, after = raw[:marker], raw[marker:]
    assert KEYS_OFF in before, \
        "the protocols were not switched off before the shell:\n%r" % before
    assert before.find(KEYS_OFF) < before.find(ALT_SCREEN_OFF), \
        "the kitty entry was popped after the screen was left:\n%r" % before
    assert KEYS_ON not in before, \
        "the protocols were switched on again before the shell started"
    assert KEYS_ON in after and after.find(ALT_SCREEN_ON) < after.find(KEYS_ON), \
        "the protocols were not switched on after the shell, after the " \
        "screen was entered:\n%r" % after


def test_the_protocols_are_off_when_the_editor_quits(tmp_path):
    """Alt-X: the off sequences are the last modes written before the
    alternate screen is left, and nothing switches a mode on after them."""
    with WpeSession(str(tmp_path), SEED) as w:
        del w.raw[:]
        w.key("\x1bx", delay=1.0)
        w._drain(SETTLE_S)
        raw = bytes(w.raw)
    assert MODES_OFF in raw, \
        "the quit did not give the modes back:\n%r" % raw[-200:]
    assert raw.find(MODES_OFF) < raw.find(ALT_SCREEN_OFF), \
        "the modes were given back after the screen was left:\n%r" % raw[-200:]
    assert KEYS_ON not in raw[raw.find(MODES_OFF):], \
        "a mode was switched on again while the editor quit"


def test_sigterm_gives_the_modes_back(tmp_path):
    """The editor's end by SIGTERM leaves nothing on in the user's shell."""
    with WpeSession(str(tmp_path), SEED) as w:
        del w.raw[:]
        os.kill(w.proc.pid, signal.SIGTERM)        # the editor's own pid
        deadline = time.time() + EXIT_WAIT_S
        while time.time() < deadline and w.alive():
            w._drain(0.25)
        w._drain(0.3)
        raw = bytes(w.raw)
    assert MODES_OFF in raw, \
        "SIGTERM did not give the modes back:\n%r" % raw[-200:]
    assert raw.find(MODES_OFF) < raw.find(ALT_SCREEN_OFF), \
        "the modes were given back after the screen was left"
    assert KEYS_ON not in raw[raw.find(MODES_OFF):]


@pytest.mark.skipif(not shutil.which("tput"), reason="no tput")
def test_a_crash_gives_the_modes_back_as_the_last_thing_written(tmp_path):
    """XWPE_TEST_CRASH=1 raises SIGSEGV at the first wait for a key: the
    handler writes the modes off, in the order the exit writes them, and then
    the pointer and the terminal's own strings that leave the alternate
    screen: the release string is the last thing written."""
    with WpeSession(str(tmp_path), SEED, env_extra={"XWPE_TEST_CRASH": "1"}) as w:
        w._drain(2.0)
        raw = bytes(w.raw)
        assert not w.alive(), "the editor did not crash"
    assert raw.rfind(CRASH_OFF) > raw.rfind(KEYS_ON), \
        "the crash left the protocols on:\n%r" % raw[-200:]
    assert raw.endswith(release_string()), \
        "the release string is not the last thing written:\n%r" % raw[-160:]


def test_a_stop_and_a_continue_give_the_modes_back_and_take_them_again(tmp_path):
    """SIGTSTP: the handler writes the modes off, ncurses' handler stops the
    process (a pty session leader is an orphaned group, where the stop is
    ignored, so the handler returns at once), and the modes are written on
    when it returns."""
    with WpeSession(str(tmp_path), SEED) as w:
        del w.raw[:]
        os.kill(w.proc.pid, signal.SIGTSTP)
        w._drain(1.5)
        raw = bytes(w.raw)
        assert w.alive(), "the editor died on SIGTSTP"
    off = raw.find(MODES_OFF)
    assert off >= 0, "SIGTSTP did not give the modes back:\n%r" % raw[-200:]
    assert raw.find(KEYS_ON, off) > off, \
        "the modes were not taken again after the stop:\n%r" % raw[-200:]


PROG = ('#include <stdio.h>\n'
        'int main(void){ printf("USERSCR%d\\n", 6 * 7); return 0; }\n')
F9 = "\x1b[20~"
CTRL_F9 = "\x1b[20;5~"
ALT_F5 = "\x1b[15;3~"
BUILD_S = 20


def _drain_until(w, predicate, seconds):
    deadline = time.time() + seconds
    while time.time() < deadline:
        w._drain(0.25)
        if predicate():
            return True
    return False


def _handed_over_and_back(raw):
    """Each hand-over in @raw: off before the screen is left, on after it is
    entered again, and the protocols on at the end."""
    off = raw.find(KEYS_OFF)
    assert off >= 0, "the protocols were not switched off:\n%r" % raw[:300]
    assert off < raw.find(ALT_SCREEN_OFF, off), \
        "the kitty entry was popped after the screen was left:\n%r" % raw[:300]
    back = raw.find(ALT_SCREEN_ON, off)
    assert back >= 0 and raw.find(KEYS_ON, back) > back, \
        "the protocols were not switched on after the screen came back"
    assert raw.rfind(KEYS_ON) > raw.rfind(KEYS_OFF), \
        "the protocols were left off after the hand-over"


@pytest.mark.skipif(not shutil.which("gcc"), reason="no gcc")
def test_a_build_hands_the_terminal_over_without_the_protocols(tmp_path):
    """F9: the compiler runs with the terminal handed over (the compile and
    the link each leave the editor's screen), so the protocols are off
    while it runs and on again afterwards."""
    with WpeSession(str(tmp_path), PROG, filename="t.c", wait=1.5) as w:
        del w.raw[:]
        w.key(F9, delay=1.0)
        _drain_until(w, lambda: os.path.exists(str(tmp_path / "t")), BUILD_S)
        w._drain(1.0)
        raw = bytes(w.raw)
    _handed_over_and_back(raw)


@pytest.mark.skipif(not shutil.which("gcc"), reason="no gcc")
def test_the_user_screen_hands_the_terminal_over_without_the_protocols(tmp_path):
    """Alt-F5 after a Run: the User Screen shows the program's output on
    the shell's screen with the protocols off, and a key brings the editor
    back with them on."""
    with WpeSession(str(tmp_path), PROG, filename="t.c", wait=1.5) as w:
        w.key(CTRL_F9, delay=1.0)
        assert wait_screen(w, "USERSCR42", BUILD_S), "Run never showed its output"
        w.key("\x1b", delay=0.5)
        del w.raw[:]
        w.key(ALT_F5, delay=1.5)
        shown = bytes(w.raw)
        assert b"press any key" in shown, "the User Screen did not appear"
        assert KEYS_ON not in shown, "the protocols were on on the User Screen"
        w.key("x", delay=1.5)
        raw = bytes(w.raw)
        assert w.alive(), "wpe died leaving the User Screen"
    _handed_over_and_back(raw)
