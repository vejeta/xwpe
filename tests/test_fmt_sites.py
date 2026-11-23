"""The editor's own formatted texts: what the user reads stays whole where it
fits, and a text that does not fit is refused or cut, never sent or drawn
past its place (we_fmt.c).

Rows:
  - Search > Replace, Change All: the box still says "Found N" and
    "Replaced N" (the counts are built by the owner of the numbers);
  - the window frame shows the line and column of a long file in full;
  - the program's Arguments are too long for the debugger's command: the run
    is refused on the status row and the editor answers the next key; before
    this the command went to the debugger without its line end or its
    redirect.
  - Make Directory in a folder gives "new.dir" inside it;
  - Add from a subfolder stores the name "sub/extra.c" in the project;
  - a project file whose last line is a comment with no line end opens
    (the reader used to wait for a line end that never came).

Run: WPE_BIN=$PWD/wpe tests/.venv/bin/python -m pytest -q tests/test_fmt_sites.py
(under a sanitizer build, launch it as wpe-asan).
"""
import os

import pytest

pytest.importorskip("pyte")

from host_tools import needs                       # noqa: E402
from wpe_driver import WpeSession, ALT, wait_screen   # noqa: E402

CTRL_END = "\033[1;5F"
CTRL_G = "\x07"
LONG_FILE_LINES = 100000
ARGUMENTS_BYTES = 6000          # more than the editor's run command line holds
RUN_REFUSED = "Run not started: the Arguments are too long"
BOX_WAIT = 40

needs_gdb = needs("gcc", "gdb")


def _screen(s):
    return "\n".join(s.display())


# @scenario SC-fmt-30
def test_change_all_box_keeps_its_counts(tmp_path):
    """Change All of three hits ends in the box "Found 3 / Replaced 3"."""
    with WpeSession(str(tmp_path), "foo 1\nfoo 2\nbar foo\n") as s:
        s.menu(ALT.SEARCH, "r")
        s.key("foo")
        s.key("\033n")
        s.key("ZZZ")
        s.key("\033p")
        s.key("\033a")
        screen = _screen(s)
        assert "Found 3" in screen and "Replaced 3" in screen, screen
        s.key("\r")
        assert s.alive()


# @scenario SC-fmt-31
def test_frame_shows_the_line_of_a_long_file_whole(tmp_path):
    """A file of 100000 lines, the cursor at its end: the bottom frame of the
    window shows 100000:1, the number whole in its field."""
    seed = "x\n" * (LONG_FILE_LINES - 1) + "x"
    with WpeSession(str(tmp_path), seed) as s:
        s.menu(ALT.SEARCH, "g")
        s.key("100000", "\r", delay=1.5)
        screen = _screen(s)
        assert "100000:1" in screen, screen
        assert s.alive()


PRJ_TAIL = "CMP=\tgcc\nEXENAME=\tdemo\nFILES=\tmain.c\n"


# @scenario SC-fmt-33
def test_make_directory_makes_new_dir_in_the_folder(tmp_path):
    """File Manager, Make Directory: "new.dir" appears inside the folder the
    manager shows (the name is joined to the folder by the path owner)."""
    with WpeSession(str(tmp_path), "x\n") as s:
        s.key("\x1bOR", delay=0.5)           # F3: the File Manager
        s.key("\033k", delay=1.0)            # Alt-K: Make Directory
        assert s.alive()
    assert (tmp_path / "new.dir").is_dir(), sorted(os.listdir(str(tmp_path)))


# @scenario SC-fmt-34
def test_add_from_a_subfolder_stores_the_relative_name(tmp_path):
    """Project > Add from a subfolder: the project stores the name as the
    manager built it, "sub/extra.c" (the folder joined to the name with the
    bytes the project list has always held)."""
    (tmp_path / "sub").mkdir()
    (tmp_path / "main.c").write_text("int x;\n")
    (tmp_path / "sub" / "extra.c").write_text("int y;\n")
    (tmp_path / "p.prj").write_text("#\n# xwpe - project-file: p.prj\n" + PRJ_TAIL.replace("demo", "p"))
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n", filename="main.c") as s:
        s.key("\033p", "p", "\r", "\r", delay=1.0)       # Open project p.prj
        s.key("\033a", delay=1.0)                        # Add files
        s.key("\t", "\t", delay=0.4)                     # to the Directory field
        s.key(str(tmp_path / "sub"), "\r", delay=1.0)    # change folder
        s.key("\t", "\t", "\t", delay=0.4)               # to the Files list
        s.key("\r", delay=1.0)                           # add extra.c
        assert s.alive()
    files = [l for l in (tmp_path / "p.prj").read_text().splitlines() if l.startswith("FILES")]
    assert files and "sub/extra.c" in files[0], files


# @scenario SC-fmt-35
def test_project_file_ending_in_a_comment_without_a_line_end_opens(tmp_path):
    """A .prj whose last line is a comment with no line end used to hang the
    reader (it waited for a line end that never came); it opens, and the
    window is titled with the project."""
    (tmp_path / "demo.prj").write_text(PRJ_TAIL + "# last")
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n", filename="main.c") as s:
        s.key("\033p", "p", "\r", "\r", delay=1.5)
        assert wait_screen(s, "Project: demo.prj", tmo=BOX_WAIT), _screen(s)


# @scenario SC-fmt-36
def test_project_file_ending_in_a_comment_with_a_line_end_opens(tmp_path):
    """The control of the row above: the same file with the line end."""
    (tmp_path / "demo.prj").write_text(PRJ_TAIL + "# last\n")
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n", filename="main.c") as s:
        s.key("\033p", "p", "\r", "\r", delay=1.5)
        assert wait_screen(s, "Project: demo.prj", tmo=BOX_WAIT), _screen(s)


# @scenario SC-fmt-32
@needs_gdb
def test_arguments_too_long_for_the_run_command_are_refused(tmp_path):
    """Arguments of 4.7 KB in the user's own options: Ctrl-G R builds and
    starts gdb, and the run is refused with a sentence on the status row
    instead of sending gdb a command without its end; the editor then answers
    the next key."""
    home = tmp_path / "home"
    conf = home / ".config" / "xwpe"
    conf.mkdir(parents=True)
    (conf / "xwperc").write_text("[Programming]\nArguments : %s\n" % ("a" * ARGUMENTS_BYTES))
    work = tmp_path / "ws"
    work.mkdir()
    env = {"HOME": str(home)}
    with WpeSession(str(work), "int main(void){return 0;}\n", filename="t.c",
                    env_extra=env) as s:
        s.key(CTRL_G, "r", delay=1.0)
        refused = wait_screen(s, RUN_REFUSED, tmo=BOX_WAIT)
        screen = _screen(s)
        assert refused, "the run was not refused:\n" + screen
        s.key("x")
        assert s.alive(), "the editor did not answer after the refusal"
