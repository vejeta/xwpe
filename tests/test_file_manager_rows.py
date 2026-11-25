"""The File Manager's list shows true numbers for every name in a folder.

A link whose target is gone made stat fail, and the row was written from a
buffer stat never filled: a size of fourteen digits and a date of noise.
The row now shows the link itself, and a name that cannot be read at all
shows no size and a dashed date, never numbers it did not read.
"""
import os
import re

from wpe_driver import WpeSession

SEED = "int main(void){return 0;}\n"
F3 = "\033OR"
ESC = "\033"
COLS, ROWS = 120, 30
# A row's size column: the digits before the date (or the dashes).
SIZE_AND_DATE = re.compile(r"(\d+)\s+(\d\d\.\d\d\.\d{4}|--\.--\.----)")
# More digits than any file a test folder holds could have.
SIZE_DIGITS_MAX = 6


def _row_of(display, name):
    """The File Manager row that shows @name, or None."""
    for row in display:
        if name in row:
            return row
    return None


# @scenario SC-fm-typeahead-05
def test_a_dangling_link_is_listed_with_its_own_numbers(tmp_path):
    """F3 in a folder holding a link whose target is gone: the link's row
    shows a size and a date that were read, not what the row buffer held."""
    (tmp_path / "real.c").write_text("int r;\n")
    os.symlink(str(tmp_path / "gone-target"), str(tmp_path / "dangling.c"))
    with WpeSession(str(tmp_path), SEED, cols=COLS, rows=ROWS) as w:
        w.key(F3, delay=1.5)
        display = w.display()
        w.key(ESC, delay=0.5)
    row = _row_of(display, "dangling.c")
    assert row is not None, "the link is not listed:\n" + "\n".join(display)
    found = SIZE_AND_DATE.search(row.split("dangling.c", 1)[1])
    assert found, "the link's row has no size and date: %r" % row
    assert len(found.group(1)) <= SIZE_DIGITS_MAX, \
        "the link's size was never read: %r" % row
