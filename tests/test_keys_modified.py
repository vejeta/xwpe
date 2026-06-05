"""Modified-key and menu-shortcut decoding for wpe (terminal emulators).

Covers two classes of input that broke under wpe in an X11 terminal:

1. Modified FUNCTION keys.  Modern terminals report Ctrl-/Alt-modified
   function keys as DISTINCT ncurses keycodes (Ctrl-Fn = KEY_F(24+n),
   Alt-Fn = KEY_F(48+n)); e_t_getch only mapped plain F1..F10, so Ctrl-F9
   (KEY_F(33)) and Alt-F5 (KEY_F(53)) decoded to 0 and did nothing.

2. Shortcuts WITH A MENU OPEN.  The submenu loop fell back only to
   e_tst_dfkt, which does not know the Run/Compile/Debug shortcuts (those
   live in e_prog_switch).  So with the Run menu open, Alt-U (Run), Alt-M
   (Make), Ctrl-F9 etc. were no-ops, even though they work from the editor.

IMPORTANT test-design note (a previous version of these tests was a false
positive): the program's output must be a value the program COMPUTES, so the
marker string appears ONLY in the program output, never in the source shown
in the editor.  Asserting on a literal that is also in the source passes
without the program ever running.
"""
import time

from wpe_driver import WpeSession

# xterm-256color terminfo sequences (verified with curses.tigetstr)
CTRL_F9 = "\x1b[20;5~"   # kf33 -> Run
F9      = "\x1b[20~"     # kf9  -> Make
ALT_F5  = "\x1b[15;3~"   # kf53 -> User Screen

# 6*7 = 42 is computed at runtime, so "XQZ42" exists ONLY in the program's
# output -- never in the source the editor displays.
PROG = ('#include <stdio.h>\n'
        'int main(){ printf("XQZ%d\\n", 6 * 7); return 0; }\n')
RAN = "XQZ42"


def _ran(w):
    return any(RAN in line for line in w.display())


def test_ctrl_f9_runs_the_program(tmp_path):
    """Ctrl-F9 (KEY_F(33)) compiles and runs -- its computed output appears."""
    with WpeSession(str(tmp_path), PROG, filename="t.c") as w:
        time.sleep(0.6)
        w.key(CTRL_F9)
        time.sleep(3.0)
        assert w.alive(), "wpe died on Ctrl-F9"
        assert _ran(w), "Ctrl-F9 did not run the program:\n%s" % "\n".join(w.display())


def test_run_via_alt_u_with_run_menu_open(tmp_path):
    """Alt-R opens the Run menu; Alt-U then runs -- the advertised "Alt U"
    shortcut works with the menu open, via the e_prog_switch fallback (the
    item's hotkey is 'R', so the plain-hotkey match cannot catch Alt-U)."""
    with WpeSession(str(tmp_path), PROG, filename="t.c") as w:
        time.sleep(0.6)
        w.key("\033r")                  # Alt-R: open the Run menu
        time.sleep(0.6)
        w.key("\033u")                  # Alt-U: Run (advertised, hotkey != 'U')
        time.sleep(3.0)
        assert w.alive(), "wpe died on Alt-R/Alt-U"
        assert _ran(w), "Alt-R then Alt-U did not run the program:\n%s" \
            % "\n".join(w.display())


def test_alt_hotkey_matches_item_with_menu_open(tmp_path):
    """Alt+<item hotkey> selects the item with its menu open: Search -> Alt-G
    opens Go to Line (hotkey 'G' == advertised Alt-G, the plain-match path)."""
    with WpeSession(str(tmp_path), "l1\nl2\nl3\n") as w:
        time.sleep(0.5)
        w.key("\033s")                  # Alt-S: open Search
        time.sleep(0.5)
        w.key("\033g")                  # Alt-G with the menu open
        time.sleep(0.5)
        disp = "\n".join(w.display())
        assert "oto Line" in disp, \
            "Alt-G with the Search menu open did not open Go to Line:\n%s" % disp
        w.key("\x1b")


def test_alt_f5_shows_user_screen(tmp_path):
    """Alt-F5 (KEY_F(53)) drops to the Borland User Screen (its prompt shows)."""
    with WpeSession(str(tmp_path), PROG, filename="t.c") as w:
        time.sleep(0.6)
        w.key(CTRL_F9)                  # compile + run -> output captured
        time.sleep(3.0)
        w.key(ALT_F5)                   # User Screen
        time.sleep(0.6)
        disp = "\n".join(w.display())
        assert w.alive(), "wpe died on Alt-F5"
        assert "press any key to return" in disp.lower(), \
            "Alt-F5 did not show the User Screen prompt:\n%s" % disp
        w.key("\r")
