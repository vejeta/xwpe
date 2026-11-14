"""The blank row that ends a project list is not an entry, in the graphical
editor.

Twin of tests/test_list_entry.py for the rows whose code is shared but whose
keys and drawing go another road here.  Each row reads the effect from the
.prj on disk after Project > Options > Save, which no backend draws: a Save
writes no blank variable, file or command, adds no install section to a
project without one, and a project that has an install section but no FILES
line opens and saves without the editor dying.

The wayland twin runs this same body.
"""
import pytest

from conftest import _sleep

SETTLE_S = 1.5
HEADER = "#\n# xwpe - project-file: p.prj\nCMP=\tgcc\nCMPFLAGS=\t-g\nLDFLAGS=\t\nEXENAME=\tp\nCMPSWTCH=\tgnu\n\n"


def _alive(xwpe, what):
    assert xwpe.proc.poll() is None, "xwpe died: " + what


def _project(tmp_path, body):
    """The start folder holds p.prj with HEADER and @body."""
    prj = tmp_path / "p.prj"
    prj.write_text(HEADER + body)
    return prj


def _options_save(xwpe, what):
    """Project > Open Project (pick p.prj), then Options > Save."""
    _sleep(SETTLE_S)
    xwpe.menu("p", "p", delay=1.5)
    xwpe.key("Return", delay=1.0)           # into the file list
    xwpe.key("Return", delay=2.0)           # open p.prj
    _alive(xwpe, "opening the project")
    xwpe.key("alt+o", delay=1.5)            # the Project window's Options
    xwpe.key("alt+s", delay=2.0)
    _alive(xwpe, what)


def _install_lines(text):
    lines = text.split("\n")
    if "install:" not in lines:
        return None
    return [ln for ln in lines[lines.index("install:") + 1:] if ln != ""]


# @scenario SC-list-entry-06x
def test_x11_save_writes_no_blank_variable_file_or_command(tmp_path, xwpe):
    """A project with a variable, a file and an install command: Save keeps
    those and writes no blank line, no blank FILES word, no blank command."""
    prj = _project(tmp_path, "FOO=\tbar\nFILES=\tt.c\n\ninstall:\n\tmake install\n")
    _options_save(xwpe, "Project-Options Save")
    text = prj.read_text()
    lines = text.split("\n")
    assert not [ln for ln in lines if ln.strip() == "" and ln != ""], text
    assert [ln for ln in lines if ln.startswith("FILES=")] == ["FILES=\tt.c "], text
    assert _install_lines(text) == ["\tmake install"], text
    assert "FOO=bar" in lines, text


# @scenario SC-list-entry-07x
def test_x11_save_adds_no_install_section(tmp_path, xwpe):
    """A project without an install section has none after Save."""
    prj = _project(tmp_path, "FILES=\tt.c\n")
    _options_save(xwpe, "Project-Options Save")
    assert "install:" not in prj.read_text(), prj.read_text()


# @scenario SC-list-entry-05x
def test_x11_project_with_install_and_no_files_line_saves(tmp_path, xwpe):
    """Opening the options of a project that has an install section and no
    FILES line, then Save, keeps the install command and writes no file."""
    prj = _project(tmp_path, "install:\n\tmake install\n")
    _options_save(xwpe, "Project-Options on a project with no FILES line")
    text = prj.read_text()
    assert _install_lines(text) == ["\tmake install"], text
    assert [ln for ln in text.split("\n") if ln.startswith("FILES=")] == ["FILES=\t"], text
