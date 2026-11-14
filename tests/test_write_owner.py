"""The owner of "write the user's text": what the user is told and what stays
on disk when a write fails (we_write.c).

Every write of the user's own file, a block or a print copy has one outcome
the caller reads: written, declined or failed.  A failed write says once,
in one sentence, the path and the reason and what is kept; a write that
fails after the file was opened puts the old content back from the backup;
a backup that cannot be made says so on the status row and the save goes
on; a backup that stopped half way refuses the save.

Manual lines (not asserted here): a real full disk; a file system whose
fclose succeeds and loses the data later.

Run: tests/.venv/bin/python -m pytest -v tests/test_write_owner.py
"""
import os
import stat
import re
import time

import pytest

pytest.importorskip("pyte")

import wpe_driver                                    # noqa: E402
from wpe_driver import ALT, WpeSession, WAIT_SCALE  # noqa: E402

F2 = "\033OQ"                  # Save
F9 = "\033[20~"                # Make
ESC = "\033"
KIB = 1024
LINE = "a" * 63 + "\n"         # a line of exactly 64 bytes
SEED = "alpha\n"
POLL_S = 0.2
RAN_S = 15


class Squashed(str):
    """The screen's text without its spaces.  A sentence the box wrapped has
    the box's side lines ("x" in the terminal's line set), its shadow ("a")
    and the text the window shows beside it ("b" in the growing rows) between
    its words, so `needle in squashed` allows a run of them between any two
    letters."""

    def __contains__(self, needle):
        pattern = "[xab]*".join(re.escape(c) for c in needle)
        return re.search(pattern, str(self)) is not None


def _squash(s):
    """What the screen says, with the spaces taken out: a sentence the box
    wrapped at a space or in a long path reads as one."""
    text = "".join(s.display())
    return Squashed(re.sub(r"[^\x21-\x7e]", "", text))


def _setup(tmp_path):
    """A work folder, and a state folder outside it: the driver makes the
    work folder the HOME, so locking it must not lock the state folder."""
    work = tmp_path / "work"
    work.mkdir()
    state = tmp_path / "state"
    state.mkdir()
    return str(work), {"XDG_STATE_HOME": str(state)}


def _lock_folder(work):
    os.chmod(work, stat.S_IRUSR | stat.S_IXUSR)


def _unlock_folder(work):
    os.chmod(work, stat.S_IRWXU)


def _write_executable(path, text):
    with open(path, "w") as fh:
        fh.write(text)
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)


def _limited_editor(tmp_path, monkeypatch, kib):
    """The editor started under `ulimit -f <kib>`, still under its own name:
    a write past the limit then fails (SIGXFSZ ignored by the owner)."""
    real = os.path.abspath(wpe_driver.WPE_BIN)
    name = os.path.basename(real)
    bindir = tmp_path / "limited"
    bindir.mkdir()
    wrapper = str(bindir / name)
    _write_executable(wrapper, "#!/bin/bash\nulimit -f %d\nexec -a %s %s \"$@\"\n"
                      % (kib, name, real))
    monkeypatch.setattr(wpe_driver, "WPE_BIN", wrapper)
    return {"XWPE_LIB": os.path.dirname(real)}


def _grow(s, kib=3):
    """Add `kib` KiB of lines at the top of the window, as typed text."""
    lines = "".join("b" * 63 + "\r" for _ in range(kib * KIB // 64))
    s.paste(lines, lambda: None)
    s.display()


def _seed_kib(n):
    return LINE * (n * KIB // len(LINE))


def _read(path):
    with open(path, "rb") as fh:
        return fh.read()


# @scenario SC-unsaved-text-10
def test_save_as_in_a_read_only_folder_says_where_and_what_is_kept(tmp_path):
    """The name part of Save As cannot be written: one box with the path,
    the reason and the name the window keeps; no second box."""
    work, env = _setup(tmp_path)
    with WpeSession(work, SEED, env_extra=env) as s:
        s.key("X")
        _lock_folder(work)
        try:
            s.menu(ALT.FILE, "a")
            s.key("out.c", "\033s")
            shown = _squash(s)
            assert "Cannotwrite" in shown, shown
            assert "out.c:Permissiondenied." in shown, shown
            assert "Thewindowkeepsitsnamet.c." in shown, shown
            assert "%s" not in shown and "Can'topen" not in shown, shown
            s.key(ESC)
            again = _squash(s)
            assert "Cannotwrite" not in again and "Can'topen" not in again, \
                "a second box:\n%s" % again
            assert s.alive()
        finally:
            _unlock_folder(work)
        assert not os.path.exists(os.path.join(work, "out.c"))


# @scenario SC-unsaved-text-10
def test_save_as_in_a_writable_folder_still_writes(tmp_path):
    """Control: the same keys in a writable folder write out.c, no box."""
    work, env = _setup(tmp_path)
    with WpeSession(work, SEED, env_extra=env) as s:
        s.key("X")
        s.menu(ALT.FILE, "a")
        s.key("out.c", "\033s")
        time.sleep(0.4)
        assert _read(os.path.join(work, "out.c")) == b"Xalpha\n"
        assert "Cannotwrite" not in _squash(s)


# @scenario SC-unsaved-text-11
def test_f2_in_a_read_only_folder_writes_and_says_no_backup_was_made(tmp_path):
    """The file is writable, its folder is not: the save works, no box, and
    the status row says the backup could not be made."""
    work, env = _setup(tmp_path)
    with WpeSession(work, SEED, env_extra=env) as s:
        s.key("X")
        _lock_folder(work)
        try:
            s.key(F2)
            shown = _squash(s)
            assert "t.cwritten;nobackupwasmade:Permissiondenied." in shown, shown
            assert "Can'topen" not in shown and "Cannotwrite" not in shown, shown
            assert _read(os.path.join(work, "t.c")) == b"Xalpha\n"
        finally:
            _unlock_folder(work)


# @scenario SC-unsaved-text-11
def test_f2_in_a_writable_folder_makes_the_backup_and_says_nothing(tmp_path):
    """Control: a folder that takes the .bak gets it, and there is no note."""
    work, env = _setup(tmp_path)
    with WpeSession(work, SEED, env_extra=env) as s:
        s.key("X")
        s.key(F2)
        assert _read(os.path.join(work, "t.bak")) == SEED.encode()
        assert _read(os.path.join(work, "t.c")) == b"Xalpha\n"
        assert "nobackup" not in _squash(s)
        assert oct(os.stat(os.path.join(work, "t.bak")).st_mode & 0o777) == \
            oct(os.stat(os.path.join(work, "t.c")).st_mode & 0o777)


# @scenario SC-unsaved-text-12
def test_f2_on_a_read_only_file_in_a_read_only_folder_is_one_box(tmp_path):
    """Nothing can be written: one box, the edit kept, the disk unchanged."""
    work, env = _setup(tmp_path)
    with WpeSession(work, SEED, env_extra=env) as s:
        path = os.path.join(work, "t.c")
        s.key("X")
        os.chmod(path, stat.S_IRUSR)
        _lock_folder(work)
        try:
            s.key(F2)
            shown = _squash(s)
            assert "Cannotwrite" in shown and "t.c:Permissiondenied." in shown, shown
            assert "Yourtextisstillinthewindow." in shown, shown
            s.key(ESC)
            again = _squash(s)
            assert "Cannotwrite" not in again and "Can'topen" not in again, \
                "a second box:\n%s" % again
            assert "Xalpha" in again, "the edit is gone:\n%s" % again
            assert _read(path) == SEED.encode()
        finally:
            _unlock_folder(work)
            os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)


# @scenario SC-unsaved-text-14
def test_a_write_that_fails_after_the_open_puts_the_file_back(tmp_path, monkeypatch):
    """The disk takes 2 KiB of a 4 KiB file: the write fails with File too
    large, the old content is put back, the editor lives and keeps the text."""
    work, env = _setup(tmp_path)
    env.update(_limited_editor(tmp_path, monkeypatch, 2))
    seed = _seed_kib(1)
    with WpeSession(work, seed, env_extra=env) as s:
        _grow(s)
        s.key(F2)
        assert s.alive(), "the editor ended with the text in it"
        shown = _squash(s)
        assert "Cannotwrite" in shown and "t.c:Filetoolarge." in shown, shown
        assert "Yourtextisstillinthewindow." in shown, shown
        assert _read(os.path.join(work, "t.c")) == seed.encode(), "t.c is not as it was"
        s.key(ESC)
        assert "bbbbbbbb" in _squash(s), "the text left the window"


# @scenario SC-unsaved-text-14
def test_a_file_that_fits_the_limit_is_saved(tmp_path, monkeypatch):
    """Control: the same limit, a small file: written."""
    work, env = _setup(tmp_path)
    env.update(_limited_editor(tmp_path, monkeypatch, 2))
    with WpeSession(work, "x" * 100 + "\n", env_extra=env) as s:
        s.key("X")
        s.key(F2)
        assert s.alive()
        assert _read(os.path.join(work, "t.c")).startswith(b"Xxxx")
        assert "Cannotwrite" not in _squash(s)


# @scenario SC-unsaved-text-14b
def test_a_restore_that_fails_says_the_file_may_be_cut_short(tmp_path, monkeypatch):
    """The old content cannot be put back: the box says so and names the
    backup, which holds the old bytes; the window keeps the text."""
    work, env = _setup(tmp_path)
    env.update(_limited_editor(tmp_path, monkeypatch, 2))
    env["XWPE_TEST_FAIL"] = "restore"
    seed = _seed_kib(1)
    with WpeSession(work, seed, env_extra=env) as s:
        _grow(s)
        s.key(F2)
        assert s.alive()
        shown = _squash(s)
        assert "maybecutshort" in shown, shown
        assert "t.bak" in shown, shown
        assert _read(os.path.join(work, "t.bak")) == seed.encode()
        s.key(ESC)
        assert "bbbbbbbb" in _squash(s), "the text left the window"


# @scenario SC-unsaved-text-14c
def test_a_symlinked_file_stays_a_link_and_its_target_is_put_back(tmp_path, monkeypatch):
    """The write went through the link and failed: the link is still a link
    and the target holds what it held."""
    work, env = _setup(tmp_path)
    env.update(_limited_editor(tmp_path, monkeypatch, 2))
    seed = _seed_kib(1)
    target = os.path.join(work, "target")
    os.mkdir(target)
    real = os.path.join(target, "real.c")
    with open(real, "w") as fh:
        fh.write(seed)
    os.symlink(real, os.path.join(work, "t.c"))
    with WpeSession(work, seed, env_extra=env) as s:
        _grow(s)
        s.key(F2)
        assert s.alive()
        assert "Cannotwrite" in _squash(s)
        assert os.path.islink(os.path.join(work, "t.c")), "the link was replaced"
        assert _read(real) == seed.encode(), "the target is not as it was"


# @scenario SC-unsaved-text-14d
def test_a_backup_that_stops_refuses_the_save(tmp_path, monkeypatch):
    """The disk takes 1 KiB and the file is 4 KiB: the backup copy stops, the
    save is refused and the file is not touched."""
    work, env = _setup(tmp_path)
    env.update(_limited_editor(tmp_path, monkeypatch, 1))
    seed = _seed_kib(4)
    with WpeSession(work, seed, env_extra=env) as s:
        s.key("X")
        s.key(F2)
        assert s.alive()
        shown = _squash(s)
        assert "thebackupcopystoppedatFiletoolarge;thefilewasnottouched." in shown, shown
        assert _read(os.path.join(work, "t.c")) == seed.encode()


def _print_stub(tmp_path):
    """A print command that notes where it ran; the user's own setting."""
    stub = str(tmp_path / "printstub")
    _write_executable(stub, "#!/bin/sh\npwd > \"%s/printed.dir\"\n" % tmp_path)
    cfg = tmp_path / "work" / ".config" / "xwpe"
    cfg.mkdir(parents=True)
    with open(str(cfg / "xwperc"), "w") as fh:
        fh.write("[General]\nPrintCmd : %s\n" % stub)


def _print_once(s, marker):
    if os.path.exists(marker):
        os.unlink(marker)
    s.menu(ALT.FILE, "p", delay=0.8)
    s.key("y", delay=0.6)
    deadline = time.monotonic() + RAN_S * WAIT_SCALE
    while time.monotonic() < deadline and not os.path.exists(marker):
        s._drain(POLL_S)
    return os.path.exists(marker)


# @scenario SC-unsaved-text-15
def test_print_with_an_unwritable_copy_prints_nothing(tmp_path):
    """The editor's temp folder refuses the copy: the user is told nothing
    was printed, and the print command never runs."""
    work, env = _setup(tmp_path)
    _print_stub(tmp_path)
    marker = str(tmp_path / "printed.dir")
    with WpeSession(work, SEED, env_extra=env) as s:
        assert _print_once(s, marker), "control: the print command did not run"
        tmpdir = _read(marker).decode().strip()
        os.chmod(tmpdir, stat.S_IRUSR | stat.S_IXUSR)
        try:
            ran = _print_once(s, marker)
            shown = _squash(s)
            assert not ran, "the print command ran on a copy that is not there"
            assert "Nothingwasprinted." in shown and "Cannotwrite" in shown, shown
        finally:
            os.chmod(tmpdir, stat.S_IRWXU)


# @scenario SC-unsaved-text-16
def test_a_block_written_to_a_read_only_folder_says_so(tmp_path):
    """Block > Write into a folder that refuses: the box names the file and
    the reason; the block, and the window, stay."""
    work, env = _setup(tmp_path)
    with WpeSession(work, SEED, env_extra=env) as s:
        s.menu(ALT.BLOCK, "o")                  # Mark WhOle
        _lock_folder(work)
        try:
            s.menu(ALT.BLOCK, "w")
            s.key("blk.c", "\033w")
            shown = _squash(s)
            assert "Cannotwrite" in shown and "blk.c:Permissiondenied." in shown, shown
            assert s.alive()
        finally:
            _unlock_folder(work)
        assert not os.path.exists(os.path.join(work, "blk.c"))


# @scenario SC-unsaved-text-16
def test_a_block_written_to_a_writable_folder_holds_the_block(tmp_path):
    """Control: the file holds the block."""
    work, env = _setup(tmp_path)
    with WpeSession(work, SEED, env_extra=env) as s:
        s.menu(ALT.BLOCK, "o")
        s.menu(ALT.BLOCK, "w")
        s.key("blk.c", "\033w")
        time.sleep(0.4)
        assert _read(os.path.join(work, "blk.c")) == SEED.rstrip("\n").encode()


# @scenario SC-unsaved-text-26
def test_a_program_the_editor_starts_does_not_inherit_ignored_sigxfsz(tmp_path):
    """The owner ignores SIGXFSZ only around its own writes: after a save,
    the compiler F9 starts has the default action for it."""
    work, env = _setup(tmp_path)
    bindir = tmp_path / "bin"
    bindir.mkdir()
    log = str(tmp_path / "sigign.log")
    for name in ("gcc", "cc"):
        _write_executable(str(bindir / name),
                          "#!/bin/sh\ngrep SigIgn /proc/self/status > \"%s\"\n" % log)
    env["PATH"] = "%s:%s" % (bindir, os.environ["PATH"])
    with WpeSession(work, "int main(void)\n{\n return 0;\n}\n", env_extra=env) as s:
        s.key("X")
        s.key(F9, delay=1.0)
        deadline = time.monotonic() + RAN_S * WAIT_SCALE
        while time.monotonic() < deadline and not os.path.exists(log):
            s._drain(POLL_S)
        assert os.path.exists(log), "the compiler stand-in did not run:\n%s" % \
            "\n".join(s.display())
        ignored = int(_read(log).split()[1], 16)
        assert ignored & (1 << 24) == 0, "SIGXFSZ is ignored in the child: %x" % ignored
