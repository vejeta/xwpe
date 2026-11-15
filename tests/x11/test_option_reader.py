"""The graphical editor starts on an option file with a line it cannot read.

Twin of tests/test_option_reader.py (06a, 06b).  The reader and the box are
shared code with no backend branch, so these rows are a parity smoke test:
the editor must be alive, the box must change the screen, and OK must give
the desk back with the file left as the user wrote it.  The text of the box
is asserted by the terminal rows; a screenshot has no text.

The wayland twin runs this same body.
"""
import os

import pytest

from conftest import _sleep, changed_pixels

SETTLE_S = 1.5
BAD_RC = ("# a project\n[General]\nAutosave : 1\n\n# one\n# two\n"
          "nocolon here\nTab : 3\n")


@pytest.fixture
def folder_rc(tmp_path):
    """A ./xwperc with an unreadable line 7, written before the editor
    starts (the fixture of the editor is requested after this one)."""
    path = os.path.join(str(tmp_path), "xwperc")
    with open(path, "w") as fh:
        fh.write(BAD_RC)
    return path


# @scenario SC-option-codec-06i
def test_x11_unreadable_option_line_is_boxed_and_the_file_kept(folder_rc, xwpe):
    """The box comes up, OK closes it, and quitting leaves the file as it was."""
    _sleep(SETTLE_S)
    boxed = xwpe.screenshot()
    xwpe.key("Return", delay=1.0)
    desk = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died on an option file with a bad line"
    assert changed_pixels(boxed, desk) > 1000, \
        "OK should close the box (screen barely changed)"
    xwpe.key("alt+x", delay=1.5)
    with open(folder_rc) as fh:
        assert fh.read() == BAD_RC, "the quit-time save rewrote the file"
