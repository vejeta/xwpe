"""A path longer than the buffer that held it is no longer cut.

Every row here used to reach the system, a program or the user's own file
with a PREFIX of the path it meant: Ctrl-F9 ran a script from a cut folder,
a project's library and executable were cut when Project-Options saved
them, the configuration file went to a cut folder, Move renamed a file to
the first 128 bytes of its name, New Project wrote its .prj at a cut path.
Each row asserts the effect on disk (or the box the user reads), so it is
red on the commit before the owner took the copy and green after.

A path the system itself cannot hold is refused in a box that says so
(WPE_PATH_TOO_LONG_FMT: "The <what> path is longer than the system allows");
the rows at the end drive that sentence.

Run: WPE_BIN=$PWD/wpe pytest tests/test_path_cuts.py (and wpe-asan).
"""
import os
import shutil

import pytest

from wpe_driver import WpeSession

CTRL_F9 = "\033[20;5~"      # Run
CTRL_F1 = "\033[11;5~"      # Topic Search: the manual page of the word under the cursor
F3 = "\033[13~"
ALT_D = "\033d"
ALT_F = "\033f"
ALT_M = "\033m"
ALT_O = "\033o"
ALT_P = "\033p"
ALT_S = "\033s"
CTRL_O = "\x0f"
ENTER = "\r"
SANITIZER = "AddressSanitizer"
SENTENCE = "longer than the system allows"
ASAN_ENV = {"ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1"}

COMPONENT = 60              # bytes of each folder name in a deep folder
DEEP_RUN = 420              # over the 255 bytes Run kept the source's folder in
DEEP_MANAGER = 2117         # over the 2047 bytes the manager's buffers held
DEEP_JOIN = 4040            # a folder that fits, with a name that makes it not
LIBRARY_BYTES = 98          # over the 79 bytes the project's library was kept in
EXECUTABLE_BYTES = 150      # over the 128 bytes the Executable field took
NAME_BYTES = 200            # a name over the 128 bytes Move offered
INFO_FOLDER_BYTES = 200     # an Info-Path over the 128 bytes its entry took
XDG_TOO_LONG = 4300         # a configuration folder no system can hold
XDG_OVER_OLD = 1105         # over the 1023 bytes the config folder was kept in
INCLUDE_TOO_LONG = 4300     # an include folder no system can hold
NAME_FIELD_BYTES = 70       # what New Project's Name field takes (79) minus ".prj"


def _screen(w):
    return [r.rstrip() for r in w.display() if r.strip()]


def _text(w):
    return "\n".join(_screen(w))


def _intact(w, what):
    assert w.alive(), "the editor died: %s\n%s" % (what, _text(w))
    assert SANITIZER not in bytes(w.raw).decode("utf-8", "replace"), \
        "sanitizer report: %s" % what


def _folder_of_length(base, total):
    """A folder under @base whose absolute path is exactly @total bytes."""
    path = str(base)
    while len(path) + 1 + COMPONENT < total:
        path = os.path.join(path, "d" * COMPONENT)
    last = total - len(path) - 1
    if last > 0:
        path = os.path.join(path, "e" * last)
    os.makedirs(path, exist_ok=True)
    assert len(path) == total
    return path


def _project(folder, lines):
    prj = os.path.join(folder, "p.prj")
    with open(prj, "w") as fh:
        fh.write("#\n# xwpe - project-file: p.prj\nCMP=\tgcc\nCMPFLAGS=\t-g\nLDFLAGS=\t\n"
                 + lines + "CMPSWTCH=\tgnu\n\nFILES=\tt.c \n")
    with open(os.path.join(folder, "xwperc"), "w") as fh:
        fh.write("[Programming]\nProject : p.prj\n")
    return prj


def _variable(prj, name):
    with open(prj) as fh:
        for line in fh:
            if line.startswith(name + "="):
                return line.rstrip("\n").split("\t", 1)[1].strip()
    return None


def _save_project_options(w):
    """Project > Options, then Save: the .prj is written from the dialog."""
    w.key(ALT_P, delay=1.0)
    w.key("o", delay=1.5)
    w.key(ALT_S, delay=1.5)


# Ctrl-F9 on a script in a folder deeper than 255 bytes.
# @scenario SC-path-cuts-01
@pytest.mark.skipif(shutil.which("python3") is None, reason="python3 not installed")
def test_run_of_a_script_in_a_deep_folder_runs_it(tmp_path):
    """Ctrl-F9 on t.py in a 420 byte folder runs the script there.  The
    folder was kept in 255 bytes for the run after the build, so python was
    handed the file of a folder that does not exist."""
    folder = _folder_of_length(tmp_path, DEEP_RUN)
    script = "open('ran_here.txt', 'w').write('X')\n"
    with WpeSession(folder, script, filename="t.py", wait=2.0, cols=150, rows=40,
                    env_extra=ASAN_ENV) as w:
        w.key(CTRL_F9, delay=6.0)
        w.display()
        _intact(w, "Ctrl-F9 in a %d byte folder" % DEEP_RUN)
    assert os.path.exists(os.path.join(folder, "ran_here.txt")), \
        "the script did not run in its own folder"


# The project's library, written back at its length.
# @scenario SC-path-cuts-02
def test_project_options_keep_a_library_name_whole(tmp_path):
    """A .prj with a 98 byte LIBNAME: Project-Options then Save writes it
    back whole.  It was kept in 79 bytes, so Save cut the user's file."""
    library = "l" * LIBRARY_BYTES
    prj = _project(str(tmp_path), "EXENAME=\tp\nLIBNAME=\t%s\n" % library)
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n", filename="t.c",
                    wait=2.0, cols=120, rows=40, env_extra=ASAN_ENV) as w:
        _save_project_options(w)
        _intact(w, "Project-Options Save with a long LIBNAME")
    assert _variable(prj, "LIBNAME") == library, \
        "LIBNAME on disk is %r" % _variable(prj, "LIBNAME")


# A library the system cannot hold is refused, and the build ends cleanly.
# @scenario SC-path-cuts-16
def test_a_library_too_long_is_refused_and_the_build_ends_cleanly(tmp_path):
    """A .prj whose LIBNAME is 4201 bytes: F9 says the library path is
    longer than the system allows, every time, and a Ctrl-F9 pressed in
    the Messages window that refusal left on top ends cleanly.  The build
    ended on the window the key was pressed in, which the new Messages
    window had just closed (a use after free under ASan)."""
    library = "/" + "l" * 4200
    _project(str(tmp_path), "EXENAME=\tp\nLIBNAME=\t%s\n" % library)
    said = []
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n", filename="t.c",
                    wait=2.0, cols=140, rows=40, env_extra=ASAN_ENV) as w:
        for _ in range(2):
            w.key("\033[20~", delay=4.0)
            if "Command from p.prj" in _text(w):
                w.key(ENTER, delay=2.0)          # Run it: the project's compiler
            said.append("library path is " + SENTENCE in _text(w))
            w.key(ENTER, delay=1.0)
        w.key(CTRL_F9, delay=4.0)
        w.key(ENTER, delay=1.0)
        _intact(w, "F9 twice and Ctrl-F9 with a LIBNAME too long")
    assert all(said), said


# The Executable field takes a path.
# @scenario SC-path-cuts-03
def test_project_options_keep_an_executable_name_whole(tmp_path):
    """A .prj with a 150 byte EXENAME: the Executable field shows it whole and
    Save writes it back whole; the field took 128 bytes and cut the rest."""
    exe = "x" * EXECUTABLE_BYTES
    prj = _project(str(tmp_path), "EXENAME=\t%s\n" % exe)
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n", filename="t.c",
                    wait=2.0, cols=120, rows=40, env_extra=ASAN_ENV) as w:
        _save_project_options(w)
        _intact(w, "Project-Options Save with a long EXENAME")
    assert _variable(prj, "EXENAME") == exe, \
        "EXENAME on disk is %r" % _variable(prj, "EXENAME")


# Save Options with a configuration folder that cannot be held.
# @scenario SC-path-cuts-04
def test_save_options_in_a_configuration_folder_too_long_says_so(tmp_path):
    """$XDG_CONFIG_HOME of 4300 bytes, which no system can hold: Options >
    Save Options writes nothing and the status row says why.  The editor
    used to create the 1023 byte cut of such a folder and save there."""
    xdg = "/" + "x" * (XDG_TOO_LONG - 1)
    work = tmp_path / "work"
    work.mkdir()
    with WpeSession(str(work), "int x;\n", filename="t.c", wait=2.0, cols=140, rows=40,
                    env_extra={"XDG_CONFIG_HOME": xdg}) as w:
        w.key(ALT_O, delay=1.0)
        w.key("s", delay=1.5)
        text = _text(w)
        _intact(w, "Save Options with a configuration folder too long")
    stray = [d for d, _, files in os.walk(str(tmp_path)) if "xwperc" in files]
    assert not stray, "an xwperc was written at %r" % stray
    assert "Options not saved" in text and SENTENCE in text, text


# A start-folder xwperc still may not set the user's own keys.
# @scenario SC-path-cuts-14
def test_folder_xwperc_may_not_set_user_keys_beside_a_configuration_folder_too_long(tmp_path):
    """A start folder's xwperc names AIEditHook, a key only the user's own
    file may set; $XDG_CONFIG_HOME is 4300 bytes, so the user's own file
    cannot be named.  The folder's key is still refused and said on the
    status row.  With no user file the folder's xwperc was read as if it
    were the user's own, every key taken."""
    work = tmp_path / "work"
    work.mkdir()
    (work / "xwperc").write_text("[Programming]\nAIEditHook : true\n")
    xdg = "/" + "x" * (XDG_TOO_LONG - 1)
    with WpeSession(str(work), "int x;\n", filename="t.c", wait=2.0, cols=140, rows=40,
                    env_extra={"XDG_CONFIG_HOME": xdg}) as w:
        text = _text(w)
        _intact(w, "start-folder xwperc with a configuration folder too long")
    assert "may not set" in text and "AIEditHook" in text, text


# @scenario SC-path-cuts-05
@pytest.mark.parametrize("folder_bytes", [900, XDG_OVER_OLD])
def test_save_options_in_a_deep_configuration_folder_saves_there(tmp_path, folder_bytes):
    """The control of the row above: a configuration folder of 900 bytes, and
    one of 1105 (over the 1023 it was kept in), is held whole, and Save
    Options (after the question for a path over the field's width) writes
    xwperc under the folder the user named, not under a cut of it."""
    xdg = _folder_of_length(tmp_path / "cfg", folder_bytes)
    work = tmp_path / "work"
    work.mkdir()
    with WpeSession(str(work), "int x;\n", filename="t.c", wait=2.0, cols=140, rows=40,
                    env_extra={"XDG_CONFIG_HOME": xdg}) as w:
        w.key(ALT_O, delay=1.0)
        w.key("s", delay=1.5)
        w.key("y", delay=1.5)
        _intact(w, "Save Options in a %d byte configuration folder" % folder_bytes)
    assert os.path.exists(os.path.join(xdg, "xwpe", "xwperc")), \
        "no xwperc under the folder $XDG_CONFIG_HOME names"


# The file manager's Directory field in a deep folder.
# @scenario SC-path-cuts-06
def test_file_manager_directory_field_keeps_a_deep_folder(tmp_path):
    """F3 in a 2117 byte folder, Alt-D, Enter: the manager stays in the
    folder and lists its files.  The field's buffer was 2047 bytes, said
    "Can't open directory" and replaced the window's folder by the cut.
    The owner's first pass already holds the folder whole, so this row is
    green on that commit too; it stays as the control for the deep folder
    that the New Project row below uses."""
    folder = _folder_of_length(tmp_path, DEEP_MANAGER)
    with open(os.path.join(folder, "deepmarker.c"), "w") as fh:
        fh.write("int x;\n")
    with WpeSession(folder, "int x;\n", filename="t.c", wait=2.0, cols=140, rows=40,
                    env_extra=ASAN_ENV) as w:
        w.key(F3, delay=2.0)
        w.key(ALT_D, delay=1.0)
        w.key(ENTER, delay=2.0)
        text = _text(w)
        _intact(w, "Directory field in a %d byte folder" % DEEP_MANAGER)
    assert "Can't open directory" not in text, text
    assert "deepmarker.c" in text, text


# Move offers the whole name.
# @scenario SC-path-cuts-07
def test_move_offers_a_long_file_name_whole(tmp_path):
    """Alt-M on a file with a 200 byte name and Enter leaves the name as it
    was.  The field was filled with the first 128 bytes, so Enter renamed
    the file to them."""
    name = "n" * (NAME_BYTES - 2) + ".c"
    (tmp_path / name).write_text("int x;\n")
    with WpeSession(str(tmp_path), "int x;\n", filename="t.c", wait=2.0, cols=150,
                    rows=40, env_extra=ASAN_ENV) as w:
        w.key(F3, delay=1.5)
        w.key(ALT_F, delay=1.0)
        w.key(ALT_M, delay=1.0)
        w.key(ENTER, delay=1.5)
        _intact(w, "Move of a %d byte file name" % NAME_BYTES)
    names = os.listdir(str(tmp_path))
    assert name in names, "the file was renamed: %r" % sorted(len(n) for n in names)


# New Project in a deep folder.
# @scenario SC-path-cuts-08
def test_new_project_in_a_deep_folder_writes_it_there(tmp_path):
    """Project > New Project in a 2117 byte folder, name q.prj: the project
    is written in the folder.  Its path was built in 2047 bytes."""
    folder = _folder_of_length(tmp_path, DEEP_MANAGER)
    with WpeSession(folder, "int main(void){return 0;}\n", filename="t.c", wait=2.0,
                    cols=140, rows=40, env_extra=ASAN_ENV) as w:
        w.key(ALT_P, delay=1.0)
        w.key("n", delay=1.5)
        w.key("q.prj" + ENTER, delay=2.0)
        _intact(w, "New Project in a %d byte folder" % DEEP_MANAGER)
    assert os.path.exists(os.path.join(folder, "q.prj")), \
        "the project was not written in its folder"


# The program of a new project is still its name up to the first dot.
# @scenario SC-path-cuts-15
def test_new_project_names_its_program_up_to_the_first_dot(tmp_path):
    """New Project "my.app.prj" writes EXENAME "my", as it always did: the
    name is copied whole now, and only its extension is taken off, from the
    first dot."""
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n", filename="t.c", wait=2.0,
                    cols=140, rows=40, env_extra=ASAN_ENV) as w:
        w.key(ALT_P, delay=1.0)
        w.key("n", delay=1.5)
        w.key("my.app.prj" + ENTER, delay=2.0)
        _intact(w, "New Project my.app.prj")
    assert _variable(os.path.join(str(tmp_path), "my.app.prj"), "EXENAME") == "my"


# A folder and a name that each fit, joined, do not.
# @scenario SC-path-cuts-09
def test_new_project_whose_path_cannot_be_held_says_so(tmp_path):
    """A 4040 byte folder fits and so does a 74 byte name; the project's path
    does not.  The user is told, and no project is written at a cut path."""
    folder = _folder_of_length(tmp_path, DEEP_JOIN)
    name = "q" * NAME_FIELD_BYTES + ".prj"
    with WpeSession(folder, "int main(void){return 0;}\n", filename="t.c", wait=2.0,
                    cols=140, rows=40, env_extra=ASAN_ENV) as w:
        w.key(ALT_P, delay=1.0)
        w.key("n", delay=1.5)
        w.key(name + ENTER, delay=2.0)
        text = _text(w)
        _intact(w, "New Project whose path is over the limit")
    assert SENTENCE in text and "project" in text, text
    assert not [n for n in os.listdir(folder) if n.endswith(".prj")], os.listdir(folder)


# An Info-Path over the old field width can be edited.
# @scenario SC-path-cuts-11
def test_info_path_longer_than_the_old_field_is_offered(tmp_path):
    """INFOPATH of 200 bytes: Options > Help opens the Info-Path entry with
    the whole path.  The entry took 128 bytes and refused to open."""
    info = _folder_of_length(tmp_path / "info", INFO_FOLDER_BYTES)
    with WpeSession(str(tmp_path), "int x;\n", filename="t.c", wait=2.0, cols=140, rows=40,
                    env_extra={"INFOPATH": info}) as w:
        w.key(ALT_O, delay=1.0)
        w.key("h", delay=1.5)
        text = _text(w)
        _intact(w, "Info-Path of %d bytes" % INFO_FOLDER_BYTES)
    assert "Info-Path" in text, text


# The manual page's file is named by the path owner.
# @scenario SC-path-cuts-12
@pytest.mark.skipif(shutil.which("man") is None, reason="man not installed")
def test_manual_page_opens_from_its_temporary_file(tmp_path):
    """Topic Search on `printf` shows the page.  A control, not a red row:
    the page's file is built by the path owner now, but a build with mkdtemp
    (this one) always puts the editor's temporary folder under /tmp, so a
    long TMPDIR cannot reach it here; only a build with tempnam can."""
    with WpeSession(str(tmp_path), "printf\n", filename="t.c", wait=2.0, cols=140, rows=40,
                    env_extra=ASAN_ENV) as w:
        w.key(CTRL_F1, delay=6.0)
        text = _text(w)
        _intact(w, "Topic Search on printf")
    assert "PRINTF" in text and "No manual entry" not in text, text


# One word about a long include folder for a whole search.
# @scenario SC-path-cuts-13
def test_a_long_include_folder_is_said_once_per_search(tmp_path):
    """Three headers that are not found and an include folder no system can
    hold: Show Definition says the folder is too long once, not once for each
    header."""
    inc = "/" + "i" * (INCLUDE_TOO_LONG - 1)
    (tmp_path / "xwperc").write_text("[Programming]\nIncludePath : %s\n" % inc)
    source = "#include <a.h>\n#include <b.h>\n#include <c.h>\nint x = ZZ;\n"
    said = 0
    with WpeSession(str(tmp_path), source, filename="t.c", wait=2.0, cols=140, rows=40,
                    env_extra=ASAN_ENV) as w:
        w.key(CTRL_O, delay=0.6)
        w.key("s", delay=1.2)
        w.key("ZZ", delay=0.6)
        w.key(ENTER, delay=2.5)
        for _ in range(6):
            if SENTENCE in _text(w):
                said += 1
            w.key(ENTER, delay=1.0)
        _intact(w, "Show Definition through three missing headers")
    assert said == 1, "the include folder was said %d times" % said
