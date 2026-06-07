"""Run-menu (Alt-R) tests for xwpe (X11/Xft) -- the X11 twin of the wpe Run
coverage (tests/test_compile.py).

Make (F9) compiles the current file; we check for the compiled output on disk
(ground truth) and that the Messages window appears (screenshot diff).
"""
import os
import time

from conftest import changed_pixels


def wait_for_file(path, proc, timeout=25.0, interval=0.25):
    """Poll until `path` appears, the process dies, or `timeout` elapses.

    Run compiles, links and executes, so the executable lands on disk only
    after a multi-stage build driven by the GUI event loop.  On a loaded CI
    runner under Xvfb that takes noticeably longer than on a developer box, so
    we poll for the artifact instead of sleeping a fixed amount and checking
    once (which raced on Salsa CI).  Returns True as soon as the file exists.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        assert proc.poll() is None, "xwpe died before producing %s" % path
        if os.path.exists(path):
            return True
        time.sleep(interval)
    return False


def test_make_compiles_the_file(xwpe):
    """Make (F9) compiles t.c -- compiled output appears and Messages opens."""
    d = os.path.dirname(xwpe.srcfile)
    before = xwpe.screenshot()
    xwpe.key("F9")                       # Run -> Make
    time.sleep(2.5)                      # gcc + relayout
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died during Make (F9)"
    produced = [f for f in os.listdir(d) if f not in ("t.c",)]
    assert produced, \
        "Make should produce compiled output (.o/.e/exe), dir=%r" % os.listdir(d)
    assert changed_pixels(before, after) > 2000, \
        "Make should open the Messages window (screen barely changed)"


# --- shortcut path (#159): Run accelerators in the X11 input path ---
# The Run item's hotkey is 'R', but the advertised accelerators are ^F9 and
# Alt-U, dispatched via e_prog_switch.  Alt-U was a no-op until that fallback
# was wired; this guards it in the separate X11 (we_xterm.c) keyboard path.
# Run compiles+links+runs, so the executable t.e appears on disk.

def test_run_via_alt_u(xwpe):
    """Alt-U (advertised Run accelerator, hotkey != 'U') compiles+links+runs.

    This is the accelerator that was a no-op until the e_prog_switch fallback
    was wired -- guarded here in the separate X11 (we_xterm.c) keyboard path."""
    d = os.path.dirname(xwpe.srcfile)
    xwpe.key("alt+u")                    # Run
    assert wait_for_file(os.path.join(d, "t.e"), xwpe.proc), \
        "Alt-U should build+run (t.e on disk within timeout), dir=%r" \
        % os.listdir(d)


def test_run_via_ctrl_f9(xwpe):
    """Ctrl-F9 (the primary advertised Run accelerator) compiles+links+runs.

    Decoded in we_xterm.c (Control+XK_F9 -> CF9).  The combo is injected by
    keycode (ctrl+<F9 keycode>) so it survives the xdotool resolver bug -- see
    the README "Function keys are injected by keycode" note."""
    d = os.path.dirname(xwpe.srcfile)
    xwpe.key("ctrl+F9")                  # Run
    assert wait_for_file(os.path.join(d, "t.e"), xwpe.proc), \
        "Ctrl-F9 should build+run (t.e on disk within timeout), dir=%r" \
        % os.listdir(d)
