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


def test_execute_make_runs_in_file_dir(tmp_path):
    """Make runs in the active file's directory, not wpe's launch CWD.

    macOS repro: `cd xwpe-root; ./wpe docs/examples/c-lsp/main.cpp; Alt-A` used
    to build xwpe's own Makefile (the inherited CWD) instead of the example's.
    We put a DECOY Makefile in the launch dir and the real one in the file's
    subdir; only the subdir's marker may appear.
    """
    (tmp_path / "Makefile").write_text("all:\n\t@echo WRONG_DIR_MARKER\n")
    sub = tmp_path / "sub"
    sub.mkdir()
    (sub / "Makefile").write_text("all:\n\t@echo RIGHT_DIR_MARKER\n")
    # workdir (= launch CWD = HOME) is tmp_path; the file lives in sub/.
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    filename="sub/t.c", wait=1.5) as w:
        w.key(ALT_A, delay=3.0)
        screen = "\n".join(w.display())
    assert "RIGHT_DIR_MARKER" in screen, \
        "make should run in the file's dir (sub/), got:\n" + screen
    assert "WRONG_DIR_MARKER" not in screen, \
        "make ran in the launch CWD, not the file's dir:\n" + screen
