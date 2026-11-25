"""A file the user opens never makes the editor wait, and a name that is not a
regular file is refused with what it is.

File > Open and the command line load a file through the one read owner
(we_read.c).  It opens without waiting for a writer and without taking a
terminal, and judges what is under the name before a byte is read.  A pipe
and a device are refused with their kind ("it is a pipe", "it is a device");
reading from a pipe comes with a later change.  The windows the editor fills
itself (Messages, Watches, Stack, the clipboard) never read a file of the same
name from the folder, so a pipe with one of those names does not stop them.

  L1  File > Open of a pipe nobody writes to: the box says "it is a pipe",
      no window opens and the editor takes the next key.
  L2  `wpe /dev/zero` under an address-space limit: "it is a device", no
      window for it, and the editor does not read until memory ends.
  L3  a regular file, a missing file and a file closed to reading open as
      they did (text, an empty new file, the system's reason).
  L4  a pipe in the folder named like a window the editor fills itself:
      the Messages window shows the compiler's lines and the clipboard still
      copies and pastes, with the pipe absent and present.
  L5  `-pm` naming a pipe: refused with its kind.

Run: WPE_BIN=$PWD/wpe tests/.venv/bin/python -m pytest -q tests/test_read_waits_load.py
(as wpe, never we).
"""
import os

import pytest

pytest.importorskip("pyte")

from host_tools import require                                   # noqa: E402
from mode_premise import file_mode_is_obeyed                     # noqa: E402
from wpe_driver import WpeSession, wait_screen, wait_until       # noqa: E402
from test_read_refused import (                                  # noqa: E402
    CR, ESC, COLS_ROWS, SEED, STATE_WAIT_S, _open_with_f3, _put, _titled,
    _wait_said, _wait_titled)
from test_read_waits import address_space_limit, ADDRESS_SPACE_BYTES, \
    _env, _screen                                                # noqa: E402

F2 = "\033OQ"
F9 = "\033[20~"
LARGE_FILE_BYTES = 2 << 20          # over the owner's 1 MiB bound for an options file
CTRL_K = "\x0b"
CTRL_INS = "\x1b[2;5~"
SHIFT_INS = "\x1b[2;2~"
DOWN = "\033[B"
BUILD_WAIT = 4.0
COMPILE_FIND_S = 20
ERROR_SOURCE = "int main(void)\n{\n    int x = 1\n    return x;\n}\n"
WINDOW_NAMES = ("Messages", "Watches", "Stack", "Buffer")


def _fifo(folder, name):
    """A pipe named @name in @folder; nobody ever writes to it."""
    path = os.path.join(str(folder), name)
    os.mkfifo(path)
    return path


def _keys_still_land(w):
    """The editor takes a key: a letter typed reaches the window on top."""
    w.key("z", delay=0.4)
    return wait_until(w, lambda: "z" in "\n".join(w.display(drain=0)), STATE_WAIT_S)


# @scenario SC-read-waits-load-01
def test_file_open_of_a_pipe_with_no_writer_is_refused_with_its_kind(tmp_path):
    """F3, p (a pipe nobody writes to), Enter: the box names the file and says
    it is a pipe, no window titled p opens, and the editor takes the next key.
    Opened by name the editor would wait for a writer for ever."""
    _fifo(tmp_path, "p")
    with WpeSession(str(tmp_path), SEED, filename="t.c", **COLS_ROWS) as w:
        _open_with_f3(w, "p")
        screen = _wait_said(w, "Cannotopenfile", "itisapipe")
        assert "Cannotopenfile" in screen and "itisapipe" in screen, "\n".join(w.display())
        assert not _titled(w, "p"), "\n".join(w.display())
        w.key(CR, delay=0.6)                # answer the box
        w.key(ESC, delay=0.6)               # leave the File Manager
        assert w.alive()
        assert _keys_still_land(w), "the editor took no key:\n" + _screen(w)


# @scenario SC-read-waits-load-02
def test_a_device_on_the_command_line_is_refused_and_never_read_to_its_end(tmp_path):
    """wpe /dev/zero under an address-space limit: the box says it is a
    device and no window opens for it.  An editor that loads /dev/zero has no
    end to read, and the limit makes it stop at a known size."""
    with address_space_limit(ADDRESS_SPACE_BYTES):
        w = WpeSession(str(tmp_path), SEED, filename="t.c", args=("/dev/zero",),
                       **COLS_ROWS)
    with w:
        assert wait_screen(w, "Cannot open file", STATE_WAIT_S), _screen(w)
        screen = _screen(w)
        assert "it is a device" in screen, screen
        assert w.alive(), "the editor died:\n" + screen
        w.key(CR, delay=0.8)
        assert _wait_titled(w, "t.c"), \
            "t.c did not open once the box was answered:\n" + _screen(w)
        assert not any(" zero " in row for row in w.display()), _screen(w)


# @scenario SC-read-waits-load-03
def test_a_regular_file_opens_with_its_text(tmp_path):
    """File > Open of a regular file shows its own text in a window under its
    name."""
    _put(tmp_path, "r.c", "int rrr;\n", 0o644)
    with WpeSession(str(tmp_path), SEED, filename="t.c", **COLS_ROWS) as w:
        _open_with_f3(w, "r.c")
        assert _wait_titled(w, "r.c"), _screen(w)
        assert wait_screen(w, "int rrr;", STATE_WAIT_S), _screen(w)
        assert w.alive()


# @scenario SC-read-waits-load-03
def test_a_large_regular_file_opens_whole(tmp_path):
    """The user asked for the file, so its size is not limited: a source of
    2 MiB (over the owner's bound for an options file) opens, with its last
    line."""
    line = "int a_long_line_of_a_source_file = %06d; /* padding */\n"
    count = LARGE_FILE_BYTES // len(line % 0) + 1
    last = line % (count - 1)
    with open(os.path.join(str(tmp_path), "big.c"), "w") as fh:
        for n in range(count):
            fh.write(line % n)
    with WpeSession(str(tmp_path), SEED, filename="t.c", **COLS_ROWS) as w:
        _open_with_f3(w, "big.c")
        assert _wait_titled(w, "big.c"), _screen(w)
        assert "Cannot open" not in _screen(w), _screen(w)
        w.key("\033[F", "\033[6;5~", delay=1.0)      # Ctrl-PgDn: the end of the text
        assert wait_screen(w, last.strip()[:40], STATE_WAIT_S), _screen(w)


# @scenario SC-read-waits-load-03
def test_a_file_opened_through_the_owner_is_still_asked_about_when_replaced(tmp_path):
    """The window keeps the identity of the file the owner read, so a file
    renamed over the name afterwards is asked about at Save, never overwritten
    unseen."""
    _put(tmp_path, "r.c", "mine\n", 0o644)
    with WpeSession(str(tmp_path), SEED, filename="t.c", **COLS_ROWS) as w:
        _open_with_f3(w, "r.c")
        assert _wait_titled(w, "r.c"), _screen(w)
        w.key("X", delay=0.3)
        incoming = _put(tmp_path, "incoming", "theirs\n", 0o644)
        os.rename(incoming, os.path.join(str(tmp_path), "r.c"))
        w.key(F2, delay=0.8)
        screen = _wait_said(w, "wasreplacedondisk")
        assert "wasreplacedondisk" in screen, _screen(w)
        with open(os.path.join(str(tmp_path), "r.c")) as fh:
            assert fh.read() == "theirs\n", "something was written before the answer"


# @scenario SC-read-waits-load-03
def test_a_missing_file_opens_as_a_new_file(tmp_path):
    """A name that does not exist opens an empty window under that name, and
    nothing is said."""
    with WpeSession(str(tmp_path), SEED, filename="t.c", args=("fresh.c",), **COLS_ROWS) as w:
        assert _wait_titled(w, "fresh.c"), _screen(w)
        assert "Cannot open" not in _screen(w), _screen(w)
        assert "not found" not in _screen(w), _screen(w)


# @scenario SC-read-waits-load-03
def test_a_file_closed_to_reading_says_why_and_opens_no_window(tmp_path):
    """The system's reason, as before the owner read the file."""
    file_mode_is_obeyed(str(tmp_path), 0o000, os.O_RDONLY)
    locked = _put(tmp_path, "locked.c", "secret\n", 0o000)
    try:
        with WpeSession(str(tmp_path), SEED, filename="t.c", args=("locked.c",),
                        **COLS_ROWS) as w:
            assert wait_screen(w, "Cannot open file", STATE_WAIT_S), _screen(w)
            screen = _screen(w)
            assert "Permission denied" in screen, screen
            assert not _titled(w, "locked.c"), screen
    finally:
        os.chmod(locked, 0o600)


def _copy_line_to_the_clipboard_and_paste(w):
    """Mark the first line, copy it to the clipboard and paste it below."""
    w.key(CTRL_K, "b", delay=0.3)
    w.key(DOWN, delay=0.2)
    w.key(CTRL_K, "k", delay=0.3)
    w.key(CTRL_INS, delay=0.4)
    w.key(SHIFT_INS, delay=0.6)


# @scenario SC-read-waits-load-04
@pytest.mark.parametrize("present", [False, True], ids=["absent", "present"])
def test_a_pipe_named_like_an_editor_window_does_not_stop_it(tmp_path, present):
    """A folder (a cloned repository) can hold a pipe named Messages, Watches,
    Stack or Buffer.  The Messages window shows the compiler's lines, the
    editor starts with its clipboard window and the clipboard copies and
    pastes: none of those windows reads a file of its name."""
    require("gcc")
    if present:
        for name in WINDOW_NAMES:
            _fifo(tmp_path, name)
    with WpeSession(str(tmp_path), ERROR_SOURCE, filename="t.c", **COLS_ROWS) as w:
        assert w.alive(), "the editor did not start"
        w.key(F9, delay=BUILD_WAIT)
        assert wait_screen(w, "error", COMPILE_FIND_S), \
            "Messages shows no compiler line:\n" + _screen(w)
        assert "it is a pipe" not in _screen(w), _screen(w)
        assert w.alive()


# @scenario SC-read-waits-load-04
@pytest.mark.parametrize("present", [False, True], ids=["absent", "present"])
def test_the_clipboard_works_with_a_pipe_named_buffer_in_the_folder(tmp_path, present):
    """The clipboard window is the editor's own: a pipe named Buffer in the
    folder is never read, and Ctrl-Ins then Shift-Ins pastes the copied line."""
    if present:
        _fifo(tmp_path, "Buffer")
    with WpeSession(str(tmp_path), "int c;\n\n", filename="t.c", **COLS_ROWS) as w:
        assert w.alive(), "the editor did not start"
        _copy_line_to_the_clipboard_and_paste(w)
        w.save()
        assert w.text().count("int c;") == 2, repr(w.text())


# @scenario SC-read-waits-load-05
def test_parse_messages_naming_a_pipe_is_refused_with_its_kind(tmp_path):
    """wpe -pm p, p a pipe nobody writes to: the box says it is a pipe, and
    the editor goes on to open t.c."""
    _fifo(tmp_path, "p")
    with WpeSession(str(tmp_path), SEED, filename="t.c", args=("-pm", "p"),
                    **COLS_ROWS) as w:
        assert wait_screen(w, "Cannot open file", STATE_WAIT_S), _screen(w)
        assert "it is a pipe" in _screen(w), _screen(w)
        assert w.alive()
