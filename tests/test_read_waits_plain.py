"""Files the editor reads at start-up, named by the user's home or the
installation, never make it wait.

The read owner (we_read.c) opens every such file without waiting and judges
what is under the name before a byte is read.  These rows put a pipe with no
writer where the editor reads a file of its own at start, drive it as `wpe`,
and expect the first paint within the harness deadline: a plain fopen of the
name waits for a writer for ever and the screen stays blank.

  A  the user's own syntax_def (~/.xwpe/syntax_def) is a pipe: the editor
     starts and highlights with the installed rules.
  B  the old configuration file (~/.xwpe/xwperc), read once to be carried
     over to the new place, is a pipe: the editor starts.
  C  the installed help.key is a pipe (an installation folder named by
     XWPE_LIB): the editor starts with no key help.

HOME is a folder of the row's own, so nothing is read from or written into
the home of the user running the tests.

Run: WPE_BIN=$PWD/wpe pytest tests/test_read_waits_plain.py (as wpe, never we).
"""
import os
from pathlib import Path

from wpe_driver import WpeSession, WPE_BIN
from test_read_waits import (  # noqa: F401  (short_dir is a fixture)
    SEED, TALL, WIDE, _env, _folders, _screen, short_dir)

SEED_WORD = "int main"          # what the first paint shows of the seed file


def _assert_painted(session, what):
    """The editor started and drew the seed file: it did not wait on a pipe."""
    assert session.alive(), "the editor did not start"
    shown = _screen(session)
    assert SEED_WORD in shown, \
        "the editor did not paint while it read %s:\n%s" % (what, shown)


# @scenario SC-read-owner-p3a-01
def test_a_personal_syntax_file_that_is_a_pipe_does_not_stop_the_start(short_dir):
    """A pipe named ~/.xwpe/syntax_def has no writer: opened by name the
    editor waits before its first paint.  It is refused by its kind and the
    installed rules colour the text."""
    work, home = _folders(short_dir)
    (home / ".xwpe").mkdir()
    os.mkfifo(home / ".xwpe" / "syntax_def")
    with WpeSession(str(work), SEED, env_extra=_env(home), cols=WIDE, rows=TALL) as s:
        _assert_painted(s, "the personal syntax file")


# @scenario SC-read-owner-p3a-02
def test_an_old_options_file_that_is_a_pipe_does_not_stop_the_start(short_dir):
    """The old ~/.xwpe/xwperc is read once to carry it over to the new
    folder.  A pipe there has no writer: opened by name the editor waits
    before its first paint.  It is refused by its kind and named, and the
    editor starts on its default options."""
    work, home = _folders(short_dir)
    (home / ".xwpe").mkdir()
    os.mkfifo(home / ".xwpe" / "xwperc")
    with WpeSession(str(work), SEED, env_extra=_env(home), cols=WIDE, rows=TALL) as s:
        assert s.alive(), "the editor did not start"
        shown = _screen(s)
        assert "Cannot open options file" in shown and "it is a pipe" in shown, \
            "the editor did not paint, or did not say what it found:\n" + shown


def _installation_with_a_pipe_for_help_key(folder):
    """A copy of the editor's data folder, by links, whose help.key is a pipe.
    Returns the folder to give as XWPE_LIB."""
    data = Path(os.path.dirname(os.path.abspath(WPE_BIN)))
    lib = folder / "lib"
    lib.mkdir()
    for entry in data.iterdir():
        if entry.is_file() and entry.name != "help.key_eng":
            os.symlink(entry, lib / entry.name)
    os.mkfifo(lib / "help.key")
    return lib


# @scenario SC-read-owner-p3a-03
def test_an_installed_key_help_that_is_a_pipe_does_not_stop_the_start(short_dir):
    """help.key is read at start from the installation folder.  A pipe there
    has no writer: opened by name the editor waits before its first paint.
    It is refused by its kind and the editor starts without key help."""
    work, home = _folders(short_dir)
    lib = _installation_with_a_pipe_for_help_key(short_dir)
    env = _env(home)
    env["XWPE_LIB"] = str(lib)
    with WpeSession(str(work), SEED, env_extra=env, cols=WIDE, rows=TALL) as s:
        _assert_painted(s, "the installed key help")
