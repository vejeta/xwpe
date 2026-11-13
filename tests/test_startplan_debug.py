"""A debug session's start is planned: what the folder decides is shown first.

Starting a debugger in a cloned folder used to run whatever the folder said
with no question: the debugger's own start-up file in the folder (./.pdbrc,
./jdb.ini, ./.gdbinit under the user's auto-load setting), the Arguments a
start-folder xwperc puts on the debugger's run command, a program named like
an option ("-x.c" builds "-x.e", which gdb read as "-x .e": commands from the
file ".e"), and a watch or breakpoint a project file gives with a line break
in it (the rest of the line reached the debugger as a command of its own).

Now the debugger's start is a start plan.  A start-up file the working
folder holds, or a script gdb loads from beside the program
(<program>-gdb.py), is shown with its lines and asked about, once per
folder and text; "Use mine" starts gdb with none of them loaded (gdb
starts apart from the folder: tests/test_startplan_gdb_apart.py); a
debugger the start folder names is asked about in that same one box, and
"Use mine" there starts the user's own debugger; a folder's
Arguments are shown as the run command they make; the program always
reaches the debugger as a file; and no value with a line break is ever
written to a debugger.

The editor's trace (XWPE_UI_TRACE) carries every line of the box whole.
Each folder file runs `touch` on a marker when the debugger runs it, so each
row is red on an editor that starts the debugger without the question.
"""
import os
import time

import pytest
from wpe_driver import ALT, WAIT_SCALE, WpeSession, wait_screen, tool_usable

CTRL_G = "\x07"            # the debugger prefix
ESC = "\033"
ENTER = "\r"
RUN_IT = "\033r"           # Alt-R: Run it
USE_MINE = "\033u"         # Alt-U: Use mine

SEED_C = "int main(void){return 0;}\n"
SEED_PY = "print(1)\n"
SEED_JAVA = "class T { public static void main(String[] a) { System.out.println(1); } }\n"
BOX_WAIT = 30              # a build and the debugger's plan are done by then
RAN_WAIT = 12              # a debugger that runs a folder file has run it by then
SETTLE = 2.0               # whatever an answer leads to has started by then

needs_python = pytest.mark.skipif(not tool_usable("python3", "--version"),
                                  reason="python3 not usable")
needs_gdb = pytest.mark.skipif(not (tool_usable("gcc", "--version") and
                                    tool_usable("gdb", "--version")),
                               reason="gcc and gdb not usable")
needs_jdb = pytest.mark.skipif(not (tool_usable("javac", "-version") and
                                    tool_usable("jdb", "-version")),
                               reason="javac and jdb not usable")


class Folder:
    """A start folder, and a HOME of the user's own beside it."""

    def __init__(self, tmp_path):
        self.ws = tmp_path / "ws"
        self.home = tmp_path / "home"
        self.ws.mkdir(exist_ok=True)
        self.home.mkdir(exist_ok=True)
        self.trace = tmp_path / "ui.trace"
        self.ran = self.ws / "ran"

    @property
    def store(self):
        return self.home / ".local" / "state" / "xwpe" / "start-commands"

    def session(self, filename, seed, workdir=None, home=None):
        env = {"HOME": str(home or self.home), "XWPE_UI_TRACE": str(self.trace)}
        return WpeSession(str(workdir or self.ws), seed, filename=filename, env_extra=env)

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

    def touch_line(self, marker=None):
        """A line that touches @marker (the folder's "ran") when run."""
        return "touch %s" % (marker or self.ran)


def _screen(s):
    return "\n".join(s.display())


def _wait(predicate, timeout):
    end = time.time() + timeout * WAIT_SCALE
    while time.time() < end:
        if predicate():
            return True
        time.sleep(0.3)
    return predicate()


def _plan_lines(folder):
    return [l.split("startplan line ", 1)[1] for l in folder.traced().splitlines()
            if "startplan line " in l]


def _debug_box(s, title):
    """Ctrl-G R, and the box titled @title; returns its screen."""
    s.key(CTRL_G, "r", delay=1.0)
    asked = wait_screen(s, title, tmo=BOX_WAIT)
    screen = _screen(s)
    assert asked, "no question before the debugger started:\n" + screen
    return screen


# @scenario SC-compile-29
@needs_python
def test_startplan_pdbrc_in_the_folder_is_shown(tmp_path):
    """The folder holds a .pdbrc that writes a file: Ctrl-G R on t.py shows
    it, line by line, before pdb starts; Esc starts nothing and pdb never
    runs it.  pdb starts apart from the folder (tests/test_startplan_pdb_jdb_apart.py),
    so the box offers Use mine and names the file as what Run it loads."""
    folder = Folder(tmp_path)
    (folder.ws / ".pdbrc").write_text("!open('%s','w').close()\n" % folder.ran)
    with folder.session("t.py", SEED_PY) as s:
        box = _debug_box(s, "Command from ./.pdbrc")
        s.key(ESC, delay=0.8)
        s._drain(SETTLE)
    lines = _plan_lines(folder)
    assert "With Run it: ./.pdbrc (1 line)" in lines, lines
    assert "    !open('%s','w').close()" % folder.ran in lines, lines
    assert "This runs the folder's own program." in lines, lines
    assert "Your own setting: pdb with none of the folder's files" in box, box
    assert not folder.ran.exists(), "pdb ran the folder's .pdbrc after Esc"


# @scenario SC-compile-29
@needs_python
def test_startplan_pdbrc_in_home_is_the_users(tmp_path):
    """Started in the home folder, ./.pdbrc is ~/.pdbrc, the user's own:
    nothing is asked, and pdb runs it as it always did."""
    folder = Folder(tmp_path)
    (folder.home / ".pdbrc").write_text("!open('%s','w').close()\n" % folder.ran)
    with folder.session("t.py", SEED_PY, workdir=folder.home) as s:
        s.key(CTRL_G, "r", delay=1.0)
        ran = _wait(folder.ran.exists, RAN_WAIT)
        screen = _screen(s)
    assert "startcmd ask" not in folder.traced(), folder.traced()
    assert ran, "pdb did not start, or did not run the user's own .pdbrc:\n" + screen


# @scenario SC-compile-29
@needs_python
def test_startplan_changed_startup_file_asks_again(tmp_path):
    """Run it on a .pdbrc is kept for that folder and that text: the next
    start runs without a box; once the file changes, the box is back."""
    folder = Folder(tmp_path)
    rc = folder.ws / ".pdbrc"
    rc.write_text("!open('%s','w').close()\n" % folder.ran)
    with folder.session("t.py", SEED_PY) as s:
        _debug_box(s, "Command from ./.pdbrc")
        s.key(RUN_IT, delay=0.8)
        ran = _wait(folder.ran.exists, RAN_WAIT)
    assert ran, "Run it did not start pdb on the folder's .pdbrc"
    folder.ran.unlink()
    with folder.session("t.py", SEED_PY) as s:
        s.key(CTRL_G, "r", delay=1.0)
        again = _wait(folder.ran.exists, RAN_WAIT)
        screen = _screen(s)
    asks_before_change = folder.traced().count("startcmd ask debug/pdb")
    assert again, "the answered .pdbrc did not run again:\n" + screen
    assert asks_before_change == 1, folder.traced()
    rc.write_text("!open('%s','w').close()\n# changed\n" % folder.ran)
    with folder.session("t.py", SEED_PY) as s:
        _debug_box(s, "Command from ./.pdbrc")
        s.key(ESC, delay=0.8)
        s._drain(SETTLE)
    assert "With Run it: ./.pdbrc (2 lines)" in _plan_lines(folder), _plan_lines(folder)


# @scenario SC-compile-29
@needs_jdb
def test_startplan_jdb_ini_in_the_folder_is_shown(tmp_path):
    """The working folder holds a jdb.ini: Ctrl-G R on T.java names it with
    its lines before jdb starts, as what Run it loads; Esc starts nothing."""
    folder = Folder(tmp_path)
    (folder.ws / "jdb.ini").write_text("version\n")
    with folder.session("T.java", SEED_JAVA) as s:
        _debug_box(s, "Command from ./jdb.ini")
        s.key(ESC, delay=0.8)
        s._drain(SETTLE)
    lines = _plan_lines(folder)
    assert "With Run it: ./jdb.ini (1 line)" in lines, lines
    assert "    version" in lines, lines


GDB_AUTOLOAD = "set auto-load safe-path /\n"


# @scenario SC-compile-29
@needs_gdb
def test_startplan_gdbinit_in_the_folder_is_shown(tmp_path):
    """The user's gdb loads a folder's .gdbinit (his ~/.gdbinit allows any
    folder), and the folder holds one that runs a shell command: Ctrl-G R
    shows it with the condition, and Esc starts nothing."""
    folder = Folder(tmp_path)
    (folder.home / ".gdbinit").write_text(GDB_AUTOLOAD)
    (folder.ws / ".gdbinit").write_text("shell %s\n" % folder.touch_line())
    with folder.session("t.c", SEED_C) as s:
        box = _debug_box(s, "Command from ./.gdbinit")
        s.key(ESC, delay=0.8)
        s._drain(SETTLE)
    lines = _plan_lines(folder)
    assert "With Run it: ./.gdbinit (1 line)" in lines, lines
    assert not any("gdb runs it only if" in l for l in lines), lines
    assert "Your own setting: gdb with none of the folder's files" in box, box
    assert not folder.ran.exists(), "gdb ran the folder's .gdbinit after Esc"


# @scenario SC-compile-29
@needs_gdb
def test_startplan_gdbinit_use_mine_leaves_it_alone(tmp_path):
    """Use mine on that box starts gdb told to skip the folder's .gdbinit:
    the session starts, the shell command never runs, and "no" is kept for
    the folder."""
    folder = Folder(tmp_path)
    (folder.home / ".gdbinit").write_text(GDB_AUTOLOAD)
    (folder.ws / ".gdbinit").write_text("shell %s\n" % folder.touch_line())
    with folder.session("t.c", SEED_C) as s:
        _debug_box(s, "Command from ./.gdbinit")
        s.key(USE_MINE, delay=0.8)
        started = _wait(lambda: "startplan judge debug/gdb" in folder.traced() and
                        "startcmd answer debug/gdb own" in folder.traced(), RAN_WAIT)
        ran = _wait(folder.ran.exists, RAN_WAIT)
        alive = s.proc.poll() is None
    assert started, folder.traced()
    assert alive, "the editor died starting gdb"
    assert not ran, "gdb ran the folder's .gdbinit after Use mine"
    assert any(r.startswith("no\tdebug/gdb\t") for r in folder.records()), folder.records()


FOLDER_PDB = "[Programming]\nDebugger : 5\n"   # the start folder names pdb


def _asks(folder):
    """How many boxes the session showed, by the trace's ask lines."""
    return folder.traced().count("startcmd ask ")


# @scenario SC-compile-29
@needs_gdb
@needs_python
def test_startplan_folder_debugger_and_its_startup_file_ask_once(tmp_path):
    """The start folder's xwperc names pdb for a C file and the folder holds
    a .pdbrc: Ctrl-G R shows one box, naming the folder's debugger and the
    file it runs together, and Run it starts the session with no second
    box (the debugger used to be asked about in a box of its own first)."""
    folder = Folder(tmp_path)
    (folder.ws / "xwperc").write_text(FOLDER_PDB)
    (folder.ws / ".pdbrc").write_text("!open('%s','w').close()\n" % folder.ran)
    with folder.session("t.c", SEED_C) as s:
        _debug_box(s, "Command from ./xwperc")
        s.key(RUN_IT, delay=0.8)
        s._drain(SETTLE)
        alive = s.proc.poll() is None
    lines = _plan_lines(folder)
    assert "From ./xwperc: Debugger = pdb" in lines, lines
    assert "With Run it: ./.pdbrc (1 line)" in lines, lines
    assert _asks(folder) == 1, folder.traced()
    assert alive, "the editor died starting the folder's debugger"


# @scenario SC-compile-29
@needs_gdb
def test_startplan_folder_debugger_use_mine_starts_the_users(tmp_path):
    """Use mine on that box starts the user's own debugger, gdb, planned on
    the program it takes (the built t.e, not the source pdb would get): the
    folder's .pdbrc never runs, no second box opens, and "no" is kept under
    the debug start's key."""
    folder = Folder(tmp_path)
    (folder.ws / "xwperc").write_text(FOLDER_PDB)
    (folder.ws / ".pdbrc").write_text("!open('%s','w').close()\n" % folder.ran)
    with folder.session("t.c", SEED_C) as s:
        box = _debug_box(s, "Command from ./xwperc")
        rows = s.display()
        s.key(USE_MINE, delay=0.8)
        started = _wait(lambda: "startplan judge debug/gdb" in folder.traced(), RAN_WAIT)
        ran = _wait(folder.ran.exists, 4)
        alive = s.proc.poll() is None
    assert ("your own debugger (gdb) with none of the folder's files"
            in _own_line_joined(rows)), box
    assert started, folder.traced()
    assert "startcmd answer debug/pdb own" in folder.traced(), folder.traced()
    assert _asks(folder) == 1, folder.traced()
    assert alive, "the editor died starting the user's own debugger"
    assert not ran, "the folder's .pdbrc ran after Use mine"
    assert any(r.startswith("no\tdebug/pdb\t") for r in folder.records()), folder.records()


GDB_SCRIPT_PY = "open(%r, 'w').close()\n"


# @scenario SC-compile-29
@needs_gdb
def test_startplan_program_gdb_script_is_shown(tmp_path):
    """The user's gdb auto-loads from any folder, and beside the program the
    folder holds t.e-gdb.py, which gdb runs when it loads t.e: Ctrl-G R
    shows it with its lines and the condition, and Esc starts nothing."""
    folder = Folder(tmp_path)
    (folder.home / ".gdbinit").write_text(GDB_AUTOLOAD)
    (folder.ws / "t.e-gdb.py").write_text(GDB_SCRIPT_PY % str(folder.ran))
    with folder.session("t.c", SEED_C) as s:
        box = _debug_box(s, "Command from ./t.e-gdb.py")
        s.key(ESC, delay=0.8)
        ran = _wait(folder.ran.exists, 4)
    lines = _plan_lines(folder)
    assert "With Run it: ./t.e-gdb.py (1 line)" in lines, lines
    assert not any("gdb runs it only if" in l for l in lines), lines
    assert "    " + GDB_SCRIPT_PY.strip() % str(folder.ran) in lines, lines
    assert "Your own setting: gdb with none of the folder's files" in box, box
    assert not ran, "gdb ran the program's script after Esc"


# @scenario SC-compile-29
@needs_gdb
def test_startplan_program_gdb_scripts_use_mine_stops_them(tmp_path):
    """Use mine on that box, with a t.e-gdb.gdb beside the .py, starts gdb
    with neither loaded: the session starts, neither script runs, and
    "no" is kept for the folder."""
    folder = Folder(tmp_path)
    ran_gdb = folder.ws / "ran-gdb"
    (folder.home / ".gdbinit").write_text(GDB_AUTOLOAD)
    (folder.ws / "t.e-gdb.py").write_text(GDB_SCRIPT_PY % str(folder.ran))
    (folder.ws / "t.e-gdb.gdb").write_text("shell %s\n" % folder.touch_line(ran_gdb))
    with folder.session("t.c", SEED_C) as s:
        _debug_box(s, "Command from ./t.e-gdb")
        s.key(USE_MINE, delay=0.8)
        started = _wait(lambda: "startcmd answer debug/gdb own" in folder.traced(), RAN_WAIT)
        ran = _wait(lambda: folder.ran.exists() or ran_gdb.exists(), RAN_WAIT)
        alive = s.proc.poll() is None
    lines = _plan_lines(folder)
    assert "With Run it: ./t.e-gdb.gdb (1 line)" in lines, lines
    assert "With Run it: ./t.e-gdb.py (1 line)" in lines, lines
    assert started, folder.traced()
    assert alive, "the editor died starting gdb"
    assert not ran, "gdb ran a script beside the program after Use mine"
    assert any(r.startswith("no\tdebug/gdb\t") for r in folder.records()), folder.records()


# A program that carries a gdb script inside itself: its .debug_gdb_scripts
# section holds one entry, a Python file named emb.py (gdb looks for it in
# its working folder, which is the program's).
SEED_EMBEDDED = ("int main(void){return 0;}\n"
                 '__asm__(".pushsection .debug_gdb_scripts,\\"MS\\",@progbits,1\\n"\n'
                 '        ".byte 1\\n"\n'
                 '        ".asciz \\"emb.py\\"\\n"\n'
                 '        ".popsection\\n");\n')
EMBEDDED_TITLE = "Command from ./t.e (.debug_gdb_scripts)"


def _embedded_folder(tmp_path):
    """A folder whose program carries emb.py inside it, and a gdb that
    loads scripts from any folder."""
    folder = Folder(tmp_path)
    (folder.home / ".gdbinit").write_text(GDB_AUTOLOAD)
    (folder.ws / "emb.py").write_text(GDB_SCRIPT_PY % str(folder.ran))
    return folder


# @scenario SC-compile-29
@needs_gdb
def test_startplan_gdb_script_inside_the_program_is_shown(tmp_path):
    """The program's .debug_gdb_scripts section names emb.py, which gdb runs
    when it loads the program: Ctrl-G R lists the section as a start-up file
    of the program, with the script's name and lines and the condition, and
    Esc starts nothing."""
    folder = _embedded_folder(tmp_path)
    with folder.session("t.c", SEED_EMBEDDED) as s:
        box = _debug_box(s, EMBEDDED_TITLE)
        s.key(ESC, delay=0.8)
        ran = _wait(folder.ran.exists, 4)
    lines = _plan_lines(folder)
    assert "With Run it: ./t.e (.debug_gdb_scripts) (2 lines)" in lines, lines
    assert not any("gdb runs it only if" in l for l in lines), lines
    assert "    emb.py" in lines, lines
    assert "    " + GDB_SCRIPT_PY.strip() % str(folder.ran) in lines, lines
    assert "Your own setting: gdb with none of the folder's files" in box, box
    assert not ran, "gdb ran the program's own script after Esc"


# @scenario SC-compile-29
@needs_gdb
def test_startplan_gdb_script_inside_the_program_use_mine_stops_it(tmp_path):
    """Use mine on that box starts gdb with none of the program's own
    scripts loaded: the session starts, emb.py never runs, and "no" is kept
    for the folder."""
    folder = _embedded_folder(tmp_path)
    with folder.session("t.c", SEED_EMBEDDED) as s:
        _debug_box(s, EMBEDDED_TITLE)
        s.key(USE_MINE, delay=0.8)
        started = _wait(lambda: "startcmd answer debug/gdb own" in folder.traced(), RAN_WAIT)
        ran = _wait(folder.ran.exists, RAN_WAIT)
        alive = s.proc.poll() is None
    assert started, folder.traced()
    assert alive, "the editor died starting gdb"
    assert not ran, "gdb ran the program's own script after Use mine"
    assert any(r.startswith("no\tdebug/gdb\t") for r in folder.records()), folder.records()


# @scenario SC-compile-29
@needs_gdb
def test_startplan_gdb_script_inside_the_program_run_it_runs_it(tmp_path):
    """Run it on that box starts gdb as it is: the program's script runs."""
    folder = _embedded_folder(tmp_path)
    with folder.session("t.c", SEED_EMBEDDED) as s:
        _debug_box(s, EMBEDDED_TITLE)
        s.key(RUN_IT, delay=0.8)
        ran = _wait(folder.ran.exists, RAN_WAIT)
    assert ran, "Run it did not start gdb on the program's own script"


# @scenario SC-compile-29
@needs_gdb
def test_startplan_a_program_without_the_section_has_no_embedded_line(tmp_path):
    """An ordinary program: no start-up line for the section, no question."""
    folder = _embedded_folder(tmp_path)
    with folder.session("t.c", SEED_C) as s:
        s.key(CTRL_G, "r", delay=1.0)
        started = _wait(lambda: "startplan judge debug/gdb" in folder.traced(), BOX_WAIT)
        screen = _screen(s)
    assert started, folder.traced()
    assert ".debug_gdb_scripts" not in folder.traced(), folder.traced()
    assert "Command from" not in screen, screen


# @scenario SC-compile-29
@needs_gdb
def test_startplan_use_mine_line_is_wrapped_not_cut(tmp_path):
    """The start folder names pdb and holds a .pdbrc, so the line that names
    what Use mine starts ("your own debugger (gdb) with none of the folder's
    files") is longer than the box is wide.  It goes on in the next row like
    the lines above it, so every word of it is on screen."""
    folder = Folder(tmp_path)
    (folder.ws / "xwperc").write_text(FOLDER_PDB)
    (folder.ws / ".pdbrc").write_text("!open('%s','w').close()\n" % folder.ran)
    with folder.session("t.c", SEED_C) as s:
        box = _debug_box(s, "Command from ./xwperc")
        rows = s.display()
        s.key(ESC, delay=0.8)
        s._drain(SETTLE)
    shown = _own_line_joined(rows)
    assert "your own debugger (gdb) with none of the folder's files" in shown, \
        "the Use mine line was cut:\n" + box
    first = next(r for r in rows if "Your own setting" in r)
    assert "none of the folder's files" not in first, \
        "the line fits the box, so it does not test wrapping:\n" + box
    assert not folder.ran.exists(), "the folder's .pdbrc ran after Esc"


# @scenario SC-compile-29
@needs_gdb
def test_startplan_folder_arguments_reach_gdb_only_after_the_box(tmp_path):
    """The start folder's xwperc sets Arguments that end gdb's run command
    and start a shell command (gdb starts the program through a shell):
    Ctrl-G R shows the run command they make, and Esc runs nothing."""
    folder = Folder(tmp_path)
    (folder.ws / "xwperc").write_text("[Programming]\nArguments : x ; %s\n"
                                      % folder.touch_line())
    with folder.session("t.c", SEED_C) as s:
        _debug_box(s, "Command from ./xwperc")
        s.key(ESC, delay=0.8)
        ran = _wait(folder.ran.exists, 4)
    lines = _plan_lines(folder)
    assert "Then runs: r x ; %s" % folder.touch_line() in lines, lines
    assert "From ./xwperc: Arguments = x ; %s" % folder.touch_line() in lines, lines
    assert not ran, "the folder's Arguments ran a command after Esc"


# @scenario SC-compile-29
@needs_gdb
def test_startplan_a_program_named_like_an_option_is_a_file_to_gdb(tmp_path):
    """A source named -command=x.c builds -command=x.e; the folder holds a
    file x.e with a shell command.  gdb is handed ./-command=x.e, a program,
    never "-command=x.e", which gdb reads as its option "read commands from
    x.e".  (gdb 18 refuses "-x.e" as an unknown option, so the shorter name
    only stopped the session; this one ran the folder's file.)"""
    folder = Folder(tmp_path)
    (folder.ws / "x.e").write_text("shell %s\n" % folder.touch_line())
    with folder.session("./-command=x.c", SEED_C) as s:
        s.key(CTRL_G, "r", delay=1.0)
        built = _wait((folder.ws / "-command=x.e").exists, BOX_WAIT)
        ran = _wait(folder.ran.exists, RAN_WAIT)
        screen = _screen(s)
    assert built, "the program was not built:\n" + screen
    assert not ran, "gdb read the program's name as an option and ran x.e:\n" + screen
    assert "startplan judge debug/gdb" in folder.traced(), folder.traced()


PROJECT_WATCH = ("CMP=\tgcc\nCMPFLAGS=\t-g\nEXENAME=\tdemo\nFILES=\tmain.c\n"
                 "WATCHES=\tx\\nshell %s\n")
SEED_WATCHED = "int x = 1;\nint main(void){return x;}\n"


def _open_project(s):
    """Project > Open project, the Files box, the one .prj of the folder."""
    s.key(ALT.PROJECT, "p", delay=0.6)
    s.key(ENTER, delay=0.6)
    s.key(ENTER, delay=0.8)


# @scenario SC-compile-29
@needs_gdb
def test_startplan_a_project_watch_with_a_line_break_is_never_sent(tmp_path):
    """The project file gives a watch whose value, once its \\n is read,
    holds a second line: a shell command.  Opening the project does not take
    the watch (the status row says so), so no debug session of the project
    can send gdb that line; Ctrl-G R then runs nothing of it.  (In this
    setup the project's session itself stops at gdb's "No symbol table",
    on this tree and on the one before, so the watch is checked where it
    enters the editor.)"""
    folder = Folder(tmp_path)
    (folder.ws / "demo.prj").write_text(PROJECT_WATCH % folder.touch_line())
    with folder.session("main.c", SEED_WATCHED) as s:
        _open_project(s)
        refused = _wait(lambda: "debug not sent: a watch with a line break" in folder.traced(),
                        RAN_WAIT)
        screen = _screen(s)
        s.key(CTRL_G, "r", delay=1.0)
        ran = _wait(folder.ran.exists, RAN_WAIT)
    assert refused, "the project's watch was taken:\n" + screen
    assert "not sent: a watch with a line break" in screen, screen
    assert not ran, "a project's watch reached gdb as a command"


BOX_TEXT_W = 70            # the box's text width on an 80-column screen
WRAP_MARK = "\\"           # ends a row the line goes on after


def _box_rows_joined(rows, first, last):
    """The box's rows between the row holding @first and the one holding
    @last, read in the text's columns, each wrap mark taken off, joined:
    a line the box wrapped reads back whole, one it cut does not."""
    start = next(i for i, r in enumerate(rows) if first in r)
    col = rows[start].index(first)
    out = []
    for r in rows[start + 1:]:
        seg = r[col:col + BOX_TEXT_W].rstrip()
        if last in seg:
            break
        if seg.endswith(WRAP_MARK):
            seg = seg[:-len(WRAP_MARK)]
        out.append(seg)
    return "".join(out)


def _own_line_joined(rows):
    """The "Your own setting:" line of the box, from its first row to the
    row before "Folder:", each wrap mark taken off, joined."""
    first = "Your own setting:"
    start = next(i for i, r in enumerate(rows) if first in r)
    col = rows[start].index(first)
    out = []
    for r in rows[start:]:
        seg = r[col:col + BOX_TEXT_W].rstrip()
        if seg.startswith("Folder:"):
            break
        if seg.endswith(WRAP_MARK):
            seg = seg[:-len(WRAP_MARK)]
        out.append(seg)
    return "".join(out)


def _run_it_button(box):
    """Is Run it one of the box's buttons?  The box's text also says "With Run
    it:" over the files Run it loads, so only the row of buttons, the one that
    holds Cancel, answers."""
    buttons = [r for r in box.splitlines() if "Cancel" in r]
    return bool(buttons) and "Run it" in buttons[-1]


def _run_it_after_box(folder, title):
    """Ctrl-G R on t.py, Alt-R on the box titled @title; returns whether Run
    it was offered and whether the folder's command then ran."""
    with folder.session("t.py", SEED_PY) as s:
        box = _debug_box(s, title)
        s.key(RUN_IT, delay=0.8)
        ran = _wait(folder.ran.exists, RAN_WAIT)
    return _run_it_button(box), ran


# @scenario SC-compile-29
@needs_python
def test_startplan_a_nul_in_a_startup_file_hides_nothing(tmp_path):
    """The folder's .pdbrc has a harmless line ending in a NUL byte and a
    second line that writes a file.  The box can show text only up to the
    NUL, while pdb reads every line: the file is said to be not shown whole,
    Run it is not offered, and Alt-R runs nothing."""
    folder = Folder(tmp_path)
    (folder.ws / ".pdbrc").write_bytes(
        b"p 1\x00\n!open('%s','w').close()\n" % str(folder.ran).encode())
    offered, ran = _run_it_after_box(folder, "Command from ./.pdbrc")
    lines = _plan_lines(folder)
    assert any(l.startswith("With Run it: ./.pdbrc (") and "cannot be shown whole" in l
               for l in lines), lines
    assert not offered, "Run it was offered on a .pdbrc the box could not show whole"
    assert not ran, "pdb ran the line the NUL hid"


# @scenario SC-compile-29
@needs_python
def test_startplan_a_startup_line_wider_than_the_box_is_shown_whole(tmp_path):
    """A .pdbrc line holds a harmless command, blanks, and past the box's
    edge a second pdb command (";;") that writes a file.  The box wraps the
    line like the command instead of cutting it, so the end of the second
    command is on screen when Run it is offered."""
    folder = Folder(tmp_path)
    line = "p 1" + " " * 200 + ";;!open('%s','w').close()" % folder.ran
    (folder.ws / ".pdbrc").write_text(line + "\n")
    with folder.session("t.py", SEED_PY) as s:
        box = _debug_box(s, "Command from ./.pdbrc")
        rows = s.display()
        s.key(ESC, delay=0.8)
        s._drain(SETTLE)
    shown = _box_rows_joined(rows, "With Run it: ./.pdbrc", "This runs the folder")
    assert _run_it_button(box), box
    assert "    " + line in shown, "the start-up line was cut:\n" + box
    assert not folder.ran.exists(), "pdb ran the folder's .pdbrc after Esc"


# @scenario SC-compile-29
@needs_python
def test_startplan_a_pdb_separator_in_a_file_name_is_never_sent(tmp_path):
    """pdb runs each part of a line split at ";;" as a command, so a file
    named a;;!open('ran','w').close();#.py would reach pdb, in its own
    breakpoint command, as Python to run.  The breakpoint and the session
    are refused on the status row, and nothing runs."""
    folder = Folder(tmp_path)
    with folder.session("a;;!open('ran','w').close();#.py", SEED_PY) as s:
        s.key(CTRL_G, "b", delay=1.0)
        s.key(CTRL_G, "r", delay=1.0)
        ran = _wait(folder.ran.exists, RAN_WAIT)
        screen = _screen(s)
    assert "debug not sent: a file name with ;; in it" in folder.traced(), folder.traced()
    assert not ran, "a ;; in a file name reached pdb as a command:\n" + screen
