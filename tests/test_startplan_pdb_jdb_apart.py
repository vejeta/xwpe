"""pdb and jdb start apart from the program's folder, so they run nothing of
the folder's that the box did not show and the user did not accept.

pdb used to start in the program's folder and read its ./.pdbrc when it was
built, before the editor said a word; jdb read ./jdb.ini or ./.jdbrc the
same way.  Neither has a switch that turns the file off, so the box could
only show it, and "Use mine" did not exist for them.

Now each starts in an empty folder the editor made for the session, so the
"start-up file in the current folder" finds nothing.  The program's folder is
given to pdb by the editor's own first input line (a chdir that binds no name
in the program's module), and the files the box lists are loaded only by Run
it, by the editor's own commands: pdb gets the .pdbrc's lines one by one,
jdb gets "read <file>".  A folder module named pdb.py cannot replace the
debugger, because the folder is no longer where pdb starts.

HOME is the test's own.  Each route has a canary: the real debugger started
directly in the folder, with that HOME, must fire the folder's file, or the
row is skipped naming the broken probe, so an absent marker in a row means
containment and not a dead probe.
"""
import os
import re
import shutil
import stat
import subprocess

import pytest

from wpe_driver import ALT, WpeSession, tool_usable, wait_screen
from test_startplan_debug import (CTRL_G, ESC, RUN_IT, USE_MINE, RAN_WAIT, SETTLE,
                                  BOX_WAIT, Folder, _debug_box, _plan_lines, _screen,
                                  _wait)

needs_pdb = pytest.mark.skipif(not tool_usable("python3", "--version"),
                               reason="python3 not usable")
needs_jdb = pytest.mark.skipif(not (tool_usable("javac", "-version") and
                                    tool_usable("jdb", "-version")),
                               reason="javac and jdb not usable")

CANARY_S = 40              # a direct debugger run is far inside this
STOP_S = 25                # the debugger's start and its first stop
SEED_PY = "x = 1\nprint(x)\n"
DOWN = "\033OB"
ENTER = "\r"

PDB_OWN = "Your own setting: pdb with none of the folder's files"
JDB_OWN = "Your own setting: jdb with none of the folder's files"
PDB_MOVE = "!__import__('os').chdir(b'%s')"


def _touch_line(marker):
    return "!open(%r, 'w').close()\n" % str(marker)


def _clean_env(folder):
    return {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": str(folder.home),
            "TERM": "dumb"}


def _traced_sends(folder):
    """What the editor sent the debugger before it was set up, in order."""
    return [l.split("startplan send ", 1)[1] for l in folder.traced().splitlines()
            if "startplan send " in l]


def _debug_dirs(folder):
    """The session folders the owner reported, from the trace."""
    return re.findall(r"startplan contain \S+ apart=1 dir=(\S+)", folder.traced())


class PdbFolder(Folder):
    """A start folder holding t.py, a HOME whose ~/.pdbrc runs a command of
    its own, and the markers the rows look for."""

    def __init__(self, tmp_path):
        super().__init__(tmp_path)
        self.home_ran = tmp_path / "home-ran"
        self.shadow_ran = tmp_path / "shadow-ran"
        (self.home / ".pdbrc").write_text(_touch_line(self.home_ran))

    def folder_rc(self):
        (self.ws / ".pdbrc").write_text(_touch_line(self.ran))
        return "Command from ./.pdbrc"

    def markers(self):
        return [self.ran, self.home_ran, self.shadow_ran]

    def clear_markers(self):
        for m in self.markers():
            if m.exists():
                m.unlink()

    def session(self, seed=SEED_PY, extra=None):
        env = {"HOME": str(self.home), "XWPE_UI_TRACE": str(self.trace)}
        env.update(extra or {})
        return WpeSession(str(self.ws), seed, filename="t.py", env_extra=env)


def _pdb_canary(folder):
    """The real pdb, started directly in the folder with the test's HOME,
    must run the folder's file and the home one; else the row is skipped."""
    (folder.ws / "t.py").write_text(SEED_PY)
    subprocess.run(["python3", "-m", "pdb", "t.py"], cwd=str(folder.ws), env=_clean_env(folder),
                   input=b"q\n", timeout=CANARY_S, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    fired = folder.ran.exists() and folder.home_ran.exists()
    folder.clear_markers()
    (folder.ws / "t.py").unlink()
    if not fired:
        pytest.skip("canary: a pdb started directly in the folder did not run the folder's "
                    ".pdbrc and its HOME's own: this route's probe is dead here")


def _stopped(s):
    return s.mark_line() is not None


def _break_in_the_script(s):
    """A breakpoint on the second line, so the session stops there: pdb's own
    first stop, before the first line, is not shown."""
    s.key(DOWN, delay=0.1)
    s.key(CTRL_G, "b", delay=0.4)


def _start_pdb(s, folder, title, key):
    """A breakpoint on line 2, Ctrl-G R, the box titled @title, then @key; wait
    for the stop on it."""
    _break_in_the_script(s)
    box = _debug_box(s, title)
    s.key(key, delay=0.8)
    assert _wait(lambda: _stopped(s), STOP_S), \
        "pdb never stopped at the start:\n%s\n%s" % (folder.traced(), _screen(s))
    s._drain(SETTLE)
    return box


# @scenario SC-debug-117
@needs_pdb
def test_pdb_use_mine_loads_none_of_the_folders_files(tmp_path):
    """Use mine on the box that lists the folder's .pdbrc: the file never
    runs, and the user's own ~/.pdbrc still does."""
    folder = PdbFolder(tmp_path)
    title = folder.folder_rc()
    _pdb_canary(folder)
    with folder.session() as s:
        box = _start_pdb(s, folder, title, USE_MINE)
        home_ran = _wait(folder.home_ran.exists, RAN_WAIT)
    assert home_ran, "the user's own ~/.pdbrc did not run:\n" + folder.traced()
    assert not folder.ran.exists(), "pdb ran the folder's .pdbrc after Use mine"
    assert PDB_OWN in box, box


# @scenario SC-debug-117
@needs_pdb
def test_pdb_run_it_loads_the_listed_file(tmp_path):
    """Run it on that box: the .pdbrc's line runs, sent by the editor, and
    the box named it as what Run it makes the editor load."""
    folder = PdbFolder(tmp_path)
    title = folder.folder_rc()
    _pdb_canary(folder)
    with folder.session() as s:
        _start_pdb(s, folder, title, RUN_IT)
        ran = _wait(folder.ran.exists, RAN_WAIT)
        home_ran = folder.home_ran.exists()
    assert home_ran, "the user's own ~/.pdbrc did not run"
    assert ran, "Run it did not load the listed file:\n" + folder.traced()
    lines = _plan_lines(folder)
    assert any(l.startswith("With Run it: ./.pdbrc") for l in lines), lines
    assert not any(l.startswith("Runs at start: ") for l in lines), lines


# @scenario SC-debug-117
@needs_pdb
def test_pdb_a_clean_start_does_not_read_the_folders_pdbrc_or_module(tmp_path):
    """The folder holds a module named pdb.py, which a pdb started there
    would import in place of the debugger.  Nothing is listed, so no box
    opens; the debugger that runs is the system's, and the module never
    runs."""
    folder = PdbFolder(tmp_path)
    (folder.ws / "pdb.py").write_text("open(%r, 'w').close()\nraise SystemExit(0)\n"
                                      % str(folder.shadow_ran))
    (folder.ws / "t.py").write_text(SEED_PY)
    subprocess.run(["python3", "-m", "pdb", "t.py"], cwd=str(folder.ws), env=_clean_env(folder),
                   input=b"q\n", timeout=CANARY_S, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    fired = folder.shadow_ran.exists()
    folder.clear_markers()
    (folder.ws / "t.py").unlink()
    if not fired:
        pytest.skip("canary: a pdb started directly in the folder did not import the folder's "
                    "pdb.py: this route's probe is dead here")
    with folder.session() as s:
        _break_in_the_script(s)
        s.key(CTRL_G, "r", delay=1.0)
        stopped = _wait(lambda: _stopped(s), STOP_S)
        screen = _screen(s)
    assert stopped, "pdb never stopped at the start:\n" + screen
    assert "startcmd ask" not in folder.traced(), folder.traced()
    assert not folder.shadow_ran.exists(), "the folder's pdb.py replaced the debugger"


SEED_SIBLING = ("import os\nimport sib\n"
                "open(%r, 'w').write(os.getcwd() + '\\n' + str(sib.VALUE))\n")


# @scenario SC-debug-117
@needs_pdb
def test_pdb_the_program_runs_in_its_folder_and_imports_its_sibling(tmp_path):
    """pdb starts in the editor's empty folder, but the program it debugs
    runs in its own: its working folder is the program's and a module beside
    it imports."""
    folder = PdbFolder(tmp_path)
    (folder.ws / "sib.py").write_text("VALUE = 42\n")
    with folder.session(SEED_SIBLING % str(folder.ran)) as s:
        _break_in_the_script(s)
        s.key(CTRL_G, "r", delay=1.0)
        assert _wait(lambda: _stopped(s), STOP_S), _screen(s)
        s.key(CTRL_G, "r", delay=1.0)
        done = _wait(folder.ran.exists, RAN_WAIT * 2)
        s._drain(SETTLE)
    assert done, "the program never reached its end:\n" + _screen(s)
    cwd, value = folder.ran.read_text().split("\n")
    assert cwd == os.path.realpath(str(folder.ws)), cwd
    assert value == "42", value


# @scenario SC-debug-117
@needs_pdb
def test_pdb_gets_its_move_first_and_then_the_loaded_lines(tmp_path):
    """What the editor sends pdb before anything else: the move into the
    program's folder, then, after Run it, the .pdbrc's lines."""
    folder = PdbFolder(tmp_path)
    title = folder.folder_rc()
    ws = os.path.realpath(str(folder.ws))
    with folder.session() as s:
        _start_pdb(s, folder, title, RUN_IT)
    sends = _traced_sends(folder)
    assert sends[0] == PDB_MOVE % ws, sends
    assert sends[1] == _touch_line(folder.ran).rstrip("\n"), sends
    assert len(sends) == 2, sends


# @scenario SC-debug-117
@needs_pdb
def test_pdb_use_mine_sends_only_the_move(tmp_path):
    """Use mine sends the move and none of the folder's lines."""
    folder = PdbFolder(tmp_path)
    title = folder.folder_rc()
    ws = os.path.realpath(str(folder.ws))
    with folder.session() as s:
        _start_pdb(s, folder, title, USE_MINE)
    assert _traced_sends(folder) == [PDB_MOVE % ws], _traced_sends(folder)


# @scenario SC-debug-117
@needs_pdb
def test_pdb_its_own_folder_is_empty_and_gone_after_the_session(tmp_path):
    """The session's folder exists and is empty while pdb runs, and is
    removed when the session ends."""
    folder = PdbFolder(tmp_path)
    with folder.session() as s:
        _break_in_the_script(s)
        s.key(CTRL_G, "r", delay=1.0)
        assert _wait(lambda: _stopped(s), STOP_S), _screen(s)
        dirs = _debug_dirs(folder)
        during = [d for d in dirs if os.path.isdir(d)]
        empty = [d for d in during if not os.listdir(d)]
        s.menu(ALT.DEBUG, "q", delay=0.5)
        s._drain(SETTLE)
        after = [d for d in dirs if os.path.exists(d)]
    assert dirs, folder.traced()
    assert during and empty == during, "the session's folder was missing or held files: %s" % during
    assert not after, "the session's folder is still there after Quit: %s" % after


FAKE_PYTHON = """#!/bin/sh
if [ "$1" != "-m" ] || [ "$2" != "pdb" ]; then exec %s "$@"; fi
printf '> %%s/t.py(1)<module>()\\n-> x\\n(Pdb) ' "$PWD"
read first
printf '*** FileNotFoundError: no such folder\\n(Pdb) '
read second
"""
REFUSED_MOVE = "Debugger stopped: pdb could not change to the program's folder"


# @scenario SC-debug-117
@needs_pdb
def test_pdb_a_refused_move_stops_the_start_with_a_message(tmp_path):
    """A pdb that answers the editor's move with an error: the start stops
    and says so, instead of debugging from the editor's empty folder, and the
    session's folder is gone."""
    folder = PdbFolder(tmp_path)
    fake = tmp_path / "bin"
    fake.mkdir()
    (fake / "python3").write_text(FAKE_PYTHON % shutil.which("python3"))
    (fake / "python3").chmod(stat.S_IRWXU)
    path = "%s:%s" % (fake, os.environ.get("PATH", "/usr/bin:/bin"))
    with folder.session(extra={"PATH": path}) as s:
        s.key(CTRL_G, "r", delay=1.0)
        said = wait_screen(s, REFUSED_MOVE, tmo=STOP_S)
        screen = _screen(s)
        s.key(ENTER, delay=0.8)
        s._drain(SETTLE)
        marked = _stopped(s)
        dirs = _debug_dirs(folder)
        after = [d for d in dirs if os.path.exists(d)]
    assert said, "no message said why the start stopped:\n" + screen
    assert not marked, "the session went on after the move was refused"
    assert dirs and not after, "the session's folder was left: %s" % after


# @scenario SC-debug-117
@needs_pdb
def test_pdb_a_commands_block_has_no_run_it(tmp_path):
    """pdb answers a .pdbrc's "commands" with a prompt of its own, so its
    lines cannot be sent one at a time: Run it is not offered and says why;
    Use mine still starts pdb."""
    folder = PdbFolder(tmp_path)
    (folder.ws / ".pdbrc").write_text("break 1\ncommands 1\n" + _touch_line(folder.ran) + "end\n")
    with folder.session() as s:
        _break_in_the_script(s)
        box = _debug_box(s, "Command from ./.pdbrc")
        buttons = [r for r in box.splitlines() if "Use mine" in r and "Cancel" in r]
        assert buttons and "Run it" not in buttons[0], box
        assert "Run it is not offered:" in box, box
        s.key(USE_MINE, delay=0.8)
        assert _wait(lambda: _stopped(s), STOP_S), _screen(s)
    assert not folder.ran.exists(), "pdb ran the folder's .pdbrc after Use mine"


def _remember_the_old_text(folder):
    """Write, into the user's store, the answer Run it that an editor made
    before pdb started apart: the plan's text had no move line."""
    ws = os.path.realpath(str(folder.ws))
    content = (folder.ws / ".pdbrc").read_text()
    old_text = ("xwpe-start 2\nkind debug\ncwd %s\nargv python3\nargv -m\nargv pdb\n"
                "reads %s/.pdbrc %d\n%s" % (ws, ws, len(content), content))
    escaped = old_text.replace("\\", "\\\\").replace("\n", "\\n")
    folder.store.parent.mkdir(parents=True, exist_ok=True)
    folder.store.write_text("xwpe-start-commands 2\nyes\tdebug/pdb\t%s\t%s\n" % (ws, escaped))


# @scenario SC-debug-117
@needs_pdb
def test_pdb_an_answer_remembered_before_asks_once_more(tmp_path):
    """An answer remembered by an editor that started pdb in the folder is a
    different text from the one the contained start makes: the box asks once
    more, and the answer then given is kept."""
    folder = PdbFolder(tmp_path)
    title = folder.folder_rc()
    _remember_the_old_text(folder)
    with folder.session() as s:
        _start_pdb(s, folder, title, RUN_IT)
    assert folder.traced().count("startcmd ask debug/pdb") == 1, folder.traced()
    with folder.session() as s:
        _break_in_the_script(s)
        s.key(CTRL_G, "r", delay=1.0)
        assert _wait(lambda: _stopped(s), STOP_S), _screen(s)
    assert folder.traced().count("startcmd ask debug/pdb") == 1, \
        "the new answer was not kept:\n" + folder.traced()


# jdb: P (as the integration rows write it) with a breakpoint the folder's
# jdb.ini sets on line 8 and one the user sets on line 6.
JAVA_PROG = (
    "public class P {\n"
    "    static int sq(int v) {\n"
    "        return v * v;\n"
    "    }\n"
    "    public static void main(String[] a) throws Exception {\n"
    "        int x = 3;\n"
    "        int y = sq(x);\n"
    "        x = y + 1;\n"
    "        java.nio.file.Files.write(java.nio.file.Paths.get(\"%s\"),\n"
    "            new java.io.File(\".\").getCanonicalPath().getBytes());\n"
    "    }\n"
    "}\n"
)
JAVA_USER_LINE = 6
JAVA_INI_LINE = 8
JDB_INI = "stop at P:%d\n" % JAVA_INI_LINE


class JdbFolder(Folder):
    """A start folder holding P.java.  jdb's home is the account's, whatever
    $HOME says, so each row gives the JVMs the test's home by their options."""

    def __init__(self, tmp_path):
        super().__init__(tmp_path)
        self.cwd_file = tmp_path / "program-cwd"

    def folder_ini(self, text=JDB_INI):
        (self.ws / "jdb.ini").write_text(text)
        return "Command from ./jdb.ini"

    def seed(self):
        return JAVA_PROG % self.cwd_file

    def env(self):
        return {"HOME": str(self.home), "XWPE_UI_TRACE": str(self.trace),
                "JAVA_TOOL_OPTIONS": "-Duser.home=%s" % self.home}

    def session(self):
        return WpeSession(str(self.ws), self.seed(), filename="P.java", wait=1.5,
                          env_extra=self.env())


def _jdb_canary(folder):
    """The real jdb, started directly in the folder, must read the folder's
    jdb.ini; else the row is skipped naming the broken probe."""
    env = _clean_env(folder)
    env["JAVA_TOOL_OPTIONS"] = "-Duser.home=%s" % folder.home
    out = subprocess.run(["jdb", "-J-Duser.language=en", "-J-Duser.country=US"],
                         cwd=str(folder.ws), env=env, input=b"quit\n", timeout=CANARY_S,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout.decode(
                             errors="replace")
    if "Reading commands from" not in out or "jdb.ini" not in out:
        pytest.skip("canary: a jdb started directly in the folder did not read its jdb.ini: "
                    "this route's probe is dead here")


def _start_jdb(s, folder, title, key):
    """A breakpoint on line 6, Ctrl-G R, the box titled @title, then @key;
    wait for the stop on line 6."""
    for _ in range(JAVA_USER_LINE - 1):
        s.key(DOWN, delay=0.1)
    s.key(CTRL_G, "b", delay=0.4)
    box = _debug_box(s, title)
    s.key(key, delay=0.8)
    assert _wait(lambda: s.mark_line() == JAVA_USER_LINE, STOP_S), \
        "jdb never stopped on line %d:\n%s\n%s" % (JAVA_USER_LINE, folder.traced(), _screen(s))
    s._drain(SETTLE)
    return box


def _run_on(s):
    """Ctrl-G R once more: the program runs to the next breakpoint or its end."""
    s.key(CTRL_G, "r", delay=1.0)
    s._drain(SETTLE * 2)


# @scenario SC-debug-118
@needs_jdb
def test_jdb_use_mine_loads_none_of_the_folders_files(tmp_path):
    """Use mine on the box that lists the folder's jdb.ini, which sets a
    breakpoint on line 8: the program runs on past it to its end."""
    folder = JdbFolder(tmp_path)
    title = folder.folder_ini()
    _jdb_canary(folder)
    with folder.session() as s:
        box = _start_jdb(s, folder, title, USE_MINE)
        _run_on(s)
        stopped_on = s.mark_line()
    assert JDB_OWN in box, box
    assert stopped_on != JAVA_INI_LINE, \
        "jdb read the folder's jdb.ini after Use mine:\n" + folder.traced()


# @scenario SC-debug-118
@needs_jdb
def test_jdb_run_it_loads_the_listed_file(tmp_path):
    """Run it on that box: the editor sends "read <folder>/jdb.ini", and the
    program stops on the line the file names."""
    folder = JdbFolder(tmp_path)
    title = folder.folder_ini()
    _jdb_canary(folder)
    with folder.session() as s:
        _start_jdb(s, folder, title, RUN_IT)
        _run_on(s)
        reached = _wait(lambda: s.mark_line() == JAVA_INI_LINE, STOP_S)
        screen = _screen(s)
    assert reached, "Run it did not load the listed jdb.ini:\n%s\n%s" % (folder.traced(), screen)
    ws = os.path.realpath(str(folder.ws))
    assert "read %s/jdb.ini" % ws in _traced_sends(folder), _traced_sends(folder)
    lines = _plan_lines(folder)
    assert any(l.startswith("With Run it: ./jdb.ini") for l in lines), lines


# @scenario SC-debug-118
@needs_jdb
def test_jdb_the_program_runs_in_its_folder_and_the_stop_line_is_found(tmp_path):
    """jdb starts in the editor's empty folder, whose source path holds
    nothing: the stop line is still read, and the program the editor starts
    for jdb runs in its own folder."""
    folder = JdbFolder(tmp_path)
    with folder.session() as s:
        for _ in range(JAVA_USER_LINE - 1):
            s.key(DOWN, delay=0.1)
        s.key(CTRL_G, "b", delay=0.4)
        s.key(CTRL_G, "r", delay=1.0)
        assert _wait(lambda: s.mark_line() == JAVA_USER_LINE, STOP_S), \
            "the stop on line %d was not marked:\n%s" % (JAVA_USER_LINE, _screen(s))
        _run_on(s)
        done = _wait(folder.cwd_file.exists, RAN_WAIT * 2)
    assert done, "the program never reached its end"
    assert folder.cwd_file.read_text() == os.path.realpath(str(folder.ws)), folder.cwd_file.read_text()


# @scenario SC-debug-118
@needs_jdb
def test_jdb_its_own_folder_is_empty_and_gone_after_the_session(tmp_path):
    """The session's folder exists and is empty while jdb runs, and is
    removed when the session ends."""
    folder = JdbFolder(tmp_path)
    with folder.session() as s:
        for _ in range(JAVA_USER_LINE - 1):
            s.key(DOWN, delay=0.1)
        s.key(CTRL_G, "b", delay=0.4)
        s.key(CTRL_G, "r", delay=1.0)
        assert _wait(lambda: s.mark_line() == JAVA_USER_LINE, STOP_S), _screen(s)
        dirs = _debug_dirs(folder)
        during = [d for d in dirs if os.path.isdir(d)]
        empty = [d for d in during if not os.listdir(d)]
        s.menu(ALT.DEBUG, "q", delay=0.5)
        s._drain(SETTLE * 2)
        after = [d for d in dirs if os.path.exists(d)]
    assert dirs, folder.traced()
    assert during and empty == during, "the session's folder was missing or held files: %s" % during
    assert not after, "the session's folder is still there after Quit: %s" % after


JDB_REFUSED = "Debugger stopped: jdb could not read the start-up file"


# @scenario SC-debug-118
@needs_jdb
def test_jdb_a_refused_read_stops_the_start_with_a_message(tmp_path):
    """The folder's jdb.ini is gone between the box and Run it: jdb says it
    could not open it, and the start stops and says so, instead of debugging
    without the file the user accepted."""
    folder = JdbFolder(tmp_path)
    title = folder.folder_ini()
    with folder.session() as s:
        for _ in range(JAVA_USER_LINE - 1):
            s.key(DOWN, delay=0.1)
        s.key(CTRL_G, "b", delay=0.4)
        _debug_box(s, title)
        (folder.ws / "jdb.ini").unlink()
        s.key(RUN_IT, delay=0.8)
        said = wait_screen(s, JDB_REFUSED, tmo=STOP_S)
        screen = _screen(s)
        s.key(ENTER, delay=0.8)
        s._drain(SETTLE * 2)
        marked = s.mark_line()
        dirs = _debug_dirs(folder)
        after = [d for d in dirs if os.path.exists(d)]
    assert said, "no message said why the start stopped:\n" + screen
    assert marked != JAVA_USER_LINE, "the session went on after the read was refused"
    assert dirs and not after, "the session's folder was left: %s" % after


# @scenario SC-debug-118
@needs_jdb
def test_jdb_a_folder_with_a_blank_has_no_run_it(tmp_path):
    """jdb's read takes one word, so a jdb.ini in a folder whose name holds a
    blank cannot be loaded: Run it is not offered, and Use mine still starts
    jdb with none of the folder's files."""
    folder = JdbFolder(tmp_path)
    folder.ws = tmp_path / "a b"
    folder.ws.mkdir()
    title = folder.folder_ini()
    with folder.session() as s:
        for _ in range(JAVA_USER_LINE - 1):
            s.key(DOWN, delay=0.1)
        s.key(CTRL_G, "b", delay=0.4)
        box = _debug_box(s, title)
        buttons = [r for r in box.splitlines() if "Use mine" in r and "Cancel" in r]
        assert buttons and "Run it" not in buttons[0], box
        assert "Run it is not offered:" in box, box
        s.key(USE_MINE, delay=0.8)
        started = _wait(lambda: s.mark_line() == JAVA_USER_LINE, STOP_S)
    assert started, folder.traced()
    assert "read " not in " ".join(_traced_sends(folder)), _traced_sends(folder)
