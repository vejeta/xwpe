"""Ctrl-O C checks a help page for switches without headers.

The author of a help page presses Ctrl-O C in the page's window.  The editor
reads the name after every link mark and every headline mark, up to the end
mark that closes it, and compares the two lists.  A name with no end mark is
reported at once and the cursor stands where the end mark belongs, so the
author sees which one to fix.  The rows are read through the readers of
we_rowread.h: the name's end is found in the row's own bytes and never past
them.

Each row opens a crafted page with F1 (a help.xwpe found through XWPE_LIB) and
presses Ctrl-O then C.

Run: WPE_BIN=$PWD/wpe tests/.venv/bin/python -m pytest -q tests/test_help_check.py
"""
import os
import re

from wpe_driver import WpeSession, wait_screen

HBG = "\x01"            # start of a link (a switch)
HED = "\x05"            # end mark
HHD = "\x02"            # start of a headline (a header)
HFE = "\x06"            # end of the page
F1 = "\033OP"
CTRL_O = "\x0f"
ENTER = "\r"
HELP_WAIT_S = 12
MARKER = "PAGE-MARKER"
STATUS_POSITION = re.compile(r"q\s*(\d+):(\d+)\s")


def _open_page(workdir, rows):
    """An editor whose F1 opens the page made of @rows, and F1 pressed."""
    with open(os.path.join(workdir, "help.xwpe"), "w") as fh:
        fh.write("\n".join(rows) + "\n%s\n%s\n" % (MARKER, HFE))
    with open(os.path.join(workdir, "xwperc"), "w") as fh:
        fh.write("[General]\n")
    session = WpeSession(workdir, "int main;\n", env_extra={"XWPE_LIB": workdir})
    session.key(F1, delay=1.0)
    assert wait_screen(session, MARKER, HELP_WAIT_S), "the page did not open"
    return session


def _check(session):
    session.key(CTRL_O, "C", delay=1.0)
    return "\n".join(session.display(drain=0.5))


def _position(session):
    """The line and column the status line shows."""
    for row in session.display(drain=0.3):
        found = STATUS_POSITION.search(row)
        if found:
            return int(found.group(1)), int(found.group(2))
    return None


# @scenario SC-row-readers-06
def test_a_switch_and_its_header_pass(tmp_path):
    s = _open_page(str(tmp_path), [HBG + "abc" + HED + " text", HHD + "abc" + HED + " head"])
    try:
        shown = _check(s)
        assert "Error" not in shown and "no Header" not in shown
    finally:
        s.close()


# @scenario SC-row-readers-07
def test_a_switch_without_a_header_is_named(tmp_path):
    s = _open_page(str(tmp_path), [HBG + "abc" + HED + " text", HHD + "xyz" + HED + " head"])
    try:
        assert "Switch 'abc' has no Header!" in _check(s)
    finally:
        s.close()


# @scenario SC-row-readers-08
def test_a_switch_name_stops_at_its_end_mark_not_at_the_row_end(tmp_path):
    s = _open_page(str(tmp_path), [HBG + "ab" + HED + "cd" + HED + " t", HHD + "ab" + HED + " h"])
    try:
        shown = _check(s)
        assert "Error" not in shown and "no Header" not in shown
    finally:
        s.close()


# @scenario SC-row-readers-09
def test_a_switch_without_its_end_mark_puts_the_cursor_where_it_belongs(tmp_path):
    s = _open_page(str(tmp_path), [HBG + "abc", HHD + "abc" + HED])
    try:
        assert "Error in Switch!" in _check(s)
        s.key(ENTER, delay=0.8)
        # the page's own text starts on its second row, and the name is three
        # bytes after the mark: the end mark belongs four bytes into the row,
        # which the status line shows as column 4 once the mark takes no cell
        assert _position(s) == (2, 4)
    finally:
        s.close()


# @scenario SC-row-readers-10
def test_a_header_without_its_end_mark_is_reported_at_its_row(tmp_path):
    s = _open_page(str(tmp_path), ["text", HHD + "headx"])
    try:
        assert "Error in Header!" in _check(s)
        s.key(ENTER, delay=0.8)
        # the headline is the third row, its name five bytes after the mark
        assert _position(s) == (3, 6)
    finally:
        s.close()
