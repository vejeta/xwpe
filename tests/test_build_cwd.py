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


@requires_gcc
def test_project_make_links_all_sources(tmp_path):
    """A .prj Make (F9) compiles AND links EVERY listed source -- the multi-file
    build that single-file F9 cannot do (this is what the c-lsp example .prj is
    for).  Opened from the project's own directory, the normal workflow."""
    (tmp_path / "util.c").write_text("int helper(void){return 7;}\n")
    (tmp_path / "main.c").write_text(
        "int helper(void);\nint main(void){return helper() - 7;}\n")
    prj = ("#\n# xwpe - project-file: demo.prj\n#\n\n"
           "CMP=\tgcc\nCMPFLAGS=\t-g\nLDFLAGS=\nEXENAME=\tdemo\n"
           "CMPSWTCH=\tgnu\n\nFILES=\tmain.c util.c\n\n")
    with WpeSession(str(tmp_path), prj, filename="demo.prj", wait=2.0) as w:
        w.key("\033[20~", delay=10.0)   # F9 -> project Make (compile + link all)
        w.display()
    assert (tmp_path / "demo").exists(), \
        "project Make should link the executable from ALL sources"
    assert (tmp_path / "main.o").exists() and (tmp_path / "util.o").exists(), \
        "project Make should compile every source in the project"


@requires_gcc
def test_link_failure_hints_about_makefile(tmp_path):
    """When single-file Make fails to LINK a multi-file program and a Makefile is
    present, Messages guides the user to a Project / Execute Make instead of just
    showing a bare 'undefined reference' linker error."""
    (tmp_path / "Makefile").write_text("all:\n\t@echo built\n")
    # references a symbol defined in another translation unit -> link fails.
    main = "int helper(void);\nint main(void){return helper();}\n"
    with WpeSession(str(tmp_path), main, filename="main.c", wait=1.5) as w:
        w.key("\033[20~", delay=4.0)   # F9 -> compile OK, link the single file fails
        screen = "\n".join(w.display())
    assert "multi-file" in screen, \
        "link failure with a Makefile present should hint about it, got:\n" + screen


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
