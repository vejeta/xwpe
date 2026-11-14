"""The option array's factors, and one row of it as a session.

A row picks one level of every factor: the key style, the block mode, the
insert mode, the auto-indent mode, the window's width (and whether Max.
Columns changes after the window opened), the options dialog's numbers,
autosave, the kind of file, the breakpoints, what the clipboard holds,
Alt-J, and whether the file is read-only.  ``Row.xwperc()`` is the options
file it starts with, ``Row.prelude()`` the actions that bring a started
session to the row's state (the dialog, the breakpoints, the clipboard), and
``Row.levels()`` the labels the coverage report counts under.

A label is a promise about the session the row starts, and the array is only
worth its size if the promise is kept.  ``Row.constraints`` below are the
combinations no session can have (the clipboard is filled from the file, a
locked window refuses Insert and Alt-J, auto-indent from the file's kind
needs a source file), and ``observed_levels`` / ``start_mismatches`` read the
levels a started session really has from the editor's dump and the screen,
so a row that did not get its label says so at its start.

The levels the editor's harness cannot drive yet are in ``PENDING``: a
second window, the Show Buffer viewer, CRLF files.  They are not in any row
and the coverage report lists them as not built, never as covered.
"""
# Standard library only: the coverage report and the array generator read
# this module without a terminal emulator installed.

# name -> levels, in the order the TSV lists them.
FACTORS = {
    "keys": ("old", "cua"),
    "block": ("modern", "wordstar", "selection"),
    "ins": ("insert", "overwrite"),
    "autoindent": ("source", "always", "never"),
    # Max. Columns: the width the window opens with (118, 40), or the width
    # it opens with and the one the dialog sets afterwards (lowered: 118 then
    # 40, raised: 60 then 118).  One factor, so no level overrides another.
    "width": ("118", "40", "lowered", "raised"),
    "tabn": ("8", "4", "1"),
    "numundo": ("10", "2", "0"),
    "autosave": ("off", "changes"),
    "endmark": ("off", "on"),
    "file": ("mixed.c", "mixed.txt", "empty.c", "empty.txt", "oneline.c", "oneline.txt"),
    "breakpoints": ("none", "two"),
    "clipboard": ("empty", "oneline", "multiline"),
    "alt_j": ("off", "on"),
    "window": ("file", "readonly"),
    "windows": ("one",),
    # Options > Editor > Trim Auto-indent, and Keep Final newline
    "trim": ("off", "on"),
    "ending": ("add", "keep"),
}
# Levels the design names and the harness cannot drive yet.
PENDING = {
    "windows": ("second_narrower", "second_same"),
    "window": ("show_buffer",),
    "file": ("crlf.c", "crlf.txt"),
}
# Factors whose combinations are covered three at a time; every other pair
# of factors is covered two at a time.
STRENGTH_3 = (("keys", "block", "ins", "autoindent"),
              ("width", "file", "clipboard", "windows"),
              ("autoindent", "trim", "keys"))

# Max. Columns when the window opens before the dialog changes it.
WIDTH_RAISED_START = 60
WIDTH_LOWERED_TO = 40
WIDTH_RAISED_TO = 118
LOWERED_START = 118
# Max. Changes of a row with autosave: an autosave after the sixth change.
AUTOSAVE_MAXCHG = 5
AUTOSAVE_CHANGES_BIT = 2
# Options2's bits (options.h) a row sets: the programming layer's two
# always, and the rest by level.
OPTIONS2_BASE = 8 + 16
OPT_CUA = 1
OPT_ENDMARK = 2
OPT_WORDSTAR = 4
OPT_SELECTION = 0x400
OPT_TRIM = 0x800
OPT_KEEP_ENDING = 0x1000
OPT_AUTOINDENT_SOURCE = 128
OPT_AUTOINDENT_ALWAYS = 256
READONLY_MODE = 0o444

# The file's text by level.  The mixed file has a row near the default
# width, a tab, a multibyte letter, a Latin-1 byte, a row of blanks, a
# help-file marker byte, more rows than the window shows and no final line
# break.
NEAR_WIDTH_ROW = " ".join("w%03d" % i for i in range(23)) + "xy"


def _mixed_text():
    rows = [b"#include <stdio.h>", b"int main(void)", b"{", b"    int a = 1;",
            b"\tif (a) {", b"\t\treturn 0;", b"\t}", b"}",
            NEAR_WIDTH_ROW.encode(), "café au lait".encode(), b"   ",
            b"one-two three", b"caf\xe9 table", b"\x06 end of page"]
    rows += [b"filler row %02d with some words" % i for i in range(16)]
    rows.append(b"last line")
    return b"\n".join(rows)


FILE_TEXT = {
    "mixed": _mixed_text(),
    "empty": b"",
    "oneline": b"only one line here",
}

# Keys that belong to the programming layer, used only to put breakpoints
# on a row's file: the debugger's prefix and B for toggle.
BREAKPOINT_KEYS = ["\x07", "b"]
# The file level the breakpoints go in (a C file with enough rows).
BREAKPOINT_FILE = "mixed.c"
# The rows of the file the breakpoints go on, 0-based.
BREAKPOINT_ROWS = (1, 3)
# Clipboard preludes, as the model's actions.
COPY_ONE_LINE = (("block_begin", None), ("end", None), ("block_end", None),
                 ("edit_copy", None), ("block_hide", None), ("file_start", None))
COPY_SEVERAL_LINES = (("block_begin", None), ("down", None), ("down", None),
                      ("end", None), ("block_end", None), ("edit_copy", None),
                      ("block_hide", None), ("file_start", None))
# The same blocks marked with Shift: in the selection block mode a move
# without Shift collapses the block Ctrl-K B began, so the copy above would
# copy nothing and the row would start with an empty clipboard.
SELECT_ONE_LINE = (("shift_end", None), ("edit_copy", None), ("block_hide", None),
                   ("file_start", None))
SELECT_SEVERAL_LINES = (("shift_down", None), ("shift_down", None), ("shift_end", None),
                        ("edit_copy", None), ("block_hide", None), ("file_start", None))


class Row:
    """One row of the array: a level for every factor."""

    def __init__(self, levels, name="row"):
        self.name = name
        self.lv = dict(levels)
        for f, options in FACTORS.items():
            assert self.lv.get(f) in options, (f, self.lv.get(f))

    def levels(self):
        """{factor: level}, for the coverage report."""
        return dict(self.lv)

    @property
    def style(self):
        return self.lv["keys"]

    @property
    def filename(self):
        return "t." + self.lv["file"].split(".")[1]

    @property
    def seed_text(self):
        return FILE_TEXT[self.lv["file"].split(".")[0]]

    @property
    def file_mode(self):
        return READONLY_MODE if self.lv["window"] == "readonly" else None

    def start_width(self):
        """The width the window opens with (the options file's Max. Columns)."""
        wc = self.lv["width"]
        if wc == "raised":
            return WIDTH_RAISED_START
        if wc == "lowered":
            return LOWERED_START
        return int(wc)

    def options2(self):
        v = OPTIONS2_BASE
        if self.lv["keys"] == "cua":
            v += OPT_CUA
        if self.lv["endmark"] == "on":
            v += OPT_ENDMARK
        if self.lv["block"] == "wordstar":
            v += OPT_WORDSTAR
        elif self.lv["block"] == "selection":
            v += OPT_SELECTION
        if self.lv["trim"] == "on":
            v += OPT_TRIM
        if self.lv["ending"] == "keep":
            v += OPT_KEEP_ENDING
        ai = self.lv["autoindent"]
        if ai == "source":
            v += OPT_AUTOINDENT_SOURCE
        elif ai == "always":
            v += OPT_AUTOINDENT_ALWAYS
        return v

    def xwperc(self):
        """The options file the row's session starts with."""
        lines = ["[General]",
                 "Options2 : %d" % self.options2(),
                 "MaxColumn : %d" % self.start_width(),
                 "Tab : %s" % self.lv["tabn"],
                 "NumUndo : %s" % self.lv["numundo"]]
        if self.lv["autosave"] == "changes":
            lines.append("Autosave : %d" % AUTOSAVE_CHANGES_BIT)
            lines.append("MaxChanges : %d" % AUTOSAVE_MAXCHG)
        return "\n".join(lines) + "\n"

    def prelude(self):
        """[(keymap action, argument)] that bring a started session to the
        row's state.  The breakpoints are keys of the programming layer, so
        they are the pseudo-action ``toggle_breakpoint`` (rows.BREAKPOINT_KEYS)."""
        out = []
        wc = self.lv["width"]
        if wc == "lowered":
            out.append(("option_set", ("maxcol", WIDTH_LOWERED_TO)))
        elif wc == "raised":
            out.append(("option_set", ("maxcol", WIDTH_RAISED_TO)))
        out.extend(self._clipboard_prelude())
        out.extend(self._breakpoint_prelude())
        # the modes last: in Alt-J's mode the keys above would be typed
        if self.lv["ins"] == "overwrite":
            out.append(("insert_toggle", None))
        if self.lv["alt_j"] == "on":
            out.append(("literal_toggle", None))
        return out

    def _clipboard_prelude(self):
        level = self.lv["clipboard"]
        selection = self.lv["block"] == "selection"
        if level == "oneline" and selection:
            return list(SELECT_ONE_LINE)
        if level == "oneline":
            return list(COPY_ONE_LINE)
        if level == "multiline" and selection:
            return list(SELECT_SEVERAL_LINES)
        if level == "multiline":
            return list(COPY_SEVERAL_LINES)
        return []

    def _breakpoint_prelude(self):
        if self.lv["breakpoints"] != "two":
            return []
        out = [("file_start", None)]
        row = 0
        for target in BREAKPOINT_ROWS:
            while row < target:
                out.append(("down", None))
                row += 1
            out.append(("toggle_breakpoint", None))
        out.append(("file_start", None))
        return out

    def __repr__(self):
        return "Row(%s)" % self.name


def _is_source(file_level):
    return file_level.endswith(".c")


def _has_a_row_to_copy(file_level):
    return file_level.startswith(("mixed", "oneline"))


def _has_rows_to_copy(file_level):
    return file_level.startswith("mixed")


# Each constraint: (name, the two factors it relates, the test of a pair of
# levels that cannot be had together).  Every one is a fact about the editor
# or the prelude, not a convenience.
CONSTRAINTS = (
    # the editor sets breakpoints in C files, on rows that exist
    ("breakpoints need the mixed C file", ("breakpoints", "file"),
     lambda b, f: b == "two" and f != BREAKPOINT_FILE),
    # the clipboard is filled by copying a block of the file
    ("a one-line clipboard needs a line to copy", ("clipboard", "file"),
     lambda c, f: c == "oneline" and not _has_a_row_to_copy(f)),
    ("a multi-line clipboard needs several lines to copy", ("clipboard", "file"),
     lambda c, f: c == "multiline" and not _has_rows_to_copy(f)),
    # a locked window refuses Insert and Alt-J, so neither mode can be set
    ("a locked window cannot be put in overwrite mode", ("window", "ins"),
     lambda w, i: w == "readonly" and i != "insert"),
    ("a locked window cannot be put in Alt-J's mode", ("window", "alt_j"),
     lambda w, a: w == "readonly" and a != "off"),
    # auto-indent by the file's kind is on for source files only: for a text
    # file it is the same session as "never", so the level would be a label
    ("auto-indent by the file's kind needs a source file", ("autoindent", "file"),
     lambda a, f: a == "source" and not _is_source(f)),
)


def violates(levels):
    """Does a (possibly partial) assignment break a constraint of the array?
    See ``CONSTRAINTS``: each says why a pair of levels cannot be had in one
    session."""
    for _name, (f1, f2), test in CONSTRAINTS:
        if f1 in levels and f2 in levels and test(levels[f1], levels[f2]):
            return True
    return False


# ---- what a started session really has -----------------------------------------------

# A hard row shows ``$`` after its text with Show Endmark, a row that goes on
# in the next one ``\``: the first row of every file shows one or the other.
ENDMARK_CHARS = ("$", "\\")
# The first text row of the window is the row after its title row.
FIRST_TEXT_ROW_OFFSET = 1
# The mixed file's row with a tab at its start, and what follows the tab.
TAB_ROW = 4
TAB_ROW_TEXT = "if (a) {"


def _file_kind(lines):
    """empty / oneline / mixed, from the window's rows."""
    if all(not data for _end, data in lines):
        return "empty"
    return "oneline" if len(lines) == 1 else "mixed"


def _clipboard_kind(rows):
    """empty / oneline / multiline, from the clipboard window's rows."""
    filled = [data for _end, data in rows if data]
    if not filled:
        return "empty"
    return "oneline" if len(rows) == 1 else "multiline"


def observed_levels(win, clip, display, tabn_screen=True):
    """The levels of every factor a started session shows, read from the
    editor's own dump of the file window (@win), of window 0 (@clip) and the
    screen (@display): {factor: level}.  Auto-indent is "on" or "off", the
    others in the array's own words.  A factor the dump and the screen cannot
    show (the key style, the block mode, Num. Undo, Autosave, and Tabstops
    on a file with no tab) is not in the answer."""
    out = {}
    out["ins"] = "insert" if (win.ins & 1 or win.ins == INS_LOCKED) else "overwrite"
    out["alt_j"] = "on" if win.ins & 2 else "off"
    out["autoindent"] = "on" if win.flg & 1 else "off"
    out["window"] = "readonly" if win.ro else "file"
    out["width"] = win.mx
    out["file"] = _file_kind(win.lines) + "." + win.name.rsplit(".", 1)[-1]
    out["breakpoints"] = "two" if len(win.brp) == 2 else ("none" if not win.brp else "other")
    out["clipboard"] = _clipboard_kind(list(clip.lines) if clip is not None else [])
    cy, cx, ay, ax, ey, ex = win.view
    first = display[ay + FIRST_TEXT_ROW_OFFSET][ax + 1:ex].rstrip() if display else ""
    out["endmark"] = "on" if first.endswith(ENDMARK_CHARS) else "off"
    if tabn_screen and win.lines and len(win.lines) > TAB_ROW and display \
            and win.lines[TAB_ROW][1].startswith(b"\t"):
        row = display[ay + FIRST_TEXT_ROW_OFFSET + TAB_ROW][ax + 1:ex]
        at = row.find(TAB_ROW_TEXT)
        if at >= 0:
            out["tabn"] = at
    return out


# The window's mode word for a window closed to changes.
INS_LOCKED = 8


def expected_levels(row):
    """What ``observed_levels`` must read at the start of @row's session."""
    lv = row.lv
    want = {"ins": lv["ins"], "alt_j": lv["alt_j"], "window": lv["window"],
            "width": row.start_width(), "file": lv["file"],
            "breakpoints": lv["breakpoints"], "clipboard": lv["clipboard"],
            "endmark": lv["endmark"]}
    if lv["autoindent"] == "never":
        want["autoindent"] = "off"
    else:
        want["autoindent"] = "on"
    if lv["file"].startswith("mixed"):
        want["tabn"] = int(lv["tabn"])
    return want


# What the start check cannot read back from the editor's dump or screen, so
# a row counts as having it on its label's word: (factor, which part).  The
# coverage report prints this list beside the effective cover, so a reader
# knows which tuples rest on a label and not on a session.
TRUSTED_LEVELS = (
    ("keys", "the key style"),
    ("block", "the block mode"),
    ("trim", "Trim Auto-indent"),
    ("ending", "Keep Final newline"),
    ("numundo", "Num. Undo"),
    ("autosave", "Autosave"),
    ("autoindent", "source against always (both read as on)"),
    ("tabn", "Tabstops on a file with no tab at a row's start"),
    ("width", "lowered and raised: the Max. Columns the dialog sets after the "
              "window opened"),
)


def trusted_text():
    """The levels taken on the row's word, as one line of the coverage
    report."""
    return "; ".join("%s (%s)" % (f, what) for f, what in TRUSTED_LEVELS)


def start_mismatches(row, observed):
    """[(factor, wanted, observed)] where a started session did not get its
    row's label."""
    out = []
    for f, want in sorted(expected_levels(row).items()):
        got = observed.get(f)
        if got != want:
            out.append((f, want, got))
    return out
