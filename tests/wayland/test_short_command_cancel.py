"""Native-Wayland twin of tests/x11/test_short_command_cancel.py.

The test body is the X11 module's, run here under this directory's
native-wl_surface `xwpe` fixture (conftest.py).  Single source of truth -- see
run_x11_module."""
from conftest import run_x11_module

run_x11_module(__name__, "test_short_command_cancel")


# @scenario SC-keymap-19
def test_ctrl_bracket_ends_a_short_command_the_editor_waits_for(slow_man, xwpe):
    """Ctrl+[ is Esc.  The compositor reports it as a key of its own, and the
    wait for a short command takes the key the editor reads after the key
    table's fold, so it ends the command as Esc does.  Wayland only: the X11
    wait looks for the Escape keysym."""
    xwpe.menu("h", "t", delay=0.2)
    pid_file = slow_man / "man.pid"
    assert _wait(pid_file.exists, WAIT_STARTS_S), "help never ran man"
    pid = int(pid_file.read_text().strip())
    assert _alive(pid), "man ended by itself: the row proves nothing"
    xwpe.key("ctrl+bracketleft", delay=0.2)
    assert _wait(lambda: not _alive(pid), ENDED_S), \
        "Ctrl+[ should end the command the editor waits for"
    assert xwpe.proc.poll() is None, "xwpe died ending the command"
