"""A help page is painted cell by cell as it always was.

Help and Info pages are painted by the editor's plain line painter, which
walks each row of the page byte by byte: a link mark opens a button, a file
link mark opens a button up to its colon and then the name, an end mark closes
either, and the end of a page paints the rest blank.  The painter reads each
byte through the readers of we_rowread.h, and the loops that find the end of
a link stop at the end of the row's text whatever the marks hold.  The user
must see exactly what the page said: not a cell more, not a cell less, in the
colour of its kind.

Each row opens a crafted page with F1 (a help.xwpe found through XWPE_LIB),
reads the three rows of the page's first entry cell by cell (text, and the
foreground and background of each run), and compares them with the rows a
build of the tree before the readers painted.  The nine pages are the shapes
the walker can meet: a file link with no colon and no end mark, with one and
two end marks, a file link mark as the row's last byte, a link with no end
mark at 3 to 200 bytes, a headline and a highlight.  Each page is opened in
an 80 column terminal and in a 250 column one.

Run: WPE_BIN=$PWD/wpe tests/.venv/bin/python -m pytest -q tests/test_help_paint.py
"""
import itertools
import os

import pytest

from help_pages import MARKER, PAGE_NAMES, PAGES, options_text, page_text
from wpe_driver import WpeSession, wait_screen

F1 = "\033OP"
HELP_WAIT_S = 12
NARROW = 80
WIDE = 250
ROWS_OF_PAGE = 3        # the page's own row, an empty row, the marker row

# Colours of the help window, as the terminal shows them.
FRAME_EDGE = [["xx"], "white", "blue"]          # the window's left edge
SCROLL_BAR = "blue", "cyan"
PAGE_TEXT = "brown", "blue"
BUTTON = "brown", "green"
HEADLINE = "blue", "cyan"
HIGHLIGHT = "green", "blue"


def _run(pieces, colours):
    return [pieces, colours[0], colours[1]]


def _blank(count):
    return [(" ", count)]


# (page, columns) -> the runs of the page's own row between the window's edge
# and its scroll bar, as a build before the readers painted them: [text
# pieces, foreground, background], a piece being text or (character, count).
OWN_ROW = {
    ("file-link-200", NARROW): [_run([("a", 77)], BUTTON)],
    ("file-link-200", WIDE): [_run([("a", 100)], BUTTON),
                              _run([":", ("b", 90), (" ", 56)], PAGE_TEXT)],
    ("file-link-colon-one-end", NARROW): [_run(["name"], BUTTON),
                                          _run([":file", (" ", 68)], PAGE_TEXT)],
    ("file-link-colon-one-end", WIDE): [_run(["name"], BUTTON),
                                        _run([":file", (" ", 238)], PAGE_TEXT)],
    ("file-link-colon-two-ends", NARROW): [_run(["name"], BUTTON),
                                           _run([":file", (" ", 9), "x", (" ", 58)], PAGE_TEXT)],
    ("file-link-colon-two-ends", WIDE): [_run(["name"], BUTTON),
                                         _run([":file", (" ", 9), "x", (" ", 228)], PAGE_TEXT)],
    ("file-link-mark-last", NARROW): [_run(["text", (" ", 73)], PAGE_TEXT)],
    ("file-link-mark-last", WIDE): [_run(["text", (" ", 243)], PAGE_TEXT)],
    ("file-link-no-colon-no-end", NARROW): [_run(["abc"], BUTTON),
                                           _run(_blank(74), PAGE_TEXT)],
    ("file-link-no-colon-no-end", WIDE): [_run(["abc"], BUTTON),
                                          _run(_blank(244), PAGE_TEXT)],
    ("file-link-no-colon-no-end-119", NARROW): [_run([("a", 77)], BUTTON)],
    ("file-link-no-colon-no-end-119", WIDE): [_run([("a", 118)], BUTTON),
                                              _run(_blank(129), PAGE_TEXT)],
    ("headline-and-highlight", NARROW): [_run(["head"], HEADLINE),
                                         _run([" "], PAGE_TEXT),
                                         _run(["lit"], HIGHLIGHT),
                                         _run([" plain", (" ", 63)], PAGE_TEXT)],
    ("headline-and-highlight", WIDE): [_run(["head"], HEADLINE),
                                       _run([" "], PAGE_TEXT),
                                       _run(["lit"], HIGHLIGHT),
                                       _run([" plain", (" ", 233)], PAGE_TEXT)],
    ("link-no-end-200", NARROW): [_run([("l", 77)], BUTTON)],
    ("link-no-end-200", WIDE): [_run([("l", 199)], BUTTON),
                                _run(_blank(48), PAGE_TEXT)],
    ("link-no-end-short", NARROW): [_run(["link"], BUTTON),
                                    _run(_blank(73), PAGE_TEXT)],
    ("link-no-end-short", WIDE): [_run(["link"], BUTTON),
                                  _run(_blank(243), PAGE_TEXT)],
}

# What the window shows on the two rows after the page's own, which no page
# changes: the empty row and the marker row, in the window's width.
EDGE_ROOM = 3           # the left edge and the scroll bar take this much of a row


def _window_row(inner):
    return [FRAME_EDGE] + inner + [_run(["a"], SCROLL_BAR)]


def _expected_rows(name, columns):
    """The three rows the page shows: its own, an empty one, the marker."""
    own = [FRAME_EDGE] + OWN_ROW[(name, columns)] + [_run(["▮"], SCROLL_BAR)]
    empty = _window_row([_run(_blank(columns - EDGE_ROOM), PAGE_TEXT)])
    marker = _window_row([_run([MARKER, (" ", columns - EDGE_ROOM - len(MARKER))],
                               PAGE_TEXT)])
    return [own, empty, marker]


def _write(path, text):
    with open(path, "w") as fh:
        fh.write(text)


def _open_page(workdir, own_row, maxcolumn, columns):
    """An editor whose F1 opens the page, and F1 pressed."""
    _write(os.path.join(workdir, "help.xwpe"), page_text(own_row))
    _write(os.path.join(workdir, "xwperc"), options_text(maxcolumn))
    s = WpeSession(workdir, "int main;\n", cols=columns,
                   env_extra={"XWPE_LIB": workdir})
    s.key(F1, delay=1.0)
    return s


REPEAT_MIN = 4          # a character repeated this often is written as a count


def _compact(text):
    """text as pieces: a character repeated REPEAT_MIN times or more becomes
    (character, count), the rest stays text."""
    pieces = []
    for char, group in itertools.groupby(text):
        count = len(list(group))
        if count >= REPEAT_MIN:
            pieces.append((char, count))
        elif pieces and isinstance(pieces[-1], str):
            pieces[-1] += char * count
        else:
            pieces.append(char * count)
    return pieces


def _runs(cells, columns):
    """One screen row as runs of equal colours: [text pieces, foreground,
    background]."""
    runs = []
    for x in range(columns):
        cell = cells[x]
        if runs and runs[-1][1:] == [cell.fg, cell.bg]:
            runs[-1][0] += cell.data
        else:
            runs.append([cell.data, cell.fg, cell.bg])
    return [[_compact(run[0]), run[1], run[2]] for run in runs]


def page_rows(session, columns):
    """The rows of the page's first entry, ending at the marker row, each as
    its runs.  None when the marker is not on the screen."""
    if not wait_screen(session, MARKER, HELP_WAIT_S):
        return None
    shown = session.display(drain=0)
    marker_row = next(y for y, row in enumerate(shown) if MARKER in row)
    first = marker_row - (ROWS_OF_PAGE - 1)
    return [_runs(session.screen.buffer[y], columns)
            for y in range(first, marker_row + 1)]


# @scenario SC-row-readers-05
@pytest.mark.parametrize("columns", [NARROW, WIDE])
@pytest.mark.parametrize("name", PAGE_NAMES)
def test_help_page_is_painted_cell_by_cell(tmp_path, name, columns):
    """The page opens, the editor stays alive, and its rows are the rows the
    painter always drew."""
    own_row, maxcolumn = PAGES[name]
    with _open_page(str(tmp_path), own_row, maxcolumn, columns) as s:
        rows = page_rows(s, columns)
        assert s.alive(), "the editor died painting the page %s" % name
        assert rows is not None, "the page %s did not open" % name
        assert rows == _expected_rows(name, columns), \
            "the page %s at %d columns is painted differently" % (name, columns)
