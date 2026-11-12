"""F9 judges one start plan: what a folder decides about the build is shown.

A start-folder xwperc or a project file can decide more about F9 than which
compiler runs: where the program and the objects are written (EXENAME,
Exedir, a link in the folder), which project file is read (Project), and
what $(NAME) in a project file becomes.  The editor now builds the whole
build's plan before anything starts, resolves every path with its links
followed, and asks once when a folder's value puts something outside the
folder of the file that set it.  A stale object outside that folder is
never removed.  A project of many files is asked about once per build, and
an old project whose outputs stay inside its folder is not asked about.

The editor's trace (XWPE_UI_TRACE) carries every line of the box whole, so
a long path the box had to cut is still read in full.
"""
import os
import time

import pytest
from wpe_driver import ALT, WAIT_SCALE, WpeSession, wait_screen, tool_usable

F9 = "\033[20~"            # Make: compile and link
ESC = "\033"
RUN_IT = "\033r"           # Alt-R: Run it
ENTER = "\r"

SEED = "int main(void){return 0;}\n"
BOX_WAIT = 12              # the box is up by then
BUILD_WAIT = 30            # gcc has built a small project by then
SETTLE = 1.5               # whatever an answer leads to has started by then
OLD = 1000                 # seconds an object is made older than its source

needs_gcc = pytest.mark.skipif(not tool_usable("gcc", "--version"), reason="gcc not usable")


class Folder:
    """A start folder and a HOME of the user's own beside it."""

    def __init__(self, tmp_path):
        self.tmp = tmp_path
        self.ws = tmp_path / "ws"
        self.home = tmp_path / "home"
        self.ws.mkdir(exist_ok=True)
        self.home.mkdir(exist_ok=True)
        self.trace = tmp_path / "ui.trace"

    @property
    def store(self):
        return self.home / ".local" / "state" / "xwpe" / "start-commands"

    def xwperc(self, text):
        (self.ws / "xwperc").write_text(text)

    def own_config(self, text):
        cfg = self.home / ".config" / "xwpe"
        cfg.mkdir(parents=True, exist_ok=True)
        (cfg / "xwperc").write_text(text)

    def session(self, filename="t.c", seed=SEED, workdir=None, env=None):
        extra = {"HOME": str(self.home), "XWPE_UI_TRACE": str(self.trace)}
        if env:
            extra.update(env)
        return WpeSession(str(workdir or self.ws), seed, filename=filename, env_extra=extra)

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


def _wait(predicate, timeout):
    end = time.time() + timeout * WAIT_SCALE
    while time.time() < end:
        if predicate():
            return True
        time.sleep(0.3)
    return predicate()


def _open_project(s):
    """Project > Open project, the Files box, the one .prj of the folder."""
    s.key(ALT.PROJECT, "p", delay=0.6)
    s.key(ENTER, delay=0.6)
    s.key(ENTER, delay=0.8)


def _f9_box(s, title):
    """F9, and the box titled @title; returns its screen."""
    s.key(F9, delay=0.8)
    asked = wait_screen(s, title, tmo=BOX_WAIT)
    screen = _screen(s)
    assert asked, "no question before the build:\n" + screen
    return screen


def _older(path, seconds=OLD):
    past = time.time() - seconds
    os.utime(str(path), (past, past))


def _plan_lines(folder):
    return [l.split("startplan line ", 1)[1] for l in folder.traced().splitlines()
            if "startplan line " in l]


# @scenario SC-compile-29
@needs_gcc
def test_startplan_exename_outside_the_folder_asks(tmp_path):
    """The project's CMP is the user's own gcc -g, but EXENAME is
    $(HOME)/.local/bin/git: F9 shows that the link writes outside the
    folder, and what $(HOME) became; Esc builds nothing there.  (The user's
    own Exedir is empty, so the program's path is EXENAME itself; with the
    default "." an absolute EXENAME lands below the project folder.)"""
    folder = Folder(tmp_path)
    target = folder.home / ".local" / "bin"
    target.mkdir(parents=True)
    folder.own_config("[Programming]\nExedir : \n")
    (folder.ws / "p.prj").write_text(
        "CMP=\tgcc\nCMPFLAGS=\t-g\nEXENAME=\t$(HOME)/.local/bin/git\nFILES=\tmain.c\n")
    with folder.session(filename="main.c") as s:
        _open_project(s)
        box = _f9_box(s, "Command from p.prj")
        s.key(ESC, delay=0.8)
        s._drain(SETTLE + 3)
    lines = _plan_lines(folder)
    assert "Writes outside this folder:" in box, box
    assert "  %s" % (target / "git") in lines, lines
    assert any(l.startswith("From p.prj: EXENAME = $(HOME)/.local/bin/git -> ") for l in lines), lines
    assert not (target / "git").exists(), "the link wrote into ~/.local/bin after Esc"


# @scenario SC-compile-29
def test_startplan_exedir_outside_never_removes(tmp_path):
    """The folder's Exedir is a folder outside it, holding a t.o older than
    t.c: F9 shows the object written outside, and Esc leaves that file as it
    was (the old build removed it and compiled there unasked)."""
    folder = Folder(tmp_path)
    elsewhere = tmp_path / "elsewhere"
    elsewhere.mkdir()
    marker = elsewhere / "t.o"
    marker.write_text("marker\n")
    _older(marker)
    folder.xwperc("[Programming]\nExedir : %s\n" % elsewhere)
    with folder.session() as s:
        box = _f9_box(s, "Command from ./xwperc")
        s.key(ESC, delay=0.8)
        s._drain(SETTLE + 3)
    assert "Writes outside this folder:" in box, box
    assert "  %s" % marker in _plan_lines(folder), _plan_lines(folder)
    assert marker.read_text() == "marker\n", "the object outside the folder was removed or written"


# @scenario SC-compile-29
def test_startplan_exedir_link_out_of_the_folder(tmp_path):
    """The folder's Exedir is ./out, a link to a folder outside it: the box
    names the real place the object goes, past the link."""
    folder = Folder(tmp_path)
    elsewhere = tmp_path / "elsewhere"
    elsewhere.mkdir()
    (folder.ws / "out").symlink_to(elsewhere)
    folder.xwperc("[Programming]\nExedir : out\n")
    with folder.session() as s:
        box = _f9_box(s, "Command from ./xwperc")
        s.key(ESC, delay=0.8)
        s._drain(SETTLE)
    real = os.path.realpath(str(elsewhere))
    assert "Writes outside this folder:" in box, box
    assert "  %s/t.o" % real in _plan_lines(folder), _plan_lines(folder)
    assert not (elsewhere / "t.o").exists(), "the object was written after Esc"


PROJECT_OWN = "CMP=\tgcc\nCMPFLAGS=\t-g\nEXENAME=\tdemo\nFILES=\tmain.c\n"


# @scenario SC-compile-29
@needs_gcc
def test_startplan_dangling_link_out_of_the_folder_asks(tmp_path):
    """The project's line is the user's own and EXENAME is demo, but the
    folder ships demo as a link to ~/.local/bin/git, which is not there yet:
    the linker would write through the link and create it, so F9 asks, names
    the link's target, and Esc leaves ~/.local/bin empty."""
    folder = Folder(tmp_path)
    target = folder.home / ".local" / "bin"
    target.mkdir(parents=True)
    (folder.ws / "p.prj").write_text(PROJECT_OWN)
    (folder.ws / "main.c").write_text(SEED)
    (folder.ws / "demo").symlink_to(os.path.relpath(str(target / "git"), str(folder.ws)))
    with folder.session(filename="main.c") as s:
        _open_project(s)
        box = _f9_box(s, "Command from p.prj")
        s.key(ESC, delay=0.8)
        s._drain(SETTLE + 3)
    assert "Writes outside this folder:" in box, box
    assert "  %s" % os.path.realpath(str(target)) + "/git" in _plan_lines(folder), \
        _plan_lines(folder)
    assert not (target / "git").exists(), "the link wrote into ~/.local/bin"


# @scenario SC-compile-29
@needs_gcc
def test_startplan_project_outside_the_start_folder_asks(tmp_path):
    """The start folder's xwperc names a project in another folder, whose
    CMP is the user's own gcc -g: F9 says the build reads a project file
    from outside; after Run it the project builds, and the next F9 in the
    same folder asks nothing."""
    folder = Folder(tmp_path)
    other = tmp_path / "other"
    other.mkdir()
    prj = other / "p.prj"
    prj.write_text(PROJECT_OWN)
    (other / "main.c").write_text(SEED)
    folder.xwperc("[Programming]\nProject : %s\n" % prj)
    with folder.session() as s:
        box = _f9_box(s, "Command from p.prj")
        s.key(RUN_IT, delay=0.8)
        built = _wait(lambda: (other / "demo").exists(), BUILD_WAIT)
    assert "Reads outside this folder:" in box, box
    assert "  %s" % os.path.realpath(str(prj)) in _plan_lines(folder), _plan_lines(folder)
    assert built, "Run it did not build the project"
    asked = folder.traced().count("startcmd ask")
    (other / "demo").unlink()
    with folder.session() as s:
        s.key(F9, delay=0.8)
        rebuilt = _wait(lambda: (other / "demo").exists(), BUILD_WAIT)
        screen = _screen(s)
    assert rebuilt, "the second F9 did not build:\n" + screen
    assert folder.traced().count("startcmd ask") == asked, "the second F9 asked again"


MANY = 30


# @scenario SC-compile-29
@needs_gcc
def test_startplan_project_build_asks_once(tmp_path):
    """A project of 30 files whose CMPFLAGS are not the user's: one box for
    the whole build (not one per file), Run it builds every file, and after
    one source changes the next F9 asks nothing; the store holds one
    record."""
    folder = Folder(tmp_path)
    names = ["f%02d.c" % i for i in range(MANY)]
    for i, name in enumerate(names):
        (folder.ws / name).write_text("int f%02d(void){return %d;}\n" % (i, i))
    (folder.ws / "p.prj").write_text(
        "CMP=\tgcc\nCMPFLAGS=\t-O1\nEXENAME=\tdemo\nFILES=\tmain.c %s\n" % " ".join(names))
    with folder.session(filename="main.c") as s:
        _open_project(s)
        box = _f9_box(s, "Command from p.prj")
        s.key(RUN_IT, delay=0.8)
        built = _wait(lambda: (folder.ws / "demo").exists(), BUILD_WAIT * 2)
    assert built, "Run it did not build the project"
    assert folder.traced().count("startcmd ask") == 1, folder.traced()
    assert "%d files from p.prj, all inside this folder" % (MANY + 1) in _plan_lines(folder), \
        _plan_lines(folder)
    assert "From p.prj: CMPFLAGS = -O1" in box, box
    (folder.ws / "demo").unlink()
    (folder.ws / names[3]).write_text("int f03(void){return 33;}\n")
    with folder.session(filename="main.c") as s:
        _open_project(s)
        s.key(F9, delay=0.8)
        rebuilt = _wait(lambda: (folder.ws / "demo").exists(), BUILD_WAIT)
    assert rebuilt, "the second F9 did not build"
    assert folder.traced().count("startcmd ask") == 1, "the second F9 asked again"
    assert len(folder.records()) == 1, folder.records()


# @scenario SC-compile-29
@needs_gcc
def test_startplan_1995_project_inside_its_folder_does_not_ask(tmp_path):
    """An old project file with EXENAME, and an xwperc whose Exedir is a
    folder inside: everything stays inside, the line is the user's own, so
    F9 builds with no question."""
    folder = Folder(tmp_path)
    (folder.ws / "obj").mkdir()
    folder.xwperc("[Programming]\nExedir : obj\n")
    (folder.ws / "demo.prj").write_text(PROJECT_OWN)
    with folder.session(filename="main.c") as s:
        _open_project(s)
        s.key(F9, delay=0.8)
        built = _wait(lambda: (folder.ws / "obj" / "demo").exists(), BUILD_WAIT)
        screen = _screen(s)
    assert built, "the project did not build:\n" + screen
    assert "startcmd ask" not in folder.traced(), folder.traced()


# @scenario SC-compile-29
@needs_gcc
def test_startplan_exename_outside_asks_when_started_in_home(tmp_path):
    """The editor starts in the home folder and the project lives in
    ~/proj: a home-folder start does not make ~/.local/bin "inside", so the
    EXENAME there is asked about."""
    folder = Folder(tmp_path)
    proj = folder.home / "proj"
    proj.mkdir()
    target = folder.home / ".local" / "bin"
    target.mkdir(parents=True)
    (proj / "p.prj").write_text(
        "CMP=\tgcc\nCMPFLAGS=\t-g\nEXENAME=\t$(HOME)/.local/bin/git\nFILES=\tmain.c\n")
    (proj / "main.c").write_text(SEED)
    (folder.home / "xwperc").write_text("[Programming]\nProject : proj/p.prj\nExedir : \n")
    with folder.session(workdir=folder.home) as s:
        box = _f9_box(s, "Command from p.prj")
        s.key(ESC, delay=0.8)
        s._drain(SETTLE + 3)
    assert "Writes outside this folder:" in box, box
    assert "  %s" % (target / "git") in _plan_lines(folder), _plan_lines(folder)
    assert not (target / "git").exists(), "the link wrote into ~/.local/bin"


# @scenario SC-compile-29
@needs_gcc
def test_startplan_dropped_remove_is_said(tmp_path):
    """The folder's Exedir is a folder outside it, holding a stale t.o:
    after Run it the compile writes there, but the editor does not remove
    the old object first, and the status row says so."""
    folder = Folder(tmp_path)
    elsewhere = tmp_path / "elsewhere"
    elsewhere.mkdir()
    stale = elsewhere / "t.o"
    stale.write_text("marker\n")
    _older(stale)
    folder.xwperc("[Programming]\nExedir : %s\n" % elsewhere)
    with folder.session() as s:
        _f9_box(s, "Command from ./xwperc")
        s.key(RUN_IT, delay=0.8)
        built = _wait(lambda: (elsewhere / "t.e").exists(), BUILD_WAIT)
        screen = _screen(s)
    assert built, "the build did not run after Run it:\n" + screen
    assert "not removed: %s (outside this folder)" % os.path.realpath(str(stale)) in folder.traced(), \
        folder.traced()
    assert stale.read_bytes() != b"marker\n", "the compiler did not write the object"


# @scenario SC-compile-29
@needs_gcc
def test_startplan_users_own_exedir_outside_removes_as_before(tmp_path):
    """The user's own Exedir is an absolute folder outside the file's
    folder, with a stale t.o: it is his setting, so nothing asks, the old
    object is removed and rebuilt as it always was, and no "not removed"
    line appears."""
    folder = Folder(tmp_path)
    elsewhere = tmp_path / "elsewhere"
    elsewhere.mkdir()
    stale = elsewhere / "t.o"
    stale.write_text("marker\n")
    _older(stale)
    folder.own_config("[Programming]\nExedir : %s\n" % elsewhere)
    with folder.session() as s:
        s.key(F9, delay=0.8)
        built = _wait(lambda: (elsewhere / "t.e").exists(), BUILD_WAIT)
        screen = _screen(s)
    assert built, "the build did not run:\n" + screen
    assert "startcmd ask" not in folder.traced(), folder.traced()
    assert "not removed" not in folder.traced(), folder.traced()
    assert stale.read_bytes() != b"marker\n", "the object was not rebuilt"


DEMO_PRJ = "CMP=\tgcc\nCMPFLAGS=\t-g\nEXENAME=\t%s\nFILES=\tmain.c\n"


def _home_project(folder, exename):
    """A project kept in the home folder itself, whose line is the user's
    own gcc -g, started from there."""
    (folder.home / "p.prj").write_text(DEMO_PRJ % exename)
    return folder.session(filename="main.c", workdir=folder.home)


# @scenario SC-compile-29
@needs_gcc
def test_startplan_project_in_home_overwriting_a_text_asks(tmp_path):
    """A project in the home folder names EXENAME = notes.txt, a text the
    user keeps there: F9 asks before the link writes over it, and Esc
    leaves the text as it was."""
    folder = Folder(tmp_path)
    notes = folder.home / "notes.txt"
    notes.write_text("my notes\n")
    with _home_project(folder, "notes.txt") as s:
        _open_project(s)
        box = _f9_box(s, "Command from p.prj")
        s.key(ESC, delay=0.8)
        s._drain(SETTLE + 3)
    assert "Writes outside this folder:" in box, box
    assert notes.read_text() == "my notes\n", "the link wrote over the user's text"


# @scenario SC-compile-29
@needs_gcc
def test_startplan_project_in_home_builds_and_rebuilds_its_program(tmp_path):
    """A project in the home folder with EXENAME = demo and no ~/demo yet:
    F9 builds it with no question; after a source changes, the second F9
    links over the program it made, still with no question."""
    folder = Folder(tmp_path)
    demo = folder.home / "demo"
    with _home_project(folder, "demo") as s:
        _open_project(s)
        s.key(F9, delay=0.8)
        built = _wait(demo.exists, BUILD_WAIT)
        screen = _screen(s)
    assert built, "the project did not build:\n" + screen
    assert demo.read_bytes()[:4] == b"\x7fELF", "the program is not what the link made"
    _older(demo)
    _older(folder.home / "main.o")
    (folder.home / "main.c").write_text("int main(void){return 1;}\n")
    with folder.session(filename="main.c", seed="int main(void){return 1;}\n",
                        workdir=folder.home) as s:
        _open_project(s)
        s.key(F9, delay=0.8)
        rebuilt = _wait(lambda: os.path.getmtime(str(demo)) > time.time() - OLD / 2, BUILD_WAIT)
        screen = _screen(s)
    assert rebuilt, "the second F9 did not link again:\n" + screen
    assert "startcmd ask" not in folder.traced(), folder.traced()


# @scenario SC-compile-29
@needs_gcc
def test_startplan_project_in_home_over_a_script_asks(tmp_path):
    """A project in the home folder with EXENAME = demo, where ~/demo is a
    shell script of the user's: it is not a program a link made, so F9
    asks, and Esc leaves the script as it was."""
    folder = Folder(tmp_path)
    demo = folder.home / "demo"
    demo.write_text("#!/bin/sh\necho mine\n")
    with _home_project(folder, "demo") as s:
        _open_project(s)
        box = _f9_box(s, "Command from p.prj")
        s.key(ESC, delay=0.8)
        s._drain(SETTLE + 3)
    assert "Writes outside this folder:" in box, box
    assert demo.read_text() == "#!/bin/sh\necho mine\n", "the link wrote over the script"


# @scenario SC-compile-29
def test_startplan_folder_value_never_expands_a_key(tmp_path):
    """A project's CMPFLAGS hold $(XWPE_AI_KEY), set in the editor's
    environment: the box shows the name as written, never its value."""
    folder = Folder(tmp_path)
    secret = "sekrit-value-1234"
    (folder.ws / "p.prj").write_text(
        "CMP=\tgcc\nCMPFLAGS=\t-DK=$(XWPE_AI_KEY)\nEXENAME=\tdemo\nFILES=\tmain.c\n")
    with folder.session(filename="main.c", env={"XWPE_AI_KEY": secret}) as s:
        _open_project(s)
        box = _f9_box(s, "Command from p.prj")
        s.key(ESC, delay=0.8)
        s._drain(SETTLE)
    assert "-DK=$(XWPE_AI_KEY)" in box, box
    assert secret not in box and secret not in folder.traced(), "the key's value was expanded"


# @scenario SC-compile-29
def test_startplan_box_lists_folder_values(tmp_path):
    """A project file sets CMP, CMPFLAGS and LDFLAGS, none the user's own:
    the box says where each value came from, one line each."""
    folder = Folder(tmp_path)
    (folder.ws / "p.prj").write_text(
        "CMP=\t./my-cc\nCMPFLAGS=\t-O2\nLDFLAGS=\t-lm\nEXENAME=\tdemo\nFILES=\tmain.c\n")
    with folder.session(filename="main.c") as s:
        _open_project(s)
        box = _f9_box(s, "Command from p.prj")
        s.key(ESC, delay=0.8)
        s._drain(SETTLE)
    for line in ("From p.prj: CMP = ./my-cc", "From p.prj: CMPFLAGS = -O2",
                 "From p.prj: LDFLAGS = -lm"):
        assert line in box, box


ALT_A = "\033a"            # Run > Execute Make


# @scenario SC-compile-29
@pytest.mark.skipif(not tool_usable("make", "--version"), reason="make not usable")
def test_startplan_make_is_not_planned_yet_and_says_so(tmp_path):
    """Execute Make has no start plan yet (a later pass gives it one): it
    still runs, and the children's owner traces it as an unplanned start,
    which is how the sites still to convert are told apart."""
    folder = Folder(tmp_path)
    (folder.ws / "Makefile").write_text("all:\n\ttouch made\n")
    with folder.session() as s:
        s.key(ALT_A, delay=0.8)
        made = _wait(lambda: (folder.ws / "made").exists(), BUILD_WAIT)
    assert made, "make did not run"
    assert "child unplanned the build" in folder.traced(), folder.traced()


# @scenario SC-compile-29
@needs_gcc
def test_startplan_saved_config_in_folder_is_silent(tmp_path):
    """A folder xwperc that repeats the built-in values (as Save Options
    writes them) is not the folder deciding anything: F9 builds unasked,
    and its compile and link start from the build's plan (the children's
    owner traces a start that does not)."""
    folder = Folder(tmp_path)
    folder.xwperc("[Programming]\nExedir : .\nArguments : \nProject : \n"
                  "[Language/C]\nCompiler : gcc\nCompilerOptions : -g\n")
    with folder.session() as s:
        s.key(F9, delay=0.8)
        built = _wait(lambda: (folder.ws / "t.e").exists(), BUILD_WAIT)
        screen = _screen(s)
    assert built, "the build did not run:\n" + screen
    assert "startcmd ask" not in folder.traced(), folder.traced()
    assert "child unplanned the build" not in folder.traced(), folder.traced()
