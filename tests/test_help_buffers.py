"""Help viewer and Info reader: a link, header or node name longer than the
reader's buffers is safe (we_fl_fkt.c, we_helpbuf.c).

The Help viewer (F1) and the Info reader (Help > Info) copy a link name, a
header, a node name or a file name out of a row into storage of a fixed size.
Info files come from the system and the user, so a name can be longer than
the reader expected.  These rows build the long names the old code wrote past
its buffers with, open them the way a user does, and read that the editor is
alive and shows the page the name leads to.

Driving conditions that every row depends on (each one hid a failure once):
  * the xwperc is written in the working folder and starts with [General];
    an option before any section header kills the editor (a separate defect
    of the option reader, not of these rows);
  * INFOPATH is removed from the editor's environment: it overrides the
    InfoDir option after the options are read, and the compiled-in info
    directory would open the installed manual instead of the crafted file;
  * the crafted Info file is named xwpe.info, the file Help > Info opens;
  * a crafted help.xwpe is found through XWPE_LIB pointing at the folder;
  * F1 opens the help on an empty first row, so Down goes onto the link
    before Enter.

The red/green proof of each row is under a sanitizer build launched as
wpe-asan (never we-asan, under which the help layer is off):
ASAN_OPTIONS=allow_user_segv_handler=0:detect_leaks=0:abort_on_error=1.

The man-page button (HFB) lives in a man window, the one place a row is as
wide as MaxColumn: its row opens a crafted page through MANPATH with Help >
Topic Search, and skips when man is not installed.

Run: WPE_BIN=$PWD/wpe tests/.venv/bin/python -m pytest -q tests/test_help_buffers.py
"""
import os
import shutil

import pytest

from wpe_driver import ALT, WpeSession, wait_screen

HBG = "\x01"            # start of a link
HNF = "\x03"            # start of a link into another Info file
HED = "\x05"            # end mark
HHD = "\x02"            # start of a page's headline
HFE = "\x06"            # end of the page
IFE = "\x1f"            # separates the nodes of an Info file
DEL = "\x7f"            # between a node's name and its line in the tag table
HOME_KEY = "\033[H"
DOWN = "\033[B"
RIGHT = "\033[C"
ALT_T = "\033t"         # Next page, in the Info reader and in Help
F1 = "\033OP"
HELP_WAIT_S = 12


@pytest.fixture(autouse=True)
def _info_search_unset(monkeypatch):
    """The editor's Info search path comes from the file, not the host."""
    monkeypatch.delenv("INFOPATH", raising=False)
    monkeypatch.delenv("XWPE_LIB", raising=False)


def _write(path, text):
    with open(path, "w") as fh:
        fh.write(text)


def _write_xwperc(workdir, maxcol=None, infodir=None):
    lines = ["[General]"]
    if maxcol is not None:
        lines.append("MaxColumn : %d" % maxcol)
    if infodir is not None:
        lines.append("InfoDir : %s" % infodir)
    _write(os.path.join(workdir, "xwperc"), "\n".join(lines) + "\n")


def _help_session(workdir, help_text, maxcol=None):
    """An editor whose F1 opens help_text, and F1 pressed."""
    _write(os.path.join(workdir, "help.xwpe"), help_text)
    _write_xwperc(workdir, maxcol=maxcol)
    s = WpeSession(workdir, "int main;\n", env_extra={"XWPE_LIB": workdir})
    s.key(F1, delay=1.0)
    return s


def _info_session(workdir, info_text, subfiles=None, maxcol=None):
    """An editor whose Help > Info opens info_text, as xwpe.info."""
    infodir = os.path.join(workdir, "info")
    os.makedirs(infodir)
    _write(os.path.join(infodir, "xwpe.info"), info_text)
    for name, text in (subfiles or {}).items():
        _write(os.path.join(infodir, name), text)
    _write_xwperc(workdir, maxcol=maxcol, infodir=infodir)
    return WpeSession(workdir, "int main;\n")


def _open_info(s):
    s.key(ALT.HELP, "i", delay=1.0)


def _node(name, fields, body):
    """One node of an Info file: its separator, header line and text."""
    return "%s\nFile: xwpe.info,  Node: %s,  %s\n\n%s\n" % (IFE, name, fields, body)


def _shows(s, needle, tmo=HELP_WAIT_S):
    return wait_screen(s, needle, tmo)


def _text(s):
    return "\n".join(s.display(drain=0))


# @scenario SC-help-buffers-01
@pytest.mark.parametrize("length", [5, 124, 200])
def test_long_link_in_a_help_page_is_followed(tmp_path, length):
    """A help link of 124 or 200 bytes (MaxColumn 400 keeps the row whole)
    jumps to its page; a 5 byte link is the control that always did."""
    name = "a" * length
    page = ("%s%s%s first link\n\nFIRST-PAGE-MARKER\n%s\n"
            "%s%s%s Target title\nBODY-ONE-MARKER\n%s\n"
            % (HBG, name, HED, HFE, HHD, name, HED, HFE))
    with _help_session(str(tmp_path), page, maxcol=400) as s:
        assert _shows(s, "FIRST-PAGE-MARKER"), _text(s)
        assert "BODY-ONE-MARKER" not in _text(s)
        s.key(DOWN, HOME_KEY, delay=0.6)
        s.key("\r", delay=1.0)
        assert s.alive(), "wpe died following a %d byte help link" % length
        assert _shows(s, "BODY-ONE-MARKER"), _text(s)


# @scenario SC-help-buffers-02
def test_long_heading_of_a_help_page_is_shown(tmp_path):
    """A page whose heading line is 200 bytes opens (the default MaxColumn
    makes the window's rows 122 bytes) and shows the lines after it."""
    page = ("%sT%s go to the long one\n\nFIRST-PAGE-MARKER\n%s\n"
            "%sT%s %s\nBODY-TWO-MARKER\n%s\n"
            % (HBG, HED, HFE, HHD, HED, "h" * 200, HFE))
    with _help_session(str(tmp_path), page) as s:
        assert _shows(s, "FIRST-PAGE-MARKER"), _text(s)
        assert "BODY-TWO-MARKER" not in _text(s)
        s.key(DOWN, HOME_KEY, delay=0.6)
        s.key("\r", delay=1.0)
        assert s.alive(), "wpe died opening a page with a 200 byte heading"
        assert _shows(s, "BODY-TWO-MARKER"), _text(s)


# @scenario SC-help-buffers-03
def test_info_node_with_a_200_byte_next_name_opens(tmp_path):
    """A node whose Next: name is 200 bytes opens and shows its text."""
    top = _node("Top", "Next: %s,  Up: (dir)" % ("n" * 200), "TOP-THREE-MARKER")
    with _info_session(str(tmp_path), "This is xwpe.info.\n" + top) as s:
        _open_info(s)
        assert s.alive(), "wpe died opening a node with a 200 byte Next name"
        assert _shows(s, "TOP-THREE-MARKER"), _text(s)


# @scenario SC-help-buffers-05
@pytest.mark.parametrize("length", [122, 125, 127])
def test_next_node_with_a_long_name_is_opened(tmp_path, length):
    """Next on a node whose neighbour's name is 122 to 127 bytes opens that
    neighbour: the name is looked up whole, not in a 128 byte buffer."""
    name = "m" * length
    top = _node("Top", "Next: %s,  Up: (dir)" % name, "TOP-FIVE-MARKER")
    nxt = _node(name, "Prev: Top,  Up: Top", "NEXT-FIVE-MARKER")
    with _info_session(str(tmp_path), "This is xwpe.info.\n" + top + nxt) as s:
        _open_info(s)
        assert _shows(s, "TOP-FIVE-MARKER"), _text(s)
        s.key(ALT_T, delay=1.0)
        assert s.alive(), "wpe died on Next to a %d byte node name" % length
        assert _shows(s, "NEXT-FIVE-MARKER"), _text(s)


# @scenario SC-help-buffers-06
def test_indirect_subfile_with_a_200_byte_name_is_opened(tmp_path):
    """An indirect Info file whose table names a 200 byte subfile opens the
    node that subfile holds."""
    sub = "s" * 200
    main = ("This is xwpe.info, an indirect file.\n%s\nIndirect:\n%s: 0\n"
            "%s\nTag table:\n(Indirect)\nNode: Top%s0\n%s\nEnd tag table\n"
            % (IFE, sub, IFE, DEL, IFE))
    subtext = _node("Top", "Up: (dir)", "SUB-SIX-MARKER")
    with _info_session(str(tmp_path), main, {sub: subtext}) as s:
        _open_info(s)
        assert s.alive(), "wpe died opening an indirect file with a 200 byte name"
        assert _shows(s, "SUB-SIX-MARKER"), _text(s)


# @scenario SC-help-buffers-08
def test_info_menu_link_to_a_long_file_name_is_followed(tmp_path):
    """A menu entry "* x: (FILE)node." with a file name over 125 bytes opens
    that file's node."""
    other = "f" * 130
    top = _node("Top", "Up: (dir)",
                "* Menu:\n\n* x: (%s)Top.  The long one.\nMENU-EIGHT-MARKER" % other)
    target = "This is %s.info.\n" % other + _node("Top", "Up: (dir)", "FILE-EIGHT-MARKER")
    with _info_session(str(tmp_path), "This is xwpe.info.\n" + top,
                       {other + ".info": target}, maxcol=400) as s:
        _open_info(s)
        assert _shows(s, "MENU-EIGHT-MARKER"), _text(s)
        row = next(y for y, line in enumerate(s.display(drain=0)) if "x:" in line)
        s.key(*([DOWN] * (row - _cursor_row(s))), delay=0.4)
        s.key(RIGHT, RIGHT, RIGHT, delay=0.4)
        s.key("\r", delay=1.0)
        assert s.alive(), "wpe died following a link to a 130 byte file name"
        assert _shows(s, "FILE-EIGHT-MARKER"), _text(s)


def _cursor_row(s):
    """The screen row the editor's cursor is on."""
    return s.screen.cursor.y


# @scenario SC-help-buffers-09
def test_indirect_table_without_entries_does_not_crash(tmp_path):
    """An Indirect: block with no entries, then a tag table, leaves the
    editor alive (it read the first entry of an empty list)."""
    main = ("This is xwpe.info, an indirect file.\n%s\nIndirect:\n"
            "%s\nTag table:\n(Indirect)\nNode: Top%s0\n%s\nEnd tag table\n"
            % (IFE, IFE, DEL, IFE))
    with _info_session(str(tmp_path), main) as s:
        _open_info(s)
        assert s.alive(), "wpe died on an Indirect: block with no entries"
        s.key(DOWN, delay=0.5)
        assert s.alive()


# @scenario SC-help-buffers-10
def test_node_header_with_a_next_prefixed_file_name_does_not_hang(tmp_path):
    """A header "File: Nextfile.info,  Node: Top,  Next: B" opens, and Next
    goes to B.  A word that only starts with Next once held the editor in a
    loop; this row fails by its screen wait, never by hanging the suite."""
    top = ("%s\nFile: Nextfile.info,  Node: Top,  Next: B,  Up: (dir)\n\n"
           "TOP-TEN-MARKER\n" % IFE)
    b = _node("B", "Prev: Top,  Up: Top", "B-TEN-MARKER")
    with _info_session(str(tmp_path), "This is xwpe.info.\n" + top + b) as s:
        _open_info(s)
        assert _shows(s, "TOP-TEN-MARKER"), "the editor did not open the node:\n" + _text(s)
        s.key(ALT_T, delay=1.0)
        assert _shows(s, "B-TEN-MARKER"), _text(s)


# @scenario SC-help-buffers-13
def test_indirect_tag_of_a_longer_node_name_is_not_taken(tmp_path):
    """In an indirect file whose tag table lists "Topaz" before "Top", Help >
    Info opens Top: the line number is read from Top's own tag row, not from
    a row whose name only starts with Top."""
    main = ("This is xwpe.info, an indirect file.\n%s\nIndirect:\n"
            "subone: 0\nsubtwo: 1000\n%s\nTag table:\n(Indirect)\n"
            "Node: Topaz%s1500\nNode: Top%s10\n%s\nEnd tag table\n"
            % (IFE, IFE, DEL, DEL, IFE))
    subfiles = {"subone": _node("Top", "Up: (dir)", "TOP-NODE-MARKER"),
                "subtwo": _node("Topaz", "Up: Top", "TOPAZ-NODE-MARKER")}
    with _info_session(str(tmp_path), main, subfiles) as s:
        _open_info(s)
        assert _shows(s, "NODE-MARKER"), _text(s)
        assert "TOP-NODE-MARKER" in _text(s), _text(s)


def _man_page(mandir, name, body):
    """A section 1 manual page called name under mandir/man1."""
    os.makedirs(os.path.join(mandir, "man1"), exist_ok=True)
    _write(os.path.join(mandir, "man1", name + ".1"),
           ".TH %s 1\n.SH NAME\n%s \\- page of a test\n.SH DESCRIPTION\n%s\n"
           % (name.upper(), name, body))


def _onto_row(s, top, needle):
    """Put the cursor inside the button on the screen row holding needle.
    A man window opens with the cursor on its first row, the one holding
    top, so the rows between the two are the steps down."""
    lines = s.display(drain=0)
    first = next(y for y, line in enumerate(lines) if top in line)
    row = next(y for y, line in enumerate(lines) if needle in line)
    s.key(*([DOWN] * (row - first)), delay=0.4)
    s.key(HOME_KEY, *([RIGHT] * 8), delay=0.4)


# @scenario SC-help-buffers-12
@pytest.mark.skipif(shutil.which("man") is None, reason="needs man")
@pytest.mark.parametrize("which", ["long", "short"])
def test_man_page_button_with_a_long_name_is_refused(tmp_path, which):
    """In a man window (rows as wide as MaxColumn) a SEE ALSO button of 300
    bytes is refused with a message, not followed to a page named by its
    first bytes; a short button beside it still opens its page."""
    workdir = str(tmp_path)
    mandir = os.path.join(workdir, "man")
    long_name = "z" * 300           # not q: the window frame draws with q
    _man_page(mandir, "vfbpage", 'MAN-BODY-MARKER\n.SH "SEE ALSO"\n'
              "%s(1), vfbother(1)" % long_name)
    _man_page(mandir, "vfbother", "OTHER-PAGE-MARKER")
    _write_xwperc(workdir, maxcol=400)
    env = {"MANPATH": mandir, "MANWIDTH": "80"}
    with WpeSession(workdir, "vfbpage\n", env_extra=env) as s:
        s.key(ALT.HELP, "t", delay=2.0)
        assert _shows(s, "MAN-BODY-MARKER"), _text(s)
        if which == "long":
            _onto_row(s, "VFBPAGE(1) ", "zzzzzzzz")
        else:
            _onto_row(s, "VFBPAGE(1) ", "vfbother(1)")
        s.key("\r", delay=2.0)
        assert s.alive(), "wpe died on a %s man page button" % which
        if which == "long":
            assert _shows(s, "Link name too long"), _text(s)
            assert "No manual entry" not in _text(s)
        else:
            assert _shows(s, "OTHER-PAGE-MARKER"), _text(s)
