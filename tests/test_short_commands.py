"""The short commands the editor runs on the children's owner.

Printing, opening a compressed file, help on a word, the Run fallback when
no terminal can be made, and the editor's exit each used to run a shell
command that held the editor and pasted a file name or a word into shell
text.  Now the editor's own words are arguments, the wait is the loop's, and
what a command could not do is an error the user sees.

Every stub, fake lib folder and config is built inside the test's own
folder, which is the editor's HOME: nothing of the machine's is read.
"""
import glob
import gzip
import os
import shutil
import signal
import stat
import subprocess
import sys
import time

import pytest

from wpe_driver import WpeSession, WAIT_SCALE, WPE_BIN, XWPE_LIB_DIR, ALT

F1 = "\033OP"                      # Help
CTRL_F9 = "\033[20;5~"             # Run
POLL_S = 0.2
SAID_S = 15
RUN_END_S = 40
PRINTED_S = 15
PRINT_ANSWER = "y"
HOSTILE_DIR = "lib dir;touch STRAY;x"   # a space and a ';' in a folder's name
HOSTILE_FILE = "pr int;x.c"             # the same in a file's name
STRAY = "STRAY"
LONG_WORD = "x" * 90                    # longer than the editor's 80 bytes
SEED_LINES = 400
ENDED_S = 30


def _screen(w):
    return "\n".join(w.display(drain=0))


def _wait_for(w, predicate, timeout):
    deadline = time.monotonic() + timeout * WAIT_SCALE
    while time.monotonic() < deadline:
        w._drain(POLL_S)
        if predicate():
            return True
    return predicate()


def _read(path):
    try:
        with open(path) as fh:
            return fh.read()
    except OSError:
        return ""


def _write_executable(path, text):
    with open(path, "w") as fh:
        fh.write(text)
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)


def _options(tmp_path, text):
    """The option file the editor reads from its working folder."""
    with open(str(tmp_path / "xwperc"), "w") as fh:
        fh.write(text)


# A print command that records where it ran, the name it was given and the
# file it was given.
PRINT_STUB = '''#!/bin/sh
pwd > "%(dir)s/printed.dir"
printf '%%s' "$1" > "%(dir)s/printed.name"
cp -- "$1" "%(dir)s/printed.out"
'''


def _print_setup(tmp_path):
    stub = str(tmp_path / "printstub")
    _write_executable(stub, PRINT_STUB % {"dir": str(tmp_path)})
    _options(tmp_path, "[General]\nPrintCmd : %s\n" % stub)


def _seed():
    return "".join("line %d of the file\n" % i for i in range(SEED_LINES))


# @scenario SC-debug-104
def test_print_hands_the_whole_file_to_the_users_command(tmp_path):
    """File > Print File runs the user's print command on the file: the
    command gets the file's name as an argument, in the editor's temp folder,
    and the whole file is there (the command was once cut to 7 bytes and
    never ran)."""
    _print_setup(tmp_path)
    seed = _seed()
    with WpeSession(str(tmp_path), seed, filename=HOSTILE_FILE) as w:
        w.menu(ALT.FILE, "p", delay=0.8)
        w.key(PRINT_ANSWER, delay=0.6)
        assert _wait_for(w, lambda: _read(str(tmp_path / "printed.out")) == seed,
                         PRINTED_S), \
            "the print command should get the whole file; it got %d bytes:\n%s" \
            % (len(_read(str(tmp_path / "printed.out"))), _screen(w))
        assert _read(str(tmp_path / "printed.name")) == HOSTILE_FILE
        assert _read(str(tmp_path / "printed.dir")).strip() != str(tmp_path), \
            "the command should run in the editor's temp folder"
        assert w.alive()


# @scenario SC-debug-105
def test_a_compressed_help_file_with_a_hostile_folder_name_opens(tmp_path):
    """Help reads a compressed help file from a library folder whose name has
    a space and a ';': the text appears, and nothing in the name is run."""
    lib = tmp_path / HOSTILE_DIR
    lib.mkdir()
    with open(os.path.join(XWPE_LIB_DIR, "help.xwpe_eng"), "rb") as src:
        with gzip.open(str(lib / "help.xwpe.gz"), "wb") as dst:
            dst.write(src.read())
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra={"XWPE_LIB": str(lib)}) as w:
        w.key(F1, delay=1.0)
        assert _wait_for(w, lambda: "can be used with or without"
                         in _screen(w), SAID_S), \
            "Help should show the unpacked text:\n%s" % _screen(w)
        assert not os.path.exists(str(tmp_path / STRAY)), \
            "the ';' in the name was run as a command"
        assert w.alive()


# @scenario SC-debug-106
def test_help_on_a_word_longer_than_the_buffer_does_not_crash(tmp_path):
    """Help on a 90 letter word under the cursor (Topic Search) ends in
    "No manual entry", and the editor goes on."""
    with WpeSession(str(tmp_path), LONG_WORD + "\n") as w:
        w.menu(ALT.HELP, "t", delay=1.0)
        assert _wait_for(w, lambda: "No manual entry for x" in _screen(w),
                         SAID_S) or not w.alive(), _screen(w)
        assert w.alive(), "the editor ended on help for a long word"
        assert "No manual entry for x" in _screen(w), _screen(w)


# Plants what the exit must leave alone: a link inside the temp folder to a
# folder outside it.
def _plant_link(tmp_path):
    outside = tmp_path / "outside"
    outside.mkdir()
    (outside / "kept.txt").write_text("kept\n")
    return outside


# @scenario SC-debug-107
def test_exit_removes_the_temp_folder_and_not_what_a_link_points_at(tmp_path):
    """The editor's exit removes its temp folder with what is in it, and a
    link inside it to a folder outside leaves that folder alone."""
    _print_setup(tmp_path)
    outside = _plant_link(tmp_path)
    with WpeSession(str(tmp_path), _seed(), filename="t.c") as w:
        w.menu(ALT.FILE, "p", delay=0.8)
        w.key(PRINT_ANSWER, delay=0.6)
        assert _wait_for(w, lambda: _read(str(tmp_path / "printed.dir")), PRINTED_S)
        temp = _read(str(tmp_path / "printed.dir")).strip()
        assert os.path.isdir(temp), temp
        os.symlink(str(outside), os.path.join(temp, "link"))
        with open(os.path.join(temp, "inside.txt"), "w") as fh:
            fh.write("gone\n")
        w.key("\033x", delay=0.5)              # Alt-X: Quit
        deadline = time.monotonic() + ENDED_S * WAIT_SCALE
        while time.monotonic() < deadline and w.alive():
            w._drain(POLL_S)
        assert not w.alive(), "File > Quit should end the editor:\n%s" % _screen(w)
    assert not os.path.exists(temp), "the temp folder should be removed"
    assert (outside / "kept.txt").read_text() == "kept\n", \
        "what the link pointed at must be left alone"


SHIM_C = '''#include <errno.h>
#include <stdio.h>
int openpty(int *a, int *b, char *c, const void *d, const void *e)
{
    FILE *fp = fopen("%(dir)s/openpty.refused", "a");
    (void)a; (void)b; (void)c; (void)d; (void)e;
    if (fp) { fputs("refused\\n", fp); fclose(fp); }
    errno = ENOENT;
    return -1;
}
'''

RUN_SRC = '''#include <stdio.h>
int main(void)
{
    printf("first line\\n");
    printf("second line\\n");
    return 3;
}
'''


def _no_pty_shim(tmp_path):
    """A library that makes openpty fail: the machine has no terminal to give
    the program."""
    if shutil.which("gcc") is None:
        pytest.skip("no compiler to build the openpty stand-in")
    src = str(tmp_path / "nopty.c")
    with open(src, "w") as fh:
        fh.write(SHIM_C % {"dir": str(tmp_path)})
    lib = str(tmp_path / "nopty.so")
    subprocess.run(["gcc", "-shared", "-fPIC", "-o", lib, src], check=True)
    return lib


# @scenario SC-debug-108
def test_run_without_a_terminal_shows_the_output_and_the_code(tmp_path):
    """Run when no terminal can be made for the program: its output goes to
    Messages line by line and its exit code is reported, as before."""
    if shutil.which("gcc") is None:
        pytest.skip("no compiler to build the program")
    lib = _no_pty_shim(tmp_path)
    with WpeSession(str(tmp_path), RUN_SRC, filename="t.c", wait=1.5,
                    env_extra={"LD_PRELOAD": lib}) as w:
        w.key(CTRL_F9, delay=1.0)
        assert _wait_for(w, lambda: "Return-Code: 3" in _screen(w), RUN_END_S), \
            "Run should report the program's code:\n%s" % _screen(w)
        screen = _screen(w)
        assert "first line" in screen and "second line" in screen, screen
        assert _read(str(tmp_path / "openpty.refused")), \
            "the terminal was never asked for, so the fallback did not run"


FAKE_JAVA = '''#!/bin/sh
echo "start $*" >> "%(dir)s/java.log"
sleep 2
echo 'openjdk version "26.0.1" 2026-01-20' >&2
echo "end" >> "%(dir)s/java.log"
'''
STORM_S = 4.0                     # how long SIGCHLDs are sent
STORM_STEP_S = 0.02
PINNED_S = 20
# What the build tool's stand-in is run with: the editor pins JAVA_HOME to an
# LTS JDK for it when it learned that the default Java is too new.
FAKE_SCALA_CLI = '''#!/bin/sh
echo "[$JAVA_HOME]" >> "%(dir)s/scala.log"
exit 0
'''
SEED_SCALA = ("object P:\n"
              "  def main(args: Array[String]): Unit =\n"
              "    println(1)\n")


def _pinned(tmp_path):
    """Has the build tool been run with JAVA_HOME at an LTS JDK?"""
    return "[/usr/lib/jvm/" in _read(str(tmp_path / "scala.log"))


# @scenario SC-debug-109
def test_the_jdk_version_probe_survives_a_sigchld_storm(tmp_path):
    """The editor asks the default Java for its version before a Scala
    session.  A `java` that answers late while children end (SIGCHLD again
    and again) is still read whole: the editor learns the JDK is too new and
    runs the build tool with JAVA_HOME at an LTS JDK."""
    if not glob.glob("/usr/lib/jvm/*21*/bin/java") and \
            not glob.glob("/usr/lib/jvm/*17*/bin/java"):
        pytest.skip("no LTS JDK installed to pin")
    bindir = tmp_path / "bin"
    bindir.mkdir()
    _write_executable(str(bindir / "java"), FAKE_JAVA % {"dir": str(tmp_path)})
    _write_executable(str(bindir / "scala-cli"),
                      FAKE_SCALA_CLI % {"dir": str(tmp_path)})
    env = {"PATH": "%s:%s" % (bindir, os.environ.get("PATH", "")),
           "JAVA_HOME": ""}
    with WpeSession(str(tmp_path), SEED_SCALA, filename="p.scala", wait=1.5,
                    env_extra=env) as w:
        w.key("\x07", "r", delay=0.3)          # Ctrl-G R: start the session
        end = time.monotonic() + STORM_S * WAIT_SCALE
        while time.monotonic() < end and not _pinned(tmp_path):
            try:
                os.kill(w.proc.pid, signal.SIGCHLD)
            except ProcessLookupError:
                break
            w._drain(STORM_STEP_S)
        # The first probe is the one the storm was meant for: a probe cut
        # short is tried again once the storm is over, which is too late.
        assert _pinned(tmp_path), \
            "the first probe should read `java -version` whole, so the " \
            "build tool runs with JAVA_HOME pinned (its log %r, java's log " \
            "%r):\n%s" % (_read(str(tmp_path / "scala.log")),
                          _read(str(tmp_path / "java.log")), _screen(w))
        assert w.alive()


CTRL_C = "\x03"
SIGINT_CODE = 130                 # 128 + SIGINT, as a shell reports it
STOP_S = 15
TICK = "tick"
RUN_FOREVER_SRC = '''#include <stdio.h>
#include <unistd.h>
int main(void)
{
    for (;;) {
        printf("%s\\n");
        fflush(stdout);
        sleep(1);
    }
}
''' % TICK


# @scenario SC-debug-112
def test_ctrl_c_stops_a_program_run_without_a_terminal(tmp_path):
    """Run when no terminal can be made, of a program that never ends:
    Ctrl-C stops it, as it does on a terminal, and its code is the one a
    shell reports for SIGINT."""
    if shutil.which("gcc") is None:
        pytest.skip("no compiler to build the program")
    lib = _no_pty_shim(tmp_path)
    with WpeSession(str(tmp_path), RUN_FOREVER_SRC, filename="t.c", wait=1.5,
                    env_extra={"LD_PRELOAD": lib}) as w:
        w.key(CTRL_F9, delay=1.0)
        assert _wait_for(w, lambda: TICK in _screen(w), RUN_END_S), \
            "the program's output should show while it runs:\n%s" % _screen(w)
        assert _read(str(tmp_path / "openpty.refused")), \
            "the terminal was never asked for, so the fallback did not run"
        w.key(CTRL_C, delay=0.1)
        line = "Return-Code: %d" % SIGINT_CODE
        assert _wait_for(w, lambda: line in _screen(w), STOP_S), \
            "Ctrl-C should stop the program:\n%s" % _screen(w)
        assert w.alive()


# A gunzip that never ends, as on an input that never ends (a pipe, a
# damaged stream): it records its pid and sleeps.
ENDLESS_GUNZIP = '''#!/bin/sh
echo $$ > "%(dir)s/gunzip.pid"
exec sleep 300
'''
UNPACK_DEADLINE_S = 30
UNPACK_SAYS = "Waiting for gunzip (at most 30 s)"
SAYS_S = 8


def _pid_alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


# @scenario SC-debug-113
def test_an_unpack_that_never_ends_is_named_and_ended_at_its_deadline(tmp_path):
    """Help on a compressed help file whose unpack never ends: the status row
    names the wait and its deadline, and the unpack is ended there instead
    of holding the editor, whose keys wait meanwhile."""
    lib = tmp_path / "lib"
    lib.mkdir()
    with gzip.open(str(lib / "help.xwpe.gz"), "wb") as dst:
        dst.write(b"never read\n")
    bindir = tmp_path / "bin"
    bindir.mkdir()
    _write_executable(str(bindir / "gunzip"),
                      ENDLESS_GUNZIP % {"dir": str(tmp_path)})
    env = {"XWPE_LIB": str(lib),
           "PATH": "%s:%s" % (bindir, os.environ.get("PATH", ""))}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as w:
        w.key(F1, delay=0.5)
        assert _wait_for(w, lambda: UNPACK_SAYS in _screen(w), SAYS_S), \
            "the status row should name the unpack and its deadline:\n%s" \
            % _screen(w)
        pid = int(_read(str(tmp_path / "gunzip.pid")).strip())
        assert _wait_for(w, lambda: not _pid_alive(pid),
                         UNPACK_DEADLINE_S + SAYS_S), \
            "the unpack should be ended at its deadline"
        assert w.alive()
