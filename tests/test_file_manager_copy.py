"""The File Manager's Copy, Move, Link and Remove of a file (we_fileop.c).

A copy, move, link or removal of a user's file either completes whole or
leaves the source and any existing destination as they were, and the user is
told once, with the paths, the step and the reason.  The destination is a new
file in its own folder, put in place by one rename: a link in its place is
replaced and never written through, a copy that stops leaves no half file,
"Don't overwrite" leaves the file alone and says so, a Move across file
systems removes its source only after the copy is whole.

Every row drives the real editor through the terminal driver: F3 opens the
File Manager, Tab goes to the Files panel (three Tabs to the DirTree), type
ahead selects the name, Alt-O copies, Alt-M moves, Alt-L links, Alt-R removes,
and the destination is typed in the field.  The File Manager's options are
written to the work folder's xwperc, which is also the HOME of the session,
so the wastebasket is its .wastebasket.

Manual lines (not asserted here): a real full disk; a file system whose
close succeeds and loses the data later.

Run: WPE_BIN=$PWD/wpe tests/.venv/bin/python -m pytest -q tests/test_file_manager_copy.py
(under a sanitizer build, launch it as wpe-asan)
"""
import os
import re
import shutil
import stat
import tempfile
import threading
import time

import pytest

pytest.importorskip("pyte")

import gui_owner                           # noqa: E402
import procinfo                            # noqa: E402
import wpe_driver                          # noqa: E402
from mode_premise import special_bits_settable, workdir_in_my_group   # noqa: E402
from wpe_driver import WpeSession          # noqa: E402

F3 = "\033OR"
TAB = "\t"
CR = "\r"
ESC = "\033"
ALT_O = "\033o"        # Copy
ALT_M = "\033m"        # Move
ALT_L = "\033l"        # Link
ALT_R = "\033r"        # Remove
ALT_A = "\033a"        # Attributes

# The File Manager's option bits (options.h).
OPT_REKURSIVE = 0x1000
OPT_INTO_WB = 0x40
OPT_PROMPT_DELETE = 0x08
OPT_MOVE_PROMPT = 0x20
OPT_MOVE_OVERWRITE = 0x10
OPT_REMOVE_PROMPT = 0x80
OPT_ALWAYS_SYMBOLIC = 0x100

DEFAULT = OPT_REKURSIVE | OPT_INTO_WB | OPT_MOVE_PROMPT | OPT_PROMPT_DELETE
DONT_OVERWRITE = DEFAULT & ~OPT_MOVE_PROMPT
OVERWRITE_WB = OPT_REKURSIVE | OPT_INTO_WB | OPT_MOVE_OVERWRITE | OPT_PROMPT_DELETE
OVERWRITE_ABSOLUTE = OPT_REKURSIVE | OPT_MOVE_OVERWRITE | OPT_PROMPT_DELETE
PROMPT_ASK_REMOVE = OPT_REKURSIVE | OPT_MOVE_PROMPT | OPT_REMOVE_PROMPT | OPT_PROMPT_DELETE

SIZE_LIMIT_BLOCKS = 32
BIG = "x" * 200000 + "\n"
MODE_WINDOW_BYTES = 64 * 1024 * 1024
PIPE_WAIT_S = 3.0


class Squashed(str):
    """The screen's text without its spaces; a sentence the box wrapped has
    the box's side lines between its words, so `needle in squashed` allows a
    run of them between any two letters."""

    def __contains__(self, needle):
        pattern = "[xab]*".join(re.escape(c) for c in needle if not c.isspace())
        return re.search(pattern, str(self)) is not None


def _squash(s):
    text = "".join(s.display())
    return Squashed(re.sub(r"[^\x21-\x7e]", "", text))


def _seed(workdir, options, files=None):
    """The work folder: options in xwperc, and the named files (a tuple is a link)."""
    cfg = os.path.join(workdir, ".config", "xwpe")
    os.makedirs(cfg, exist_ok=True)
    with open(os.path.join(cfg, "xwperc"), "w") as fh:
        fh.write("[General]\nOptions1 : %d\nKeysNotice : 1\n" % options)
    for name, val in (files or {}).items():
        path = os.path.join(workdir, name)
        if isinstance(val, tuple):
            os.symlink(val[0], path)
        else:
            with open(path, "w") as fh:
                fh.write(val)


def _read(path):
    with open(path) as fh:
        return fh.read()


def _typed(s, text):
    for ch in text:
        s.key(ch, delay=0.05)


def _open_files_panel(s):
    s.key(F3)
    s.key(TAB)


def _open_tree_panel(s):
    s.key(F3)
    for _ in range(3):
        s.key(TAB)


def _op_to(s, alt, dest):
    s.key(alt)
    _typed(s, dest)
    s.key(CR)


def _copy(s, name, dest, alt=ALT_O):
    _open_files_panel(s)
    _typed(s, name)
    _op_to(s, alt, dest)


def _folder_entries(path):
    return sorted(os.listdir(path))


def _wastebasket(wd):
    return os.path.join(wd, ".wastebasket")


def _make_other_fs_folder():
    """A folder on another file system than tmp, or None."""
    if not os.access("/dev/shm", os.W_OK):
        return None
    return tempfile.mkdtemp(dir="/dev/shm", prefix="fmcopy.")


# --- Don't overwrite, the question and the answers ---------------------------


# @scenario SC-unsaved-text-30
def test_dont_overwrite_leaves_the_destination_and_says_so(tmp_path):
    wd = str(tmp_path)
    _seed(wd, DONT_OVERWRITE, {"src.txt": "SOURCE\n", "dst.txt": "ORIGINAL, DO NOT LOSE ME\n"})
    with WpeSession(wd, "int main;\n") as s:
        _copy(s, "src.txt", "dst.txt")
        screen = _squash(s)
        assert s.alive()
        assert _read(os.path.join(wd, "dst.txt")) == "ORIGINAL, DO NOT LOSE ME\n"
        assert "exists;notoverwritten" in screen, "the status row does not say so"
        assert "Cannot" not in screen, "a skip is not an error box"


# @scenario SC-unsaved-text-30
def test_prompt_asks_once_and_yes_replaces_no_leaves(tmp_path):
    wd = str(tmp_path)
    _seed(wd, DEFAULT, {"src.txt": "SOURCE\n", "dst.txt": "ORIGINAL\n", "keep.txt": "KEPT\n"})
    with WpeSession(wd, "int main;\n") as s:
        _copy(s, "src.txt", "dst.txt")
        assert "Replace" in "\n".join(s.display()), "no question"
        s.key("n")
        time.sleep(0.5)
        assert _read(os.path.join(wd, "dst.txt")) == "ORIGINAL\n"
        _typed(s, "src.txt")        # the panel went back to its first entry
        _op_to(s, ALT_O, "dst.txt")
        s.key("y")
        time.sleep(0.8)
        assert _read(os.path.join(wd, "dst.txt")) == "SOURCE\n"
        assert os.listdir(_wastebasket(wd)) == ["dst.txt"], "the replaced file is in the wastebasket"
        assert _read(os.path.join(_wastebasket(wd), "dst.txt")) == "ORIGINAL\n"


# @scenario SC-unsaved-text-30
def test_overwrite_replaces_and_keeps_the_old_file_in_the_wastebasket(tmp_path):
    wd = str(tmp_path)
    _seed(wd, OVERWRITE_WB, {"src.txt": "SOURCE\n", "dst.txt": "ORIGINAL\n"})
    with WpeSession(wd, "int main;\n") as s:
        _copy(s, "src.txt", "dst.txt")
        time.sleep(0.5)
        assert _read(os.path.join(wd, "dst.txt")) == "SOURCE\n"
        assert _read(os.path.join(_wastebasket(wd), "dst.txt")) == "ORIGINAL\n"
        assert [n for n in os.listdir(wd) if "xwpe-copy" in n] == []


# @scenario SC-unsaved-text-30b
def test_stopping_a_folder_copy_leaves_the_users_options(tmp_path):
    """An Esc at the replace question of a folder copy used to return with the
    Remove question switched off for the rest of the session.

    Two Escapes: one answers the replace question, one closes the box that
    says how far the copy got.  The third Escape of the earlier row ended the
    per-file wait of the progress box, which was a 2026 debug leftover
    (1.5.30a polled for a key and went on), and is gone."""
    wd = str(tmp_path)
    _seed(wd, PROMPT_ASK_REMOVE, {"aaa.txt": "a\n", "zz.txt": "KEEP ME\n"})
    for d in ("sd", "sdcopy"):
        os.makedirs(os.path.join(wd, d))
        with open(os.path.join(wd, d, "inner.txt"), "w") as fh:
            fh.write(d + "\n")
    with WpeSession(wd, "int main;\n") as s:
        _open_tree_panel(s)
        _typed(s, "sd")
        _op_to(s, ALT_O, "sdcopy")
        s.key(ESC)
        s.key(ESC)
        _open_files_panel(s)
        _typed(s, "zz.txt")
        s.key(ALT_R)
        time.sleep(0.5)
        assert os.path.exists(os.path.join(wd, "zz.txt")), "removed without the Remove question"
        assert "Remove" in "\n".join(s.display())


# --- links at the destination --------------------------------------------------


# @scenario SC-unsaved-text-31a
def test_a_dangling_link_at_the_destination_is_replaced_not_written_through(tmp_path):
    wd = str(tmp_path / "w")
    out = str(tmp_path / "outside")
    os.makedirs(wd)
    os.makedirs(out)
    _seed(wd, DEFAULT, {"src.txt": "SOURCE\n"})
    os.symlink(os.path.join(out, "planted.txt"), os.path.join(wd, "dst.lnk"))
    with WpeSession(wd, "int main;\n") as s:
        _copy(s, "src.txt", "dst.lnk")
        s.key("y")
        time.sleep(0.8)
        assert not os.path.exists(os.path.join(out, "planted.txt")), "written through the link"
        assert not os.path.islink(os.path.join(wd, "dst.lnk"))
        assert _read(os.path.join(wd, "dst.lnk")) == "SOURCE\n"


# @scenario SC-unsaved-text-31
def test_dont_overwrite_leaves_a_live_link_and_its_target(tmp_path):
    wd = str(tmp_path)
    _seed(wd, DONT_OVERWRITE, {"src.txt": "SOURCE\n", "victim.txt": "OUTSIDE THE COPY\n"})
    os.symlink("victim.txt", os.path.join(wd, "dst.lnk"))
    with WpeSession(wd, "int main;\n") as s:
        _copy(s, "src.txt", "dst.lnk")
        assert s.alive()
        assert _read(os.path.join(wd, "victim.txt")) == "OUTSIDE THE COPY\n"
        assert os.path.islink(os.path.join(wd, "dst.lnk"))


# @scenario SC-unsaved-text-31
def test_overwrite_replaces_a_live_link_as_a_name(tmp_path):
    wd = str(tmp_path)
    _seed(wd, OVERWRITE_WB, {"src.txt": "SOURCE\n", "victim.txt": "VICTIM\n"})
    os.symlink("victim.txt", os.path.join(wd, "dst.lnk"))
    with WpeSession(wd, "int main;\n") as s:
        _copy(s, "src.txt", "dst.lnk")
        time.sleep(0.5)
        assert _read(os.path.join(wd, "victim.txt")) == "VICTIM\n"
        assert not os.path.islink(os.path.join(wd, "dst.lnk"))
        assert _read(os.path.join(wd, "dst.lnk")) == "SOURCE\n"
        assert os.path.islink(os.path.join(_wastebasket(wd), "dst.lnk")), "the old link is in the wastebasket"


# @scenario SC-unsaved-text-30u
def test_a_file_copied_onto_a_folder_goes_inside_it(tmp_path):
    wd = str(tmp_path)
    _seed(wd, DEFAULT, {"src.txt": "SOURCE\n"})
    os.makedirs(os.path.join(wd, "box"))
    with open(os.path.join(wd, "box", "kept.txt"), "w") as fh:
        fh.write("in the folder\n")
    with WpeSession(wd, "int main;\n") as s:
        _copy(s, "src.txt", "box")
        time.sleep(0.8)
        assert _read(os.path.join(wd, "box", "src.txt")) == "SOURCE\n"
        assert _read(os.path.join(wd, "box", "kept.txt")) == "in the folder\n"
        assert not os.path.exists(_wastebasket(wd)) or not os.listdir(_wastebasket(wd))


# --- a destination that is not a file ------------------------------------------


# @scenario SC-unsaved-text-32
def test_a_device_as_the_destination_is_refused_and_nothing_moves(tmp_path):
    if not os.path.exists("/dev/full"):
        pytest.skip("no /dev/full")
    wd = str(tmp_path)
    _seed(wd, OVERWRITE_ABSOLUTE, {"src.txt": "SOURCE\n"})
    with WpeSession(wd, "int main;\n") as s:
        _copy(s, "src.txt", "/dev/full")
        screen = _squash(s)
        assert s.alive()
        assert "/dev/fullisnotafile" in screen, "\n".join(s.display())
        assert "Nothingwaschanged" in screen
    assert os.path.exists("/dev/full")


# @scenario SC-unsaved-text-46
def test_a_pipe_as_the_source_is_refused_at_once(tmp_path):
    wd = str(tmp_path)
    _seed(wd, DEFAULT, {})
    os.mkfifo(os.path.join(wd, "fifo"))
    # the Files panel lists regular files and links, so the pipe is reached by a link
    os.symlink("fifo", os.path.join(wd, "fifolink"))
    with WpeSession(wd, "int main;\n") as s:
        _open_files_panel(s)
        _typed(s, "fifolink")
        _op_to(s, ALT_O, "out.bin")
        deadline = time.time() + PIPE_WAIT_S
        screen = _squash(s)
        while "isnotafile" not in screen and time.time() < deadline:
            time.sleep(0.3)
            screen = _squash(s)
        assert "isnotafile" in screen, "\n".join(s.display())
        assert s.alive()
        assert not os.path.exists(os.path.join(wd, "out.bin"))


# --- a copy that fails -----------------------------------------------------------


def skip_without_cpu_time():
    """Skip the row, naming why, when this system cannot say how much CPU a
    process has used."""
    if procinfo.cpu_seconds(os.getpid()) is None:
        pytest.skip("needs a CPU time per process: /proc or ps")


# @scenario SC-unsaved-text-33
def test_a_source_read_error_ends_the_copy_with_a_box(tmp_path):
    skip_without_cpu_time()
    unreadable = procinfo.self_memory_file()
    if unreadable is None:
        pytest.skip("needs a file whose read fails: the process's own memory in /proc")
    wd = str(tmp_path)
    _seed(wd, DONT_OVERWRITE, {"src.txt": "SOURCE\n"})
    os.symlink(unreadable, os.path.join(wd, "badlink"))
    with WpeSession(wd, "int main;\n") as s:
        _open_files_panel(s)
        _typed(s, "badlink")
        s.key(ALT_O)
        before = procinfo.cpu_seconds(s.proc.pid)
        _typed(s, "out.bin")
        s.key(CR)
        time.sleep(2.0)
        after = procinfo.cpu_seconds(s.proc.pid)
        screen = _squash(s)
        assert after - before < 1.0, "the copy loop spun: %.2f CPU s" % (after - before)
        assert "readingthesource:Input/outputerror" in screen, "\n".join(s.display())
        assert "Nothingwaschanged" in screen
        assert not os.path.exists(os.path.join(wd, "out.bin"))
        s.key(ESC)
        assert "File-Manager" in "\n".join(s.display())


# @scenario SC-unsaved-text-34
def test_a_file_size_limit_is_reported_and_leaves_no_partial_copy(tmp_path, monkeypatch):
    real = os.path.abspath(wpe_driver.WPE_BIN)
    name = os.path.basename(real)
    wrapper = tmp_path / name
    wrapper.write_text("#!/bin/bash\nulimit -f %d\nexec -a %s %s \"$@\"\n" % (SIZE_LIMIT_BLOCKS, name, real))
    wrapper.chmod(0o755)
    monkeypatch.setattr(wpe_driver, "WPE_BIN", str(wrapper))
    wd = str(tmp_path / "work")
    os.makedirs(wd)
    _seed(wd, DONT_OVERWRITE, {"src.txt": BIG})
    # No trace file for an editor under a size limit: a CI run appends every
    # row of a shard to one XWPE_UI_TRACE, which is already past this limit
    # when the row starts, so the editor's first trace line would kill it
    # with SIGXFSZ before it painted anything.
    env = {"XWPE_LIB": os.path.dirname(real)}
    env.update({var: None for var in gui_owner.TRACE_PASS_THROUGH})
    with WpeSession(wd, "int main;\n", env_extra=env) as s:
        _copy(s, "src.txt", "out.bin")
        time.sleep(1.0)
        screen = _squash(s)
        assert s.alive(), "killed, exit %s" % s.exit_code()
        assert "writing:Filetoolarge" in screen, "\n".join(s.display())
        assert not os.path.exists(os.path.join(wd, "out.bin"))
        assert [n for n in os.listdir(wd) if "xwpe-copy" in n] == []


# @scenario SC-unsaved-text-36
def test_a_failed_destination_says_so_and_leaks_no_descriptor(tmp_path):
    wd = str(tmp_path)
    _seed(wd, DONT_OVERWRITE, {"src.txt": "SOURCE DATA\n"})
    with WpeSession(wd, "int main;\n") as s:
        _open_files_panel(s)
        _typed(s, "src.txt")
        before = procinfo.open_descriptors(s.proc.pid)
        if before is None:
            pytest.skip("needs a descriptor table: /proc, lsof or fstat")
        _op_to(s, ALT_O, "nodir/out.txt")
        screen = _squash(s)
        assert "creatingthecopy:Nosuchfileordirectory" in screen, "\n".join(s.display())
        s.key(CR)
        assert s.alive()
        after = procinfo.open_descriptors(s.proc.pid)
        assert after is not None, "the editor's descriptor table was unreadable"
        assert after == before, "leaked %d descriptor(s)" % (len(after) - len(before))


# @scenario SC-unsaved-text-37
def test_a_copied_file_is_never_readable_by_others(tmp_path):
    wd = str(tmp_path)
    _seed(wd, DONT_OVERWRITE, {})
    src = os.path.join(wd, "secret.txt")
    with open(src, "wb") as fh:
        fh.write(b"P" * MODE_WINDOW_BYTES)
    os.chmod(src, 0o600)
    seen = set()
    stop = threading.Event()

    def poll():
        while not stop.is_set():
            try:
                names = os.listdir(wd)
            except OSError:
                continue
            for name in names:
                if name == "leaked.txt" or "xwpe-copy" in name:
                    try:
                        seen.add(stat.S_IMODE(os.stat(os.path.join(wd, name)).st_mode))
                    except OSError:
                        pass

    with WpeSession(wd, "int main;\n") as s:
        watcher = threading.Thread(target=poll, daemon=True)
        watcher.start()
        try:
            _copy(s, "secret.txt", "leaked.txt")
            time.sleep(2.0)
        finally:
            stop.set()
            watcher.join(timeout=5)
        assert s.alive()
        assert seen, "the copy was never seen"
        assert not [m for m in seen if m & 0o077], "modes seen: %s" % ["%04o" % m for m in sorted(seen)]
        assert stat.S_IMODE(os.stat(os.path.join(wd, "leaked.txt")).st_mode) == 0o600


# --- Move ------------------------------------------------------------------------


# @scenario SC-unsaved-text-38
def test_a_move_across_file_systems_keeps_the_source_when_nothing_is_replaced(tmp_path):
    other = _make_other_fs_folder()
    if other is None:
        pytest.skip("needs a second file system")
    try:
        wd = str(tmp_path)
        if os.stat(wd).st_dev == os.stat(other).st_dev:
            pytest.skip("same device")
        os.symlink("/dev/full", os.path.join(other, "sink"))
        _seed(wd, DONT_OVERWRITE, {"src.txt": "ONLY COPY OF THE DATA\n"})
        with WpeSession(wd, "int main;\n") as s:
            _copy(s, "src.txt", os.path.join(other, "sink"), alt=ALT_M)
            screen = _squash(s)
            assert "exists;notoverwritten" in screen
            assert _read(os.path.join(wd, "src.txt")) == "ONLY COPY OF THE DATA\n"
            assert os.path.islink(os.path.join(other, "sink"))
    finally:
        shutil.rmtree(other, ignore_errors=True)


# @scenario SC-unsaved-text-38
def test_a_move_across_file_systems_removes_the_source_after_the_copy(tmp_path):
    other = _make_other_fs_folder()
    if other is None:
        pytest.skip("needs a second file system")
    try:
        wd = str(tmp_path)
        if os.stat(wd).st_dev == os.stat(other).st_dev:
            pytest.skip("same device")
        os.symlink("/dev/full", os.path.join(other, "sink"))
        _seed(wd, OVERWRITE_ABSOLUTE, {"src.txt": "ONLY COPY OF THE DATA\n"})
        with WpeSession(wd, "int main;\n") as s:
            _copy(s, "src.txt", os.path.join(other, "sink"), alt=ALT_M)
            time.sleep(1.0)
            sink = os.path.join(other, "sink")
            assert not os.path.islink(sink), "the link was written through"
            assert _read(sink) == "ONLY COPY OF THE DATA\n"
            assert not os.path.exists(os.path.join(wd, "src.txt"))
            assert [n for n in os.listdir(other) if "xwpe-copy" in n] == []
    finally:
        shutil.rmtree(other, ignore_errors=True)


# @scenario SC-unsaved-text-39
def test_a_move_whose_source_cannot_be_removed_says_where_the_copy_is(tmp_path):
    other = _make_other_fs_folder()
    if other is None:
        pytest.skip("needs a second file system")
    try:
        wd = str(tmp_path)
        if os.stat(wd).st_dev == os.stat(other).st_dev:
            pytest.skip("same device")
        _seed(wd, DONT_OVERWRITE, {"src.txt": "ONLY COPY\n"})
        with WpeSession(wd, "int main;\n", env_extra={"XWPE_TEST_FAIL": "fileop-remove-source"}) as s:
            _copy(s, "src.txt", os.path.join(other, "dst.txt"), alt=ALT_M)
            screen = _squash(s)
            assert "removingthesource:Permissiondenied" in screen, "\n".join(s.display())
            assert "holdsthecopy" in screen and "isstillinplace" in screen
            assert _read(os.path.join(other, "dst.txt")) == "ONLY COPY\n"
            assert _read(os.path.join(wd, "src.txt")) == "ONLY COPY\n"
    finally:
        shutil.rmtree(other, ignore_errors=True)


# @scenario SC-unsaved-text-40
def test_a_close_that_fails_is_reported_and_leaves_no_temporary(tmp_path):
    wd = str(tmp_path)
    _seed(wd, DONT_OVERWRITE, {"src.txt": "SOURCE\n"})
    with WpeSession(wd, "int main;\n", env_extra={"XWPE_TEST_FAIL": "fileop-close"}) as s:
        _copy(s, "src.txt", "out.bin")
        screen = _squash(s)
        assert "closing:Input/outputerror" in screen, "\n".join(s.display())
        assert "Nothingwaschanged" in screen
        assert not os.path.exists(os.path.join(wd, "out.bin"))
        assert [n for n in os.listdir(wd) if "xwpe-copy" in n] == []


# @scenario SC-unsaved-text-41
def test_a_rename_that_fails_leaves_the_destination_and_the_wastebasket(tmp_path):
    wd = str(tmp_path)
    _seed(wd, OVERWRITE_WB, {"src.txt": "SOURCE\n", "dst.txt": "ORIGINAL\n"})
    with WpeSession(wd, "int main;\n", env_extra={"XWPE_TEST_FAIL": "fileop-rename"}) as s:
        _copy(s, "src.txt", "dst.txt")
        screen = _squash(s)
        assert "puttingitinplace:Input/outputerror" in screen, "\n".join(s.display())
        assert "Nothingwaschanged" in screen
        assert _read(os.path.join(wd, "dst.txt")) == "ORIGINAL\n"
        assert not os.path.exists(_wastebasket(wd)) or os.listdir(_wastebasket(wd)) == []
        assert [n for n in os.listdir(wd) if "xwpe-copy" in n] == []


# @scenario SC-unsaved-text-42
def test_a_moved_link_keeps_its_target_text(tmp_path):
    wd = str(tmp_path)
    _seed(wd, DONT_OVERWRITE, {"x": "TARGET\n"})
    os.makedirs(os.path.join(wd, "sub"))
    os.symlink("../x", os.path.join(wd, "l"))
    with WpeSession(wd, "int main;\n") as s:
        _open_files_panel(s)
        _typed(s, "l")
        _op_to(s, ALT_M, "sub/l")
        time.sleep(0.8)
        moved = os.path.join(wd, "sub", "l")
        assert os.path.islink(moved)
        assert os.readlink(moved) == "../x"
        assert not os.path.lexists(os.path.join(wd, "l"))


# @scenario SC-unsaved-text-42
def test_a_link_moved_across_file_systems_keeps_its_target_text(tmp_path):
    other = _make_other_fs_folder()
    if other is None:
        pytest.skip("needs a second file system")
    try:
        wd = str(tmp_path)
        if os.stat(wd).st_dev == os.stat(other).st_dev:
            pytest.skip("same device")
        _seed(wd, DONT_OVERWRITE, {"x": "TARGET\n"})
        # a target with no slash is the one the original rewrote under the panel's folder
        os.symlink("x", os.path.join(wd, "l"))
        with WpeSession(wd, "int main;\n") as s:
            _open_files_panel(s)
            _typed(s, "l")
            _op_to(s, ALT_M, os.path.join(other, "l"))
            time.sleep(1.0)
            moved = os.path.join(other, "l")
            assert os.path.islink(moved)
            assert os.readlink(moved) == "x"
            assert not os.path.lexists(os.path.join(wd, "l"))
    finally:
        shutil.rmtree(other, ignore_errors=True)


# --- Link ------------------------------------------------------------------------


# @scenario SC-unsaved-text-43
def test_link_from_the_files_panel_follows_dont_overwrite(tmp_path):
    wd = str(tmp_path)
    _seed(wd, DONT_OVERWRITE, {"src.txt": "SOURCE\n", "dst.txt": "ORIGINAL\n"})
    with WpeSession(wd, "int main;\n") as s:
        _copy(s, "src.txt", "dst.txt", alt=ALT_L)
        screen = _squash(s)
        assert "exists;notoverwritten" in screen
        assert not os.path.islink(os.path.join(wd, "dst.txt"))
        assert _read(os.path.join(wd, "dst.txt")) == "ORIGINAL\n"


# @scenario SC-unsaved-text-43
def test_a_hard_link_across_file_systems_falls_back_to_a_symbolic_one(tmp_path):
    other = _make_other_fs_folder()
    if other is None:
        pytest.skip("needs a second file system")
    try:
        wd = str(tmp_path)
        if os.stat(wd).st_dev == os.stat(other).st_dev:
            pytest.skip("same device")
        _seed(wd, DONT_OVERWRITE, {"src.txt": "SOURCE\n"})
        with WpeSession(wd, "int main;\n") as s:
            _copy(s, "src.txt", os.path.join(other, "lnk"), alt=ALT_L)
            time.sleep(0.8)
            screen = _squash(s)
            assert "Cannot" not in screen, "\n".join(s.display())
            assert os.path.islink(os.path.join(other, "lnk"))
    finally:
        shutil.rmtree(other, ignore_errors=True)


# @scenario SC-unsaved-text-43
def test_a_hard_link_on_one_file_system_is_a_hard_link(tmp_path):
    wd = str(tmp_path)
    _seed(wd, DONT_OVERWRITE, {"src.txt": "SOURCE\n"})
    with WpeSession(wd, "int main;\n") as s:
        _copy(s, "src.txt", "hard.txt", alt=ALT_L)
        time.sleep(0.5)
        assert not os.path.islink(os.path.join(wd, "hard.txt"))
        assert os.stat(os.path.join(wd, "hard.txt")).st_nlink == 2


# --- the wastebasket -------------------------------------------------------------


# @scenario SC-unsaved-text-44
def test_remove_never_replaces_an_earlier_file_in_the_wastebasket(tmp_path):
    wd = str(tmp_path)
    _seed(wd, DEFAULT, {"a.txt": "NEW\n", "zz.txt": "z\n"})
    os.makedirs(_wastebasket(wd))
    with open(os.path.join(_wastebasket(wd), "a.txt"), "w") as fh:
        fh.write("EARLIER\n")
    with WpeSession(wd, "int main;\n") as s:
        _open_files_panel(s)
        _typed(s, "a.txt")
        s.key(ALT_R)
        time.sleep(0.8)
        assert not os.path.exists(os.path.join(wd, "a.txt"))
        assert _read(os.path.join(_wastebasket(wd), "a.txt")) == "EARLIER\n"
        assert _read(os.path.join(_wastebasket(wd), "a.txt.1")) == "NEW\n"


# @scenario SC-unsaved-text-44
def test_remove_absolute_unlinks_without_the_wastebasket(tmp_path):
    wd = str(tmp_path)
    _seed(wd, OPT_REKURSIVE | OPT_PROMPT_DELETE, {"a.txt": "NEW\n", "zz.txt": "z\n"})
    with WpeSession(wd, "int main;\n") as s:
        _open_files_panel(s)
        _typed(s, "a.txt")
        s.key(ALT_R)
        time.sleep(0.8)
        assert not os.path.exists(os.path.join(wd, "a.txt"))
        assert not os.path.exists(_wastebasket(wd)) or os.listdir(_wastebasket(wd)) == []


# @scenario SC-unsaved-text-44
def test_remove_into_a_wastebasket_open_to_others_closes_it(tmp_path):
    wd = str(tmp_path)
    _seed(wd, DEFAULT, {"a.txt": "NEW\n", "zz.txt": "z\n"})
    os.makedirs(_wastebasket(wd))
    os.chmod(_wastebasket(wd), 0o755)
    with WpeSession(wd, "int main;\n") as s:
        _open_files_panel(s)
        _typed(s, "a.txt")
        s.key(ALT_R)
        time.sleep(0.8)
        assert _read(os.path.join(_wastebasket(wd), "a.txt")) == "NEW\n"
        assert stat.S_IMODE(os.stat(_wastebasket(wd)).st_mode) == 0o700


# --- a copy that changes hands ---------------------------------------------------


# @scenario SC-unsaved-text-48d
def test_a_copy_into_another_group_is_named_on_the_status_row(tmp_path):
    """A setgid file copied into a folder of another group of the user is a
    copy with another group than its original: it gets no setgid bit and the
    status row names both groups.  A real group of the user and a real
    folder, no hook: the copy's group is the one the system gave it.  Skips
    by name when the user has no second group.  The X11 and Wayland twins
    are not needed: the sentence is made by shared code and shown by the
    same status row on every backend."""
    wd = str(tmp_path)
    workdir_in_my_group(wd)
    special_bits_settable(wd, stat.S_ISGID)
    others = [g for g in os.getgroups() if g != os.getegid()]
    if not others:
        pytest.skip("this user has no second group to copy into")
    _seed(wd, DEFAULT, {"a.txt": "prog\n"})
    os.chmod(os.path.join(wd, "a.txt"), 0o2755)
    folder = os.path.join(wd, "g2")
    os.makedirs(folder)
    os.chown(folder, -1, others[0])
    os.chmod(folder, 0o2770)
    with WpeSession(wd, "int main;\n") as s:
        _open_files_panel(s)
        _typed(s, "a.txt")
        _op_to(s, ALT_O, "g2/a.txt")
        time.sleep(0.8)
        screen = _squash(s)
        assert "thesetgidbitwasnotkept" in screen, "\n".join(s.display())
        assert "thecopybelongstogroup" in screen, "\n".join(s.display())
    assert _read(os.path.join(folder, "a.txt")) == "prog\n"
    assert stat.S_IMODE(os.stat(os.path.join(folder, "a.txt")).st_mode) == 0o755


# --- attributes ------------------------------------------------------------------


# @scenario SC-unsaved-text-45
def test_attributes_keep_the_bits_the_dialog_does_not_show(tmp_path):
    wd = str(tmp_path)
    workdir_in_my_group(wd)
    special_bits_settable(wd, stat.S_ISGID)
    _seed(wd, DEFAULT, {"aaa.txt": "a\n"})
    folder = os.path.join(wd, "sg")
    os.makedirs(folder)
    os.chmod(folder, 0o2755)
    with WpeSession(wd, "int main;\n") as s:
        _open_tree_panel(s)
        _typed(s, "sg")
        s.key(ALT_A)
        s.key("\033d")      # Other: ReaD off
        s.key("\033u")      # Other: ExecUte off
        s.key(ALT_O)
        time.sleep(0.8)
        assert stat.S_IMODE(os.stat(folder).st_mode) == 0o2750


# @scenario SC-unsaved-text-45
def test_attributes_name_the_special_bit_the_system_would_not_keep(tmp_path):
    """The Attributes dialog applies the nine bits it shows and, when the
    system does not let this user keep the setgid bit the folder had, says
    so on the status row instead of refusing the change or saying nothing.
    The test hook makes the owner report the refusal over a real kernel.  The
    X11 and Wayland twins are not needed: the sentence is made by shared code
    and shown by the same status row on every backend."""
    wd = str(tmp_path)
    workdir_in_my_group(wd)
    special_bits_settable(wd, stat.S_ISGID)
    _seed(wd, DEFAULT, {"aaa.txt": "a\n"})
    folder = os.path.join(wd, "sg")
    os.makedirs(folder)
    os.chmod(folder, 0o2755)
    env = {"XWPE_TEST_FAIL": "fileop-refuse-special"}
    with WpeSession(wd, "int main;\n", env_extra=env) as s:
        _open_tree_panel(s)
        _typed(s, "sg")
        s.key(ALT_A)
        s.key("\033d")      # Other: ReaD off
        s.key("\033u")      # Other: ExecUte off
        s.key(ALT_O)
        time.sleep(0.8)
        screen = _squash(s)
        # longer than the status row: a box, which shows the whole sentence
        assert ("Permissionsofsgchanged,butthesetgidbitwasnotkept:"
                "Operationnotpermitted.") in screen, "\n".join(s.display())
    assert stat.S_IMODE(os.stat(folder).st_mode) & 0o777 == 0o750


# @scenario SC-unsaved-text-45
def test_attributes_of_a_plain_file_change_its_permissions(tmp_path):
    wd = str(tmp_path)
    _seed(wd, DEFAULT, {"aaa.txt": "a\n"})
    os.chmod(os.path.join(wd, "aaa.txt"), 0o755)
    with WpeSession(wd, "int main;\n") as s:
        _open_files_panel(s)
        _typed(s, "aaa.txt")
        s.key(ALT_A)
        s.key("\033d")
        s.key("\033u")
        s.key(ALT_O)
        time.sleep(0.8)
        assert stat.S_IMODE(os.stat(os.path.join(wd, "aaa.txt")).st_mode) == 0o750
