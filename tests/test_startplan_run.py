"""Run, Make, Execute and the language servers start from a start plan.

A start-folder xwperc can set the Arguments that Run (Ctrl-F9) and Execute
Make (Alt-A) hand to the program and to make.  The original spliced them,
and the program's and the file's names, into a shell line, so a folder's
"; touch ran" ran, and so did a file named "a;touch pwned;.c" at Ctrl-F9 or
at the file manager's Execute.  Now the program and every file name are
words of the plan, handed to the shell as "$1", "$2" ..., a folder's
Arguments are words too, and the box asks once per folder before they are
used.  A language server that runs the folder's build code when a file is
opened (rust-analyzer) asks once per folder; one that only reads the files
(clangd) starts without a question.

The servers here are stubs on PATH that only note they were started: the
real ones would build the folder.
"""
import os
import stat

import pytest
from wpe_driver import ALT, WpeSession, wait_screen, tool_usable
from test_startplan import Folder, _screen, _wait

CTRL_F9 = "\033[20;5~"     # Run: build, then run
ALT_A = "\033a"            # Execute Make
ALT_Q = "\033q"            # the language server prefix
ALT_E = "\033e"            # the file manager's Execute button
ESC = "\033"
RUN_IT = "\033r"           # Alt-R: Run it
ENTER = "\r"

BOX_WAIT = 12              # the box is up by then
RUN_WAIT = 40              # gcc has built and run a small program by then
SETTLE = 1.5               # whatever an answer leads to has started by then
SERVER_WAIT = 8            # a stub server has started by then

# A program that writes each argument it got, bracketed, to args.out.
ARGS_PROGRAM = r'''#include <stdio.h>
int main(int argc, char **argv)
{
  FILE *f = fopen("args.out", "w");
  int i;
  for (i = 1; i < argc; i++)
    fprintf(f, "[%s]", argv[i]);
  fclose(f);
  return 0;
}
'''

needs_gcc = pytest.mark.skipif(not tool_usable("gcc", "--version"), reason="gcc not usable")
needs_make = pytest.mark.skipif(not tool_usable("make", "--version"), reason="make not usable")

# A folder's Arguments whose tab the shell splits and make's word cut keeps.
FOLDER_ARGS = ";touch\tran"


def _args_out(folder):
    path = folder.ws / "args.out"
    try:
        return path.read_text()
    except OSError:
        return None


# @scenario SC-compile-30
@needs_gcc
def test_startplan_run_arguments_from_the_folder_ask(tmp_path):
    """The folder's xwperc sets Arguments to ";touch<TAB>ran": Ctrl-F9 shows
    them and that Run runs the folder's own program, and Esc runs nothing
    (the original handed them to the shell, which ran "touch ran")."""
    folder = Folder(tmp_path)
    folder.xwperc("[Programming]\nArguments : %s\n" % FOLDER_ARGS)
    with folder.session(seed=ARGS_PROGRAM) as s:
        s.key(CTRL_F9, delay=0.8)
        asked = wait_screen(s, "Command from ./xwperc", tmo=RUN_WAIT)
        box = _screen(s)
        s.key(ESC, delay=0.8)
        s._drain(SETTLE)
    lines = [l.split("startplan line ", 1)[1] for l in folder.traced().splitlines()
             if "startplan line " in l]
    assert asked, "no question before Run used the folder's Arguments:\n" + box
    assert any(l.startswith("From ./xwperc: Arguments = ;touch") for l in lines), lines
    assert "This runs the folder's own program." in lines, lines
    assert not (folder.ws / "ran").exists(), "the folder's Arguments ran as shell text"
    assert _args_out(folder) is None, "the program ran after Esc"


# @scenario SC-compile-30
@needs_gcc
def test_startplan_run_arguments_reach_the_program_as_words(tmp_path):
    """After Run it the folder's Arguments reach the program as its argument,
    the tab inside it as it is, and the shell runs nothing of them."""
    folder = Folder(tmp_path)
    folder.xwperc("[Programming]\nArguments : %s\n" % FOLDER_ARGS)
    with folder.session(seed=ARGS_PROGRAM) as s:
        s.key(CTRL_F9, delay=0.8)
        assert wait_screen(s, "Command from ./xwperc", tmo=RUN_WAIT), _screen(s)
        s.key(RUN_IT, delay=0.8)
        ran = _wait(lambda: _args_out(folder) is not None, RUN_WAIT)
        s._drain(SETTLE)
    assert ran, "the program did not run after Run it"
    assert _args_out(folder) == "[%s]" % FOLDER_ARGS, _args_out(folder)
    assert not (folder.ws / "ran").exists(), "the folder's Arguments ran as shell text"


# @scenario SC-compile-30
@needs_gcc
def test_startplan_run_users_own_arguments_stay_his_shell_text(tmp_path):
    """The user's own Arguments are his shell text, as they always were:
    no question, and a quoted pair of words reaches the program as one."""
    folder = Folder(tmp_path)
    folder.own_config("[Programming]\nArguments : 'one two' three\n")
    with folder.session(seed=ARGS_PROGRAM) as s:
        s.key(CTRL_F9, delay=0.8)
        ran = _wait(lambda: _args_out(folder) is not None, RUN_WAIT)
        screen = _screen(s)
        s._drain(SETTLE)
    assert ran, "the program did not run:\n" + screen
    assert "Command from" not in screen, screen
    assert _args_out(folder) == "[one two][three]", _args_out(folder)


SEMI_NAME = "a;touch pwned;.c"
QUOTE_NAME = "it's.c"


# @scenario SC-compile-30
@needs_gcc
def test_startplan_run_file_name_is_not_shell(tmp_path):
    """Ctrl-F9 on a file named "a;touch pwned;.c": the program built from it
    runs, and nothing in its name does (the original's shell line ran
    "touch pwned")."""
    folder = Folder(tmp_path)
    with folder.session(filename=SEMI_NAME, seed=ARGS_PROGRAM) as s:
        s.key(CTRL_F9, delay=0.8)
        ran = _wait(lambda: _args_out(folder) is not None, RUN_WAIT)
        screen = _screen(s)
        s._drain(SETTLE)
    assert not (folder.ws / "pwned").exists(), "a file name ran as shell text"
    assert ran, "the program built from the file did not run:\n" + screen


# @scenario SC-compile-30
@needs_gcc
def test_startplan_run_file_name_with_a_quote_runs(tmp_path):
    """Ctrl-F9 on "it's.c" runs the program (the original's shell line had an
    unterminated quote and ran nothing)."""
    folder = Folder(tmp_path)
    with folder.session(filename=QUOTE_NAME, seed=ARGS_PROGRAM) as s:
        s.key(CTRL_F9, delay=0.8)
        ran = _wait(lambda: _args_out(folder) is not None, RUN_WAIT)
        screen = _screen(s)
        s._drain(SETTLE)
    assert ran, "the program built from the file did not run:\n" + screen


EXEC_NAME = "a;touch pwned;x"
EXEC_SCRIPT = "#!/bin/sh\necho ran > executed.out\n"
NAME_BOX_MAX = 79          # the file manager's name box takes this many bytes


def _executable(path, text):
    path.write_text(text)
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


# @scenario SC-compile-30
def test_startplan_execute_file_name_is_not_shell(tmp_path):
    """File > Execute on a script named "a;touch pwned;x": the script runs,
    and nothing in its name does (the original handed the name to the
    shell, which ran "touch pwned")."""
    folder = Folder(tmp_path)
    _executable(folder.ws / EXEC_NAME, EXEC_SCRIPT)
    with folder.session() as s:
        s.key(ALT.FILE, delay=0.6)
        s.key("e", delay=1.0)
        s.key(ENTER, delay=0.8)            # the pattern: the file list
        for _ in range(4):
            if EXEC_NAME in _screen(s):
                break
            s._drain(0.5)
        s.key(ALT_E, delay=1.5)            # the Execute button
        done = _wait(lambda: (folder.ws / "executed.out").exists(), RUN_WAIT)
        s.key(ENTER, delay=0.8)            # "Hit CR"
        s._drain(SETTLE)
    assert not (folder.ws / "pwned").exists(), "a file name ran as shell text"
    assert done, "the selected file did not run"


# @scenario SC-compile-30
def test_startplan_execute_absolute_name_runs_as_typed(tmp_path):
    """File > Execute with the whole path typed in the name box ("/dir/tool"):
    that program runs.  The file manager splits a typed name at its last
    "/" into a folder and a file name, so the name Execute gets is a bare
    one and the row passes on the tree before the fix too; the rule that a
    whole path is kept as typed is held by the owner's C case
    (case_operand_in of tests/test_startplan.c)."""
    folder = Folder(tmp_path)
    tool = folder.ws / "tool"
    if len(str(tool)) > NAME_BOX_MAX:
        pytest.skip("the test folder's path is longer than the name box takes")
    _executable(tool, EXEC_SCRIPT)
    with folder.session() as s:
        s.key(ALT.FILE, delay=0.6)
        s.key("e", delay=1.0)
        s.key(str(tool), delay=0.8)        # the name box: the whole path
        s.key(ENTER, delay=1.5)            # the name is taken
        screen = _screen(s)
        s.key(ALT_E, delay=1.5)            # the Execute button
        done = _wait(lambda: (folder.ws / "executed.out").exists(), RUN_WAIT)
        s.key(ENTER, delay=0.8)            # "Hit CR"
        s._drain(SETTLE)
    assert done, "the program typed with its whole path did not run:\n" + screen


MAKEFILE = "all:\n\t@echo made > made.out\n"
MAKE_ARGS = "--eval=x:=$(shell\ttouch\tran)"


# @scenario SC-compile-30
@needs_make
def test_startplan_make_arguments_from_the_folder_ask(tmp_path):
    """The folder's xwperc hands make an --eval that would touch a file:
    Alt-A shows it and that make runs the folder's own Makefile, and Esc
    runs nothing (the original ran make with it unasked)."""
    folder = Folder(tmp_path)
    (folder.ws / "Makefile").write_text(MAKEFILE)
    folder.xwperc("[Programming]\nArguments : %s\n" % MAKE_ARGS)
    with folder.session() as s:
        s.key(ALT_A, delay=0.8)
        asked = wait_screen(s, "Command from ./xwperc", tmo=BOX_WAIT)
        box = _screen(s)
        s.key(ESC, delay=0.8)
        s._drain(SETTLE + 2)
    lines = [l.split("startplan line ", 1)[1] for l in folder.traced().splitlines()
             if "startplan line " in l]
    assert asked, "no question before make got the folder's Arguments:\n" + box
    assert any(l.startswith("From ./xwperc: Arguments = --eval=") for l in lines), lines
    assert "This runs the folder's own Makefile." in lines, lines
    assert not (folder.ws / "ran").exists(), "make ran the folder's --eval after Esc"
    assert not (folder.ws / "made.out").exists(), "make ran after Esc"


# @scenario SC-compile-30
@needs_make
def test_startplan_make_with_the_users_own_arguments_is_silent(tmp_path):
    """make with no folder value runs at once, as always."""
    folder = Folder(tmp_path)
    (folder.ws / "Makefile").write_text(MAKEFILE)
    with folder.session() as s:
        s.key(ALT_A, delay=0.8)
        done = _wait(lambda: (folder.ws / "made.out").exists(), RUN_WAIT)
        screen = _screen(s)
        s._drain(SETTLE)
    assert done, "make did not run:\n" + screen
    assert "Command from" not in screen, screen


INSTALL_PROJECT = ("CMP=\tgcc\nCMPFLAGS=\t-g\nEXENAME=\tdemo\nFILES=\tmain.c\n"
                   "\ninstall:\n\ttouch installed.one\n\ttouch installed.two\n")
ALT_L = "\033l"            # Alt-L: build, then install


def _open_project(s):
    """Project > Open project, the Files box, the one .prj of the folder."""
    s.key(ALT.PROJECT, "p", delay=0.6)
    s.key(ENTER, delay=0.6)
    s.key(ENTER, delay=0.8)


# @scenario SC-compile-30
@needs_gcc
def test_startplan_install_lines_ask_once_and_all_run(tmp_path):
    """A project's two install lines are one question; after Run it both
    run, and the next Alt-L runs them without asking."""
    folder = Folder(tmp_path)
    (folder.ws / "demo.prj").write_text(INSTALL_PROJECT)
    one, two = folder.ws / "installed.one", folder.ws / "installed.two"
    with folder.session(filename="main.c") as s:
        _open_project(s)
        s.key(ALT_L, delay=0.8)
        assert wait_screen(s, "Command from demo.prj", tmo=RUN_WAIT), _screen(s)
        s.key(RUN_IT, delay=0.8)
        ran = _wait(lambda: one.exists() and two.exists(), RUN_WAIT)
        s._drain(SETTLE)
    assert ran, "the install lines did not both run after Run it"
    one.unlink()
    two.unlink()
    with folder.session(filename="main.c") as s:
        _open_project(s)
        s.key(ALT_L, delay=0.8)
        again = _wait(lambda: one.exists() and two.exists(), RUN_WAIT)
        screen = _screen(s)
        s._drain(SETTLE)
    assert again, "the remembered install lines did not run:\n" + screen
    assert "Command from" not in screen, screen


def _stub_server(folder, name):
    """A server named @name on PATH that notes each start in started.<name>."""
    bindir = folder.tmp / "bin"
    bindir.mkdir(exist_ok=True)
    marker = folder.tmp / ("started." + name)
    _executable(bindir / name, "#!/bin/sh\necho started >> '%s'\nexec cat >/dev/null\n" % marker)
    return marker, {"PATH": "%s:%s" % (bindir, os.environ.get("PATH", ""))}


def _starts(marker):
    try:
        return len(marker.read_text().splitlines())
    except OSError:
        return 0


RUST_SEED = "fn main() {}\n"


# @scenario SC-compile-30
def test_startplan_language_server_that_builds_asks_once(tmp_path):
    """A Cargo folder: Alt-Q E shows that rust-analyzer runs the folder's
    build scripts, Esc starts nothing; Run it starts it and is kept, so the
    next session starts it without a question."""
    folder = Folder(tmp_path)
    (folder.ws / "Cargo.toml").write_text('[package]\nname = "x"\nversion = "0.1.0"\n')
    marker, env = _stub_server(folder, "rust-analyzer")
    with folder.session(filename="main.rs", seed=RUST_SEED, env=env) as s:
        s.key(ALT_Q, delay=0.4)
        s.key("e", delay=0.8)
        asked = wait_screen(s, "Command from this folder", tmo=BOX_WAIT)
        box = _screen(s)
        s.key(ESC, delay=0.8)
        s._drain(SETTLE)
    assert asked, "no question before rust-analyzer ran the folder's build code:\n" + box
    assert "build scripts" in box, box
    assert _starts(marker) == 0, "the server started after Esc"
    with folder.session(filename="main.rs", seed=RUST_SEED, env=env) as s:
        s.key(ALT_Q, delay=0.4)
        s.key("e", delay=0.8)
        assert wait_screen(s, "Command from this folder", tmo=BOX_WAIT), _screen(s)
        s.key(RUN_IT, delay=0.8)
        started = _wait(lambda: _starts(marker) == 1, SERVER_WAIT)
        s._drain(SETTLE)
    assert started, "the server did not start after Run it"
    with folder.session(filename="main.rs", seed=RUST_SEED, env=env) as s:
        s.key(ALT_Q, delay=0.4)
        s.key("e", delay=0.8)
        again = _wait(lambda: _starts(marker) == 2, SERVER_WAIT)
        screen = _screen(s)
        s._drain(SETTLE)
    assert again, "the remembered server did not start:\n" + screen
    assert "Command from" not in screen, screen


# @scenario SC-compile-30
def test_startplan_server_rows_answer_the_box_once(tmp_path):
    """The rows that test a real server's features open their session with
    answer_server_start: the box comes up during a long wait and is
    answered at once, and the server starts."""
    folder = Folder(tmp_path)
    marker, env = _stub_server(folder, "rust-analyzer")
    env.update(HOME=str(folder.home))
    with WpeSession(str(folder.ws), RUST_SEED, filename="main.rs", env_extra=env,
                    answer_server_start=True) as s:
        s.key(ALT_Q, delay=0.4)
        s.key("e", delay=SERVER_WAIT)
        started = _starts(marker)
        s._drain(SETTLE)
    assert started == 1, "the box was not answered, or answered twice"


# @scenario SC-compile-30
def test_startplan_language_server_that_reads_starts_silently(tmp_path):
    """clangd only reads the files: Alt-Q E starts it without a question."""
    folder = Folder(tmp_path)
    marker, env = _stub_server(folder, "clangd")
    with folder.session(env=env) as s:
        s.key(ALT_Q, delay=0.4)
        s.key("e", delay=0.8)
        started = _wait(lambda: _starts(marker) == 1, SERVER_WAIT)
        screen = _screen(s)
        s._drain(SETTLE)
    assert started, "clangd did not start:\n" + screen
    assert "Command from" not in screen, screen
