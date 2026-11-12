"""A program a project folder names runs only after the user says yes.

An xwperc in the folder the editor starts in, and a project file, may name
the print command, a language's compiler line or the debugger.  A cloned
folder used to run its own program on the user's first F9 or print, with no
question.  Now the first run of a command that differs from the user's own
setting shows it, every control byte written out, and asks Run it / Use
mine / Cancel, with the focus on Use mine.  The answer is kept for that
folder, key and command text in the user's state folder; a changed command
asks again, and a store that cannot be read asks, never runs.

Every command here is a script the test writes into its own folder, which
touches a marker when it runs, so each row is red on any editor that runs
the folder's command without the question.
"""
import json
import os
import shutil
import stat
import time

import pytest
from wpe_driver import ALT, WAIT_SCALE, WpeSession, wait_screen, tool_usable

F9 = "\033[20~"            # Make: compile and link
ESC = "\033"
RUN_IT = "\033r"           # Alt-R: Run it
USE_MINE = "\033u"         # Alt-U: Use mine
CTRL_G = "\x07"            # the debugger prefix
ENTER = "\r"

BOX_TITLE = "Command from ./xwperc"
OWN_LINE = "Your own setting: gcc -g"
STORE_MAGIC = "xwpe-start-commands 2"
SEED = "int main(void){return 0;}\n"
BOX_WAIT = 12              # the box is up by then
BUILD_WAIT = 20            # gcc has built t.c by then
SETTLE = 1.5               # whatever an answer leads to has started by then

FAKE = '''#!/bin/sh
touch "%(marker)s"
exit 1
'''

needs_gcc = pytest.mark.skipif(not tool_usable("gcc", "--version"), reason="gcc not usable")


def _executable(path, text):
    path.write_text(text)
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


class Folder:
    """A start folder with its own xwperc and a fake program in it, and a
    HOME of the user's own beside it."""

    def __init__(self, tmp_path, xwperc=None):
        self.ws = tmp_path / "ws"
        self.home = tmp_path / "home"
        self.ws.mkdir(exist_ok=True)
        self.home.mkdir(exist_ok=True)
        self.marker = tmp_path / "ran-fake"
        self.trace = tmp_path / "ui.trace"
        self.fake = self.ws / "fake-cc"
        _executable(self.fake, FAKE % {"marker": self.marker})
        if xwperc is not None:
            (self.ws / "xwperc").write_text(xwperc)

    @property
    def store(self):
        return self.home / ".local" / "state" / "xwpe" / "start-commands"

    def own_config(self, text):
        cfg = self.home / ".config" / "xwpe"
        cfg.mkdir(parents=True, exist_ok=True)
        (cfg / "xwperc").write_text(text)

    def session(self, filename="t.c", seed=SEED):
        env = {"HOME": str(self.home), "XWPE_UI_TRACE": str(self.trace)}
        return WpeSession(str(self.ws), seed, filename=filename, env_extra=env)

    def traced(self):
        try:
            return self.trace.read_text()
        except OSError:
            return ""

    def records(self):
        try:
            return [l for l in self.store.read_text().splitlines()[1:] if l]
        except OSError:
            return []


def _screen(s):
    return "\n".join(s.display())


def _box_command(box, head):
    """The command the box shows below @head, its wrapped rows joined again:
    the box cuts a long command by characters, wherever the test's own
    folder path happens to put the cut, and ends every row but the last
    with the wrap mark, which is taken off here."""
    rows = box.splitlines()
    start = next(i for i, r in enumerate(rows) if head in r) + 2
    parts = []
    for row in rows[start:]:
        inner = row.split("xx", 1)[-1].rsplit(" x", 1)[0].strip()
        if not inner:
            break
        parts.append(inner)
    wrapped = [p[:-len(WRAP_MARK)] for p in parts[:-1] if p.endswith(WRAP_MARK)]
    if len(wrapped) != len(parts[:-1]):
        return "(a wrapped row without its mark: %r)" % parts
    return "".join(wrapped + parts[-1:])


def _wait(predicate, timeout):
    end = time.time() + timeout * WAIT_SCALE
    while time.time() < end:
        if predicate():
            return True
        time.sleep(0.3)
    return predicate()


def _compile_and_answer(folder, answer):
    """F9 in the start folder, the box, then @answer; returns the box."""
    with folder.session() as s:
        s.key(F9, delay=0.8)
        assert wait_screen(s, BOX_TITLE, tmo=BOX_WAIT), \
            "no question before the folder's compiler ran:\n" + _screen(s)
        box = _screen(s)
        s.key(answer, delay=0.8)
        s._drain(SETTLE)
        after = _screen(s)
    return box, after


# @scenario SC-compile-28
def test_startcmd_compiler_asks(tmp_path):
    """The folder's xwperc names ./fake-cc as the C compiler: F9 shows it and
    the user's own setting, Esc runs nothing and remembers nothing."""
    folder = Folder(tmp_path, "[Language/C]\nCompiler : ./fake-cc\n")
    box, _ = _compile_and_answer(folder, ESC)
    assert "./fake-cc -g" in box, box
    assert OWN_LINE in box, box
    assert "Use mine" in box and "Run it" in box, box
    assert not folder.marker.exists(), "the folder's compiler ran after Esc"
    assert folder.records() == [], "a cancelled question was remembered"


# @scenario SC-compile-28
@needs_gcc
def test_startcmd_use_mine(tmp_path):
    """Use mine builds with the user's own gcc, the folder's program never
    runs, and the store keeps "no" for this folder and the build's plan.

    The store is version 2: the record's key is the action and language
    (build/C) and its text the build's whole plan (escaped onto one line),
    in which the folder's compiler is a word."""
    folder = Folder(tmp_path, "[Language/C]\nCompiler : ./fake-cc\n")
    _compile_and_answer(folder, USE_MINE)
    assert _wait(lambda: (folder.ws / "t.e").exists(), BUILD_WAIT), \
        "the user's own compiler did not build the program"
    assert not folder.marker.exists(), "the folder's compiler ran after Use mine"
    real = os.path.realpath(str(folder.ws))
    records = folder.records()
    assert len(records) == 1, records
    assert records[0].startswith("no\tbuild/C\t%s\t" % real), records
    assert "\\nargv ./fake-cc\\n" in records[0], records
    assert "using your own C compiler" in folder.traced(), folder.traced()


# @scenario SC-compile-28
def test_startcmd_focus_starts_on_use_mine(tmp_path):
    """Enter on the question takes the focused button, and that is Use mine:
    the folder's program does not run.  (The answer's key names the action,
    build/C, since F9's compile and link are one plan.)"""
    folder = Folder(tmp_path, "[Language/C]\nCompiler : ./fake-cc\n")
    _compile_and_answer(folder, ENTER)
    assert not folder.marker.exists(), "Enter ran the folder's compiler"
    assert "startcmd answer build/C own" in folder.traced(), folder.traced()


# @scenario SC-compile-28
def test_startcmd_remembered(tmp_path):
    """Run it once; the next start in the same folder compiles with the
    folder's program and no question, and the store holds one line."""
    folder = Folder(tmp_path, "[Language/C]\nCompiler : ./fake-cc\n")
    _compile_and_answer(folder, RUN_IT)
    assert _wait(folder.marker.exists, BUILD_WAIT), "Run it did not run the folder's compiler"
    folder.marker.unlink()
    asked_before = folder.traced().count("startcmd ask")
    with folder.session() as s:
        s.key(F9, delay=0.8)
        assert _wait(folder.marker.exists, BUILD_WAIT), \
            "the remembered command did not run:\n" + _screen(s)
        screen = _screen(s)
    assert BOX_TITLE not in screen, screen
    assert folder.traced().count("startcmd ask") == asked_before, folder.traced()
    assert len(folder.records()) == 1, folder.records()


# @scenario SC-compile-28
def test_startcmd_changed_asks_again(tmp_path):
    """After Run it, the folder's file changes the compiler's options: the
    next F9 asks again and shows the new command."""
    folder = Folder(tmp_path, "[Language/C]\nCompiler : ./fake-cc\n")
    _compile_and_answer(folder, RUN_IT)
    (folder.ws / "xwperc").write_text(
        "[Language/C]\nCompiler : ./fake-cc\nCompilerOptions : -fplugin=./x.so\n")
    box, _ = _compile_and_answer(folder, ESC)
    assert "./fake-cc -fplugin=./x.so" in box, box


# @scenario SC-compile-28
def test_startcmd_same_as_own_silent(tmp_path):
    """The user's own config and the folder's xwperc name the same compiler:
    it is his, so F9 runs it with no question."""
    folder = Folder(tmp_path, "[Language/C]\nCompiler : ./fake-cc\n")
    folder.own_config("[Language/C]\nCompiler : ./fake-cc\n")
    with folder.session() as s:
        s.key(F9, delay=0.8)
        assert _wait(folder.marker.exists, BUILD_WAIT), \
            "the user's own compiler did not run:\n" + _screen(s)
        screen = _screen(s)
    assert BOX_TITLE not in screen, screen
    assert "startcmd ask" not in folder.traced(), folder.traced()


# @scenario SC-compile-28
def test_startcmd_store_fails_closed(tmp_path):
    """A store of another version holding a yes for this very command: the
    question comes anyway, the status line says why, the store is kept."""
    folder = Folder(tmp_path, "[Language/C]\nCompiler : ./fake-cc\n")
    folder.store.parent.mkdir(parents=True)
    real = os.path.realpath(str(folder.ws))
    planted = "xwpe-start-commands 9\nyes\tCompiler/C\t%s\t./fake-cc -g\n" % real
    folder.store.write_text(planted)
    _compile_and_answer(folder, ESC)
    assert not folder.marker.exists(), "an unreadable store let the command run"
    assert "could not read the remembered commands" in folder.traced(), folder.traced()
    assert folder.store.read_text() == planted, "the unreadable store was written over"


# @scenario SC-compile-28
def test_startcmd_too_long(tmp_path):
    """A 5000-character compiler cannot be shown whole: the box says how
    long it is and where to read it, and Run it is not offered."""
    long_cc = "./fake-cc" + "x" * 5000
    folder = Folder(tmp_path, "[Language/C]\nCompiler : %s\n" % long_cc)
    box, _ = _compile_and_answer(folder, ESC)
    assert "read it in ./xwperc" in box, box
    assert "Run it" not in box, "Run it offered for a command the user cannot see:\n" + box
    assert "Use mine" in box, box


ROW_ROOM = 67              # characters of the command on a row, at 80 columns
WRAP_MARK = "\\"           # ends a row the command goes on after


# @scenario SC-compile-28
def test_startcmd_a_blank_at_a_wrap_is_seen(tmp_path):
    """The folder's compiler has a blank exactly where the box cuts the
    command into rows: the row ends with the blank and then the wrap mark,
    so the user sees two words where they might have read one."""
    head = "./fake-cc" + "x" * (ROW_ROOM - 1 - len("./fake-cc"))
    folder = Folder(tmp_path, "[Language/C]\nCompiler : %s tail\n" % head)
    box, _ = _compile_and_answer(folder, ESC)
    assert (head + " \\") in box, "the blank at the cut is not marked:\n" + box
    assert "tail -g" in box, box


# @scenario SC-compile-28
def test_startcmd_print_asks(tmp_path):
    """The folder's PrintCmd is a script: File > Print, Yes to "print it?",
    and the command box follows before anything runs."""
    folder = Folder(tmp_path)
    (folder.ws / "xwperc").write_text("[General]\nPrintCmd : %s\n" % folder.fake)
    with folder.session() as s:
        s.menu(ALT.FILE, "p", delay=0.8)
        s.key("y", delay=0.8)
        assert wait_screen(s, BOX_TITLE, tmo=BOX_WAIT), \
            "no question before the folder's print command:\n" + _screen(s)
        box = _screen(s)
        s.key(ESC, delay=0.8)
        s._drain(SETTLE)
    assert "sets the print command to" in box, box
    assert _box_command(box, "sets the print command to") == str(folder.fake), box
    assert "startcmd ask PrintCmd %s" % folder.fake in folder.traced(), folder.traced()
    assert not folder.marker.exists(), "the folder's print command ran"


# @scenario SC-compile-28
def test_startcmd_escape_shown(tmp_path):
    """A print command holding an escape sequence that clears the screen:
    the box shows it written out, and the screen is not cleared."""
    folder = Folder(tmp_path)
    (folder.ws / "xwperc").write_text("[General]\nPrintCmd : echo \x1b[2J\n")
    with folder.session() as s:
        s.menu(ALT.FILE, "p", delay=0.8)
        s.key("y", delay=0.8)
        assert wait_screen(s, BOX_TITLE, tmo=BOX_WAIT), \
            "no question before the folder's print command:\n" + _screen(s)
        rows = s.display()
        s.key(ESC, delay=0.8)
    box = "\n".join(rows)
    assert "echo \\x1b[2J" in box, box
    assert "File" in rows[0], "the menu bar was cleared:\n" + box


# @scenario SC-compile-28
@needs_gcc
def test_startcmd_debugger_asks(tmp_path):
    """The folder's xwperc names pdb as the debugger for a C file: Ctrl-G R
    builds, then asks, naming pdb against the user's gdb."""
    folder = Folder(tmp_path, "[Programming]\nDebugger : 5\n")
    with folder.session() as s:
        s.key(CTRL_G, "r", delay=1.0)
        assert wait_screen(s, BOX_TITLE, tmo=BUILD_WAIT), \
            "no question before the folder's debugger:\n" + _screen(s)
        box = _screen(s)
        s.key(ESC, delay=0.8)
        s._drain(SETTLE)
    assert "sets the debugger to" in box and "pdb" in box, box
    assert "Your own setting: gdb" in box, box
    assert "startcmd answer Debugger cancel" in folder.traced(), folder.traced()


PROJECT = "CMP=\t./fake-cc\nCMPFLAGS=\t-g\nEXENAME=\tdemo\nFILES=\tmain.c\n"


# @scenario SC-compile-28
def test_startcmd_project_asks(tmp_path):
    """A project file names ./fake-cc as its compiler: the first build of
    the project shows it and runs nothing on Cancel, with no xwperc in the
    folder at all."""
    folder = Folder(tmp_path)
    (folder.ws / "demo.prj").write_text(PROJECT)
    with folder.session(filename="main.c") as s:
        s.key(ALT.PROJECT, "p", delay=0.6)
        s.key(ENTER, delay=0.6)
        s.key(ENTER, delay=0.8)
        s.key(F9, delay=0.8)
        assert wait_screen(s, "Command from demo.prj", tmo=BOX_WAIT), \
            "no question before the project's compiler:\n" + _screen(s)
        box = _screen(s)
        s.key(ESC, delay=0.8)
        s._drain(SETTLE)
    assert "./fake-cc -g" in box, box
    assert not folder.marker.exists(), "the project's compiler ran after Esc"


def _open_project(s):
    """Project > Open project, the Files box, demo.prj."""
    s.key(ALT.PROJECT, "p", delay=0.6)
    s.key(ENTER, delay=0.6)
    s.key(ENTER, delay=0.8)


def _wait_alive(s, seconds):
    """Drain the editor for @seconds; True when it is still running."""
    s._drain(seconds)
    return s.proc.poll() is None


# @scenario SC-compile-28
@needs_gcc
def test_startcmd_project_use_mine(tmp_path):
    """The project's ./fake-cc, Use mine: the project builds with the user's
    own gcc, the project's compiler never runs, the store keeps "no" (under
    the version-2 key of the project's build, project/demo.prj)."""
    folder = Folder(tmp_path)
    (folder.ws / "demo.prj").write_text(PROJECT)
    with folder.session(filename="main.c") as s:
        _open_project(s)
        s.key(F9, delay=0.8)
        assert wait_screen(s, "Command from demo.prj", tmo=BOX_WAIT), _screen(s)
        s.key(USE_MINE, delay=0.8)
        assert _wait(lambda: (folder.ws / "demo").exists(), BUILD_WAIT), \
            "the user's own compiler did not build the project:\n" + _screen(s)
    assert not folder.marker.exists(), "the project's compiler ran after Use mine"
    assert any(r.startswith("no\tproject/demo.prj\t") for r in folder.records()), folder.records()


RSP_WRAPPER = "-wrapper /bin/sh,-c,touch\\ %s main.c\n"


# @scenario SC-compile-28
@needs_gcc
def test_startcmd_project_files_are_never_options(tmp_path):
    """FILES names `@evil.rsp`, a response file whose -wrapper runs a shell,
    and CMP/CMPFLAGS are the user's own gcc -g, so nothing asks: the word is
    passed as the file ./@evil.rsp, never read as more options."""
    folder = Folder(tmp_path)
    (folder.ws / "evil.rsp").write_text(RSP_WRAPPER % folder.marker)
    (folder.ws / "demo.prj").write_text(
        "CMP=\tgcc\nCMPFLAGS=\t-g\nEXENAME=\tdemo\nFILES=\t@evil.rsp main.c\n")
    with folder.session(filename="main.c") as s:
        _open_project(s)
        s.key(F9, delay=0.8)
        assert _wait_alive(s, 6), "the editor died building the project"
        screen = _screen(s)
    assert BOX_TITLE not in screen and "Command from" not in screen, screen
    assert not folder.marker.exists(), "a FILES word was read as compiler options"


# @scenario SC-compile-28
@needs_gcc
def test_exedir_response_file_reaches_gcc_unasked(tmp_path):
    """The folder's Exedir is `@r`, and r/t.o holds `-wrapper ... touch M`:
    the object path is handed to gcc as ./@r/t.o, a file, never as a
    response file of options, so F9 runs nothing of the folder's."""
    folder = Folder(tmp_path, "[Programming]\nExedir : @r\n")
    (folder.ws / "r").mkdir()
    (folder.ws / "r" / "t.o").write_text(
        "x.o -wrapper /bin/sh,-c,touch${IFS}%s\n" % folder.marker)
    with folder.session() as s:
        s.key(F9, delay=0.8)
        ran = _wait(folder.marker.exists, 10)
        screen = _screen(s)
    assert not ran, "a folder's Exedir ran a program at F9:\n" + screen


QUOTED_RUST_NAME = "a';touch pwned;'.rs"
DEBUG_COMPILE_LINE = "Compiling for debug"


# @scenario SC-compile-28
@pytest.mark.skipif(shutil.which("rustc") is None, reason="rustc not on PATH")
def test_a_file_name_never_runs_at_the_compile_for_debug(tmp_path):
    """A cloned folder holds a Rust file whose name closes a shell quote
    and runs `touch pwned`: Ctrl-G R compiles it for debug, and the name
    reaches the compiler as one file name, never as a shell command."""
    folder = Folder(tmp_path)
    marker = folder.ws / "pwned"
    with folder.session(filename=QUOTED_RUST_NAME, seed="fn main(){}\n") as s:
        s.key(CTRL_G, "r", delay=1.0)
        assert wait_screen(s, DEBUG_COMPILE_LINE, tmo=BOX_WAIT), _screen(s)
        ran = _wait(marker.exists, BUILD_WAIT)
        screen = _screen(s)
    assert not ran, "a file name ran a command at the compile for debug:\n" + screen


UTIL = "int util(void){return 1;}\n"


# @scenario SC-compile-28
@needs_gcc
def test_startcmd_project_library_is_never_an_option(tmp_path):
    """LIBNAME names `@lib.rsp`, a response file holding `--version`: ar
    gets the file ./@lib.rsp, and never reads options out of it (ar would
    print its version)."""
    folder = Folder(tmp_path)
    rsp = folder.ws / "@lib.rsp"
    rsp.write_text("--version\n")
    (folder.ws / "util.c").write_text(UTIL)
    (folder.ws / "demo.prj").write_text(
        "CMP=\tgcc\nCMPFLAGS=\t-g\nEXENAME=\tdemo\nLIBNAME=\t@lib.rsp\n"
        "FILES=\tmain.c util.c\n")
    with folder.session(filename="main.c") as s:
        _open_project(s)
        s.key(F9, delay=0.8)
        assert wait_screen(s, "ar -r", tmo=BUILD_WAIT), _screen(s)
        s._drain(SETTLE)
        screen = _screen(s)
    assert "ar -r ./@lib.rsp" in screen, "ar was handed the library as an option:\n" + screen
    assert "GNU ar" not in screen, "ar read options out of the library's name:\n" + screen


INSTALL_PROJECT = ("CMP=\tgcc\nCMPFLAGS=\t-g\nEXENAME=\tdemo\nFILES=\tmain.c\n"
                   "\ninstall:\n\ttouch %s\n")
INSTALL = "\033l"          # Alt-L: build, then install


# @scenario SC-compile-28
@needs_gcc
def test_startcmd_project_install_asks(tmp_path):
    """The project's install: line goes to the shell at Alt-L: it is shown
    first, and Cancel runs nothing."""
    folder = Folder(tmp_path)
    (folder.ws / "demo.prj").write_text(INSTALL_PROJECT % folder.marker)
    with folder.session(filename="main.c") as s:
        _open_project(s)
        s.key(INSTALL, delay=0.8)
        assert wait_screen(s, "Command from demo.prj", tmo=BUILD_WAIT), \
            "no question before the project's install commands:\n" + _screen(s)
        box = _screen(s)
        s.key(ESC, delay=0.8)
        s._drain(SETTLE)
    assert "install commands" in box and "touch" in box, box
    assert not folder.marker.exists(), "the install line ran after Cancel"


def _compiler_dialog_ok(folder, keys):
    """Options > Compiler > C, @keys in the dialog, Enter; then F9.
    Returns the screen after F9 (the question, or the build)."""
    with folder.session() as s:
        s.key("\033o", delay=0.6)
        s.key("c", delay=0.8)
        s.key("C", delay=0.8)
        assert "Compiler-Options" in _screen(s), _screen(s)
        for k in keys:
            s.key(k, delay=0.5)
        s.key(ENTER, delay=0.8)
        s.key(F9, delay=0.8)
        asked = wait_screen(s, BOX_TITLE, tmo=BOX_WAIT)
        screen = _screen(s)
        if asked:
            s.key(ESC, delay=0.8)
    return asked, screen


# @scenario SC-compile-28
def test_startcmd_another_dialog_field_is_not_a_yes(tmp_path):
    """The folder sets ./fake-cc; the user changes only Executable in the
    Compiler dialog: the folder's compiler is still not his, F9 asks."""
    folder = Folder(tmp_path, "[Language/C]\nCompiler : ./fake-cc\n")
    asked, screen = _compiler_dialog_ok(folder, ["\033e", "prog"])
    assert asked, "changing Executable approved the folder's compiler:\n" + screen
    assert not folder.marker.exists(), "the folder's compiler ran"
    assert folder.records() == [], folder.records()


# @scenario SC-compile-28
def test_startcmd_a_cut_dialog_value_is_never_a_yes(tmp_path):
    """The folder's compiler is longer than the dialog's field: OK on the
    untouched dialog does not approve the cut text, F9 still asks."""
    folder = Folder(tmp_path, "[Language/C]\nCompiler : ./fake-cc%s\n" % ("x" * 200))
    asked, screen = _compiler_dialog_ok(folder, [])
    assert asked, "an untouched OK approved a cut compiler line:\n" + screen
    assert not folder.marker.exists(), "the folder's compiler ran"


OWN_PRINT = '''#!/bin/sh
touch "%(marker)s"
'''


def _print_and_answer(folder, answer):
    with folder.session() as s:
        s.menu(ALT.FILE, "p", delay=0.8)
        s.key("y", delay=0.8)
        assert wait_screen(s, BOX_TITLE, tmo=BOX_WAIT), _screen(s)
        s.key(answer, delay=0.8)
        s._drain(SETTLE)


# @scenario SC-compile-28
def test_startcmd_print_run_it(tmp_path):
    """Run it on the folder's print command runs it and remembers yes.  The
    print's start plan names the command as the folder's (its judge alone
    would ask: verdict 4), and the answer to the print question runs it."""
    folder = Folder(tmp_path)
    (folder.ws / "xwperc").write_text("[General]\nPrintCmd : %s\n" % folder.fake)
    _print_and_answer(folder, RUN_IT)
    assert _wait(folder.marker.exists, BUILD_WAIT), "Run it did not print"
    assert any(r.startswith("yes\tPrintCmd\t") for r in folder.records()), folder.records()
    assert "startplan judge print/PrintCmd 4" in folder.traced(), folder.traced()


# @scenario SC-compile-28
def test_startcmd_print_use_mine(tmp_path):
    """Use mine prints with the user's own command; the folder's never runs."""
    folder = Folder(tmp_path)
    own = folder.home / "own-lpr"
    own_marker = tmp_path / "ran-own"
    _executable(own, OWN_PRINT % {"marker": own_marker})
    folder.own_config("[General]\nPrintCmd : %s\n" % own)
    (folder.ws / "xwperc").write_text("[General]\nPrintCmd : %s\n" % folder.fake)
    _print_and_answer(folder, USE_MINE)
    assert _wait(own_marker.exists, BUILD_WAIT), "Use mine did not print with the user's command"
    assert not folder.marker.exists(), "the folder's print command ran after Use mine"
    assert any(r.startswith("no\tPrintCmd\t") for r in folder.records()), folder.records()


FAKE_SCALA_CLI = "#!/bin/sh\nexit 0\n"
SCALA_SEED = ("object P:\n"
              "  def main(args: Array[String]): Unit =\n"
              "    println(1)\n")


# @scenario SC-compile-28
def test_startcmd_bsp_asks(tmp_path):
    """A Scala folder's .bsp/evil.json names a shell command as its build
    server: Ctrl-G R shows it first, and Cancel starts nothing."""
    folder = Folder(tmp_path)
    bindir = tmp_path / "bin"
    bindir.mkdir()
    _executable(bindir / "scala-cli", FAKE_SCALA_CLI)
    bsp = folder.ws / ".bsp"
    bsp.mkdir()
    (bsp / "evil.json").write_text(json.dumps(
        {"name": "evil", "argv": ["/bin/sh", "-c", "touch %s" % folder.marker],
         "version": "1", "bspVersion": "2.1.1", "languages": ["scala"]}))
    env = {"HOME": str(folder.home), "XWPE_UI_TRACE": str(folder.trace),
           "PATH": "%s:%s" % (bindir, os.environ.get("PATH", ""))}
    with WpeSession(str(folder.ws), SCALA_SEED, filename="p.scala", env_extra=env) as s:
        s.key(CTRL_G, "r", delay=1.0)
        assert wait_screen(s, "Command from .bsp/evil.json", tmo=BUILD_WAIT), \
            "no question before the folder's build server:\n" + _screen(s)
        box = _screen(s)
        s.key(ESC, delay=0.8)
        s._drain(SETTLE)
    assert "build server" in box and "/bin/sh -c" in box, box
    assert not folder.marker.exists(), "the folder's build server ran after Cancel"


SETUP_IDE = '''#!/bin/sh
# scala-cli setup-ide <dir>: write the connection file naming the server.
mkdir -p "$2/.bsp"
printf '{"name":"scala-cli","argv":["%(server)s"],"version":"1","bspVersion":"2.1.1","languages":["scala"]}' > "$2/.bsp/scala-cli.json"
'''
TOUCH_AND_END = '''#!/bin/sh
touch "%(marker)s"
'''


# @scenario SC-compile-28
def test_startcmd_bsp_only_what_setup_ide_wrote_is_spared(tmp_path):
    """The editor's own setup-ide writes .bsp/scala-cli.json: that very
    command starts without a question.  The file is then changed to name
    another program: the next Ctrl-G R in the same session asks."""
    folder = Folder(tmp_path)
    bindir = tmp_path / "bin"
    bindir.mkdir()
    first = tmp_path / "ran-setup-server"
    server = bindir / "setup-server"
    _executable(server, TOUCH_AND_END % {"marker": first})
    _executable(bindir / "scala-cli", SETUP_IDE % {"server": server})
    evil = bindir / "evil-server"
    _executable(evil, TOUCH_AND_END % {"marker": folder.marker})
    env = {"HOME": str(folder.home), "XWPE_UI_TRACE": str(folder.trace),
           "PATH": "%s:%s" % (bindir, os.environ.get("PATH", ""))}
    with WpeSession(str(folder.ws), SCALA_SEED, filename="p.scala", env_extra=env) as s:
        s.key(CTRL_G, "r", delay=1.0)
        assert _wait(first.exists, BUILD_WAIT), \
            "the server setup-ide named did not start:\n" + _screen(s)
        asked_first = "startcmd ask" in folder.traced()
        s._drain(SETTLE)
        s.key(ESC, delay=0.8)
        s.key(ESC, delay=0.8)
        (folder.ws / ".bsp" / "scala-cli.json").write_text(json.dumps(
            {"name": "scala-cli", "argv": [str(evil)], "version": "1",
             "bspVersion": "2.1.1", "languages": ["scala"]}))
        s.key(CTRL_G, "r", delay=1.0)
        asked = wait_screen(s, "Command from .bsp/scala-cli.json", tmo=BUILD_WAIT)
        screen = _screen(s)
        if asked:
            s.key(ESC, delay=0.8)
        s._drain(SETTLE)
    assert not asked_first, "the command setup-ide itself wrote was asked about"
    assert asked, "a changed .bsp command started unasked:\n" + screen
    assert not folder.marker.exists(), "the changed build server ran"


DEEP_PART = "d" * 60


def _deep_folder(tmp_path, depth):
    ws = tmp_path
    for _ in range(depth):
        ws = ws / DEEP_PART
    ws.mkdir(parents=True)
    return ws


# @scenario SC-compile-28
@pytest.mark.parametrize("depth", [1, 4])
def test_save_options_shows_a_long_option_file_path_whole(tmp_path, depth):
    """Options > Save Options in a folder whose xwperc path is longer than
    the name field (129-255 bytes, and past 255): the editor shows the
    whole path and Yes saves there; nothing is cut, nothing overflows."""
    ws = _deep_folder(tmp_path, depth)
    (ws / "xwperc").write_text("[General]\nTab : 3\n")
    assert len(str(ws / "xwperc")) > 128
    with WpeSession(str(ws), SEED, filename="t.c") as s:
        s.key("\033o", delay=0.6)
        s.key("s", delay=0.8)
        assert wait_screen(s, "Save the options to", tmo=BOX_WAIT), _screen(s)
        s.key("y", delay=0.8)
        assert _wait_alive(s, 1.0), "the editor died saving its options"
    assert "[General]" in (ws / "xwperc").read_text() and \
        "Version" in (ws / "xwperc").read_text(), "the options were not saved there"


LONG_PROJECT = "CMP=\tgcc\nCMPFLAGS=\t-g\nFILES=\tmain.c\n\ninstall:\n\ttrue\n"


# @scenario SC-compile-28
@needs_gcc
def test_a_long_project_path_walks_the_project_code(tmp_path):
    """The folder's Project key names a project file more than 256 bytes
    deep: Project-Options, its Save, F9 and Install all handle the path
    whole (they used to copy it into 256-byte buffers)."""
    deep = _deep_folder(tmp_path, 5)
    prj = deep / "demo.prj"
    prj.write_text(LONG_PROJECT)
    (deep / "main.c").write_text(SEED)
    assert len(str(prj)) > 256
    folder = Folder(tmp_path, "[Programming]\nProject : %s\n" % prj)
    with folder.session() as s:
        s.key(ALT.PROJECT, "o", delay=1.0)
        s.key("\033s", delay=1.0)                # Save in Project-Options
        assert _wait_alive(s, 0.5), "the editor died saving the project"
        s.key(F9, delay=1.0)
        assert _wait_alive(s, 4), "the editor died building the project"
        s.key(INSTALL, delay=1.0)
        assert _wait_alive(s, 4), "the editor died installing the project"
        s.key(ESC, delay=0.5)
    assert "CMP=" in prj.read_text(), "the project file was not saved whole"


# @scenario SC-compile-28
def test_run_of_a_script_with_long_arguments(tmp_path):
    """Run on a shell script with Run arguments longer than the old command
    buffer: the script runs and the editor lives.  The arguments come from
    the folder's xwperc, so Run first shows them and asks (a folder's
    Arguments are asked about once per folder); Run it answers."""
    folder = Folder(tmp_path, "[Programming]\nArguments : %s\n" % ("a" * 300))
    out = folder.ws / "ran.txt"
    script = "#!/bin/sh\necho ran > %s\n" % out
    with folder.session(filename="t.sh", seed=script) as s:
        s.key("\033[20;5~", delay=1.0)        # Ctrl-F9: Run
        assert wait_screen(s, "Command from ./xwperc", tmo=BUILD_WAIT), \
            "no question before the folder's long Arguments:\n" + _screen(s)
        s.key(RUN_IT, delay=1.0)
        assert _wait(out.exists, BUILD_WAIT), "the script did not run:\n" + _screen(s)
        assert _wait_alive(s, 0.5), "the editor died running the script"


# @scenario SC-compile-28
@needs_gcc
def test_project_with_a_long_program_name_builds(tmp_path):
    """EXENAME of 200 characters: the build makes that program and the
    editor lives (the path was formatted into 128 bytes)."""
    folder = Folder(tmp_path)
    name = "e" * 200
    (folder.ws / "demo.prj").write_text(
        "CMP=\tgcc\nCMPFLAGS=\t-g\nEXENAME=\t%s\nFILES=\tmain.c\n" % name)
    with folder.session(filename="main.c") as s:
        _open_project(s)
        s.key(F9, delay=0.8)
        assert _wait(lambda: (folder.ws / name).exists(), BUILD_WAIT), _screen(s)
        assert _wait_alive(s, 0.5), "the editor died building the project"
