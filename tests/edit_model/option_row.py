"""The option array's factors, and one row of it as a session.

A row picks one level of every factor: the key style, the block mode, the
insert mode, the auto-indent mode, the options dialog's numbers, autosave,
the kind of file, the breakpoints, what the clipboard holds, Alt-J, whether
Max. Columns changes after the window opened, and whether the file is
read-only.  ``Row.xwperc()`` is the options file it starts with,
``Row.prelude()`` the actions that bring a started session to the row's
state (the dialog, the breakpoints, the clipboard), and ``Row.levels()`` the
labels the coverage report counts under.

The levels the editor's harness cannot drive yet are in ``PENDING``: a
second window, the Show Buffer viewer, CRLF files.  They are not in any row
and the coverage report lists them as not built, never as covered.
"""
# Standard library only: the coverage report and the array generator read
# this module without a terminal emulator installed.

# name -> levels, in the order the TSV lists them.
FACTORS = {
    "keys": ("old", "cua"),
    "block": ("modern", "wordstar"),
    "ins": ("insert", "overwrite"),
    "autoindent": ("source", "always", "never"),
    "maxcol": ("118", "40"),
    "tabn": ("8", "4", "1"),
    "numundo": ("10", "2", "0"),
    "autosave": ("off", "changes"),
    "endmark": ("off", "on"),
    "file": ("mixed.c", "mixed.txt", "empty.c", "empty.txt", "oneline.c", "oneline.txt"),
    "breakpoints": ("none", "two"),
    "clipboard": ("empty", "oneline", "multiline"),
    "alt_j": ("off", "on"),
    "width_change": ("none", "lowered", "raised"),
    "window": ("file", "readonly"),
    "windows": ("one",),
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
              ("width_change", "file", "clipboard", "windows"))

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
        wc = self.lv["width_change"]
        if wc == "raised":
            return WIDTH_RAISED_START
        if wc == "lowered":
            return LOWERED_START
        return int(self.lv["maxcol"])

    def options2(self):
        v = OPTIONS2_BASE
        if self.lv["keys"] == "cua":
            v += OPT_CUA
        if self.lv["endmark"] == "on":
            v += OPT_ENDMARK
        if self.lv["block"] == "wordstar":
            v += OPT_WORDSTAR
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
        wc = self.lv["width_change"]
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
        if level == "oneline":
            return list(COPY_ONE_LINE)
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


def violates(levels):
    """Does a (possibly partial) assignment break a constraint of the array?
    Two breakpoints need two rows to sit on, and the editor sets them in C
    files only: the mixed C file is the one file that has both."""
    return (levels.get("breakpoints") == "two"
            and "file" in levels and levels["file"] != BREAKPOINT_FILE)
