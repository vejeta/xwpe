"""Dragging a modal dialog by its title bar must never crash.

The option-dialog window mover (e_opt_eck_mouse) kept one saved-image "view"
and re-stamped it at each new position while dragging.  e_close_view() frees
the view, so the second drag-motion wrote through a freed PIC and the release
freed it a second time -- a heap use-after-free that segfaulted xwpe when the
user grabbed the AI-settings dialog's title bar and moved it.  This drives that
exact gesture (SGR mouse press on the title bar, several motions, release) and
asserts the editor is still alive afterwards.
"""
import os
import tempfile

from wpe_driver import WpeSession


def test_ai_dialog_title_drag_does_not_crash(tmp_path):
    home = tempfile.mkdtemp()
    os.makedirs(os.path.join(home, ".config", "xwpe"), exist_ok=True)
    env = {"XWPE_AI_ENABLE": "1", "HOME": home}

    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key("\033o", delay=0.5)          # Options menu
        s.key("i", delay=0.7)              # AI settings dialog
        # The dialog's title bar is 0-based row 1 (SGR 1-based row 2), columns
        # ~7..71.  Column 30 (0-based) -> SGR col 31, clear of the close boxes.
        # SGR press of button 1 on the title bar:
        s.key("\033[<0;31;2M", delay=0.3)
        # Drag downward: button-1 motion reports (button code 32 = 0x20 motion
        # flag).  Two or more registered motions are what triggered the
        # use-after-free (free on the first, dereference on the next).
        s.key("\033[<32;31;3M", delay=0.25)
        s.key("\033[<32;31;4M", delay=0.25)
        s.key("\033[<32;32;5M", delay=0.25)
        # Release button 1 (lowercase final byte).
        s.key("\033[<0;32;5m", delay=0.4)

        assert s.proc.poll() is None, (
            "xwpe exited (crash) while dragging the dialog title bar; "
            "poll()=%r" % s.proc.poll())

        # Still responsive: Esc should close the dialog, leaving the editor up.
        s.key("\033", delay=0.4)
        assert s.proc.poll() is None, "xwpe exited after the drag+Esc"
