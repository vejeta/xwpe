"""The terminal is handed back whole: what the editor writes to it when it ends,
and what it does not write while a program or the User Screen has it.

The terminal session (we_term.c) has one state: the editor's own, handed over
to a program or a prompt, or given back for good.  These rows read the bytes
the editor writes to its own pty, not a screen emulator's picture of them,
because the defects they guard are bytes that a terminal which resets itself
on exit would hide: the alternate screen entered again after it was left, the
mouse and keyboard modes switched on again, a full reset at the end.

Run: WPE_BIN=$PWD/wpe tests/.venv/bin/python -m pytest -q tests/test_terminal_owner.py
"""
import os
import shutil
import signal
import stat
import subprocess
import time

import pytest

from wpe_driver import WpeSession, ALT, wait_screen
from test_file_shell import SHELL_ENTRY

# What the editor writes, byte for byte (unixkeys.h).
KEYS_ON = b"\x1b[>4;2m\x1b[>5u"
KEYS_OFF = b"\x1b[<u\x1b[>4m"
KITTY_POP = b"\x1b[<u"
PASTE_OFF = b"\x1b[?2004l"
MOUSE_OFF = b"\x1b[?1002l\x1b[?1006l\x1b[?1000l"
POINTER_RESET = b"\x1b]22;\a"
MODES_OFF = KEYS_OFF + PASTE_OFF
CRASH_OFF = KEYS_OFF + PASTE_OFF + MOUSE_OFF
ALT_SCREEN_ON = b"\x1b[?1049h"
ALT_SCREEN_OFF = b"\x1b[?1049l"
RIS = b"\x1bc"
# Switched on again after the editor has said goodbye: all of these are
# defects when they follow the modes-off string.
SWITCHED_ON = (ALT_SCREEN_ON, b"\x1b[?1000h", b"\x1b[?1002h", b"\x1b[?1006h",
               b"\x1b[?2004h", b"\x1b[?1006;1000h", KEYS_ON, RIS)

SEED = "AAAAA\nBBBBB\n"
EXIT_WAIT_S = 8
SETTLE_S = 0.3
TERM_NAME = "xterm-256color"
PROG = ('#include <stdio.h>\n'
        'int main(void){ printf("USERSCR%d\\n", 6 * 7); return 0; }\n')
CTRL_F9 = "\x1b[20;5~"
ALT_F5 = "\x1b[15;3~"
RUN_S = 20
SLEEP_MARK = b"SLEEPMARK"
SLEEP_S = 30


def terminfo_string(cap):
    """What terminfo gives for @cap on the terminal the rows run under."""
    return subprocess.run(["tput", "-T", TERM_NAME, cap], check=True,
                          stdout=subprocess.PIPE).stdout


def release_string():
    """What a signal handler writes to give the terminal back: the modes off,
    the pointer, then the terminal's own reset strings, the alternate screen
    last."""
    return (CRASH_OFF + POINTER_RESET + terminfo_string("sgr0")
            + terminfo_string("cnorm") + terminfo_string("rmkx")
            + terminfo_string("rmcup"))


needs_tput = pytest.mark.skipif(not shutil.which("tput"), reason="no tput")
needs_gcc = pytest.mark.skipif(not shutil.which("gcc"), reason="no gcc")


def wait_for_exit(w, seconds=EXIT_WAIT_S):
    deadline = time.time() + seconds
    while time.time() < deadline and w.alive():
        w._drain(0.25)
    w._drain(SETTLE_S)


def end_the_editor(w, how):
    """Run @how (a signal, a key) and read until the editor is gone.
    Return: (every byte written since, the exit status)."""
    del w.raw[:]
    how(w)
    wait_for_exit(w)
    return bytes(w.raw), w.proc.poll()


def send_signal(sig):
    return lambda w: os.kill(w.proc.pid, sig)


def assert_given_back_whole(raw, what):
    """After the modes-off string the editor writes nothing that switches a
    mode on, enters the alternate screen again or resets the terminal, and the
    last screen switch is the one that leaves the alternate screen."""
    off = raw.find(MODES_OFF)
    assert off >= 0, "%s did not give the modes back:\n%r" % (what, raw[-200:])
    tail = raw[off + len(MODES_OFF):]
    for seq in SWITCHED_ON:
        assert seq not in tail, \
            "%s wrote %r after giving the modes back (%d bytes):\n%r" \
            % (what, seq, len(tail), tail[:300])
    assert raw.rfind(ALT_SCREEN_OFF) > raw.rfind(ALT_SCREEN_ON), \
        "%s left the shell in the alternate screen" % what


# @scenario SC-term-01
def test_sigterm_leaves_the_terminal_as_the_shell_had_it(tmp_path):
    """SIGTERM: the modes are given back and nothing is written after the
    alternate screen is left; the work is in the .ESV file; the exit is by the
    signal."""
    with WpeSession(str(tmp_path), SEED) as w:
        w.key("Z", delay=0.5)
        raw, code = end_the_editor(w, send_signal(signal.SIGTERM))
        esv = w.path + ".ESV"
    assert code == -signal.SIGTERM, "exit status %r" % code
    assert_given_back_whole(raw, "SIGTERM")
    assert os.path.exists(esv), "the unsaved work was not written to an .ESV"


# @scenario SC-term-02
def test_sighup_leaves_the_terminal_as_the_shell_had_it(tmp_path):
    """SIGHUP, a closed terminal's signal: the same as SIGTERM."""
    with WpeSession(str(tmp_path), SEED) as w:
        raw, code = end_the_editor(w, send_signal(signal.SIGHUP))
    assert code == -signal.SIGHUP, "exit status %r" % code
    assert_given_back_whole(raw, "SIGHUP")


# @scenario SC-term-03
def test_alt_x_leaves_the_terminal_as_the_shell_had_it(tmp_path):
    """Alt-X: the editor's own quit writes the same and exits 0."""
    with WpeSession(str(tmp_path), SEED) as w:
        raw, code = end_the_editor(w, lambda s: s.key("\x1bx", delay=1.0))
    assert code == 0, "exit status %r" % code
    assert_given_back_whole(raw, "Alt-X")


# @scenario SC-term-04
@needs_tput
def test_a_crash_writes_the_whole_release_string_last(tmp_path):
    """XWPE_TEST_CRASH=1 raises SIGSEGV at the first wait for a key: the
    handler writes the modes off in the order the exit does, the pointer reset
    and the terminal's own reset strings, so the alternate screen is left and
    the shell is back."""
    with WpeSession(str(tmp_path), SEED, env_extra={"XWPE_TEST_CRASH": "1"}) as w:
        w._drain(2.0)
        raw = bytes(w.raw)
        assert not w.alive(), "the editor did not crash"
    assert raw.rfind(CRASH_OFF) > raw.rfind(KEYS_ON), \
        "the crash left the protocols on:\n%r" % raw[-200:]
    assert raw.endswith(release_string()), \
        "the release string is not the last thing written:\n%r" % raw[-160:]
    assert raw.rfind(ALT_SCREEN_OFF) > raw.rfind(ALT_SCREEN_ON), \
        "the crash left the shell in the alternate screen"


# @scenario SC-term-04
@needs_tput
def test_the_crash_string_is_what_the_normal_exit_writes(tmp_path):
    """Parity: what the handler writes after the modes off and that changes
    the terminal's state (the pointer reset, the keypad mode and the screen
    switch) is also written by the editor's own quit.  Curses writes
    attributes and the cursor only when they are not already plain, so the
    handler's attribute and cursor resets are the safe superset, not a
    copy."""
    with WpeSession(str(tmp_path), SEED) as w:
        raw, _ = end_the_editor(w, lambda s: s.key("\x1bx", delay=1.0))
    tail = raw[raw.find(MODES_OFF) + len(MODES_OFF):]
    at = tail.find(POINTER_RESET)
    assert at >= 0, "the quit did not give the pointer back:\n%r" % tail[:300]
    for cap in ("rmkx", "rmcup"):
        piece = terminfo_string(cap)
        assert tail.find(piece, at) >= 0, \
            "the quit did not write %s (%r) after the pointer:\n%r" % (cap, piece, tail)


def write_sleeping_shell(workdir):
    """A SHELL that says it runs and then sleeps, so a signal reaches the
    editor while the shell has the terminal."""
    path = os.path.join(workdir, "sleeping-shell.sh")
    with open(path, "w") as fh:
        fh.write("#!/bin/sh\nprintf '%s\\n'\nexec sleep %d\n"
                 % (SLEEP_MARK.decode(), SLEEP_S))
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)
    return path


def assert_nothing_reset_after(raw, mark, what):
    """After @mark (the hand-over's last byte) the editor wrote no mode reset
    and no second screen switch: the screen was left once, and the keyboard
    entry popped once, which is the shell's to keep."""
    after = raw[mark:]
    for seq in (KITTY_POP, ALT_SCREEN_OFF, ALT_SCREEN_ON, b"\x1b[?1002l",
                b"\x1b[?2004l", RIS):
        assert seq not in after, \
            "%s wrote %r after the terminal was handed over:\n%r" \
            % (what, seq, after[:300])


def shell_has_the_terminal(w):
    """Open File > Shell on a shell that sleeps; return how much was written
    by the time the shell said it was running."""
    w.key("Z", delay=0.4)
    del w.raw[:]
    w.menu(ALT.FILE, SHELL_ENTRY, delay=1.0)
    deadline = time.time() + 6
    while time.time() < deadline and SLEEP_MARK not in bytes(w.raw):
        w._drain(0.25)
    assert SLEEP_MARK in bytes(w.raw), "the shell never ran"
    w._drain(SETTLE_S)
    return len(w.raw)


# @scenario SC-term-06
def test_sigterm_while_a_shell_has_the_terminal_resets_nothing_twice(tmp_path):
    """SIGTERM during File > Shell: the screen and the keyboard were already
    given up, so the end writes no second reset, and the work is saved."""
    shell = write_sleeping_shell(str(tmp_path))
    with WpeSession(str(tmp_path), SEED, env_extra={"SHELL": shell}) as w:
        mark = shell_has_the_terminal(w)
        os.kill(w.proc.pid, signal.SIGTERM)
        wait_for_exit(w)
        raw, code = bytes(w.raw), w.proc.poll()
        esv = w.path + ".ESV"
    assert not w.alive(), "the editor did not end"
    assert_nothing_reset_after(raw, mark, "SIGTERM in a shell")
    assert raw.count(ALT_SCREEN_OFF) == 1, \
        "the alternate screen was left %d times" % raw.count(ALT_SCREEN_OFF)
    assert os.path.exists(esv), "the unsaved work was not written to an .ESV"


# @scenario SC-term-06
def test_a_crash_while_a_shell_has_the_terminal_resets_nothing_twice(tmp_path):
    """SIGSEGV during File > Shell: the handler leaves the keyboard modes and
    the screen alone, which the hand-over already gave back."""
    shell = write_sleeping_shell(str(tmp_path))
    with WpeSession(str(tmp_path), SEED, env_extra={"SHELL": shell}) as w:
        mark = shell_has_the_terminal(w)
        os.kill(w.proc.pid, signal.SIGSEGV)
        wait_for_exit(w)
        raw = bytes(w.raw)
    assert not w.alive(), "the editor did not end"
    assert_nothing_reset_after(raw, mark, "a crash in a shell")
    assert raw.count(ALT_SCREEN_OFF) == 1, \
        "the alternate screen was left %d times" % raw.count(ALT_SCREEN_OFF)


def user_screen_is_up(w):
    """Ctrl-F9 a program, then Alt-F5: return how much was written once the
    User Screen shows its prompt."""
    w.key(CTRL_F9, delay=1.0)
    assert wait_screen(w, "USERSCR42", RUN_S), "Run never showed its output"
    w.key("\x1b", delay=0.5)
    w.key(ALT_F5, delay=1.5)
    assert b"press any key" in bytes(w.raw), "the User Screen did not appear"
    w._drain(SETTLE_S)
    return len(w.raw)


# @scenario SC-term-06
@needs_gcc
def test_a_crash_on_the_user_screen_resets_nothing_twice(tmp_path):
    """SIGSEGV on the User Screen, which has the terminal handed over."""
    with WpeSession(str(tmp_path), PROG, filename="t.c", wait=1.5) as w:
        mark = user_screen_is_up(w)
        os.kill(w.proc.pid, signal.SIGSEGV)
        wait_for_exit(w)
        raw = bytes(w.raw)
    assert not w.alive(), "the editor did not end"
    assert_nothing_reset_after(raw, mark, "a crash on the User Screen")


# @scenario SC-term-06
@needs_gcc
def test_sigterm_on_the_user_screen_resets_nothing_twice(tmp_path):
    """SIGTERM on the User Screen.  The screen's wait is interrupted by the
    signal, so the editor takes the terminal back and ends from its own
    screen (the end is then the ordinary one, which must be whole); had it not
    been interrupted, the end would come from the handler with the terminal
    handed over, which must reset nothing twice."""
    with WpeSession(str(tmp_path), PROG, filename="t.c", wait=1.5) as w:
        mark = user_screen_is_up(w)
        os.kill(w.proc.pid, signal.SIGTERM)
        wait_for_exit(w, 1.5)
        if w.alive():
            os.kill(w.proc.pid, signal.SIGTERM)
            wait_for_exit(w)
        raw = bytes(w.raw)
    assert not w.alive(), "the editor did not end"
    after = raw[mark:]
    if ALT_SCREEN_ON in after:
        assert_given_back_whole(after, "SIGTERM on the User Screen")
    else:
        assert_nothing_reset_after(raw, mark, "SIGTERM on the User Screen")
