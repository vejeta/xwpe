"""An executable model of editing in ``wpe``: what each key of the key loop
does to the text, the cursor, the block marks, the undo lists, the modified
count and the file on disk.

The model is a function: ``apply(state, action)`` returns a new state and
never changes the one it was given.  A test runs the model beside the editor
(``lockstep.py``) and reads every difference as a finding, so the model has
to say, for each thing it predicts, where that prediction comes from.  Every
rule below carries its source:

``manual``
    the 1990s help text (``help.xwpe_eng``) or the manual's chapters
    (``docs/chapters/editor.texi``): what a user was told the editor does.
``chapter``
    ``docs/chapters/compatibility.texi``: a deliberate change from the
    original, recorded with its reason.
``decision``
    the user's word, recorded in the design before its chapter entry is
    written (one Ctrl-U per user action, 2026-11-12).
``golden``
    an approved golden trace (``goldens/APPROVED``).
``design``
    the edit-model design's own choice where no text above says anything
    (Undo as a snapshot of the cursor): a prediction, not a specification.
``code``
    read from the C source where nothing else says what is right.  An
    agreement on such a rule is regression only, never evidence; a
    disagreement is ``unspecified`` and goes to the user's list.

The rows of a window are kept as the editor keeps them: a row of bytes that
ends either with a line break (``H``, hard) or soft-wrapped into the next row
(``S``).  The primitives that change them (insert, delete, split, the line
owner's insert and delete) are modelled at that level so a wrap or a reflow
lands where the editor's does; the user-level rules are written over them.

Blocks, the clipboard, Search > Replace and the option dialog are in
``blocks.py``, which registers its rules here.  Not modelled: Find and Search
again, Go to Line, a second window, the mouse, and the files Ctrl-K R and W
read and write; those keys are not actions of this model.
"""
import unicodedata

HARD = "H"
SOFT = "S"
WR = 0x0A            # the byte that ends a hard row
NUL = 0x00
TAB = 0x09
SPACE = 0x20
DASH = 0x2D
HASH = 0x23
BRACE_OPEN = 0x7B
UTF8_CONT_MASK = 0xC0
UTF8_CONT = 0x80
UTF8_LEAD_MIN = 0xC0
UTF8_LEAD_END = 0xFE
ASCII_MAX = 0x7F
CONTROL_MAX = 0x1F   # bytes below a space show as two columns (^X)

# What a line counts toward the modified count, and a character (we_buffer.h).
SAVE_PER_LINE = 10
SAVE_PER_CHAR = 1

# f->ins: bit 0 is insert mode, bit 1 Alt-J's literal mode; 8 a locked window.
INS_INSERT = 1
INS_LITERAL = 2
INS_READONLY = 8
# f->flg bit 0: auto-indent is on for this window.
FLG_AUTOINDENT = 1

# The option defaults (we_main.c) and the undo list's one extra step.
DEFAULT_TABN = 8
DEFAULT_MAXCOL = 120
# The markers a window has (edit.h, POINT pt[9]); Ctrl-K 9 and Ctrl-O 9 name
# a tenth that is not there, a known bug the walks stay clear of.
MARKER_COUNT = 9
DEFAULT_AUTOINDENT_COL = 3
DEFAULT_NUMUNDO = 10
DEFAULT_MAXCHG = 999
UNDO_EXTRA_STEPS = 1

# ---- sources and rules -------------------------------------------------------

SOURCE_MANUAL = "manual"
SOURCE_CHAPTER = "chapter"
SOURCE_DECISION = "decision"
SOURCE_GOLDEN = "golden"
SOURCE_DESIGN = "design"
SOURCE_CODE = "code"
SOURCES = (SOURCE_MANUAL, SOURCE_CHAPTER, SOURCE_DECISION, SOURCE_GOLDEN,
           SOURCE_DESIGN, SOURCE_CODE)
# Sources that say what is right: a disagreement where only these predicted
# the field is the editor's.  The others predict without specifying.
SPECIFYING_SOURCES = (SOURCE_MANUAL, SOURCE_CHAPTER, SOURCE_DECISION)

# The fields a rule decides, as the lockstep compares them.
F_TEXT = "text"
F_CURSOR = "cursor"
F_MARKS = "marks"
F_ANCHOR = "anchor"
F_MODE = "mode"
F_SAVE = "save"
F_STEPS = "steps"
F_BREAKPOINTS = "breakpoints"
F_DISK = "disk"
F_AUTOSAVE = "autosave"
F_BOX = "box"            # a message box the action opens, by its text
F_CLIP = "clipboard"     # the text of the clipboard window
F_MARKERS = "markers"    # the ten markers Ctrl-K 0..9 set
FIELDS = (F_TEXT, F_CURSOR, F_MARKS, F_ANCHOR, F_MODE, F_SAVE, F_STEPS,
          F_BREAKPOINTS, F_DISK, F_AUTOSAVE, F_BOX, F_CLIP, F_MARKERS)

BOX_NO_UNDO = "No more Undo"
BOX_NO_REDO = "No more Redo"
BOX_SAVE_FAILED = "File failed to save"


class Rule:
    """One thing the model predicts, the fields it decides, and its source."""

    def __init__(self, name, source, ref, says, fields):
        assert source in SOURCES, source
        self.name = name
        self.source = source
        self.ref = ref
        self.says = says
        self.fields = tuple(fields)

    def __repr__(self):
        return "Rule(%s, %s, %s)" % (self.name, self.source, self.ref)


RULES = {}


def _rule(name, source, ref, says, fields):
    RULES[name] = Rule(name, source, ref, says, fields)


HELP = "help.xwpe_eng"
EDITOR = "docs/chapters/editor.texi"
COMPAT = "docs/chapters/compatibility.texi"
DECIDED_ONE_STEP = "user's decision 2026-11-12: one Ctrl-U per user action"

# -- characters
_rule("type_insert", SOURCE_MANUAL, HELP + ":602-603",
      "In insert mode a typed character goes in at the cursor and the cursor "
      "moves past it.", (F_TEXT, F_CURSOR))
_rule("type_overwrite", SOURCE_MANUAL, HELP + ":602-603",
      "In overwrite mode a typed character replaces the one under the cursor.",
      (F_TEXT, F_CURSOR))
_rule("overwrite_at_end_inserts", SOURCE_CODE, "we_edit.c:2627",
      "Overwrite at the end of a row inserts.", (F_TEXT, F_CURSOR))
_rule("type_multibyte_inserts", SOURCE_CODE, "we_edit.c:942-947",
      "A character past ASCII is inserted, in overwrite mode too.", (F_TEXT, F_CURSOR))
_rule("wrap_at_max_columns", SOURCE_MANUAL, HELP + ":1578-1580",
      "Typing past Max. Columns wraps the line at a word.", (F_TEXT,))
_rule("wrap_typing_point", SOURCE_CODE, "we_edit.c:2470-2496",
      "The wrap happens when length + inserted >= width - 1, at the last blank "
      "or dash; a hard row pushes its tail down as a new row, a soft row into "
      "the next row.", (F_TEXT, F_CURSOR, F_MARKS))
_rule("literal_mode_inserts_controls", SOURCE_CODE, "we_edit.c:937-938",
      "In Alt-J's mode a control key goes into the text as its byte.",
      (F_TEXT, F_CURSOR))
# -- Enter
_rule("enter_splits", SOURCE_MANUAL, HELP + ":589",
      "Return inserts a line break at the cursor.", (F_TEXT,))
_rule("enter_cursor_next_row", SOURCE_CODE, "we_edit.c:2157-2161",
      "After Return the cursor is at the start of the new row.", (F_CURSOR,))
_rule("enter_soft_end_hardens", SOURCE_CODE, "we_edit.c:2144-2145",
      "Return at the end of a soft-wrapped row only ends that row.", (F_TEXT,))
_rule("enter_autoindent", SOURCE_MANUAL, HELP + ":2041-2043",
      "With auto-indent the new line is indented to the beginning of the "
      "previous line.", (F_TEXT, F_CURSOR))
_rule("enter_indent_quirks", SOURCE_CODE, "we_edit.c:2072-2080",
      "The indent skips lines starting with '#', reads a leading '{' as "
      "indent, and is written as spaces.", (F_TEXT, F_CURSOR))
_rule("enter_one_step", SOURCE_CHAPTER, COMPAT + ":125-130",
      "Return with its auto-indent is one Ctrl-U.", (F_STEPS,))
# -- Tab
_rule("tab_literal", SOURCE_CODE, "we_edit.c:937",
      "Without auto-indent Tab types a tab character (overwriting in "
      "overwrite mode).", (F_TEXT, F_CURSOR))
_rule("tab_autoindent", SOURCE_CODE, "we_edit.c:1896-2011",
      "With auto-indent Tab inserts spaces up to the previous indent, or "
      "Auto Ind. Col. past it, or a tab; trailing blanks are erased.",
      (F_TEXT, F_CURSOR))
_rule("tab_autoindent_manual", SOURCE_MANUAL, HELP + ":2045-2053",
      "With auto-indent, Tab in front of the first visible character indents "
      "the line like the previous one; behind that column it indents N "
      "characters more than the previous line, N being the Tabstops number; "
      "behind that position, or between visible characters, it inserts a tab.",
      (F_TEXT, F_CURSOR))
_rule("tab_one_step", SOURCE_DECISION, DECIDED_ONE_STEP,
      "Tab with auto-indent is one Ctrl-U.", (F_STEPS,))
# -- Backspace and Delete
_rule("backspace_char", SOURCE_MANUAL, HELP + ":592-593",
      "Backspace deletes the character before the cursor (a whole UTF-8 "
      "character).", (F_TEXT, F_CURSOR))
_rule("backspace_joins", SOURCE_MANUAL, EDITOR + ":66",
      "Backspace at the start of a line joins it to the line above.", (F_TEXT,))
_rule("backspace_cursor_rows", SOURCE_CODE, "we_edit.c:961-990",
      "After a join or an empty line's deletion the cursor is at the end of "
      "the row above, one byte before it when that row is soft.", (F_CURSOR,))
_rule("backspace_autoindent", SOURCE_MANUAL, HELP + ":2055-2058",
      "With auto-indent, Backspace in front of the first visible character "
      "re-indents the line like the last line that started before the cursor.",
      (F_TEXT, F_CURSOR))
_rule("backspace_autoindent_detail", SOURCE_CODE, "we_edit.c:2014-2062",
      "Tabs in the indent become spaces; '#' lines are skipped; with no such "
      "line one byte at column 0 is deleted.", (F_TEXT, F_CURSOR))
_rule("backspace_indent_one_step", SOURCE_DECISION, DECIDED_ONE_STEP,
      "Backspace inside an indent is one Ctrl-U.", (F_STEPS,))
_rule("delete_char", SOURCE_MANUAL, HELP + ":596-597",
      "Delete deletes the character the cursor is on (the line break at the "
      "end of a line).", (F_TEXT, F_CURSOR))
_rule("delete_edges", SOURCE_CODE, "we_edit.c:1006-1007",
      "Delete does nothing at the end of a soft row or of the last row.",
      (F_TEXT,))
_rule("reflow_after_delete", SOURCE_CODE, "we_edit.c:2366-2384",
      "A row left soft pulls words up from the row below as far as they fit.",
      (F_TEXT, F_CURSOR, F_MARKS))
_rule("ctrl_t_word", SOURCE_MANUAL, HELP + ":611",
      "Ctrl-T kills the next word after the cursor.", (F_TEXT,))
_rule("ctrl_t_extent", SOURCE_CODE, "we_edit.c:1233-1244, 2640-2651",
      "Ctrl-T deletes to the start of the next word, line break included; at "
      "the end of a soft row it moves to the next row.", (F_TEXT, F_CURSOR))
_rule("kill_to_eol", SOURCE_MANUAL, HELP + ":615-617",
      "Ctrl-Z and Ctrl-O Y kill from the cursor to the end of the line.", (F_TEXT,))
_rule("kill_soft_row_whole", SOURCE_CODE, "we_edit.c:2365",
      "Killing a soft row from column 0 removes the row.", (F_TEXT, F_CURSOR))
_rule("ctrl_y_line", SOURCE_MANUAL, HELP + ":614",
      "Ctrl-Y deletes a whole line.", (F_TEXT,))
_rule("ctrl_y_only_line", SOURCE_CHAPTER, COMPAT + ":144-147",
      "Ctrl-Y on the only line is undoable like any other.", (F_STEPS,))
_rule("ctrl_y_cursor", SOURCE_CODE, "we_edit.c:1250-1255",
      "After Ctrl-Y the cursor stays on its row (the one above at the end).",
      (F_CURSOR,))
_rule("only_line_emptied", SOURCE_CODE, "we_buffer.c:991-996",
      "Deleting the only line leaves one empty soft row.", (F_TEXT,))
# -- the line owner
_rule("owner_marks_rows", SOURCE_CHAPTER, COMPAT + ":137-142",
      "A mark on a deleted row goes to the start of the row that took its "
      "place, or to the end of the file.", (F_MARKS,))
_rule("primitive_marks", SOURCE_CODE, "we_edit.c:2360-2363, 2544-2547; we_buffer.c:1210-1225",
      "Marks inside a row move with inserted and deleted bytes and with the "
      "tail of a split.", (F_MARKS,))
_rule("line_identity", SOURCE_CHAPTER, COMPAT + ":149-158",
      "Breakpoints stay on their code when lines move; a deleted or joined "
      "line's breakpoint goes and comes back with Undo.", (F_BREAKPOINTS,))
# -- cursor
_rule("cursor_keys", SOURCE_MANUAL, EDITOR + ":17-33; " + HELP + ":560-583",
      "Arrows, Home/End, Page keys and the file and window ends move the "
      "cursor as named.", (F_CURSOR,))
_rule("cursor_detail", SOURCE_CODE, "we_edit.c:1084-1287",
      "The arrows keep a column across rows, Left/Right cross row ends, the "
      "word keys stop at word starts, the page keys move by the window.",
      (F_CURSOR,))
_rule("cursor_clamped", SOURCE_CODE, "we_edit.c:2250-2252",
      "The cursor is put back inside its row after each key.", (F_CURSOR,))
_rule("anchor_after_key", SOURCE_CODE, "we_edit.c:1073",
      "The Shift-marking anchor is the cursor after every key, before the "
      "clamp.", (F_ANCHOR,))
_rule("shift_marks", SOURCE_MANUAL, HELP + ":623-624; " + EDITOR + ":87",
      "Shift with a cursor key marks a block.", (F_MARKS,))
_rule("shift_marks_detail", SOURCE_CODE, "we_edit.c:1269-1285",
      "The block grows from the anchor: an end at the anchor follows the "
      "cursor, else the block is the anchor-to-cursor span.", (F_MARKS,))
# -- modes
_rule("insert_toggle", SOURCE_MANUAL, HELP + ":602-603",
      "Insert and Alt-I switch insert and overwrite mode.", (F_MODE,))
_rule("alt_j_toggle", SOURCE_MANUAL, HELP + ":605",
      "Alt-J switches the special-character insert mode.", (F_MODE,))
_rule("locked_refuses", SOURCE_CODE, "we_wind.c:855-869",
      "A locked window refuses every change.", (F_TEXT, F_MODE))
# -- save and autosave
_rule("save_writes_file", SOURCE_MANUAL, HELP + ":699",
      "F2 saves the file (Alt-F2 in the CUA key style).", (F_DISK,))
_rule("save_format", SOURCE_CODE, "we_edit.c e_write",
      "The file is the rows, with a line break after each hard row.", (F_DISK,))
_rule("save_fails_read_only", SOURCE_CODE, "probe of F2 in a 0444 file, 2026-11-12",
      "Saving a window that is closed to changes writes nothing and shows the "
      "box File failed to save.", (F_DISK, F_BOX))
_rule("modified_count", SOURCE_CODE, "we_edit.c:2110-2114; we_buffer.c:1039",
      "Each changed character counts 1, each line 10; Save sets 0.", (F_SAVE,))
_rule("autosave_asv", SOURCE_MANUAL, HELP + ":1547-1553",
      "With Autosave on, after Max. Changes the text goes to <name>.ASV, the "
      "file itself unchanged.", (F_AUTOSAVE,))
_rule("autosave_threshold", SOURCE_CODE, "we_edit.c:847, 2828-2853",
      "Before the next key, a count above Max. Changes autosaves and sets the "
      "count to 1, also with Autosave off.", (F_SAVE, F_AUTOSAVE))
# -- undo
_rule("undo_text", SOURCE_MANUAL, HELP + ":683; " + EDITOR + ":58-69",
      "Ctrl-U undoes the last change: the text as it was.", (F_TEXT, F_BREAKPOINTS))
_rule("redo_text", SOURCE_MANUAL, HELP + ":686",
      "Ctrl-R undoes the last undo.", (F_TEXT, F_BREAKPOINTS))
_rule("undo_line_cursor", SOURCE_MANUAL, EDITOR + ":62-63",
      "Undo of a deleted line puts the cursor and the block back as they "
      "were.", (F_CURSOR, F_MARKS))
_rule("undo_cursor_snapshot", SOURCE_DESIGN, "this model: Undo as a snapshot",
      "Undo puts the cursor where it was before the change, Redo where it was "
      "after.", (F_CURSOR,))
_rule("undo_marks_span", SOURCE_CODE, "we_buffer.c replay of 'a' and 'r' records",
      "Undo of a deletion marks the text it brings back (a line break as "
      "len+1); Redo of an insertion marks it; else the marks from before.",
      (F_MARKS,))
_rule("one_step_per_action", SOURCE_DECISION, DECIDED_ONE_STEP,
      "One user action is one undo step.", (F_STEPS,))
_rule("undo_bound", SOURCE_MANUAL, HELP + ":1589-1591",
      "Num. Undo steps are kept for Undo.", (F_STEPS,))
_rule("undo_bound_extra", SOURCE_CODE, "we_buffer.c:608",
      "The list keeps Num. Undo + 1 steps.", (F_STEPS,))
_rule("fresh_change_clears_redo", SOURCE_CODE, "we_buffer.c record push",
      "A fresh change empties the redo list.", (F_STEPS,))
_rule("undo_count_unpredicted", SOURCE_CODE, "we_undo replay",
      "The modified count after Undo/Redo is read back, not predicted.", (F_SAVE,))
_rule("undo_empty_says_so", SOURCE_CODE, "we_main.c:71, 81",
      "Undo or Redo with nothing left shows the error box No more Undo / "
      "No more Redo.", (F_BOX,))


# Where a manual rule and the code say different things.  The model follows
# the code (so the editor and the model agree) and the user decides which
# is right; ``goldens/APPROVAL.md`` lists each under "To decide".
CONFLICTS = {
    "tab_autoindent": ("help.xwpe_eng:2045-2053 says Tab behind the first "
                       "visible character indents N more than the line above, "
                       "N being Tabstops; we_edit.c uses Auto Ind. Col. "
                       "instead (3 by default, Tabstops is 8)."),
}


def rule_counts():
    """How many rules each source backs, for the report."""
    out = dict((s, 0) for s in SOURCES)
    for r in RULES.values():
        out[r.source] += 1
    return out


# ---- options -------------------------------------------------------------------

class Options:
    """The options the key loop reads, as an xwperc's [General] sets them."""

    def __init__(self, tabn=DEFAULT_TABN, autoindent_col=DEFAULT_AUTOINDENT_COL,
                 numundo=DEFAULT_NUMUNDO, maxchg=DEFAULT_MAXCHG, autosave=False,
                 cua=False, maxcol=DEFAULT_MAXCOL, wordstar=False, endmark=False):
        self.tabn = tabn
        self.autoindent_col = autoindent_col
        self.numundo = numundo
        self.maxchg = maxchg
        self.autosave = autosave
        self.cua = cua
        self.maxcol = maxcol          # the width a window opened now gets
        self.wordstar = wordstar      # Ctrl-K B keeps the block's end
        self.endmark = endmark        # a hard row shows $ after its text

    @classmethod
    def from_xwperc(cls, text):
        """Options from the text of an xwperc (None: the defaults)."""
        o = cls()
        if not text:
            return o
        for line in text.splitlines():
            if ":" not in line:
                continue
            key, value = [p.strip() for p in line.split(":", 1)]
            if not value.lstrip("-").isdigit():
                continue
            v = int(value)
            if key == "Tab":
                o.tabn = v
            elif key == "AutoIndent":
                o.autoindent_col = v
            elif key == "NumUndo":
                o.numundo = v
            elif key == "MaxChanges":
                o.maxchg = v
            elif key == "Autosave":
                o.autosave = (v & 2) != 0
            elif key == "Options2":
                o.cua = (v & CUA_STYLE_BIT) != 0
                o.wordstar = (v & WORDSTAR_BIT) != 0
                o.endmark = (v & ENDMARK_BIT) != 0
            elif key == "MaxColumn":
                o.maxcol = v
        return o


# Options2's bits for the CUA key style and the WordStar block mode (options.h).
CUA_STYLE_BIT = 1
ENDMARK_BIT = 2
WORDSTAR_BIT = 4


# ---- state ---------------------------------------------------------------------

class Row:
    """One row: its bytes, whether it ends hard, and the line it belongs to."""

    __slots__ = ("data", "end", "ident")

    def __init__(self, data, end, ident):
        self.data = bytes(data)
        self.end = end
        self.ident = ident

    def __eq__(self, other):
        return (self.data, self.end, self.ident) == (other.data, other.end, other.ident)

    def __repr__(self):
        return "Row(%r, %s, #%d)" % (self.data, self.end, self.ident)


class Step:
    """One undo step: the state before an action and after it."""

    def __init__(self, before, after, action, line_record=False, ins=INS_INSERT):
        self.before = before
        self.after = after
        self.action = action
        self.line_record = line_record
        self.ins = ins          # the mode the action was typed in
        self.undo_marks = None  # (begin, end) Undo leaves, when the action says


class Snapshot:
    """What Undo and Redo give back: rows with their identities, the cursor
    and the marks."""

    def __init__(self, st):
        self.rows = [Row(r.data, r.end, r.ident) for r in st.rows]
        self.cursor = (st.y, st.x)
        self.mb = st.mb
        self.me = st.me


class State:
    """Everything the model knows about one window."""

    def __init__(self):
        self.rows = []
        self.y = 0
        self.x = 0
        self.mb = (0, 0)
        self.me = (0, 0)
        self.ks = (0, 0)
        self.ins = INS_INSERT
        self.flg = 0
        self.save = 0
        self.width = 0
        self.clsv = 0           # the column Up and Down keep
        self.undo = []          # [Step]
        self.redo = []
        self.opaque_undo = 0    # steps the editor holds that the model cannot replay
        self.opaque_redo = 0
        self.brp = set()        # identities with a breakpoint
        self.next_ident = 0
        self.disk = None        # the file as last saved by the model, or None
        self.asv = None
        self.autosaved = False
        self.save_known = True  # False after Undo/Redo: the count is read back
        self.view = (0, 0, 1, 0, 20, 79)
        self.options = Options()
        self.box = None         # the message box the last action opened
        self.fired = []         # rule names the last action used
        self.clip = [(HARD, b"")]   # the clipboard window's rows, (end, bytes)
        self.clip_width = DEFAULT_MAXCOL   # the clipboard window's width
        self.pt = [(-1, -1)] * MARKER_COUNT   # the markers, (row, column); (-1, -1) unset
        self.resync_fields = set()  # fields the last action does not predict

    # -- reading ---------------------------------------------------------------
    @property
    def ud(self):
        return len(self.undo) + self.opaque_undo

    @property
    def rd(self):
        return len(self.redo) + self.opaque_redo

    def lines(self):
        """The rows as (end, bytes), as the dump reports them."""
        return [(r.end, r.data) for r in self.rows]

    def brp_rows(self):
        return sorted(i for i, r in enumerate(self.rows) if r.ident in self.brp)

    def copy(self):
        s = State.__new__(State)
        s.__dict__.update(self.__dict__)
        s.rows = [Row(r.data, r.end, r.ident) for r in self.rows]
        s.undo = list(self.undo)
        s.redo = list(self.redo)
        s.brp = set(self.brp)
        s.pt = list(self.pt)
        s.options = Options.__new__(Options)
        s.options.__dict__.update(self.options.__dict__)
        s.fired = []
        s.resync_fields = set()
        return s


def clipboard_rows(w0):
    """The rows the clipboard window (dump window 0) holds, as (end, bytes)."""
    if w0 is None:
        return [(HARD, b"")]
    return list(w0.lines)


def _dump_marker(p):
    """A dump's marker ``(x, y)`` as the model's (row, column)."""
    return (p[1], p[0]) if p[0] >= 0 else (-1, -1)


def state_from_window(w, options=None, clip=None):
    """The model's state for a dump window (oracle.Window), as at start;
    @clip is the clipboard window of the same dump, if any."""
    st = State()
    st.options = options or Options()
    st.clip = clipboard_rows(clip)
    if clip is not None:
        st.clip_width = clip.mx
    st.pt = [_dump_marker(p) for p in w.pt]
    st.rows = []
    for end, data in w.lines:
        st.rows.append(Row(data, end, st.next_ident))
        st.next_ident += 1
    st.y, st.x = w.y, w.x
    st.mb, st.me, st.ks = w.mb, w.me, w.ks
    st.ins, st.flg, st.save = w.ins, w.flg, w.save
    st.width = w.mx
    st.view = w.view
    st.brp = set(st.rows[r].ident for r in w.brp if 0 <= r < len(st.rows))
    st.opaque_undo = w.ud
    st.opaque_redo = w.rd
    return st


def resync(st, w, vertical=False, clip=None):
    """Take the editor's word for the fields the model got wrong, keeping
    line identities where the rows still match (common head and tail) and
    the undo steps the model holds (they were agreed when pushed).  Unless
    the key was Up or Down (@vertical), the column those keys keep is the
    editor's cursor column, as the key loop sets it after every other key.
    @clip is the clipboard window of the same dump."""
    if clip is not None:
        st.clip = clipboard_rows(clip)
    st.pt = [_dump_marker(p) for p in w.pt]
    old = st.rows
    new = list(w.lines)
    head = 0
    while head < len(old) and head < len(new) and (old[head].end, old[head].data) == new[head]:
        head += 1
    tail = 0
    while (tail < len(old) - head and tail < len(new) - head
           and (old[-1 - tail].end, old[-1 - tail].data) == new[-1 - tail]):
        tail += 1
    rows = [Row(d, e, old[i].ident) for i, (e, d) in enumerate(new[:head])]
    for e, d in new[head:len(new) - tail]:
        rows.append(Row(d, e, st.next_ident))
        st.next_ident += 1
    for k in range(tail):
        i_old = len(old) - tail + k
        e, d = new[len(new) - tail + k]
        rows.append(Row(d, e, old[i_old].ident))
    st.rows = rows
    st.y, st.x = w.y, w.x
    st.mb, st.me, st.ks = w.mb, w.me, w.ks
    st.ins, st.flg, st.save = w.ins, w.flg, w.save
    st.view = w.view
    st.brp = set(st.rows[r].ident for r in w.brp if 0 <= r < len(st.rows))
    _align_steps(st, w.ud, w.rd)
    st.save_known = True
    if not vertical and 0 <= st.y < len(st.rows):
        st.clsv = visual_col(st.rows[st.y].data, st.x, st.options.tabn)


def _align_steps(st, ud, rd):
    """Make the undo and redo depths the editor's: steps the model lacks
    are opaque, steps it has beyond the editor's are dropped (oldest first
    for undo)."""
    while st.ud > ud:
        if st.opaque_undo:
            st.opaque_undo -= 1
        else:
            st.undo.pop(0)
    while st.ud < ud:
        st.opaque_undo += 1
    while st.rd > rd:
        if st.opaque_redo:
            st.opaque_redo -= 1
        else:
            st.redo.pop(0)
    while st.rd < rd:
        st.opaque_redo += 1


# ---- the row machine (the C primitives at byte level) ---------------------------

def _isspace(c):
    return c in (SPACE, TAB, WR, 0x0B, 0x0C, 0x0D)


def _isalnum1(c):
    return (0x30 <= c <= 0x39) or (0x41 <= c <= 0x5A) or (0x61 <= c <= 0x7A) or c == 0x5F


class Rows:
    """A working copy of the rows as C buffers: content, then the line
    break of a hard row, then NUL, in a buffer of width + 2 bytes."""

    def __init__(self, st):
        self.st = st
        self.mx = st.width
        self.bufs = [self._buf(r.data, r.end == HARD) for r in st.rows]
        self.ids = [r.ident for r in st.rows]

    def _buf(self, data, hard):
        b = bytearray(self.mx + 2)
        n = min(len(data), self.mx)
        b[:n] = data[:n]
        if hard and n <= self.mx:
            b[n] = WR
        return b

    @property
    def nlines(self):
        return len(self.bufs)

    def str_len(self, y):
        b = self.bufs[y]
        for i, c in enumerate(b):
            if c == WR or c == NUL:
                return i
        return len(b)

    def cstr(self, y):
        """The row as strlen sees it: content plus its line break."""
        b = self.bufs[y]
        i = b.index(NUL) if NUL in b else len(b)
        return bytes(b[:i])

    def hard(self, y):
        return self.bufs[y][self.str_len(y)] == WR

    def content(self, y):
        return bytes(self.bufs[y][:self.str_len(y)])

    def commit(self):
        """Write the buffers back into the state's rows."""
        self.st.rows = [Row(self.content(y), HARD if self.hard(y) else SOFT, self.ids[y])
                        for y in range(self.nlines)]

    def new_ident(self):
        i = self.st.next_ident
        self.st.next_ident += 1
        return i

    # -- marks ----------------------------------------------------------------
    def marks(self):
        return [list(self.st.mb), list(self.st.me)]

    def set_marks(self, m):
        self.st.mb, self.st.me = tuple(m[0]), tuple(m[1])

    def clamp_point(self, p):
        if p[1] < 0:
            return
        if p[0] >= self.nlines:
            p[0] = self.nlines - 1
        if p[1] > self.str_len(p[0]):
            p[1] = self.str_len(p[0])

    def clamp_all(self):
        m = self.marks()
        for p in m:
            self.clamp_point(p)
        self.set_marks(m)
        c = [self.st.y, self.st.x]
        self.clamp_point(c)
        self.st.y, self.st.x = c

    def count(self, n):
        self.st.save += n

    # -- the line owner ---------------------------------------------------------
    def _splice_marks(self, y, n_del, n_ins, changed):
        m = self.marks()
        for p in m:
            if p[0] < y or p[0] < y + changed:
                continue
            if p[0] < y + n_del:
                if y + n_ins < self.nlines:
                    p[0], p[1] = y + n_ins, 0
                else:
                    p[0] = self.nlines - 1
                    p[1] = self.str_len(p[0])
                continue
            p[0] += n_ins - n_del
        self.set_marks(m)
        self.clamp_all()

    def insert_blank(self, y):
        """e_buffer_insert_blank: an empty row at y."""
        self.bufs.insert(y, self._buf(b"", False))
        self.ids.insert(y, self.new_ident())
        self._splice_marks(y, 0, 1, 0)
        self.count(SAVE_PER_LINE)

    def delete_line(self, y):
        """e_buffer_delete_lines(y, 1): the only line becomes one empty row."""
        self.st.fired.append("owner_marks_rows")
        if self.nlines == 1:
            self.bufs[0] = self._buf(b"", False)
            self.ids[0] = self.new_ident()
            self.st.fired.append("only_line_emptied")
            self._splice_marks(0, 1, 1, 0)
            self.count(2 * SAVE_PER_LINE)
            return
        del self.bufs[y]
        del self.ids[y]
        self._splice_marks(y, 1, 0, 0)
        self.count(SAVE_PER_LINE)

    def split_line(self, y, x):
        """e_buffer_split_line(y, x, HEAD_WR)."""
        n = self.str_len(y)
        if x > n:
            x = n
        if x == 0:
            self.bufs.insert(y, self._buf(b"", True))
            self.ids.insert(y, self.new_ident())
            self._splice_marks(y, 0, 1, 0)
            self.count(SAVE_PER_LINE)
            return
        src = self.cstr(y)
        head = self._buf(src[:x], True)
        tail = bytearray(self.mx + 2)
        t = src[x:x + self.mx]
        tail[:len(t)] = t
        m = self.marks()
        moved = [None, None]
        for k, p in enumerate(m):
            if p[0] == y and p[1] > x:
                moved[k] = [y + 1, p[1] - x]
        self.bufs[y:y + 1] = [head, tail]
        self.ids[y:y + 1] = [self.ids[y], self.new_ident()]
        self._splice_marks(y, 1, 2, 1)
        m = self.marks()
        for k in range(2):
            if moved[k] is not None:
                m[k] = moved[k]
        self.set_marks(m)
        self.count(3 * SAVE_PER_LINE)

    # -- the character primitives -------------------------------------------------
    def ins_nchar(self, s, xa, ya):
        """e_ins_nchar: insert the bytes @s at (ya, xa), wrapping."""
        s = bytes(s)
        n = len(s)
        sp = 0
        mx = self.mx
        self.count(n)
        buf = self.bufs[ya]
        buf[mx] = NUL
        ln = self.str_len(ya)
        wrapped = True
        if ln + n >= mx - 1:
            self.st.fired.append("wrap_at_max_columns")
            self.st.fired.append("wrap_typing_point")
            if xa < ln:
                i = mx - n - 1
                if i >= ln - 1:
                    i = ln - 1
            else:
                while xa < mx - 1 and n > 0:
                    buf[xa + 1] = buf[xa]
                    buf[xa] = s[sp]
                    xa += 1
                    sp += 1
                    n -= 1
                buf[xa + 1] = NUL
                i = mx
                ln = self.str_len(ya)
            while i > 0 and buf[i] != SPACE and buf[i] != DASH:
                i -= 1
            if i <= 0:
                i = ln - 2
            if i < 0:
                i = 0
            if buf[ln] == WR or ya + 1 >= self.nlines:
                wrapped = self._wrap_tail_down(ya, xa, n, i)
            else:
                tail = bytes(buf[i + 1:ln])
                save_cursor = (self.st.y, self.st.x)
                self.ins_nchar(tail, 0, ya + 1)
                self.st.y, self.st.x = save_cursor
                m = self.marks()
                for p in m:
                    if p[0] == ya:
                        if p[1] > i:
                            p[0] += 1
                            p[1] -= i + 1
                        elif p[1] >= xa:
                            p[1] += n
                self.set_marks(m)
            buf = self.bufs[ya]
            if wrapped:
                buf[i + 1:] = bytes(len(buf) - i - 1)
                ln = self.str_len(ya)
            if wrapped and xa > ln:
                xa -= ln
                ya += 1
                m = self.marks()
                for p in m:
                    if p[0] == ya and p[1] >= xa:
                        p[1] += n
                self.set_marks(m)
        else:
            m = self.marks()
            for p in m:
                if p[0] == ya and p[1] >= xa:
                    p[1] += n
            self.set_marks(m)
        self.st.fired.append("primitive_marks")
        buf = self.bufs[ya]
        ln = self.str_len(ya)
        for j in range(ln, xa - 1, -1):
            if j + n <= mx:
                buf[j + n] = buf[j]
        for j in range(n):
            if xa + j <= mx:
                buf[xa + j] = s[sp + j]
        if buf[ln] == WR:
            buf[ln + 1] = NUL
        buf[mx] = NUL
        self.st.x = xa + n
        self.st.y = ya
        return xa + n

    def _wrap_tail_down(self, ya, xa, n, i):
        """Typing past the margin of a hard row: its tail goes to a new row."""
        self.insert_blank(ya + 1)
        m = self.marks()
        for p in m:
            if p[0] != ya:
                continue
            if p[1] > i:
                p[0] += 1
                p[1] -= i + 1
            elif p[1] >= xa:
                p[1] += n
        self.set_marks(m)
        src = self.bufs[ya]
        dst = self.bufs[ya + 1]
        j = i + 1
        while j < len(src) and src[j] != WR and src[j] != NUL and (j - i - 1) < self.mx - 1:
            dst[j - i - 1] = src[j]
            j += 1
        dst[j - i - 1] = WR
        dst[j - i] = NUL
        return True

    def del_nchar(self, x, y, n):
        """e_del_nchar: delete @n bytes at (y, x), reflowing a soft row."""
        self.count(n)
        buf = self.bufs[y]
        ln = self.str_len(y)
        if buf[ln] == WR:
            ln += 1
        for j in range(x, ln - n + 1):
            buf[j] = buf[j + n] if j + n < len(buf) else NUL
        m = self.marks()
        for p in m:
            if p[0] == y and p[1] > x:
                p[1] = p[1] - n if p[1] - n > x else x
        self.set_marks(m)
        self.st.fired.append("primitive_marks")
        if ln <= n:
            self.delete_line(y)
        elif y < self.nlines - 1 and buf[ln - n - 1] != WR:
            nxt = self.bufs[y + 1]
            nlen = self.str_len(y + 1)
            room = self.mx + n - ln
            if room > nlen:
                i = nlen
            else:
                i = su_rblk(room, self.cstr(y + 1))
            if nxt[i] == WR:
                i += 1
            if i > 0:
                self.st.fired.append("reflow_after_delete")
                for j in range(i):
                    buf[ln - n + j] = nxt[j]
                buf[ln - n + i] = NUL
                for j in range(ln - n + i + 1, len(buf)):
                    buf[j] = NUL
                m = self.marks()
                for p in m:
                    if p[0] == y + 1 and p[1] <= i:
                        p[0] -= 1
                        p[1] += ln - n
                self.set_marks(m)
                self.del_nchar(0, y + 1, i)
        return x + n

    def put_char(self, c):
        """e_put_char: overwrite the byte at the cursor (insert at the end)."""
        y, x = self.st.y, self.st.x
        if x == self.str_len(y):
            self.st.fired.append("overwrite_at_end_inserts")
            self.ins_nchar(bytes([c]), x, y)
        else:
            self.count(SAVE_PER_CHAR)
            self.bufs[y][x] = c
        self.st.x += 1


def su_lblk(xa, s):
    """e_su_lblk: from @xa past the word and the non-word after it."""
    ln = len(s)
    if ln == 0:
        return 0
    if xa >= ln:
        xa = ln - 1
    while xa < ln and _isalnum1(s[xa]):
        xa += 1
    while xa < ln and not _isalnum1(s[xa]):
        xa += 1
    return xa


def su_rblk(xa, s):
    """e_su_rblk: from @xa back to the start of the word before it."""
    ln = len(s)
    if xa <= 0:
        return xa
    if xa > ln:
        xa = ln
    xa -= 1
    while xa > 0 and not _isalnum1(s[xa]):
        xa -= 1
    if xa == 0:
        return 0
    while xa > 0 and _isalnum1(s[xa]):
        xa -= 1
    return 0 if xa == 0 else xa + 1


# ---- columns -----------------------------------------------------------------------

def _char_width(data, i):
    """(columns, bytes) of the UTF-8 character at data[i], 1 byte when invalid."""
    c = data[i]
    if c < UTF8_LEAD_MIN or c >= UTF8_LEAD_END:
        return 1, 1
    for nb in (4, 3, 2):
        try:
            ch = data[i:i + nb].decode("utf-8")
        except UnicodeDecodeError:
            continue
        if len(ch) == 1:
            w = 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1
            return w, nb
    return 1, 1


def visual_col(data, x, tabn):
    """The screen column of byte @x of a row (e_cursor's arithmetic)."""
    i = j = 0
    while i < x and i < len(data):
        c = data[i]
        if c == TAB:
            j += tabn - ((j + i) % tabn) - 1
        elif c <= CONTROL_MAX:
            j += 1
        elif UTF8_LEAD_MIN <= c < UTF8_LEAD_END:
            w, nb = _char_width(data, i)
            if nb > 1:
                j += w - nb
                i += nb - 1
        i += 1
    return x + j


def byte_at_col(data, col, tabn):
    """The byte index Up and Down land on for column @col (e_chr_sp)."""
    i = j = 0
    while i + j < col and i < len(data):
        c = data[i]
        if c == TAB:
            j += tabn - ((j + i) % tabn) - 1
        elif c <= CONTROL_MAX:
            j += 1
        elif UTF8_LEAD_MIN <= c < UTF8_LEAD_END:
            w, nb = _char_width(data, i)
            if nb > 1:
                j += w - nb
                i += nb - 1
        i += 1
    return i


def _tab_col(col, c, tabn):
    if c == TAB:
        return (col // tabn + 1) * tabn
    return col + 1


# ---- the actions -------------------------------------------------------------------

# Actions that change no text and push no step.
MOVES = ("left", "right", "up", "down", "home", "end", "file_start", "file_end",
         "word_left", "word_right", "page_up", "page_down", "window_top",
         "window_bottom")
SHIFT_MOVES = tuple("shift_" + m for m in MOVES)
EDITS = ("type", "enter", "tab", "backspace", "delete", "delete_word",
         "kill_to_eol", "delete_line")
MODES = ("insert_toggle", "literal_toggle")
OTHERS = ("save", "undo", "redo", "toggle_breakpoint")
ACTIONS = EDITS + MOVES + SHIFT_MOVES + MODES + OTHERS


class Action:
    """One user action of the model; @arg is the bytes typed for "type"."""

    def __init__(self, name, arg=None):
        assert name in ACTIONS, name
        self.name = name
        self.arg = arg

    def __repr__(self):
        if self.arg is None:
            return self.name
        return "%s(%r)" % (self.name, self.arg)


def apply(state, action):
    """The state after @action; @state itself is not changed."""
    st = state.copy()
    st.autosaved = False
    st.box = None
    name = action.name
    pre = (st.y, st.x)
    if name in MOVES or name in SHIFT_MOVES:
        _move(st, name)
    elif name in EDITS:
        _edit(st, action)
    elif name == "insert_toggle":
        _toggle_mode(st, INS_INSERT, "insert_toggle")
    elif name == "literal_toggle":
        _toggle_mode(st, INS_LITERAL, "alt_j_toggle")
    elif name == "save":
        _save(st)
    elif name == "undo":
        _undo(st)
    elif name == "redo":
        _redo(st)
    elif name == "toggle_breakpoint":
        _toggle_breakpoint(st)
    elif name in blocks.BLOCK_ACTIONS:
        blocks.apply_action(st, action)
    _after_key(st, name, pre)
    return st


def _after_key(st, name, pre):
    """What the key loop does after every key: the anchor, the clamp, the
    column memory, and the autosave check before the next key."""
    st.ks = (st.y, st.x)
    st.fired.append("anchor_after_key")
    if 0 <= st.y < len(st.rows):
        ln = len(st.rows[st.y].data)
        if st.x > ln:
            st.x = ln
            st.fired.append("cursor_clamped")
        if st.x < 0:
            st.x = 0
    base = name[len("shift_"):] if name.startswith("shift_") else name
    if base not in ("up", "down") and 0 <= st.y < len(st.rows):
        st.clsv = visual_col(st.rows[st.y].data, st.x, st.options.tabn)
    if st.save_known and st.save > st.options.maxchg:
        st.fired.append("autosave_threshold")
        st.save = 1
        if st.options.autosave:
            st.fired.append("autosave_asv")
            st.asv = render_disk(st)
            st.autosaved = True


def _locked(st):
    return st.ins == INS_READONLY


def _toggle_mode(st, bit, rule):
    if _locked(st):
        st.fired.append("locked_refuses")
        return
    st.fired.append(rule)
    st.ins ^= bit


# -- moves

def _move(st, name):
    shift = name.startswith("shift_")
    base = name[len("shift_"):] if shift else name
    rows = st.rows
    st.fired.append("cursor_keys")
    st.fired.append("cursor_detail")
    tabn = st.options.tabn
    last = len(rows) - 1
    y, x = st.y, st.x
    data = rows[y].data
    hard = rows[y].end == HARD
    cstr = data + (b"\n" if hard else b"")
    cy, cx, ay, ax, ey, ex = st.view
    height = ey - ay
    if base == "up":
        if y > 0:
            y -= 1
        x = byte_at_col(rows[y].data, st.clsv, tabn)
    elif base == "down":
        if y < last:
            y += 1
        x = byte_at_col(rows[y].data, st.clsv, tabn)
    elif base == "left":
        x -= 1
        while x > 0 and (data[x] & UTF8_CONT_MASK) == UTF8_CONT:
            x -= 1
        if x < 0:
            if y > 0:
                y -= 1
                x = len(rows[y].data)
            else:
                x = 0
    elif base == "right":
        if x < len(data):
            c = data[x]
            x += 1
            if UTF8_LEAD_MIN <= c < UTF8_LEAD_END:
                while x < len(data) and (data[x] & UTF8_CONT_MASK) == UTF8_CONT:
                    x += 1
        else:
            x += 1
        if x > len(data):
            if y < last:
                y += 1
                x = 0
            else:
                x = len(data)
    elif base == "word_left":
        if x <= 0 and y > 0:
            y -= 1
            x = len(rows[y].data)
        elif x > 0:
            x = su_rblk(x - 1, cstr)
    elif base == "word_right":
        if x >= len(data) and y < last:
            x = 0
            y += 1
        elif x < len(data):
            x = su_lblk(x, cstr)
    elif base == "home":
        x = 0
    elif base == "end":
        x = len(data)
    elif base == "file_start":
        if y != 0:
            x = 0
            y = 0
    elif base == "file_end":
        y = last
        x = len(rows[last].data)
    elif base == "page_down":
        y = y + height - 2
        if y > last:
            y = last
        x = min(x, len(rows[y].data))     # e_cursor runs inside the key
    elif base == "page_up":
        y = y - ey + ay + 2
        if y < 0:
            y = 0
        x = min(x, len(rows[y].data))
    elif base == "window_top":
        x = 0
        y = cy
    elif base == "window_bottom":
        bottom = height + cy - 1
        if bottom < len(rows):
            y = bottom - 1
        else:
            y = last
        if 0 <= y < len(rows):
            x = len(rows[y].data)
    st.y, st.x = y, x
    if shift:
        _shift_marks(st)


def _shift_marks(st):
    """The block after a Shift-move, from the anchor (the cursor before it)."""
    st.fired.append("shift_marks")
    st.fired.append("shift_marks_detail")
    ks = st.ks
    cur = (st.y, st.x)
    has_block = st.mb != st.me
    if ks == st.mb and has_block:
        st.mb = cur
    elif ks == st.me and has_block:
        st.me = cur
    elif ks < cur:
        st.mb, st.me = ks, cur
    else:
        st.mb, st.me = cur, ks


# -- edits

def _edit(st, action):
    if _locked(st):
        st.fired.append("locked_refuses")
        return
    before = Snapshot(st)
    rows = Rows(st)
    name = action.name
    line_record = False
    if name == "type":
        _type(st, rows, action.arg)
    elif name == "enter":
        _enter(st, rows)
    elif name == "tab":
        _tab(st, rows)
    elif name == "backspace":
        line_record = _backspace(st, rows)
    elif name == "delete":
        _delete(st, rows)
    elif name == "delete_word":
        _delete_word(st, rows)
    elif name == "kill_to_eol":
        _kill_to_eol(st, rows)
    elif name == "delete_line":
        line_record = True
        _delete_line(st, rows)
    rows.commit()
    if [(r.end, r.data) for r in before.rows] != st.lines():
        _push_step(st, before, action, line_record)


def _type(st, rows, data):
    """A typed key: its bytes, or in Alt-J's mode a control key's byte."""
    y, x = st.y, st.x
    if len(data) == 1 and data[0] <= CONTROL_MAX and data[0] != TAB:
        st.fired.append("literal_mode_inserts_controls")
    if len(data) > 1 or data[0] > ASCII_MAX:
        st.fired.append("type_multibyte_inserts")
        rows.ins_nchar(data, x, y)
        return
    if st.ins & INS_INSERT:
        st.fired.append("type_insert")
        rows.ins_nchar(data, x, y)
    else:
        st.fired.append("type_overwrite")
        rows.put_char(data[0])


def _enter(st, rows):
    st.fired.extend(["enter_splits", "enter_cursor_next_row", "enter_one_step"])
    y, x = st.y, st.x
    ln = rows.str_len(y)
    if x == ln and not rows.hard(y):
        st.fired.append("enter_soft_end_hardens")
        rows.bufs[y][x] = WR
        rows.bufs[y][x + 1] = NUL
    else:
        rows.split_line(y, x)
    rows.count(SAVE_PER_CHAR)
    if y < rows.nlines - 1:
        st.y, st.x = y + 1, 0
    if st.flg & FLG_AUTOINDENT:
        _enter_indent(st, rows)


def _enter_indent(st, rows):
    """e_car_a_ind: the new row's indent from the line above."""
    st.fired.append("enter_autoindent")
    y = st.y
    if y == 0:
        return
    tabn = st.options.tabn
    j = y
    while True:
        j -= 1
        s = rows.content(j)
        i = k = 0
        while k < len(s) and (_isspace(s[k]) or (i < len(s) and s[i] == BRACE_OPEN)):
            if not _isspace(s[k]) or s[k] == TAB:
                st.fired.append("enter_indent_quirks")
            i = _tab_col(i, s[k], tabn)
            k += 1
        nxt = s[k] if k < len(s) else NUL
        if not (j > 0 and nxt == HASH):
            break
        st.fired.append("enter_indent_quirks")
    if k == len(s) and k > 0 and s[k - 1] == BRACE_OPEN:
        st.fired.append("enter_indent_quirks")
        i -= 1
    if i > 0:
        rows.ins_nchar(b" " * i, 0, y)
        st.x = i


def _first_nospace(s):
    for k, c in enumerate(s):
        if not _isspace(c):
            return k
    return -1


def _x_of_char(s, k_want, tabn):
    x = 0
    for k, c in enumerate(s):
        if k == k_want:
            return x
        x = _tab_col(x, c, tabn)
    return 0


def _char_of_x(s, want, tabn):
    x = 0
    for c in s:
        nx = _tab_col(x, c, tabn)
        if nx > want:
            return x
        x = nx
    return 0


def _tab(st, rows):
    if not st.flg & FLG_AUTOINDENT:
        st.fired.append("tab_literal")
        if st.ins & INS_INSERT:
            rows.ins_nchar(b"\t", st.x, st.y)
        else:
            rows.put_char(TAB)
        return
    st.fired.extend(["tab_autoindent", "tab_autoindent_manual", "tab_one_step"])
    tabn = st.options.tabn
    line = st.y
    s = rows.content(line)
    fk = _first_nospace(s)
    do_ai = False
    if fk > -1:
        if st.x > _x_of_char(s, fk, tabn):
            do_ai = True
            for k in range(_char_of_x(s, st.x, tabn), len(s)):
                if not _isspace(s[k]):
                    do_ai = False
                    break
            if do_ai:
                rows.del_nchar(st.x, line, len(s) - st.x)
        else:
            do_ai = True
    else:
        do_ai = True
        if len(s) > 0:
            rows.del_nchar(st.x, line, len(s) - st.x)
    if not do_ai:
        ins = b"\t"
    else:
        x = 0
        for ln in range(st.y - 1, 0, -1):
            t = rows.content(ln)
            k = _first_nospace(t)
            if k > -1:
                x = _x_of_char(t, k, tabn)
                break
        col = st.options.autoindent_col
        if st.x < x:
            ins = b" " * (x - st.x)
        elif st.x < x + col:
            ins = b" " * (x + col - st.x)
        else:
            ins = b"\t"
    rows.ins_nchar(ins, st.x, st.y)


def _back_over_char(data, x):
    """The byte where the character before @x starts."""
    x -= 1
    while x > 0 and (data[x] & UTF8_CONT_MASK) == UTF8_CONT:
        x -= 1
    return x


def _backspace(st, rows):
    """Backspace; returns True when a whole line went (a line record)."""
    st.fired.append("backspace_char")
    y, x = st.y, st.x
    if y == 0 and x == 0:
        return False
    data = rows.content(y)
    if len(data) == 0:
        st.fired.extend(["backspace_joins", "backspace_cursor_rows"])
        rows.delete_line(y)
        st.y = y - 1
        st.x = rows.str_len(st.y)
        if not rows.hard(st.y):
            st.x -= 1
        return True
    autoindent = (st.flg & FLG_AUTOINDENT) != 0
    if x > 0:
        nx = _back_over_char(data, x)
        st.x = nx
        if autoindent and all(_isspace(c) for c in data[:x]) and y > 0:
            _backspace_reindent(st, rows, x)
        else:
            rows.del_nchar(nx, y, x - nx)
        return False
    st.fired.extend(["backspace_joins", "backspace_cursor_rows"])
    st.y = y - 1
    st.x = rows.str_len(st.y)
    if not rows.hard(st.y):
        st.x -= 1
    rows.del_nchar(st.x, st.y, 1)
    return False


def _backspace_reindent(st, rows, x0):
    """Backspace in front of the first visible character with auto-indent:
    the line goes back to the indent of the last line above that started
    before the cursor (e_del_a_ind once the branch is taken)."""
    st.fired.extend(["backspace_autoindent", "backspace_indent_one_step"])
    tabn = st.options.tabn
    y = st.y
    bx = st.x
    s = rows.content(y)
    col = 0
    j = 0
    while j <= bx:
        c = s[j] if j < len(s) else NUL
        col = _tab_col(col, c, tabn)
        j += 1
    if col != j:
        st.fired.append("backspace_autoindent_detail")
        rows.del_nchar(0, y, j)
        rows.ins_nchar(b" " * col, 0, y)
        bx = col - 1
    found = False
    for jj in range(y - 1, -1, -1):
        t = rows.content(jj)
        i = k = 0
        while k < len(t) and _isspace(t[k]):
            i = _tab_col(i, t[k], tabn)
            k += 1
        if k < len(t) and t[k] != HASH and i <= bx:
            n = bx - i + 1
            bx -= n - 1
            found = True
            break
    if not found:
        st.fired.append("backspace_autoindent_detail")
        bx = 0
        n = 1
    st.x = bx
    rows.del_nchar(bx, y, n)


def _delete(st, rows):
    st.fired.append("delete_char")
    y, x = st.y, st.x
    buf = rows.bufs[y]
    if buf[x] == NUL or (y >= rows.nlines - 1 and buf[x] == WR):
        st.fired.append("delete_edges")
        return
    data = rows.content(y)
    n = 1
    if x < len(data) and UTF8_LEAD_MIN <= data[x] < UTF8_LEAD_END:
        pos = x + 1
        while pos < len(data) and (data[pos] & UTF8_CONT_MASK) == UTF8_CONT:
            pos += 1
        n = pos - x
    rows.del_nchar(x, y, n)


def _delete_word(st, rows):
    y, x = st.y, st.x
    data = rows.content(y)
    if y == rows.nlines - 1 and x >= len(data):
        st.fired.append("ctrl_t_word")      # no next word at the end of the text
        return
    st.fired.extend(["ctrl_t_word", "ctrl_t_extent"])
    if x < len(data):
        c = su_lblk(x, rows.cstr(y))
        rows.del_nchar(x, y, c - x)
    elif rows.bufs[y][x] == WR:
        rows.del_nchar(x, y, 1)
    elif x >= len(data) and y < rows.nlines - 1:
        st.x, st.y = 0, y + 1


def _kill_to_eol(st, rows):
    st.fired.append("kill_to_eol")
    y, x = st.y, st.x
    n = rows.str_len(y) - x
    if x == 0 and not rows.hard(y):
        st.fired.append("kill_soft_row_whole")
    rows.del_nchar(x, y, n)


def _delete_line(st, rows):
    st.fired.extend(["ctrl_y_line", "ctrl_y_cursor"])
    if rows.nlines == 1:
        st.fired.append("ctrl_y_only_line")
    rows.delete_line(st.y)
    if st.y > rows.nlines - 1:
        st.y -= 1


# -- undo

def _undo_limit(st):
    return st.options.numundo + UNDO_EXTRA_STEPS


def _push_step(st, before, action, line_record):
    st.fired.extend(["one_step_per_action", "undo_bound", "undo_bound_extra",
                     "fresh_change_clears_redo", "line_identity"])
    st.undo.append(Step(before, Snapshot(st), action, line_record, st.ins))
    st.redo = []
    st.opaque_redo = 0
    while st.ud > _undo_limit(st):
        if st.opaque_undo:
            st.opaque_undo -= 1
        else:
            st.undo.pop(0)


def _restore(st, snap):
    st.rows = [Row(r.data, r.end, r.ident) for r in snap.rows]
    st.y, st.x = snap.cursor


def _text_offsets(rows):
    """The logical text of rows and where each row starts in it."""
    out = bytearray()
    starts = []
    for r in rows:
        starts.append(len(out))
        out += r.data
        if r.end == HARD:
            out += b"\n"
    return bytes(out), starts


def _offset_point(rows, starts, off, is_end_of_break):
    """(row, col) of a text offset; an offset just past a hard break that
    ends a span is reported as len+1 on the break's row, as the editor marks
    a restored line break."""
    for i in range(len(rows) - 1, -1, -1):
        if starts[i] <= off:
            col = off - starts[i]
            if is_end_of_break and col == 0 and i > 0 and rows[i - 1].end == HARD:
                return (i - 1, len(rows[i - 1].data) + 1)
            return (i, col)
    return (0, 0)


def _span(short_rows, long_rows):
    """Where the longer text holds bytes the shorter one lacks, if it is one
    contiguous insertion: (start, end) offsets in the longer text."""
    a, _ = _text_offsets(short_rows)
    b, _ = _text_offsets(long_rows)
    if len(b) <= len(a):
        return None
    p = 0
    while p < len(a) and a[p] == b[p]:
        p += 1
    q = 0
    while q < len(a) - p and a[len(a) - 1 - q] == b[len(b) - 1 - q]:
        q += 1
    if p + q < len(a):
        return None
    return (p, len(b) - q)


def _marks_on_inserted(st, short_rows, long_rows, fallback):
    """The marks after a replay that inserted text: that text; else @fallback."""
    sp = _span(short_rows, long_rows)
    if sp is None:
        st.mb, st.me = fallback
        return
    text, starts = _text_offsets(long_rows)
    ends_break = sp[1] > sp[0] and text[sp[1] - 1:sp[1]] == b"\n"
    st.mb = _offset_point(long_rows, starts, sp[0], False)
    st.me = _offset_point(long_rows, starts, sp[1], ends_break)


def _undo(st):
    if _locked(st):
        st.fired.append("locked_refuses")
        return
    st.fired.extend(["undo_text", "undo_count_unpredicted"])
    if not st.undo:
        if st.opaque_undo:
            st.opaque_undo -= 1
            st.opaque_redo += 1
            st.save_known = False
        else:
            st.fired.append("undo_empty_says_so")
            st.box = BOX_NO_UNDO
        return
    step = st.undo.pop()
    st.redo.append(step)
    _restore(st, step.before)
    st.save_known = False
    if step.undo_marks is not None:
        st.fired.extend(["undo_cursor_snapshot", "block_undo_marks"])
        st.mb, st.me = step.undo_marks
    elif step.line_record:
        st.fired.append("undo_line_cursor")
        st.mb, st.me = step.before.mb, step.before.me
    else:
        st.fired.extend(["undo_cursor_snapshot", "undo_marks_span"])
        _marks_on_inserted(st, step.after.rows, step.before.rows,
                           (step.before.mb, step.before.me))


def _redo(st):
    if _locked(st):
        st.fired.append("locked_refuses")
        return
    st.fired.extend(["redo_text", "undo_count_unpredicted"])
    if not st.redo:
        if st.opaque_redo:
            st.opaque_redo -= 1
            st.opaque_undo += 1
            st.save_known = False
        else:
            st.fired.append("undo_empty_says_so")
            st.box = BOX_NO_REDO
        return
    step = st.redo.pop()
    st.undo.append(step)
    _restore(st, step.after)
    st.save_known = False
    st.fired.extend(["undo_cursor_snapshot", "undo_marks_span"])
    if step.undo_marks is not None or step.line_record:
        st.mb, st.me = step.after.mb, step.after.me
    else:
        _marks_on_inserted(st, step.before.rows, step.after.rows,
                           (step.after.mb, step.after.me))


# -- save and breakpoints

def _save(st):
    if _locked(st):
        st.fired.append("save_fails_read_only")
        st.box = BOX_SAVE_FAILED
        st.resync_fields.add(F_DISK)
        return
    st.fired.extend(["save_writes_file", "save_format", "modified_count"])
    st.disk = render_disk(st)
    st.save = 0
    st.save_known = True


def _toggle_breakpoint(st):
    st.fired.append("line_identity")
    ident = st.rows[st.y].ident
    if ident in st.brp:
        st.brp.discard(ident)
    else:
        st.brp.add(ident)


def render_disk(st):
    """The file a Save writes from the state's rows."""
    from .render import render_disk as _rd
    return _rd(st.lines())


def rules_for(fired, field):
    """The rules among @fired that decide @field, without repeats."""
    out = []
    for name in fired:
        r = RULES[name]
        if field in r.fields and r not in out:
            out.append(r)
    return out


def classify(fired, field):
    """``editor`` when every rule that predicted @field specifies it (a
    manual, the chapter or the user's decision), else ``unspecified``."""
    rules = rules_for(fired, field)
    if rules and all(r.source in SPECIFYING_SOURCES for r in rules):
        return "editor", rules
    return "unspecified", rules


# The blocks, the clipboard, Replace and the option dialog: their rules
# register themselves in RULES, their actions join the model's.
from . import blocks  # noqa: E402

ACTIONS = ACTIONS + blocks.BLOCK_ACTIONS
