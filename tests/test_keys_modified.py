"""Modified function-key decoding for wpe (terminal emulators).

Modern terminals (xterm modifyOtherKeys, gnome/vte, kitty) report Ctrl-/Alt-
modified function keys as DISTINCT ncurses keycodes -- Ctrl-Fn = KEY_F(24+n),
Alt-Fn = KEY_F(48+n).  e_t_getch (we_term.c) only mapped the plain F1..F10 and
a legacy Shift block, so Ctrl-F9 (KEY_F(33)) and Alt-F5 (KEY_F(53)) fell to the
default and decoded to 0 -- the keys silently did nothing in an emulator.

These tests send the real xterm-256color escape sequences (the harness sets
TERM=xterm-256color, so ncurses decodes them exactly as a real xterm would)
and assert the resulting ACTION, so they would have caught the regression.
"""
import time

from wpe_driver import WpeSession

# xterm-256color terminfo sequences (verified with curses.tigetstr)
CTRL_F9 = "\x1b[20;5~"   # kf33 -> Run program
F9      = "\x1b[20~"     # kf9  -> Make/compile
ALT_F5  = "\x1b[15;3~"   # kf53 -> Borland User Screen

PRINTS = ('#include <stdio.h>\n'
          'int main(){ printf("RUNOK_42\\n"); return 0; }\n')


def test_ctrl_f9_runs_the_program(tmp_path):
    """Ctrl-F9 (KEY_F(33)) runs the compiled program -- its output appears."""
    with WpeSession(str(tmp_path), PRINTS, filename="h.c") as w:
        time.sleep(0.6)
        w.key(F9)                       # compile
        time.sleep(2.5)
        w.key(CTRL_F9)                  # run
        time.sleep(2.5)
        assert w.alive(), "wpe died on Ctrl-F9"
        assert any("RUNOK_42" in l for l in w.display()), \
            "Ctrl-F9 did not run the program (KEY_F(33) decode regressed):\n%s" \
            % "\n".join(w.display())


def test_alt_f5_shows_user_screen(tmp_path):
    """Alt-F5 (KEY_F(53)) drops to the Borland User Screen (with its prompt)."""
    with WpeSession(str(tmp_path), PRINTS, filename="h.c") as w:
        time.sleep(0.6)
        w.key(F9)
        time.sleep(2.5)
        w.key(CTRL_F9)                  # run -> output captured
        time.sleep(2.0)
        w.key(ALT_F5)                   # User Screen
        time.sleep(0.6)
        disp = "\n".join(w.display())
        assert w.alive(), "wpe died on Alt-F5"
        assert "press any key to return" in disp.lower(), \
            "Alt-F5 did not show the User Screen prompt (KEY_F(53) decode):\n%s" \
            % disp
        w.key("\r")                     # return to the editor
