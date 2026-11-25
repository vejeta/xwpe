"""The open project is a file the editor holds: the file under its name is not
read or written in silence when it is another file.

A checkout, vim and sed -i save by renaming, and a link can be put at the
name, so the file at the project's name can stop being the file the editor
read as the project.  The editor then asks one question and leaves the other
file alone on No.  The rows drive the terminal editor as `wpe`.

Run: tests/.venv/bin/python -m pytest -q tests/test_project_hold.py
"""

import os
import stat

from mode_premise import file_mode_is_obeyed
from wpe_driver import WpeSession

NO_BIRTH = {"XWPE_TEST_FAIL": "seen-no-birth"}

THIS_PROJECT = ("#\n# xwpe - project-file: p.prj\n#\nCMP=\tgcc\nCMPFLAGS=\t-g\n"
                "LDFLAGS=\t\nEXENAME=\tp\nCMPSWTCH=\tgnu\n\nFILES=\tm.c \n")
OTHER_PROJECT = ("#\n# xwpe - project-file: q.prj\n#\nCMP=\tclang\nCMPFLAGS=\t-O2\n"
                 "LDFLAGS=\t\nEXENAME=\tq\nCMPSWTCH=\tgnu\n\nFILES=\tzz.c \n")

# What the question says, and what the screen shows when a box is up.
READ_OTHER = "another file took the name"
READ_UNSURE = "changed or replaced"
WRITE_OTHER = "was replaced on disk since the project was opened"
OPTIONS_BOX = "Project-Options"
CLOSE_TAIL = "No closes the project window"

KEY_YES = "\033y"
KEY_NO = "\033n"
KEY_OPTIONS = "\033o"      # Options button of the project window
KEY_ADD = "\033a"          # Add button: file box, then the list
KEY_CLOSE_WINDOW = "\x17"  # Ctrl-W
KEY_F9 = "\033[20~"        # compile / build
KEY_ENTER = "\r"
KEY_ESC = "\033"

PROJECT_WINDOW = " Project: p.prj q"      # the title in the frame, not the status row


def _files(tmp_path):
    """The folder of one row: this project (p.prj, member m.c), another
    project (q.prj, member zz.c) and the sources."""
    for name in ("m.c", "zz.c", "e.c"):
        (tmp_path / name).write_text("int x;\n")
    (tmp_path / "p.prj").write_text(THIS_PROJECT)
    (tmp_path / "q.prj").write_text(OTHER_PROJECT)
    return tmp_path


def _open_project(s):
    """Open Project p.prj from the menu: the project window is the top one."""
    s.key("\033p", "p", KEY_ENTER, KEY_ENTER)
    assert PROJECT_WINDOW in "\n".join(s.display()), "the project did not open"


def _screen(s):
    return "\n".join(s.display())


def _session(tmp_path, env_extra=None):
    """The editor on a source, with the project opened from the menu."""
    return WpeSession(str(tmp_path), "int a;\n", filename="zz_seed.c", env_extra=env_extra)


def _swap_by_rename(tmp_path):
    """Another project takes the name by rename, as a checkout does."""
    os.replace(tmp_path / "q.prj", tmp_path / "p.prj")


def _swap_by_link(tmp_path):
    """The name becomes a link to another project's file (a checkout)."""
    os.rename(tmp_path / "p.prj", tmp_path / "keep.prj")
    (tmp_path / "real.prj").write_text(OTHER_PROJECT)
    os.symlink(tmp_path / "real.prj", tmp_path / "p.prj")


def _add_member_asking(s):
    """Add Item: the first file of the list is added to the project, and the
    box of the Add dialog stays up (a question about the file is over it)."""
    s.key(KEY_ADD, KEY_ENTER, KEY_ENTER)


def _finish_add(s):
    """Done with the Add dialog."""
    s.key(KEY_ESC, delay=0.8)


def _add_member(s):
    """Add Item for a row that is not asked: add, answer the Ok of "Added to
    project", and leave the dialog."""
    _add_member_asking(s)
    s.key(KEY_ENTER, delay=0.8)
    _finish_add(s)


def _field(text, name):
    for line in text.splitlines():
        if line.startswith(name + "="):
            return line.split("=", 1)[1].strip()
    return ""


# @scenario SC-read-owner-H1
def test_h1_a_project_swapped_between_two_reads_is_asked_about(tmp_path):
    """The Options box of a project whose name now leads to another file asks
    first.  No leaves the lists and settings as they were: the next write of
    the project (after the file is put back) carries this project's own
    compiler and program, not the defaults and not the other project's."""
    _files(tmp_path)
    with _session(tmp_path) as s:
        _open_project(s)
        os.rename(tmp_path / "p.prj", tmp_path / "keep.prj")
        _swap_by_rename(tmp_path)
        s.key(KEY_OPTIONS, delay=0.9)
        shown = _screen(s)
        assert READ_OTHER in shown, "nothing was asked about the other file:\n" + shown
        assert OPTIONS_BOX not in shown, "the options box opened on the other file:\n" + shown
        s.key(KEY_NO)
        shown = _screen(s)
        assert OPTIONS_BOX not in shown, "the options box opened after No:\n" + shown
        assert "m.c" in shown and "zz.c" not in shown, "the window lost its list:\n" + shown
        os.replace(tmp_path / "keep.prj", tmp_path / "p.prj")
        _add_member(s)
    written = (tmp_path / "p.prj").read_text()
    assert _field(written, "CMP") == "gcc", written
    assert _field(written, "EXENAME") == "p", "the settings after No were not the project's:\n" + written
    assert "m.c" in _field(written, "FILES") and "e.c" in _field(written, "FILES"), written


# @scenario SC-read-owner-H2
def test_h2_yes_reads_the_file_that_is_there_now(tmp_path):
    """Yes makes the file that is there now the project: the box shows its
    settings, and nothing is asked again."""
    _files(tmp_path)
    with _session(tmp_path) as s:
        _open_project(s)
        _swap_by_rename(tmp_path)
        s.key(KEY_OPTIONS, delay=0.9)
        assert READ_OTHER in _screen(s)
        s.key(KEY_ENTER, delay=0.9)
        shown = _screen(s)
        assert OPTIONS_BOX in shown and "clang" in shown, \
            "Yes did not read the newcomer:\n" + shown
        s.key(KEY_ESC, delay=0.8)
        s.key(KEY_OPTIONS, delay=0.9)
        shown = _screen(s)
        assert READ_OTHER not in shown and OPTIONS_BOX in shown, \
            "the editor asked again about a file it had accepted:\n" + shown


# @scenario SC-read-owner-H2b
def test_h2b_a_yes_is_about_the_file_that_was_asked_about(tmp_path):
    """A third file takes the name while the question is up: the Yes is not
    an answer about it.  It is refused with the sentence, not read, and not
    asked about again."""
    _files(tmp_path)
    third = tmp_path / "third.prj"
    third.write_text(OTHER_PROJECT.replace("clang", "tcc"))
    with _session(tmp_path) as s:
        _open_project(s)
        _swap_by_rename(tmp_path)
        s.key(KEY_OPTIONS, delay=0.9)
        assert READ_OTHER in _screen(s)
        os.replace(third, tmp_path / "p.prj")        # while the box is up
        s.key(KEY_YES, delay=1.0)
        shown = _screen(s)
        assert OPTIONS_BOX not in shown and "tcc" not in shown, \
            "a Yes about one file read another:\n" + shown
        assert READ_OTHER in shown, "the third file was not said:\n" + shown


# @scenario SC-read-owner-H3
def test_h3_the_editors_own_save_is_not_another_file(tmp_path):
    """A guard: after the editor wrote the project, on a system with no birth
    time, its next read asks nothing."""
    _files(tmp_path)
    with _session(tmp_path, NO_BIRTH) as s:
        _open_project(s)
        _add_member(s)
        s.key(KEY_OPTIONS, delay=0.9)
        shown = _screen(s)
        assert READ_OTHER not in shown and READ_UNSURE not in shown, \
            "the editor asked about its own save:\n" + shown
        assert OPTIONS_BOX in shown, shown


# @scenario SC-read-owner-H4
def test_h4_an_own_save_that_makes_the_file_again_is_not_another_file(tmp_path):
    """A guard: the project file removed outside and made again by the
    editor's save is the editor's own, on every system."""
    _files(tmp_path)
    with _session(tmp_path) as s:
        _open_project(s)
        os.remove(tmp_path / "p.prj")
        _add_member(s)
        assert (tmp_path / "p.prj").exists(), "the save did not make the project again"
        s.key(KEY_OPTIONS, delay=0.9)
        shown = _screen(s)
        assert READ_OTHER not in shown and READ_UNSURE not in shown, \
            "the editor asked about the file its own save made:\n" + shown
        assert OPTIONS_BOX in shown, shown


# @scenario SC-read-owner-H5
def test_h5_the_write_does_not_go_through_a_swapped_link(tmp_path):
    """The name became a link to another project's file: Add Item asks, the
    default answer is No, and the other project's file is as it was."""
    _files(tmp_path)
    with _session(tmp_path) as s:
        _open_project(s)
        _swap_by_link(tmp_path)
        before = (tmp_path / "real.prj").read_bytes()
        _add_member_asking(s)
        shown = _screen(s)
        assert WRITE_OTHER in shown, "nothing was asked about the other file:\n" + shown
        s.key(KEY_ENTER, delay=0.9)
        _finish_add(s)
        assert (tmp_path / "real.prj").read_bytes() == before, \
            "the project list was written into another project's file"
        assert (tmp_path / "keep.prj").read_bytes() == THIS_PROJECT.encode()


# @scenario SC-read-owner-H5b
def test_h5b_the_write_does_not_overwrite_a_file_renamed_over_the_name(tmp_path):
    """The same by rename-over: the newcomer is as it was after the default
    answer."""
    _files(tmp_path)
    with _session(tmp_path) as s:
        _open_project(s)
        _swap_by_rename(tmp_path)
        before = (tmp_path / "p.prj").read_bytes()
        _add_member_asking(s)
        assert WRITE_OTHER in _screen(s)
        s.key(KEY_ENTER, delay=0.9)
        _finish_add(s)
    assert (tmp_path / "p.prj").read_bytes() == before, \
        "the newcomer was overwritten by the default answer"


# @scenario SC-read-owner-H5d
def test_h5d_yes_writes_the_list_into_the_file_that_was_asked_about(tmp_path):
    """Yes writes this project's list over the file the user was asked about,
    once: the file is the project from then on and nothing is asked twice."""
    _files(tmp_path)
    with _session(tmp_path) as s:
        _open_project(s)
        _swap_by_rename(tmp_path)
        _add_member_asking(s)
        assert WRITE_OTHER in _screen(s)
        s.key(KEY_YES, delay=1.0)
        shown = _screen(s)
        assert WRITE_OTHER not in shown, "a Yes was asked about twice:\n" + shown
        assert "Added to project" in shown, shown
        s.key(KEY_ENTER, delay=0.8)           # the Ok of "Added to project"
        _finish_add(s)
        s.key(KEY_OPTIONS, delay=0.9)
        shown = _screen(s)
        assert READ_OTHER not in shown and OPTIONS_BOX in shown, \
            "the file the user said Yes about is not the project:\n" + shown
    written = (tmp_path / "p.prj").read_text()
    assert "e.c" in _field(written, "FILES"), "Yes did not write the list:\n" + written


# @scenario SC-read-owner-H5c
def test_h5c_the_write_question_defaults_to_no_and_the_read_question_to_yes(tmp_path):
    """Enter on the write question leaves the file; Enter on the read
    question reads it."""
    _files(tmp_path)
    with _session(tmp_path) as s:
        _open_project(s)
        _swap_by_rename(tmp_path)
        s.key(KEY_OPTIONS, delay=0.9)
        s.key(KEY_ENTER, delay=0.9)
        assert OPTIONS_BOX in _screen(s), "Enter did not answer Yes to the read question"


# @scenario SC-read-owner-H9
def test_h9_a_project_that_was_swapped_can_still_be_closed(tmp_path):
    """The write was refused, the window is marked unsaved, and closing it
    asks: No closes the window and the other file is byte for byte as it
    was."""
    _files(tmp_path)
    with _session(tmp_path) as s:
        _open_project(s)
        _swap_by_rename(tmp_path)
        before = (tmp_path / "p.prj").read_bytes()
        _add_member_asking(s)
        assert WRITE_OTHER in _screen(s)
        s.key(KEY_ENTER, delay=0.9)
        _finish_add(s)
        s.key(KEY_CLOSE_WINDOW, delay=0.9)
        shown = _screen(s)
        assert READ_OTHER in shown and CLOSE_TAIL in shown, \
            "closing a swapped project asked nothing about it:\n" + shown
        s.key(KEY_NO, delay=0.9)
        shown = _screen(s)
        assert PROJECT_WINDOW not in shown, "No did not close the project window:\n" + shown
    assert (tmp_path / "p.prj").read_bytes() == before, "the newcomer was written on a No"


def _leave_unsaved_over_a_swapped_name(s, tmp_path):
    """The name was swapped for another project's file, a member was added and
    the write question was answered No: the project window is unsaved."""
    _open_project(s)
    _swap_by_rename(tmp_path)
    _add_member_asking(s)
    assert WRITE_OTHER in _screen(s)
    s.key(KEY_ENTER, delay=0.9)
    _finish_add(s)


# @scenario SC-read-owner-H9b
def test_h9b_a_yes_to_reading_is_not_a_yes_to_writing_when_closing(tmp_path):
    """Enter on every question of the close: the read question defaults to Yes,
    and the write that follows is its own question, defaulting to No.  The
    other project's file is byte for byte as it was."""
    _files(tmp_path)
    with _session(tmp_path) as s:
        _leave_unsaved_over_a_swapped_name(s, tmp_path)
        before = (tmp_path / "p.prj").read_bytes()
        s.key(KEY_CLOSE_WINDOW, delay=0.9)
        assert READ_OTHER in _screen(s) and CLOSE_TAIL in _screen(s)
        s.key(KEY_ENTER, delay=0.9)
        shown = _screen(s)
        assert WRITE_OTHER in shown, \
            "a Yes to reading wrote the list without asking:\n" + shown
        s.key(KEY_ENTER, delay=0.9)
    assert (tmp_path / "p.prj").read_bytes() == before, "the newcomer was overwritten on Enter"


# @scenario SC-read-owner-H9c
def test_h9c_a_yes_to_reading_is_not_a_yes_to_writing_before_a_build(tmp_path):
    """The same on the build of a project with an unsaved list: Enter on the
    read question, then Enter on the write question, leaves the file."""
    _files(tmp_path)
    with _session(tmp_path) as s:
        _leave_unsaved_over_a_swapped_name(s, tmp_path)
        before = (tmp_path / "p.prj").read_bytes()
        s.key(KEY_F9, delay=0.9)
        assert READ_OTHER in _screen(s), "the build did not ask about the swapped file:\n" + _screen(s)
        s.key(KEY_ENTER, delay=0.9)
        shown = _screen(s)
        assert WRITE_OTHER in shown, \
            "a Yes to reading wrote the list without asking:\n" + shown
        s.key(KEY_ENTER, delay=0.9)
    assert (tmp_path / "p.prj").read_bytes() == before, "the newcomer was overwritten on Enter"


# @scenario SC-read-owner-HD
def test_hd_a_project_closed_to_reading_opens_no_box_on_defaults(tmp_path):
    """A refused read says why and stops: the Options box does not open on
    the compiler defaults, whose Save would then write them over the
    project."""
    _files(tmp_path)
    file_mode_is_obeyed(str(tmp_path), 0o000, os.O_RDONLY)
    with _session(tmp_path) as s:
        _open_project(s)
        (tmp_path / "p.prj").chmod(0)
        s.key(KEY_OPTIONS, delay=0.9)
        assert "Permission denied" in _screen(s)
        s.key(KEY_ENTER, delay=0.9)
        shown = _screen(s)
        assert OPTIONS_BOX not in shown, "the options box opened on defaults:\n" + shown
        (tmp_path / "p.prj").chmod(stat.S_IRUSR | stat.S_IWUSR)


def _session_name(place):
    """The file name the assistant keeps a project's conversation under: a
    hash of the project file's place (the editor's unsigned long djb2)."""
    h = 5381
    for byte in place.encode():
        h = (h * 33 + byte) & 0xFFFFFFFFFFFFFFFF
    return "project-%d.session" % h


# @scenario SC-read-owner-H6
def test_h6_the_conversation_follows_the_file_the_editor_read(tmp_path):
    """The project's name became a link to another project's file after the
    editor read the project: the conversation is still kept for the file the
    editor read, not for the other project."""
    work = tmp_path / "work"
    work.mkdir()
    _files(work)
    state = tmp_path / "state"
    trace = tmp_path / "ai.trace"
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_MOCK_REPLY": "noted",
           "XWPE_AI_TRACE": str(trace), "XDG_STATE_HOME": str(state)}
    expected = _session_name(os.path.realpath(work / "p.prj"))
    with WpeSession(str(work), "int a;\n", filename="zz_seed.c", env_extra=env) as s:
        _open_project(s)
        s.key(KEY_ESC, delay=0.8)             # the project window closes, the project stays open
        _swap_by_link(work)
        s.key("\033g", "a", "hello", delay=0.5)
        s.key(KEY_ENTER, delay=1.0)
        s._drain(3.0)
    saved = [l for l in trace.read_text().splitlines() if l.startswith("session save ")]
    assert saved, "no conversation was saved:\n" + trace.read_text()
    kept = os.path.basename(saved[-1].split(" ", 2)[2].rsplit(" cli=", 1)[0])
    assert kept == expected, "the conversation was kept for %s, not for the file read (%s)" % (kept, expected)


# @scenario SC-read-owner-H7
def test_h7_save_as_to_another_project_name_asks_nothing(tmp_path):
    """The user names another project file in Project-Options > Save As: the
    old project's hold does not decide about the new name."""
    _files(tmp_path)
    with _session(tmp_path) as s:
        _open_project(s)
        s.key(KEY_OPTIONS, delay=0.9)
        s.key("\033a", delay=0.9)               # Save As
        shown = _screen(s)
        assert "Project:" in shown
        s.key("q.prj", KEY_ENTER, delay=0.9)    # the typed name replaces the field's text
        shown = _screen(s)
        assert WRITE_OTHER not in shown and READ_OTHER not in shown, \
            "the old project's hold asked about the new name:\n" + shown
    assert _field((tmp_path / "q.prj").read_text(), "EXENAME") == "p", \
        "Save As did not write this project into q.prj"
