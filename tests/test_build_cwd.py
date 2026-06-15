"""Build/run actions execute in the file's directory, not wpe's launch CWD.

These pin the whole family of the macOS-found "Execute Make ran in the launch
directory" bug.  xwpe historically never chdir()'d, so a forked compiler /
program / script inherited the directory the shell launched wpe from; opening a
file elsewhere (`cd /x; wpe sub/main.c`) then built/ran it in the wrong place.
Now compile, make-link, run and run-.sh all chdir to the active file's directory
(e_build_pushd / e_build_dir in we_prog.c).

Each test launches wpe from a PARENT directory with the source in a sub/ dir and
asserts the build artifact / runtime side effect lands in sub/, never in the
launch directory.  The assertions are on the filesystem, so they are exact and
deterministic (no screen scraping).  Execute-Make is covered separately in
tests/test_make_output.py.
"""
import shutil

import pytest
from wpe_driver import WpeSession

ALT_C = "\033c"           # Compile
CTRL_F9 = "\033[20;5~"    # Run (compile + link + run)

requires_gcc = pytest.mark.skipif(shutil.which("gcc") is None,
                                  reason="gcc not installed")


@requires_gcc
def test_compile_writes_object_in_file_dir(tmp_path):
    """Alt-C writes the object file next to the source, not in the launch dir."""
    sub = tmp_path / "sub"
    sub.mkdir()
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    filename="sub/t.c", wait=1.5) as w:
        w.key(ALT_C, delay=3.0)
        w.display()
    assert (sub / "t.o").exists(), \
        "compile should write t.o next to the source (sub/)"
    assert not (tmp_path / "t.o").exists(), \
        "compile leaked t.o into wpe's launch directory"


@requires_gcc
def test_run_executes_in_file_dir(tmp_path):
    """Ctrl-F9 runs the program with the file's dir as CWD (it writes there)."""
    sub = tmp_path / "sub"
    sub.mkdir()
    prog = ('#include <stdio.h>\n'
            'int main(void){FILE*f=fopen("ran_here.txt","w");'
            'if(f){fputs("X",f);fclose(f);}return 0;}\n')
    with WpeSession(str(tmp_path), prog, filename="sub/r.c", wait=1.5) as w:
        w.key(CTRL_F9, delay=4.0)
        w.display()
    assert (sub / "ran_here.txt").exists(), \
        "the program should run in sub/ and write its file there"
    assert not (tmp_path / "ran_here.txt").exists(), \
        "the program ran in the launch dir, not the file's dir"


def test_run_sh_executes_in_file_dir(tmp_path):
    """Ctrl-F9 on a .sh runs it (as ./name.sh) in the file's directory."""
    sub = tmp_path / "sub"
    sub.mkdir()
    with WpeSession(str(tmp_path), "#!/bin/sh\ntouch sh_ran_here\n",
                    filename="sub/s.sh", wait=1.5) as w:
        w.key(CTRL_F9, delay=3.0)
        w.display()
    assert (sub / "sh_ran_here").exists(), \
        "the .sh should run in sub/ (./s.sh + chdir)"
    assert not (tmp_path / "sh_ran_here").exists(), \
        "the .sh ran in the launch dir, not the file's dir"
