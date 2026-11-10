"""One key table on the terminal: what the user sees when a chord arrives.

The Debug menu and the bars of a debug session show Ctrl-F chords (^F8
Toggle Breakpoint, ^F7 Make Watch, ^F10 Run/Continue, ^F2 Quit).  A terminal
sends them as Ctrl-Fn function keys, and the editor used to read only two of
them (Ctrl-F9 and Alt-F5), so the others did nothing, and Ctrl-F2 was read
as Shift-F10.  Every key now goes through one table (we_keymap.c) that the
terminal, X11 and Wayland share, and these rows drive it with the bytes a
terminal sends, under the TERM values a user has:

- xterm-256color: terminfo folds the sequence into KEY_F(25..36), the Ctrl
  block;
- linux: its terminfo has no Ctrl function keys, so the same bytes reach the
  editor raw and are read as an xterm escape sequence.

Both must give the same key.  The graphical twins are tests/x11/test_keymap.py
and tests/wayland/test_keymap.py; the table itself is test_keymap in
`make check`.
"""
import os
import time

import pytest

from wpe_driver import WpeSession, incoherence, wait_screen
from test_debugger_integration import (
    C_AFTER_PRINT_LINE, C_BREAK_LINE, STEP_TIMEOUT_S, _c_session, _children,
    _mark_line, _move_down, _need, _screen, _start_c_session, _wait_for)

# The xterm-256color terminfo sequences (curses.tigetstr): kf26, kf31, kf32,
# kf34 are Ctrl-F2, Ctrl-F7, Ctrl-F8, Ctrl-F10; kf22 is Shift-F10; kf33 and
# kf53 are Ctrl-F9 and Alt-F5.
CTRL_F2 = "\x1b[1;5Q"
CTRL_F7 = "\x1b[18;5~"
CTRL_F8 = "\x1b[19;5~"
CTRL_F9 = "\x1b[20;5~"
CTRL_F10 = "\x1b[21;5~"
SHIFT_F10 = "\x1b[21;2~"
ALT_F5 = "\x1b[15;3~"

# Back-tab: kcbt of each terminfo entry.
SHIFT_TAB = {"xterm-256color": "\x1b[Z", "linux": "\x1b\t"}

TERMS = ["xterm-256color", "linux"]

# Gaps between the ESC and the key of an Alt chord whose bytes arrive apart.
ESC_ALT_GAP_S = 0.015
ESC_LATE_GAP_S = 0.080

# Ctrl-F2 spelled the vt220 way (F2 is 12, the modifier after the ';'): no
# terminfo entry of this machine has it, so it reaches the editor raw even
# under xterm-256color, which is what the debug rows need: they find the
# current-line mark by its bright green background, which an 8-colour
# terminfo (linux, screen) cannot draw.
CTRL_F2_VT220 = "\x1b[12;5~"

# 6*7 = 42 is computed at runtime, so the marker exists only in the output.
PROG = ('#include <stdio.h>\n'
        'int main(){ printf("XQZ%d\\n", 6 * 7); return 0; }\n')
RAN = "XQZ42"
RUN_TIMEOUT_S = 15
USER_SCREEN_PROMPT = "press any key to return to the editor"

# Alt-Insert and Alt-Delete as a terminal sends them: with Alt as an ESC
# prefix (the Linux console, rxvt, xterm's metaSendsEscape) and as xterm's
# modified key, which TERM=linux's terminfo does not name, so it arrives raw.
ALT_INSERT = {"xterm-256color": "\x1b\x1b[2~", "linux": "\x1b[2;3~"}
ALT_DELETE = {"xterm-256color": "\x1b\x1b[3~", "linux": "\x1b[3;3~"}
# Spelled so that both terminfo entries read them (folded or raw).
RIGHT = "\x1b[C"
END = "\x1b[4~"
CTRL_K_BEGIN = "\x0bb"
CTRL_K_END = "\x0bk"


def _session(tmp_path, term="xterm-256color"):
    return _c_session(tmp_path, env_extra={"TERM": term})


# @scenario SC-debug-26
def test_ctrl_f8_toggles_a_breakpoint_and_ctrl_f10_runs_on(tmp_path):
    """The chords the Debug menu shows for Toggle Breakpoint
    (^F8) and Run/Continue (^F10) do what the menu says.  Stopped on the
    first breakpoint, ^F8 on a later line marks it and ^F10 runs on to it."""
    _need("gcc", "gdb")
    with _session(tmp_path) as w:
        _start_c_session(w)
        _move_down(w, C_AFTER_PRINT_LINE - C_BREAK_LINE)
        w.key(CTRL_F8, delay=0.6)
        assert _wait_for(w, lambda: C_AFTER_PRINT_LINE in w.breakpoint_lines(),
                         STEP_TIMEOUT_S), \
            "^F8 should mark line %d as a breakpoint:\n%s" \
            % (C_AFTER_PRINT_LINE, _screen(w))
        w.key(CTRL_F10, delay=0.6)
        assert _wait_for(w, lambda: _mark_line(w) == C_AFTER_PRINT_LINE,
                         STEP_TIMEOUT_S), \
            "^F10 should run on to the breakpoint at line %d:\n%s" \
            % (C_AFTER_PRINT_LINE, _screen(w))


# @scenario SC-debug-26
def test_ctrl_f7_opens_the_add_watch_box(tmp_path):
    """^F7, the chord the Debug menu shows for Make Watch, opens the
    Add Watch box."""
    _need("gcc", "gdb")
    with _session(tmp_path) as w:
        _start_c_session(w)
        w.key(CTRL_F7, delay=0.8)
        assert "Add Watch" in _screen(w), \
            "^F7 should open the Add Watch box:\n%s" % _screen(w)
        w.key("\x1b")


# @scenario SC-debug-26
@pytest.mark.parametrize("chord", [CTRL_F2, CTRL_F2_VT220],
                         ids=["xterm-folded", "vt220-raw"])
def test_ctrl_f2_asks_to_quit_the_debugger_and_yes_ends_it(tmp_path, chord):
    """xterm's Ctrl-F2 bytes were read as KEY_F(26), which the editor
    took for Shift-F10, so ^F2 Quit worked only by clicking the bar.  It now
    asks, and Yes ends gdb; the vt220 spelling, which terminfo does not fold,
    gives the same key."""
    _need("gcc", "gdb")
    with _session(tmp_path) as w:
        _start_c_session(w)
        w.key(chord, delay=0.8)
        assert "Quit Debugger" in _screen(w), \
            "^F2 should ask whether to quit the debugger:\n%s" % _screen(w)
        w.key("y", delay=0.6)
        assert _wait_for(w, lambda: not _children(w, "gdb"), STEP_TIMEOUT_S), \
            "answering Yes should end gdb"


# @scenario SC-debug-62
def test_shift_f10_is_not_ctrl_f2(tmp_path):
    """Guard for the Ctrl-F2 row: Shift-F10 and Ctrl-F2 were one key to the editor; now
    Shift-F10 is its own key, and in a session it does not ask to quit."""
    _need("gcc", "gdb")
    with _session(tmp_path, "xterm-256color") as w:
        _start_c_session(w)
        w.key(SHIFT_F10, delay=0.8)
        assert "Quit Debugger" not in _screen(w), \
            "Shift-F10 must not be read as ^F2:\n%s" % _screen(w)
        assert _mark_line(w) == C_BREAK_LINE, \
            "Shift-F10 should leave the session where it was:\n%s" % _screen(w)


def _esc_then_letter(tmp_path, gap_s):
    """Write ESC, wait gap_s, write "o", and return the screen rows: what the
    editor made of an Alt-O whose two bytes arrived apart."""
    with WpeSession(str(tmp_path), "line one\nline two\n",
                    filename="t.c") as w:
        os.write(w.master_fd, b"\x1b")
        time.sleep(gap_s)              # the gap under test, not a wait
        os.write(w.master_fd, b"o")
        w._drain(1.0)
        return w.display(drain=0)


# @scenario SC-debug-62
def test_esc_and_a_letter_close_together_are_alt_and_the_letter(tmp_path):
    """Guard: ESC and "o" up to the Alt window apart (25 ms in curses,
    25 ms more in the decoder, measured: 45 ms works) are Alt-O, which opens
    the Options menu, and the "o" is not typed."""
    rows = _esc_then_letter(tmp_path, ESC_ALT_GAP_S)
    text = "\n".join(rows)
    assert "Adjust Colors" in text, \
        "ESC then o, %.0f ms apart, should open the Options menu:\n%s" \
        % (ESC_ALT_GAP_S * 1000, text)
    assert "oline one" not in text, "the o was typed into the file:\n%s" % text


# @scenario SC-debug-62
@incoherence("ESC and a letter more than the Alt window apart (80 ms "
             "measured) are a lone Esc and a typed letter, so an Alt-O whose "
             "bytes the terminal delivered late opens no menu and types an o; "
             "not reproduced with both bytes in one write, 15 runs. The window "
             "is the terminal's platform difference; widening it is a choice "
             "about how long a lone Esc takes, left to the user")
def test_esc_and_a_letter_far_apart_are_still_alt_and_the_letter(tmp_path):
    rows = _esc_then_letter(tmp_path, ESC_LATE_GAP_S)
    assert "Adjust Colors" in "\n".join(rows)


# @scenario SC-debug-62
@pytest.mark.parametrize("term", TERMS)
def test_ctrl_f9_runs_the_program_under_every_terminfo(tmp_path, term):
    """Ctrl-F9 gives the same key whether terminfo folds it (xterm) or the
    bytes reach the editor raw (linux has no Ctrl function keys)."""
    with WpeSession(str(tmp_path), PROG, filename="t.c",
                    env_extra={"TERM": term}) as w:
        w.key(CTRL_F9)
        ran = wait_screen(w, RAN, RUN_TIMEOUT_S)
        assert w.alive(), "wpe died on Ctrl-F9"
        assert ran, "Ctrl-F9 did not run the program:\n%s" % "\n".join(w.display())


# @scenario SC-debug-62
@pytest.mark.parametrize("term", TERMS)
def test_alt_f5_shows_the_user_screen_under_every_terminfo(tmp_path, term):
    """Alt-F5 gives the same key under every terminfo, raw or folded."""
    with WpeSession(str(tmp_path), PROG, filename="t.c",
                    env_extra={"TERM": term}) as w:
        w.key(CTRL_F9)
        assert wait_screen(w, RAN, RUN_TIMEOUT_S), \
            "Ctrl-F9 did not run the program:\n%s" % "\n".join(w.display())
        w.key(ALT_F5)
        shown = wait_screen(w, USER_SCREEN_PROMPT, RUN_TIMEOUT_S)
        assert shown, "Alt-F5 did not show the User Screen prompt:\n%s" \
            % "\n".join(w.display())
        w.key("\r")


# @scenario SC-debug-62
@pytest.mark.parametrize("term", TERMS)
def test_shift_tab_goes_back_one_field_under_every_terminfo(tmp_path, term):
    """Shift-Tab is the same key under every terminfo.  In the Replace box
    the first field takes a typed text over a selected one: after Tab to the
    second field and Shift-Tab back, a typed X replaces the first field
    instead of joining the second."""
    with WpeSession(str(tmp_path), "line one\nline two\n", filename="t.c",
                    env_extra={"TERM": term}) as w:
        w.key("\x1bs", delay=0.6)       # Alt-S: Search
        w.key("r", delay=0.8)           # Replace
        w.key("ab", "\t", "cd", SHIFT_TAB[term], "X", delay=0.4)
        disp = w.display()
        find = next((r for r in disp if "Text to Find:" in r), "")
        new = next((r for r in disp if "New Text:" in r), "")
        assert "Text to Find:  X" in find, \
            "Shift-Tab did not go back to the first field:\n%s" % "\n".join(disp)
        assert "cd" in new and "cdX" not in new, \
            "the X went to the second field:\n%s" % "\n".join(disp)
        w.key("\x1b")


# @scenario SC-debug-62
@pytest.mark.parametrize("term", TERMS)
def test_alt_delete_copies_and_alt_insert_pastes_in_a_terminal(tmp_path, term):
    """Alt-Delete and Alt-Insert are the X buffer's copy and paste on X11.
    Read the same way in a terminal, which has no X buffer, they reached an
    empty display hook and the editor crashed.  They copy the marked block
    and paste it here too: "abc" marked, Alt-Delete, End, Alt-Insert gives
    "abcabc" on the first line."""
    with WpeSession(str(tmp_path), "abc\nline two\n", filename="t.c",
                    env_extra={"TERM": term}) as w:
        w.key(CTRL_K_BEGIN, RIGHT, RIGHT, RIGHT, CTRL_K_END, delay=0.3)
        w.key(ALT_DELETE[term], delay=0.6)
        assert w.alive(), "wpe died on Alt-Delete (exit %r)" % (w.exit_code(),)
        w.key(END, ALT_INSERT[term], delay=0.6)
        assert w.alive(), "wpe died on Alt-Insert (exit %r)" % (w.exit_code(),)
        w.save()
        first = w.text().splitlines()[0]
        assert first == "abcabc", \
            "Alt-Delete should copy the block and Alt-Insert paste it, " \
            "the first line is %r" % first
