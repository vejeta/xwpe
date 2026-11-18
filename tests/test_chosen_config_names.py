"""A name the editor chooses, or a folder a repository names, is never written
through a link and never removes or loads a file of the user's.

A folder you cloned can carry a ./xwperc, a project file, a file called
Messages and a file called <program>.dbgerr.  None of them is the user's:

  - Options > Save never rewrites the xwperc of the folder you started in
    through a link a repository committed there; your own configuration file,
    which a dotfiles manager may link, still saves.
  - The project list is never written through a link at the name of a project
    that the start folder's xwperc named.
  - A Project that the start folder's xwperc names outside that folder is not
    opened, and the editor says why; one inside it is opened as before.
  - A build neither removes nor shows a file called Messages in the folder,
    and the debug compile keeps its compiler's messages out of a file beside
    the source.
  - The one place a user names a file to read as messages, -pm, still works.

Every row asserts on the file on disk first and on the screen second.  HOME is
a folder of the row's own, so nothing is read from or written into the home of
the user running the tests.

Run: WPE_BIN=$PWD/wpe pytest tests/test_chosen_config_names.py (as wpe, never we).
"""
import os
import shutil
import stat
import tempfile
from pathlib import Path

import pytest

from wpe_driver import WpeSession, ALT

SEED = "int main(void){return 0;}\n"
RC_TEXT = "[General]\nTab : 5\n"
KEEP = "KEEP\n"
LINK_WORDS = "is a link"
OUTSIDE_WORDS = "outside this folder"
PROJECT_TEXT = ("#\n# xwpe - project-file: p.prj\n#\n\nCMP=\tgcc\nCMPFLAGS=\t-g\n"
                "LDFLAGS=\nEXENAME=\tp\nCMPSWTCH=\tgnu\n\nFILES=\tmain.c\n\n")
F9 = "\033[20~"
CTRL_G = "\x07"
STALE_MESSAGES = "KEEPMARK t.c:3: error: stale text of the user's own\n"


@pytest.fixture
def short_dir():
    """A short folder of the row's own: the Save dialog's name field holds
    only so many bytes, and a long temporary path would turn Options > Save
    into a different question."""
    path = tempfile.mkdtemp(prefix="c2b")
    yield Path(path)
    shutil.rmtree(path, ignore_errors=True)


def _screen(session):
    return "\n".join(session.display())


def _env(home):
    return {"HOME": str(home), "XDG_CONFIG_HOME": "", "XDG_STATE_HOME": ""}


def _own_config(home, text):
    cfg = home / ".config" / "xwpe"
    cfg.mkdir(parents=True, exist_ok=True)
    (cfg / "xwperc").write_text(text)
    return cfg / "xwperc"


def _save_options(session):
    """Options > Save, Enter on the file name."""
    session.key(ALT.OPTIONS)
    session.key("s", delay=0.8)
    session.key("\r", delay=1.0)


# @scenario SC-chosen-config-names-15
def test_options_save_does_not_write_through_a_linked_start_xwperc(short_dir):
    work = short_dir / "w"
    work.mkdir()
    home = short_dir / "h"
    home.mkdir()
    victim = short_dir / "victim"
    victim.write_text(RC_TEXT)
    (work / "xwperc").symlink_to(victim)
    with WpeSession(str(work), SEED, env_extra=_env(home)) as s:
        assert s.alive(), "the editor died at start"
        _save_options(s)
        shown = _screen(s)
    assert victim.read_text() == RC_TEXT, \
        "the options were written through the link:\n" + victim.read_text()
    assert (work / "xwperc").is_symlink(), "the link was replaced"
    assert LINK_WORDS in shown, "the user was not told why nothing was saved:\n" + shown


# @scenario SC-chosen-config-names-16
def test_a_linked_users_own_config_still_saves(short_dir):
    """Guard: the user's own configuration file may be a link (stow); a save
    from a folder with no xwperc of its own follows it."""
    work = short_dir / "w"
    work.mkdir()
    home = short_dir / "h"
    cfg = home / ".config" / "xwpe"
    cfg.mkdir(parents=True)
    dotfiles = short_dir / "dot"
    dotfiles.mkdir()
    target = dotfiles / "xwperc"
    target.write_text(RC_TEXT)
    (cfg / "xwperc").symlink_to(target)
    with WpeSession(str(work), SEED, env_extra=_env(home)) as s:
        _save_options(s)
    assert (cfg / "xwperc").is_symlink(), "the user's link was replaced"
    saved = target.read_text()
    assert "[Programming]" in saved, "the link's target was not saved to:\n" + saved
    assert "Tab : 5" in saved, "his own setting was lost:\n" + saved


# @scenario SC-chosen-config-names-16c
def test_started_inside_the_config_folder_a_linked_xwperc_still_saves(short_dir):
    """Started from ~/.config/xwpe, the folder's xwperc IS the user's own file,
    and a stow link there is followed on Save, as when started anywhere else.
    The screen is not read: the box that says "is a link" wraps."""
    home = short_dir / "h"
    cfg = home / ".config" / "xwpe"
    cfg.mkdir(parents=True)
    dotfiles = short_dir / "dot"
    dotfiles.mkdir()
    target = dotfiles / "xwperc"
    target.write_text(RC_TEXT)
    (cfg / "xwperc").symlink_to(target)
    with WpeSession(str(cfg), SEED, env_extra=_env(home)) as s:
        assert s.alive(), "the editor died at start"
        _save_options(s)
    assert (cfg / "xwperc").is_symlink(), "the user's link was replaced"
    assert "[Programming]" in target.read_text(), \
        "the options were not saved through the user's own link:\n" + target.read_text()


# @scenario SC-chosen-config-names-17
def test_a_regular_start_xwperc_still_saves(short_dir):
    """Guard: the folder's own xwperc, a regular file, is saved as before."""
    work = short_dir / "w"
    work.mkdir()
    home = short_dir / "h"
    home.mkdir()
    (work / "xwperc").write_text(RC_TEXT)
    with WpeSession(str(work), SEED, env_extra=_env(home)) as s:
        _save_options(s)
        shown = _screen(s)
    saved = (work / "xwperc").read_text()
    assert "[Programming]" in saved, "the folder's xwperc was not saved:\n" + saved
    assert LINK_WORDS not in shown


def _clone_with_project(work, project_name, links_to=None):
    """A start folder whose xwperc names a project, as a cloned repository does."""
    work.mkdir(exist_ok=True)
    (work / "main.c").write_text("int x;\n")
    (work / "xwperc").write_text("[Programming]\nProject : %s\n" % project_name)
    if links_to is not None:
        (work / "p.prj").symlink_to(links_to)
    else:
        (work / "p.prj").write_text(PROJECT_TEXT)


def _add_main_to_project(session):
    """Project > Add Item, then the Files box: Enter adds the file under the cursor."""
    session.key(ALT.PROJECT, delay=0.6)
    session.key("a", delay=0.8)
    session.key("\r", delay=0.8)
    session.key("\r", delay=1.0)


# @scenario SC-chosen-config-names-25
def test_a_folder_named_project_file_is_not_written_through_a_link(tmp_path):
    work = tmp_path / "proj"
    _clone_with_project(work, "p.prj", links_to="notes.txt")
    (work / "notes.txt").write_text(KEEP)
    home = tmp_path / "home"
    home.mkdir()
    with WpeSession(str(work), SEED, filename="main.c", env_extra=_env(home)) as s:
        assert s.alive(), "the editor died at start"
        _add_main_to_project(s)
        shown = _screen(s)
    assert (work / "notes.txt").read_text() == KEEP, \
        "the project list was written into the file the link names:\n" + \
        (work / "notes.txt").read_text()
    assert (work / "p.prj").is_symlink(), "the link was replaced"
    assert LINK_WORDS in shown, "the user was not told why the list was not written:\n" + shown


# @scenario SC-chosen-config-names-26
def test_a_project_file_that_is_a_regular_file_is_still_written(tmp_path):
    """Guard: a start folder's project that is a plain file takes its list."""
    work = tmp_path / "proj"
    _clone_with_project(work, "p.prj")
    (work / "extra.c").write_text("int y;\n")
    home = tmp_path / "home"
    home.mkdir()
    with WpeSession(str(work), SEED, filename="main.c", env_extra=_env(home)) as s:
        _add_main_to_project(s)
        shown = _screen(s)
    assert LINK_WORDS not in shown, shown
    assert "FILES=" in (work / "p.prj").read_text()


# @scenario SC-chosen-config-names-31
@pytest.mark.parametrize("how", ["absolute", "relative", "linked"])
def test_a_project_the_start_folder_names_outside_it_is_not_opened(tmp_path, how):
    elsewhere = tmp_path / "elsewhere"
    elsewhere.mkdir()
    (elsewhere / "real.prj").write_text(PROJECT_TEXT)
    work = tmp_path / "proj"
    work.mkdir()
    (work / "main.c").write_text("int x;\n")
    if how == "absolute":
        named = str(elsewhere / "real.prj")
    elif how == "relative":
        named = "../elsewhere/real.prj"
    else:
        (work / "away").symlink_to(elsewhere)
        named = "away/real.prj"
    (work / "xwperc").write_text("[Programming]\nProject : %s\n" % named)
    home = tmp_path / "home"
    home.mkdir()
    with WpeSession(str(work), SEED, filename="main.c", env_extra=_env(home),
                    wait=2.0) as s:
        shown = _screen(s)
        assert OUTSIDE_WORDS in shown, \
            "the editor did not say the project was refused:\n" + shown
        s.key("\r", delay=0.6)
        s.key(ALT.PROJECT, delay=0.6)
        s.key("a", delay=0.8)
        after = _screen(s)
    assert "Add Files to Project" not in after, \
        "a project that lies outside the start folder was opened:\n" + after


# @scenario SC-chosen-config-names-32
@pytest.mark.parametrize("named", ["p.prj", "./p.prj", "sub/../p.prj"])
def test_a_project_inside_the_start_folder_is_opened(tmp_path, named):
    """Guard: a project in the folder itself is opened with no question."""
    work = tmp_path / "proj"
    (work / "sub").mkdir(parents=True)
    _clone_with_project(work, named)
    home = tmp_path / "home"
    home.mkdir()
    with WpeSession(str(work), SEED, filename="main.c", env_extra=_env(home),
                    wait=2.0) as s:
        shown = _screen(s)
        assert OUTSIDE_WORDS not in shown, \
            "a project inside the folder was refused:\n" + shown
        s.key(ALT.PROJECT, delay=0.6)
        s.key("a", delay=0.8)
        after = _screen(s)
    assert "Add Files to Project" in after, \
        "the project in the folder was not opened:\n" + after


# @scenario SC-chosen-config-names-33
def test_a_project_the_user_names_in_his_own_config_is_not_judged(tmp_path):
    """Guard: only the xwperc of the folder you started in is judged; a
    project named in your own configuration is yours, wherever it is."""
    elsewhere = tmp_path / "elsewhere"
    elsewhere.mkdir()
    (elsewhere / "mine.prj").write_text(PROJECT_TEXT)
    work = tmp_path / "work"
    work.mkdir()
    (work / "main.c").write_text("int x;\n")
    home = tmp_path / "home"
    _own_config(home, "[Programming]\nProject : %s\n" % (elsewhere / "mine.prj"))
    with WpeSession(str(work), SEED, filename="main.c", env_extra=_env(home),
                    wait=2.0) as s:
        shown = _screen(s)
    assert OUTSIDE_WORDS not in shown, shown


# @scenario SC-chosen-config-names-16b
def test_a_build_keeps_a_file_named_messages(tmp_path):
    """F9 neither removes ./Messages nor shows its text in the Messages window."""
    work = tmp_path / "w"
    work.mkdir()
    (work / "Messages").write_text(STALE_MESSAGES)
    with WpeSession(str(work), "int main(void){ return x; }\n", cols=120,
                    env_extra=_env(tmp_path / "h")) as s:
        s.key(F9, delay=4.0)
        shown = _screen(s)
    assert (work / "Messages").read_text() == STALE_MESSAGES, \
        "the user's Messages file was removed or changed"
    assert "Messages" in shown and "undeclared" in shown, \
        "the build's own messages are not in the window:\n" + shown
    assert "KEEPMARK" not in shown, \
        "the user's Messages file was loaded into the Messages window:\n" + shown


# @scenario SC-chosen-config-names-34
def test_the_messages_window_never_shows_a_file_of_the_same_name(tmp_path):
    """GUARD, not red on base (the base empties this buffer after reading the
    file): Window > Messages before any build shows the editor's own window,
    not a file called Messages in the folder, and quitting leaves it as it was.
    The red row for "the window does not load the file" is the Watches one."""
    work = tmp_path / "w"
    work.mkdir()
    (work / "Messages").write_text(STALE_MESSAGES)
    with WpeSession(str(work), SEED, cols=120, env_extra=_env(tmp_path / "h")) as s:
        s.key(ALT.WINDOW, delay=0.6)
        s.key("m", delay=1.0)
        shown = _screen(s)
        s.key("\033x", delay=1.0)
    assert "KEEPMARK" not in shown, \
        "the user's Messages file was loaded into the window:\n" + shown
    assert "No Messages" in shown, "the Messages window is not the editor's own:\n" + shown
    assert (work / "Messages").read_text() == STALE_MESSAGES, \
        "the user's Messages file was changed"


# @scenario SC-chosen-config-names-34b
def test_the_watches_window_never_shows_a_file_of_the_same_name(tmp_path):
    """./Watches is read into the Watches window by the original.  The window
    is the debugger's own: a file of that name in the folder is not shown."""
    work = tmp_path / "w"
    work.mkdir()
    (work / "Watches").write_text("WATCHMARK planted text\n")
    with WpeSession(str(work), SEED, cols=120, env_extra=_env(tmp_path / "h")) as s:
        s.key(ALT.WINDOW, delay=0.6)
        s.key("w", delay=1.0)
        shown = _screen(s)
    assert "WATCHMARK" not in shown, \
        "the file called Watches was loaded into the Watches window:\n" + shown
    assert (work / "Watches").read_text() == "WATCHMARK planted text\n"


# @scenario SC-chosen-config-names-35
def test_pm_still_reads_the_file_the_user_names_as_messages(tmp_path):
    """Guard: `wpe -pm <file>` is the user naming the file to read as messages."""
    work = tmp_path / "w"
    work.mkdir()
    with WpeSession(str(work), "KEEPMARK t.c:1: error: boom\n", filename="build.log",
                    args=("-pm",), cols=120, env_extra=_env(tmp_path / "h")) as s:
        shown = _screen(s)
    assert "Messages" in shown and "KEEPMARK" in shown, \
        "-pm did not read the named file as messages:\n" + shown


def _debug_compile(tmp_path, folder, leave=3.0):
    """Ctrl-G r on a.rs with a stand-in compiler that records its arguments."""
    from test_path_debugger import _compiler_started, _recording_env
    from test_signal_save import DAP_RUST_PROG
    home = tmp_path / "home"
    home.mkdir(exist_ok=True)
    env = _recording_env(tmp_path, "")
    env.update({"HOME": str(home)})
    with WpeSession(str(folder), DAP_RUST_PROG, filename="a.rs", wait=1.5,
                    env_extra=env) as s:
        s.key(CTRL_G, "r", delay=0.2)
        assert _compiler_started(s, tmp_path) or s.alive()
        s._drain(leave)
        assert s.alive(), "the editor died in the debug compile:\n" + _screen(s)


# @scenario SC-chosen-config-names-17b
def test_a_users_own_dbgerr_beside_the_source_is_kept(tmp_path):
    """The compiler's messages go to the editor's private folder: a file of
    the user's called a.dbgerr is neither written nor removed."""
    folder = tmp_path / "work"
    folder.mkdir()
    (folder / "a.dbgerr").write_text(KEEP)
    _debug_compile(tmp_path, folder)
    assert (folder / "a.dbgerr").exists(), "the user's a.dbgerr was removed"
    assert (folder / "a.dbgerr").read_text() == KEEP, "the user's a.dbgerr was overwritten"


# @scenario SC-chosen-config-names-17c
def test_a_dbgerr_link_beside_the_source_is_not_written_through(tmp_path):
    folder = tmp_path / "work"
    folder.mkdir()
    victim = tmp_path / "victim"
    victim.write_text(KEEP)
    (folder / "a.dbgerr").symlink_to(victim)
    _debug_compile(tmp_path, folder)
    assert victim.read_text() == KEEP, "the compiler's messages went through the link"
    assert (folder / "a.dbgerr").is_symlink(), "the link was removed"


# @scenario SC-chosen-config-names-36
def test_the_migration_does_not_write_through_a_link(tmp_path):
    """The old ~/.xwpe/xwperc is carried over to the XDG folder as a new file:
    a link already at its name is not written through, the old file is kept
    in use, and nothing is lost."""
    home = tmp_path / "home"
    (home / ".xwpe").mkdir(parents=True)
    (home / ".xwpe" / "xwperc").write_text(RC_TEXT)
    cfg = home / ".config" / "xwpe"
    cfg.mkdir(parents=True)
    victim = tmp_path / "victim"
    victim.write_text(KEEP)
    (cfg / "xwperc").symlink_to(tmp_path / "missing-target")     # dangling
    work = tmp_path / "w"
    work.mkdir()
    with WpeSession(str(work), SEED, env_extra=_env(home)) as s:
        assert s.alive(), "the editor died at start"
    assert not (tmp_path / "missing-target").exists(), \
        "the migration created the file a dangling link names"
    assert victim.read_text() == KEEP


# @scenario SC-chosen-config-names-37
def test_the_migration_copy_and_note_are_new_private_files(tmp_path):
    old = os.umask(0o022)
    try:
        home = tmp_path / "home"
        (home / ".xwpe").mkdir(parents=True)
        (home / ".xwpe" / "xwperc").write_text(RC_TEXT)
        work = tmp_path / "w"
        work.mkdir()
        with WpeSession(str(work), SEED, env_extra=_env(home)) as s:
            assert s.alive(), "the editor died at start"
    finally:
        os.umask(old)
    new = home / ".config" / "xwpe" / "xwperc"
    note = home / ".xwpe" / "MOVED_TO_XDG.txt"
    assert new.read_text() == RC_TEXT, "the settings were not carried over"
    assert note.exists() and "XDG" in note.read_text(), "no note in the old folder"
    for made in (new, note):
        assert stat.S_IMODE(made.stat().st_mode) == 0o600, "%s is not private" % made
