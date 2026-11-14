"""Blocks, the clipboard, Search > Replace and the option dialog, as actions
of the editing model.

This is the second half of ``model.py``: the same ``apply(state, action)``
function, the same rule registry with a source on every rule, the same
rows as the editor keeps them.  A block is read as a span of the window's
text, with soft-wrapped rows joined and a line break after each hard row;
a copy, a move, a delete or a paste changes that text and the lines it
touches are wrapped again as the file reader wraps them (the compatibility
chapter's "Where a moved or pasted block wraps").  The character-level
actions (indent, unindent, the help-file markers, a replacement) use the
same row machine as typing, so a wrap lands where the editor's does.

What is not modelled, and is therefore resynchronised from the editor's
dump after the action: where a breakpoint goes when a block is moved
(``F_BREAKPOINTS``), the file a block is read from or written to (Ctrl-K R
and W belong to the File Manager), and a beautify.
"""
# ---- the actions (named before the model is imported: the model imports this
# module for its action list, and either may be imported first) --------------

BLOCK_MARKS = ("block_begin", "block_end", "block_hide", "block_goto_begin",
               "block_goto_end", "block_line", "block_all")
BLOCK_EDITS = ("block_copy", "block_move", "block_delete", "block_indent",
               "block_unindent", "help_button", "help_highlight", "help_header",
               "help_end")
CLIPBOARD = ("edit_copy", "edit_cut", "edit_paste")
MARKERS = ("marker_set", "marker_goto")
DIALOGS = ("replace_all", "option_set")
BLOCK_ACTIONS = BLOCK_MARKS + BLOCK_EDITS + CLIPBOARD + MARKERS + DIALOGS

from . import model  # noqa: E402
from .model import (HARD, SOFT, NUL, SPACE, TAB, DASH, SAVE_PER_CHAR, Rows, Row,  # noqa: E402
                    Snapshot, UTF8_CONT_MASK, UTF8_CONT)

# What Ctrl-O U, M and H put around a block inside one row (keys.h).
HELP_END = 0x05
HELP_BUTTON = 0x01
HELP_HIGHLIGHT = 0x0F
HELP_HEADER = 0x02
HELP_PAGE_END = 0x06

# Where Ctrl-K B puts the block's start when the mode keeps its end.
POINT_AT_START = 0
POINT_AT_END = 1

# The option dialog's fields, by the name an action gives them.
OPTION_FIELDS = ("maxcol", "tabn", "maxchg", "numundo", "autoindent_col")
# Enough Change All replacements to say the editor looped; the real loop
# ends when no occurrence is left.
REPLACE_LIMIT = 5000

MSG_NOT_FOUND = "String NOT found"

BOX_FOUND = "Found %d Replaced %d"
BOX_FOUND_REFUSED = "Found %d Replaced 0 " + model.STATUS_READ_ONLY

SPECIFIC = model.SPECIFYING_SOURCES

HELP = model.HELP
EDITOR = model.EDITOR
COMPAT = model.COMPAT
DECIDED = model.DECIDED_ONE_STEP

_r = model._rule

# -- marking
_r("block_marks_keys", model.SOURCE_MANUAL, HELP + ":626-631, 635",
   "Ctrl-K B and K set the beginning and the end mark, A and Z jump to them, "
   "L marks the line, X the whole text, H hides the marks.", (model.F_MARKS, model.F_CURSOR))
_r("block_begin_mode", model.SOURCE_CODE, "we_block.c:1812-1815",
   "Begin Mark starts an empty block at the cursor; in WordStar mode it moves "
   "only the beginning.", (model.F_MARKS,))
_r("block_line_end", model.SOURCE_CODE, "we_edit.c:1493-1505",
   "Mark Line ends at the start of the next line, or at the end of the last "
   "line.", (model.F_MARKS,))
_r("block_hide_resets", model.SOURCE_CODE, "we_block.c:1790-1791",
   "Hide sets both marks to the start of the text.", (model.F_MARKS,))
_r("block_empty", model.SOURCE_CODE, "we_block.c:85-88",
   "A block whose end is not after its beginning holds nothing, so Copy, Move, "
   "Delete and Copy to the clipboard do nothing with it.", (model.F_TEXT,))
# -- copy, move, delete
_r("block_copy_text", model.SOURCE_MANUAL, HELP + ":632,887",
   "Ctrl-K C copies the block to the cursor.", (model.F_TEXT,))
_r("block_move_text", model.SOURCE_MANUAL, HELP + ":633,887",
   "Ctrl-K V moves the block to the cursor.", (model.F_TEXT,))
_r("block_delete_text", model.SOURCE_MANUAL, HELP + ":634,887",
   "Ctrl-K Y kills the block.", (model.F_TEXT,))
_r("block_copy_lands_inside", model.SOURCE_CODE, "we_block.c:1592-1602",
   "A copy that would land inside its own block is refused.", (model.F_TEXT,))
_r("block_move_into_itself", model.SOURCE_CODE, "we_block.c:1239-1269",
   "A move with the cursor inside the block, or a block of several lines "
   "dropped on one of its own lines, is refused.", (model.F_TEXT,))
_r("block_rewrap", model.SOURCE_CHAPTER, COMPAT + ":324-328",
   "A line a block operation makes, with the rows it goes on to, is wrapped "
   "where the file reader wraps a long line.", (model.F_TEXT,))
_r("block_marks_after", model.SOURCE_CODE, "we_block.c:1355-1400",
   "After a copy, a move or a paste the block is marked where it landed and "
   "the cursor is at its end; after a delete both marks and the cursor are "
   "where the block began.", (model.F_MARKS, model.F_CURSOR))
_r("block_in_line_delete", model.SOURCE_CODE, "we_block.c:244-248",
   "A block inside one row is deleted by the character primitive: its marks "
   "close up and the cursor column goes to where the block began.",
   (model.F_TEXT, model.F_MARKS, model.F_CURSOR))
_r("block_save_count", model.SOURCE_CODE, "we_block.c:272, 583, 608, 659",
   "A copy, a move, a paste and a block over several rows set the modified "
   "count to Max. Changes + 1.", (model.F_SAVE,))
_r("block_one_step", model.SOURCE_DESIGN, model.DESIGN_ONE_STEP,
   "A block copy, move, delete or paste is one Ctrl-U.", (model.F_STEPS,))
_r("block_undo_marks", model.SOURCE_CODE, "probe of the copy and paste Undo, 2026-11-12",
   "Undo of a copy or a paste leaves both marks at the start of the text it "
   "took away.", (model.F_MARKS,))
_r("block_move_breakpoints", model.SOURCE_CHAPTER, COMPAT + ":149-158",
   "A move carries the breakpoints of its lines; the model does not follow "
   "them and takes the editor's rows.", (model.F_BREAKPOINTS,))
_r("block_locked", model.SOURCE_CHAPTER, COMPAT + ":276-282; " + EDITOR + ":86-94",
   "In a read-only window Ctrl-K I, U, D, F, Ctrl-O B, L, U, M, H, E and "
   "Replace are refused and the status row says the text is unchanged; the "
   "block Delete, Move and Paste do nothing; Copy still works, so Cut copies "
   "and leaves the text.", (model.F_TEXT, model.F_STATUS))
# -- indent
_r("indent_keys", model.SOURCE_MANUAL, HELP + ":362,638-639",
   "Ctrl-K I shifts the block to the right, Ctrl-K U to the left.",
   (model.F_TEXT,))
_r("indent_width", model.SOURCE_CODE, "we_block.c:2058, 2106",
   "A shift is Tabstops / 2 columns, on every row that starts inside the "
   "block (the first only when the block begins at column 0).",
   (model.F_TEXT, model.F_MARKS, model.F_CURSOR))
_r("indent_one_step", model.SOURCE_DECISION, DECIDED,
   "Ctrl-K I and Ctrl-K U are one Ctrl-U for the whole block.", (model.F_STEPS,))
_r("unindent_puts_back_blanks", model.SOURCE_CODE, "we_block.c:2058-2077, 2122",
   "When the last thing a shift to the left takes is a tab worth more "
   "columns than the shift, the columns beyond it come back as blanks: "
   "the row moves exactly Tabstops / 2 to the left.", (model.F_TEXT,))
_r("indent_blank_row", model.SOURCE_CODE, "we_block.c:2127",
   "The blanks of a shift go after the row's own leading blanks and never "
   "past its end: a row of only blanks gets them after its last one.",
   (model.F_TEXT,))
# -- help-file markers
_r("help_markers", model.SOURCE_MANUAL, HELP + ":817-821",
   "Ctrl-O U, M and H insert a button, a highlight marker or a header around "
   "the text block.", (model.F_TEXT,))
_r("help_markers_detail", model.SOURCE_CODE, "we_edit.c:1717-1746",
   "Only a block inside one row gets them: the marker byte goes in front, "
   "the end byte after, and the cursor ends just past the marker byte.",
   (model.F_TEXT, model.F_CURSOR))
_r("help_end_line", model.SOURCE_CHAPTER, COMPAT + ":284-289",
   "Ctrl-O E puts the end-of-page mark on a line of its own in the file, "
   "above the cursor's line.", (model.F_TEXT, model.F_CURSOR))
_r("help_end_one_step", model.SOURCE_CHAPTER, COMPAT + ":284-289",
   "One Ctrl-U takes that line back.", (model.F_STEPS,))
_r("help_markers_one_step", model.SOURCE_DECISION, DECIDED,
   "Ctrl-O U, M and H are one Ctrl-U each.", (model.F_STEPS,))
# -- clipboard
_r("clip_copy", model.SOURCE_CODE, "we_block.c:494-544",
   "Copy puts the block in the clipboard window (nothing when the block is "
   "empty) and leaves the modified count as it was.", (model.F_CLIP, model.F_SAVE))
_r("clip_paste", model.SOURCE_CODE, "we_block.c:573-610",
   "Paste copies the whole clipboard to the cursor (nothing when it is "
   "empty).", (model.F_TEXT,))
_r("clip_cut", model.SOURCE_MANUAL, HELP + ":320",
   "Cut is Copy followed by deleting the block.", (model.F_TEXT, model.F_CLIP))
# -- markers
_r("marker_keys", model.SOURCE_MANUAL, HELP + ":645-646; " + EDITOR + ":204-211",
   "Ctrl-K n sets marker n and Ctrl-O n jumps to it, n from 0 to 9; each "
   "marker keeps its own place.", (model.F_MARKERS,))
_r("marker_place", model.SOURCE_CODE, "we_edit.c:1578-1582, 1768-1770",
   "A marker is the cursor's place when it is set; a jump to a marker never "
   "set goes to the start of the text.", (model.F_MARKERS, model.F_CURSOR))
# -- replace
_r("replace_all_text", model.SOURCE_MANUAL, HELP + ":657, 1213-1219",
   "Change All replaces the string everywhere, from the start of the text.",
   (model.F_TEXT,))
_r("replace_scan", model.SOURCE_CODE, "we_block.c:2152-2241; we_hfkt.c:43-70",
   "The scan goes row by row from the cursor; a match never spans two rows; "
   "case is ignored by default; the next search starts after the replacement.",
   (model.F_TEXT, model.F_CURSOR))
_r("replace_by_primitives", model.SOURCE_CODE, "we_edit.c:2934-2945",
   "Each replacement is a delete and an insert by the character primitives, "
   "so a replacement that makes its row wrap wraps as typing does.",
   (model.F_TEXT, model.F_MARKS))
_r("replace_one_step", model.SOURCE_DECISION, DECIDED,
   "Change All is one Ctrl-U, whatever Num. Undo is.", (model.F_STEPS,))
_r("replace_message", model.SOURCE_CODE, "we_block.c:2447-2453",
   "Change All ends with a box that says how many it found and replaced, or "
   "that the string was not found.", (model.F_BOX,))
# -- options
_r("option_dialog_numbers", model.SOURCE_MANUAL, HELP + ":1570-1595",
   "Max. Columns, Tabstops, Max. Changes, Num. Undo and Auto Ind. Col. take "
   "effect when the dialog is closed with Ok.", (model.F_TEXT,))
_r("option_width_open", model.SOURCE_CODE, "we_edit.c:404; we_opt.c:3381",
   "Max. Columns changes the width of windows opened afterwards, not of the "
   "ones already open.", (model.F_TEXT,))


# ---- the text as one string ------------------------------------------------------

def _point_offset(st, y, x):
    """Where (y, x) falls in the text read as one string; a column past a
    hard row's end is the line break's place, never beyond it."""
    rows = st.rows
    y = max(0, min(y, len(rows) - 1))
    text, starts = model._text_offsets(rows)
    row = rows[y]
    limit = len(row.data) + (1 if row.end == HARD else 0)
    return starts[y] + max(0, min(x, limit))


def _offset_point(st, off, which):
    """The (row, column) of text offset @off; on the boundary between a
    soft row and the next, the block's start is on the next row and its end
    stays at the end of the first (we_block.c, e_blk_point_on_rows)."""
    rows = st.rows
    text, starts = model._text_offsets(rows)
    off = max(0, min(off, len(text)))
    for i in range(len(rows) - 1, -1, -1):
        if starts[i] <= off:
            col = off - starts[i]
            if (which == POINT_AT_END and col == 0 and i > 0
                    and rows[i - 1].end == SOFT):
                return (i - 1, len(rows[i - 1].data))
            return (i, min(col, len(rows[i].data) + (1 if rows[i].end == HARD else 0)))
    return (0, 0)


def _logical_lines(rows):
    """[(first row, last row, bytes, ends with a break)] for each line the
    user wrote: a soft row goes on in the row after it."""
    out = []
    i = 0
    while i < len(rows):
        j = i
        data = bytearray(rows[i].data)
        while rows[j].end == SOFT and j + 1 < len(rows):
            j += 1
            data += rows[j].data
        out.append((i, j, bytes(data), rows[j].end == HARD))
        i = j + 1
    return out


def wrap_cut(piece, maxc):
    """Where the file reader breaks a piece that does not fit: just after a
    blank or a dash, else before the limit, never inside a UTF-8 character
    (we_buffer.c, buffer_wrap_cut)."""
    brk = maxc
    while brk > 0 and piece[brk - 1] not in (SPACE, DASH):
        brk -= 1
    if brk > 0:
        return brk
    cut = maxc
    while cut > 1 and (piece[cut] & UTF8_CONT_MASK) == UTF8_CONT:
        cut -= 1
    return cut


def wrap_reader(piece, width, terminated):
    """The rows the file reader makes of one logical line: every row but the
    last soft, the last hard when the line ends with a break.  A row holds
    up to Max. Columns bytes, so a line exactly that long is one row."""
    maxc = max(1, width)
    out = []
    start = 0
    while True:
        seg = len(piece) - start
        hard = True
        if seg > maxc:
            seg = wrap_cut(piece[start:], maxc)
            hard = False
        end = HARD if (hard and terminated) else SOFT
        out.append((piece[start:start + seg], end))
        start += seg
        if start >= len(piece):
            return out


def _pieces(segment):
    """[(bytes, ends with a break)] for a stretch of text; a final piece
    with no break is the last line of a file with no final newline."""
    parts = segment.split(b"\n")
    out = [(p, True) for p in parts[:-1]]
    if parts[-1]:
        out.append((parts[-1], False))
    return out


def splice_text(st, a, b, ins, first_row=None):
    """Replace the text between offsets @a and @b by @ins.  The lines it
    touches, with the rows they go on to, are made again by the file
    reader's wrap; every other row stays as it is.  With @first_row (a row
    of the line @a is on), the rows of that line above it keep their
    breaks and the wrap starts there.  The first row of the stretch keeps
    its line's identity, except that bytes put in at column 0 leave the old
    line, with its identity, below them."""
    rows = st.rows
    text, starts = model._text_offsets(rows)
    lines = _logical_lines(rows)

    def line_of(off):
        for k, (fr, lr, data, term) in enumerate(lines):
            end = starts[fr] + len(data) + (1 if term else 0)
            if off < end:
                return k
        return len(lines) - 1

    la, lb = line_of(a), line_of(b)
    fr, lr = lines[la][0], lines[lb][1]
    if first_row is not None and fr <= first_row <= lines[la][1]:
        fr = first_row
    last = lines[lb]
    s0 = starts[fr]
    e0 = starts[last[0]] + len(last[2]) + (1 if last[3] else 0)
    seg = text[s0:e0]
    new_seg = seg[:a - s0] + ins + seg[b - s0:]
    new_rows = []
    counts = []
    for data, term in _pieces(new_seg):
        made = wrap_reader(data, st.width, term)
        counts.append(len(made))
        new_rows.extend(made)
    if not new_rows:
        new_rows = [(b"", SOFT)]
        counts = [1]
    old_ident = rows[fr].ident
    first_owner = 0
    at_line_start = (a == s0 and a == b and ins.endswith(b"\n"))
    if at_line_start:
        first_owner = sum(counts[:ins.count(b"\n")])
        first_owner = min(first_owner, len(new_rows) - 1)
    built = []
    for k, (data, end) in enumerate(new_rows):
        if k == first_owner:
            ident = old_ident
        else:
            ident = st.next_ident
            st.next_ident += 1
        built.append(Row(data, end, ident))
    st.rows = rows[:fr] + built + rows[lr + 1:]
    st.fired.append("block_rewrap")


# ---- the block -----------------------------------------------------------------

def _marks_in_text(st):
    """Are both marks on rows the text has?  An editor whose marks left the
    text (a finding of its own, filed by the inside check) leaves the model
    no block to work on."""
    return 0 <= st.mb[0] < len(st.rows) and 0 <= st.me[0] < len(st.rows)


def _block_empty(st):
    return st.me <= st.mb or not _marks_in_text(st)


def _block_span(st):
    return (_point_offset(st, st.mb[0], st.mb[1]), _point_offset(st, st.me[0], st.me[1]))


def _set_marks(st, mb, me):
    st.mb, st.me = mb, me


def _land(st, start, length):
    """Mark the text that now stands at offsets [start, start + length) as
    the block and put the cursor at its end."""
    st.mb = _offset_point(st, start, POINT_AT_START)
    st.me = _offset_point(st, start + length, POINT_AT_END)
    st.y, st.x = st.me
    st.fired.append("block_marks_after")


def _commit(st, before, action, undo_marks=None):
    """Push the one undo step of an action that changed the text."""
    if [(r.end, r.data) for r in before.rows] != st.lines():
        model._push_step(st, before, action, False)
        st.undo[-1].undo_marks = undo_marks
        if not any(r in st.fired for r in model.ONE_STEP_RULES):
            st.fired.append("block_one_step")


def _mark_actions(st, name):
    st.fired.append("block_marks_keys")
    rows = st.rows
    if name == "block_begin":
        st.fired.append("block_begin_mode")
        if st.options.wordstar:
            st.mb = (st.y, st.x)
        else:
            st.mb = st.me = (st.y, st.x)
    elif name == "block_end":
        st.me = (st.y, st.x)
    elif name == "block_hide":
        st.fired.append("block_hide_resets")
        st.mb = st.me = (0, 0)
    elif name == "block_goto_begin":
        st.y, st.x = st.mb
    elif name == "block_goto_end":
        st.y, st.x = st.me
    elif name == "block_line":
        st.fired.append("block_line_end")
        st.mb = (st.y, 0)
        if st.y < len(rows) - 1:
            st.me = (st.y + 1, 0)
        else:
            st.me = (st.y, len(rows[st.y].data))
    elif name == "block_all":
        st.mb = (0, 0)
        st.me = (len(rows) - 1, len(rows[-1].data))


# -- copying rows: the owner's way ------------------------------------------------------

def _part(row, a, b=None):
    """Bytes @a..@b of a row with its line break (when hard) as the last
    byte, as e_blk_part reads them."""
    full = row.data + (b"\n" if row.end == HARD else b"")
    return full[a:b] if b is not None else full[a:]


def _line_rows(text, width):
    """The rows one line of @text makes: wrapped as the file reader wraps,
    ended by a break when @text ends with one (e_buffer_lines_from_parts)."""
    term = text.endswith(b"\n")
    body = text[:-1] if term else text
    return wrap_reader(body, width, term)


def _continuation(rows, y, exclude):
    """How many rows after row @y go on from it (a soft-wrapped line), not
    counting rows of @exclude."""
    n = 0
    r = y
    while rows[r].end == SOFT and r + 1 < len(rows) and (r + 1) not in exclude:
        n += 1
        r += 1
    return n


def _rows_text(rows, a, b):
    return b"".join(_part(rows[r], 0) for r in range(a, b))


def copy_rows(dest, width, y, x, src, block, exclude=()):
    """Copy the block ``(kay, kax), (key, kex)`` of the rows @src to column
    @x of row @y of the rows @dest, the way e_blk_plan_copy does it: the
    lines where the block's two ends meet the text are made again and
    wrapped as the file reader wraps; the rows between the block's first
    and last stay as they are.  Returns ``(y0, y1, built, length)``: rows
    @y0 up to @y1 of @dest are replaced by @built, a list of
    ``(bytes, end, keeps_identity)``, and the block's text has @length bytes."""
    (kay, kax), (key, kex) = block
    x = max(0, min(x, len(dest[y].data)))
    n = _continuation(dest, y, exclude)
    cont = _rows_text(dest, y + 1, y + 1 + n)
    row = dest[y]
    if kay == key:
        chars = _part(src[kay], kax, kex)
        line = _part(row, 0, x) + chars + _part(row, x) + cont
        built = [(d, e, k == 0) for k, (d, e) in enumerate(_line_rows(line, width))]
        return y, y + 1 + n, built, len(chars)
    first = _part(src[kay], kax)
    mid = [(src[r].data, src[r].end, False) for r in range(kay + 1, key)]
    last = _part(src[key], 0, kex)
    length = len(first) + sum(len(d) + (1 if e == HARD else 0) for d, e, _k in mid) + len(last)
    if x == 0:
        built = [(d, e, False) for d, e in _line_rows(first, width)] + mid
        if kex == 0:
            return y, y, built, length
        tail = _line_rows(last + _part(row, 0) + cont, width)
        built += [(d, e, k == 0) for k, (d, e) in enumerate(tail)]
        return y, y + 1 + n, built, length
    head = _line_rows(_part(row, 0, x) + first, width)
    tail = _line_rows(last + _part(row, x) + cont, width)
    built = ([(d, e, k == 0) for k, (d, e) in enumerate(head)] + mid
             + [(d, e, False) for d, e in tail])
    return y, y + 1 + n, built, length


def _apply_built(st, y0, y1, built):
    """Put the rows @built in place of rows @y0 .. @y1 of the state: a row
    flagged as keeping identity takes the first replaced row's."""
    keep = st.rows[y0].ident if y0 < len(st.rows) and y1 > y0 else None
    rows = []
    for data, end, keeps in built:
        if keeps and keep is not None:
            ident = keep
            keep = None
        else:
            ident = st.next_ident
            st.next_ident += 1
        rows.append(Row(data, end, ident))
    st.rows = st.rows[:y0] + rows + st.rows[y1:]
    st.fired.append("block_rewrap")


def _model_rows(pairs):
    return [Row(d, e, 0) for e, d in pairs]


def _clip_block(st):
    """The block a paste copies: the whole clipboard window."""
    last = len(st.clip) - 1
    return ((0, 0), (last, len(st.clip[last][1])))


def _clip_copy(st):
    """Edit > Copy: the block into the clipboard window, copied to the
    start of an empty clipboard as a block is copied anywhere; nothing for
    an empty block."""
    st.fired.append("clip_copy")
    if _block_empty(st):
        st.fired.append("block_empty")
        return False
    dest = [Row(b"", HARD, 0)]
    y0, y1, built, _length = copy_rows(dest, st.clip_width, 0, 0, st.rows,
                                       (st.mb, st.me))
    st.clip = [(e, d) for d, e, _k in built] + [(r.end, r.data) for r in dest[y1:]]
    return True


def _paste(st, action):
    if model._locked(st):
        st.fired.append("block_locked")
        return
    st.fired.append("clip_paste")
    block = _clip_block(st)
    if block[1] <= block[0]:
        st.fired.append("block_empty")
        return
    before = Snapshot(st)
    p = _point_offset(st, st.y, st.x)
    src = _model_rows(st.clip)
    y0, y1, built, length = copy_rows(st.rows, st.width, st.y, st.x, src, block)
    _apply_built(st, y0, y1, built)
    _land(st, p, length)
    st.save = st.options.maxchg + 1
    st.fired.append("block_save_count")
    _commit(st, before, action, undo_marks=None)
    _undo_marks_at_start(st, p)


def _undo_marks_at_start(st, offset):
    """Undo of a copy or a paste leaves both marks at the start of the text
    that went (the editor's own probe, 2026-11-12)."""
    if st.undo:
        pt = _offset_point(st, offset, POINT_AT_START)
        st.undo[-1].undo_marks = (pt, pt)
        st.fired.append("block_undo_marks")


# -- copy, move, delete in the window ------------------------------------------------

def _lands_inside(st):
    """e_blk_copy_lands_inside: the cursor, as a copy's destination, falls
    in the block."""
    (kay, kax), (key, kex) = st.mb, st.me
    y, x = st.y, st.x
    if kay == key:
        return y == kay and kax <= x < kex
    return ((kay < y < key) or (y == kay and x >= kax) or (y == key and x < kex))


def _block_copy(st, action):
    if model._locked(st):
        st.fired.append("block_locked")
        _clip_copy(st)
        return
    if _block_empty(st):
        st.fired.append("block_empty")
        return
    if _lands_inside(st):
        st.fired.append("block_copy_lands_inside")
        return
    st.fired.append("block_copy_text")
    before = Snapshot(st)
    p = _point_offset(st, st.y, st.x)
    exclude = set(range(st.mb[0], st.me[0] + 1))
    y0, y1, built, length = copy_rows(st.rows, st.width, st.y, st.x, list(st.rows),
                                      (st.mb, st.me), exclude)
    _apply_built(st, y0, y1, built)
    _land(st, p, length)
    st.save = st.options.maxchg + 1
    st.fired.append("block_save_count")
    _commit(st, before, action)
    _undo_marks_at_start(st, p)


def _cursor_inside(st):
    (kay, kax), (key, kex) = st.mb, st.me
    return kay <= st.y <= key and kax <= st.x <= kex


def _move_refused(st):
    """The reasons e_blk_plan_move gives for not moving."""
    (kay, kax), (key, kex) = st.mb, st.me
    if _block_empty(st) or _cursor_inside(st):
        return True
    if kay == key:
        return False
    if st.y == kay and st.x < kax:
        return False
    if st.y == key and st.x > kex:
        return False
    return kay <= st.y <= key


def _block_move(st, action):
    if model._locked(st):
        st.fired.append("block_locked")
        return
    if _move_refused(st):
        st.fired.append("block_move_into_itself")
        return
    st.fired.append("block_move_text")
    st.resync_fields.add(model.F_BREAKPOINTS)
    st.fired.append("block_move_breakpoints")
    before = Snapshot(st)
    a, b = _block_span(st)
    p = _point_offset(st, st.y, st.x)
    one_row = st.mb[0] == st.me[0]
    exclude = set(range(st.mb[0], st.me[0] + 1))
    # put a copy in where the cursor is (the rows between the block's ends
    # stay as they are), then take the block out of its old place
    y0, y1, built, length = copy_rows(st.rows, st.width, st.y, st.x, list(st.rows),
                                      (st.mb, st.me), exclude)
    _apply_built(st, y0, y1, built)
    if p <= a:
        _take_out_old_block(st, a + length, b + length, one_row)
        dest = p
    else:
        _take_out_old_block(st, a, b, one_row)
        dest = p - length
    _land(st, dest, length)
    st.save = st.options.maxchg + 1
    st.fired.append("block_save_count")
    _commit(st, before, action)


def _take_out_old_block(st, a, b, one_row):
    """Remove the moved block's old text, offsets @a to @b.  A block of one
    row goes out of that row alone (we_block.c e_blk_plan_chars_move): the
    rows above it in a soft-wrapped line keep their breaks, and the block's
    row with the rows its line goes on to is wrapped where the file reader
    wraps, as for every block operation; a block of several rows wraps the
    lines it touches."""
    if one_row:
        start = _offset_point(st, a, POINT_AT_START)
        stop = _offset_point(st, b, POINT_AT_END)
        if start[0] == stop[0]:
            splice_text(st, a, b, b"", first_row=start[0])
            return
    splice_text(st, a, b, b"")


def _block_delete(st, action):
    if model._locked(st):
        st.fired.append("block_locked")
        return False
    if _block_empty(st):
        st.fired.append("block_empty")
        return False
    st.fired.append("block_delete_text")
    before = Snapshot(st)
    (kay, kax), (key, kex) = st.mb, st.me
    if kay == key:
        st.fired.append("block_in_line_delete")
        rows = Rows(st)
        rows.del_nchar(kax, kay, kex - kax)
        rows.commit()
        # the cursor goes where the block began, on that row (e_blk_delete_in_line_now)
        st.y, st.x = kay, kax
        st.me = (kay, kax)
    else:
        a, b = _block_span(st)
        splice_text(st, a, b, b"")
        st.mb = st.me = _offset_point(st, a, POINT_AT_START)
        st.y, st.x = st.mb
        st.save = st.options.maxchg + 1
        st.fired.append("block_save_count")
        st.fired.append("block_marks_after")
    _commit(st, before, action)
    return True


def _cut(st, action):
    st.fired.append("clip_cut")
    _clip_copy(st)
    _block_delete(st, action)


# -- indent and the help markers ---------------------------------------------------------

def _is_blank(c):
    return c in (SPACE, TAB, 0x0A, 0x0B, 0x0C, 0x0D)


def _first_visible_c(rows, i):
    """Where a shift puts the blanks in row @i: after the row's own leading
    blanks, and never past the row's end (a row of only blanks gets them
    after its last blank, an empty row at column 0)."""
    buf = rows.bufs[i]
    ln = rows.str_len(i)
    j = 0
    while j < ln and _is_blank(buf[j]):
        j += 1
    return j


def _block_rows(st):
    """The rows the block shifts: from the first row that starts in it up to
    the one holding its end (not a row the end begins)."""
    (kay, kax), (key, kex) = st.mb, st.me
    i = kay if kax == 0 else kay + 1
    out = []
    while i < key or (i == key and kex > 0):
        if i >= len(st.rows):
            break
        out.append(i)
        i += 1
    return out


def _block_rows_as_they_go(st, rows):
    """The rows a shift to the left visits, as the editor's loop reads them
    (we_block.c e_blck_to_left): from the first row that starts in the
    block, up to the row holding its end read again after every row.  A
    soft row of blanks the shift empties goes, and the block's end moves up
    with the rows below it, so the rows are never counted in advance."""
    (kay, kax) = st.mb
    i = kay if kax == 0 else kay + 1
    while i < rows.nlines and (i < st.me[0] or (i == st.me[0] and st.me[1] > 0)):
        yield i
        i += 1


def _say_read_only(st):
    """A command that tried to change a locked window was refused: the status
    row says the text is unchanged."""
    st.fired.append("block_locked")
    st.status = model.STATUS_READ_ONLY


def _block_indent(st, action):
    if model._locked(st):
        st.fired.append("block_locked")
        if _block_rows(st):
            _say_read_only(st)
        return
    st.fired.extend(["indent_keys", "indent_width", "indent_blank_row", "indent_one_step"])
    before = Snapshot(st)
    n = st.options.tabn // 2
    rows = Rows(st)
    markers = list(st.pt)     # the shift works on a copy of the rows: the markers wait
    tstr = b" " * n
    for i in _block_rows(st):
        j = _first_visible_c(rows, i)
        if n > 0:
            rows.ins_nchar(tstr, j, i)
        if i == st.mb[0]:
            st.mb = (st.mb[0], 0)
    rows.commit()
    st.pt = markers
    _commit(st, before, action)


def _tab_stops_before(rows, i, l, tabn):
    """The columns the characters before position @l of row @i take up."""
    nn = 0
    s = rows.bufs[i]
    for m in range(l):
        if s[m] == SPACE:
            nn += 1
        elif s[m] == TAB:
            nn += tabn - (nn % tabn)
    return nn


def _put_back_columns(st, rows, want, x, y):
    """The columns a tab took beyond what the shift removes are put back as
    blanks, @want of them (at most one tab's width less one)."""
    st.fired.append("unindent_puts_back_blanks")
    rows.ins_nchar(b" " * want, x, y)


def _unindent_scan(rows, i, tabn, n):
    """How a shift to the left reads row @i: the width of its leading blanks
    ``j``, the columns ``k`` taken from their end back to Tabstops / 2, and
    the number of bytes ``l`` that come out."""
    ln = rows.str_len(i)
    j = 0
    buf = rows.bufs[i]
    while j < ln and _is_blank(buf[j]):
        j += 1
    k = 0
    l = j - 1
    while l >= 0 and k < n:
        if buf[l] == SPACE:
            k += 1
        elif buf[l] == TAB:
            nn = _tab_stops_before(rows, i, l, tabn)
            k += tabn - (nn % tabn)
        l -= 1
    return j, k, j - l - 1


def _block_unindent(st, action):
    tabn = st.options.tabn
    n = tabn // 2
    if model._locked(st):
        st.fired.append("block_locked")
        rows = Rows(st)
        if any(_unindent_scan(rows, i, tabn, n)[2] > 0 for i in _block_rows(st)):
            _say_read_only(st)
        return
    st.fired.extend(["indent_keys", "indent_width", "indent_one_step"])
    before = Snapshot(st)
    rows = Rows(st)
    markers = list(st.pt)     # the shift works on a copy of the rows: the markers wait
    for i in _block_rows_as_they_go(st, rows):
        j, k, l = _unindent_scan(rows, i, tabn, n)
        if l > 0:
            keep = st.mb[1]
            rows.del_nchar(j - l, i, l)
            if k > n:
                _put_back_columns(st, rows, k - n, j - l, i)
            st.mb = (st.mb[0], keep)
    rows.commit()
    st.pt = markers
    _commit(st, before, action)


def _help_marker(st, action, marker):
    (kay, kax), (key, kex) = st.mb, st.me
    if not (kay == key and kay >= 0 and kax < kex) or not _marks_in_text(st):
        return
    if model._locked(st):
        _say_read_only(st)
        return
    st.fired.extend(["help_markers", "help_markers_detail", "help_markers_one_step"])
    before = Snapshot(st)
    rows = Rows(st)
    markers = list(st.pt)     # the marks go in on a copy of the rows: the markers wait
    rows.ins_nchar(bytes([HELP_END]), kex, key)
    rows.ins_nchar(bytes([marker]), kax, key)
    rows.commit()
    st.pt = markers
    _commit(st, before, action)


def _help_end(st, action):
    """Ctrl-O E: the end-of-page mark on a new row of its own above the
    cursor's row, the cursor just after the mark."""
    if model._locked(st):
        _say_read_only(st)
        return
    st.fired.extend(["help_end_line", "help_end_one_step"])
    before = Snapshot(st)
    rows = Rows(st)
    y = st.y
    rows.insert_blank(y)
    rows.bufs[y] = rows._buf(bytes([HELP_PAGE_END]), True)
    rows.commit()
    st.x = 1
    _commit(st, before, action)


# -- Search > Replace > Change All ---------------------------------------------------------

def _upper(c):
    if 0x61 <= c <= 0x7A:
        return c - 0x20
    if 0xE0 <= c <= 0xFE:
        return c - 0x20
    return c


def ustrstr(x, n, s, f):
    """e_ustrstr: the first match of @f in @s between @x and @n ignoring
    case, or -1; from @x past @n it looks backwards, as the C code does."""
    nf = len(f)
    if x > n:
        i = x - nf
        while i >= n:
            if all(_upper(s[i + j] if i + j < len(s) else 0) == _upper(f[j]) for j in range(nf)):
                return i
            i -= 1
        return -1
    i = 0 if x < 0 else x
    while i <= n - nf:
        if all(_upper(s[i + j] if i + j < len(s) else 0) == _upper(f[j]) for j in range(nf)):
            return i
        i += 1
    return -1


def _rep_search(st, rows, search):
    """e_rep_search, forward over the whole text with the dialog's defaults
    (case ignored, no whole words, no regular expression): the match as
    (x, y), or None."""
    ret = st.x
    j = st.y
    jend = rows.nlines - 1
    iend = rows.str_len(jend)
    while True:
        if j > jend:
            return None
        while True:
            end = iend if j == jend else rows.str_len(j)
            ret = ustrstr(ret, end, rows.cstr(j), search)
            if j == jend and ret > iend:
                break
            if ret >= 0:
                st.y = j
                return (ret, j)
            break
        j += 1
        ret = -1


def _replace_all_refused(st, search):
    """Change All in a read-only window: every occurrence is found, none is
    replaced, and the box and the status row say the text is unchanged."""
    st.fired.extend(["block_locked", "replace_all_text", "replace_scan", "replace_message"])
    rows = Rows(st)
    st.y = st.x = 0
    found = 0
    while found < REPLACE_LIMIT:
        hit = _rep_search(st, rows, search)
        if hit is None:
            break
        found += 1
        st.x = hit[0] + len(search)
    if found:
        st.box = BOX_FOUND_REFUSED % found
        st.status = model.STATUS_READ_ONLY
    else:
        st.box = MSG_NOT_FOUND


def _replace_all(st, action):
    search, repl = action.arg
    if model._locked(st):
        _replace_all_refused(st, search)
        return
    st.fired.extend(["replace_all_text", "replace_scan", "replace_by_primitives",
                     "replace_one_step", "replace_message"])
    before = Snapshot(st)
    rows = Rows(st)
    markers = list(st.pt)     # Change All works on a copy of the rows: the markers wait
    st.y = st.x = 0
    found = 0
    sn = len(search)
    while found < REPLACE_LIMIT:
        hit = _rep_search(st, rows, search)
        if hit is None:
            break
        found += 1
        x, y = hit
        rows.del_nchar(x, y, sn)
        rows.ins_nchar(repl, x, y)   # the search goes on behind it, where it ends
    rows.commit()
    st.pt = markers
    if found:
        st.box = BOX_FOUND % (found, found)
    else:
        st.box = MSG_NOT_FOUND
    _commit(st, before, action)


# -- the option dialog and the markers ------------------------------------------------------

def _option_set(st, action):
    name, value = action.arg
    assert name in OPTION_FIELDS, name
    st.fired.append("option_dialog_numbers")
    if name == "maxcol":
        st.fired.append("option_width_open")
        st.options.maxcol = value
    elif name == "tabn":
        st.options.tabn = value
    elif name == "maxchg":
        st.options.maxchg = value
    elif name == "numundo":
        st.options.numundo = value
    elif name == "autoindent_col":
        st.options.autoindent_col = value


def _marker(st, name, n):
    if n >= model.MARKER_COUNT:
        return                      # a marker the editor does not have: no key names it
    st.fired.extend(["marker_keys", "marker_place"])
    if name == "marker_set":
        st.pt[n] = (st.y, st.x)
    else:
        # an unset marker is (-1, -1); the cursor is put back inside the text
        st.y, st.x = max(st.pt[n][0], 0), max(st.pt[n][1], 0)
        if st.y < len(st.rows):
            # on the glyph that stands there now (the next paint puts it so)
            st.x = model.utf8_glyph_floor(st.rows[st.y].data, st.x)


def apply_action(st, action):
    """Run one block, clipboard, replace or option action on the state @st
    (already a copy the caller owns)."""
    name = action.name
    if name in BLOCK_MARKS:
        _mark_actions(st, name)
    elif name == "block_copy":
        _block_copy(st, action)
    elif name == "block_move":
        _block_move(st, action)
    elif name == "block_delete":
        _block_delete(st, action)
    elif name == "block_indent":
        _block_indent(st, action)
    elif name == "block_unindent":
        _block_unindent(st, action)
    elif name == "help_button":
        _help_marker(st, action, HELP_BUTTON)
    elif name == "help_highlight":
        _help_marker(st, action, HELP_HIGHLIGHT)
    elif name == "help_header":
        _help_marker(st, action, HELP_HEADER)
    elif name == "help_end":
        _help_end(st, action)
    elif name == "edit_copy":
        _clip_copy(st)
    elif name == "edit_cut":
        _cut(st, action)
    elif name == "edit_paste":
        _paste(st, action)
    elif name in MARKERS:
        _marker(st, name, action.arg)
    elif name == "replace_all":
        _replace_all(st, action)
    elif name == "option_set":
        _option_set(st, action)
    if name not in model.POSITION_NEUTRAL:
        _land_on_glyphs(st)


def _land_on_glyphs(st):
    """The editor lands the cursor and the block of a block command through
    the owner (e_buffer_cursor_set, e_buffer_block_set), which puts each back
    to the start of the glyph it falls in; so does the model."""
    if st.rows and 0 <= st.y < len(st.rows):
        st.x = model.utf8_glyph_floor(st.rows[st.y].data, st.x)
    st.mb = model._mark_inside(st.rows, st.mb)
    st.me = model._mark_inside(st.rows, st.me)
