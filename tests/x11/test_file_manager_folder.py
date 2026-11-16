"""A folder copied in the File Manager, in the xwpe/X11 window (twin of the
terminal rows of tests/test_file_manager_folder.py).

The folder is taken whole, with its permissions, and no key is needed: the
editor's standard streams are a pty nobody types on, as when xwpe is started
from a terminal, so a copy that still waited for a key per file (the earlier
progress box read the launching terminal's input, not the window) never
finishes.  Folders 0777 and 2755 hold a hidden file, a file any pattern would
hide, and a subfolder; the copies must have the modes and every entry.
"""
import os
import stat
import time

import pytest

from mode_premise import special_bits_settable, workdir_in_my_group

PTY = {"XWPE_TEST_PTY": True}
WAIT_S = 20.0


def _write(path, text, mode=0o644):
    with open(path, "w") as fh:
        fh.write(text)
    os.chmod(path, mode)


def _make_folders(wd):
    for name, mode in (("sd", 0o777), ("sg", 0o2755)):
        folder = os.path.join(wd, name)
        os.makedirs(os.path.join(folder, "sub"))
        _write(os.path.join(folder, "inner.c"), "C\n")
        _write(os.path.join(folder, ".hidden"), "H\n", 0o600)
        _write(os.path.join(folder, "note.txt"), "N\n", 0o600)
        _write(os.path.join(folder, "sub", "deep.txt"), "D\n")
        os.chmod(os.path.join(folder, "sub"), 0o711)
        os.chmod(folder, mode)


def _wait_for(predicate, what):
    end = time.time() + WAIT_S
    while time.time() < end:
        if predicate():
            return
        time.sleep(0.2)
    raise AssertionError("timed out waiting for " + what)


def _copy_folder(xwpe, wd, name, dest):
    """The DirTree: three Tabs from the File Manager's first field, the
    folder by type-ahead, Alt-O, the destination typed."""
    xwpe.key("F3", delay=1.2)
    for _ in range(3):
        xwpe.key("Tab", delay=0.3)
    xwpe.type(name, delay=0.6)
    xwpe.key("alt+o", delay=0.8)
    xwpe.type(dest, delay=0.6)
    xwpe.key("Return", delay=0.5)
    _wait_for(lambda: os.path.exists(os.path.join(wd, dest, "sub", "deep.txt")), "the copy of " + name)


# @scenario SC-unsaved-text-35x
@pytest.mark.parametrize("xwpe", [PTY], indirect=True)
def test_x11_a_folder_copy_is_whole_keeps_its_modes_and_needs_no_key(xwpe):
    wd = os.path.dirname(xwpe.srcfile)
    workdir_in_my_group(wd)
    special_bits_settable(wd, stat.S_ISGID)
    _make_folders(wd)
    _copy_folder(xwpe, wd, "sd", "sdcopy")
    time.sleep(0.5)
    xwpe.key("Escape", delay=0.8)
    _copy_folder(xwpe, wd, "sg", "sgcopy")
    assert xwpe.proc.poll() is None, "xwpe died copying a folder"
    assert stat.S_IMODE(os.stat(os.path.join(wd, "sdcopy")).st_mode) == 0o777
    assert stat.S_IMODE(os.stat(os.path.join(wd, "sgcopy")).st_mode) == 0o2755
    for rel in ("inner.c", ".hidden", "note.txt", os.path.join("sub", "deep.txt")):
        for src, copy in (("sd", "sdcopy"), ("sg", "sgcopy")):
            with open(os.path.join(wd, src, rel)) as a, open(os.path.join(wd, copy, rel)) as b:
                assert a.read() == b.read(), os.path.join(copy, rel)
    assert stat.S_IMODE(os.stat(os.path.join(wd, "sgcopy", ".hidden")).st_mode) == 0o600
    assert stat.S_IMODE(os.stat(os.path.join(wd, "sgcopy", "sub")).st_mode) == 0o711


# Overwrite, Remove = Absolute, recursive (the bits of options.h, as the
# terminal rows seed them).
OVERWRITE_ABSOLUTE = 0x1000 | 0x10 | 0x08


def _snapshot(root):
    """Names, kinds, modes, bytes and link texts under `root`."""
    out = {}
    for base, dirs, files in os.walk(root):
        for name in dirs + files:
            path = os.path.join(base, name)
            st = os.lstat(path)
            rel = os.path.relpath(path, root)
            if stat.S_ISLNK(st.st_mode):
                out[rel] = ("link", os.readlink(path))
            elif stat.S_ISDIR(st.st_mode):
                out[rel] = ("dir", stat.S_IMODE(st.st_mode))
            else:
                with open(path, "rb") as fh:
                    out[rel] = ("file", stat.S_IMODE(st.st_mode), fh.read())
    return out


def _options(tmp_path):
    """Before the editor starts: the options word."""
    cfg = tmp_path / ".config" / "xwpe"
    cfg.mkdir(parents=True)
    (cfg / "xwperc").write_text("[General]\nOptions1 : %d\nKeysNotice : 1\n" % OVERWRITE_ABSOLUTE)


@pytest.fixture
def links_outside(tmp_path):
    """The options, a folder `precious` outside, and `rd` holding a file and
    links to it (a folder, a file, nothing, the folder itself, a loop)."""
    _options(tmp_path)
    out = tmp_path / "precious"
    (out / "inner").mkdir(parents=True)
    _write(str(out / "keep.txt"), "KEEP\n")
    _write(str(out / ".dot"), "DOT\n")
    _write(str(out / "inner" / "deep.txt"), "DEEP\n")
    rd = tmp_path / "rd"
    (rd / "sub").mkdir(parents=True)
    _write(str(rd / "a.txt"), "A\n")
    _write(str(rd / "sub" / "deep.txt"), "D\n")
    for name, target in (("lnk", "../precious"), ("lfile", "../precious/keep.txt"),
                         ("dangling", "nowhere"), ("self", "."), ("loop1", "loop2"),
                         ("loop2", "loop1"), (os.path.join("sub", "back"), "../../precious/inner")):
        os.symlink(target, str(rd / name))
    return str(out)


@pytest.fixture
def links_in_destination(tmp_path):
    """The options, `precious` with `a.txt` and `keep.txt` in it, `rd` with a
    folder `lnk` and a file `a.txt`, and `dst` holding a link of each name to
    `precious`."""
    _options(tmp_path)
    out = tmp_path / "precious"
    out.mkdir()
    _write(str(out / "a.txt"), "OUTSIDE\n")
    _write(str(out / "keep.txt"), "OUTSIDE\n")
    rd = tmp_path / "rd"
    (rd / "lnk").mkdir(parents=True)
    _write(str(rd / "lnk" / "keep.txt"), "NEW\n")
    _write(str(rd / "a.txt"), "NEW\n")
    dst = tmp_path / "dst"
    dst.mkdir()
    os.symlink("../precious", str(dst / "lnk"))
    os.symlink("../precious", str(dst / "a.txt"))
    return str(out)


def _tree_op(xwpe, alt, name, dest=None):
    """F3, the DirTree, the folder by type-ahead, the operation, the destination."""
    xwpe.key("F3", delay=1.2)
    for _ in range(3):
        xwpe.key("Tab", delay=0.3)
    xwpe.type(name, delay=0.6)
    xwpe.key(alt, delay=0.8)
    if dest is not None:
        xwpe.type(dest, delay=0.6)
        xwpe.key("Return", delay=0.5)


# @scenario SC-unsaved-text-68x
@pytest.mark.parametrize("xwpe", [PTY], indirect=True)
def test_x11_a_merge_never_writes_through_a_link_inside_the_destination(links_in_destination, xwpe):
    """Overwrite and Remove = Absolute: `rd` copied into `dst`, whose `lnk`
    and `a.txt` are links to the folder outside.  The folder outside is byte
    for byte as it was and the window is alive."""
    before = _snapshot(links_in_destination)
    _tree_op(xwpe, "alt+o", "rd", "dst")
    time.sleep(3.0)
    assert xwpe.proc.poll() is None, "xwpe died on a merge into links"
    assert _snapshot(links_in_destination) == before, "the merge changed the folder outside"


# @scenario SC-unsaved-text-70
@pytest.mark.parametrize("xwpe", [PTY], indirect=True)
def test_x11_no_folder_operation_touches_what_a_link_inside_points_to(links_outside, xwpe):
    """Copy, Link, Move and Remove of a folder holding links to outside data,
    a dangling link, a link to the folder itself and a link loop, in the X11
    window: the folder outside is byte for byte as it was after each."""
    wd = os.path.dirname(xwpe.srcfile)
    before = _snapshot(links_outside)
    steps = (("alt+o", "rd", "cp", lambda: os.path.exists(os.path.join(wd, "cp", "sub", "deep.txt"))),
             ("alt+l", "rd", "lk", lambda: os.path.lexists(os.path.join(wd, "lk", "sub", "back"))),
             ("alt+m", "cp", "mv", lambda: not os.path.exists(os.path.join(wd, "cp"))),
             ("alt+r", "rd", None, lambda: not os.path.lexists(os.path.join(wd, "rd"))))
    for alt, name, dest, done in steps:
        _tree_op(xwpe, alt, name, dest)
        _wait_for(done, "%s of %s" % (alt, name))
        time.sleep(0.5)
        assert xwpe.proc.poll() is None, "xwpe died at %s of %s" % (alt, name)
        assert _snapshot(links_outside) == before, "%s of %s changed the folder outside" % (alt, name)
        xwpe.key("Escape", delay=0.8)
    assert os.path.islink(os.path.join(wd, "mv", "lnk")), "the moved link is not a link"
    assert os.readlink(os.path.join(wd, "mv", "lnk")) == "../precious"
