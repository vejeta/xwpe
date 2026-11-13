"""A file's line endings and its final line break have one owner (we_lineend.c).

Saving a file changes only the lines the user changed: a CRLF file is saved
CRLF, an LF file LF, a carriage return that is not part of a CR LF break is
a byte of the text (shown ^M) and is written back where it was.  Every row
reads the file's bytes with open(path, "rb"): the driver's text() reads in
text mode and turns CR LF into LF, which hides exactly what these rows test.

The dump's W line carries the buffer's style and whether the file ended on a
line break (eol=lf|crlf final=0|1).
"""
import os
import signal

import pytest

pytest.importorskip("pyte")

from edit_model.oracle import ENTER, F2, EditSession, alt, ctrl  # noqa: E402

BLOCK_READ = [ctrl("k"), "r"]


def _folder(tmp_path, name):
    """A folder of its own for one more session of a row: a session's options
    file lives in its folder, and the next one must not read it."""
    path = os.path.join(str(tmp_path), name)
    os.makedirs(path, exist_ok=True)
    return path


def _save_after(tmp_path, seed, keys, filename="t.txt", xwperc=None):
    """Open @seed, send @keys, save with F2: the bytes on disk.  The folder
    is the filename's own, so a row may call it again with another options
    file."""
    folder = _folder(tmp_path, "%s.%s" % (filename, "rc" if xwperc else "plain"))
    with EditSession(folder, seed, filename=filename, xwperc=xwperc) as s:
        s.send(keys)
        s.send([F2])
        return s.disk(filename)


def _window(tmp_path, seed, filename="t.txt", xwperc=None):
    """The dump's window of a file just opened."""
    with EditSession(str(tmp_path), seed, filename=filename, xwperc=xwperc) as s:
        return s.last.win


def _project_options(tmp_path, text):
    """An xwperc in the folder the editor starts in, as a cloned project has."""
    with open(os.path.join(str(tmp_path), "xwperc"), "w") as fh:
        fh.write("[General]\n" + text)


# @scenario SC-line-endings-01
def test_a_crlf_file_is_saved_crlf(tmp_path):
    assert _save_after(tmp_path, b"a\r\nb\r\n", ["x"]) == b"xa\r\nb\r\n"


# @scenario SC-line-endings-02
def test_a_mixed_file_keeps_each_line_as_it_was(tmp_path):
    """One CR LF among bare LFs does not make the file DOS: the lines the
    user did not touch are written as they were read."""
    got = _save_after(tmp_path, b"a\r\nb\nc\r\n", ["x"])
    assert got == b"xa\r\nb\nc\r\n", got


# @scenario SC-line-endings-03
def test_a_lone_carriage_return_is_text(tmp_path):
    """The byte stays in its row, shown ^M, and is written back where it was."""
    with EditSession(str(tmp_path), b"a\rb\n", filename="t.txt") as s:
        shown = "\n".join(s.display())
        s.send(["x"])
        s.send([F2])
        got = s.disk("t.txt")
    assert got == b"xa\rb\n", got
    assert "a^Mb" in shown, shown


# @scenario SC-line-endings-04
def test_the_dump_names_a_crlf_files_style(tmp_path):
    w = _window(tmp_path, b"a\r\nb\r\n")
    assert (w.eol, w.final) == ("crlf", 1)
    w = _window(tmp_path, b"a\nb\n")
    assert (w.eol, w.final) == ("lf", 1)


# @scenario SC-line-endings-05
def test_a_file_without_a_final_break_is_known_and_saved_with_one(tmp_path):
    """The dump says the file had no final break; a save writes one, as the
    editor always has (the option to keep it is the keymap's)."""
    with EditSession(str(tmp_path), b"a\r\nb", filename="t.txt") as s:
        w = s.last.win
        s.send([F2])
        got = s.disk("t.txt")
    assert (w.eol, w.final) == ("crlf", 0)
    assert got == b"a\r\nb\r\n", got


# @scenario SC-line-endings-06
def test_a_zero_byte_file_takes_the_default_style(tmp_path):
    """No line break says nothing of a style: the Data key of the options
    file, the original's way to ask for MS-DOS files, decides."""
    got = _save_after(tmp_path, b"", ["q"], xwperc="[General]\nData : 109\n")
    assert got == b"q\r\n", got
    got = _save_after(tmp_path, b"", ["q"], filename="u.txt")
    assert got == b"q\n", got


# @scenario SC-line-endings-07
def test_a_new_file_takes_the_default_style(tmp_path):
    with EditSession(_folder(tmp_path, "dos"), None, filename="new.txt",
                     xwperc="[General]\nData : 109\n") as s:
        w = s.last.win
        s.send(["q"])
        s.send([F2])
        got = s.disk("new.txt")
    assert w.eol == "crlf"
    assert got == b"q\r\n", got
    got = _save_after(tmp_path, None, ["q"], filename="other.txt")
    assert got == b"q\n", got


# @scenario SC-line-endings-08
def test_a_file_that_has_breaks_keeps_its_own_style_against_the_default(tmp_path):
    got = _save_after(tmp_path, b"a\nb\n", ["q"], xwperc="[General]\nData : 109\n")
    assert got == b"qa\nb\n", got
    got = _save_after(tmp_path, b"a\r\nb\r\n", ["q"], filename="c.txt")
    assert got == b"qa\r\nb\r\n", got


def _block_read(s, name):
    """Block > Read of @name at the cursor."""
    s.send(BLOCK_READ)
    s.send(list(name) + [ENTER])


# @scenario SC-line-endings-09
def test_a_file_read_into_an_lf_file_takes_the_buffers_style(tmp_path):
    with open(os.path.join(str(tmp_path), "r.txt"), "wb") as fh:
        fh.write(b"X\r\nY\r\n")
    with EditSession(str(tmp_path), b"a\nb\n", filename="t.txt") as s:
        s.send(["\033[B"])
        _block_read(s, "r.txt")
        s.send([F2])
        got = s.disk("t.txt")
    assert got == b"a\nX\nY\nb\n", got


# @scenario SC-line-endings-10
def test_a_mixed_file_read_in_keeps_the_carriage_return_of_its_line(tmp_path):
    with open(os.path.join(str(tmp_path), "r.txt"), "wb") as fh:
        fh.write(b"X\r\nY\n")
    with EditSession(str(tmp_path), b"a\nb\n", filename="t.txt") as s:
        s.send(["\033[B"])
        _block_read(s, "r.txt")
        s.send([F2])
        got = s.disk("t.txt")
    assert got == b"a\nX\r\nY\nb\n", got


# @scenario SC-line-endings-11
def test_an_autosave_of_a_crlf_file_is_crlf(tmp_path):
    rc = "[General]\nAutosave : 2\nMaxChanges : 3\n"
    with EditSession(str(tmp_path), b"a\r\nb\r\n", filename="t.txt", xwperc=rc) as s:
        s.send(["x", "y", "z", "w"])
        got = s.disk("t.txt.ASV")
    assert got == b"xyzwa\r\nb\r\n", got


# @scenario SC-line-endings-12
def test_an_emergency_save_of_a_crlf_file_is_crlf(tmp_path):
    with EditSession(str(tmp_path), b"a\r\nb\r\n", filename="t.txt") as s:
        s.send(["x"])
        s.proc.send_signal(signal.SIGTERM)
        s.proc.wait(timeout=10)
        got = s.disk("t.txt.ESV")
    assert got == b"xa\r\nb\r\n", got


# @scenario SC-line-endings-13
def test_a_long_crlf_line_is_written_back_as_one_line(tmp_path):
    """A line wider than Max. Columns is wrapped into rows; the carriage
    return stays with its line break, not inside a row."""
    seed = b"a" * 200 + b"\r\n" + b"b\r\n"
    with EditSession(str(tmp_path), seed, filename="t.txt") as s:
        w = s.last.win
        s.send([F2])
        got = s.disk("t.txt")
    assert w.rows == -1, "a row is wider than Max. Columns or ends wrongly"
    assert got == seed, got


# @scenario SC-line-endings-14
def test_a_nul_byte_does_not_merge_lines(tmp_path):
    """A row cannot hold a NUL byte: the byte is dropped, but the rest of
    its row and its line break are kept."""
    got = _save_after(tmp_path, b"a\x00b\nc\n", ["x"])
    assert got == b"xab\nc\n", got


# @scenario SC-line-endings-15
@pytest.mark.parametrize("value", ["70", "0", "68"])
def test_a_data_value_in_a_folders_options_does_not_break_the_editor(tmp_path, value):
    """Data was read into the window kind: 70 (a file manager's) crashed the
    editor at start, 0 and 68 made a window that took no keys."""
    _project_options(tmp_path, "Data : %s\n" % value)
    with EditSession(str(tmp_path), b"a\n", filename="t.txt") as s:
        assert s.alive()
        s.send(["q"])
        s.send([F2])
        got = s.disk("t.txt")
        assert s.alive()
    assert got == b"qa\n", got


# @scenario SC-line-endings-16
@pytest.mark.parametrize("value, written", [("109", "109"), ("70", "110"), ("0", "110")])
def test_the_data_key_is_written_back_as_the_original_wrote_it(tmp_path, value, written):
    """Saving the options writes Data : 109 (MS-DOS files) or 110, so an
    older editor reading the same file means the same; a value that was
    never a style is not written back.  The file is rewritten whole (it
    gets its Version line), so the check sees a save really happened."""
    rc = "[General]\nData : %s\n" % value
    folder = _folder(tmp_path, "data" + value)
    with EditSession(folder, b"a\n", filename="t.txt", xwperc=rc) as s:
        s.send([alt("o"), "s"])               # Options > Save options
        s.send([ENTER])
    with open(os.path.join(folder, ".config", "xwpe", "xwperc")) as fh:
        text = fh.read()
    assert "Version :" in text, "the options were not saved: %r" % text
    assert "Data : %s\n" % written in text, text
