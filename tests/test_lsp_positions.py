"""The language server's columns land on the right letters (we_lsp_pos.c).

A server counts the characters of a line in the units it negotiated (UTF-16
code units unless it agreed to UTF-8) and the editor counts bytes.  On a line
with an accented letter, a CJK character or an emoji before a symbol the two
differ, and every highlight, problem mark, jump, selection, rename and
semantic colour of the Alt-Q actions used to land a few letters to the left.

A stand-in `clangd` on PATH plays the server in the units the test names
("units": no positionEncoding answer, so UTF-16; "utf8": it answers utf-8).
Its answers are computed here, by Python's own UTF-16 length, never by the
editor's conversion, and it logs the `character` of every request it reads, so
the rows that check what the editor SENDS read the wire from the stand-in's
log, and the rows that check what it PAINTS read the screen.

The fixture file (every line, 0-based; screen row = line + 2, screen column =
visual column + 1 for the frame):

     0  #include <stdio.h>
     1
     2  int add(int a, int b) { return a + b; }
     3
     4  int main(void) {
     5      /* café 漢字 😀 */ int total = add(2, 3);      (F1)
     6      printf("%d\\n", total);
     7      /* é */ return bad_symbol;
     8  }

F1: `total` is at byte 32, UTF-16 unit 25, screen column 28; `bad_symbol` at
byte 20, unit 19.  F2 has the emoji alone (`total`: byte 19, unit 17).  F0,
the positive control, has the same lines in ASCII, where every unit is a
byte: each row is green on F0 before and after.
"""
import json
import os
import stat
import sys
import time

import pytest

from wpe_driver import WpeSession, WAIT_SCALE

ALT_Q = "\033q"                   # the language server prefix
ALT_S = "\033s"                   # the Search menu
ENTER = "\r"
CTRL_K = "\x0b"                   # the block prefix: Ctrl-K B, Ctrl-K K
LEFT = "\033[D"
RIGHT = "\033[C"
POLL_S = 0.2
STARTED_S = 20
SAID_S = 10
TEXT_ROW = 2                      # screen row of line 0
FRAME = 1                         # the window's left border
FAKE_LOG = "lsp-fake.log"
FAKE_CONFIG = "lsp-fake.json"
FAKE_TRACE = "lsp-trace.log"
HOVER_TEXT = "HOVER-TEXT-OK"
RENAMED = "sum"
COMPLETED = "total2"
TOTAL_LINE = 5                    # `total` is declared here, and used on 6
USE_LINE = 6                      # printf("%d\n", total): ASCII on every fixture
BAD_LINE = 7                      # return bad_symbol;
USE_BYTE = 19                     # `total` on the ASCII line, bytes = units
TOTAL_LEN = 5
BAD_LEN = 10

HEAD_LINES = ["#include <stdio.h>", "",
              "int add(int a, int b) { return a + b; }", "",
              "int main(void) {"]
USE_TEXT = '    printf("%d\\n", total);'
FIXTURES = {
    "F1": ("    /* café 漢字 😀 */ int total = add(2, 3);",
           "    /* é */ return bad_symbol;"),
    "F2": ("    /* 😀 */ int total = add(2, 3);",
           "    /* é */ return bad_symbol;"),
    "F0": ("    /* cafe CJK :-) */ int total = add(2, 3);",
           "    /* e */ return bad_symbol;"),
}
# Written by hand from the rule, the numbers the stand-in's log must show for
# `total` and for the cursor after `tot` (row 08), per fixture, in utf-16.
TOTAL_UNITS = {"F1": 25, "F2": 17, "F0": 27}
TOTAL_BYTES = {"F1": 32, "F2": 19, "F0": 27}

FAKE_CLANGD = r'''#!%(python)s -I
import json, os, select, sys
here = %(dir)r
def config():
    """What to answer, read at each request: a test may change it meanwhile."""
    with open(os.path.join(here, %(config)r)) as fh:
        return json.load(fh)
inp, out = sys.stdin.buffer, sys.stdout.buffer
doc = {"uri": ""}

def note(text):
    with open(os.path.join(here, %(log)r), "a") as fh:
        fh.write(text + "\n")

def send(obj):
    body = json.dumps(obj).encode()
    out.write(b"Content-Length: %%d\r\n\r\n" %% len(body) + body)
    out.flush()

def uri(msg):
    return msg.get("params", {}).get("textDocument", {}).get("uri", "")

def fill(value, msg):
    """The answer with the file's URI put in, and @LINK@ for the same file
    under another name (a link to it)."""
    text = json.dumps(value).replace("@URI@", doc["uri"])
    text = text.replace("@LINK@", doc["uri"].replace("demo.c", "link.c"))
    return json.loads(text)

def result(msg):
    method = msg["method"]
    cfg = config()
    if method == "initialize":
        caps = {"semanticTokensProvider": {
            "legend": {"tokenTypes": ["variable"], "tokenModifiers": []},
            "full": True}}
        if cfg["encoding"]:
            caps["positionEncoding"] = cfg["encoding"]
        return {"capabilities": caps}
    return fill(cfg["answers"].get(method), msg)

def log_request(msg):
    params = msg.get("params", {})
    method = msg["method"]
    if method == "initialize":
        offered = params.get("capabilities", {}).get("general", {}) \
                        .get("positionEncodings")
        note("INIT offer=%%s" %% (",".join(offered) if offered else "none"))
        return
    pos = params.get("position")
    if pos is not None:
        note("REQ %%s character=%%d" %% (method, pos["character"]))
    elif "range" in params:
        rng = params["range"]
        note("REQ %%s start=%%d end=%%d" %% (method, rng["start"]["character"],
                                         rng["end"]["character"]))
    else:
        note("REQ %%s" %% method)

pending = b""

def messages():
    global pending
    got = []
    while True:
        head, sep, rest = pending.partition(b"\r\n\r\n")
        if not sep:
            return got
        length = 0
        for line in head.split(b"\r\n"):
            if line.lower().startswith(b"content-length:"):
                length = int(line.split(b":")[1])
        if len(rest) < length:
            return got
        got.append(json.loads(rest[:length]))
        pending = rest[length:]

def take(msg):
    method = msg.get("method")
    if method is None:
        return True
    if "id" not in msg:
        note("NOTE %%s" %% method)
        if method == "textDocument/didOpen":
            doc["uri"] = uri(msg)
            diag = config()["diag"]
            diags = []
            if diag:
                diags = [{"range": {
                    "start": {"line": diag["line"], "character": diag["start"]},
                    "end": {"line": diag["line"], "character": diag["end"]}},
                    "severity": 1, "message": "undeclared bad_symbol"}]
            send({"jsonrpc": "2.0", "method": "textDocument/publishDiagnostics",
                  "params": {"uri": doc["uri"], "diagnostics": diags}})
        return method != "exit"
    log_request(msg)
    send({"jsonrpc": "2.0", "id": msg["id"], "result": result(msg)})
    return True

going = True
while going:
    data = os.read(0, 65536)
    if not data:
        break
    pending += data
    for msg in messages():
        going = going and take(msg)
'''


# ---- what the server would count: Python's own, not the editor's -----------

def units_of(row, byte_col, enc):
    """The units of byte column `byte_col` of `row` in encoding `enc`."""
    prefix = row.encode()[:byte_col].decode()
    if enc == "utf-8":
        return byte_col
    return len(prefix.encode("utf-16-le")) // 2


def byte_of(row, word):
    return row.encode().index(word.encode())


def source(kind):
    row5, row7 = FIXTURES[kind]
    return "\n".join(HEAD_LINES + [row5, USE_TEXT, row7, "}"]) + "\n"


def span(line, start, end):
    return {"start": {"line": line, "character": start},
            "end": {"line": line, "character": end}}


def fake_config(kind, enc, diag_end=None, highlight_start=None):
    """What the stand-in answers, in the units of `enc` ("utf-16" or
    "utf-8"): the problem, the occurrences, the selection, the rename, the
    references and the semantic token, all on `total` and `bad_symbol`."""
    row5, row7 = FIXTURES[kind]
    u5 = units_of(row5, byte_of(row5, "total"), enc)
    u7 = units_of(row7, byte_of(row7, "bad_symbol"), enc)
    end5 = units_of(row5, len(row5.encode()), enc)
    start = u5 if highlight_start is None else highlight_start
    location = lambda line, a, b: {"uri": "@URI@", "range": span(line, a, b)}
    return {
        "encoding": "utf-8" if enc == "utf-8" else None,
        "diag": {"line": BAD_LINE, "start": u7,
                 "end": u7 + BAD_LEN if diag_end is None else diag_end},
        "answers": {
            "textDocument/hover": {"contents": {"kind": "plaintext",
                                                "value": HOVER_TEXT}},
            "textDocument/completion": [{"label": COMPLETED,
                                         "insertText": COMPLETED}],
            "textDocument/documentHighlight": [
                {"range": span(TOTAL_LINE, start, start + TOTAL_LEN), "kind": 1},
                {"range": span(USE_LINE, USE_BYTE, USE_BYTE + TOTAL_LEN),
                 "kind": 1}],
            "textDocument/selectionRange": [
                {"range": span(TOTAL_LINE, u5, u5 + TOTAL_LEN),
                 "parent": {"range": span(TOTAL_LINE, 0, end5)}}],
            "textDocument/rename": {"changes": {"@URI@": [
                {"range": span(TOTAL_LINE, u5, u5 + TOTAL_LEN),
                 "newText": RENAMED}]}},
            "textDocument/references": [
                location(TOTAL_LINE, u5, u5 + TOTAL_LEN),
                location(USE_LINE, USE_BYTE, USE_BYTE + TOTAL_LEN)],
            "textDocument/semanticTokens/full": {
                "data": [TOTAL_LINE, u5, TOTAL_LEN, 0, 0]},
            "textDocument/rangeFormatting": [],
        },
    }


# ---- the session -----------------------------------------------------------

def _write_executable(path, text):
    with open(path, "w") as fh:
        fh.write(text)
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)


def _read(path):
    try:
        with open(path) as fh:
            return fh.read()
    except OSError:
        return ""


def _log(tmp_path):
    return _read(str(tmp_path / FAKE_LOG))


def _trace(tmp_path):
    return _read(str(tmp_path / FAKE_TRACE))


def _screen(w):
    return "\n".join(w.display(drain=0))


def _wait_for(w, predicate, timeout):
    deadline = time.monotonic() + timeout * WAIT_SCALE
    while time.monotonic() < deadline:
        w._drain(POLL_S)
        if predicate():
            return True
    return predicate()


def _env(tmp_path, config):
    bindir = tmp_path / "bin"
    bindir.mkdir()
    with open(str(tmp_path / FAKE_CONFIG), "w") as fh:
        json.dump(config, fh)
    _write_executable(str(bindir / "clangd"),
                      FAKE_CLANGD % {"python": sys.executable,
                                     "dir": str(tmp_path),
                                     "config": FAKE_CONFIG, "log": FAKE_LOG})
    return {"PATH": "%s:%s" % (bindir, os.environ.get("PATH", "")),
            "XWPE_LSP_TRACE": str(tmp_path / FAKE_TRACE)}


class Session:
    """A wpe with the stand-in server, the fixture open and the server
    started; the cursor is wherever the test puts it."""

    def __init__(self, tmp_path, kind, enc="utf-16", **config):
        self.tmp_path = tmp_path
        self.kind = kind
        self.enc = enc
        self.row5, self.row7 = FIXTURES[kind]
        self.w = WpeSession(str(tmp_path), source(kind), filename="demo.c",
                            wait=1.5,
                            env_extra=_env(tmp_path,
                                           fake_config(kind, enc, **config)))

    def __enter__(self):
        self.w.__enter__()
        w = self.w
        w.key(ALT_Q, delay=0.4)
        w.key("e", delay=0.1)
        assert _wait_for(w, lambda: "NOTE textDocument/didOpen"
                         in _log(self.tmp_path), STARTED_S), \
            "Alt-Q E should start the server:\n%s" % _screen(w)
        return self

    def __exit__(self, *exc):
        return self.w.__exit__(*exc)

    def find(self, term):
        """The cursor on the first match of `term` (Search > Find)."""
        w = self.w
        w.key(ALT_S, delay=0.6)
        w.key("f", delay=0.6)
        for ch in term:
            w.key(ch, delay=0.05)
        w.key(ENTER, delay=1.0)

    def ask(self, letter, method):
        """Alt-Q <letter>; wait until the stand-in has the request."""
        w = self.w
        w.key(ALT_Q, delay=0.4)
        w.key(letter, delay=0.1)
        assert _wait_for(w, lambda: ("REQ %s" % method) in _log(self.tmp_path),
                         SAID_S), \
            "Alt-Q %s should ask for %s:\n%s" % (letter.upper(), method,
                                                _screen(w))

    def settle(self, seconds=1.5):
        self.w._drain(seconds)

    def style(self, row, col):
        cell = self.w.screen.buffer[row][col]
        return (cell.fg, cell.bg, cell.reverse)

    def screen_col(self, line, byte_col):
        """The screen column of a byte of the text row `line`: the visual
        columns before it, one more for the frame."""
        import unicodedata
        text = {TOTAL_LINE: self.row5, USE_LINE: USE_TEXT,
                BAD_LINE: self.row7}[line]
        before = text.encode()[:byte_col].decode()
        width = sum(2 if unicodedata.east_asian_width(c) in "WF" else 1
                    for c in before)
        return FRAME + width


def _sent(tmp_path, method):
    """The `character` the stand-in read in the last `method` request."""
    last = None
    for line in _log(tmp_path).splitlines():
        if line.startswith("REQ %s character=" % method):
            last = int(line.rsplit("=", 1)[1])
    return last


def _painted_apart(s, row, cols, outside):
    """The cells `cols` of screen row `row` share one style that none of the
    cells `outside` has."""
    styles = {s.style(row, c) for c in cols}
    assert len(styles) == 1, \
        "cells %s of row %d should be painted alike, got %s:\n%s" \
        % (list(cols), row, styles, _screen(s.w))
    mine = styles.pop()
    for c in outside:
        assert s.style(row, c) != mine, \
            "cell %d of row %d should not be painted:\n%s" \
            % (c, row, _screen(s.w))
    return mine


def _total_cols(s, line, byte_col, length=TOTAL_LEN):
    first = s.screen_col(line, byte_col)
    return range(first, first + length)


ALL = ["F1", "F2", "F0"]


# @scenario SC-lsp-positions-01
@pytest.mark.parametrize("kind", ALL)
def test_01_hover_asks_in_the_servers_units(tmp_path, kind):
    """Alt-Q H on `total`: the column on the wire is the UTF-16 unit
    (F1 25, F2 17), not the byte (32, 19).  F0 is ASCII: both agree."""
    with Session(tmp_path, kind) as s:
        s.find("total")
        s.ask("h", "textDocument/hover")
        assert _sent(tmp_path, "textDocument/hover") == TOTAL_UNITS[kind], \
            "the hover should ask for unit %d:\n%s" \
            % (TOTAL_UNITS[kind], _log(tmp_path))
        assert s.w.alive()


# @scenario SC-lsp-positions-02
@pytest.mark.parametrize("kind", ALL)
def test_02_highlight_paints_the_symbol(tmp_path, kind):
    """Alt-Q U paints the letters of `total`, on the line with the
    wide characters and on the ASCII line below, and nothing before it."""
    with Session(tmp_path, kind) as s:
        s.find("total")
        s.ask("u", "textDocument/documentHighlight")
        s.settle()
        row = TEXT_ROW + TOTAL_LINE
        byte = byte_of(s.row5, "total")
        cols = _total_cols(s, TOTAL_LINE, byte)
        start = s.screen_col(TOTAL_LINE, byte)
        before = range(FRAME + 1, start)          # the comment and `int `
        found = _painted_apart(s, row, cols, before)
        # The positive control on screen: the ASCII `total` of the next line
        # is painted by the same answer, green on the parent and after.
        use_cols = _total_cols(s, USE_LINE, USE_BYTE)
        assert {s.style(row + 1, c) for c in use_cols} == {found}, \
            "the ASCII `total` should be painted the same:\n%s" % _screen(s.w)


# @scenario SC-lsp-positions-03
@pytest.mark.parametrize("kind", ALL)
def test_03_the_problem_jump_lands_on_the_symbol(tmp_path, kind):
    """Alt-Q . puts the cursor on the first letter of `bad_symbol`."""
    with Session(tmp_path, kind) as s:
        w = s.w
        s.settle()
        w.key(ALT_Q, delay=0.4)
        w.key(".", delay=1.0)
        w._drain(1.0)
        want_x = s.screen_col(BAD_LINE, byte_of(s.row7, "bad_symbol"))
        want_y = TEXT_ROW + BAD_LINE
        assert (w.screen.cursor.x, w.screen.cursor.y) == (want_x, want_y), \
            "the cursor should be on `bad_symbol` at %s, is at %s:\n%s" \
            % ((want_x, want_y), (w.screen.cursor.x, w.screen.cursor.y),
               _screen(w))


# @scenario SC-lsp-positions-04
@pytest.mark.parametrize("kind", ALL)
def test_04_the_problem_mark_covers_the_symbol(tmp_path, kind):
    """The error mark covers the ten letters of `bad_symbol` and not the
    space before it."""
    with Session(tmp_path, kind) as s:
        s.settle(2.0)
        row = TEXT_ROW + BAD_LINE
        byte = byte_of(s.row7, "bad_symbol")
        cols = _total_cols(s, BAD_LINE, byte, BAD_LEN)
        before = s.screen_col(BAD_LINE, byte) - 1
        _painted_apart(s, row, cols, [before])


# @scenario SC-lsp-positions-05
@pytest.mark.parametrize("kind", ALL)
def test_05_expand_selection_marks_the_symbol(tmp_path, kind):
    """Alt-Q V marks `total` and not the bytes before it."""
    with Session(tmp_path, kind) as s:
        s.find("total")
        s.ask("v", "textDocument/selectionRange")
        s.settle()
        row = TEXT_ROW + TOTAL_LINE
        byte = byte_of(s.row5, "total")
        cols = _total_cols(s, TOTAL_LINE, byte)
        start = s.screen_col(TOTAL_LINE, byte)
        _painted_apart(s, row, cols, range(FRAME + 1, start - 1))


# @scenario SC-lsp-positions-06
@pytest.mark.parametrize("kind", ALL)
def test_06_rename_edits_the_symbol(tmp_path, kind):
    """Alt-Q N renames `total`: the file has the new name there and the
    comment before it untouched."""
    with Session(tmp_path, kind) as s:
        w = s.w
        s.find("total")
        w.key(ALT_Q, delay=0.4)
        w.key("n", delay=0.8)
        w.key(ENTER, delay=0.1)
        assert _wait_for(w, lambda: "REQ textDocument/rename"
                         in _log(tmp_path), SAID_S)
        assert _wait_for(w, lambda: ("int %s =" % RENAMED) in _screen(w),
                         SAID_S), \
            "the rename should be applied:\n%s" % _screen(w)
        w.save()
        lines = w.text().split("\n")
        want = s.row5.replace("total", RENAMED)
        assert lines[TOTAL_LINE] == want, \
            "line %d should be %r, is %r" % (TOTAL_LINE, want, lines[TOTAL_LINE])


# @scenario SC-lsp-positions-07
@pytest.mark.parametrize("kind", ALL)
def test_07_semantic_colours_paint_the_symbol(tmp_path, kind):
    """Alt-Q M colours the letters of `total` as a variable and not the
    bytes before it."""
    with Session(tmp_path, kind) as s:
        w = s.w
        w.key(ALT_Q, delay=0.4)
        w.key("m", delay=0.1)
        assert _wait_for(w, lambda: "REQ textDocument/semanticTokens/full"
                         in _log(tmp_path), SAID_S)
        s.settle(2.0)
        row = TEXT_ROW + TOTAL_LINE
        byte = byte_of(s.row5, "total")
        cols = _total_cols(s, TOTAL_LINE, byte)
        start = s.screen_col(TOTAL_LINE, byte)
        fgs = {w.screen.buffer[row][c].fg for c in cols}
        assert len(fgs) == 1, "the letters should be coloured alike: %s" % fgs
        variable = fgs.pop()
        for c in range(start - 6, start):         # `int ` and the comment's end
            assert w.screen.buffer[row][c].fg != variable, \
                "cell %d should not be a variable colour:\n%s" \
                % (c, _screen(w))


# @scenario SC-lsp-positions-08
@pytest.mark.parametrize("kind", ALL)
def test_08_completion_asks_in_the_servers_units(tmp_path, kind):
    """Alt-Q C with the cursor after `tot`: the column on the wire is the
    unit after those letters, three past `total`'s."""
    with Session(tmp_path, kind) as s:
        s.find("tot")                  # the cursor ends just after the match
        s.ask("c", "textDocument/completion")
        want = TOTAL_UNITS[kind] + 3
        assert _sent(tmp_path, "textDocument/completion") == want, \
            "completion should ask for unit %d:\n%s" % (want, _log(tmp_path))


# @scenario SC-lsp-positions-09
@pytest.mark.parametrize("kind", ALL)
def test_09_range_format_sends_the_marks_in_units(tmp_path, kind):
    """A block marked by hand over `total` (Ctrl-K B and Ctrl-K K) and
    Alt-Q F: the range on the wire is in units (F1 25..30), not in bytes
    (32..37)."""
    with Session(tmp_path, kind) as s:
        w = s.w
        s.find("total")                # the cursor ends just after the match
        for _ in range(TOTAL_LEN):
            w.key(LEFT, delay=0.1)
        w.key(CTRL_K, "b", delay=0.3)
        for _ in range(TOTAL_LEN):
            w.key(RIGHT, delay=0.1)
        w.key(CTRL_K, "k", delay=0.3)
        s.ask("f", "textDocument/rangeFormatting")
        want = "REQ textDocument/rangeFormatting start=%d end=%d" \
            % (TOTAL_UNITS[kind], TOTAL_UNITS[kind] + TOTAL_LEN)
        assert want in _log(tmp_path), \
            "the format range should be %r:\n%s" % (want, _log(tmp_path))


# @scenario SC-lsp-positions-10
def test_10_a_unit_inside_a_pair_goes_back_to_the_emoji(tmp_path):
    """The server's highlight starts on the emoji's second UTF-16 unit
    (F2, unit 8): the mark begins on the emoji, never on a later byte.
    Measured before the owner: columns 10 and 11 (the space and the `*`
    after the emoji) were painted and the emoji's columns 8 and 9 were not;
    now 8, 9 and 10 are, and 18 to 22 (`total`, the second occurrence)."""
    start_unit = 8
    with Session(tmp_path, "F2", highlight_start=start_unit) as s:
        s.find("total")
        row = TEXT_ROW + TOTAL_LINE
        emoji_col = s.screen_col(TOTAL_LINE, byte_of(s.row5, "😀"))
        before = {c: s.style(row, c) for c in range(FRAME + 1, emoji_col + 1)}
        s.ask("u", "textDocument/documentHighlight")
        s.settle()
        assert s.style(row, emoji_col) != before[emoji_col], \
            "the emoji's cell should be painted:\n%s" % _screen(s.w)
        for c in range(FRAME + 1, emoji_col):
            assert s.style(row, c) == before[c], \
                "cell %d is before the emoji and should not be painted:\n%s" \
                % (c, _screen(s.w))


# @scenario SC-lsp-positions-11
@pytest.mark.parametrize("kind", ["F1", "F2"])
def test_11_a_utf8_answer_is_read(tmp_path, kind):
    """A server that answers positionEncoding utf-8: the session reads it
    (the trace says so), the wire carries the byte (32 / 19) and the
    server's byte answers paint `total`."""
    with Session(tmp_path, kind, enc="utf-8") as s:
        s.find("total")
        s.ask("u", "textDocument/documentHighlight")
        s.settle()
        assert "positionEncoding=utf-8 -> utf-8" in _trace(tmp_path), \
            "the trace should name the encoding:\n%s" % _trace(tmp_path)
        assert _sent(tmp_path, "textDocument/documentHighlight") \
            == TOTAL_BYTES[kind]
        row = TEXT_ROW + TOTAL_LINE
        byte = byte_of(s.row5, "total")
        start = s.screen_col(TOTAL_LINE, byte)
        _painted_apart(s, row, _total_cols(s, TOTAL_LINE, byte),
                       range(FRAME + 1, start - 1))


# @scenario SC-lsp-positions-11
def test_11b_no_answer_is_utf16(tmp_path):
    """A server that names no encoding is read as utf-16, once, in the
    trace."""
    with Session(tmp_path, "F1") as s:
        s.settle()
        assert "positionEncoding=absent -> utf-16" in _trace(tmp_path), \
            "the trace should name the encoding:\n%s" % _trace(tmp_path)


# @scenario SC-lsp-positions-13
@pytest.mark.parametrize("kind", ALL)
def test_13_a_problem_in_units_past_the_row_is_clamped(tmp_path, kind):
    """A guard (green before and after on F0; on F1 and F2 it also holds
    the mark to the right cells): the server says a problem ends at
    character 400, far past the row.  The mark ends at the row's end, the
    editor is alive and the rows below carry no mark.  Under the ASan
    build it is the row that would read past the end of a row."""
    with Session(tmp_path, kind, diag_end=400) as s:
        s.settle(2.0)
        assert s.w.alive(), "the editor ended"
        row = TEXT_ROW + BAD_LINE
        byte = byte_of(s.row7, "bad_symbol")
        _painted_apart(s, row, _total_cols(s, BAD_LINE, byte, BAD_LEN),
                       [s.screen_col(BAD_LINE, byte) - 1])
        plain = s.style(row + 1, FRAME + 1)
        for c in range(FRAME + 1, 12):
            assert s.style(row + 1, c) == plain, \
                "row %d should carry no mark:\n%s" % (row + 1, _screen(s.w))


# @scenario SC-lsp-positions-15
def test_15_a_location_in_the_same_file_by_a_link_counts_over_the_unsaved_text(
        tmp_path):
    """The server names the open file by a link to it (link.c -> demo.c)
    and the buffer has an unsaved emoji before `total` that the file on disk
    does not.  The column in Messages is the byte of the BUFFER's row + 1:
    the open file's text is never read from disk."""
    kind = "F1"
    with Session(tmp_path, kind) as s:
        w = s.w
        os.symlink(str(tmp_path / "demo.c"), str(tmp_path / "link.c"))
        s.find("/*")                   # the cursor ends just after the "/*"
        w.key("\U0001f600", delay=0.6)
        row5 = s.row5.replace("/*", "/*\U0001f600", 1)
        u5 = units_of(row5, byte_of(row5, "total"), "utf-16")
        config = json.load(open(str(tmp_path / FAKE_CONFIG)))
        config["answers"]["textDocument/references"] = [
            {"uri": "@LINK@", "range": span(TOTAL_LINE, u5, u5 + TOTAL_LEN)}]
        with open(str(tmp_path / FAKE_CONFIG), "w") as fh:
            json.dump(config, fh)
        s.settle(1.0)
        s.find("total")
        s.ask("r", "textDocument/references")
        want = "link.c:%d:%d" % (TOTAL_LINE + 1, byte_of(row5, "total") + 1)
        assert _wait_for(w, lambda: want in _screen(w), SAID_S), \
            "Messages should list %s:\n%s" % (want, _screen(w))


# @scenario SC-lsp-positions-14
@pytest.mark.parametrize("kind", ALL)
def test_14_the_references_say_the_byte_column(tmp_path, kind):
    """Alt-Q R: the Messages lines give line:byte column + 1, as the status
    row shows (F1: demo.c:6:33 and demo.c:7:20)."""
    with Session(tmp_path, kind) as s:
        s.find("total")
        s.ask("r", "textDocument/references")
        byte = byte_of(s.row5, "total")
        first = "demo.c:%d:%d" % (TOTAL_LINE + 1, byte + 1)
        second = "demo.c:%d:%d" % (USE_LINE + 1, USE_BYTE + 1)
        assert _wait_for(s.w, lambda: first in _screen(s.w)
                         and second in _screen(s.w), SAID_S), \
            "Messages should list %s and %s:\n%s" % (first, second,
                                                    _screen(s.w))


# ---- a lone CR inside a row (one server line is one editor row) ------------
CR_LINE = 2
CR_ROW = "int add(int a,\rint b) { return a + b; }"


class CrSession(Session):
    """The fixture with a lone CR in line 2 (the editor keeps it as row
    content; the server is sent it as a space)."""

    def __init__(self, tmp_path, kind, enc="utf-16", **config):
        self.tmp_path = tmp_path
        self.kind = kind
        self.enc = enc
        self.row5, self.row7 = FIXTURES[kind]
        head = list(HEAD_LINES)
        head[CR_LINE] = CR_ROW
        text = "\n".join(head + [self.row5, USE_TEXT, self.row7, "}"]) + "\n"
        self.w = WpeSession(str(tmp_path), text, filename="demo.c", wait=1.5,
                            env_extra=_env(tmp_path,
                                           fake_config(kind, enc, **config)))


def _disk(tmp_path):
    with open(str(tmp_path / "demo.c"), "rb") as fh:
        return fh.read()


# @scenario SC-lsp-positions-16
def test_16_a_lone_cr_survives_a_rename_elsewhere(tmp_path):
    """Alt-Q N on `total` (line 5): line 2 is in no edit and keeps its CR
    byte in the saved file (the server's copy has a space, the editor's
    does not)."""
    with CrSession(tmp_path, "F1") as s:
        w = s.w
        s.find("total")
        w.key(ALT_Q, delay=0.4)
        w.key("n", delay=0.8)
        w.key(ENTER, delay=0.1)
        assert _wait_for(w, lambda: "REQ textDocument/rename"
                         in _log(tmp_path), SAID_S)
        assert _wait_for(w, lambda: ("int %s =" % RENAMED) in _screen(w),
                         SAID_S), _screen(w)
        w.save()
        rows = _disk(tmp_path).split(b"\n")
    assert rows[TOTAL_LINE] == s.row5.replace("total", RENAMED).encode()
    assert rows[CR_LINE] == CR_ROW.encode(), \
        "the untouched line %d changed: %r" % (CR_LINE, rows[CR_LINE])


# @scenario SC-lsp-positions-16
def test_16b_a_lone_cr_before_the_symbol_keeps_the_highlight(tmp_path):
    """A lone CR on an earlier line: Alt-Q U still paints `total` (the
    server's lines are the editor's rows)."""
    with CrSession(tmp_path, "F1") as s:
        s.find("total")
        s.ask("u", "textDocument/documentHighlight")
        s.settle()
        row = TEXT_ROW + TOTAL_LINE
        byte = byte_of(s.row5, "total")
        start = s.screen_col(TOTAL_LINE, byte)
        _painted_apart(s, row, _total_cols(s, TOTAL_LINE, byte),
                       range(FRAME + 1, start))
