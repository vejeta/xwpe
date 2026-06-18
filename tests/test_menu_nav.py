"""Menu-to-menu navigation regression tests for wpe (ncurses, pyte-driven).

User-visible bug they cover (reported against xwpe X11): with one top-level
menu already open, pressing Alt+<letter> for a DIFFERENT menu must SWITCH to
that menu, not stay on the current one and not close the menu bar.  Examples
that previously misbehaved on XQuartz (xwpe, not pyte/wpe):
  - Window menu open + Alt+E -> stays in Window (Edit never opens)
  - Any menu  open + Alt+B -> menu closes (Block never opens)
The terminal binary has always handled these transitions correctly; this file
pins that, so a future change to the menu loop (we_menue.c's
WpeHandleSubmenu_run / WpeHandleMainmenu) cannot regress the path silently.
The corresponding X11 coverage lives in tests/x11/test_menu_nav.py.

Detection method: each menu has a UNIQUE submenu-item label (e.g. "Cut" only
appears in the Edit dropdown, "Mark" only in Block, etc.).  Open menu A, send
Alt+<letter> for B, then scan the pyte display for B's tell and assert A's
tell is gone.  This is a tighter check than a generic "screen changed" diff:
it rules out closed-menu false positives.
"""
from wpe_driver import WpeSession, ALT


# Distinctive submenu items used to identify which top-level menu is open.
TELL = {
    "system":  "About",           # # menu first item
    "file":    "File-Manager",    # File menu first item
    "edit":    "Cut",             # Edit menu first item ("Cut  Shift Del / ^X")
    "search":  "Find",            # Search menu first item
    "block":   "Mark",            # Block menu items begin with "Mark ..."
    "window":  "Size/Move",       # Window menu first item
    "help":    "Editor",          # Help menu first item ("Editor  F1")
}

# Alt-key sequence per top-level menu.
ALT_SEQ = {
    "system":  ALT.SYSTEM,
    "file":    ALT.FILE,
    "edit":    ALT.EDIT,
    "search":  ALT.SEARCH,
    "block":   ALT.BLOCK,
    "window":  ALT.WINDOW,
    "help":    ALT.HELP,
}


def _open_then_switch(tmp_path, opener, target):
    """Open the `opener` menu, switch via Alt+<target's letter>, return display."""
    w = WpeSession(str(tmp_path), "int main(){\n  return 0;\n}\n")
    try:
        w.key(ALT_SEQ[opener], delay=0.5)
        w.key(ALT_SEQ[target], delay=0.5)
        return "\n".join(w.display(drain=0.5)), w.alive()
    finally:
        w.close()


def _assert_switched(disp, alive, opener, target):
    """Target's tell must be on screen; opener's tell must NOT (it would mean
    the open menu never changed)."""
    assert alive, "wpe died switching from %s to %s" % (opener, target)
    o_tell, t_tell = TELL[opener], TELL[target]
    assert t_tell in disp, \
        ("Alt+%s with %s menu open should switch to %s "
         "(target tell %r missing).\nScreen:\n%s"
         % (target[0].upper(), opener, target, t_tell, disp))
    assert o_tell not in disp, \
        ("Alt+%s with %s menu open should switch AWAY from %s "
         "(opener tell %r still on screen).\nScreen:\n%s"
         % (target[0].upper(), opener, opener, o_tell, disp))


# --- the originally-reported failing cases ---------------------------------

def test_window_then_alt_e_opens_edit(tmp_path):
    disp, alive = _open_then_switch(tmp_path, "window", "edit")
    _assert_switched(disp, alive, "window", "edit")


def test_system_then_alt_e_opens_edit(tmp_path):
    disp, alive = _open_then_switch(tmp_path, "system", "edit")
    _assert_switched(disp, alive, "system", "edit")


def test_window_then_alt_b_opens_block(tmp_path):
    disp, alive = _open_then_switch(tmp_path, "window", "block")
    _assert_switched(disp, alive, "window", "block")


def test_edit_then_alt_b_opens_block(tmp_path):
    disp, alive = _open_then_switch(tmp_path, "edit", "block")
    _assert_switched(disp, alive, "edit", "block")


def test_file_then_alt_b_opens_block(tmp_path):
    disp, alive = _open_then_switch(tmp_path, "file", "block")
    _assert_switched(disp, alive, "file", "block")


# --- broader cross-menu coverage so a future regression cannot slip through -

def test_edit_then_alt_w_opens_window(tmp_path):
    disp, alive = _open_then_switch(tmp_path, "edit", "window")
    _assert_switched(disp, alive, "edit", "window")


def test_block_then_alt_e_opens_edit(tmp_path):
    disp, alive = _open_then_switch(tmp_path, "block", "edit")
    _assert_switched(disp, alive, "block", "edit")


def test_window_then_alt_f_opens_file(tmp_path):
    disp, alive = _open_then_switch(tmp_path, "window", "file")
    _assert_switched(disp, alive, "window", "file")


def test_window_then_alt_s_opens_search(tmp_path):
    disp, alive = _open_then_switch(tmp_path, "window", "search")
    _assert_switched(disp, alive, "window", "search")


def test_help_then_alt_e_opens_edit(tmp_path):
    disp, alive = _open_then_switch(tmp_path, "help", "edit")
    _assert_switched(disp, alive, "help", "edit")
