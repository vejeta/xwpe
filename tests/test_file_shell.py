"""File > Shell: the shell gets a usable terminal and the editor repaints after it.

File > Shell (we_fl_unix.c: WpeShell) hands the whole terminal to the user's
shell and takes it back when the shell exits.  Two things must hold for that
to look right, and both were broken on the console:

* the shell must inherit a cooked terminal with every private mode the editor
  switched on turned off again -- bracketed paste, mouse tracking, the
  alternate screen, keypad transmit -- otherwise a paste or a mouse movement
  in the shell prints escape sequences and the shell "looks like it needs a
  reset";
* on return the editor must repaint its whole screen from its own buffer,
  because the shell scrolled and overwrote whatever was on the terminal.

The tests drive the real menu under a pty with ``SHELL`` pointing at a script
that prints a marker, captures ``stty -a`` and exits, so the round trip runs
headlessly.  The raw bytes the editor wrote are split at the marker: what
came before is the terminal state the shell inherited, what came after is the
editor taking it back.  Every escape sequence is asserted byte for byte as
we_term.c / we_mouse.c emit it.

Run: WPE_BIN=$PWD/wpe tests/.venv/bin/python -m pytest -q tests/test_file_shell.py
"""
import os
import re
import stat

from wpe_driver import WpeSession, ALT

SEED = "AAAAA\nBBBBB\nCCCCC\n"
SHELL_MARK = b"SHELLMARK"
STTY_CAPTURE = "stty.txt"
# File menu entry "SHell" (we_menue.c): its hotkey is H.
SHELL_ENTRY = "h"

# Private-mode sequences, byte for byte as the editor emits them.
BRACKETED_PASTE_ON = b"\x1b[?2004h"       # we_term.c, startup / terminal back
BRACKETED_PASTE_OFF = b"\x1b[?2004l"      # we_term.c, e_endwin
MOUSE_BUTTON_ON = b"\x1b[?1002h"          # we_mouse.c, e_mouse_tracking_enable
MOUSE_SGR_ON = b"\x1b[?1006h"
MOUSE_BUTTON_OFF = b"\x1b[?1002l"         # we_mouse.c, e_mouse_tracking_disable
MOUSE_SGR_OFF = b"\x1b[?1006l"
ALT_SCREEN_ON = b"\x1b[?1049h"            # ncurses smcup, on refresh after endwin
ALT_SCREEN_OFF = b"\x1b[?1049l"           # ncurses rmcup, in endwin
KEYPAD_ON = b"\x1b[?1h\x1b="              # ncurses smkx
KEYPAD_OFF = b"\x1b[?1l\x1b>"             # ncurses rmkx, in endwin

# Mode numbers of the xterm mouse-tracking family; whichever of these startup
# switched on must be switched off again before the shell starts.
MOUSE_MODES = (1000, 1002, 1003, 1006)
BRACKETED_PASTE_MODE = 2004
ALT_SCREEN_MODE = 1049
KEYPAD_MODE = 1

PRIVATE_MODE = re.compile(rb"\x1b\[\?([0-9;]+)([hl])")

# stty -a flags a shell needs to behave: line editing, echo, CR->NL on input,
# output post-processing.  Each must appear without a leading "-".
COOKED_FLAGS = ("icanon", "echo", "icrnl", "opost")


def private_modes(raw):
    """Replay every ESC [ ? ... h/l in ``raw`` in order and return
    {mode number: True if set, False if reset} for the modes mentioned."""
    state = {}
    for params, action in PRIVATE_MODE.findall(raw):
        for number in params.split(b";"):
            state[int(number)] = (action == b"h")
    return state


def modes_set(raw):
    """The mode numbers ``raw`` leaves switched ON."""
    return {mode for mode, on in private_modes(raw).items() if on}


def write_marker_shell(workdir):
    """A SHELL that prints the marker, captures stty -a and exits, so the
    whole File > Shell round trip runs without a human typing ``exit``."""
    path = os.path.join(workdir, "marker-shell.sh")
    capture = os.path.join(workdir, STTY_CAPTURE)
    with open(path, "w") as fh:
        fh.write("#!/bin/sh\n"
                 "printf '%s\\n'\n" % SHELL_MARK.decode()
                 + "stty -a > '%s' 2>&1\n" % capture
                 + "exit 0\n")
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)
    return path


def stty_tokens(workdir):
    """The words of the shell's ``stty -a`` capture."""
    with open(os.path.join(workdir, STTY_CAPTURE)) as fh:
        return fh.read().replace(";", " ").split()


def split_at_marker(raw):
    """(bytes the editor wrote before the shell printed the marker,
    bytes written after it)."""
    i = raw.find(SHELL_MARK)
    assert i >= 0, "the shell never ran (no %r in the pty stream):\n%r" % (
        SHELL_MARK, raw)
    return raw[:i], raw[i + len(SHELL_MARK):]


# @scenario SC-shell-01
def test_shell_inherits_a_cooked_terminal_with_modes_off(tmp_path):
    """Before the marker every private mode is off and the tty is cooked;
    after it the editor switches its modes back on."""
    workdir = str(tmp_path)
    shell = write_marker_shell(workdir)
    with WpeSession(workdir, SEED, env_extra={"SHELL": shell}) as w:
        startup_mouse = modes_set(bytes(w.raw)) & set(MOUSE_MODES)
        assert startup_mouse, "startup enabled no mouse tracking mode at all"
        del w.raw[:]
        w.menu(ALT.FILE, SHELL_ENTRY, delay=1.0)
        raw = bytes(w.raw)
        assert w.alive(), "wpe died around File > Shell"
        before, after = split_at_marker(raw)

        # The editor turned each of its modes off explicitly ...
        assert BRACKETED_PASTE_OFF in before, \
            "bracketed paste was not switched off before the shell:\n%r" % before
        assert MOUSE_SGR_OFF in before and MOUSE_BUTTON_OFF in before, \
            "mouse tracking was not switched off before the shell:\n%r" % before
        assert ALT_SCREEN_OFF in before, \
            "the alternate screen was not left before the shell:\n%r" % before
        assert KEYPAD_OFF in before, \
            "keypad transmit mode was not switched off before the shell:\n%r" % before
        # ... and nothing switched them on again before the shell started.
        inherited = private_modes(before)
        assert not inherited.get(BRACKETED_PASTE_MODE), \
            "bracketed paste is on again when the shell starts:\n%r" % before
        for mode in startup_mouse:
            assert not inherited.get(mode), \
                "mouse mode %d is on again when the shell starts:\n%r" % (mode, before)
        assert not inherited.get(ALT_SCREEN_MODE), \
            "the editor re-entered the alternate screen before the shell:\n%r" % before
        assert not inherited.get(KEYPAD_MODE), \
            "keypad transmit mode is on again when the shell starts:\n%r" % before

        # The shell saw a cooked line discipline.
        tokens = stty_tokens(workdir)
        for flag in COOKED_FLAGS:
            assert flag in tokens and "-" + flag not in tokens, \
                "the shell inherited a raw tty (%s off): %s" % (flag, " ".join(tokens))

        # Back in the editor every mode is on again.
        restored = private_modes(after)
        assert BRACKETED_PASTE_ON in after and restored.get(BRACKETED_PASTE_MODE), \
            "bracketed paste was not switched back on after the shell:\n%r" % after
        assert MOUSE_BUTTON_ON in after and MOUSE_SGR_ON in after, \
            "mouse tracking was not switched back on after the shell:\n%r" % after
        for mode in startup_mouse:
            assert restored.get(mode), \
                "mouse mode %d stayed off after the shell:\n%r" % (mode, after)
        assert ALT_SCREEN_ON in after and restored.get(ALT_SCREEN_MODE), \
            "the editor did not re-enter the alternate screen:\n%r" % after
        assert KEYPAD_ON in after and restored.get(KEYPAD_MODE), \
            "keypad transmit mode was not switched back on:\n%r" % after


# @scenario SC-shell-02
def test_editor_repaints_its_screen_when_the_shell_exits(tmp_path):
    """The first drain after the shell exits shows exactly the screen from
    before File > Shell, and the keyboard still reaches the editor."""
    workdir = str(tmp_path)
    shell = write_marker_shell(workdir)
    with WpeSession(workdir, SEED, env_extra={"SHELL": shell}) as w:
        before = list(w.display())
        last = len(before) - 1
        assert "File" in before[0] and "Help" in before[0], \
            "menu bar missing before the shell:\n%s" % "\n".join(before)
        assert "F1" in before[last], \
            "shortcut bar missing before the shell:\n%s" % "\n".join(before)
        w.menu(ALT.FILE, SHELL_ENTRY, delay=1.0)
        assert SHELL_MARK in bytes(w.raw), "the shell never ran"
        after = list(w.display())
        assert w.alive(), "wpe died around File > Shell"
        wrong = ["row %2d: before=%r\n        after =%r" % (n, a, b)
                 for n, (a, b) in enumerate(zip(before, after)) if a != b]
        assert not wrong, \
            "the editor did not repaint its screen after the shell:\n" + "\n".join(wrong)

        w.key("Z", delay=0.4)
        assert "ZAAAAA" in w.display()[2], \
            "a key typed after the shell did not reach the editor:\n%s" % (
                "\n".join(w.display()))
        w.save()
        assert w.text() == "Z" + SEED, "the edit after the shell was not saved"
