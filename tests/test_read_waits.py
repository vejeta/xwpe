"""A name the editor reads that is not a regular file never makes it wait.

Every file the editor reads by a name a folder, a source file or a user chose
goes through one owner (we_read.c).  It opens without waiting and without
taking a terminal, judges what is under the name before a byte is read, and
refuses a file over its bound whole.  These rows drive the editor as `wpe`
through the places that used to meet such a name with a plain fopen:

  A  the xwperc of the folder you started in is a link to /dev/zero, or a
     pipe, or you name /dev/zero with -sf: the editor starts, says what it
     found ("it is a link", "it is a pipe", "it is a device") and runs on your
     own options; Options > Save writes your own file, never the refused one.
  B  a source that includes "/dev/tty": F9 does not read the terminal the
     editor is running on, so the keys you type are the editor's.
  C  a project file that is a pipe: it is refused with its kind, the open
     project stays as it was.
  D  an options file or a project file one byte over its bound is refused
     whole, never read cut short.

HOME is a folder of the row's own, so nothing is read from or written into the
home of the user running the tests.  Rows A1 and A2 start the editor under an
address-space limit: an editor that reads /dev/zero as options has no end to
read, and the limit makes it stop at a known size instead of taking the
machine's memory.

Run: WPE_BIN=$PWD/wpe pytest tests/test_read_waits.py (as wpe, never we).
"""
import contextlib
import os
import resource
import shutil
import tempfile
import time
from pathlib import Path

import pytest

from host_tools import require
from wpe_driver import WpeSession, ALT

SEED = "int main(void){return 0;}\n"
RC_TEXT = "[General]\nTab : 5\n"
F9 = "\033[20~"
ENTER = "\r"
WIDE = 200
TALL = 40
ADDRESS_SPACE_BYTES = 2 << 30          # what a row that reads /dev/zero may hold
OPTIONS_BOUND = 1 << 20                # the owner's bound for an options file
PROJECT_BOUND = 16 << 20               # and for a project file
CHUNK = 1 << 20
PROJECT_TEXT = ("#\n# xwpe - project-file: p.prj\n#\n\nCMP=\tgcc\nCMPFLAGS=\t-g\n"
                "LDFLAGS=\nEXENAME=\tp\nCMPSWTCH=\tgnu\n\nFILES=\tt.c\n\n")


@pytest.fixture
def short_dir():
    """A short folder of the row's own: a sentence that names a long
    temporary path wraps, and the rows read its words, not its layout."""
    path = tempfile.mkdtemp(prefix="rw")
    yield Path(path)
    shutil.rmtree(path, ignore_errors=True)


@contextlib.contextmanager
def address_space_limit(nbytes):
    """The editor started inside this block cannot map more than nbytes.  The
    limit is on this process only while the block runs; the editor keeps the
    one it started with."""
    soft, hard = resource.getrlimit(resource.RLIMIT_AS)
    resource.setrlimit(resource.RLIMIT_AS, (nbytes, hard))
    try:
        yield
    finally:
        resource.setrlimit(resource.RLIMIT_AS, (soft, hard))


def _screen(session):
    return "\n".join(session.display())


def _env(home):
    return {"HOME": str(home), "XDG_CONFIG_HOME": "", "XDG_STATE_HOME": ""}


def _own_config(home, text):
    cfg = home / ".config" / "xwpe"
    cfg.mkdir(parents=True, exist_ok=True)
    (cfg / "xwperc").write_text(text)
    return cfg / "xwperc"


def _folders(short_dir):
    work = short_dir / "w"
    work.mkdir()
    home = short_dir / "h"
    home.mkdir()
    return work, home


def _save_options(session):
    """Options > Save, Enter on the file name."""
    session.key(ALT.OPTIONS)
    session.key("s", delay=0.8)
    session.key(ENTER, delay=1.0)


def _start_limited(work, home, args=()):
    """An editor in ``work`` started under the address-space limit."""
    with address_space_limit(ADDRESS_SPACE_BYTES):
        return WpeSession(str(work), SEED, env_extra=_env(home), args=args,
                          cols=WIDE, rows=TALL)


def _assert_said_and_own_options_saved(session, words, own, refused):
    """The editor started and said ``words``; its options are the user's own,
    so Options > Save writes ``own`` (with the line it read) and leaves the
    refused name alone."""
    assert session.alive(), "the editor did not start"
    shown = _screen(session)
    assert "Cannot open options file" in shown and words in shown, \
        "the refusal was not said:\n" + shown
    session.key(ENTER, delay=0.8)
    _save_options(session)
    saved = own.read_text()
    assert "[Programming]" in saved and "Tab : 5" in saved, \
        "Options > Save did not write the user's own file with what it read:\n" + saved
    assert os.path.lexists(refused), "the refused name was removed"


# @scenario SC-read-owner-01
def test_a_start_folder_xwperc_that_is_a_link_to_a_device_is_refused(short_dir):
    """A repository can commit ./xwperc as a link to anything.  The editor
    says "it is a link", runs on the user's own options and saves them to the
    user's own file."""
    work, home = _folders(short_dir)
    own = _own_config(home, RC_TEXT)
    (work / "xwperc").symlink_to("/dev/zero")
    with _start_limited(work, home) as s:
        _assert_said_and_own_options_saved(s, "it is a link", own, work / "xwperc")
    assert os.readlink(work / "xwperc") == "/dev/zero", "the link was replaced"


# @scenario SC-read-owner-02
def test_a_start_folder_xwperc_that_is_a_pipe_does_not_stop_the_start(short_dir):
    """A pipe named xwperc has no writer: opened by name the editor would wait
    for ever before its first paint.  It is refused by its kind."""
    work, home = _folders(short_dir)
    own = _own_config(home, RC_TEXT)
    os.mkfifo(work / "xwperc")
    with WpeSession(str(work), SEED, env_extra=_env(home), cols=WIDE, rows=TALL) as s:
        assert s.alive(), "the editor did not start"
        shown = _screen(s)
        assert "it is a pipe" in shown, \
            "the editor did not start, or did not say what it found:\n" + shown
        s.key(ENTER, delay=0.8)
        _save_options(s)
    assert "Tab : 5" in own.read_text()


# @scenario SC-read-owner-03
def test_sf_naming_a_device_is_refused_and_the_options_stay_the_users_own(short_dir):
    """-sf names the option file.  A device there is refused with its kind and
    the editor runs on the user's own file, which Options > Save writes."""
    work, home = _folders(short_dir)
    own = _own_config(home, RC_TEXT)
    with _start_limited(work, home, args=("-sf", "/dev/zero")) as s:
        _assert_said_and_own_options_saved(s, "it is a device", own, Path("/dev/zero"))


def _write_filler(path, size):
    """A file of ``size`` bytes of valid lines, made in chunks."""
    line = b"# filler line of a valid option file\n"
    with open(path, "wb") as fh:
        done = 0
        while done + len(line) <= size:
            fh.write(line)
            done += len(line)
        fh.write(b"#" * (size - done))


# @scenario SC-read-owner-04
def test_an_options_file_over_the_bound_is_refused_whole(short_dir):
    """A file cut short would be written back short by Options > Save, so a
    start-folder xwperc of 1 MiB and one byte is refused whole: none of its
    lines is applied and the user's own file is what is in force."""
    work, home = _folders(short_dir)
    own = _own_config(home, RC_TEXT)
    _write_filler(work / "xwperc", OPTIONS_BOUND + 1)
    with open(work / "xwperc", "ab") as fh:
        fh.write(b"\n[General]\nTab : 7\n")
    with WpeSession(str(work), SEED, env_extra=_env(home), cols=WIDE, rows=TALL) as s:
        assert s.alive(), "the editor did not start"
        shown = _screen(s)
        assert "it is larger than 1 MiB" in shown, \
            "the file over the bound was not refused:\n" + shown
        s.key(ENTER, delay=0.8)
        _save_options(s)
    saved = own.read_text()
    assert "Tab : 5" in saved and "Tab : 7" not in saved, \
        "a line of the refused file was applied:\n" + saved


BUILD_SECONDS = 3.0
BUILT_LINE = "0 errors, 0 warnings"      # what Messages says when a build succeeded
OLDER_SECONDS = 10                       # how much older than the object the source is made


# @scenario SC-read-owner-05
def test_a_source_that_includes_the_terminal_does_not_take_the_users_keys(short_dir):
    """#include "/dev/tty" in a cloned source: the header check opened the
    include text as written and read the editor's own terminal, so F9 waited
    on the user's keys and the screen stayed as it was.  A terminal is refused
    by its kind; the header counts as changed, so the build starts again (the
    compiler, not the editor's scan, meets the name).

    The object must be one this editor built, so the row builds a good source
    first and then puts the include on disk.  The second build's start is the
    proof: Messages no longer holds the first build's "0 errors" line.  What
    the compiler then does with /dev/tty is the compiler's and is not
    asserted."""
    require("gcc")
    work, home = _folders(short_dir)
    with WpeSession(str(work), SEED, env_extra=_env(home), cols=WIDE, rows=TALL) as s:
        s.key(F9, delay=BUILD_SECONDS)
        assert (work / "t.o").exists(), "the first build made no object:\n" + _screen(s)
        assert BUILT_LINE in _screen(s), "the first build did not succeed:\n" + _screen(s)
        (work / "t.c").write_text('#include "/dev/tty"\n' + SEED)
        # The source is older than the object, so only the header check can
        # say rebuild.  The object's own times stay as the build left them:
        # the editor trusts an object only when its record still matches.
        older = os.stat(work / "t.o").st_mtime - OLDER_SECONDS
        os.utime(work / "t.c", (older, older))
        s.key(F9, delay=BUILD_SECONDS)
        shown = _screen(s)
        assert s.alive(), "the editor does not answer"
        assert "gcc -c" in shown and BUILT_LINE not in shown, \
            "F9 did not start a build: the editor is reading the terminal:\n" + shown


def _open_project_from_files_panel(session, name):
    """File Manager (F3), type the name, Enter."""
    session.key("\033OR", delay=0.8)
    session.key(name, delay=0.3)
    session.key(ENTER, delay=1.0)


# @scenario SC-read-owner-06
def test_a_project_that_is_a_pipe_is_refused_and_no_project_opens(short_dir):
    """A pipe named as a project passed the old existence check, then waited in
    the open for a writer.  It is refused by its kind before the open project
    is touched, and no project is open afterwards."""
    work, home = _folders(short_dir)
    os.mkfifo(work / "p.prj")
    with WpeSession(str(work), SEED, env_extra=_env(home), cols=WIDE, rows=TALL) as s:
        assert s.alive(), "the editor did not start"
        _open_project_from_files_panel(s, "p.prj")
        shown = _screen(s)
        assert "Cannot open project file" in shown and "it is a pipe" in shown, \
            "the pipe was not refused by its kind:\n" + shown
        s.key(ENTER, delay=0.8)
        assert "Project:" not in _screen(s), \
            "a project window is open after the refusal:\n" + _screen(s)
        s.key(ALT.WINDOW if hasattr(ALT, "WINDOW") else "\033w", delay=0.6)
        assert s.alive(), "the editor does not answer"


# @scenario SC-read-owner-06b
def test_a_refused_project_leaves_the_open_project_as_it_was(short_dir):
    """Opening a project releases the old one first, but a refusal keeps it:
    the pipe is judged before anything changes, so the project window that was
    open is still there."""
    work, home = _folders(short_dir)
    (work / "good.prj").write_text(PROJECT_TEXT)
    os.mkfifo(work / "p.prj")
    with WpeSession(str(work), SEED, env_extra=_env(home), cols=WIDE, rows=TALL) as s:
        assert s.alive(), "the editor did not start"
        _open_project_from_files_panel(s, "good.prj")
        assert "Project: good.prj" in _screen(s), "the good project did not open:\n" + _screen(s)
        _open_project_from_files_panel(s, "p.prj")
        shown = _screen(s)
        assert "it is a pipe" in shown, "the pipe was not refused:\n" + shown
        s.key(ENTER, delay=0.8)
        assert "Project: good.prj" in _screen(s), \
            "the open project was released by a refused one:\n" + _screen(s)


# @scenario SC-read-owner-07
def test_a_project_file_over_the_bound_is_refused_whole(short_dir):
    """A project cut short would lose members on the next write, so one of
    16 MiB and one byte is refused whole, with the bound named."""
    work, home = _folders(short_dir)
    prj = work / "big.prj"
    with open(prj, "wb") as fh:
        fh.write(PROJECT_TEXT.encode())
        fh.write(b"#" * (PROJECT_BOUND + 1 - len(PROJECT_TEXT)))
    with WpeSession(str(work), SEED, env_extra=_env(home), cols=WIDE, rows=TALL) as s:
        assert s.alive(), "the editor did not start"
        _open_project_from_files_panel(s, "big.prj")
        shown = _screen(s)
        assert "Cannot open project file" in shown and "it is larger than 16 MiB" in shown, \
            "the file over the bound was not refused:\n" + shown
