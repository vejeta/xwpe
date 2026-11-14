"""gdb starts apart from the program's folder, so it runs nothing of the
folder's that the box did not show and the user did not accept.

gdb used to start in the program's folder and read, before the editor said a
word, that folder's ./.gdbinit, the script beside the program, the scripts
the program carries inside itself and, when it loaded them, the scripts of
the shared libraries it ran; whether it ran them was decided by the user's
own auto-load setting, which the editor cannot read.  Every enumeration of
what gdb loads is behind by one more place.

Now gdb starts in an empty folder the editor made for the session, so its
"init file in the current directory" finds nothing; its auto-load trust is
pinned to gdb's own default before any program is loaded, whatever
~/.gdbinit set; and the program's folder is given to it by the editor's own
commands.  The files the box lists are loaded only by Run it, by the
editor's own command.  What the listing misses (a shared library's script)
cannot load at all.

Each row writes what it observes into the folder it runs in.  HOME is the
test's own and its ~/.gdbinit widens gdb's auto-load trust to every folder
and runs a command of its own, so a gdb that is not contained fires the
folder's markers, and the user's own init still runs in every row.  Each
route has a canary: the real gdb started directly in the folder, with that
HOME, must fire the marker, or the row is skipped naming the broken probe,
so an absent marker in a row means containment and not a dead probe.
"""
import os
import re
import subprocess

import pytest

from wpe_driver import ALT, WAIT_SCALE, WpeSession, tool_usable
from test_startplan_debug import (CTRL_G, ENTER, ESC, RUN_IT, SEED_C, USE_MINE,
                                  RAN_WAIT, SETTLE, BOX_WAIT, Folder, _debug_box,
                                  _own_line_joined, _plan_lines, _screen, _wait)

needs_gdb = pytest.mark.skipif(not (tool_usable("gcc", "--version") and
                                    tool_usable("gdb", "--version")),
                               reason="gcc and gdb not usable")

INTERFACES = ["cli", "mi"]
CANARY_S = 40              # a direct gdb run is far inside this
STOP_S = 25                # a build, gdb's start and its first stop
LOADED_S = 25              # the editor's last line reached gdb by then
PIN = "set auto-load safe-path $debugdir:$datadir/auto-load"
MI_PIN = "-gdb-set auto-load safe-path $debugdir:$datadir/auto-load"
HOME_INIT = "set auto-load safe-path /\nshell touch %s\n"

# A program carrying its gdb script inside itself, as a file entry (emb.py,
# looked up in gdb's working folder) and as a text entry (the script itself,
# its first line the name).
SEED_FILE_ENTRY = ("int main(void){return 0;}\n"
                   '__asm__(".pushsection .debug_gdb_scripts,\\"MS\\",@progbits,1\\n"\n'
                   '        ".byte 1\\n"\n'
                   '        ".asciz \\"emb.py\\"\\n"\n'
                   '        ".popsection\\n");\n')
SEED_TEXT_ENTRY = r'''int main(void){return 0;}
__asm__(".pushsection .debug_gdb_scripts,\"MS\",@progbits,1\n"
        ".byte 4\n"
        ".asciz \"inline-script\\nopen('%s','w').close()\"\n"
        ".popsection\n");
'''
# A program that loads a shared library of the folder, which has a script
# beside it and one inside it.
SEED_LOADS_LIBRARY = ('#include <dlfcn.h>\n'
                      'int main(void){void *h = dlopen("./libx.so", RTLD_NOW); return h ? 0 : 1;}\n')
LIBRARY_C = ("int lib_f(void){return 1;}\n"
             '__asm__(".pushsection .debug_gdb_scripts,\\"MS\\",@progbits,1\\n"\n'
             '        ".byte 1\\n"\n'
             '        ".asciz \\"libemb.py\\"\\n"\n'
             '        ".popsection\\n");\n')
PY_TOUCH = "open(%r, 'w').close()\n"


class GdbFolder(Folder):
    """A start folder, a HOME whose ~/.gdbinit widens gdb's trust and runs a
    command of its own, and the markers the rows look for."""

    def __init__(self, tmp_path, interface):
        super().__init__(tmp_path)
        self.interface = interface
        self.home_ran = tmp_path / "home-ran"
        self.lib_beside = tmp_path / "lib-beside-ran"
        self.lib_inside = tmp_path / "lib-inside-ran"
        (self.home / ".gdbinit").write_text(HOME_INIT % self.home_ran)

    def gdbinit(self):
        (self.ws / ".gdbinit").write_text("shell %s\n" % self.touch_line())
        return SEED_C, "Command from ./.gdbinit"

    def beside(self):
        (self.ws / "t.e-gdb.py").write_text(PY_TOUCH % str(self.ran))
        return SEED_C, "Command from ./t.e-gdb.py"

    def inline_text(self):
        return SEED_TEXT_ENTRY % str(self.ran), "Command from ./t.e (.debug_gdb_scripts)"

    def inline_file(self):
        (self.ws / "emb.py").write_text(PY_TOUCH % str(self.ran))
        return SEED_FILE_ENTRY, "Command from ./t.e (.debug_gdb_scripts)"

    def library(self):
        (self.ws / "libx.c").write_text(LIBRARY_C)
        (self.ws / "libx.so-gdb.py").write_text(PY_TOUCH % str(self.lib_beside))
        (self.ws / "libemb.py").write_text(PY_TOUCH % str(self.lib_inside))
        subprocess.run(["gcc", "-g", "-shared", "-fPIC", "-o", str(self.ws / "libx.so"),
                        str(self.ws / "libx.c")], check=True, timeout=CANARY_S)
        return SEED_LOADS_LIBRARY

    def env(self):
        env = {"HOME": str(self.home), "XWPE_UI_TRACE": str(self.trace)}
        if self.interface == "mi":
            env["XWPE_GDB_INTERFACE"] = "mi"
        return env

    def session(self, seed, filename="t.c"):
        return WpeSession(str(self.ws), seed, filename=filename, env_extra=self.env())

    def markers(self):
        return [self.ran, self.lib_beside, self.lib_inside]

    def clear_markers(self):
        for m in self.markers() + [self.home_ran]:
            if m.exists():
                m.unlink()


def _build(folder, seed, filename="t.c"):
    """The program the editor will build, built here for the canary."""
    (folder.ws / filename).write_text(seed)
    subprocess.run(["gcc", "-g", "-o", str(folder.ws / "t.e"), str(folder.ws / filename)],
                   check=True, timeout=CANARY_S)


def _canary(folder, seed, run=False):
    """The real gdb, started directly in the folder with the test's HOME,
    must fire the folder's markers; else the row is skipped naming the probe."""
    _build(folder, seed)
    args = ["gdb", "-q", "-batch"]
    if run:
        args += ["-ex", "run"]
    args.append(str(folder.ws / "t.e"))
    env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": str(folder.home),
           "TERM": "dumb"}
    subprocess.run(args, cwd=str(folder.ws), env=env, timeout=CANARY_S,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    fired = {m.name for m in folder.markers() if m.exists()}
    ok = folder.home_ran.exists() and bool(fired)
    folder.clear_markers()
    if not ok:
        pytest.skip("canary: a gdb started directly in the folder fired %s and its HOME's "
                    "own init %s: this route's probe is dead here"
                    % (sorted(fired) or "no marker", "ran" if folder.home_ran.exists() else "did not run"))


def _traced_sends(folder):
    """What the editor sent gdb before the program was set up, in order."""
    return [l.split("startplan send ", 1)[1] for l in folder.traced().splitlines()
            if "startplan send " in l]


def _last_line_sent(folder):
    sends = _traced_sends(folder)
    return any(s.startswith(("file ", "-file-exec-and-symbols ")) for s in sends)


def _answer_box(s, folder, title, key):
    """Ctrl-G R, the box titled @title, then @key; returns the box's screen."""
    box = _debug_box(s, title)
    s.key(key, delay=0.8)
    assert _wait(lambda: s.mark_line() is not None, STOP_S), \
        "gdb never stopped at the start:\n%s\n%s" % (folder.traced(), _screen(s))
    s._drain(SETTLE)
    return box


KINDS = ["gdbinit", "beside", "inline_text", "inline_file"]


def _kind_folder(tmp_path, interface, kind):
    folder = GdbFolder(tmp_path, interface)
    seed, title = getattr(folder, kind)()
    _canary(folder, seed)
    (folder.ws / "t.e").unlink()
    return folder, seed, title


# @scenario SC-debug-115
@needs_gdb
@pytest.mark.parametrize("interface", INTERFACES)
@pytest.mark.parametrize("kind", KINDS)
def test_gdb_use_mine_loads_none_of_the_folders_files(tmp_path, interface, kind):
    """Use mine on the box that lists the folder's file: the file never
    runs, and the user's own ~/.gdbinit still does."""
    folder, seed, title = _kind_folder(tmp_path, interface, kind)
    with folder.session(seed) as s:
        box = _answer_box(s, folder, title, USE_MINE)
        home_ran = _wait(folder.home_ran.exists, RAN_WAIT)
        alive = s.proc.poll() is None
    assert alive, "the editor died starting gdb"
    assert home_ran, "the user's own ~/.gdbinit did not run:\n" + folder.traced()
    assert not folder.ran.exists(), "gdb ran the folder's file after Use mine"
    assert "with none of the folder's files" in box, box


# @scenario SC-debug-115
@needs_gdb
@pytest.mark.parametrize("interface", INTERFACES)
@pytest.mark.parametrize("kind", KINDS)
def test_gdb_run_it_loads_the_listed_file(tmp_path, interface, kind):
    """Run it on that box: the file runs, loaded by the editor's own
    command, and the box said so as what Run it makes the editor load."""
    folder, seed, title = _kind_folder(tmp_path, interface, kind)
    with folder.session(seed) as s:
        _answer_box(s, folder, title, RUN_IT)
        ran = _wait(folder.ran.exists, RAN_WAIT)
        home_ran = folder.home_ran.exists()
    assert home_ran, "the user's own ~/.gdbinit did not run"
    assert ran, "Run it did not load the listed file:\n" + folder.traced()
    assert any(l.startswith("With Run it: ") for l in _plan_lines(folder)), _plan_lines(folder)
    assert not any(l.startswith("Runs at start: ") for l in _plan_lines(folder)), _plan_lines(folder)


def _start_and_run_to_the_end(s, folder):
    """Ctrl-G R (build, start, stop at main), then Ctrl-G R once more: the
    program runs on and loads whatever it loads."""
    s.key(CTRL_G, "r", delay=1.0)
    assert _wait(lambda: s.mark_line() is not None, STOP_S), \
        "gdb never stopped at the start:\n%s\n%s" % (folder.traced(), _screen(s))
    s.key(CTRL_G, "r", delay=1.0)
    s._drain(SETTLE * 2)


# @scenario SC-debug-115
@needs_gdb
@pytest.mark.parametrize("interface", INTERFACES)
def test_gdb_a_clean_start_loads_no_script_of_a_library(tmp_path, interface):
    """Nothing the box could list: the start is clean and asks nothing.  The
    program loads a library of the folder that has a script beside it and
    one inside it; gdb's trust is its default, so neither runs.  This is the
    route no listing can cover."""
    folder = GdbFolder(tmp_path, interface)
    seed = folder.library()
    _canary(folder, seed, run=True)
    (folder.ws / "t.e").unlink()
    with folder.session(seed) as s:
        _start_and_run_to_the_end(s, folder)
        asked = "startcmd ask" in folder.traced()
        home_ran = _wait(folder.home_ran.exists, RAN_WAIT)
    assert not asked, "a start with nothing listed asked a question:\n" + folder.traced()
    assert home_ran, "the user's own ~/.gdbinit did not run"
    assert not folder.lib_beside.exists(), "gdb ran the library's script beside it"
    assert not folder.lib_inside.exists(), "gdb ran the script inside the library"


# @scenario SC-debug-115
@needs_gdb
@pytest.mark.parametrize("interface", INTERFACES)
def test_gdb_run_it_loads_what_is_listed_and_not_the_librarys(tmp_path, interface):
    """The folder holds a .gdbinit (listed) and the library with its two
    scripts (not): Run it runs the .gdbinit and neither library script."""
    folder = GdbFolder(tmp_path, interface)
    seed = folder.library()
    (folder.ws / ".gdbinit").write_text("shell %s\n" % folder.touch_line())
    _canary(folder, seed, run=True)
    (folder.ws / "t.e").unlink()
    with folder.session(seed) as s:
        _debug_box(s, "Command from ./.gdbinit")
        s.key(RUN_IT, delay=0.8)
        assert _wait(lambda: s.mark_line() is not None, STOP_S), _screen(s)
        s.key(CTRL_G, "r", delay=1.0)
        s._drain(SETTLE * 2)
        ran = folder.ran.exists()
    assert ran, "Run it did not run the listed .gdbinit"
    assert not folder.lib_beside.exists(), "Run it ran the library's script beside it"
    assert not folder.lib_inside.exists(), "Run it ran the script inside the library"


# @scenario SC-debug-115
@needs_gdb
@pytest.mark.parametrize("interface", INTERFACES)
def test_gdb_gets_its_lines_in_order_and_the_pin_is_first(tmp_path, interface):
    """What the editor sends gdb before anything else: the pin of its
    auto-load trust first, then (gdb's console only) the two lines that turn
    off gdb 18's error and warning symbols, then the move to the program's
    folder, the history question, and the program last, in gdb's own words
    for the interface the user chose."""
    folder = GdbFolder(tmp_path, interface)
    with folder.session(SEED_C) as s:
        s.key(CTRL_G, "r", delay=1.0)
        assert _wait(lambda: _last_line_sent(folder), STOP_S + LOADED_S), folder.traced()
        s._drain(SETTLE)
    sends = _traced_sends(folder)
    ws = os.path.realpath(str(folder.ws))
    if interface == "cli":
        assert sends[0] == PIN, sends
        assert sends[1] == "set style error-prefix", sends
        assert sends[2] == "set style warning-prefix", sends
        assert sends[3] == "cd %s" % ws, sends
        assert sends[4] == "show history filename", sends
        assert sends[5] == "set history filename %s/.gdb_history" % ws, sends
        assert sends[6] == "file %s/t.e" % ws, sends
    else:
        assert sends[0] == MI_PIN, sends
        assert sends[1] == '-environment-cd "%s"' % ws, sends
        assert sends[2] == "-gdb-show history filename", sends
        assert sends[3] == ('-interpreter-exec console "set history filename %s/.gdb_history"' % ws), sends
        assert sends[4] == '-file-exec-and-symbols "%s/t.e"' % ws, sends
    assert len(sends) == (7 if interface == "cli" else 5), sends


# @scenario SC-debug-115
@needs_gdb
def test_gdb_history_the_user_placed_is_left_alone(tmp_path):
    """A user who put gdb's history file elsewhere (set history filename in
    his own init) keeps it: the editor asks and sends no move."""
    folder = GdbFolder(tmp_path, "cli")
    with open(str(folder.home / ".gdbinit"), "a") as init:
        init.write("set history filename %s\n" % (tmp_path / "my-history"))
    with folder.session(SEED_C) as s:
        s.key(CTRL_G, "r", delay=1.0)
        assert _wait(lambda: _last_line_sent(folder), STOP_S + LOADED_S), folder.traced()
    sends = _traced_sends(folder)
    assert "show history filename" in sends, sends
    assert not any(x.startswith("set history filename") for x in sends), sends


def _debug_dirs(folder):
    """The session folders the owner reported, from the trace."""
    return re.findall(r"startplan contain \S+ apart=1 dir=(\S+)", folder.traced())


# @scenario SC-debug-115
@needs_gdb
@pytest.mark.parametrize("interface", INTERFACES)
def test_gdb_its_own_folder_is_gone_after_the_session(tmp_path, interface):
    """The session's folder exists while gdb runs and is removed when the
    session ends."""
    folder = GdbFolder(tmp_path, interface)
    with folder.session(SEED_C) as s:
        s.key(CTRL_G, "r", delay=1.0)
        assert _wait(lambda: s.mark_line() is not None, STOP_S), _screen(s)
        dirs = _debug_dirs(folder)
        during = [d for d in dirs if os.path.isdir(d)]
        s.menu(ALT.DEBUG, "q", delay=0.5)
        s._drain(SETTLE)
        after = [d for d in dirs if os.path.exists(d)]
    assert dirs, folder.traced()
    assert during, "the session's folder did not exist while gdb ran: %s" % dirs
    assert not after, "the session's folder is still there after Quit: %s" % after


FORTY = 40


# @scenario SC-debug-115
@needs_gdb
def test_gdb_forty_starts_leave_no_folder(tmp_path):
    """Forty debug starts in one editor: each folder is ended with its
    session, so none is left and the owner's table never fills."""
    folder = GdbFolder(tmp_path, "cli")
    with folder.session(SEED_C) as s:
        for n in range(FORTY):
            s.key(CTRL_G, "r", delay=0.6)
            assert _wait(lambda: s.mark_line() is not None, STOP_S), \
                "start %d never stopped:\n%s" % (n, _screen(s))
            s.menu(ALT.DEBUG, "q", delay=0.3)
            assert _wait(lambda: s.mark_line() is None, STOP_S), \
                "start %d never ended" % n
    dirs = _debug_dirs(folder)
    left = [d for d in dirs if os.path.exists(d)]
    assert len(dirs) == FORTY, "%d starts were contained, not %d" % (len(dirs), FORTY)
    assert not left, "%d session folders left: %s" % (len(left), left[:3])


# @scenario SC-debug-115
@needs_gdb
@pytest.mark.parametrize("interface", INTERFACES)
def test_gdb_the_program_opens_a_file_by_a_relative_name(tmp_path, interface):
    """The program runs in its own folder, not in gdb's empty one: it finds
    data.txt by a relative name."""
    folder = GdbFolder(tmp_path, interface)
    (folder.ws / "data.txt").write_text("x\n")
    seed = ('#include <stdio.h>\nint main(void){FILE *f = fopen("data.txt", "r");\n'
            ' if (f) { fclose(f); f = fopen("%s", "w"); if (f) fclose(f); }\n'
            ' return 0; }\n') % folder.ran
    with folder.session(seed) as s:
        _start_and_run_to_the_end(s, folder)
        found = _wait(folder.ran.exists, RAN_WAIT)
    assert found, "the program did not find data.txt: gdb's folder is not the program's"


def _remember_the_old_text(folder):
    """Write, into the user's store, the answer Run it that an editor made
    before gdb started apart: the plan's text had no containment lines."""
    ws = os.path.realpath(str(folder.ws))
    content = (folder.ws / ".gdbinit").read_text()
    old_text = ("xwpe-start 2\nkind debug\ncwd %s\nargv gdb\nreads %s/.gdbinit %d\n%s"
                % (ws, ws, len(content), content))
    escaped = old_text.replace("\\", "\\\\").replace("\n", "\\n")
    folder.store.parent.mkdir(parents=True, exist_ok=True)
    folder.store.write_text("xwpe-start-commands 2\nyes\tdebug/gdb\t%s\t%s\n" % (ws, escaped))


# @scenario SC-debug-115
@needs_gdb
def test_gdb_an_answer_remembered_before_asks_once_more(tmp_path):
    """An answer remembered for a gdb start by an editor that started gdb in
    the folder is a different text from the one the contained start makes:
    the box asks once more, and the answer then given is kept."""
    folder = GdbFolder(tmp_path, "cli")
    seed, title = folder.gdbinit()
    _remember_the_old_text(folder)
    with folder.session(seed) as s:
        _debug_box(s, title)
        s.key(RUN_IT, delay=0.8)
        assert _wait(lambda: s.mark_line() is not None, STOP_S), _screen(s)
    assert folder.traced().count("startcmd ask debug/gdb") == 1, folder.traced()
    with folder.session(seed) as s:
        s.key(CTRL_G, "r", delay=1.0)
        assert _wait(lambda: s.mark_line() is not None, STOP_S), _screen(s)
    assert folder.traced().count("startcmd ask debug/gdb") == 1, \
        "the new answer was not kept:\n" + folder.traced()


FOLDER_NAMED_PDB = "[Programming]\nDebugger : 5\n"   # the start folder names pdb


# @scenario SC-debug-115
@needs_gdb
def test_gdb_the_users_own_debugger_after_use_mine_is_contained(tmp_path):
    """The start folder's xwperc names pdb and holds a .pdbrc: Use mine
    starts the user's own debugger, gdb, planned apart as every gdb start is
    (the trace names the session's own folder for it)."""
    folder = GdbFolder(tmp_path, "cli")
    (folder.ws / "xwperc").write_text(FOLDER_NAMED_PDB)
    (folder.ws / ".pdbrc").write_text("!open('%s','w').close()\n" % folder.ran)
    with folder.session(SEED_C) as s:
        box = _debug_box(s, "Command from ./xwperc")
        rows = s.display()
        s.key(USE_MINE, delay=0.8)
        assert _wait(lambda: "startplan send file " in folder.traced(), STOP_S), folder.traced()
    assert "your own debugger (gdb) with none of the folder's files" in _own_line_joined(rows), box
    assert "startplan contain gdb apart=1" in folder.traced(), folder.traced()
    assert not folder.ran.exists(), "the folder's .pdbrc ran after Use mine"


# @scenario SC-debug-115
@needs_gdb
def test_gdb_a_folder_gdb_cannot_be_given_has_no_run_it(tmp_path):
    """gdb's list of trusted files is colon separated, so a file whose folder
    name holds a colon cannot be admitted alone: Run it is not offered, and
    Use mine still starts gdb with none of the folder's files."""
    folder = GdbFolder(tmp_path, "cli")
    folder.ws = tmp_path / "a:b"
    folder.ws.mkdir()
    folder.ran = folder.ws / "ran"
    seed, title = folder.beside()
    with folder.session(seed) as s:
        box = _debug_box(s, title)
        buttons = [r for r in box.splitlines() if "Use mine" in r and "Cancel" in r]
        assert buttons and "Run it" not in buttons[0], box
        assert "Run it is not offered:" in box, box
        s.key(USE_MINE, delay=0.8)
        assert _wait(lambda: _last_line_sent(folder), LOADED_S), folder.traced()
        s._drain(SETTLE * 2)
    assert not folder.ran.exists(), "gdb ran the script beside the program after Use mine"
