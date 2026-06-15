"""Run -> Execute Make (Alt-A) shows make's real STDOUT in Messages.

This is the macOS-found bug (latent on Linux): the old temp-file + wait-then-read
capture lost make's block-buffered stdout, so Messages showed only the echoed
"make" command.  We now drain stdout+stderr with poll() while make runs, so its
output appears.  A unique marker printed by the Makefile makes the assert exact.
"""
from wpe_driver import WpeSession

ALT_A = "\033a"   # Execute Make (programming mode)


def test_execute_make_shows_stdout(tmp_path):
    (tmp_path / "Makefile").write_text("all:\n\t@echo MAKE_STDOUT_MARKER_OK\n")
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    filename="t.c", wait=1.5) as w:
        w.key(ALT_A, delay=3.0)        # Alt-A -> make runs, output drained live
        screen = "\n".join(w.display())
    assert "MAKE_STDOUT_MARKER_OK" in screen, \
        "Execute Make should show make's stdout in Messages, got:\n" + screen
