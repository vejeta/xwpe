"""The selected entry of a project list (Files, Variables, Install).

A project list ends in one blank row where the cursor stands to add a new
entry.  That row is not an entry: Enter on it opens nothing, Delete leaves it
alone, and a Save never writes it.  Whatever the .prj holds (no FILES line,
no install section, an empty list) the three lists open, add, edit and delete
without the editor dying.

The rows that used to kill the editor write past a list's memory, which a
plain run can survive; they run on a sanitizer build launched under a
wpe-prefixed name (WPE_BIN=.../wpe-asan) and fail there.  The rows that read
the .prj back from disk need no sanitizer.

Run: WPE_BIN=$PWD/wpe tests/.venv/bin/python -m pytest -q tests/test_list_entry.py
"""
import os

from wpe_driver import WpeSession, ALT

NO_ENTRY = "No entry in this list"

KEY_DOWN = "\033[B"
KEY_UP = "\033[A"
KEY_DEL = "\033[3~"
KEY_ENTER = "\r"
KEY_CLOSE_WINDOW = "\x17"
ALT_VARIABLES = "\033v"
ALT_INSTALL = "\033i"
ALT_SAVE = "\033s"
ALT_OPTIONS = "\033o"
ALT_EDIT = "\033e"

# A crash inside the editor must end it, not be caught by its own handler.
SANITIZER_ENV = {"ASAN_OPTIONS": "allow_user_segv_handler=0:detect_leaks=0:abort_on_error=1"}

HEADER = "CMP=\tgcc\nEXENAME=\tp\n"


def _write_project(workdir, body):
    """A work folder with two sources and p.prj holding HEADER and @body."""
    for name in ("main.c", "extra.c"):
        with open(os.path.join(workdir, name), "w") as fh:
            fh.write("int x;\n")
    with open(os.path.join(workdir, "p.prj"), "w") as fh:
        fh.write(HEADER + body)


def _session(workdir):
    return WpeSession(workdir, "int x;\n", filename="main.c", wait=2.0,
                      env_extra=SANITIZER_ENV)


def _screen(w):
    return "\n".join(w.display())


def _prj(workdir):
    with open(os.path.join(workdir, "p.prj")) as fh:
        return fh.read()


def _open_project(w):
    """Project > Open Project, pick p.prj: the Project window is on top."""
    w.key(ALT.PROJECT, "p", delay=0.8)
    w.key(KEY_ENTER, delay=0.8)
    w.key(KEY_ENTER, delay=1.3)


def _open_options_list(w, which):
    """From the Project window: Options, then the Variables or Install list."""
    w.key(ALT_OPTIONS, delay=1.0)
    w.key(which, delay=1.0)


def _save_options(w):
    """Leave the list, then press Save in the Project-Options dialog."""
    w.key("\033", delay=0.9)
    w.key(ALT_SAVE, delay=1.0)


def _assert_alive(w, what):
    assert w.alive(), "%s ended the editor:\n%s" % (what, _screen(w))


def _install_lines(text):
    """The lines under the install section of a .prj."""
    lines = text.split("\n")
    if "install:" not in lines:
        return None
    return [ln for ln in lines[lines.index("install:") + 1:] if ln != ""]


# @scenario SC-list-entry-01
def test_typing_a_variable_into_an_empty_variables_list(tmp_path):
    """Options > Variables on a project with no variables, Enter on the blank
    row, type a value, confirm.  The value is kept and saved.  Needs the
    sanitizer build to fail on the old code."""
    _write_project(str(tmp_path), "FILES=\tmain.c\n")
    with _session(str(tmp_path)) as w:
        _open_project(w)
        _open_options_list(w, ALT_VARIABLES)
        w.key(KEY_ENTER, delay=1.0)
        w.key("X=1", delay=0.6)
        w.key(KEY_ENTER, delay=1.0)
        _assert_alive(w, "confirming a new variable")
        _save_options(w)
        _assert_alive(w, "saving the options")
    assert "\nX=1\n" in _prj(str(tmp_path)), _prj(str(tmp_path))


# @scenario SC-list-entry-02
def test_typing_a_command_into_an_empty_install_list(tmp_path):
    """Options > Install on a project with no install section, Enter on the
    blank row, type a command, confirm.  The command is saved under
    install:.  Needs the sanitizer build to fail on the old code."""
    _write_project(str(tmp_path), "FILES=\tmain.c\n")
    with _session(str(tmp_path)) as w:
        _open_project(w)
        _open_options_list(w, ALT_INSTALL)
        w.key(KEY_ENTER, delay=1.0)
        w.key("make all", delay=0.6)
        w.key(KEY_ENTER, delay=1.0)
        _assert_alive(w, "confirming a new install command")
        _save_options(w)
        _assert_alive(w, "saving the options")
    assert _install_lines(_prj(str(tmp_path))) == ["\tmake all"], _prj(str(tmp_path))


# @scenario SC-list-entry-03
def test_deleting_every_install_command_then_typing_a_new_one(tmp_path):
    """Install list with one command: Delete on the blank row does nothing,
    Delete on the command removes it, Enter and a new command is kept as the
    only one.  Needs the sanitizer build to fail on the old code."""
    _write_project(str(tmp_path), "FILES=\tmain.c\n\ninstall:\n\tmake install\n")
    with _session(str(tmp_path)) as w:
        _open_project(w)
        _open_options_list(w, ALT_INSTALL)
        w.key(KEY_DOWN, delay=0.6)          # onto the blank row
        w.key(KEY_DEL, delay=0.8)           # refused: the blank row stays
        w.key(KEY_UP, delay=0.6)
        w.key(KEY_DEL, delay=0.8)           # the command goes
        w.key(KEY_ENTER, delay=1.0)
        w.key("make all", delay=0.6)
        w.key(KEY_ENTER, delay=1.0)
        _assert_alive(w, "typing a command after deleting the last one")
        _save_options(w)
        _assert_alive(w, "saving the options")
    assert _install_lines(_prj(str(tmp_path))) == ["\tmake all"], _prj(str(tmp_path))


# @scenario SC-list-entry-04
def test_add_item_to_a_project_with_an_empty_files_list(tmp_path):
    """Project > Add Item with the Project window closed, on a project whose
    FILES line is empty and that has no install section: the file picked is
    added and saved.  Needs the sanitizer build to fail on the old code."""
    _write_project(str(tmp_path), "FILES=\t\n")
    with _session(str(tmp_path)) as w:
        _open_project(w)
        w.key(KEY_CLOSE_WINDOW, delay=0.8)
        w.key(ALT.PROJECT, "a", delay=1.0)
        w.key(KEY_ENTER, delay=0.8)         # into the file list
        w.key(KEY_ENTER, delay=1.2)         # add the highlighted file
        _assert_alive(w, "Project > Add Item")
    files = [ln for ln in _prj(str(tmp_path)).split("\n") if ln.startswith("FILES=")]
    assert len(files) == 1 and files[0].split()[1:] == ["extra.c"], _prj(str(tmp_path))


# @scenario SC-list-entry-05
def test_open_a_project_with_install_and_no_files_line(tmp_path):
    """Open a project that has an install section but no FILES line: the
    Project window opens empty.  Needs the sanitizer build to fail on the
    old code."""
    _write_project(str(tmp_path), "install:\n\tmake install\n")
    with _session(str(tmp_path)) as w:
        _open_project(w)
        _assert_alive(w, "opening a project with no FILES line")
        assert "Project: p.prj" in _screen(w), _screen(w)
        w.key(KEY_ENTER, delay=1.0)
        assert NO_ENTRY in _screen(w), "Enter in the empty Project window:\n" + _screen(w)
        w.key(KEY_ENTER, delay=0.8)
        _assert_alive(w, "Enter in the empty Project window")


# @scenario SC-list-entry-06
def test_save_writes_no_blank_variable_file_or_command(tmp_path):
    """Options > Save on a project with a variable, a file and an install
    command: the .prj holds those and no blank line, no blank FILES word
    and no blank install line."""
    _write_project(str(tmp_path),
                   "FOO=\tbar\nFILES=\tmain.c\n\ninstall:\n\tmake install\n")
    with _session(str(tmp_path)) as w:
        _open_project(w)
        w.key(ALT_OPTIONS, delay=1.0)
        w.key(ALT_SAVE, delay=1.0)
        _assert_alive(w, "saving the options")
    text = _prj(str(tmp_path))
    lines = text.split("\n")
    assert not [ln for ln in lines if ln.strip() == "" and ln != ""], \
        "a line holds only blanks:\n" + text
    files = [ln for ln in lines if ln.startswith("FILES=")]
    assert files == ["FILES=\tmain.c "], files
    assert _install_lines(text) == ["\tmake install"], text
    assert "FOO=bar" in lines, text


# @scenario SC-list-entry-07
def test_save_adds_no_install_section_to_a_project_without_one(tmp_path):
    """Options > Save on a project that has no install section writes none:
    the blank row of the Install list is not a command."""
    _write_project(str(tmp_path), "FILES=\tmain.c\n")
    with _session(str(tmp_path)) as w:
        _open_project(w)
        w.key(ALT_OPTIONS, delay=1.0)
        w.key(ALT_SAVE, delay=1.0)
        _assert_alive(w, "saving the options")
    assert "install:" not in _prj(str(tmp_path)), _prj(str(tmp_path))


def _blank_row_says_no_entry(w, key, name):
    w.key(key, delay=1.3)
    screen = _screen(w)
    assert w.alive(), "%r on the blank row of %s ended the editor" % (key, name)
    assert NO_ENTRY in screen, \
        "%r on the blank row of %s did not say so:\n%s" % (key, name, screen)
    w.key(KEY_ENTER, delay=0.8)             # dismiss the note
    assert "Project: p.prj" in _screen(w), \
        "a window was opened from the blank row:\n" + _screen(w)


# @scenario SC-list-entry-08
def test_enter_and_alt_e_on_the_blank_row_of_the_project_list(tmp_path):
    """Move onto the blank row under the files and press Enter, then Alt-E:
    each says "No entry in this list" and opens no window."""
    _write_project(str(tmp_path), "FILES=\tmain.c\n\ninstall:\n\tmake install\n")
    with _session(str(tmp_path)) as w:
        _open_project(w)
        w.key(KEY_DOWN, delay=0.6)
        _blank_row_says_no_entry(w, KEY_ENTER, "Project")
        _blank_row_says_no_entry(w, ALT_EDIT, "Project")


# @scenario SC-list-entry-09
def test_editing_the_last_variable_changes_it_in_place(tmp_path):
    """A project without an install section, one variable: edit it and the
    .prj holds the new text once, not the old one as well."""
    _write_project(str(tmp_path), "FOO=\told\nFILES=\tmain.c\n")
    with _session(str(tmp_path)) as w:
        _open_project(w)
        _open_options_list(w, ALT_VARIABLES)
        w.key(KEY_ENTER, delay=1.0)
        w.key("\x7f" * 8, delay=0.6)
        w.key("FOO=new", delay=0.6)
        w.key(KEY_ENTER, delay=1.0)
        _save_options(w)
        _assert_alive(w, "editing the last variable")
    lines = _prj(str(tmp_path)).split("\n")
    assert "FOO=new" in lines and "FOO=old" not in lines, "\n".join(lines)


# @scenario SC-list-entry-10
def test_cancelling_the_options_keeps_the_project_windows_files(tmp_path):
    """Positive control: delete a file in the Project window, open the
    options and cancel.  The window still lists the file that is left and
    not the one that was deleted."""
    _write_project(str(tmp_path), "FILES=\tmain.c extra.c\n")
    with _session(str(tmp_path)) as w:
        _open_project(w)
        w.key(KEY_DEL, delay=0.8)           # main.c goes
        w.key(ALT_OPTIONS, delay=1.0)
        w.key("\033", delay=1.0)            # Cancel
        _assert_alive(w, "cancelling the options")
        rows = "\n".join(w.display()[3:19])      # the Project window's rows
        assert "extra.c" in rows, _screen(w)
        assert "main.c" not in rows, _screen(w)


# A project as 1.5.30a saved it: its Install list's blank row went to disk
# as a tab-and-blank line under install:, also when it had no commands.
LEGACY_INSTALL = ("CMP=\tgcc\nEXENAME=\tp\nCMPSWTCH=\tgnu\n\nFILES=\tmain.c  \n"
                  "\n\ninstall:\n%s\t \n")
INSTALL_ASKS = "Command from p.prj"
ALT_INSTALL_RUN = "\033l"
INSTALL_POLLS = 30


def _install_question(workdir, commands):
    """Alt-L (build, then install) on a 1.5.30a project whose install
    section holds @commands and the blank line: the screen once the build
    ended and the install question came or did not."""
    with open(os.path.join(workdir, "main.c"), "w") as fh:
        fh.write("int main(void) { return 0; }\n")
    with open(os.path.join(workdir, "p.prj"), "w") as fh:
        fh.write(LEGACY_INSTALL % commands)
    with WpeSession(workdir, "int main(void) { return 0; }\n", filename="main.c",
                    wait=2.0, env_extra=SANITIZER_ENV) as w:
        _open_project(w)
        w.key(ALT_INSTALL_RUN, delay=1.0)
        for _ in range(INSTALL_POLLS):
            w.key("", delay=0.5)
            screen = _screen(w)
            if INSTALL_ASKS in screen or "0 errors" in screen:
                w.key("", delay=1.5)
                break
        _assert_alive(w, "Alt-L")
        return _screen(w)


# @scenario SC-list-entry-12
def test_install_asks_nothing_for_the_blank_line_of_an_old_project(tmp_path):
    """Alt-L on a project saved by 1.5.30a, whose install section holds only
    the blank line: the build runs and no install command is offered.  The
    same file with one real command still asks for it (positive control)."""
    real = tmp_path / "real"
    blank = tmp_path / "blank"
    real.mkdir()
    blank.mkdir()
    screen = _install_question(str(real), "\ttouch installed\n")
    assert INSTALL_ASKS in screen and "touch installed" in screen, \
        "a real install command was not offered:\n" + screen
    screen = _install_question(str(blank), "")
    assert "0 errors" in screen, "the build did not end:\n" + screen
    assert INSTALL_ASKS not in screen, \
        "Install offered the blank line as a command:\n" + screen
