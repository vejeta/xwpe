"""The nine help pages the painter rows drive the editor with.

A help page is a text of marks the painter walks: a link mark opens a button,
a file link mark opens a button up to its colon and then the file name, an end
mark closes either, and the end of a page paints the rest of the row blank.
The nine pages are the shapes that walk can meet.  The terminal row
(tests/test_help_paint.py) reads the cells back; the graphical twins
(tests/x11/, tests/wayland/) read pixels, and only they run the box the
graphical painter draws round a link.  All three drive the editor with these
pages and no others, so a shape that breaks the walk breaks a row that can
see it.
"""

HBG = "\x01"            # start of a link
HNF = "\x03"            # start of a link into a file
HED = "\x05"            # end mark
HHD = "\x02"            # start of a headline
HBB = "\x0f"            # start of a highlight
HFE = "\x06"            # end of the page

MARKER = "PAGE-MARKER"  # the row the terminal row finds the page's rows by
LONG_MAXCOLUMN = 400    # a page row of 200 bytes stays one row

# name -> (the page's own row, MaxColumn or None for the editor's own)
PAGES = {
    "file-link-no-colon-no-end": (HNF + "abc", None),
    "file-link-no-colon-no-end-119": (HNF + "a" * 118, None),
    "file-link-colon-one-end": (HNF + "name:file" + HED + " tail", None),
    "file-link-colon-two-ends": (HNF + "name:file" + HED + "skip" + HED + "tail.x", None),
    "file-link-mark-last": ("text" + HNF, None),
    "link-no-end-short": (HBG + "link", None),
    "link-no-end-200": (HBG + "l" * 199, LONG_MAXCOLUMN),
    "file-link-200": (HNF + "a" * 100 + ":" + "b" * 90 + HED + "end", LONG_MAXCOLUMN),
    "headline-and-highlight": (HHD + "head" + HED + " " + HBB + "lit" + HED + " plain", None),
}

PAGE_NAMES = sorted(PAGES)


def page_text(own_row):
    """The help file whose first entry is own_row: the row itself, an empty
    row, and a marker row the terminal rows find the page's rows by."""
    return "%s\n\n%s\n%s\n" % (own_row, MARKER, HFE)


def options_text(maxcolumn):
    """The editor options of a page: MaxColumn wide enough to hold the page's
    row as one row, or the editor's own when that is wide enough."""
    if maxcolumn is None:
        return "[General]\n"
    return "[General]\nMaxColumn : %d\n" % maxcolumn
