"""What the model's rows look like to the user: the file a Save writes, the
text as one logical string, and the rows a window shows.

These are the comparisons with the world outside the dump: the disk after
F2 or an autosave, and the screen.  They take the rows as the dump reports
them, ``[(end, bytes)]`` with end ``H`` (a line break follows) or ``S``
(the row wraps into the next one), so a model state and an editor dump
render the same way.
"""
HARD = "H"
LINE_BREAK = b"\n"
TAB = 0x09
PRINTABLE_MIN = 0x20
PRINTABLE_MAX = 0x7E


def render_disk(lines):
    """The bytes of the file: each row, and a line break after a hard one."""
    out = bytearray()
    for end, data in lines:
        out += data
        if end == HARD:
            out += LINE_BREAK
    return bytes(out)


def logical_lines(lines):
    """The lines the user wrote: soft rows joined to the row after them."""
    return render_disk(lines).split(LINE_BREAK)


def expand_tabs(data, tabn):
    """A row as the screen shows it, tabs to the next stop, or None when a
    byte outside printable ASCII makes the column arithmetic uncertain."""
    out = []
    col = 0
    for c in data:
        if c == TAB:
            stop = (col // tabn + 1) * tabn if tabn > 0 else col + 1
            out.append(" " * (stop - col))
            col = stop
        elif PRINTABLE_MIN <= c <= PRINTABLE_MAX:
            out.append(chr(c))
            col += 1
        else:
            return None
    return "".join(out)


ENDMARK = "$"
# What Show Endmark puts after a row that goes on in the next one (a soft
# wrap, or the last row of a file with no final line break): unixkeys.h PNL.
SOFT_ENDMARK = "\\"


def _endmark_of(end):
    """The mark Show Endmark draws after a row that ends with @end."""
    if end == HARD:
        return ENDMARK
    return SOFT_ENDMARK


def render_window(lines, view, tabn, endmark=False):
    """The text rows a window shows, top to bottom, as ``{screen row: text}``.

    @view is the dump's ``(c.y, c.x, a.y, a.x, e.y, e.x)``: the first row and
    column shown and the window's corners.  Rows a byte past ASCII makes
    uncertain are left out; trailing blanks are not significant.  With
    @endmark (Options > Show Endmark) a row that ends with a line break
    shows ``$`` after its last character, and any other row ``\\``.
    """
    cy, cx, ay, ax, ey, ex = view
    width = ex - ax - 1
    shown = {}
    for sy in range(ay + 1, ey):
        li = cy + (sy - ay - 1)
        if li >= len(lines):
            shown[sy] = ""
            continue
        text = expand_tabs(lines[li][1], tabn)
        if text is None:
            continue
        if endmark:
            text += _endmark_of(lines[li][0])
        shown[sy] = text[cx:cx + width].rstrip()
    return shown


def screen_disagreements(lines, view, tabn, display, endmark=False):
    """The screen rows that differ from what @lines render to: [(row, want,
    got)]."""
    cy, cx, ay, ax, ey, ex = view
    width = ex - ax - 1
    out = []
    for sy, want in sorted(render_window(lines, view, tabn, endmark).items()):
        if sy >= len(display):
            continue
        got = display[sy][ax + 1:ax + 1 + width].rstrip()
        if got != want:
            out.append((sy, want, got))
    return out


def show_rows(lines, cursor=None, marks=None):
    """The rows as plain text for a person: the cursor as ``|``, the block
    between ``[`` and ``]``, a soft row ending in ``~`` and a hard one in
    ``$``."""
    out = []
    for y, (end, data) in enumerate(lines):
        text = bytes(data)
        inserts = []
        if marks is not None and marks[0] != marks[1]:
            (by, bx), (ey, ex) = marks
            if by == y:
                inserts.append((bx, "["))
            if ey == y:
                inserts.append((ex, "]"))
        if cursor is not None and cursor[0] == y:
            inserts.append((cursor[1], b"|"))
        for x, mark in sorted(((x, m if isinstance(m, bytes) else m.encode())
                               for x, m in inserts), reverse=True):
            x = min(x, len(text))
            text = text[:x] + mark + text[x:]
        shown = text.decode("utf-8", "replace").replace("\t", "\\t")
        out.append(shown + ("$" if end == HARD else "~"))
    return out
