"""The File Manager's Copy, Move, Link and Remove of a folder (we_fileop.c).

A folder is taken whole: every entry, hidden ones included, with the folder's
permissions (setgid too).  Every entry is judged by what it is: a link is a
name, copied, moved, linked or removed as a name, and a link to a folder is
never entered, so no operation reaches a file outside the folder it was
started on (Remove used to delete the files a link pointed to).  An existing
destination folder is merged into under the Move/Copy option.  Remove of a
folder takes it whole after the Delete Directory dialog.  A folder operation
no longer waits for a key per file: the status row names the entry, and
Ctrl-C stops it and says how far it got.

Every row drives the real editor through the terminal driver, as
test_file_manager_copy.py does: F3 opens the File Manager, three Tabs go to
the DirTree, type ahead selects the folder, Alt-O copies, Alt-M moves, Alt-L
links, Alt-R removes.

Run: WPE_BIN=$PWD/wpe tests/.venv/bin/python -m pytest -q tests/test_file_manager_folder.py
(under a sanitizer build, launch it as wpe-asan).  FM_TEST_BOX_KEYS=n sends n
Returns after a folder operation, for running a row against a build that
still waits for a key per file.
"""
import os
import re
import stat
import time

import pytest

pytest.importorskip("pyte")

import procinfo                            # noqa: E402
from wpe_driver import WpeSession          # noqa: E402
from test_file_manager_copy import (        # noqa: E402
    ALT_L, ALT_M, ALT_O, ALT_R, CR, ESC, DEFAULT, DONT_OVERWRITE,
    OVERWRITE_ABSOLUTE, PROMPT_ASK_REMOVE, OPT_ALWAYS_SYMBOLIC,
    _make_other_fs_folder, _op_to, _open_files_panel,
    _open_tree_panel, _read, _seed, _squash, _typed, _wastebasket,
    skip_without_cpu_time)

ALT_D = "\033d"          # Delete without Prompt
ALT_OK = "\033o"
CTRL_C = "\x03"
BOX_KEYS = int(os.environ.get("FM_TEST_BOX_KEYS", "0"))
WAIT_S = 8.0
SETTLE_S = 0.6


def _wait_for(predicate, what, timeout=WAIT_S):
    """Poll the disk until `predicate()` holds: a folder operation has no
    box to wait for any more."""
    end = time.time() + timeout
    while time.time() < end:
        if predicate():
            return
        time.sleep(0.1)
    raise AssertionError("timed out waiting for " + what)


def _pass_boxes(s):
    """Only for a build that still waits for a key per file (see the header)."""
    for _ in range(BOX_KEYS):
        s.key(CR, delay=0.3)


def _write(path, text="x\n", mode=0o644):
    with open(path, "w") as fh:
        fh.write(text)
    os.chmod(path, mode)


def _snapshot(root):
    """Everything under `root`: names, kinds, modes, bytes, link texts."""
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
                out[rel] = ("file", stat.S_IMODE(st.st_mode), _read(path))
    return out


def _outside(wd):
    """The folder a link inside the folder under test points to."""
    out = os.path.join(wd, "precious")
    os.makedirs(os.path.join(out, "inner"))
    _write(os.path.join(out, "keep.txt"), "KEEP\n")
    _write(os.path.join(out, ".dot"), "DOT\n")
    _write(os.path.join(out, "inner", "deep.txt"), "DEEP\n")
    return out


def _folder_with_a_link(wd, name="rd"):
    """`rd` holds a.txt and `lnk`, a link to ../precious."""
    folder = os.path.join(wd, name)
    os.makedirs(folder)
    _write(os.path.join(folder, "a.txt"), "A\n")
    os.symlink("../precious", os.path.join(folder, "lnk"))
    return folder


def _folder_op(s, alt, name, dest=None):
    """The DirTree, the folder, the operation and (for Copy, Move, Link) where to."""
    _open_tree_panel(s)
    _typed(s, name)
    if dest is None:
        s.key(alt)
    else:
        _op_to(s, alt, dest)
    _pass_boxes(s)


# --- a folder is taken whole -----------------------------------------------------


# @scenario SC-unsaved-text-35
@pytest.mark.parametrize("hidden_and_odd", [True, False], ids=["hidden-and-setgid", "plain-control"])
def test_a_folder_copy_is_whole_and_keeps_its_modes(tmp_path, hidden_and_odd):
    """Copy folders 0777 and 2755 from the DirTree, with a hidden file and a
    file the panel's pattern hides: every entry is there with its mode, and
    no key was needed.  The earlier copy gave 0775 and 0755, left the hidden
    file behind, and its box stood waiting for a key."""
    wd = str(tmp_path)
    _seed(wd, DEFAULT, {"aaa.txt": "a\n"})
    for name, mode in (("sd", 0o777), ("sg", 0o2755)):
        folder = os.path.join(wd, name)
        os.makedirs(os.path.join(folder, "sub"))
        _write(os.path.join(folder, "inner.c"), "C\n")
        _write(os.path.join(folder, "note.txt"), "N\n", 0o600)
        _write(os.path.join(folder, "sub", "deep.txt"), "D\n")
        if hidden_and_odd:
            _write(os.path.join(folder, ".hidden"), "H\n", 0o600)
        os.chmod(os.path.join(folder, "sub"), 0o711)
        os.chmod(folder, mode)
    with WpeSession(wd, "int main;\n") as s:
        _folder_op(s, ALT_O, "sd", "sdcopy")
        _wait_for(lambda: os.path.exists(os.path.join(wd, "sdcopy", "sub", "deep.txt")), "the first copy")
        _typed(s, "sg")
        _op_to(s, ALT_O, "sgcopy")
        _wait_for(lambda: os.path.exists(os.path.join(wd, "sgcopy", "sub", "deep.txt")), "the second copy")
        time.sleep(SETTLE_S)
        assert s.alive()
    assert stat.S_IMODE(os.stat(os.path.join(wd, "sdcopy")).st_mode) == 0o777
    assert stat.S_IMODE(os.stat(os.path.join(wd, "sgcopy")).st_mode) == 0o2755
    assert _snapshot(os.path.join(wd, "sdcopy")) == _snapshot(os.path.join(wd, "sd"))
    assert _snapshot(os.path.join(wd, "sgcopy")) == _snapshot(os.path.join(wd, "sg"))
    if hidden_and_odd:
        assert _read(os.path.join(wd, "sgcopy", ".hidden")) == "H\n"


# @scenario SC-unsaved-text-50
def test_ctrl_c_stops_a_folder_copy_and_says_how_far_it_got(tmp_path):
    """Fifty files, and a Ctrl-C already waiting when the copy starts: the
    loop stops between entries, the box names the count, the files copied are
    there and no temporary copy is left.  The control copies all fifty."""
    wd = str(tmp_path)
    _seed(wd, DEFAULT, {"aaa.txt": "a\n"})
    os.makedirs(os.path.join(wd, "fifty"))
    for i in range(50):
        _write(os.path.join(wd, "fifty", "f%02d.txt" % i))
    with WpeSession(wd, "int main;\n") as s:
        _open_tree_panel(s)
        _typed(s, "fifty")
        s.key(ALT_O)
        _typed(s, "stopped")
        s.key(CR + CTRL_C, delay=1.5)
        text = _squash(s)
        found = re.search(r"Stopped:(\d+)of50entrieswerecopied", str(text))
        assert found, "no sentence:\n" + "\n".join(s.display())
        copied = os.listdir(os.path.join(wd, "stopped"))
        assert len(copied) == int(found.group(1)) < 50
        assert not [n for n in copied if "xwpe-copy" in n]
        s.key(ESC)
        _typed(s, "fifty")
        _op_to(s, ALT_O, "all")
        _wait_for(lambda: os.path.isdir(os.path.join(wd, "all")) and len(os.listdir(os.path.join(wd, "all"))) == 50,
                  "the copy of all fifty")


# --- a link is a name ---------------------------------------------------------------


def _expect_outside_untouched(wd, before):
    assert _snapshot(os.path.join(wd, "precious")) == before, "a file outside the folder changed"


# @scenario SC-unsaved-text-51
def test_remove_of_a_folder_never_enters_a_link_to_a_folder(tmp_path):
    """Remove = Absolute, no prompt, recursive: `rd` holds a.txt and a link to
    a folder outside it.  The folder goes and the folder the link pointed to
    is byte for byte as it was.  The earlier walk listed the link as a
    subfolder and deleted keep.txt, .dot and inner/deep.txt outside."""
    wd = str(tmp_path)
    _seed(wd, OVERWRITE_ABSOLUTE, {"aaa.txt": "a\n"})
    _outside(wd)
    before = _snapshot(os.path.join(wd, "precious"))
    _folder_with_a_link(wd)
    with WpeSession(wd, "int main;\n") as s:
        _folder_op(s, ALT_R, "rd")
        _wait_for(lambda: not os.path.exists(os.path.join(wd, "rd", "a.txt")), "the files to go")
        time.sleep(2 * SETTLE_S)
        assert s.alive()
    _expect_outside_untouched(wd, before)
    assert not os.path.exists(os.path.join(wd, "rd")), "the folder was not removed"


# @scenario SC-unsaved-text-51
def test_copy_of_a_folder_with_a_link_keeps_the_link_and_does_not_spin(tmp_path):
    """The link is copied as a link with its text, the folder it points to is
    not walked (the earlier copy followed it until the depth limit), and the
    editor does not spin."""
    skip_without_cpu_time()
    wd = str(tmp_path)
    _seed(wd, DEFAULT, {"aaa.txt": "a\n"})
    _outside(wd)
    before = _snapshot(os.path.join(wd, "precious"))
    _folder_with_a_link(wd)
    with WpeSession(wd, "int main;\n") as s:
        cpu = procinfo.cpu_seconds(s.proc.pid)
        _folder_op(s, ALT_O, "rd", "rdcopy")
        _wait_for(lambda: os.path.islink(os.path.join(wd, "rdcopy", "lnk")), "the link to be copied")
        time.sleep(SETTLE_S)
        spent = procinfo.cpu_seconds(s.proc.pid) - cpu
        assert spent < 1.5, "the copy spun: %.2f CPU s" % spent
        assert s.alive()
    assert os.readlink(os.path.join(wd, "rdcopy", "lnk")) == "../precious"
    assert _read(os.path.join(wd, "rdcopy", "a.txt")) == "A\n"
    assert not os.path.exists(os.path.join(wd, "rdcopy", "lnk", "..", "rdcopy", "keep.txt"))
    _expect_outside_untouched(wd, before)


# @scenario SC-unsaved-text-51
def test_move_of_a_folder_with_a_link_moves_the_link(tmp_path):
    wd = str(tmp_path)
    _seed(wd, DEFAULT, {"aaa.txt": "a\n"})
    _outside(wd)
    before = _snapshot(os.path.join(wd, "precious"))
    _folder_with_a_link(wd)
    with WpeSession(wd, "int main;\n") as s:
        _folder_op(s, ALT_M, "rd", "rdmoved")
        _wait_for(lambda: os.path.islink(os.path.join(wd, "rdmoved", "lnk")), "the folder to move")
        time.sleep(SETTLE_S)
    assert not os.path.exists(os.path.join(wd, "rd"))
    assert os.readlink(os.path.join(wd, "rdmoved", "lnk")) == "../precious"
    _expect_outside_untouched(wd, before)


# @scenario SC-unsaved-text-51
def test_move_across_file_systems_of_a_folder_with_a_link_moves_the_link(tmp_path):
    other = _make_other_fs_folder()
    if other is None or os.stat(other).st_dev == os.stat(str(tmp_path)).st_dev:
        pytest.skip("no second file system")
    wd = str(tmp_path)
    _seed(wd, DEFAULT, {"aaa.txt": "a\n"})
    _outside(wd)
    before = _snapshot(os.path.join(wd, "precious"))
    _folder_with_a_link(wd)
    dest = os.path.join(other, "rd")
    try:
        with WpeSession(wd, "int main;\n") as s:
            _folder_op(s, ALT_M, "rd", dest)
            _wait_for(lambda: os.path.islink(os.path.join(dest, "lnk")) and not os.path.exists(os.path.join(wd, "rd")),
                      "the folder to move across")
            time.sleep(SETTLE_S)
        assert os.readlink(os.path.join(dest, "lnk")) == "../precious"
        assert _read(os.path.join(dest, "a.txt")) == "A\n"
        _expect_outside_untouched(wd, before)
    finally:
        import shutil
        shutil.rmtree(other, ignore_errors=True)


# @scenario SC-unsaved-text-51
@pytest.mark.parametrize("symbolic", [True, False], ids=["symbolic", "hard"])
def test_link_of_a_folder_is_a_folder_of_links(tmp_path, symbolic):
    """Alt-L of a folder keeps 1.5.30a's result: a new folder holding a link
    to each entry, hidden ones included, hard links unless the option says
    Always Symbolic Link."""
    wd = str(tmp_path)
    _seed(wd, DEFAULT | (OPT_ALWAYS_SYMBOLIC if symbolic else 0), {"aaa.txt": "a\n"})
    folder = os.path.join(wd, "src")
    os.makedirs(os.path.join(folder, "sub"))
    _write(os.path.join(folder, "a.txt"), "A\n")
    _write(os.path.join(folder, ".dot"), "DOT\n")
    _write(os.path.join(folder, "sub", "b.txt"), "B\n")
    with WpeSession(wd, "int main;\n") as s:
        _folder_op(s, ALT_L, "src", "links")
        _wait_for(lambda: os.path.lexists(os.path.join(wd, "links", "sub", "b.txt")), "the links")
        time.sleep(SETTLE_S)
    for rel in ("a.txt", ".dot", os.path.join("sub", "b.txt")):
        made = os.path.join(wd, "links", rel)
        assert os.path.islink(made) == symbolic, rel
        assert _read(made) == _read(os.path.join(folder, rel))
        assert os.path.samefile(made, os.path.join(folder, rel))
    assert not os.path.islink(os.path.join(wd, "links"))
    assert not os.path.islink(os.path.join(wd, "links", "sub"))


# --- merging, the wastebasket, Remove whole -----------------------------------------


# @scenario SC-unsaved-text-52
def test_copy_into_an_existing_folder_merges_and_says_what_it_left(tmp_path):
    """Don't overwrite: the entries that are not there are copied, the ones
    that are there stay as they were, the status row counts them, and the
    folder that was already there keeps its permissions."""
    wd = str(tmp_path)
    _seed(wd, DONT_OVERWRITE, {"aaa.txt": "a\n"})
    os.makedirs(os.path.join(wd, "sg", "sub"))
    _write(os.path.join(wd, "sg", "a.c"), "NEW\n")
    _write(os.path.join(wd, "sg", "note.txt"), "N\n")
    _write(os.path.join(wd, "sg", "sub", "deep.txt"), "D\n")
    os.makedirs(os.path.join(wd, "ex"))
    _write(os.path.join(wd, "ex", "a.c"), "OLD\n")
    _write(os.path.join(wd, "ex", "mine.txt"), "MINE\n")
    os.chmod(os.path.join(wd, "ex"), 0o700)
    with WpeSession(wd, "int main;\n") as s:
        _folder_op(s, ALT_O, "sg", "ex")
        _wait_for(lambda: os.path.exists(os.path.join(wd, "ex", "sub", "deep.txt")), "the merge")
        time.sleep(SETTLE_S)
        screen = _squash(s)
        assert "2of3entrieswerecopied;1leftastheywere" in screen, "\n".join(s.display())
    assert _read(os.path.join(wd, "ex", "a.c")) == "OLD\n"
    assert _read(os.path.join(wd, "ex", "mine.txt")) == "MINE\n"
    assert _read(os.path.join(wd, "ex", "note.txt")) == "N\n"
    assert stat.S_IMODE(os.stat(os.path.join(wd, "ex")).st_mode) == 0o700


# @scenario SC-unsaved-text-52
def test_a_folder_into_the_wastebasket_is_whole_and_never_over_an_earlier_one(tmp_path):
    """A folder named `same` is already in the wastebasket: the second one
    goes there whole, hidden files included, as `same.1`.  The earlier
    removal merged it into the first."""
    wd = str(tmp_path)
    _seed(wd, DEFAULT, {"aaa.txt": "a\n"})
    os.makedirs(os.path.join(_wastebasket(wd), "same"))
    _write(os.path.join(_wastebasket(wd), "same", "EARLIER"), "EARLIER\n")
    os.makedirs(os.path.join(wd, "same", "sub"))
    _write(os.path.join(wd, "same", ".hidden"), "ONE\n")
    _write(os.path.join(wd, "same", "sub", "b.c"), "B\n")
    with WpeSession(wd, "int main;\n") as s:
        _folder_op(s, ALT_R, "same")
        _wait_for(lambda: os.path.isdir(os.path.join(_wastebasket(wd), "same.1")), "the folder in the wastebasket")
        time.sleep(SETTLE_S)
    assert _read(os.path.join(_wastebasket(wd), "same", "EARLIER")) == "EARLIER\n"
    assert _read(os.path.join(_wastebasket(wd), "same.1", ".hidden")) == "ONE\n"
    assert not os.path.exists(os.path.join(wd, "same"))


# @scenario SC-unsaved-text-53
def test_remove_of_a_folder_takes_it_whole_after_the_dialog(tmp_path):
    """Remove = Absolute (Prompt): the Delete Directory dialog asks once, and
    Delete without Prompt removes the folder with its hidden file and its
    subfolders, however the panel is filtered.  The earlier walk followed the
    pattern and the hidden option, left the unseen files and failed at the
    last rmdir."""
    wd = str(tmp_path)
    _seed(wd, PROMPT_ASK_REMOVE, {"aaa.txt": "a\n"})
    folder = os.path.join(wd, "rd")
    os.makedirs(os.path.join(folder, "sub", "deeper"))
    _write(os.path.join(folder, "a.txt"))
    _write(os.path.join(folder, ".hidden"))
    _write(os.path.join(folder, "sub", "b.c"))
    _write(os.path.join(folder, "sub", "deeper", ".dot"))
    with WpeSession(wd, "int main;\n") as s:
        _open_tree_panel(s)
        _typed(s, "rd")
        s.key(ALT_R, delay=0.8)
        assert "Delete Directory" in "\n".join(s.display()), "no dialog"
        s.key(ALT_D)
        s.key(ALT_OK)
        _wait_for(lambda: not os.path.exists(folder), "the folder to go")
        time.sleep(SETTLE_S)
        assert "Cannot" not in _squash(s)


# @scenario SC-unsaved-text-53
def test_cancel_at_the_delete_directory_dialog_removes_nothing(tmp_path):
    wd = str(tmp_path)
    _seed(wd, PROMPT_ASK_REMOVE, {"aaa.txt": "a\n"})
    folder = os.path.join(wd, "rd")
    os.makedirs(folder)
    _write(os.path.join(folder, "a.txt"))
    with WpeSession(wd, "int main;\n") as s:
        _open_tree_panel(s)
        _typed(s, "rd")
        s.key(ALT_R, delay=0.8)
        s.key(ESC)
        time.sleep(SETTLE_S)
    assert _read(os.path.join(folder, "a.txt")) == "x\n"


# --- a destination that is a link to a folder, a link onto its target ----------------


# @scenario SC-unsaved-text-54
def test_a_file_copied_onto_a_link_to_a_folder_goes_inside_the_folder(tmp_path):
    wd = str(tmp_path)
    _seed(wd, DEFAULT, {"src.txt": "SOURCE\n"})
    os.makedirs(os.path.join(wd, "box"))
    os.symlink("box", os.path.join(wd, "boxlink"))
    with WpeSession(wd, "int main;\n") as s:
        _open_files_panel(s)
        _typed(s, "src.txt")
        _op_to(s, ALT_O, "boxlink")
        _wait_for(lambda: os.path.exists(os.path.join(wd, "box", "src.txt")), "the file inside the folder")
        time.sleep(SETTLE_S)
    assert _read(os.path.join(wd, "box", "src.txt")) == "SOURCE\n"
    assert os.path.islink(os.path.join(wd, "boxlink")), "the link was replaced"


# @scenario SC-unsaved-text-54
def test_a_link_moved_onto_its_own_target_is_the_same_file(tmp_path):
    """Default options, Y at the question: the target keeps its data and is
    no link.  The earlier code moved the target to the wastebasket and left
    a link that pointed to itself."""
    wd = str(tmp_path)
    _seed(wd, DEFAULT, {"t.txt": "TARGET DATA\n"})
    os.symlink("t.txt", os.path.join(wd, "l.lnk"))
    with WpeSession(wd, "int main;\n") as s:
        _open_files_panel(s)
        _typed(s, "l.lnk")
        _op_to(s, ALT_M, "t.txt")
        time.sleep(SETTLE_S)
        screen = _squash(s)
        s.key("y")
        time.sleep(SETTLE_S)
        assert "arethesamefile" in screen or "arethesamefile" in _squash(s), "\n".join(s.display())
    assert not os.path.islink(os.path.join(wd, "t.txt"))
    assert _read(os.path.join(wd, "t.txt")) == "TARGET DATA\n"
    assert os.path.islink(os.path.join(wd, "l.lnk"))


# @scenario SC-unsaved-text-54
def test_a_name_with_a_slash_at_its_end_makes_no_file(tmp_path):
    wd = str(tmp_path)
    _seed(wd, DEFAULT, {"src.txt": "SOURCE\n"})
    with WpeSession(wd, "int main;\n") as s:
        _open_files_panel(s)
        _typed(s, "src.txt")
        _op_to(s, ALT_O, "nodir/")
        time.sleep(SETTLE_S)
        screen = _squash(s)
        assert "Nosuchfileordirectory" in screen, "\n".join(s.display())
    assert not os.path.exists(os.path.join(wd, "nodir"))


# --- the user's options survive a stopped folder operation ---------------------------


def _saved_options1(wd):
    found = []
    for base, _, files in os.walk(wd):
        if "xwperc" in files:
            with open(os.path.join(base, "xwperc")) as fh:
                for line in fh:
                    m = re.match(r"\s*Options1\s*:\s*(\d+)", line)
                    if m:
                        found.append((base, int(m.group(1))))
    return found


def _save_options(s):
    s.key(ALT_O, delay=1.0)
    s.key("s", delay=1.5)
    if "Save Option File" in "\n".join(s.display()):
        s.key(CR, delay=1.5)


# @scenario SC-unsaved-text-55
def test_esc_in_a_folder_copy_then_options_save_keeps_the_option_word(tmp_path):
    """Esc at the replace question of a folder copy, then Options > Save: the
    word written to xwperc is the one the user chose (the earlier walk had
    switched the Remove prompt off for the session, and Save wrote that)."""
    wd = str(tmp_path)
    _seed(wd, PROMPT_ASK_REMOVE, {"aaa.txt": "a\n"})
    for d in ("sd", "sdcopy"):
        os.makedirs(os.path.join(wd, d))
        _write(os.path.join(wd, d, "inner.txt"), d + "\n")
    with WpeSession(wd, "int main;\n") as s:
        _folder_op(s, ALT_O, "sd", "sdcopy")
        time.sleep(SETTLE_S)
        s.key(ESC)
        s.key(ESC)
        s.key(ESC)
        _save_options(s)
    found = _saved_options1(wd)
    assert found and all(v == PROMPT_ASK_REMOVE for _, v in found), found
    assert _read(os.path.join(wd, "sdcopy", "inner.txt")) == "sdcopy\n"


# @scenario SC-unsaved-text-55
def test_esc_at_the_delete_directory_dialog_then_options_save_keeps_the_option_word(tmp_path):
    wd = str(tmp_path)
    _seed(wd, PROMPT_ASK_REMOVE, {"aaa.txt": "a\n"})
    os.makedirs(os.path.join(wd, "rd"))
    _write(os.path.join(wd, "rd", "x.txt"))
    with WpeSession(wd, "int main;\n") as s:
        _open_tree_panel(s)
        _typed(s, "rd")
        s.key(ALT_R, delay=0.8)
        s.key(ESC)
        s.key(ESC)
        s.key(ESC)
        _save_options(s)
    found = _saved_options1(wd)
    assert os.path.exists(os.path.join(wd, "rd", "x.txt"))
    assert found and all(v == PROMPT_ASK_REMOVE for _, v in found), found


# --- a link inside the destination is a name ----------------------------------------


# @scenario SC-unsaved-text-68
def test_a_merge_never_writes_through_a_link_inside_the_destination(tmp_path):
    """Overwrite and Remove = Absolute: `rd` copied into `dst`, which holds
    `lnk`, a link to the folder outside, and `a.txt`, a link to it too.  The
    folder outside is byte for byte as it was and the editor is alive (cp
    refuses the first entry, and replaces the link of the second as a name)."""
    wd = str(tmp_path)
    _seed(wd, OVERWRITE_ABSOLUTE, {"aaa.txt": "a\n"})
    out = _outside(wd)
    _write(os.path.join(out, "a.txt"), "OUTSIDE\n")
    _write(os.path.join(out, "keep.txt"), "OUTSIDE\n")
    before = _snapshot(out)
    rd = os.path.join(wd, "rd")
    os.makedirs(os.path.join(rd, "lnk"))
    _write(os.path.join(rd, "lnk", "keep.txt"), "NEW\n")
    _write(os.path.join(rd, "a.txt"), "NEW\n")
    os.makedirs(os.path.join(wd, "dst"))
    os.symlink("../precious", os.path.join(wd, "dst", "lnk"))
    os.symlink("../precious", os.path.join(wd, "dst", "a.txt"))
    with WpeSession(wd, "int main;\n") as s:
        _folder_op(s, ALT_O, "rd", "dst")
        time.sleep(4 * SETTLE_S)
        assert s.alive()
    _expect_outside_untouched(wd, before)


# @scenario SC-unsaved-text-69
def test_a_link_where_a_folder_goes_is_refused_and_said(tmp_path):
    """Copy of `rd` into `dst` that holds `sub`, a link to the folder outside,
    where `rd` has a folder `sub`: a box says `dst/sub is not a folder`, the
    folder outside is as it was, and Esc returns to the panel."""
    wd = str(tmp_path)
    _seed(wd, OVERWRITE_ABSOLUTE, {"aaa.txt": "a\n"})
    out = _outside(wd)
    before = _snapshot(out)
    os.makedirs(os.path.join(wd, "rd", "sub"))
    _write(os.path.join(wd, "rd", "sub", "deep.txt"), "NEW\n")
    os.makedirs(os.path.join(wd, "dst"))
    os.symlink("../precious", os.path.join(wd, "dst", "sub"))
    with WpeSession(wd, "int main;\n") as s:
        _folder_op(s, ALT_O, "rd", "dst")
        time.sleep(2 * SETTLE_S)
        screen = _squash(s)
        assert "dst/subisnotafolder" in screen, "\n".join(s.display())
        s.key(ESC, delay=0.8)
        assert s.alive()
        assert "dst/subisnotafolder" not in _squash(s), "\n".join(s.display())
    _expect_outside_untouched(wd, before)
    assert os.path.islink(os.path.join(wd, "dst", "sub"))


# @scenario SC-unsaved-text-69
def test_a_real_folder_where_a_folder_goes_is_merged_without_a_box(tmp_path):
    """The control of the row above: a real `dst/sub` shows no box and merges."""
    wd = str(tmp_path)
    _seed(wd, OVERWRITE_ABSOLUTE, {"aaa.txt": "a\n"})
    os.makedirs(os.path.join(wd, "rd", "sub"))
    _write(os.path.join(wd, "rd", "sub", "deep.txt"), "NEW\n")
    os.makedirs(os.path.join(wd, "dst", "sub"))
    with WpeSession(wd, "int main;\n") as s:
        _folder_op(s, ALT_O, "rd", "dst")
        _wait_for(lambda: os.path.exists(os.path.join(wd, "dst", "sub", "deep.txt")), "the merge")
        time.sleep(SETTLE_S)
        assert "isnotafolder" not in _squash(s), "\n".join(s.display())
