"""The graphical editor starts with an empty error pattern and opens the
Compiler options on it.

Twin of tests/test_option_codec.py.  The codec of an option value is shared
code with no backend branch, so this row is a parity smoke row: on a plain
build it passes before and after the fix, and it is a guard only on a
sanitizer xwpe, where the one-byte overflow on an empty pattern is reported.
The memory rows are the terminal ones; here the editor must be alive after
each step and its screen must change when the dialog opens.

The wayland twin runs this same body.
"""
import os

import pytest

from conftest import _sleep, changed_pixels

SETTLE_S = 1.5
EMPTY_PATTERN_RC = ("[General]\nAutosave : 1\n"
                    "[Language/C]\nCompiler : gcc\nMessageString :\n")


@pytest.fixture
def folder_rc(tmp_path):
    """A ./xwperc with an empty MessageString, written before the editor
    starts (the fixture of the editor is requested after this one)."""
    with open(os.path.join(str(tmp_path), "xwperc"), "w") as fh:
        fh.write(EMPTY_PATTERN_RC)


# @scenario SC-option-codec-08x
def test_x11_compiler_options_open_on_an_empty_pattern(folder_rc, xwpe):
    """Alt-O, C, C on an empty MessageString: the editor is alive and the
    dialog is on screen."""
    _sleep(SETTLE_S)
    before = xwpe.screenshot()
    xwpe.key("alt+o", delay=1.0)
    xwpe.key("c", delay=1.0)
    xwpe.key("c", delay=1.5)
    opened = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died opening the Compiler options"
    assert changed_pixels(before, opened) > 3000, \
        "Options > Compiler should open its dialog (screen barely changed)"
