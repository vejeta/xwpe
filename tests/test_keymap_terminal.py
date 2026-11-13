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
import subprocess
import time

import pytest

from wpe_driver import WpeSession, incoherence, wait_screen
from edit_model.oracle import EditSession
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


# ---- Shift and Ctrl with the cursor keys -----------------------------------
#
# xterm sends Shift-Right as "ESC [ 1;2C" and Ctrl-Right as "ESC [ 1;5C".
# Under xterm-256color and tmux-256color curses folds the first into
# KEY_SRIGHT and the second into a code it numbers itself and names kRIT5;
# screen-256color and linux name neither, so the bytes arrive raw.  Every
# spelling must give the same key.  (On a real Linux console the keys send
# the plain sequences and the shift state comes from an ioctl, which a pty
# cannot answer, so that path is test_keymap's console rows.)

MOD_TERMS = ["xterm-256color", "tmux-256color", "screen-256color", "linux"]

# st sends xterm's bytes, but its terminfo gives some of them to standard
# capabilities that are other keys by name: Ctrl-End is ked (KEY_EOS),
# Ctrl-Insert kil1 (KEY_IL), Ctrl-Delete kclr (KEY_CLEAR), and Shift-End is
# kel (KEY_EOL).  The rows run under it too, when this machine has the entry.
ST_TERM = "st-256color"
MOD_TERMS_ST = MOD_TERMS + [ST_TERM]

MOD_SHIFT_RIGHT = "\x1b[1;2C"
MOD_SHIFT_END = "\x1b[1;2F"
MOD_CTRL_RIGHT = "\x1b[1;5C"
MOD_CTRL_LEFT = "\x1b[1;5D"
MOD_CTRL_PGUP = "\x1b[5;5~"
MOD_CTRL_PGDN = "\x1b[6;5~"
MOD_CTRL_HOME = "\x1b[1;5H"
MOD_CTRL_END = "\x1b[1;5F"
MOD_CTRL_DEL = "\x1b[3;5~"
MOD_CTRL_INS = "\x1b[2;5~"
MOD_SHIFT_INS = "\x1b[2;2~"
MOD_END = "\x1b[4~"             # End in both spellings (folded or raw)
MOD_F2 = "\x1bOQ"
MOD_CTRL_K = "\x0b"

# Longer than the 30-row window, so its last row is not the file's.
MOD_LONG_LINES = 60
MOD_LONG_SEED = "".join("line %02d\n" % i for i in range(MOD_LONG_LINES))


def _mod_session(tmp_path, seed, term):
    _need_terminfo(term)
    return EditSession(str(tmp_path), seed, filename="t.txt",
                       env_extra={"TERM": term})


def _mod_saved_lines(s):
    """Save with F2 and return the file's lines as written to disk."""
    s.send([MOD_F2])
    return s.disk().decode("utf-8").splitlines()


# @scenario SC-debug-62
@pytest.mark.parametrize("term", MOD_TERMS_ST)
@pytest.mark.parametrize("delete", [[MOD_CTRL_K, "y"], [MOD_CTRL_DEL]],
                         ids=["ctrl-k-y", "ctrl-del"])
def test_shift_right_marks_a_block_under_every_terminfo(tmp_path, term, delete):
    """Shift-Right twice marks the first two characters, and Ctrl-K Y or
    Ctrl-Del deletes them: "abcdef" is "cdef" on disk.  Under
    xterm-256color Shift-Right did nothing, so nothing was deleted."""
    with _mod_session(tmp_path, "abcdef\nsecond\n", term) as s:
        s.send([MOD_SHIFT_RIGHT, MOD_SHIFT_RIGHT])
        s.send(delete)
        lines = _mod_saved_lines(s)
    assert lines[0] == "cdef", \
        "Shift-Right twice then %r should delete 'ab', line 1 is %r" \
        % (delete, lines[0])


# @scenario SC-debug-62
@pytest.mark.parametrize("term", MOD_TERMS_ST)
def test_ctrl_insert_copies_and_shift_insert_pastes_under_every_terminfo(tmp_path, term):
    """Shift-Right twice marks "ab", Ctrl-Insert copies it, and Shift-Insert
    at the end of the line pastes it: "abcdefab" on disk."""
    with _mod_session(tmp_path, "abcdef\nsecond\n", term) as s:
        s.send([MOD_SHIFT_RIGHT, MOD_SHIFT_RIGHT])
        s.send([MOD_CTRL_INS])
        s.send([MOD_END, MOD_SHIFT_INS])
        lines = _mod_saved_lines(s)
    assert lines[0] == "abcdefab", \
        "Ctrl-Insert then Shift-Insert should copy 'ab' to the end, " \
        "line 1 is %r" % lines[0]


# @scenario SC-debug-62
@pytest.mark.parametrize("term", MOD_TERMS_ST)
def test_shift_end_marks_to_the_end_of_the_line_under_every_terminfo(tmp_path, term):
    """Shift-End marks from the cursor to the end of the line, and Ctrl-K Y
    deletes it: the first line is empty on disk, the second untouched."""
    with _mod_session(tmp_path, "abcdef\nsecond\n", term) as s:
        s.send([MOD_SHIFT_END])
        s.send([MOD_CTRL_K, "y"])
        lines = _mod_saved_lines(s)
    assert lines[:2] == ["", "second"], \
        "Shift-End then Ctrl-K Y should empty line 1, the file is %r" % lines


# @scenario SC-debug-62
@pytest.mark.parametrize("term", MOD_TERMS_ST)
def test_ctrl_right_and_ctrl_left_move_by_a_word_under_every_terminfo(tmp_path, term):
    """Ctrl-Right goes to the next word and Ctrl-Left back: an X typed after
    Ctrl-Right twice and Ctrl-Left once lands before "def"."""
    with _mod_session(tmp_path, "abc def ghi\n", term) as s:
        s.send([MOD_CTRL_RIGHT, MOD_CTRL_RIGHT, MOD_CTRL_LEFT])
        s.send(["X"])
        lines = _mod_saved_lines(s)
    assert lines[0] == "abc Xdef ghi", \
        "Ctrl-Right twice and Ctrl-Left should put the cursor on 'def', " \
        "line 1 is %r" % lines[0]


# @scenario SC-debug-62
@pytest.mark.parametrize("term", MOD_TERMS_ST)
def test_ctrl_pgdn_and_ctrl_pgup_go_to_the_ends_of_the_file(tmp_path, term):
    """Ctrl-PgDn goes to the last line of the file, Ctrl-PgUp to the first:
    an E typed after the first and an S after the second land there."""
    with _mod_session(tmp_path, MOD_LONG_SEED, term) as s:
        s.send([MOD_CTRL_PGDN])
        s.send(["E"])
        s.send([MOD_CTRL_PGUP])
        s.send(["S"])
        lines = _mod_saved_lines(s)
    assert "S" in lines[0], "Ctrl-PgUp: line 1 is %r" % lines[0]
    assert "E" in lines[-1], "Ctrl-PgDn: the last line is %r" % lines[-1]


# @scenario SC-debug-62
@pytest.mark.parametrize("term", MOD_TERMS_ST)
def test_ctrl_end_and_ctrl_home_go_to_the_ends_of_the_window(tmp_path, term):
    """Ctrl-End goes to the last row of the window, not of the file, and
    Ctrl-Home back to the first: in a file longer than the window an E typed
    after Ctrl-End lands on a line between the first and the last, and an S
    after Ctrl-Home on the first."""
    with _mod_session(tmp_path, MOD_LONG_SEED, term) as s:
        s.send([MOD_CTRL_END])
        s.send(["E"])
        s.send([MOD_CTRL_HOME])
        s.send(["S"])
        lines = _mod_saved_lines(s)
    marked = [i for i, line in enumerate(lines) if "E" in line]
    assert "S" in lines[0], "Ctrl-Home: line 1 is %r" % lines[0]
    assert len(marked) == 1 and 0 < marked[0] < MOD_LONG_LINES - 1, \
        "Ctrl-End should go to the window's last row, E is on lines %r" % marked


# rxvt-unicode spells the keys its own way: Shift-Right "ESC [ c" (kRIT,
# folded into KEY_SRIGHT) and Ctrl-Delete "ESC [ 3 ^" (kDC5).  ncurses
# numbers the keys only an extended capability names from KEY_MAX itself,
# and kDC5 is the first of them in this entry, so it arrives as exactly
# KEY_MAX.
RXVT_TERM = "rxvt-unicode-256color"
RXVT_SHIFT_RIGHT = "\x1b[c"
RXVT_CTRL_DEL = "\x1b[3^"


def _has_terminfo(term):
    """True when this machine has a terminfo entry for ``term``."""
    found = subprocess.run(["infocmp", term], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    return found.returncode == 0


def _need_terminfo(term):
    """Skip the row when this machine has no terminfo entry for ``term``."""
    if not _has_terminfo(term):
        pytest.skip("no %s terminfo entry here" % term)


# @scenario SC-debug-62
def test_ctrl_delete_deletes_a_block_under_rxvt(tmp_path):
    """Shift-Right twice then Ctrl-Delete as rxvt-unicode sends them delete
    the first two characters: "abcdef" is "cdef" on disk.  Ctrl-Delete is
    the key ncurses numbers KEY_MAX there, which the decoder missed."""
    if not _has_terminfo(RXVT_TERM):
        pytest.skip("no %s terminfo entry here" % RXVT_TERM)
    with _mod_session(tmp_path, "abcdef\nsecond\n", RXVT_TERM) as s:
        s.send([RXVT_SHIFT_RIGHT, RXVT_SHIFT_RIGHT])
        s.send([RXVT_CTRL_DEL])
        lines = _mod_saved_lines(s)
    assert lines[0] == "cdef", \
        "Shift-Right twice then Ctrl-Delete under %s should delete 'ab', " \
        "line 1 is %r" % (RXVT_TERM, lines[0])


# rxvt's other spellings: the "ESC [ n ~" keys end in "$" with Shift, "^"
# with Ctrl and "@" with both, and the arrows are lower-case letters,
# "ESC [ a" with Shift and "ESC O a" with Ctrl.  Its terminfo gives Ctrl-End
# ("ESC [ 8 ^", kEND5) to kel as well, so curses delivers KEY_EOL for it.
RXVT_CTRL_END = "\x1b[8^"
RXVT_CTRL_HOME = "\x1b[7^"
RXVT_SHIFT_UP = "\x1b[a"
RXVT_SHIFT_DOWN = "\x1b[b"
RXVT_SHIFT_LEFT = "\x1b[d"
RXVT_CTRL_RIGHT = "\x1bOc"
RXVT_CTRL_LEFT = "\x1bOd"
RXVT_DOWN = "\x1b[B"
RXVT_END = "\x1b[8~"


# @scenario SC-debug-62
def test_ctrl_end_and_ctrl_home_go_to_the_ends_of_the_window_under_rxvt(tmp_path):
    """Ctrl-End and Ctrl-Home as rxvt-unicode sends them go to the last and
    the first row of the window: an E typed after Ctrl-End lands between the
    first and the last line of a file longer than the window, an S after
    Ctrl-Home on the first.  Ctrl-End arrived as KEY_EOL and did nothing."""
    with _mod_session(tmp_path, MOD_LONG_SEED, RXVT_TERM) as s:
        s.send([RXVT_CTRL_END])
        s.send(["E"])
        s.send([RXVT_CTRL_HOME])
        s.send(["S"])
        lines = _mod_saved_lines(s)
    marked = [i for i, line in enumerate(lines) if "E" in line]
    assert "S" in lines[0], "Ctrl-Home: line 1 is %r" % lines[0]
    assert len(marked) == 1 and 0 < marked[0] < MOD_LONG_LINES - 1, \
        "Ctrl-End should go to the window's last row, E is on lines %r" % marked


# @scenario SC-debug-62
@pytest.mark.parametrize("keys, first_lines", [
    ([RXVT_SHIFT_DOWN], ["second", "third"]),
    ([RXVT_DOWN, RXVT_DOWN, RXVT_SHIFT_UP], ["abcdef", "third"]),
    ([RXVT_END, RXVT_SHIFT_LEFT, RXVT_SHIFT_LEFT], ["abcd", "second"]),
], ids=["shift-down", "shift-up", "shift-left"])
def test_shift_arrows_mark_a_block_under_rxvt(tmp_path, keys, first_lines):
    """Shift and an arrow as rxvt-unicode sends it marks a block, and
    Ctrl-K Y deletes it: Shift-Down from the start marks the first line,
    Shift-Up from the third the second, Shift-Left twice from the end the
    last two characters."""
    with _mod_session(tmp_path, "abcdef\nsecond\nthird\n", RXVT_TERM) as s:
        s.send(keys)
        s.send([MOD_CTRL_K, "y"])
        lines = _mod_saved_lines(s)
    assert lines[:2] == first_lines, \
        "%r then Ctrl-K Y should leave %r, the file is %r" \
        % (keys, first_lines, lines)


# @scenario SC-debug-62
def test_ctrl_right_and_ctrl_left_move_by_a_word_under_rxvt(tmp_path):
    """Ctrl-Right and Ctrl-Left as rxvt-unicode sends them ("ESC O c",
    "ESC O d") move by a word: an X typed after Ctrl-Right twice and
    Ctrl-Left once lands before "def"."""
    with _mod_session(tmp_path, "abc def ghi\n", RXVT_TERM) as s:
        s.send([RXVT_CTRL_RIGHT, RXVT_CTRL_RIGHT, RXVT_CTRL_LEFT])
        s.send(["X"])
        lines = _mod_saved_lines(s)
    assert lines[0] == "abc Xdef ghi", \
        "Ctrl-Right twice and Ctrl-Left should put the cursor on 'def', " \
        "line 1 is %r" % lines[0]


# --- The two unambiguous keyboard protocols -------------------------------
#
# The editor asks the terminal for xterm's modifyOtherKeys and kitty's
# keyboard protocol, so Ctrl-Shift-letter, Esc at once and the rest arrive as
# reports.  With either on, Esc, every Ctrl-letter and every Alt-letter come
# ONLY as a report, so each must read as the key the legacy byte reads as.
# These rows send the reports a terminal sends (the pty under test sends
# nothing of its own).

PROTOCOL_SEED = "ab\n"
# A report is no text and leaks no tail: each, then Q, leaves only Q typed.
# The ':' forms and the long junk were typed as text before (the tail reader
# stopped at the first byte that is not a digit or ';').
REPORTS_THAT_TYPE_NOTHING = [
    "\x1b[27;6;112~", "\x1b[112;6u", "\x1b[23~", "\x1b[27u",
    "\x1b[27;5;115~", "\x1b[27;5;99~",
    "\x1b[112:80;6u", "\x1b[1103:1071:1094;6u",
    "\x1b[" + ";".join(["1"] * 20) + "z",
]
KITTY_CTRL_Y = "\x1b[121;5u"
MOK_CTRL_Y = "\x1b[27;5;121~"
KITTY_CTRL_SHIFT_Y = "\x1b[121;6u"
MOK_CTRL_SHIFT_Y = "\x1b[27;6;121~"
KITTY_CTRL_H = "\x1b[104;5u"
KITTY_CTRL_M = "\x1b[109;5u"
KITTY_CTRL_I = "\x1b[105;5u"
KITTY_ALT_F = "\x1b[102;3u"
MOK_ALT_F = "\x1b[27;3;102~"
KITTY_ESC = "\x1b[27u"
KITTY_CTRL_LBRACKET = "\x1b[91;5u"
KITTY_CTRL_BACKSLASH = "\x1b[92;5u"
FILE_MENU_ITEM = "Save As"      # only in the open menu; the bar has "F2 Save"


def _typed_file(tmp_path, seed, *keys, term="xterm-256color"):
    """The file on disk after the keys, then File > Save."""
    with WpeSession(str(tmp_path), seed, filename="t.c",
                    env_extra={"TERM": term}) as w:
        w.key(*keys, delay=0.3)
        w.save()
        return w.text()


# @scenario SC-keymap-01
@pytest.mark.parametrize("report", REPORTS_THAT_TYPE_NOTHING)
def test_a_key_report_types_nothing_and_leaks_no_tail(tmp_path, report):
    """Each report, then Q, leaves only the Q in the file: the report is the
    key it is or is dropped whole, and no part of its tail is typed."""
    got = _typed_file(tmp_path, PROTOCOL_SEED, report, "Q")
    assert got == "Q" + PROTOCOL_SEED, \
        "%r then Q left %r, not 'Qab' (part of the report was typed)" \
        % (report, got)


# @scenario SC-keymap-02
@pytest.mark.parametrize("report", [KITTY_CTRL_Y, MOK_CTRL_Y,
                                    KITTY_CTRL_SHIFT_Y, MOK_CTRL_SHIFT_Y],
                         ids=["kitty", "mok", "kitty-shift", "mok-shift"])
def test_ctrl_y_deletes_the_line_in_every_protocol_form(tmp_path, report):
    """Ctrl-Y deletes a line in the Borland key set.  Under a keyboard
    protocol it arrives as a report, which used to be swallowed (nothing
    happened); Ctrl-Shift-Y does what Ctrl-Y does there."""
    got = _typed_file(tmp_path, "a\nb\nc\n", report)
    assert got == "b\nc\n", \
        "%r should delete the first line, the file is %r" % (report, got)


# @scenario SC-keymap-03
@pytest.mark.parametrize("pair", [
    (KITTY_CTRL_H, "\x08"), (KITTY_CTRL_M, "\r"), (KITTY_CTRL_I, "\t")],
    ids=["ctrl-h-is-backspace", "ctrl-m-is-enter", "ctrl-i-is-tab"])
def test_ctrl_h_m_i_do_what_backspace_enter_tab_do(tmp_path, pair):
    """The three letters whose control byte is another key's get codes of
    their own under a protocol, and do what that other key does."""
    report, legacy = pair
    reported_dir = tmp_path / "reported"
    legacy_dir = tmp_path / "legacy"
    reported_dir.mkdir()
    legacy_dir.mkdir()
    with_report = _typed_file(reported_dir, "x\n", "ab", report, "c")
    with_legacy = _typed_file(legacy_dir, "x\n", "ab", legacy, "c")
    assert with_report == with_legacy, \
        "%r should be read like %r: %r against %r" \
        % (report, legacy, with_report, with_legacy)
    assert with_legacy != "abcx\n", "the legacy key did nothing"


# @scenario SC-keymap-16
@pytest.mark.parametrize("pair", [
    ("\x1b[127;2u", "\x7f"), ("\x1b[27;2;127~", "\x7f"),
    ("\x1b[127;5u", "\x08"), ("\x1b[127;6u", "\x7f"),
    ("\x1b[9;5u", "\t"), ("\x1b[27;5;9~", "\t")],
    ids=["kitty-shift-backspace", "mok-shift-backspace",
         "kitty-ctrl-backspace", "kitty-ctrl-shift-backspace",
         "kitty-ctrl-tab", "mok-ctrl-tab"])
def test_chords_a_legacy_terminal_sends_as_a_plain_key_still_do_it(tmp_path, pair):
    """A legacy terminal sends Shift-, Ctrl- and Ctrl-Shift-Backspace as
    Backspace's byte and Ctrl-Tab as Tab's; xterm and kitty report them as
    chords once the editor asks.  The Borland keys must not lose them: each
    report does what the legacy byte does."""
    report, legacy = pair
    reported_dir = tmp_path / "reported"
    legacy_dir = tmp_path / "legacy"
    reported_dir.mkdir()
    legacy_dir.mkdir()
    with_report = _typed_file(reported_dir, "x\n", "ab", report, "c")
    with_legacy = _typed_file(legacy_dir, "x\n", "ab", legacy, "c")
    assert with_report == with_legacy, \
        "%r should be read like %r: %r against %r" \
        % (report, legacy, with_report, with_legacy)
    assert with_legacy != "abcx\n", "the legacy key did nothing"


# @scenario SC-keymap-04
@pytest.mark.parametrize("report", [KITTY_ALT_F, MOK_ALT_F],
                         ids=["kitty", "mok"])
def test_alt_letter_opens_its_menu_in_every_protocol_form(tmp_path, report):
    """Alt-F opens the File menu.  Under a protocol it is a report."""
    with WpeSession(str(tmp_path), PROTOCOL_SEED, filename="t.c") as w:
        w.key(report, delay=0.5)
        shown = "\n".join(w.display())
        assert FILE_MENU_ITEM in shown, \
            "%r did not open the File menu:\n%s" % (report, shown)


# @scenario SC-keymap-05
@pytest.mark.parametrize("esc", [KITTY_ESC, KITTY_CTRL_LBRACKET],
                         ids=["esc", "ctrl-lbracket"])
def test_a_reported_esc_closes_a_menu(tmp_path, esc):
    """Esc, and Ctrl-[ which is the same key, close an open menu when they
    arrive as reports (Esc is a report under the kitty protocol)."""
    with WpeSession(str(tmp_path), PROTOCOL_SEED, filename="t.c") as w:
        w.key("\x1bf", delay=0.5)           # the legacy Alt-F: the menu
        assert FILE_MENU_ITEM in "\n".join(w.display()), "the menu did not open"
        w.key(esc, delay=0.5)
        shown = "\n".join(w.display())
        assert FILE_MENU_ITEM not in shown, \
            "%r did not close the menu:\n%s" % (esc, shown)


# Ctrl-\ is the byte Shift-Tab used to be numbered as; in a dialog it moved
# the focus back.  In the Replace box: Tab to the second field, then the key
# under test, then X; where the X lands says where the focus is.
def _focus_after_back_key(tmp_path, back_key):
    with WpeSession(str(tmp_path), "line one\nline two\n",
                    filename="t.c") as w:
        w.key("\x1bs", delay=0.6)
        w.key("r", delay=0.8)
        w.key("ab", "\t", "cd", back_key, "X", delay=0.4)
        disp = w.display()
        w.key("\x1b")
    find = next((r for r in disp if "Text to Find:" in r), "")
    new = next((r for r in disp if "New Text:" in r), "")
    return find, new, "\n".join(disp)


# @scenario SC-keymap-06
@pytest.mark.parametrize("ctrl_backslash", ["\x1c", KITTY_CTRL_BACKSLASH],
                         ids=["byte", "report"])
def test_ctrl_backslash_does_not_move_focus_back(tmp_path, ctrl_backslash):
    """Ctrl-\\ sends byte 28, which was also the code of Shift-Tab: it moved
    the focus back in a dialog.  It does nothing there now, and Shift-Tab
    still moves back (the rows below)."""
    find, new, whole = _focus_after_back_key(tmp_path, ctrl_backslash)
    assert "Text to Find:  ab" in find and "X" in new, \
        "Ctrl-\\ moved the focus back to the first field:\n%s" % whole


# @scenario SC-keymap-07
@pytest.mark.parametrize("shift_tab", ["\x1b[Z", "\x1b[27;2;9~", "\x1b[9;2u"],
                         ids=["legacy", "mok", "kitty"])
def test_shift_tab_moves_focus_back_in_every_form(tmp_path, shift_tab):
    """Positive control for the row above: Shift-Tab, as the legacy
    terminal, modifyOtherKeys and kitty each send it, moves back."""
    find, new, whole = _focus_after_back_key(tmp_path, shift_tab)
    assert "Text to Find:  X" in find and "cdX" not in new, \
        "Shift-Tab did not move the focus back:\n%s" % whole


# A program that reads its input to the end, so Ctrl-D (end of input) and
# Ctrl-C (stop) are told apart from text.
ECHO_PROG = ('#include <stdio.h>\n'
             'int main(void){char b[100];int n=0;'
             'puts("READY");fflush(stdout);'
             'while(fgets(b,sizeof b,stdin))n+=(b[0]==\'a\'&&b[1]==\'b\'&&b[2]==10);'
             'printf("EOFSEEN %d\\n",n);return 0;}\n')
LOOP_PROG = ('#include <stdio.h>\n#include <unistd.h>\n'
             'int main(void){FILE*o=fopen("loop.pid","w");'
             'fprintf(o,"%d",(int)getpid());fclose(o);'
             'for(;;)usleep(100000);return 0;}\n')
RUN_CTRL_F9 = "\x1b[20;5~"
PROTOCOL_CTRL_C = ["\x1b[99;5u", "\x1b[27;5;99~"]
PROTOCOL_CTRL_D = ["\x1b[100;5u", "\x1b[27;5;100~"]
RUN_WAIT_S = 20


def _drain_until(w, predicate, seconds):
    deadline = time.time() + seconds
    while time.time() < deadline:
        w._drain(0.25)
        if predicate():
            return True
    return False


# @scenario SC-keymap-08
@pytest.mark.parametrize("ctrl_c", PROTOCOL_CTRL_C, ids=["kitty", "mok"])
def test_ctrl_c_in_a_protocol_form_stops_the_program_run_runs(tmp_path, ctrl_c):
    """The Run loop reads keys through the one reader: Ctrl-C as a report
    stops the program instead of being typed into it byte by byte."""
    _need("gcc")
    pidfile = tmp_path / "loop.pid"
    with WpeSession(str(tmp_path), LOOP_PROG, filename="l.c", wait=1.5) as w:
        w.key(RUN_CTRL_F9, delay=0.5)
        assert _drain_until(
            w, lambda: pidfile.exists() and pidfile.stat().st_size > 0,
            RUN_WAIT_S), "Run never started the program"
        pid = int(pidfile.read_text())
        w.key(ctrl_c, delay=0.5)
        gone = _drain_until(w, lambda: not os.path.exists("/proc/%d" % pid),
                            RUN_WAIT_S)
        if not gone:
            os.kill(pid, 9)            # the pid the program wrote, nobody else's
    assert gone, "Ctrl-C as %r did not stop the program (pid %d)" % (ctrl_c, pid)


# @scenario SC-keymap-09
@pytest.mark.parametrize("ctrl_d", PROTOCOL_CTRL_D, ids=["kitty", "mok"])
def test_ctrl_d_in_a_protocol_form_ends_the_programs_input(tmp_path, ctrl_d):
    """Type ab, Enter, then Ctrl-D as a report: the program reads ab and
    then sees the end of its input (Ctrl-D is the byte 4 on its terminal)."""
    _need("gcc")
    with WpeSession(str(tmp_path), ECHO_PROG, filename="e.c", wait=1.5) as w:
        w.key(RUN_CTRL_F9, delay=0.5)
        assert wait_screen(w, "READY", RUN_WAIT_S), "Run never started the program"
        w.key("ab", "\r", delay=0.4)
        w.key(ctrl_d, delay=0.5)
        ended = wait_screen(w, "EOFSEEN 1", RUN_WAIT_S)
        shown = "\n".join(w.display())
    assert ended, "Ctrl-D as %r did not end the program's input after the " \
        "one line ab (EOFSEEN 1):\n%s" % (ctrl_d, shown)


# @scenario SC-keymap-10
def test_run_sends_the_cursor_keys_to_the_program(tmp_path):
    """The Run loop used to drop the arrow and function keys.  A program
    reading its input in raw bytes sees the xterm sequence of each."""
    _need("gcc")
    prog = ('#include <stdio.h>\n#include <unistd.h>\n#include <termios.h>\n'
            'int main(void){struct termios t;tcgetattr(0,&t);'
            't.c_lflag&=~(ICANON|ECHO);tcsetattr(0,TCSANOW,&t);'
            'puts("RAWREADY");fflush(stdout);'
            'unsigned char b[16];int n=read(0,b,sizeof b);'
            'printf("N=%d B1=%d B2=%d B3=%d\\n",n,b[0],b[1],b[2]);'
            'return 0;}\n')
    with WpeSession(str(tmp_path), prog, filename="k.c", wait=1.5) as w:
        w.key(RUN_CTRL_F9, delay=0.5)
        assert wait_screen(w, "RAWREADY", RUN_WAIT_S), "Run never started it"
        w.key("\x1b[A", delay=0.5)          # Up
        ok = wait_screen(w, "N=3 B1=27 B2=91 B3=65", RUN_WAIT_S)
        shown = "\n".join(w.display())
    assert ok, "the program did not read ESC [ A for Up:\n%s" % shown


# @scenario SC-keymap-15
def test_the_keypad_enter_is_enter_in_a_terminal(tmp_path):
    """xterm in application keypad mode sends the keypad's Enter as "ESC O M",
    which curses names KEY_ENTER; the editor ignored it.  It breaks the line
    as Enter does: ab, keypad Enter, c is two lines."""
    got = _typed_file(tmp_path, "", "ab", "\x1bOM", "c")
    assert got == "ab\nc\n", \
        "the keypad Enter should break the line, the file is %r" % got


# @scenario SC-keymap-12
def test_a_reported_esc_in_a_hit_cr_wait_leaves_no_tail_behind(tmp_path):
    """Ctrl-F9 on a shell script shows its output and waits for a key with
    the terminal handed over (a line at a time).  Esc as a kitty report
    ends the wait as one key; the wait used to take the ESC and leave
    "[27u" queued for the editor, which typed it.  The Q after it is the
    editor's (the line's end, the cooked terminal's Enter, ends the wait's
    line and is dropped with it)."""
    with WpeSession(str(tmp_path), "echo HITMARK\n", filename="h.sh") as w:
        w.key(RUN_CTRL_F9, delay=1.5)
        assert "Strike <Return> to continue" in "\n".join(w.display()), \
            "the script's wait did not appear"
        w.key(KITTY_ESC, "Q", "\r", delay=0.6)
        w.save()
        got = w.text()
    assert "[27u" not in got, "the tail of the Esc report was typed: %r" % got
    assert got == "Qecho HITMARK\n", \
        "after the wait, the Q is typed into the file: %r" % got


# @scenario SC-keymap-11
def test_the_screen_harness_ignores_the_protocol_sequences(tmp_path):
    """pyte read "CSI > 4 ; 2 m" as SGR 4;2 (underline) and drew the "u" of
    "CSI < u".  The harness consumes the private key-protocol sequences, and
    the screen after a real start has no underlined cell."""
    from wpe_driver import _RepStream
    from test_utf8_border import SafeScreen
    screen = SafeScreen(20, 3)
    stream = _RepStream(screen)
    stream.feed("ab\x1b[>4;2mcd\x1b[<uef\x1b[?ugh\x1b[>5u")
    stream.feed("ij\x1b[>4;")        # a sequence cut by the end of a read
    stream.feed("2mkl")
    assert screen.display[0].rstrip() == "abcdefghijkl", screen.display[0]
    assert not any(screen.buffer[0][x].underscore for x in range(12)), \
        "the harness took a key-protocol sequence for SGR underline"
    with WpeSession(str(tmp_path), PROTOCOL_SEED, filename="t.c") as w:
        sent = bytes(w.raw)
        assert b"\x1b[>4;2m" in sent and b"\x1b[>5u" in sent, \
            "the editor did not ask for the protocols: %r" % sent[:200]
        scr = w.screen
        assert not any(scr.buffer[y][x].underscore for y in range(scr.lines)
                       for x in range(scr.columns)), \
            "a cell is underlined after the start"


def _screen_after(tmp_path, name, *strokes):
    """The screen after @strokes, in a session of its own."""
    where = tmp_path / name
    where.mkdir()
    with WpeSession(str(where), PROTOCOL_SEED, filename="t.c") as w:
        for stroke in strokes:
            w.key(stroke, delay=0.5)
        return [row.rstrip() for row in w.display()]


# @scenario SC-keymap-18
@pytest.mark.parametrize("opening,report,legacy", [
    ("\x1bOS", KITTY_CTRL_H, "\x08"),
    ("\x1bOR", KITTY_CTRL_LBRACKET, "\x1b"),
    ("\x1bf", KITTY_CTRL_LBRACKET, "\x1b")],
    ids=["find-dialog-ctrl-h", "file-manager-ctrl-lbracket", "menu-ctrl-lbracket"])
def test_a_dialog_the_file_manager_and_a_menu_read_the_folded_key(
        tmp_path, opening, report, legacy):
    """The fold of Ctrl-H and Ctrl-[ to Backspace and Esc is done once, where
    the editor takes a key, so a field of a dialog, the file manager and a menu
    see the same key as the editor: the screen after the report is the screen
    after the legacy byte."""
    typed = "ab" if opening == "\x1bOS" else ""
    shown_report = _screen_after(tmp_path, "report", opening, typed, report) \
        if typed else _screen_after(tmp_path, "report", opening, report)
    shown_legacy = _screen_after(tmp_path, "legacy", opening, typed, legacy) \
        if typed else _screen_after(tmp_path, "legacy", opening, legacy)
    assert shown_report == shown_legacy, \
        "%r after %r is not read like %r:\n%s\n---\n%s" % (
            report, opening, legacy, "\n".join(shown_report),
            "\n".join(shown_legacy))
