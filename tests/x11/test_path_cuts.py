"""A path longer than the buffer that held it is no longer cut, in the
graphical editor.

Twin of tests/test_path_cuts.py for the rows whose code is shared but whose
keys and drawing go another road in the graphical editor: Move in the file
manager and a project's names saved from Project-Options.  Each row reads
the effect from the disk: the file's name after Move, the project file
after Save.

The wayland twin runs this same body.
"""
import os

import pytest

from conftest import _sleep

SETTLE_S = 1.5
NAME_BYTES = 200            # a name over the 128 bytes Move offered
LIBRARY_BYTES = 98          # over the 79 bytes the project's library was kept in
EXECUTABLE_BYTES = 150      # over the 128 bytes the Executable field took
# Starts with "a" so it is the first entry of the Files list, before the
# frame dump the Wayland session writes in the same folder.
LONG_NAME = "a" * (NAME_BYTES - 2) + ".c"
LIBRARY = "l" * LIBRARY_BYTES
EXECUTABLE = "x" * EXECUTABLE_BYTES


def _alive(xwpe, what):
    assert xwpe.proc.poll() is None, "xwpe died: " + what


@pytest.fixture
def long_named_file(tmp_path):
    """A second file in the editor's folder whose name is 200 bytes; made
    before the file manager lists the folder."""
    (tmp_path / LONG_NAME).write_text("int x;\n")
    return LONG_NAME


@pytest.fixture
def project_with_long_names(tmp_path):
    """The start folder names a project whose LIBNAME and EXENAME are longer
    than the old fields."""
    prj = tmp_path / "p.prj"
    prj.write_text("#\n# xwpe - project-file: p.prj\nCMP=\tgcc\nCMPFLAGS=\t-g\nLDFLAGS=\t\n"
                   "EXENAME=\t%s\nLIBNAME=\t%s\nCMPSWTCH=\tgnu\n\nFILES=\tt.c \n"
                   % (EXECUTABLE, LIBRARY))
    (tmp_path / "xwperc").write_text("[Programming]\nProject : p.prj\n")
    return prj


def _variable(prj, name):
    for line in prj.read_text().splitlines():
        if line.startswith(name + "="):
            return line.split("\t", 1)[1].strip()
    return None


# @scenario SC-path-cuts-07x
def test_x11_move_offers_a_long_file_name_whole(long_named_file, tmp_path, xwpe):
    """Alt-M on a file with a 200 byte name and Enter leaves the name as it was
    (the field was filled with the first 128 bytes and Enter renamed the
    file to them)."""
    _sleep(SETTLE_S)
    xwpe.key("F3", delay=1.5)
    xwpe.key("Tab", delay=1.0)      # from Name to the Files list
    xwpe.key("alt+m", delay=1.0)
    xwpe.key("Return", delay=1.5)
    _alive(xwpe, "Move of a %d byte file name" % NAME_BYTES)
    assert long_named_file in os.listdir(str(tmp_path)), \
        "the file was renamed: %r" % sorted(len(n) for n in os.listdir(str(tmp_path)))


# @scenario SC-path-cuts-02x
def test_x11_project_options_keep_the_library_and_executable_whole(
        project_with_long_names, xwpe):
    """Project > Options then Save writes the 98 byte LIBNAME and the 150 byte
    EXENAME back whole (they were kept in 79 and 128 bytes)."""
    _sleep(SETTLE_S)
    xwpe.menu("p", "o", delay=1.5)
    xwpe.key("alt+s", delay=2.0)
    _alive(xwpe, "Project-Options Save with long names")
    assert _variable(project_with_long_names, "LIBNAME") == LIBRARY
    assert _variable(project_with_long_names, "EXENAME") == EXECUTABLE
