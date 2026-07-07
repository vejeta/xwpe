"""ncurses: resizing the terminal re-lays-out without crashing or losing Messages.

The terminal resize path -- SIGWINCH -> resizeterm -> KEY_RESIZE ->
e_relayout_windows -- had NO test coverage, and that is exactly where the
Messages-window layout regression hid for weeks (nobody drove a resize).  This
opens the Messages window (Execute Make), sweeps the terminal size down and back
up several times, and asserts xwpe stays alive throughout and the Messages window
is still present afterwards.

Scope, stated honestly: this reliably guards the SIGWINCH resize path against
crashes/hangs and against the Messages window disappearing outright.  The precise
pixel docking of the split (Messages flush under the editor) is verified visually
and by the shared e_relayout_windows re-dock the X11/Wayland resize tests also
exercise -- it is not asserted here, because a resized ncurses screen is not
captured reliably enough through a pty for a pixel-exact split assertion.
"""
from wpe_driver import WpeSession

ALT_A = "\033a"   # Execute Make (programming mode) -> opens Messages
MARK = "MSG_RESIZE_MARKER_OK"


def test_ncurses_resize_keeps_messages(tmp_path):
    (tmp_path / "Makefile").write_text("all:\n\t@echo %s\n" % MARK)
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    filename="t.c", wait=1.5) as w:
        w.key(ALT_A, delay=3.0)
        assert "Messages" in "\n".join(w.display()), "Messages window did not open"

        # A few shrink/grow cycles through modest sizes, driving KEY_RESIZE each
        # time.  xwpe must never die, and Messages must not vanish.
        for cols, rows in [(60, 16), (95, 34), (55, 14), (100, 36), (72, 20)]:
            w.resize(cols, rows, delay=0.8)
            assert w.alive(), "xwpe died on resize to %dx%d" % (cols, rows)

        screen = "\n".join(w.display())
    assert "Messages" in screen, \
        "Messages window vanished after a resize sweep; screen:\n" + screen
