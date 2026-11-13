"""The text a window shows has one owner (we_buffer.c), terminal layer.

Every change to the lines of a window goes through the buffer owner, which
keeps the window's undo, modified mark, block marks, breakpoint lines and
colours in step with the lines.  Each row below drives one change the user
makes (or a tool makes for them) and checks what the user sees afterwards:
where the breakpoint is, what Ctrl-U and Ctrl-R give back, whether the
window reads as modified, and the colours of the text.

Breakpoints are set without a debugger session (Ctrl-G B), as
tests/test_menu_debug.py does; the margin's red line is what the user sees.
The assistant rows use the in-process mock backend.
"""
import concurrent.futures
import os
import subprocess
import time

import pytest

from wpe_driver import WpeSession, ALT, WPE_BIN
from edit_model.oracle import EditSession
import test_ai_edit_hook_contracts as hook_contracts
import test_debugger_integration as dbg

CTRL_G = "\x07"          # the debugger's prefix; Ctrl-G B toggles a breakpoint
CTRL_U = "\x15"          # Undo
CTRL_R = "\x12"          # Redo
CTRL_Y = "\x19"          # delete the line
CTRL_W = "\x17"          # close the window
CTRL_K = "\x0b"          # the block prefix: B begin, K end, C copy
CTRL_C = "\x03"          # Edit > Copy: the block to the clipboard
CTRL_V = "\x16"          # Edit > Paste: the clipboard at the cursor
BACKSPACE = "\x7f"
DOWN = "\033[B"
UP = "\033[A"
RIGHT = "\033[C"
HOME = "\033OH"
PAGE_DOWN = "\033[6~"
PAGE_UP = "\033[5~"
ENTER = "\r"
F2 = "\033OQ"            # Save
ALT_X = "\033x"
ESC = "\033"
BOX = "Proposed change"  # the assistant's review box
SHRINK_QUESTION = "review anyway?"
MODIFIED_MARK = "*"
# e_zlsplt paints the modified mark three cells right of the window's left
# border on its status row; the file window starts at column 0.
MODIFIED_MARK_COL = 3
TURN_S = 12

# A C file; rows are 0-based, the margin reports 1-based line numbers.
SEED = ("int main(void)\n"
        "{\n"
        "  int a = 1;\n"
        "  foo(a);\n"
        "  bar(a);\n"
        "  baz(a);\n"
        "  return 0;\n"
        "}\n")
BAZ_ROW = 5
# The same file with two lines added after the opening brace.
TWO_ADDED = SEED.replace("{\n", "{\n  int x = 0;\n  int y = 0;\n", 1)

# A file whose fourth line fills the window's width (118 bytes): a paste that
# ends on it makes it wrap onto one more line.
LONG_ROW = ("  /* " + " ".join("w%03d" % i for i in range(40)))[:117] + "Q"
WRAP_SEED = "\n".join(["int main(void)", "{", "  int a = 1;", LONG_ROW, "  foo(a);",
                       "  bar(a);", "  k1;", "  k2;", "  baz(a);", "  return 0;",
                       "}"]) + "\n"
WRAP_SEED_BREAKPOINT_ROWS = (4, 5, 8)     # foo, bar, baz
WRAP_SEED_LONG_ROW = 3


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


needs_ai = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _ai_env(tmp_path, reply):
    return {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_TRACE": str(tmp_path / "e.trace"),
    }


def _trace_has(tmp_path, needle):
    p = tmp_path / "e.trace"
    return p.exists() and needle in p.read_text()


def _accept_edit(s, tmp_path, instr="do it"):
    """Ask for an Edit, wait for its review box and accept its one hunk."""
    s.key(ALT.AI)
    s.key("e")
    s._drain(0.4)
    s.key(instr)
    s.key(ENTER, delay=0.8)
    end = time.time() + TURN_S
    while time.time() < end and BOX not in "\n".join(s.display(drain=0.4)):
        if SHRINK_QUESTION in "\n".join(s.display(drain=0.0)):
            s.key("y", delay=0.6)          # a reply much shorter than the file
    assert BOX in "\n".join(s.display()), "no review box:\n" + "\n".join(s.display())
    s.key("y", delay=0.8)
    end = time.time() + TURN_S
    while time.time() < end and not _trace_has(tmp_path, "edit applied"):
        s._drain(0.3)
    assert _trace_has(tmp_path, "edit applied"), "the Edit was not applied"
    s._drain(0.6)


def _down(s, n):
    for _ in range(n):
        s.key(DOWN, delay=0.15)


def _toggle_breakpoint_at(s, row):
    """From the top of the file, put a breakpoint on 0-based @row."""
    _down(s, row)
    s.key(CTRL_G, "b")
    s._drain(0.4)


def _line_text(s, line):
    """The screen text of 1-based file line @line (the window does not scroll)."""
    return s.display(drain=0.0)[line + 1]


def _status_row(disp):
    for i, row in enumerate(disp):
        if row.startswith("m") and ":" in row:
            return i
    return -1


def _saved(s):
    s.key(F2, delay=0.7)
    return s.text()


# @scenario SC-buffer-05
def test_colours_after_a_whole_buffer_swap_are_the_new_texts(tmp_path):
    """An accepted Edit that adds two lines above a comment: the comment's
    colours are those of the same text opened fresh, not of the old rows."""
    if not _ai_build():
        pytest.skip("wpe built without --enable-ai")
    seed = ("int a;\n"
            "/* comment one\n"
            "   comment two\n"
            "   comment three */\n"
            "int b;\n"
            "int c;\n")
    reply = "int x;\nint y;\n" + seed
    cols = 22
    rows = range(2, 11)

    def colours(s):
        out = []
        for y in rows:
            row = s.screen.buffer[y]
            out.append(("".join(row[x].data for x in range(1, cols)).rstrip(),
                        tuple((row[x].fg, row[x].bold) for x in range(1, cols))))
        return out

    ref_dir = tmp_path / "fresh"
    swap_dir = tmp_path / "swap"
    ref_dir.mkdir()
    swap_dir.mkdir()
    with WpeSession(str(ref_dir), reply) as r:
        r._drain(0.6)
        fresh = colours(r)
    with WpeSession(str(swap_dir), seed, env_extra=_ai_env(swap_dir, reply)) as s:
        _accept_edit(s, swap_dir)
        swapped = colours(s)
        assert s.alive(), "wpe died"
    assert [t for t, _ in swapped] == [t for t, _ in fresh], \
        "the swapped text is not the reply:\n%r\n%r" % (swapped, fresh)
    differ = [i for i, (a, b) in enumerate(zip(swapped, fresh)) if a != b]
    assert differ == [], \
        "rows %s are coloured as the old text, not as the new one" % differ


# @scenario SC-buffer-04
@needs_ai
def test_a_breakpoint_below_an_accepted_edit_moves_with_its_line(tmp_path):
    """Two lines added above a breakpoint by an accepted Edit: the breakpoint
    stays on its code, two lines further down."""
    with WpeSession(str(tmp_path), SEED, env_extra=_ai_env(tmp_path, TWO_ADDED)) as s:
        _toggle_breakpoint_at(s, BAZ_ROW)
        assert s.breakpoint_lines() == [BAZ_ROW + 1], "the breakpoint was not set"
        _accept_edit(s, tmp_path)
        assert "baz(a);" in _line_text(s, BAZ_ROW + 3), "the Edit did not add two lines"
        assert s.breakpoint_lines() == [BAZ_ROW + 3], \
            "the breakpoint stayed on line %d under other code: %s" % (
                BAZ_ROW + 1, s.breakpoint_lines())
        assert s.alive(), "wpe died"


# The same file as a formatter leaves it: a blank line after the opening
# brace and the body indented by four.
FORMATTED = ("int main(void)\n"
             "{\n"
             "\n"
             "    int a = 1;\n"
             "    foo(a);\n"
             "    bar(a);\n"
             "    baz(a);\n"
             "    return 0;\n"
             "}\n")
FOO_ROW = 3


# @scenario SC-buffer-04
@needs_ai
def test_a_format_that_adds_a_line_keeps_the_breakpoints_on_their_code(tmp_path):
    """A format adds a line after the brace and re-indents every line below
    it: each breakpoint is still on its own code, one line further down, and
    Ctrl-U puts them back on the lines they had."""
    with WpeSession(str(tmp_path), SEED, env_extra=_ai_env(tmp_path, FORMATTED)) as s:
        _toggle_breakpoint_at(s, FOO_ROW)
        _go_top(s, FOO_ROW)
        _toggle_breakpoint_at(s, BAZ_ROW)
        _go_top(s, BAZ_ROW)
        assert s.breakpoint_lines() == [FOO_ROW + 1, BAZ_ROW + 1], \
            "the breakpoints were not set: %s" % s.breakpoint_lines()
        _accept_edit(s, tmp_path)
        assert "    baz(a);" in _line_text(s, BAZ_ROW + 2), "the format was not applied"
        assert s.breakpoint_lines() == [FOO_ROW + 2, BAZ_ROW + 2], \
            "the breakpoints are on lines %s, their code is on %s" % (
                s.breakpoint_lines(), [FOO_ROW + 2, BAZ_ROW + 2])
        s.key(CTRL_U, delay=0.8)
        assert "  baz(a);" in _line_text(s, BAZ_ROW + 1), "Ctrl-U did not revert the format"
        assert s.breakpoint_lines() == [FOO_ROW + 1, BAZ_ROW + 1], \
            "after Ctrl-U the breakpoints are on lines %s" % s.breakpoint_lines()
        assert s.alive(), "wpe died"


# @scenario SC-buffer-01
@needs_ai
def test_redo_of_an_edit_with_a_hook_restores_both_swaps(tmp_path):
    """An accepted Edit and the post-edit hook's rewrite are one Ctrl-U; one
    Ctrl-R gives the Edit and the hook's rewrite back together."""
    hook = hook_contracts._hook_script(tmp_path, 'printf %s >> "$1"' % hook_contracts.MARK)
    home = hook_contracts._home_with_hook(tmp_path, hook)
    env = hook_contracts._edit_env(home, hook_contracts.EDIT_NEW, tmp_path / "e.trace")
    with WpeSession(str(tmp_path), hook_contracts.EDIT_ORIG, env_extra=env,
                    filename="t.c") as s:
        hook_contracts._accept_edit(s, tmp_path, "e.trace")
        shown = "\n".join(s.display())
        assert "int b = 22;" in shown and hook_contracts.MARK in shown, \
            "the Edit and the hook's rewrite are not both shown:\n" + shown
        s.key(CTRL_U, delay=0.8)
        undone = "\n".join(s.display())
        assert "int b = 2;" in undone and hook_contracts.MARK not in undone, \
            "one Ctrl-U did not take back both:\n" + undone
        s.key(CTRL_R, delay=0.8)
        redone = "\n".join(s.display())
        assert "int b = 22;" in redone, "Ctrl-R did not give the Edit back:\n" + redone
        assert hook_contracts.MARK in redone, \
            "Ctrl-R did not give the hook's rewrite back:\n" + redone
        assert s.alive(), "wpe died"


# @scenario SC-buffer-04
@needs_ai
def test_undo_of_a_swap_brings_the_breakpoint_back_to_its_line(tmp_path):
    """Ctrl-U of the accepted Edit takes the breakpoint back up with its
    line, and Ctrl-R down again."""
    with WpeSession(str(tmp_path), SEED, env_extra=_ai_env(tmp_path, TWO_ADDED)) as s:
        _toggle_breakpoint_at(s, BAZ_ROW)
        _accept_edit(s, tmp_path)
        s.key(CTRL_U, delay=0.8)
        assert "baz(a);" in _line_text(s, BAZ_ROW + 1), "Ctrl-U did not revert the Edit"
        assert s.breakpoint_lines() == [BAZ_ROW + 1], \
            "after Ctrl-U the breakpoint is not on its line: %s" % s.breakpoint_lines()
        s.key(CTRL_R, delay=0.8)
        assert "baz(a);" in _line_text(s, BAZ_ROW + 3), "Ctrl-R did not redo the Edit"
        assert s.breakpoint_lines() == [BAZ_ROW + 3], \
            "after Ctrl-R the breakpoint is not on its line: %s" % s.breakpoint_lines()
        assert s.alive(), "wpe died"


# @scenario SC-buffer-04
def test_a_wrapped_line_moves_the_breakpoint_below_it(tmp_path):
    """Typing into a line that fills the window's width wraps its tail onto
    a new line: the breakpoint on the line below moves down with it."""
    words = " ".join("word%02d" % i for i in range(15))       # 104 bytes
    long_row = "  bar(a); /* " + words + " */"                 # 120 > the width
    seed = SEED.replace("  bar(a);", long_row[:117], 1)
    with WpeSession(str(tmp_path), seed) as s:
        _toggle_breakpoint_at(s, BAZ_ROW)
        assert s.breakpoint_lines() == [BAZ_ROW + 1], "the breakpoint was not set"
        s.key(UP, delay=0.2)                                    # up to the long line
        s.key("ZZZZZ", delay=0.6)
        assert "baz(a);" in _line_text(s, BAZ_ROW + 2), \
            "the long line did not wrap:\n" + "\n".join(s.display()[1:10])
        assert s.breakpoint_lines() == [BAZ_ROW + 2], \
            "the breakpoint stayed on the wrapped tail: %s" % s.breakpoint_lines()
        assert s.alive(), "wpe died"


# @scenario SC-buffer-04
def test_a_deleted_line_with_a_breakpoint_undone_has_it_again(tmp_path):
    """Ctrl-Y on a line with a breakpoint, then Ctrl-U: the line and its
    breakpoint are back."""
    with WpeSession(str(tmp_path), SEED) as s:
        _toggle_breakpoint_at(s, BAZ_ROW)
        s.key(CTRL_Y, delay=0.6)
        assert "return 0;" in _line_text(s, BAZ_ROW + 1), "Ctrl-Y did not delete the line"
        assert s.breakpoint_lines() == [], \
            "a breakpoint stayed after its line went: %s" % s.breakpoint_lines()
        s.key(CTRL_U, delay=0.8)
        assert "baz(a);" in _line_text(s, BAZ_ROW + 1), "Ctrl-U did not bring the line back"
        assert s.breakpoint_lines() == [BAZ_ROW + 1], \
            "the breakpoint did not come back with its line: %s" % s.breakpoint_lines()
        assert s.alive(), "wpe died"


# @scenario SC-buffer-04
def test_a_line_joined_by_backspace_undone_has_its_breakpoint_again(tmp_path):
    """Backspace at the start of a line with a breakpoint joins it to the
    line above; Ctrl-U splits them again and the breakpoint is back."""
    with WpeSession(str(tmp_path), SEED) as s:
        _toggle_breakpoint_at(s, BAZ_ROW)
        s.key(BACKSPACE, delay=0.6)
        assert "bar(a);  baz(a);" in _line_text(s, BAZ_ROW), "Backspace did not join the lines"
        assert s.breakpoint_lines() == [], \
            "a breakpoint stayed after its line was joined: %s" % s.breakpoint_lines()
        s.key(CTRL_U, delay=0.8)
        assert "baz(a);" in _line_text(s, BAZ_ROW + 1), "Ctrl-U did not split the lines"
        assert s.breakpoint_lines() == [BAZ_ROW + 1], \
            "the breakpoint did not come back with its line: %s" % s.breakpoint_lines()
        assert s.alive(), "wpe died"


# @scenario SC-buffer-01
def test_ctrl_y_on_the_only_line_is_undone_by_ctrl_u(tmp_path):
    """The only line of a file deleted with Ctrl-Y comes back with Ctrl-U,
    and Ctrl-R deletes it again without leaving an extra empty line."""
    with WpeSession(str(tmp_path), "hello\n") as s:
        s.key(CTRL_Y, delay=0.6)
        assert _saved(s).strip() == "", "Ctrl-Y did not empty the line"
        s.key(CTRL_U, delay=0.8)
        assert _saved(s) == "hello\n", "Ctrl-U did not bring the line back: %r" % s.text()
        s.key(CTRL_R, delay=0.8)
        assert _saved(s).strip() == "", "Ctrl-R did not delete it again: %r" % s.text()
        assert s.alive(), "wpe died"


# @scenario SC-buffer-02
def test_save_then_ctrl_u_of_a_ctrl_y_marks_the_window_modified(tmp_path):
    """Ctrl-Y, save, Ctrl-U: the window differs from the file again, so it
    reads as modified and quitting asks."""
    with WpeSession(str(tmp_path), SEED) as s:
        _down(s, BAZ_ROW)
        s.key(CTRL_Y, delay=0.6)
        _saved(s)
        s.key(CTRL_U, delay=0.8)
        disp = s.display()
        assert "baz(a);" in disp[BAZ_ROW + 2], "Ctrl-U did not bring the line back"
        status = _status_row(disp)
        assert status >= 0, "no status row:\n" + "\n".join(disp)
        assert disp[status][MODIFIED_MARK_COL] == MODIFIED_MARK, \
            "the window reads as saved after Ctrl-U changed it:\n%r" % disp[status]
        s.key(ALT_X, delay=0.8)
        assert s.alive(), "wpe quit without asking and lost the restored line"
        s.key(ESC, delay=0.5)


# @scenario SC-buffer-01
def test_enter_with_auto_indent_is_one_undo_and_redo_indents_once(tmp_path):
    """Enter on a C line with auto-indent: one Ctrl-U takes back the break
    and the indent, one Ctrl-R gives both back, indented once."""
    seed = "int main()\n{\n  foo();\n}\n"
    after = "int main()\n{\n  \n  foo();\n}\n"
    with WpeSession(str(tmp_path), seed) as s:
        _down(s, 2)
        s.key(RIGHT, RIGHT, delay=0.2)               # before "foo();"
        s.key(ENTER, delay=0.6)
        assert _saved(s) == after, "Enter did not break and indent: %r" % s.text()
        s.key(CTRL_U, delay=0.8)
        assert _saved(s) == seed, "one Ctrl-U did not take back Enter: %r" % s.text()
        s.key(CTRL_R, delay=0.8)
        assert _saved(s) == after, "one Ctrl-R did not give Enter back: %r" % s.text()
        assert s.alive(), "wpe died"


# @scenario SC-buffer-01
def test_a_replacement_undone_and_redone_gives_each_text_back(tmp_path):
    """Search > Replace of one word, Ctrl-U, Ctrl-R: each key gives back
    the text it should, once (the replacement records its own inverse)."""
    seed = "foo 1\nbar\n"
    with WpeSession(str(tmp_path), seed) as s:
        s.menu(ALT.SEARCH, "r")              # Replace dialog, Find field active
        s.key("foo")
        s.key("\033n")
        s.key("ZZZ")                         # Alt-N: New Text
        s.key("\033p")                       # no prompt per replacement
        s.key("\033a")                       # Change All
        s.key(ENTER)                         # the "Replaced" note
        assert _saved(s) == "ZZZ 1\nbar\n", "Replace did not replace: %r" % s.text()
        s.key(CTRL_U, delay=0.8)
        assert _saved(s) == seed, "Ctrl-U did not take the replacement back: %r" % s.text()
        s.key(CTRL_R, delay=0.8)
        assert _saved(s) == "ZZZ 1\nbar\n", "Ctrl-R did not replace again: %r" % s.text()
        assert s.alive(), "wpe died"


# @scenario SC-buffer-03
@needs_ai
def test_marks_after_a_rename_are_inside_the_file(tmp_path):
    """A block marked to the end of a file a tool then shortens: copying it
    reads only lines the file has (the sanitizer build sees any other read)."""
    seed = "".join("int v%d = %d;\n" % (i, i) for i in range(10))
    short = "int v0 = 0;\nint v1 = 1;\nint v2 = 2;\n"
    turns = "TOOL write_file t.c\n%s@@END@@TURN@@DONE wrote it" % short
    env = _ai_env(tmp_path, turns)
    env["XWPE_AI_POLICY"] = "auto"
    with WpeSession(str(tmp_path), seed, env_extra=env) as s:
        _down(s, 5)
        s.key(CTRL_K, "b", delay=0.3)                # block from line 6 ...
        _down(s, 4)
        s.key(CTRL_K, "k", delay=0.3)                # ... to line 10
        for _ in range(9):
            s.key(UP, delay=0.15)                    # back to line 1, outside it
        s.key(ALT.AI)
        s.key("g")
        s._drain(0.5)
        s.key("shorten the file")
        s.key(ENTER, delay=0.8)
        end = time.time() + TURN_S
        while time.time() < end and not _trace_has(tmp_path, "changeset"):
            s._drain(0.3)
        assert _trace_has(tmp_path, "changeset"), "the agent run did not end"
        s.key("a", delay=0.6)                        # keep all
        s.key(ESC, delay=0.6)                        # back to the file
        assert "v9" not in "\n".join(s.display()), "the write did not reach the window"
        s.key(CTRL_K, "c", delay=0.8)                # copy the block here
        s.key(ESC, delay=0.3)
        assert s.alive(), "wpe died copying a block marked past the end of the file"
        text = _saved(s)
        assert text.startswith("int v0 = 0;"), "the file lost its text: %r" % text
        assert "v9" not in text, "a line the tool removed came back: %r" % text


# @scenario SC-buffer-01
@needs_ai
def test_hook_after_a_write_that_changes_nothing_keeps_the_users_typing(tmp_path):
    """The user types and saves; the agent writes the same text back, so the
    window has nothing to change; the post-edit hook then appends a marker.
    One Ctrl-U takes back the hook's change and nothing the user typed."""
    seed = "int main(void){return 0;}\n"
    hook = hook_contracts._hook_script(tmp_path, 'printf %s >> "$1"' % hook_contracts.MARK)
    home = hook_contracts._home_with_hook(tmp_path, hook)
    env = hook_contracts._agent_env(home, "t.c", "XY" + seed, tmp_path / "a.trace")
    with WpeSession(str(tmp_path), seed, env_extra=env, filename="t.c") as s:
        s.key("X", delay=0.3)
        s.key("Y", delay=0.3)
        _saved(s)
        hook_contracts._run_agent_write(s, tmp_path, "a.trace")
        assert hook_contracts.MARK in "\n".join(s.display()), "the hook's change is not shown"
        s.key(CTRL_U, delay=0.8)
        undone = "\n".join(s.display(drain=0.4))
        assert hook_contracts.MARK not in undone, "Ctrl-U did not take the hook's change back"
        assert "XYint" in undone, \
            "one Ctrl-U also took back what the user typed:\n" + undone
        assert s.alive(), "wpe died"


def _go_top(s, rows):
    """Back to column 0 of the first line from @rows lines below it."""
    for _ in range(rows):
        s.key(UP, delay=0.1)
    s.key(HOME, delay=0.2)


def _forced_repaint(s):
    """Page down and back: the window is drawn again from its state."""
    s.key(PAGE_DOWN, delay=0.4)
    s.key(PAGE_UP, delay=0.4)


def _copy_block_to_clipboard(s, down, right):
    """From the cursor, mark a block @down lines and @right columns on and
    copy it to the clipboard with Ctrl-C."""
    s.key(CTRL_K, "b", delay=0.3)
    _down(s, down)
    for _ in range(right):
        s.key(RIGHT, delay=0.1)
    s.key(CTRL_K, "k", delay=0.3)
    s.key(CTRL_C, delay=0.5)


# @scenario SC-buffer-04
def test_a_paste_whose_last_line_wraps_undone_leaves_the_breakpoints_on_their_lines(tmp_path):
    """A block pasted into a line that fills the width makes it wrap; Ctrl-U
    takes the paste back and every breakpoint is on its own code again,
    shown so at once and after the window is drawn again."""
    want = [row + 1 for row in WRAP_SEED_BREAKPOINT_ROWS]
    with WpeSession(str(tmp_path), WRAP_SEED) as s:
        for row in WRAP_SEED_BREAKPOINT_ROWS:
            _go_top(s, WRAP_SEED_BREAKPOINT_ROWS[-1])
            _toggle_breakpoint_at(s, row)
        _go_top(s, WRAP_SEED_BREAKPOINT_ROWS[-1])
        assert s.breakpoint_lines() == want, "the breakpoints were not set"
        _copy_block_to_clipboard(s, 2, 3)            # "int main(void)\n{\n  i"
        _down(s, 1)
        s.key(HOME, RIGHT, delay=0.2)                # column 1 of the long line
        s.key(CTRL_V, delay=0.8)
        assert LONG_ROW[1:40] not in _line_text(s, WRAP_SEED_LONG_ROW + 1), \
            "the paste did not land in the long line"
        s.key(CTRL_U, delay=0.8)
        assert "foo(a);" in _line_text(s, WRAP_SEED_BREAKPOINT_ROWS[0] + 1), \
            "Ctrl-U did not take the paste back"
        assert s.breakpoint_lines(drain=0.3) == want, \
            "after Ctrl-U the margin shows %s, the code is on %s" % (
                s.breakpoint_lines(), want)
        _forced_repaint(s)
        assert s.breakpoint_lines(drain=0.3) == want, \
            "after Ctrl-U the breakpoints are on lines %s, the code is on %s" % (
                s.breakpoint_lines(), want)
        assert s.alive(), "wpe died"


# @scenario SC-buffer-04
def test_the_margin_shows_the_breakpoints_a_paste_moved_at_once(tmp_path):
    """Two lines pasted above a breakpoint: the margin shows it on its code,
    two lines down, as soon as the paste is drawn, without waiting for
    anything else to draw the window again."""
    with WpeSession(str(tmp_path), SEED) as s:
        _toggle_breakpoint_at(s, BAZ_ROW)
        _go_top(s, BAZ_ROW)
        _copy_block_to_clipboard(s, 2, 0)            # "int main(void)\n{\n"
        _down(s, 1)
        s.key(HOME, delay=0.2)                       # column 0 of "  foo(a);"
        s.key(CTRL_V, delay=0.8)
        assert "baz(a);" in _line_text(s, BAZ_ROW + 3), "the paste did not add two lines"
        assert s.breakpoint_lines(drain=0.3) == [BAZ_ROW + 3], \
            "the margin still shows the breakpoint on line %s, the code is on %d" % (
                s.breakpoint_lines(), BAZ_ROW + 3)
        _forced_repaint(s)
        assert s.breakpoint_lines(drain=0.3) == [BAZ_ROW + 3], \
            "the breakpoint is on line %s, its code on %d" % (
                s.breakpoint_lines(), BAZ_ROW + 3)
        assert s.alive(), "wpe died"


# SEED's rows (0-based) the block rows below use.
INT_A_ROW = 2
BAR_ROW = 4
RETURN_ROW = 6


def _breakpoints_at(s, rows):
    """From the top of the file, put a breakpoint on each 0-based row of
    @rows, coming back to the top after each."""
    for row in rows:
        _toggle_breakpoint_at(s, row)
        _go_top(s, row)
    want = [row + 1 for row in rows]
    assert s.breakpoint_lines() == want, \
        "the breakpoints were not set: %s" % s.breakpoint_lines()


def _mark_whole_lines(s, first, past):
    """From the top of the file, mark rows @first up to (not including)
    @past as a block of whole lines, and come back to the top."""
    _down(s, first)
    s.key(CTRL_K, "b", delay=0.3)
    _down(s, past - first)
    s.key(CTRL_K, "k", delay=0.3)
    _go_top(s, past)


def _breakpoints_after(s, key, delay=0.8):
    """Press @key and return the breakpoint lines the margin then shows, once
    drawn at once and once after the window is drawn again (both must agree)."""
    s.key(key, delay=delay)
    shown = s.breakpoint_lines(drain=0.3)
    _forced_repaint(s)
    again = s.breakpoint_lines(drain=0.3)
    assert shown == again, \
        "the margin showed %s, the window drawn again %s" % (shown, again)
    return shown


# @scenario SC-buffer-04
def test_block_copy_above_a_breakpoint_moves_it(tmp_path):
    """Two whole lines copied above two breakpoints (one of them on a copied
    line): both stay on their code, two lines further down; the copy carries
    no breakpoint; Ctrl-U and Ctrl-R move them back and forth with it."""
    with WpeSession(str(tmp_path), SEED) as s:
        _breakpoints_at(s, [FOO_ROW, BAZ_ROW])
        _mark_whole_lines(s, BAR_ROW, RETURN_ROW)    # "  bar(a);" and "  baz(a);"
        _down(s, INT_A_ROW)
        assert _breakpoints_after(s, CTRL_K + "c") == [FOO_ROW + 3, BAZ_ROW + 3], \
            "after the copy the breakpoints are on lines %s, their code on %s" % (
                s.breakpoint_lines(), [FOO_ROW + 3, BAZ_ROW + 3])
        assert "bar(a);" in _line_text(s, INT_A_ROW + 1), "the block was not copied"
        assert _breakpoints_after(s, CTRL_U) == [FOO_ROW + 1, BAZ_ROW + 1], \
            "after Ctrl-U the breakpoints are on lines %s" % s.breakpoint_lines()
        assert "int a = 1;" in _line_text(s, INT_A_ROW + 1), "Ctrl-U did not take the copy back"
        assert _breakpoints_after(s, CTRL_R) == [FOO_ROW + 3, BAZ_ROW + 3], \
            "after Ctrl-R the breakpoints are on lines %s" % s.breakpoint_lines()
        assert s.alive(), "wpe died"


# @scenario SC-buffer-04
def test_block_move_above_a_breakpoint_moves_it(tmp_path):
    """A block of whole lines whose first line has a breakpoint, moved above
    another breakpoint: each breakpoint travels with its code, and Ctrl-U and
    Ctrl-R move them back and forth with the block."""
    with WpeSession(str(tmp_path), SEED) as s:
        _breakpoints_at(s, [FOO_ROW, BAR_ROW])
        _mark_whole_lines(s, BAR_ROW, RETURN_ROW)    # "  bar(a);" and "  baz(a);"
        _down(s, INT_A_ROW)
        moved = [INT_A_ROW + 1, FOO_ROW + 3]         # bar on row 2, foo on row 5
        assert _breakpoints_after(s, CTRL_K + "v") == moved, \
            "after the move the breakpoints are on lines %s, their code on %s" % (
                s.breakpoint_lines(), moved)
        assert "bar(a);" in _line_text(s, INT_A_ROW + 1), "the block did not move"
        assert "foo(a);" in _line_text(s, FOO_ROW + 3), "the block did not move"
        assert _breakpoints_after(s, CTRL_U) == [FOO_ROW + 1, BAR_ROW + 1], \
            "after Ctrl-U the breakpoints are on lines %s" % s.breakpoint_lines()
        assert "bar(a);" in _line_text(s, BAR_ROW + 1), "Ctrl-U did not move the block back"
        assert _breakpoints_after(s, CTRL_R) == moved, \
            "after Ctrl-R the breakpoints are on lines %s" % s.breakpoint_lines()
        assert s.alive(), "wpe died"


# @scenario SC-buffer-04
def test_a_deleted_block_undone_has_its_breakpoint_again(tmp_path):
    """A block of whole lines with a breakpoint on its first and on its
    second line is deleted: they go with it, the one above stays; Ctrl-U
    brings both back on their code, Ctrl-R takes them again."""
    with WpeSession(str(tmp_path), SEED) as s:
        _breakpoints_at(s, [FOO_ROW, BAR_ROW, BAZ_ROW])
        _mark_whole_lines(s, BAR_ROW, RETURN_ROW)    # "  bar(a);" and "  baz(a);"
        assert _breakpoints_after(s, CTRL_K + "y") == [FOO_ROW + 1], \
            "after the delete the breakpoints are on lines %s" % s.breakpoint_lines()
        assert "return 0;" in _line_text(s, BAR_ROW + 1), "the block was not deleted"
        assert _breakpoints_after(s, CTRL_U) == [FOO_ROW + 1, BAR_ROW + 1, BAZ_ROW + 1], \
            "after Ctrl-U the breakpoints are on lines %s" % s.breakpoint_lines()
        assert "baz(a);" in _line_text(s, BAZ_ROW + 1), "Ctrl-U did not bring the block back"
        assert _breakpoints_after(s, CTRL_R) == [FOO_ROW + 1], \
            "after Ctrl-R the breakpoints are on lines %s" % s.breakpoint_lines()
        assert s.alive(), "wpe died"


# Options > Editor > Max. Columns set to 40 (the field holds three digits).
SET_MAX_COLUMNS_40 = (ALT.OPTIONS, "e", "\033m", BACKSPACE, BACKSPACE, BACKSPACE, "4", "0", ENTER)
# One line wider than 40 columns and narrower than the window the file opened in.
WIDE_LINE = " ".join("w%03d" % i for i in range(17))
WIDE_SEED = WIDE_LINE + "\nsecond line\nthird line\n"


# @scenario SC-buffer-09
def test_a_block_delete_undone_after_max_columns_lowered_keeps_its_lines(tmp_path):
    """Max. Columns lowered after the file was opened, a block from column 3
    of a wide line to the next line deleted and undone: the wide line comes
    back as the one row it was, not cut at the new Max. Columns into rows
    narrower than the window (where the first character typed would write
    past the line)."""
    assert len(WIDE_LINE) == 84
    with WpeSession(str(tmp_path), WIDE_SEED) as s:
        for k in SET_MAX_COLUMNS_40:
            s.key(k, delay=0.3)
        s.key(RIGHT, RIGHT, RIGHT, delay=0.2)
        s.key(CTRL_K, "b", delay=0.3)
        s.key(DOWN, delay=0.2)
        s.key(CTRL_K, "k", delay=0.3)
        s.key(CTRL_K, "y", delay=0.8)
        assert "w00ond line" in _line_text(s, 1), \
            "the block was not deleted:\n" + "\n".join(s.display())
        s.key(CTRL_U, delay=0.8)
        assert "second line" in _line_text(s, 2), \
            "after Ctrl-U the wide line takes more than one row:\n" + "\n".join(s.display())
        assert _saved(s) == WIDE_SEED, "Ctrl-U did not give the text back"
        assert s.alive(), "wpe died"


# @scenario SC-buffer-04
def test_a_block_delete_that_joins_two_lines_keeps_the_breakpoint_below_on_its_line(tmp_path):
    """A block from inside one line to inside a line two below is deleted, so
    the two ends join: the breakpoint below moves up with its line, and
    Ctrl-U puts it back where it was."""
    with WpeSession(str(tmp_path), SEED) as s:
        _breakpoints_at(s, [FOO_ROW, RETURN_ROW])
        _down(s, FOO_ROW)
        s.key(RIGHT, RIGHT, RIGHT, RIGHT, delay=0.2)   # "  fo|o(a);"
        s.key(CTRL_K, "b", delay=0.3)
        _down(s, BAZ_ROW - FOO_ROW)                    # "  ba|z(a);"
        s.key(CTRL_K, "k", delay=0.3)
        assert _breakpoints_after(s, CTRL_K + "y") == [FOO_ROW + 1, RETURN_ROW - 1], \
            "after the delete the breakpoints are on lines %s, their code on %s" % (
                s.breakpoint_lines(), [FOO_ROW + 1, RETURN_ROW - 1])
        assert "  foz(a);" in _line_text(s, FOO_ROW + 1), "the two ends did not join"
        assert _breakpoints_after(s, CTRL_U) == [FOO_ROW + 1, RETURN_ROW + 1], \
            "after Ctrl-U the breakpoints are on lines %s" % s.breakpoint_lines()
        assert "bar(a);" in _line_text(s, BAR_ROW + 1), "Ctrl-U did not bring the block back"
        assert s.alive(), "wpe died"


# @scenario SC-buffer-04
def test_redo_of_a_paste_moves_the_breakpoint_below_it_again(tmp_path):
    """Two lines pasted above a breakpoint, Ctrl-U, Ctrl-R: after each key
    the breakpoint is on its own code."""
    with WpeSession(str(tmp_path), SEED) as s:
        _toggle_breakpoint_at(s, BAZ_ROW)
        _go_top(s, BAZ_ROW)
        _copy_block_to_clipboard(s, 2, 0)            # "int main(void)\n{\n"
        _down(s, 1)
        s.key(HOME, delay=0.2)                       # column 0 of "  foo(a);"
        assert _breakpoints_after(s, CTRL_V) == [BAZ_ROW + 3], \
            "after the paste the breakpoint is on line %s" % s.breakpoint_lines()
        assert _breakpoints_after(s, CTRL_U) == [BAZ_ROW + 1], \
            "after Ctrl-U the breakpoint is on line %s" % s.breakpoint_lines()
        assert "baz(a);" in _line_text(s, BAZ_ROW + 1), "Ctrl-U did not take the paste back"
        assert _breakpoints_after(s, CTRL_R) == [BAZ_ROW + 3], \
            "after Ctrl-R the breakpoint is on line %s, its code on %d" % (
                s.breakpoint_lines(), BAZ_ROW + 3)
        assert "baz(a);" in _line_text(s, BAZ_ROW + 3), "Ctrl-R did not paste again"
        assert s.alive(), "wpe died"


# @scenario SC-buffer-01
def test_a_block_copied_onto_its_own_first_line_copies_the_marked_text(tmp_path):
    """A block over three lines copied to its own first line, before the
    block's start: the file holds the marked text twice (no stray bytes),
    and Ctrl-U and Ctrl-R give each text back."""
    block = "int a = 1;\n  foo(a);\n  b"                    # (2,2) to (3,4)
    want = SEED.replace("  int a = 1;\n", " " + block + " int a = 1;\n", 1)
    with WpeSession(str(tmp_path), SEED) as s:
        _down(s, INT_A_ROW)
        s.key(RIGHT, RIGHT, delay=0.2)               # "  |int a = 1;"
        s.key(CTRL_K, "b", delay=0.3)
        _down(s, BAR_ROW - INT_A_ROW)                # the column stays 2
        s.key(RIGHT, delay=0.2)                      # "  b|ar(a);"
        s.key(CTRL_K, "k", delay=0.3)
        _go_top(s, BAR_ROW)
        _down(s, INT_A_ROW)
        s.key(RIGHT, delay=0.2)                      # " | int a = 1;"
        s.key(CTRL_K, "c", delay=0.8)
        assert _saved(s) == want, "the copy is not the marked text: %r" % s.text()
        s.key(CTRL_U, delay=0.8)
        assert _saved(s) == SEED, "Ctrl-U did not take the copy back: %r" % s.text()
        s.key(CTRL_R, delay=0.8)
        assert _saved(s) == want, "Ctrl-R did not copy again: %r" % s.text()
        assert s.alive(), "wpe died"


def _change_case_to_upper(s):
    """Ctrl-K D, Upcase, OK."""
    s.key(CTRL_K, "d", delay=0.6)
    s.key("\033u", delay=0.3)                    # Alt-U: Upcase
    s.key(ENTER, delay=0.6)


# @scenario SC-buffer-01
def test_a_case_change_is_one_undo(tmp_path):
    """A block over two lines turned to upper case: one Ctrl-U gives back
    the old text with the block and the breakpoint where they were, one
    Ctrl-R the upper case again."""
    upper = SEED.replace("  bar(a);", "  BAR(A);").replace("  baz(a);", "  BAZ(a);")
    with WpeSession(str(tmp_path), SEED) as s:
        _breakpoints_at(s, [BAZ_ROW])
        _down(s, BAR_ROW)
        s.key(RIGHT, RIGHT, delay=0.2)               # "  |bar(a);"
        s.key(CTRL_K, "b", delay=0.3)
        _down(s, 1)
        s.key(RIGHT, RIGHT, RIGHT, delay=0.2)        # "  baz|(a);"
        s.key(CTRL_K, "k", delay=0.3)
        _change_case_to_upper(s)
        assert _saved(s) == upper, "the case change did not apply: %r" % s.text()
        s.key(CTRL_U, delay=0.8)
        assert _saved(s) == SEED, "one Ctrl-U did not take the case change back: %r" % s.text()
        assert s.breakpoint_lines(drain=0.3) == [BAZ_ROW + 1], \
            "after Ctrl-U the breakpoint is on line %s" % s.breakpoint_lines()
        s.key(CTRL_R, delay=0.8)
        assert _saved(s) == upper, "one Ctrl-R did not give the upper case back: %r" % s.text()
        assert s.alive(), "wpe died"


# @scenario SC-buffer-04
def test_a_replacement_that_wraps_its_line_moves_the_breakpoint_below_it(tmp_path):
    """Search > Replace makes a line that fills the window's width longer, so
    its tail wraps onto a new line: the breakpoint on the line below moves
    down with it, and Ctrl-U takes it back up."""
    words = " ".join("word%02d" % i for i in range(15))
    long_row = ("  bar(a); /* " + words + " */")[:117]
    seed = SEED.replace("  bar(a);", long_row, 1)
    with WpeSession(str(tmp_path), seed) as s:
        _breakpoints_at(s, [BAZ_ROW])
        s.menu(ALT.SEARCH, "r")              # Replace dialog, Find field active
        s.key("word07")
        s.key("\033n")
        s.key("word07word07")                # Alt-N: New Text, longer
        s.key("\033p")                       # no prompt per replacement
        s.key("\033a")                       # Change All
        s.key(ENTER, delay=0.6)              # the "Replaced" note
        s.key(PAGE_UP, delay=0.4)            # the file's first line at the top again
        assert "baz(a);" in _line_text(s, BAZ_ROW + 2), \
            "the replacement did not wrap the line:\n" + "\n".join(s.display()[1:10])
        assert s.breakpoint_lines(drain=0.3) == [BAZ_ROW + 2], \
            "the breakpoint stayed on the wrapped tail: %s" % s.breakpoint_lines()
        s.key(CTRL_U, delay=0.8)
        assert "baz(a);" in _line_text(s, BAZ_ROW + 1), "Ctrl-U did not take the replacement back"
        assert s.breakpoint_lines(drain=0.3) == [BAZ_ROW + 1], \
            "after Ctrl-U the breakpoint is on line %s" % s.breakpoint_lines()
        assert s.alive(), "wpe died"


# ---- the views and the file reader ------------------------------------------

CTRL_G_RUN = (CTRL_G, "r")       # Debug > Run: start, or continue
CTRL_G_STACK = (CTRL_G, "k")     # Debug > Stack
CTRL_G_WATCH = (CTRL_G, "w")     # Debug > Watches: add a watch
F8 = "\033[19~"                  # step over
NO_MORE_UNDO = "No more Undo"    # the box Edit > Undo shows with nothing to undo

# Three calls deep, so the Stack window lists three frames at the first stop and
# one at the second.  Lines are 1-based; a breakpoint goes on 3 and on 12.
STACK_SEED = ("int leaf(int v)\n"
              "{\n"
              "  return v + 1;\n"
              "}\n"
              "int mid(int v)\n"
              "{\n"
              "  return leaf(v) * 2;\n"
              "}\n"
              "int main(void)\n"
              "{\n"
              "  int x = mid(3);\n"
              "  x = x + 1;\n"
              "  return x;\n"
              "}\n")
STACK_IN_LEAF = 3
STACK_IN_MAIN = 12
STACK_COLS = 100
STACK_ROWS = 34
# The Stack window takes the right third of the screen.
STACK_LEFT_COL = STACK_COLS * 2 // 3 + 1


def _need_gdb():
    if not dbg.shutil.which("gcc") or not dbg.shutil.which("gdb"):
        pytest.skip("no gcc or gdb")


def _stack_session(tmp_path):
    return WpeSession(str(tmp_path), STACK_SEED, filename="p.c", wait=1.5,
                      cols=STACK_COLS, rows=STACK_ROWS)


def _run_to_the_leaf(s):
    """Breakpoints on the leaf's return and on the line after the call, then
    Run: the debugger stops in the leaf, three frames deep."""
    _down(s, STACK_IN_LEAF - 1)
    s.key(CTRL_G, "b", delay=0.4)
    _down(s, STACK_IN_MAIN - STACK_IN_LEAF)
    s.key(CTRL_G, "b", delay=0.4)
    s.key(*CTRL_G_RUN, delay=1.0)
    assert dbg._wait_for(s, lambda: dbg._mark_line(s) == STACK_IN_LEAF,
                         dbg.START_TIMEOUT_S), \
        "Run should stop in the leaf:\n" + dbg._screen(s)


def _open_the_stack(s):
    s.key(*CTRL_G_STACK, delay=0.8)
    assert dbg._wait_for(s, lambda: " Stack " in dbg._screen(s), dbg.STEP_TIMEOUT_S), \
        "Ctrl-G K should open the Stack window:\n" + dbg._screen(s)


def _continue_to_main(s):
    """Continue to the second breakpoint: the stack is one frame now."""
    s.key(*CTRL_G_RUN, delay=0.5)
    assert dbg._wait_for(s, lambda: dbg._mark_line(s) == STACK_IN_MAIN,
                         dbg.STEP_TIMEOUT_S), \
        "Run should continue to main:\n" + dbg._screen(s)


def _stack_window_text(s):
    """The text of the screen's right third, where the Stack window is."""
    return "\n".join(row[STACK_LEFT_COL:] for row in s.display(drain=0.3))


def _stack_window_body(s):
    """The rows of the Stack window between its title and its bottom border."""
    rows = [row[STACK_LEFT_COL:] for row in s.display(drain=0.3)]
    title = next(i for i, row in enumerate(rows) if " Stack " in row)
    body = []
    for row in rows[title + 1:]:
        if row.startswith("q"):
            break
        body.append(row.rstrip())
    return "\n".join(body)


# @scenario SC-buffer-02
def test_the_stack_window_has_no_undo_after_three_refreshes(tmp_path):
    """The Stack window is rewritten at every stop, and what the debugger
    printed is nothing the user did: Edit > Undo in it says there is
    nothing to undo, where it used to put back a row of an earlier listing."""
    _need_gdb()
    with _stack_session(tmp_path) as s:
        _run_to_the_leaf(s)
        _open_the_stack(s)                  # refresh 1: three frames
        _continue_to_main(s)                # refresh 2: one frame
        s.key(F8, delay=0.5)                # refresh 3
        dbg._wait_for(s, lambda: dbg._mark_line(s) == STACK_IN_MAIN + 1,
                      dbg.STEP_TIMEOUT_S)
        s.key(*CTRL_G_STACK, delay=0.8)     # the Stack window on top
        s.menu(ALT.EDIT, "u")
        shown = "\n".join(s.display(drain=0.8))
        assert NO_MORE_UNDO in shown, \
            "Edit > Undo in the Stack window changed it instead of saying " \
            "there is nothing to undo:\n" + shown
        assert s.alive(), "wpe died"


# @scenario SC-buffer-06
def test_a_stack_shrinking_by_two_leaves_no_stale_row(tmp_path):
    """The stack goes from three frames to one: the Stack window shows the
    one frame and no row of the deeper listing."""
    _need_gdb()
    with _stack_session(tmp_path) as s:
        _run_to_the_leaf(s)
        _open_the_stack(s)
        shown = _stack_window_text(s)
        assert "#2" in shown, "the three-frame listing is not shown:\n" + shown
        _continue_to_main(s)
        s.key(*CTRL_G_STACK, delay=0.8)
        rewritten = _stack_window_body(s)
        assert "#0" in rewritten and "main" in rewritten, \
            "the Stack window does not show the frame of main:\n" + rewritten
        s.key(CTRL_W, delay=0.6)                    # close it, list again in a new one
        _open_the_stack(s)
        fresh = _stack_window_body(s)
        assert rewritten == fresh, \
            "the rewritten Stack window keeps a row of the deeper listing; " \
            "a window opened now shows:\n%s\nand the old one shows:\n%s" % (fresh, rewritten)
        assert s.alive(), "wpe died"


# @scenario SC-buffer-02
def test_the_watches_window_has_no_undo_after_a_refresh(tmp_path):
    """The Watches window is rewritten at every stop: Edit > Undo in it says
    there is nothing to undo."""
    _need_gdb()
    with _stack_session(tmp_path) as s:
        _run_to_the_leaf(s)
        s.key(*CTRL_G_WATCH, delay=0.8)
        s.key("v", delay=0.1)
        s.key(ENTER, delay=0.5)
        assert dbg._wait_for(s, lambda: dbg._watch_value(s, "v") == 3,
                             dbg.STEP_TIMEOUT_S), \
            "the Watches window should show v:\n" + dbg._screen(s)
        s.key(F8, delay=0.5)                # the watch is read again at the stop
        dbg._wait_for(s, lambda: dbg._mark_line(s) == STACK_IN_LEAF + 1,
                      dbg.STEP_TIMEOUT_S)
        s.menu(ALT.EDIT, "u")
        shown = "\n".join(s.display(drain=0.8))
        assert NO_MORE_UNDO in shown, \
            "Edit > Undo in the Watches window did not say there is nothing " \
            "to undo:\n" + shown
        assert s.alive(), "wpe died"


INSERT_SEED = "aaa\nbbb\nccc\nddd\n"
INSERT_FILE = "ins.txt"
INSERT_TEXT = "X\nY\n"


def _read_a_file_into_the_window(s, name):
    """Block > Read: the File Manager takes the file's name and puts the file
    at the cursor."""
    s.key(CTRL_K, "r", delay=1.0)
    s.key(name, delay=0.4)
    s.key(ENTER, delay=1.0)


# @scenario SC-buffer-01
def test_a_file_inserted_from_the_file_manager_is_one_undo_and_marks_the_window(tmp_path):
    """Block > Read inside a line: the file's lines go between the two halves
    of the line, the window reads as modified, and one Ctrl-U takes the
    insert back and nothing the user typed before it."""
    (tmp_path / INSERT_FILE).write_text(INSERT_TEXT)
    with WpeSession(str(tmp_path), INSERT_SEED) as s:
        s.key("zz", delay=0.3)                       # the user's own line
        _saved(s)
        _down(s, 1)                                  # "bb|b": the column stays 2
        _read_a_file_into_the_window(s, INSERT_FILE)
        disp = s.display()
        rows = [disp[2 + i][1:-1].rstrip() for i in range(5)]   # inside the borders
        assert rows == ["zzaaa", "bbX", "Y", "b", "ccc"], \
            "the file was not inserted inside the line:\n" + "\n".join(disp)
        status = _status_row(disp)
        assert disp[status][MODIFIED_MARK_COL] == MODIFIED_MARK, \
            "the window does not read as modified after the insert:\n%r" % disp[status]
        s.key(CTRL_U, delay=0.8)
        assert _saved(s) == "zz" + INSERT_SEED, \
            "one Ctrl-U did not take the insert back and keep the typing: %r" % s.text()
        s.key(CTRL_R, delay=0.8)
        assert _saved(s) == "zzaaa\nbbX\nY\nb\nccc\nddd\n", \
            "Ctrl-R did not insert the file again: %r" % s.text()
        assert s.alive(), "wpe died"


# Build output at the rate a noisy compiler makes it: one warning per
# directive, three lines of output each.
RATE_WARNINGS = 3400
RATE_SEED = "#warning w\n" * RATE_WARNINGS + "int main(void) { return 0; }\n"
RATE_COLS = 120
RATE_POLL_S = 0.02
F9 = "\033[20~"                                    # Make
# Seconds from the key to the build's count line, the compiler's own run
# included.  Six runs before the lines went through the buffer owner took
# 0.05 to 0.06 s; the budget leaves room for a loaded machine and still
# fails a path that costs time in proportion to the lines already there.
RATE_BUDGET_S = 2.0


# @scenario SC-buffer-06
def test_messages_output_rate_is_unchanged(tmp_path):
    """A build that prints about 10000 lines reaches Messages in the time it
    took before the lines went through the buffer owner."""
    if not dbg.shutil.which("gcc"):
        pytest.skip("no gcc")
    with WpeSession(str(tmp_path), RATE_SEED, filename="p.c", cols=RATE_COLS) as s:
        done = "%d warnings" % RATE_WARNINGS
        start = time.time()
        s.key(F9, delay=0.02)
        while time.time() - start < RATE_BUDGET_S * 2:
            s._drain(RATE_POLL_S)
            if done in "\n".join(s.display(drain=0)):
                break
        took = time.time() - start
        print("messages rate: %d warnings in %.2f s" % (RATE_WARNINGS, took))
        assert done in "\n".join(s.display(drain=0.3)), \
            "the build's count line never reached Messages in %.0f s" % took
        assert took < RATE_BUDGET_S, \
            "10000 lines of build output took %.1f s, budget %.1f s" % (took, RATE_BUDGET_S)
        assert s.alive(), "wpe died"


# ---- a change refused for want of memory ------------------------------------
#
# XWPE_TEST_FAULT ("<point>:<n>") refuses memory to the first action that
# enters the point, from its n-th allocation of the text's owner on; the
# trace says when it fired, so a misread variable cannot pass.

def _fault_env(tmp_path, spec):
    return {"XWPE_TEST_FAULT": spec, "XWPE_UI_TRACE": str(tmp_path / "ui.trace")}


def _ui_trace(tmp_path):
    p = tmp_path / "ui.trace"
    if not p.exists():
        return ""
    return p.read_text()


def _assert_fault_fired(tmp_path, point):
    assert "buffer fault %s armed" % point in _ui_trace(tmp_path), \
        "the %s fault never fired:\n%s" % (point, _ui_trace(tmp_path))


def _said_out_of_memory(s, tmp_path, what):
    """The status row says the action was not made, and the trace agrees."""
    line = "Out of memory: %s was not made" % what
    return line in "\n".join(s.display(drain=0.3)) and \
        ("buffer nomem %s" % what) in _ui_trace(tmp_path)


def _model_move(text, begin, end, at):
    """The text after moving the block from (x, y) @begin up to @end to the
    place (x, y) @at, as positions in @text."""
    lines = text.split("\n")

    def offset(p):
        return sum(len(l) + 1 for l in lines[:p[1]]) + p[0]
    a, e, c = offset(begin), offset(end), offset(at)
    block, rest = text[a:e], text[:a] + text[e:]
    if c > a:
        c -= len(block)
    return rest[:c] + block + rest[c:]


# Case A of the move: a block of whole lines dropped inside a line.  The
# block's first line is merged into the cursor's line (its breakpoint goes,
# as it always did), the middle line moves whole with its breakpoint.
FAULT_DROP = (2, INT_A_ROW)
FAULT_MOVED = _model_move(SEED, (0, BAR_ROW), (0, RETURN_ROW), FAULT_DROP)
FAULT_MOVED_BREAKPOINTS = [INT_A_ROW + 2]          # baz(a); on row 3
FAULT_SEED_BREAKPOINTS = [BAR_ROW + 1, BAZ_ROW + 1]
FAULT_SWEEP_MAX = 64
FAULT_SWEEP_BATCH = 8       # sessions run side by side, so the sweep stays short


def _block_move_with_fault(s):
    """Breakpoints on bar and baz, the two lines marked, the cursor at
    column 2 of "  int a = 1;", Ctrl-K V."""
    _breakpoints_at(s, [BAR_ROW, BAZ_ROW])
    _mark_whole_lines(s, BAR_ROW, RETURN_ROW)
    _down(s, INT_A_ROW)
    s.key(RIGHT, RIGHT, delay=0.2)
    s.key(CTRL_K, "v", delay=0.8)


def _moved_whole(s):
    return _saved(s) == FAULT_MOVED and \
        s.breakpoint_lines(drain=0.3) == FAULT_MOVED_BREAKPOINTS


def _assert_undone_by_ctrl_u(s, what):
    """A change that went through can be taken back: its record was kept."""
    s.key(CTRL_U, delay=0.8)
    assert _saved(s) == SEED, "Ctrl-U did not take the %s back: %r" % (what, s.text())


def _assert_nothing_moved(s, tmp_path):
    """A refused move: the file, the breakpoints and the status row."""
    assert _saved(s) == SEED, "a refused move changed the text: %r" % s.text()
    assert s.breakpoint_lines(drain=0.3) == FAULT_SEED_BREAKPOINTS, \
        "a refused move left the breakpoints on %s" % s.breakpoint_lines()
    assert _said_out_of_memory(s, tmp_path, "the block move"), \
        "the status row does not say the move was refused:\n" + "\n".join(s.display())


def _assert_block_still_marked(s):
    """The block survived the refusal: Ctrl-K V, with memory, moves it."""
    s.key(CTRL_K, "v", delay=0.8)
    assert _moved_whole(s), \
        "after the refusal Ctrl-K V did not move the block: %r, breakpoints %s" % (
            s.text(), s.breakpoint_lines())


# @scenario SC-buffer-08
@pytest.mark.parametrize("n", range(1, 9))
def test_a_block_move_refused_for_memory_changes_nothing(tmp_path, n):
    """A block move whose memory runs out at its n-th allocation is made
    whole or not at all: refused, the file, the breakpoints and the block
    are as they were and the status row says so; then, with memory, the
    same Ctrl-K V moves the block."""
    with WpeSession(str(tmp_path), SEED, env_extra=_fault_env(tmp_path, "block-move:%d" % n)) as s:
        _block_move_with_fault(s)
        _assert_fault_fired(tmp_path, "block-move")
        if _moved_whole(s):
            _assert_undone_by_ctrl_u(s, "move")
            return
        _assert_nothing_moved(s, tmp_path)
        _assert_block_still_marked(s)
        assert s.alive(), "wpe died"


def _one_faulted_move(tmp_path, n):
    """Run the move with its memory cut at the n-th allocation.
    Return: "whole", or "nothing" when it was refused and changed nothing."""
    work = tmp_path / ("n%02d" % n)
    work.mkdir()
    with WpeSession(str(work), SEED, env_extra=_fault_env(work, "block-move:%d" % n)) as s:
        _block_move_with_fault(s)
        _assert_fault_fired(work, "block-move")
        if _moved_whole(s):
            _assert_undone_by_ctrl_u(s, "move")
            return "whole"
        _assert_nothing_moved(s, work)
        assert s.alive(), "wpe died at n=%d" % n
        return "nothing"


def _faulted_moves(tmp_path, cuts, pool):
    """Run the faulted move once for each cut in @cuts, side by side, each in
    a session of its own.  Return: {cut: outcome}."""
    return dict(zip(cuts, pool.map(lambda n: _one_faulted_move(tmp_path, n), cuts)))


# @scenario SC-buffer-08
def test_a_block_move_fault_sweep_ends_in_a_whole_move(tmp_path):
    """Memory cut at the first allocation of a block move, then the second,
    and on: every refused move changed nothing, the first one is refused,
    and once the cut comes after the move's last allocation it is whole."""
    outcomes = {}
    first = 1
    with concurrent.futures.ThreadPoolExecutor(FAULT_SWEEP_BATCH) as pool:
        while first <= FAULT_SWEEP_MAX and "whole" not in outcomes.values():
            last = min(first + FAULT_SWEEP_BATCH, FAULT_SWEEP_MAX + 1)
            outcomes.update(_faulted_moves(tmp_path, range(first, last), pool))
            first = last
    whole = [n for n in sorted(outcomes) if outcomes[n] == "whole"]
    assert outcomes[1] == "nothing", "the first allocation refused still moved the block"
    assert whole, "no cut up to %d allocations let the move through" % FAULT_SWEEP_MAX
    assert all(outcomes[n] == "nothing" for n in outcomes if n < whole[0]), \
        "a cut before the move's last allocation let it through: %s" % outcomes
    print("block move: whole from allocation %d on" % whole[0])


FAULT_DELETED = SEED.replace("  bar(a);\n  baz(a);\n", "", 1)


# @scenario SC-buffer-08
@pytest.mark.parametrize("n", range(1, 9))
def test_a_block_delete_refused_for_memory_changes_nothing(tmp_path, n):
    """A block delete whose memory runs out at its n-th allocation (its undo
    record, the buffer that keeps the lines, the move into it) deletes the
    block whole, or leaves the file, the breakpoints and the block as they
    were and says so; then, with memory, Ctrl-K Y deletes it."""
    with WpeSession(str(tmp_path), SEED, env_extra=_fault_env(tmp_path, "block-move:%d" % n)) as s:
        _breakpoints_at(s, [BAR_ROW, BAZ_ROW])
        _mark_whole_lines(s, BAR_ROW, RETURN_ROW)
        s.key(CTRL_K, "y", delay=0.8)
        _assert_fault_fired(tmp_path, "block-move")
        if _saved(s) == FAULT_DELETED:
            assert s.breakpoint_lines(drain=0.3) == [], "the deleted lines left a breakpoint"
            _assert_undone_by_ctrl_u(s, "delete")
            assert s.breakpoint_lines(drain=0.3) == FAULT_SEED_BREAKPOINTS, \
                "after Ctrl-U the breakpoints are on %s" % s.breakpoint_lines()
            return
        assert s.text() == SEED, "a refused delete changed the text: %r" % s.text()
        assert s.breakpoint_lines(drain=0.3) == FAULT_SEED_BREAKPOINTS, \
            "a refused delete left the breakpoints on %s" % s.breakpoint_lines()
        assert _said_out_of_memory(s, tmp_path, "the block deletion"), \
            "the status row does not say the delete was refused:\n" + "\n".join(s.display())
        s.key(CTRL_K, "y", delay=0.8)
        assert _saved(s) == FAULT_DELETED, \
            "after the refusal Ctrl-K Y did not delete the block: %r" % s.text()
        s.key(CTRL_U, delay=0.8)
        assert _saved(s) == SEED, "Ctrl-U did not bring the block back: %r" % s.text()
        assert s.alive(), "wpe died"


# The move of test_block_move_above_a_breakpoint_moves_it: whole lines to
# column 0, so both breakpoints travel and come back.
UNDO_FAULT_MOVED = _model_move(SEED, (0, BAR_ROW), (0, RETURN_ROW), (0, INT_A_ROW))


# @scenario SC-buffer-08
def test_an_undo_of_a_block_move_refused_for_memory_keeps_its_record(tmp_path):
    """Ctrl-U of a block move refused for want of memory: the text stays
    moved and the status row says so; the next Ctrl-U still has the move to
    take back, and takes it back with its breakpoints."""
    with WpeSession(str(tmp_path), SEED, env_extra=_fault_env(tmp_path, "undo:1")) as s:
        _breakpoints_at(s, [FOO_ROW, BAR_ROW])
        _mark_whole_lines(s, BAR_ROW, RETURN_ROW)
        _down(s, INT_A_ROW)
        s.key(CTRL_K, "v", delay=0.8)
        assert _saved(s) == UNDO_FAULT_MOVED, "the block did not move: %r" % s.text()
        s.key(CTRL_U, delay=0.8)
        _assert_fault_fired(tmp_path, "undo")
        assert _saved(s) == UNDO_FAULT_MOVED, "a refused Ctrl-U changed the text: %r" % s.text()
        assert _said_out_of_memory(s, tmp_path, "Undo"), \
            "the status row does not say Undo was refused:\n" + "\n".join(s.display())
        s.key(CTRL_U, delay=0.8)
        assert _saved(s) == SEED, \
            "the second Ctrl-U did not take the move back: %r" % s.text()
        assert s.breakpoint_lines(drain=0.3) == [FOO_ROW + 1, BAR_ROW + 1], \
            "after Ctrl-U the breakpoints are on %s" % s.breakpoint_lines()
        assert s.alive(), "wpe died"


UNDO_FAULT_COPIED = SEED.replace("  int a = 1;\n", "  bar(a);\n  baz(a);\n  int a = 1;\n", 1)


# @scenario SC-buffer-08
def test_an_undo_of_a_block_copy_refused_for_memory_keeps_its_record(tmp_path):
    """Ctrl-U of a block copy refused for want of memory: the copy stays
    and the status row says so; the next Ctrl-U still takes the copy back."""
    with WpeSession(str(tmp_path), SEED, env_extra=_fault_env(tmp_path, "undo:1")) as s:
        _mark_whole_lines(s, BAR_ROW, RETURN_ROW)
        _down(s, INT_A_ROW)
        s.key(CTRL_K, "c", delay=0.8)
        assert _saved(s) == UNDO_FAULT_COPIED, "the block was not copied: %r" % s.text()
        s.key(CTRL_U, delay=0.8)
        _assert_fault_fired(tmp_path, "undo")
        assert _saved(s) == UNDO_FAULT_COPIED, "a refused Ctrl-U changed the text: %r" % s.text()
        assert _said_out_of_memory(s, tmp_path, "Undo"), \
            "the status row does not say Undo was refused:\n" + "\n".join(s.display())
        s.key(CTRL_U, delay=0.8)
        assert _saved(s) == SEED, "the second Ctrl-U did not take the copy back: %r" % s.text()
        assert s.alive(), "wpe died"


# A block over three lines dropped at column 1 of the line that fills the
# window's width: the line the block's last part makes is wider than the
# window and wraps.
WRAP_BLOCK_END = (3, 2)                            # "int main(void)\n{\n  i"
WRAP_DROP = (1, WRAP_SEED_LONG_ROW)


def _model_copy(text, begin, end, at):
    """The text after copying the block from (x, y) @begin up to @end to the
    place (x, y) @at."""
    lines = text.split("\n")

    def offset(p):
        return sum(len(l) + 1 for l in lines[:p[1]]) + p[0]
    a, e, c = offset(begin), offset(end), offset(at)
    return text[:c] + text[a:e] + text[c:]


WRAP_COPIED = _model_copy(WRAP_SEED, (0, 0), WRAP_BLOCK_END, WRAP_DROP)
WRAP_BREAKPOINTS = [row + 1 for row in WRAP_SEED_BREAKPOINT_ROWS]


def _mark_the_wrapping_block(s):
    """Breakpoints on foo, bar and baz; the block from the top to column 3 of
    row 2 marked; the cursor at column 1 of the long line."""
    for row in WRAP_SEED_BREAKPOINT_ROWS:
        _go_top(s, WRAP_SEED_BREAKPOINT_ROWS[-1])
        _toggle_breakpoint_at(s, row)
    _go_top(s, WRAP_SEED_BREAKPOINT_ROWS[-1])
    s.key(CTRL_K, "b", delay=0.3)
    _down(s, WRAP_BLOCK_END[1])
    s.key(RIGHT, RIGHT, RIGHT, delay=0.2)
    s.key(CTRL_K, "k", delay=0.3)


def _to_the_long_line(s):
    _down(s, WRAP_DROP[1] - WRAP_BLOCK_END[1])
    s.key(HOME, RIGHT, delay=0.2)


def _assert_copy_whole_or_nothing(s, tmp_path, key, what):
    """After @key with its memory cut: the copy is whole and Ctrl-U takes it
    back, or the file, the line it was dropped into and the breakpoints are
    as they were and the status row says so; then @key, with memory, copies."""
    if _saved(s) == WRAP_COPIED:
        s.key(CTRL_U, delay=0.8)
        assert _saved(s) == WRAP_SEED, "Ctrl-U did not take the %s back: %r" % (what, s.text())
        return
    assert s.text() == WRAP_SEED, "a refused %s changed the text: %r" % (what, s.text())
    assert s.breakpoint_lines(drain=0.3) == WRAP_BREAKPOINTS, \
        "a refused %s left the breakpoints on %s" % (what, s.breakpoint_lines())
    assert _said_out_of_memory(s, tmp_path, "the " + what), \
        "the status row does not say the %s was refused:\n%s" % (what, "\n".join(s.display()))
    s.key(key, delay=0.8)
    assert _saved(s) == WRAP_COPIED, \
        "after the refusal the %s did not land: %r" % (what, s.text())


# @scenario SC-buffer-08
@pytest.mark.parametrize("n", range(1, 9))
def test_a_block_copy_refused_for_memory_changes_nothing(tmp_path, n):
    """Ctrl-K C into a line as wide as the window, its memory cut at the
    n-th allocation: the copy is whole, or nothing changed, the line it was
    dropped into keeps every byte, and the status row says so."""
    with WpeSession(str(tmp_path), WRAP_SEED, env_extra=_fault_env(tmp_path, "block-copy:%d" % n)) as s:
        _mark_the_wrapping_block(s)
        _to_the_long_line(s)
        s.key(CTRL_K, "c", delay=0.8)
        _assert_fault_fired(tmp_path, "block-copy")
        _assert_copy_whole_or_nothing(s, tmp_path, CTRL_K + "c", "block copy")
        assert s.alive(), "wpe died"


# @scenario SC-buffer-08
@pytest.mark.parametrize("n", range(1, 9))
def test_a_paste_refused_for_memory_changes_nothing(tmp_path, n):
    """Ctrl-V into a line as wide as the window, its memory cut at the n-th
    allocation: the paste is whole, or nothing changed, the line it was
    dropped into keeps every byte, and the status row says so."""
    with WpeSession(str(tmp_path), WRAP_SEED, env_extra=_fault_env(tmp_path, "block-copy:%d" % n)) as s:
        _mark_the_wrapping_block(s)
        s.key(CTRL_C, delay=0.5)
        _to_the_long_line(s)
        s.key(CTRL_V, delay=0.8)
        _assert_fault_fired(tmp_path, "block-copy")
        _assert_copy_whole_or_nothing(s, tmp_path, CTRL_V, "paste")
        assert s.alive(), "wpe died"


def _mark_in_line(s, row, begin, end):
    """From the top of the file, mark columns @begin up to @end of @row and
    come back to the top."""
    _down(s, row)
    s.key(HOME, delay=0.1)
    for _ in range(begin):
        s.key(RIGHT, delay=0.05)
    s.key(CTRL_K, "b", delay=0.3)
    for _ in range(end - begin):
        s.key(RIGHT, delay=0.05)
    s.key(CTRL_K, "k", delay=0.3)
    _go_top(s, row)


# @scenario SC-buffer-01
@pytest.mark.parametrize("drop", [1, 8], ids=["earlier", "later"])
def test_a_block_moved_inside_its_own_line_is_undone_and_redone(tmp_path, drop):
    """"ba" of "  bar(a);" moved to an earlier and to a later place of its own
    line: Ctrl-U gives the line back as it was, Ctrl-R the move again."""
    want = _model_move(SEED, (2, BAR_ROW), (4, BAR_ROW), (drop, BAR_ROW))
    with WpeSession(str(tmp_path), SEED) as s:
        _mark_in_line(s, BAR_ROW, 2, 4)
        _down(s, BAR_ROW)
        s.key(HOME, delay=0.1)
        for _ in range(drop):
            s.key(RIGHT, delay=0.05)
        s.key(CTRL_K, "v", delay=0.8)
        assert _saved(s) == want, "the block did not move: %r" % s.text()
        s.key(CTRL_U, delay=0.8)
        assert _saved(s) == SEED, "Ctrl-U did not put the block back: %r" % s.text()
        s.key(CTRL_R, delay=0.8)
        assert _saved(s) == want, "Ctrl-R did not move it again: %r" % s.text()
        assert s.alive(), "wpe died"


WRAP_BAR_ROW = 5                                   # "  bar(a);" in WRAP_SEED


BLOCK_KEY_NAME = {"c": "copy", "v": "move"}        # Ctrl-K <key>


def _wrap_model(key, begin, end, at):
    if key == "c":
        return _model_copy(WRAP_SEED, begin, end, at)
    return _model_move(WRAP_SEED, begin, end, at)


def _drop_into_the_long_line(s, col, key):
    """The cursor to column @col of the line as wide as the window, then
    Ctrl-K @key (c: copy, v: move)."""
    _down(s, WRAP_SEED_LONG_ROW)
    s.key(HOME, delay=0.1)
    for _ in range(col):
        s.key(RIGHT, delay=0.01)
    s.key(CTRL_K, key, delay=0.8)


def _assert_undone_and_redone(s, want, what):
    assert _saved(s) == want, "the %s is not where it was dropped: %r" % (what, s.text())
    s.key(CTRL_U, delay=0.8)
    assert _saved(s) == WRAP_SEED, "Ctrl-U did not take back the %s alone: %r" % (what, s.text())
    s.key(CTRL_R, delay=0.8)
    assert _saved(s) == want, "Ctrl-R did not make the %s again: %r" % (what, s.text())


# @scenario SC-buffer-01
@pytest.mark.parametrize("key", ["c", "v"])
@pytest.mark.parametrize("col", [100, 112, 115, 117])
def test_a_block_dropped_into_a_full_line_is_undone(tmp_path, col, key):
    """"bar" copied or moved to a column near the end of a line as wide as
    the window, so the line wraps before or across it: Ctrl-U takes back the
    change and only it (not the user's own characters), Ctrl-R makes it
    again."""
    want = _wrap_model(key, (2, WRAP_BAR_ROW), (5, WRAP_BAR_ROW), (col, WRAP_SEED_LONG_ROW))
    with WpeSession(str(tmp_path), WRAP_SEED) as s:
        _mark_in_line(s, WRAP_BAR_ROW, 2, 5)
        _drop_into_the_long_line(s, col, key)
        _assert_undone_and_redone(s, want, BLOCK_KEY_NAME[key])
        assert s.alive(), "wpe died"


def _mark_bar_to_k1(s):
    """From the top, mark "bar(a);\n  k" (row 5 column 2 to row 6 column 3)."""
    _down(s, WRAP_BAR_ROW)
    s.key(HOME, RIGHT, RIGHT, delay=0.2)
    s.key(CTRL_K, "b", delay=0.3)
    _down(s, 1)
    s.key(HOME, RIGHT, RIGHT, RIGHT, delay=0.2)
    s.key(CTRL_K, "k", delay=0.3)
    _go_top(s, WRAP_BAR_ROW + 1)


# @scenario SC-buffer-01
@pytest.mark.parametrize("key", ["c", "v"])
@pytest.mark.parametrize("col", [100, 115])
def test_a_block_of_two_lines_dropped_into_a_full_line_is_undone(tmp_path, col, key):
    """A block over two lines copied or moved into a line as wide as the
    window: the line it ends in wraps, and Ctrl-U still takes back exactly
    the change."""
    want = _wrap_model(key, (2, WRAP_BAR_ROW), (3, WRAP_BAR_ROW + 1), (col, WRAP_SEED_LONG_ROW))
    with WpeSession(str(tmp_path), WRAP_SEED) as s:
        _mark_bar_to_k1(s)
        _drop_into_the_long_line(s, col, key)
        _assert_undone_and_redone(s, want, BLOCK_KEY_NAME[key])
        assert s.alive(), "wpe died"


ALT_FILE = "\033f"
ALT_HELP = "\033h"
F3 = "\033OR"                                      # File > Open: the File Manager
USER_LINES = ["printf", "", "", "", "", "end"]


def _a_new_window_with_the_users_lines(s):
    """File > New, the user's six lines typed, the cursor back on the first."""
    s.key(ALT_FILE, delay=0.5)
    s.key("n", delay=1.0)
    s.key(USER_LINES[0], delay=0.3)
    for line in USER_LINES[1:]:
        s.key(ENTER, delay=0.2)
        s.key(line, delay=0.2)
    for _ in range(len(USER_LINES) - 1):
        s.key(UP, delay=0.15)
    s.key(HOME, delay=0.2)


NONAME_TITLE = " Noname "
FRAME_TOP_LEFT = "lq"                              # pyte's line drawing of a corner


def _window_rows(s):
    """The first text rows of the Noname window, inside its left border (the
    window is drawn one cell in and down from the window under it)."""
    disp = s.display(drain=0.5)
    top = [i for i, row in enumerate(disp) if NONAME_TITLE in row]
    if not top:
        return disp[:len(USER_LINES)]
    left = disp[top[0]].index(FRAME_TOP_LEFT)
    return [disp[top[0] + 1 + i][left + 1:].split("   ")[0].rstrip()
            for i in range(len(USER_LINES))]


def _assert_the_users_window_is_whole(s, before):
    assert _window_rows(s) == before, \
        "the user's window changed:\n%s\nwas\n%s" % ("\n".join(_window_rows(s)), "\n".join(before))
    s.key(CTRL_U, delay=0.8)
    assert "end" not in _window_rows(s)[-1], "Ctrl-U no longer takes back the user's typing"


# @scenario SC-buffer-08
def test_a_manual_page_that_cannot_be_read_for_memory_leaves_the_users_window(tmp_path):
    """Help > Topic Search on a word, the page's file refused for memory:
    no page window opens, and the window the user typed in keeps its lines
    and its Undo (the page's clean-up must not run on it)."""
    with WpeSession(str(tmp_path), "seed\n", filename="Help",
                    env_extra=_fault_env(tmp_path, "read:1")) as s:
        _a_new_window_with_the_users_lines(s)
        before = _window_rows(s)
        assert before[0] == "printf" and before[-1] == "end", "the lines were not typed:\n" + \
            "\n".join(before)
        s.key(ALT_HELP, delay=0.5)
        s.key("t", delay=6.0)
        _assert_fault_fired(tmp_path, "read")
        assert "buffer nomem opening the file" in _ui_trace(tmp_path), \
            "the page was not refused:\n" + _ui_trace(tmp_path)
        _assert_the_users_window_is_whole(s, before)
        assert s.alive(), "wpe died"


# @scenario SC-buffer-08
def test_a_file_opened_from_the_file_manager_without_memory_leaves_the_users_window(tmp_path):
    """File > Open of a file whose read is refused for memory: it does not
    open, and the window the user typed in keeps its lines and its Undo."""
    (tmp_path / "u.c").write_text(SEED)
    with WpeSession(str(tmp_path), "seed\n", filename="Help",
                    env_extra=_fault_env(tmp_path, "read:1")) as s:
        _a_new_window_with_the_users_lines(s)
        before = _window_rows(s)
        s.key(F3, delay=1.0)
        s.key("u.c", delay=0.4)
        s.key(ENTER, delay=1.0)
        _assert_fault_fired(tmp_path, "read")
        assert "buffer nomem opening the file" in _ui_trace(tmp_path), \
            "the open was not refused:\n" + _ui_trace(tmp_path)
        s.key(ESC, delay=0.6)
        _assert_the_users_window_is_whole(s, before)
        assert (tmp_path / "u.c").read_text() == SEED, "the file on disk changed"
        assert s.alive(), "wpe died"


# @scenario SC-buffer-04
def test_a_block_moved_to_the_start_of_a_line_leaves_its_breakpoint_on_it(tmp_path):
    """A block that starts inside a line, moved to column 0 of a line with a
    breakpoint: the block's first part goes above that line, its last part
    joins the line's front, and the breakpoint stays on the line's code."""
    want = _model_move(SEED, (2, BAR_ROW), (3, BAZ_ROW), (0, INT_A_ROW))
    with WpeSession(str(tmp_path), SEED) as s:
        _breakpoints_at(s, [INT_A_ROW])
        _down(s, BAR_ROW)
        s.key(RIGHT, RIGHT, delay=0.2)               # "  |bar(a);"
        s.key(CTRL_K, "b", delay=0.3)
        _down(s, 1)
        s.key(RIGHT, delay=0.2)                      # "  b|az(a);"
        s.key(CTRL_K, "k", delay=0.3)
        _go_top(s, BAZ_ROW)
        _down(s, INT_A_ROW)
        s.key(CTRL_K, "v", delay=0.8)
        assert _saved(s) == want, "the block did not move: %r" % s.text()
        assert "int a = 1;" in _line_text(s, INT_A_ROW + 2), "the line did not move down"
        assert s.breakpoint_lines(drain=0.3) == [INT_A_ROW + 2], \
            "the breakpoint is on line %s, its code on %d" % (s.breakpoint_lines(), INT_A_ROW + 2)
        s.key(CTRL_U, delay=0.8)
        assert _saved(s) == SEED, "Ctrl-U did not move the block back: %r" % s.text()
        assert s.breakpoint_lines(drain=0.3) == [INT_A_ROW + 1], \
            "after Ctrl-U the breakpoint is on line %s" % s.breakpoint_lines()
        assert s.alive(), "wpe died"


# @scenario SC-buffer-08
def test_a_file_that_cannot_be_read_for_memory_is_not_opened(tmp_path):
    """A file whose reading runs out of memory does not open as an empty
    window under its name, so F2 cannot empty it on disk; the refusal is
    said."""
    with WpeSession(str(tmp_path), SEED, env_extra=_fault_env(tmp_path, "read:1")) as s:
        _assert_fault_fired(tmp_path, "read")
        assert "Out of memory: opening the file was not made" in "\n".join(s.display()), \
            "the refusal is not on the screen:\n" + "\n".join(s.display())
        s.key(F2, delay=0.7)
        s.key(ALT_X, delay=0.8)
        assert s.text() == SEED, \
            "the file on disk was changed after a refused read: %r" % s.text()
        assert "buffer nomem opening the file" in _ui_trace(tmp_path), \
            "the refusal was not said:\n" + _ui_trace(tmp_path)


# ---- the assistant's pane: rows, roles and marks behind the owner ----

PANE_REPLY = " ".join("w%02d" % i for i in range(19))   # two rows at 80 columns
PANE_ROWS = 30


def _pane_env(tmp_path, reply):
    env = _ai_env(tmp_path, reply)
    env["XWPE_UI_TRACE"] = str(tmp_path / "ui.trace")
    return env


def _open_the_chat_and_ask(s, question="q", last_word="w18"):
    s.key(ALT.AI)
    s.key("a", delay=0.8)
    s.key(question)
    s.key(ENTER, delay=1.5)
    assert last_word in "\n".join(s.display()), \
        "the reply did not land in the pane:\n" + "\n".join(s.display())


def _drag_mark(s, row, col, to_row, to_col):
    """Mark from screen (row, col) to (to_row, to_col), 0-based, with the mouse:
    the arrows belong to the chat's input."""
    s.key("\033[<0;%d;%dM" % (col + 1, row + 1), delay=0.2)
    s.key("\033[<32;%d;%dM" % (to_col + 1, to_row + 1), delay=0.2)
    s.key("\033[<0;%d;%dm" % (to_col + 1, to_row + 1), delay=0.3)


def _pane_trace_steps(trace):
    """The trace's pane rows markers and the splices between them, as
    [(rows, roles, splices_that_changed_the_row_count, net_rows)] one entry
    per marker after the first."""
    steps = []
    running = None
    grown = 0
    net = 0
    for line in trace.splitlines():
        if line.startswith("pane rows="):
            rows, roles = [int(x.split("=")[1]) for x in line.split()[1:3]]
            if running is not None:
                steps.append((rows, roles, grown, running + net))
            running = rows
            grown = 0
            net = 0
        elif line.startswith("buffer splice ") and running is not None:
            parts = dict(p.split("=") for p in line.split()[2:] if "=" in p)
            delta = int(parts["ins"]) - int(parts["del"])
            net += delta
            if delta != 0:
                grown += 1
    return steps


# @scenario SC-buffer-03
@needs_ai
def test_a_mark_in_the_pane_survives_a_narrowing_resize(tmp_path):
    """A block marked on the reply, then the pane narrowed and widened again:
    the editor lives (under the sanitizer build a mark left past the rows is
    a read of freed memory), a copy takes only text that is in the pane, and
    the pane still reads whole."""
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=_pane_env(tmp_path, PANE_REPLY), filename="t.c") as s:
        _open_the_chat_and_ask(s)
        disp = s.display()
        first = next(i for i, r in enumerate(disp) if "AI: w00" in r)
        _drag_mark(s, first, 2, first + 1, 30)
        for cols in (40, 80, 120, 40):
            s.resize(cols, PANE_ROWS, delay=0.8)
            s._drain(0.5)
            assert s.alive(), "the editor died at %d columns" % cols
        s.key(CTRL_C, delay=0.6)
        assert s.alive(), "the editor died copying the marked block"
        payload = s.osc52_payload()
        words = set(PANE_REPLY.split()) | {"AI:", "You:", "q", ">"}
        assert payload is None or set(payload.split()) <= words, \
            "the copy holds text that is not in the pane: %r" % payload
        pane = "\n".join(s.display(drain=0))
    for w in PANE_REPLY.split():
        assert w in pane, "%s is gone from the narrowed pane:\n%s" % (w, pane)


# @scenario SC-buffer-03
@needs_ai
def test_a_mark_on_the_prompt_rows_survives_the_prompt_shrinking(tmp_path):
    """Marked across three prompt rows, then two Backspaces and the row that
    held the end of the mark is gone: the mark moves up onto the rows that
    remain, so the copy takes what is left of it (and not freed memory, which
    the sanitizer build reports as a heap-use-after-free)."""
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=_pane_env(tmp_path, "r"), filename="t.c") as s:
        s.key(ALT.AI)
        s.key("a", delay=0.8)
        s.key("aa")
        s.key("\n", delay=0.3)
        s.key("bb")
        s.key("\n", delay=0.3)
        s.key("cc", delay=0.4)
        disp = s.display()
        top = next(i for i, r in enumerate(disp) if r.startswith("x> aa"))
        _drag_mark(s, top, 2, top + 2, 7)
        for _ in range(4):
            s.key(BACKSPACE, delay=0.3)
        assert s.alive(), "the editor died while the prompt shrank"
        s.key(CTRL_C, delay=0.6)
        assert s.alive(), "the editor died copying the marked block"
        payload = s.osc52_payload()
    assert payload is not None and "aa" in payload, \
        "the rows that remain of the mark were not copied: %r" % payload
    assert set(payload) <= set(" ab\n>"), \
        "the copy holds more than the prompt rows: %r" % payload


# @scenario SC-buffer-07
@needs_ai
def test_roles_and_rows_stay_in_step_through_a_reflow(tmp_path):
    """The reply is laid out again at four widths.  After every change of the
    row count the pane says how many rows and how many roles it has (the two
    are equal), and each change of the count is exactly one splice of the
    buffer's owner that adds up to the count the pane reports."""
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=_pane_env(tmp_path, PANE_REPLY), filename="t.c") as s:
        _open_the_chat_and_ask(s)
        for cols in (40, 80, 120, 40, 80):
            s.resize(cols, PANE_ROWS, delay=0.8)
            s._drain(0.5)
            s.display()
        trace = _ui_trace(tmp_path)
    steps = _pane_trace_steps(trace)
    assert len(steps) >= 4, "the pane reported too few row changes:\n" + trace
    for rows, roles, changes, expected in steps:
        assert rows == roles, \
            "the roles table (%d) is not as long as the buffer (%d):\n%s" \
            % (roles, rows, trace)
        assert changes == 1, \
            "a row change took %d splices of the owner:\n%s" % (changes, trace)
        assert rows == expected, \
            "the splices add up to %d rows, the pane says %d:\n%s" \
            % (expected, rows, trace)
    counts = [rows for rows, _, _, _ in steps]
    assert max(counts) > min(counts), "no resize changed the row count: %r" % counts


END = "\033OF"
TAB = "\t"
INSERT_KEY = "\033[2~"   # Insert: typing overwrites
# A text file (no auto-indent, so Tab is typed as a character) whose first
# line fills the width: one more character makes it wrap.
MARGIN_SEED = LONG_ROW + "\nnext line\n"
# One character wider than LONG_ROW: the widest line a row holds, so the
# Undo of a deletion at its end has to wrap it.
WIDEST_ROW = LONG_ROW + "R"
WIDEST_SEED = WIDEST_ROW + "\nnext line\n"


def _assert_undo_and_redo_on_disk(s, seed, edited, what):
    """The file holds @edited; Ctrl-U gives @seed back, Ctrl-R @edited."""
    assert _saved(s) == edited, "%s gave %r" % (what, s.text())
    s.key(CTRL_U, delay=0.8)
    assert _saved(s) == seed, "Ctrl-U of %s gave %r" % (what, s.text())
    s.key(CTRL_R, delay=0.8)
    assert _saved(s) == edited, "Ctrl-R of %s gave %r" % (what, s.text())


# @scenario SC-buffer-01
@pytest.mark.parametrize("typed", ["Z", TAB, " "], ids=["char", "tab", "space"])
def test_an_undo_of_typing_past_the_margin_gives_the_line_back(tmp_path, typed):
    """A character typed at the end of a line as wide as the window makes
    the line wrap onto the next row: Ctrl-U takes that character away and
    nothing else, Ctrl-R types it again."""
    edited = MARGIN_SEED.replace("Q\n", "Q" + typed + "\n", 1)
    with WpeSession(str(tmp_path), MARGIN_SEED, filename="t.txt") as s:
        s.key(END, delay=0.3)
        s.key(typed, delay=0.5)
        _assert_undo_and_redo_on_disk(s, MARGIN_SEED, edited, "typing %r at the margin" % typed)
        assert s.alive(), "wpe died"


# @scenario SC-buffer-01
def test_a_redo_of_a_deletion_at_the_margin_gives_the_text_back(tmp_path):
    """The last character of the widest line deleted: Ctrl-U puts it back,
    which wraps the line, and Ctrl-R deletes that character again."""
    edited = WIDEST_SEED.replace("R\n", "\n", 1)
    with WpeSession(str(tmp_path), WIDEST_SEED, filename="t.txt") as s:
        s.key(END, delay=0.3)
        s.key(BACKSPACE, delay=0.5)
        _assert_undo_and_redo_on_disk(s, WIDEST_SEED, edited, "the deletion at the margin")
        assert s.alive(), "wpe died"


# @scenario SC-buffer-01
def test_an_undo_of_a_replacement_that_wraps_gives_the_line_back(tmp_path):
    """Search > Replace makes the word near the end of a full line longer, so
    the line wraps before it: Ctrl-U gives the line back as it was, Ctrl-R
    the replacement again."""
    seed = LONG_ROW[:108] + " foo end\nnext line\n"
    edited = seed.replace(" foo ", " foofoofoofoo ", 1)
    with WpeSession(str(tmp_path), seed, filename="t.txt") as s:
        s.menu(ALT.SEARCH, "r")
        s.key("foo")
        s.key("\033n")
        s.key("foofoofoofoo")
        s.key("\033p")                       # no prompt per replacement
        s.key("\033a")                       # Change All
        s.key(ENTER)                         # the "Replaced" note
        _assert_undo_and_redo_on_disk(s, seed, edited, "the replacement")
        assert s.alive(), "wpe died"


# @scenario SC-buffer-01
def test_an_undo_of_overwrite_typing_can_be_redone(tmp_path):
    """Insert switches typing to overwrite; a character typed over another
    is taken back by Ctrl-U and typed over it again by Ctrl-R."""
    seed = "abc\n"
    with WpeSession(str(tmp_path), seed, filename="t.txt") as s:
        s.key(INSERT_KEY, delay=0.4)
        s.key("q", delay=0.4)
        _assert_undo_and_redo_on_disk(s, seed, "qbc\n", "overwriting a with q")
        assert s.alive(), "wpe died"


# A line with no blank to wrap at: the file reader fills its first row to the
# full width, without a line break, and the next row holds the rest.
SOFT_ROW = "a" * 119
SOFT_SEED = SOFT_ROW + "tail\n"


def _make_the_soft_row_the_last(s):
    """Delete the second row of the wrapped line: its first row, which ends
    without a line break, is the last row of the text."""
    s.key(DOWN, delay=0.3)
    s.key(CTRL_Y, delay=0.6)


# @scenario SC-buffer-01
@pytest.mark.parametrize("where", ["home", "end"])
def test_typing_on_a_last_row_without_a_line_break_wraps_it(tmp_path, where):
    """The rest of a wrapped line deleted with Ctrl-Y, a character typed at
    the start or the end of the full row left, which then has to wrap: the
    character is where it was typed and the text keeps every byte."""
    with WpeSession(str(tmp_path), SOFT_SEED, filename="t.txt") as s:
        _make_the_soft_row_the_last(s)
        before = _saved(s)
        assert before.startswith(SOFT_ROW) and "ail" not in before, \
            "Ctrl-Y did not delete the second row: %r" % before
        if where == "home":
            want = "Z" + before
            s.key(HOME, delay=0.2)
        else:
            want = before + "Z"
            s.key(END, delay=0.2)
        s.key("Z", delay=0.6)
        assert s.alive(), "wpe died typing on the last row"
        assert _saved(s) == want, "typing on the last row gave %r" % s.text()
        assert s.alive(), "wpe died"


# @scenario SC-buffer-01
def test_a_paste_into_a_narrower_window_keeps_every_byte(tmp_path):
    """Three lines copied from a window as wide as Max. Columns was, pasted
    into a new window opened after Max. Columns was lowered to 40: the
    line wider than 40 is wrapped onto rows of the new width and keeps
    every byte and its line break; Ctrl-U takes the paste back, Ctrl-R
    pastes it again."""
    long_line = "x" * 50 + " " + "y" * 49 + "Z"
    seed = "first\n" + long_line + "\nthird\nfourth\n"
    pasted = "first\n" + long_line + "\nthird\n"
    with WpeSession(str(tmp_path), seed, filename="t.txt") as s:
        s.key(CTRL_K, "b", delay=0.3)
        _down(s, 3)
        s.key(CTRL_K, "k", delay=0.3)
        s.key(CTRL_C, delay=0.8)
        for k in SET_MAX_COLUMNS_40:
            s.key(k, delay=0.3)
        s.key(ALT.FILE, "n", delay=1.0)              # File > New: 40 wide
        s.key(CTRL_V, delay=1.0)
        s.key(ALT.FILE, "a", delay=0.8)              # Save As
        s.key("out.txt", ENTER, delay=1.0)
        out = os.path.join(str(tmp_path), "out.txt")

        def saved_out():
            s.key(F2, delay=0.8)
            with open(out) as fh:
                return fh.read()

        assert saved_out().startswith(pasted), \
            "the paste lost bytes: %r" % saved_out()
        s.key(CTRL_U, delay=0.8)
        assert pasted not in saved_out() and "first" not in saved_out(), \
            "Ctrl-U did not take the paste back: %r" % saved_out()
        s.key(CTRL_R, delay=0.8)
        assert saved_out().startswith(pasted), \
            "Ctrl-R did not paste every byte again: %r" % saved_out()
        assert s.alive(), "wpe died"


# ---- a row holds Max. Columns bytes, and its line break ----

ROW_WIDTH = 120                          # Max. Columns, as a fresh editor has it
NARROW_RC = "[General]\nMaxColumn : 20\n"
NARROW = 20
ROWS_FIT = -1                            # the dump's rows=: no row past the width
E_ACUTE = "é"                       # two bytes in UTF-8


def _assert_rows_fit(w, what):
    """Every row of window @w holds at most Max. Columns bytes, and the
    editor's own check after the key agrees."""
    wide = [(end, len(data)) for end, data in w.lines if len(data) > w.mx]
    assert not wide, "%s left rows wider than %d: %r" % (what, w.mx, wide)
    assert w.rows == ROWS_FIT, "%s: the editor found row %d past the width" % (what, w.rows)


# @scenario SC-buffer-09
@pytest.mark.parametrize("where", ["middle", "last"])
def test_enter_at_the_end_of_a_full_row_without_a_line_break_keeps_every_byte(tmp_path, where):
    """A line wider than the window: its first row is full and has no line
    break.  End and Enter there give that row its break and keep every
    byte, whether the rest of the line follows below or the row is the last
    of the text."""
    with WpeSession(str(tmp_path), SOFT_SEED, filename="t.txt") as s:
        want = SOFT_ROW + "t\nail\n"
        if where == "last":
            _make_the_soft_row_the_last(s)
            want = SOFT_ROW + "t\n"
        s.key(END, delay=0.3)
        s.key(ENTER, delay=0.6)
        assert s.alive(), "wpe died at Enter on a full row"
        assert _saved(s) == want, "Enter on a full row gave %r" % s.text()
        assert s.alive(), "wpe died"


# @scenario SC-buffer-09
def test_a_line_exactly_max_columns_long_is_one_row(tmp_path):
    """At Max. Columns 20 a line of exactly 20 characters is one row with its
    line break, not a full row and an empty one under it; saved, the file
    is as it was."""
    seed = b"x" * NARROW + b"\ny\n"
    with EditSession(str(tmp_path), seed, filename="t.txt", xwperc=NARROW_RC) as s:
        w = s.last.win
        assert w.mx == NARROW
        assert w.lines == [("H", b"x" * NARROW), ("H", b"y")], w.lines
        _assert_rows_fit(w, "reading the file")
        s.send([F2])
        assert s.disk() == seed, "saved as %r" % s.disk()


# @scenario SC-buffer-09
def test_a_line_one_byte_over_max_columns_wraps_its_last_byte(tmp_path):
    """At Max. Columns 20 a line of 21 characters without a blank fills one
    row and puts its last character on the next."""
    seed = b"x" * (NARROW + 1) + b"\n"
    with EditSession(str(tmp_path), seed, filename="t.txt", xwperc=NARROW_RC) as s:
        w = s.last.win
        assert w.lines == [("S", b"x" * NARROW), ("H", b"x")], w.lines
        _assert_rows_fit(w, "reading the file")


# @scenario SC-buffer-09
def test_a_multibyte_character_at_the_margin_is_not_split(tmp_path):
    """At Max. Columns 20 a line whose last two-byte character would straddle
    the margin is cut before that character: every row is valid UTF-8, and
    the file is saved as it was."""
    seed = ("a" + E_ACUTE * 10 + "\n").encode("utf-8")
    with EditSession(str(tmp_path), seed, filename="t.txt", xwperc=NARROW_RC) as s:
        w = s.last.win
        for end, data in w.lines:
            data.decode("utf-8")           # raises on a split character
        _assert_rows_fit(w, "reading the file")
        s.send([F2])
        assert s.disk() == seed, "saved as %r" % s.disk()


# @scenario SC-buffer-09
def test_typing_at_the_start_of_a_full_row_keeps_its_line_break(tmp_path):
    """A line exactly as long as the window, a character typed at its start:
    the line wraps and keeps its line break, so it does not run into the
    line below."""
    seed = b"x" * ROW_WIDTH + b"\ny\n"
    with EditSession(str(tmp_path), seed, filename="t.txt") as s:
        w = s.send(["z"]).win
        _assert_rows_fit(w, "typing at the start of a full row")
        s.send([F2])
        assert s.disk() == b"z" + b"x" * ROW_WIDTH + b"\ny\n", "saved as %r" % s.disk()


MAN_HELP_TOPIC = (ALT_HELP, "t")         # Help > Topic Search: man on the word


def _a_fake_man(tmp_path, page):
    """A directory holding a `man` that prints @page whatever it is asked."""
    bindir = tmp_path / "bin"
    bindir.mkdir()
    man = bindir / "man"
    man.write_bytes(b"#!/bin/sh\nprintf '%s'\n" % page)
    man.chmod(0o755)
    return {"PATH": "%s:%s" % (bindir, os.environ.get("PATH", ""))}


# @scenario SC-buffer-09
def test_a_manual_page_with_bold_at_the_margin_stays_inside_its_rows(tmp_path):
    """A manual page whose first line runs to the margin with no blank, a
    bold character ending two columns short of it and one more character
    after it: the page opens, the editor goes on answering, and no row of
    the page is wider than the window."""
    page = b"a" * (ROW_WIDTH - 3) + b"b\\bbc\\n"
    with EditSession(str(tmp_path), "printf\n", filename="t.txt",
                     env_extra=_a_fake_man(tmp_path, page)) as s:
        w = s.send(list(MAN_HELP_TOPIC)).win
        assert s.alive(), "wpe died reading the page"
        assert w is not None and w.name != "t.txt", "no page window opened"
        wide = [len(data) for end, data in w.lines if len(data) > w.mx]
        assert not wide, "the page has rows wider than %d: %r" % (w.mx, wide)


# ---- typing at the margin keeps every byte ----

EURO = "€"                               # three bytes in UTF-8
ROW_WIDER = "row wider"                  # the owner's trace of a row it refused


def _keys_typed(text):
    """@text as keys, one character each."""
    return list(text)


def _rights(n):
    return [RIGHT] * n


def _assert_typed_text_on_disk(s, keys, want, what):
    """Type @keys, save: the file is @want, valid UTF-8, every row inside the
    width, and the owner refused nothing."""
    s.trace_since_last()
    w = s.send(keys).win
    _assert_rows_fit(w, what)
    for end, data in w.lines:
        data.decode("utf-8")               # raises on a split character
    s.send([F2])
    refused = [line for line in s.trace_since_last() if ROW_WIDER in line]
    assert s.disk() == want, "%s saved %r" % (what, s.disk())
    assert not refused, "%s: the owner refused a row: %r" % (what, refused)


# The verifier's probes: a character typed where a row has to wrap, at the
# end of a row that ends at a blank, in the middle of an unbroken run, and a
# three-byte character typed into a full row.  Each keeps every byte.
TYPING_PROBES = [
    ("blank-at-margin-hard", NARROW_RC, "x" * 19 + " \nnext\n", [END, "z"],
     "x" * 19 + " z\nnext\n"),
    ("blank-at-margin-soft", NARROW_RC, "a" * 19 + " bbb\n", [END, "z"],
     "a" * 19 + " zbbb\n"),
    ("into-a-full-row-below", NARROW_RC, "a" * 19 + " " + "x" * 20 + "\n", [END, "z"],
     "a" * 19 + " z" + "x" * 20 + "\n"),
    ("three-bytes-into-a-full-row", NARROW_RC, "a " + "x" * 18 + "\n",
     [HOME] + _rights(5) + [EURO], "a xxx" + EURO + "x" * 15 + "\n"),
    ("three-bytes-at-120", None, "a " + "x" * 118 + "\n",
     _rights(50) + [EURO], "a " + "x" * 48 + EURO + "x" * 70 + "\n"),
    ("unbroken-run", None, "A1b2C3d4" * 40 + "\n",
     [HOME] + _keys_typed("Xy ZZZ"), "Xy ZZZ" + "A1b2C3d4" * 40 + "\n"),
    ("into-a-full-row-below-at-120", None, "a" * 119 + " " + "x" * 120 + "\n", [END, "z"],
     "a" * 119 + " z" + "x" * 120 + "\n"),
    ("three-bytes-before-a-line-at-120", None, "a " + "x" * 118 + "\nnext\n",
     [HOME] + _rights(50) + [EURO], "a " + "x" * 48 + EURO + "x" * 70 + "\nnext\n"),
]


# @scenario SC-buffer-09
@pytest.mark.parametrize("name,rc,seed,keys,want", TYPING_PROBES,
                         ids=[p[0] for p in TYPING_PROBES])
def test_typing_where_a_row_must_wrap_keeps_every_byte(tmp_path, name, rc, seed, keys, want):
    """A character typed where its row has to wrap goes in where it was
    typed: the file saved holds every byte typed, no character is split,
    and no row is wider than Max. Columns."""
    with EditSession(str(tmp_path), seed, filename="t.txt", xwperc=rc) as s:
        _assert_typed_text_on_disk(s, keys, want.encode("utf-8"), name)
        assert s.alive(), "wpe died"


# @scenario SC-buffer-09
def test_typing_wraps_after_max_columns_not_before(tmp_path):
    """At Max. Columns 20, twenty characters typed on an empty line stay on
    one row; the twenty-first wraps onto the next row and the file holds
    all twenty-one."""
    with EditSession(str(tmp_path), "\n", filename="t.txt", xwperc=NARROW_RC) as s:
        w = s.send(_keys_typed("a" * NARROW)).win
        assert w.lines == [("H", b"a" * NARROW)], w.lines
        _assert_typed_text_on_disk(s, ["a"], b"a" * (NARROW + 1) + b"\n",
                                   "the character after Max. Columns")
        assert s.last.win.lines == [("S", b"a" * NARROW), ("H", b"a")], s.last.win.lines


# @scenario SC-buffer-09
def test_backspace_joining_a_line_that_exactly_fills_the_row_takes_its_break(tmp_path):
    """At Max. Columns 20, Backspace at the start of a line whose text exactly
    fills what the row above has left joins the whole line, its line break
    with it, and leaves no empty row behind."""
    seed = "abcde\n" + "x" * 15 + "\ntail\n"
    with EditSession(str(tmp_path), seed, filename="t.txt", xwperc=NARROW_RC) as s:
        w = s.send([DOWN, HOME, BACKSPACE]).win
        assert w.lines == [("H", b"abcde" + b"x" * 15), ("H", b"tail")], w.lines
        _assert_rows_fit(w, "the join")


UNDO_CASES = [
    ("enter-at-a-full-soft-row", None, SOFT_SEED, [END, ENTER], SOFT_ROW + "t\nail\n",
     [("S", SOFT_ROW.encode() + b"t"), ("H", b"ail")],
     [("H", SOFT_ROW.encode() + b"t"), ("H", b"ail")]),
    ("typing-after-max-columns", NARROW_RC, "a" * NARROW + "\n", [END, "a"],
     "a" * (NARROW + 1) + "\n",
     [("H", b"a" * NARROW)],
     [("S", b"a" * NARROW), ("H", b"a")]),
]


def _assert_saved(s, want, rows, what):
    """Save: the file is @want and, when @rows is given, the window's rows
    are @rows."""
    w = s.send([F2]).win
    assert s.disk() == want.encode("utf-8"), "%s saved %r" % (what, s.disk())
    if rows is not None:
        assert w.lines == rows, "%s left the rows %r" % (what, w.lines)


# @scenario SC-buffer-09
@pytest.mark.parametrize("name,rc,seed,keys,edited,seed_rows,edited_rows", UNDO_CASES,
                         ids=[c[0] for c in UNDO_CASES])
def test_undo_and_redo_at_a_full_row_give_each_text_back(tmp_path, name, rc, seed, keys,
                                                         edited, seed_rows, edited_rows):
    """Enter at the end of a full row without a line break, and a character
    typed after Max. Columns: Ctrl-U gives the file back as it was, Ctrl-R
    the edited text and its rows again.  (The row a wrap added may stay,
    empty, after Ctrl-U: the file is the same.)"""
    with EditSession(str(tmp_path), seed, filename="t.txt", xwperc=rc) as s:
        assert s.last.win.lines == seed_rows, s.last.win.lines
        s.send(keys)
        _assert_saved(s, edited, edited_rows, name)
        s.send([CTRL_U])
        _assert_saved(s, seed, None, "Ctrl-U of " + name)
        s.send([CTRL_R])
        _assert_saved(s, edited, edited_rows, "Ctrl-R of " + name)
        _assert_rows_fit(s.last.win, name)


# ---- a replacement longer than a row ----

ALT_S, ALT_N, ALT_P, ALT_A = "\033s", "\033n", "\033p", "\033a"
REPLACE_SEED = "int a = 1;\nint b = 2;\nint c = 3;\nreturn 0;\n"
REPLACE_RC = "[General]\nMaxColumn : %d\nNumUndo : 20\n"
REPLACE_TEXT_MAX = 128                   # the bytes New Text takes
LEAVE_ESC_MAX = 4


def _long_text(width, n, wide):
    """@n bytes of `x`, with the character @wide (or none) straddling the
    margin of a row @width wide."""
    if not wide:
        return "x" * n
    rest = n - (width - 1) - len(wide.encode("utf-8"))
    return "x" * (width - 1) + wide + "x" * rest


def _replace_keys(search, text, change_all):
    keys = [ALT_S, "r"] + list(search) + [ALT_N] + list(text) + [ALT_P]
    if change_all:
        return keys + [ALT_A]
    return keys + [ENTER]


def _back_to_the_text(s, d):
    for _ in range(LEAVE_ESC_MAX):
        if d.ctx == "edit":
            return d
        d = s.send([ESC])
    return d


def _assert_rows_whole(w, want, what):
    """The window holds @want, every row valid UTF-8 and within the width."""
    assert w.disk() == want.encode("utf-8"), "%s: %r" % (what, w.disk())
    for end, data in w.lines:
        data.decode("utf-8")
    _assert_rows_fit(w, what)


# @scenario SC-buffer-09
def test_change_all_goes_on_behind_a_replacement_that_wrapped(tmp_path):
    """At Max. Columns 20, Change All of `b` in a line where the first
    replacement makes the row wrap: every occurrence is replaced, once, and
    one Ctrl-U gives the line back (Change All is one action)."""
    seed = "x" * 14 + " b b b\nnext\n"
    edited = seed.replace("b", "yyyyyy")
    with EditSession(str(tmp_path), seed, filename="t.txt", xwperc=REPLACE_RC % NARROW) as s:
        d = _back_to_the_text(s, s.send(_replace_keys("b", "yyyyyy", True)))
        _assert_rows_whole(d.win, edited, "Change All")
        _assert_rows_whole(s.send([CTRL_U]).win, seed, "Ctrl-U")
        assert s.alive(), "wpe died"


ALT_B = "\033b"                          # Replace: Back from Cursor
CTRL_END = "\033[1;5F"                   # the end of the text
# A row exactly Max. Columns 20 wide: the last replacement wraps it after
# the blank before the occurrence next to it, which goes down with it.
BACKWARD_SEED = "ab ab ab ab ab ab ab\n"


# @scenario SC-buffer-09
def test_change_all_backwards_replaces_what_a_wrap_moved_down(tmp_path):
    """At Max. Columns 20, Change All back from the end of a full row: the
    first replacement wraps the row and takes the occurrence before it to
    the row below; that occurrence is replaced too, none is skipped."""
    edited = BACKWARD_SEED.replace("ab", "abab")
    with EditSession(str(tmp_path), BACKWARD_SEED, filename="t.txt",
                     xwperc=REPLACE_RC % NARROW) as s:
        s.send([CTRL_END])
        keys = [ALT_S, "r", "a", "b", ALT_N, "a", "b", "a", "b", ALT_P, ALT_B, ALT_A]
        d = _back_to_the_text(s, s.send(keys))
        _assert_rows_whole(d.win, edited, "Change All back")
        assert s.alive(), "wpe died"


# New Text takes characters up to Latin-1 from the keyboard (a character
# above it, like EURO, empties the field), so the multibyte case is E_ACUTE.
LONG_REPLACEMENTS = [(w, n, wide, all_)
                     for w in (NARROW, ROW_WIDTH)
                     for n in (w + 5, 2 * w + 3) if n <= REPLACE_TEXT_MAX
                     for wide in ("", E_ACUTE)
                     for all_ in (True, False)]


# @scenario SC-buffer-09
@pytest.mark.parametrize("width,n,wide,change_all", LONG_REPLACEMENTS,
                         ids=["w%d-n%d-%s-%s" % (w, n, len(c.encode()), "all" if a else "one")
                              for w, n, c, a in LONG_REPLACEMENTS])
def test_a_replacement_longer_than_a_row_keeps_every_byte(tmp_path, width, n, wide, change_all):
    """Search > Replace of a word by New Text longer than Max. Columns: the
    text goes in over as many rows as it needs, no character cut, and
    Ctrl-U, Ctrl-R, Ctrl-U give each text back; the file saved is the one
    the user had."""
    text = _long_text(width, n, wide)
    edited = REPLACE_SEED.replace("b", text, 1)
    with EditSession(str(tmp_path), REPLACE_SEED, filename="t.txt",
                     xwperc=REPLACE_RC % width) as s:
        s.trace_since_last()
        d = _back_to_the_text(s, s.send(_replace_keys("b", text, change_all)))
        _assert_rows_whole(d.win, edited, "the replacement")
        _assert_rows_whole(s.send([CTRL_U]).win, REPLACE_SEED, "Ctrl-U")
        _assert_rows_whole(s.send([CTRL_R]).win, edited, "Ctrl-R")
        _assert_rows_whole(s.send([CTRL_U]).win, REPLACE_SEED, "the second Ctrl-U")
        s.send([F2])
        refused = [line for line in s.trace_since_last() if ROW_WIDER in line]
        assert s.disk() == REPLACE_SEED.encode("utf-8"), "saved %r" % s.disk()
        assert not refused, refused
        assert s.alive(), "wpe died"


# ---- an Undo or a Redo refused for want of memory -------------------------
#
# One Ctrl-U or Ctrl-R is made whole or not at all, however many records the
# action took.  Refused at any of its allocations, the text, the breakpoints
# and both lists are as they were and the status row says so; the next
# Ctrl-U or Ctrl-R (the fault fires once) makes it.

UNDO_FAULT_ROWS = range(1, 9)
UNDO_SWEEP_MAX = 64
# baz(a); joined onto bar(a); by Backspace at its start.
JOINED = SEED.replace("  bar(a);\n  baz(a);", "  bar(a);  baz(a);", 1)
WHOLE = "whole"
NOTHING = "nothing"


def _assert_replay_said_nothing(work, what):
    """A replay that was made says nothing about memory."""
    assert ("buffer nomem %s" % what) not in _ui_trace(work), \
        "%s was made and still said it ran out of memory" % what


def _replay_whole_or_nothing(s, work, key, what, before, after):
    """Press @key (Ctrl-U or Ctrl-R, @what "Undo" or "Redo") with its fault
    armed: the file is @after and nothing was said (whole), or it is still
    @before and the status row says the action was not made (nothing).
    Return: WHOLE or NOTHING."""
    point = what.lower()
    s.key(key, delay=0.8)
    _assert_fault_fired(work, point)
    said = _said_out_of_memory(s, work, what)
    got = _saved(s)
    if got == after:
        _assert_replay_said_nothing(work, what)
        return WHOLE
    assert got == before, "a refused %s changed the text: %r" % (what, got)
    assert said, "the status row does not say %s was refused:\n%s" % (
        what, "\n".join(s.display()))
    return NOTHING


def _undo_whole_or_nothing(s, work, seed, edited, what):
    """Ctrl-U of an edit that left @edited, with memory refused once: whole,
    one Ctrl-R gives @edited back; refused, the record is still there, so
    the next Ctrl-U gives @seed and Ctrl-R @edited."""
    outcome = _replay_whole_or_nothing(s, work, CTRL_U, "Undo", edited, seed)
    if outcome == WHOLE:
        s.key(CTRL_R, delay=0.8)
        assert _saved(s) == edited, "Ctrl-R after a whole Ctrl-U of %s gave %r" % (what, s.text())
    else:
        _assert_undo_and_redo_on_disk(s, seed, edited, what)
    assert s.alive(), "wpe died"
    return outcome


def _join_with_breakpoint(s):
    """A breakpoint on baz(a);, then Backspace at its start: the line joins
    bar(a); and its breakpoint goes with it into the record."""
    _toggle_breakpoint_at(s, BAZ_ROW)
    s.key(BACKSPACE, delay=0.6)
    assert _saved(s) == JOINED, "Backspace did not join the lines: %r" % s.text()
    assert s.breakpoint_lines(drain=0.3) == [], "a breakpoint stayed on the joined line"


def _join_undone_with_fault(work, n):
    """The join's Ctrl-U with memory refused at its n-th allocation.
    Return: WHOLE or NOTHING, each checked with its breakpoint."""
    with WpeSession(str(work), SEED, env_extra=_fault_env(work, "undo:%d" % n)) as s:
        _join_with_breakpoint(s)
        outcome = _replay_whole_or_nothing(s, work, CTRL_U, "Undo", JOINED, SEED)
        if outcome == WHOLE:
            assert s.breakpoint_lines(drain=0.3) == [BAZ_ROW + 1], \
                "the line came back without its breakpoint: %s" % s.breakpoint_lines()
            s.key(CTRL_R, delay=0.8)
            assert _saved(s) == JOINED, "Ctrl-R did not join the lines again: %r" % s.text()
            return outcome
        assert s.breakpoint_lines(drain=0.3) == [], \
            "a refused Undo put the breakpoint on %s" % s.breakpoint_lines()
        s.key(CTRL_U, delay=0.8)
        assert _saved(s) == SEED, "the next Ctrl-U did not split the lines: %r" % s.text()
        assert s.breakpoint_lines(drain=0.3) == [BAZ_ROW + 1], \
            "the next Ctrl-U left the breakpoint on %s" % s.breakpoint_lines()
        assert s.alive(), "wpe died at n=%d" % n
        return outcome


# @scenario SC-buffer-08
@pytest.mark.parametrize("n", UNDO_FAULT_ROWS)
def test_an_undo_of_a_backspace_join_refused_for_memory_keeps_the_breakpoint_parked(tmp_path, n):
    """Ctrl-U of a line Backspace joined to the one above, refused at its
    n-th allocation: the line stays joined, no breakpoint lands on a line
    the user never marked, the status row says so, and the next Ctrl-U
    splits the line with its breakpoint back on it.  (An Undo once used up
    its record, left the line joined and put the breakpoint on bar.)"""
    _join_undone_with_fault(tmp_path, n)


def _join_undone_in(tmp_path, n):
    work = tmp_path / ("n%02d" % n)
    work.mkdir()
    return _join_undone_with_fault(work, n)


# @scenario SC-buffer-08
def test_an_undo_of_a_backspace_join_fault_sweep_ends_in_a_whole_undo(tmp_path):
    """Memory cut at the first allocation of the join's Undo, then the
    second, and on: every refused Undo changed nothing, the first one is
    refused, and once the cut comes after its last allocation it is whole."""
    outcomes = {}
    first = 1
    with concurrent.futures.ThreadPoolExecutor(FAULT_SWEEP_BATCH) as pool:
        while first <= UNDO_SWEEP_MAX and WHOLE not in outcomes.values():
            last = min(first + FAULT_SWEEP_BATCH, UNDO_SWEEP_MAX + 1)
            cuts = range(first, last)
            outcomes.update(zip(cuts, pool.map(lambda n: _join_undone_in(tmp_path, n), cuts)))
            first = last
    whole = [n for n in sorted(outcomes) if outcomes[n] == WHOLE]
    assert outcomes[1] == NOTHING, "the first allocation refused still made the Undo"
    assert whole, "no cut up to %d allocations let the Undo through" % UNDO_SWEEP_MAX
    assert all(outcomes[n] == NOTHING for n in outcomes if n < whole[0]), \
        "a cut before the Undo's last allocation let it through: %s" % outcomes
    print("join undo: whole from allocation %d on" % whole[0])


# Enter before foo(); in a C file: the line breaks and the new line is
# indented, two records of one step.
INDENT_SEED = "int main()\n{\n  foo();\n}\n"
INDENT_AFTER = "int main()\n{\n  \n  foo();\n}\n"


# @scenario SC-buffer-08
@pytest.mark.parametrize("n", range(1, 13))
def test_an_undo_of_enter_with_auto_indent_refused_for_memory_keeps_both_records(tmp_path, n):
    """Ctrl-U of an Enter that auto-indented, refused at its n-th
    allocation: neither the indent nor the line break is taken back (an
    Undo once took the indent and said the text was unchanged); the next
    Ctrl-U takes both, and one Ctrl-R gives both again."""
    with WpeSession(str(tmp_path), INDENT_SEED,
                    env_extra=_fault_env(tmp_path, "undo:%d" % n)) as s:
        _down(s, 2)
        s.key(RIGHT, RIGHT, delay=0.2)
        s.key(ENTER, delay=0.6)
        _undo_whole_or_nothing(s, tmp_path, INDENT_SEED, INDENT_AFTER, "Enter")


# @scenario SC-buffer-08
@pytest.mark.parametrize("n", UNDO_FAULT_ROWS)
def test_a_redo_of_a_backspace_join_refused_for_memory_keeps_its_record(tmp_path, n):
    """Ctrl-R of an undone join, refused at its n-th allocation: the lines
    stay apart with the breakpoint on baz and the status row says so; the
    next Ctrl-R joins them, and Ctrl-U splits them again."""
    with WpeSession(str(tmp_path), SEED, env_extra=_fault_env(tmp_path, "redo:%d" % n)) as s:
        _join_with_breakpoint(s)
        s.key(CTRL_U, delay=0.8)
        assert _saved(s) == SEED, "Ctrl-U did not split the lines: %r" % s.text()
        outcome = _replay_whole_or_nothing(s, tmp_path, CTRL_R, "Redo", SEED, JOINED)
        if outcome == NOTHING:
            assert s.breakpoint_lines(drain=0.3) == [BAZ_ROW + 1], \
                "a refused Redo moved the breakpoint to %s" % s.breakpoint_lines()
            s.key(CTRL_R, delay=0.8)
            assert _saved(s) == JOINED, "the next Ctrl-R did not join the lines: %r" % s.text()
        assert s.breakpoint_lines(drain=0.3) == [], "a breakpoint stayed on the joined line"
        s.key(CTRL_U, delay=0.8)
        assert _saved(s) == SEED, "Ctrl-U after the Redo gave %r" % s.text()
        assert s.breakpoint_lines(drain=0.3) == [BAZ_ROW + 1], \
            "after Ctrl-U the breakpoint is on %s" % s.breakpoint_lines()
        assert s.alive(), "wpe died"


IN_LINE_DELETED = SEED.replace("  bar(a);\n", "  r(a);\n", 1)


# @scenario SC-buffer-08
@pytest.mark.parametrize("n", UNDO_FAULT_ROWS)
def test_an_in_line_block_delete_refused_for_memory_changes_nothing(tmp_path, n):
    """Ctrl-K Y over "ba" of "  bar(a);", refused at its n-th allocation:
    the line, the block and the status row say it was not made (it was once
    made with no Ctrl-U to take it back); with memory the same Ctrl-K Y
    deletes it, Ctrl-U gives the characters back and Ctrl-R deletes them
    again."""
    with WpeSession(str(tmp_path), SEED, env_extra=_fault_env(tmp_path, "block-move:%d" % n)) as s:
        _mark_in_line(s, BAR_ROW, 2, 4)
        s.key(CTRL_K, "y", delay=0.8)
        _assert_fault_fired(tmp_path, "block-move")
        if _saved(s) == IN_LINE_DELETED:
            _assert_undo_and_redo_on_disk(s, SEED, IN_LINE_DELETED, "the in-line delete")
            return
        assert s.text() == SEED, "a refused delete changed the text: %r" % s.text()
        assert _said_out_of_memory(s, tmp_path, "the block deletion"), \
            "the status row does not say the delete was refused:\n" + "\n".join(s.display())
        s.key(CTRL_K, "y", delay=0.8)
        _assert_undo_and_redo_on_disk(s, SEED, IN_LINE_DELETED, "the delete after the refusal")
        assert s.alive(), "wpe died"


def _with_ba_at(text, row, col):
    """@text with "ba" put in at column @col of 0-based @row."""
    lines = text.split("\n")
    lines[row] = lines[row][:col] + "ba" + lines[row][col:]
    return "\n".join(lines)


IN_LINE_COPIED = _with_ba_at(SEED, BAR_ROW, 8)


# @scenario SC-buffer-08
@pytest.mark.parametrize("n", UNDO_FAULT_ROWS)
def test_an_undo_of_an_in_line_block_copy_refused_for_memory_keeps_its_record(tmp_path, n):
    """"ba" of "  bar(a);" copied before its ";", then Ctrl-U refused at its
    n-th allocation: the copy stays and the status row says so (the copy was
    once taken away with no Ctrl-R to bring it back); the next Ctrl-U takes
    it back and Ctrl-R copies it again."""
    with WpeSession(str(tmp_path), SEED, env_extra=_fault_env(tmp_path, "undo:%d" % n)) as s:
        _mark_in_line(s, BAR_ROW, 2, 4)
        _down(s, BAR_ROW)
        s.key(HOME, delay=0.1)
        for _ in range(8):
            s.key(RIGHT, delay=0.05)
        s.key(CTRL_K, "c", delay=0.8)
        assert _saved(s) == IN_LINE_COPIED, "the block was not copied: %r" % s.text()
        s.key(CTRL_U, delay=0.8)
        _assert_fault_fired(tmp_path, "undo")
        if _saved(s) == SEED:
            s.key(CTRL_R, delay=0.8)
            assert _saved(s) == IN_LINE_COPIED, "Ctrl-R of the copy gave %r" % s.text()
            return
        assert s.text() == IN_LINE_COPIED, "a refused Ctrl-U changed the text: %r" % s.text()
        assert _said_out_of_memory(s, tmp_path, "Undo"), \
            "the status row does not say Undo was refused:\n" + "\n".join(s.display())
        s.key(CTRL_U, delay=0.8)
        assert _saved(s) == SEED, "the Ctrl-U after the refusal gave %r" % s.text()
        s.key(CTRL_R, delay=0.8)
        assert _saved(s) == IN_LINE_COPIED, "Ctrl-R of the copy gave %r" % s.text()
        assert s.alive(), "wpe died"


# @scenario SC-buffer-08
@pytest.mark.parametrize("n", UNDO_FAULT_ROWS)
def test_an_undo_of_a_replacement_refused_for_memory_keeps_its_record(tmp_path, n):
    """Ctrl-U of a word Search > Replace changed, refused at its n-th
    allocation: the new word stays and the status row says so; the next
    Ctrl-U gives the old word back and Ctrl-R the new one."""
    seed, edited = "foo 1\nbar\n", "ZZZ 1\nbar\n"
    with WpeSession(str(tmp_path), seed, env_extra=_fault_env(tmp_path, "undo:%d" % n)) as s:
        s.menu(ALT.SEARCH, "r")
        s.key("foo")
        s.key("\033n")
        s.key("ZZZ")
        s.key("\033p")                       # no prompt per replacement
        s.key("\033a")                       # Change All
        s.key(ENTER)                         # the "Replaced" note
        assert _saved(s) == edited, "Replace did not replace: %r" % s.text()
        _undo_whole_or_nothing(s, tmp_path, seed, edited, "the replacement")


# @scenario SC-buffer-08
@pytest.mark.parametrize("n", UNDO_FAULT_ROWS)
def test_an_undo_of_typing_past_the_margin_refused_for_memory_changes_nothing(tmp_path, n):
    """Ctrl-U of a character typed at the end of a line as wide as the
    window, which wrapped it, refused at its n-th allocation: the character
    stays and the status row says so (an Undo once took it with nothing
    said and lost its Redo); the next Ctrl-U takes it away, Ctrl-R types it
    again."""
    edited = MARGIN_SEED.replace("Q\n", "QZ\n", 1)
    with WpeSession(str(tmp_path), MARGIN_SEED, filename="t.txt",
                    env_extra=_fault_env(tmp_path, "undo:%d" % n)) as s:
        s.key(END, delay=0.3)
        s.key("Z", delay=0.5)
        assert _saved(s) == edited, "typing at the margin gave %r" % s.text()
        _undo_whole_or_nothing(s, tmp_path, MARGIN_SEED, edited, "typing at the margin")


# @scenario SC-buffer-08
@pytest.mark.parametrize("n", UNDO_FAULT_ROWS)
def test_an_undo_of_overwrite_typing_refused_for_memory_changes_nothing(tmp_path, n):
    """Ctrl-U of a character typed over another, refused at its n-th
    allocation (the record its Ctrl-R needs is memory too): the typed
    character stays and the status row says so, never an Undo made without
    its Redo; the next Ctrl-U gives the old character back, Ctrl-R types
    over it again."""
    seed, edited = "abc\n", "qbc\n"
    with WpeSession(str(tmp_path), seed, filename="t.txt",
                    env_extra=_fault_env(tmp_path, "undo:%d" % n)) as s:
        s.key(INSERT_KEY, delay=0.4)
        s.key("q", delay=0.4)
        assert _saved(s) == edited, "overwriting a with q gave %r" % s.text()
        _undo_whole_or_nothing(s, tmp_path, seed, edited, "overwriting a with q")


def _session_with_options(tmp_path, seed, general, filename="t.c", env_extra=None):
    """A session whose options file sets the [General] lines @general."""
    conf = tmp_path / ".config" / "xwpe"
    conf.mkdir(parents=True, exist_ok=True)
    (conf / "xwperc").write_text("[General]\n" + "\n".join(general) + "\n")
    return WpeSession(str(tmp_path), seed, filename=filename, env_extra=env_extra)


# @scenario SC-buffer-01
def test_a_redo_of_an_auto_indented_enter_with_num_undo_0_keeps_both_records(tmp_path):
    """Num. Undo 0 keeps one action to undo: Enter with auto-indent is two
    records of that one action, so one Ctrl-U takes both back, one Ctrl-R
    gives both, and Ctrl-U takes both back again (the replay keeps every
    record of the step, whatever Num. Undo is)."""
    seed = "int main(void)\n{\n    foo();\n}\n"
    after = "int main(void)\n{\n    foo();\n    \n}\n"
    with _session_with_options(tmp_path, seed, ["NumUndo : 0"]) as s:
        _down(s, 2)
        s.key(END, delay=0.2)
        s.key(ENTER, delay=0.6)
        _assert_undo_and_redo_on_disk(s, seed, after, "Enter with auto-indent")
        s.key(CTRL_U, delay=0.8)
        assert _saved(s) == seed, "the second Ctrl-U after Redo gave %r" % s.text()
        assert s.alive(), "wpe died"


def _autosave_text(tmp_path):
    p = tmp_path / "t.txt.ASV"
    if not p.exists():
        return None
    return p.read_text()


# @scenario SC-buffer-02
def test_an_autosave_after_undo_counts_characters(tmp_path):
    """Autosave after 2 changes: three characters typed are saved; one
    Ctrl-U is one changed character, not two lines' worth, so the autosave
    file still holds the three; after the second Ctrl-U it holds one."""
    with _session_with_options(tmp_path, "abc\n", ["Autosave : 2", "MaxChanges : 2"],
                               filename="t.txt") as s:
        s.key(END, delay=0.2)
        for ch in "123":
            s.key(ch, delay=0.2)
        s._drain(0.5)
        assert _autosave_text(tmp_path) == "abc123\n", \
            "typing three characters autosaved %r" % _autosave_text(tmp_path)
        s.key(CTRL_U, delay=0.8)
        assert _autosave_text(tmp_path) == "abc123\n", \
            "one Ctrl-U autosaved at once: %r" % _autosave_text(tmp_path)
        s.key(CTRL_U, delay=0.8)
        assert _autosave_text(tmp_path) == "abc1\n", \
            "the second Ctrl-U autosaved %r" % _autosave_text(tmp_path)
        assert s.alive(), "wpe died"


# @scenario SC-buffer-03
def test_an_undo_of_a_join_marks_the_line_break_as_the_block(tmp_path):
    """Backspace at the start of a line joins it; Ctrl-U splits it again and
    marks what it put back, the line break; Ctrl-K Y deletes that block,
    which joins the lines again."""
    seed = "first\nsecond\nthird\n"
    with WpeSession(str(tmp_path), seed, filename="t.txt") as s:
        s.key(DOWN, HOME, delay=0.2)
        s.key(BACKSPACE, delay=0.5)
        assert _saved(s) == "firstsecond\nthird\n", "Backspace did not join: %r" % s.text()
        s.key(CTRL_U, delay=0.8)
        assert _saved(s) == seed, "Ctrl-U did not split the line: %r" % s.text()
        s.key(CTRL_K, "y", delay=0.8)
        assert _saved(s) == "firstsecond\nthird\n", \
            "Ctrl-K Y after the Undo did not delete the line break: %r" % s.text()
        assert s.alive(), "wpe died"


# The block style of the options file: WordStar keys, or the default.
WORDSTAR_GENERAL = ["Options2 : 156"]
CTRL_HOME = "\033[1;5H"


# @scenario SC-buffer-03
@pytest.mark.parametrize("general", [WORDSTAR_GENERAL, []], ids=["wordstar", "modern"])
def test_an_undo_of_a_deletion_marks_the_text_again_after_ctrl_k_h(tmp_path, general):
    """The block hidden with Ctrl-K H, the last character of the first line
    deleted, Ctrl-U: the character is back and marked, so Ctrl-K Y deletes
    it again."""
    seed = "one\ntwo\nthree\nfour\nfive\n"
    with _session_with_options(tmp_path, seed, general, filename="t.txt") as s:
        s.key(DOWN, CTRL_K, "b", DOWN, DOWN, CTRL_K, "k", delay=0.3)
        s.key(CTRL_K, "h", delay=0.3)
        s.key(CTRL_HOME, END, delay=0.3)
        s.key(BACKSPACE, delay=0.5)
        assert _saved(s) == seed.replace("one", "on", 1), "Backspace gave %r" % s.text()
        s.key(CTRL_U, delay=0.8)
        assert _saved(s) == seed, "Ctrl-U did not put the character back: %r" % s.text()
        s.key(CTRL_K, "y", delay=0.8)
        assert _saved(s) == seed.replace("one", "on", 1), \
            "Ctrl-K Y after the Undo did not delete the character put back: %r" % s.text()
        assert s.alive(), "wpe died"


# ---- one Ctrl-U and one Ctrl-R per user action ----
# The original took Tab with auto-indent, Backspace in an indent, Ctrl-K I
# and U, Ctrl-O U, M and H and Change All back one piece at a time.

CTRL_O = "\x0f"          # the editing prefix: U button, M highlight, H headline
# Tab behind the "a": the blanks after it go and the line is indented up to
# foo(); above it, two records.
TAB_SEED = "int main()\n{\n    foo();\na      \n}\n"
TAB_AFTER = "int main()\n{\n    foo();\na   \n}\n"
TAB_ROW = 3
# Backspace behind two tabs: the tabs become blanks, then the indent goes
# back to foo();'s, three records.
INDENT_TABS_SEED = "int main()\n{\n    foo();\n\t\tbar();\n}\n"
INDENT_TABS_AFTER = "int main()\n{\n    foo();\n    bar();\n}\n"
INDENT_TABS_ROW = 3
INDENT_TABS_COL = 2
SHIFT_PLAIN = "one\ntwo\nthree\nfour\n"
SHIFT_INDENTED = "    one\n    two\n    three\nfour\n"   # Tabstops 8: 4 blanks
SHIFT_ROWS = 3
HELP_SEED = "see the topic here\nnext\n"
HELP_MARKED = (4, 7)                     # "the"
HELP_END = "\x05"                        # the mark that ends a button, highlight or headline
HELP_STARTS = {"u": "\x01", "m": "\x0f", "h": "\x02"}
REPLACE_ALL_SEED = "foo 1\nfoo 2\nfoo 3\n"
NUM_UNDO_LEVELS = [["NumUndo : 0"], ["NumUndo : 1"], []]
NUM_UNDO_IDS = ["num-undo-0", "num-undo-1", "num-undo-default"]


def _tab_behind_the_a(s):
    _down(s, TAB_ROW)
    s.key(HOME, RIGHT, delay=0.2)
    s.key(TAB, delay=0.6)


def _backspace_behind_the_tabs(s):
    _down(s, INDENT_TABS_ROW)
    s.key(HOME, delay=0.1)
    for _ in range(INDENT_TABS_COL):
        s.key(RIGHT, delay=0.1)
    s.key(BACKSPACE, delay=0.6)


def _mark_whole_rows_from_the_top(s, n):
    """Mark the first @n lines whole: from the start of the file to the
    start of line @n + 1."""
    s.key(HOME, delay=0.1)
    s.key(CTRL_K, "b", delay=0.3)
    _down(s, n)
    s.key(HOME, delay=0.1)
    s.key(CTRL_K, "k", delay=0.3)


def _shift_block(letter, n):
    def do(s):
        _mark_whole_rows_from_the_top(s, n)
        s.key(CTRL_K, letter, delay=0.8)
    return do


def _help_mark(letter):
    def do(s):
        _mark_in_line(s, 0, *HELP_MARKED)
        s.key(CTRL_O, letter, delay=0.8)
    return do


def _help_marked(letter):
    b, e = HELP_MARKED
    return HELP_SEED[:b] + HELP_STARTS[letter] + HELP_SEED[b:e] + HELP_END + HELP_SEED[e:]


def _change_all(search, text):
    def do(s):
        s.menu(ALT.SEARCH, "r")
        s.key(search)
        s.key(ALT_N)
        s.key(text)
        s.key(ALT_P)                         # no prompt per replacement
        s.key(ALT_A, delay=1.0)              # Change All
        s.key(ENTER, delay=0.6)              # the "Replaced" note
    return do


# name: (file, seed, the text after the action, the keys of the action)
ONE_STEP_ACTIONS = {
    "tab-auto-indent": ("t.c", TAB_SEED, TAB_AFTER, _tab_behind_the_a),
    "backspace-in-indent": ("t.c", INDENT_TABS_SEED, INDENT_TABS_AFTER,
                            _backspace_behind_the_tabs),
    "ctrl-k-i": ("t.txt", SHIFT_PLAIN, SHIFT_INDENTED, _shift_block("i", SHIFT_ROWS)),
    "ctrl-k-u": ("t.txt", SHIFT_INDENTED, SHIFT_PLAIN, _shift_block("u", SHIFT_ROWS)),
    "ctrl-o-u": ("t.txt", HELP_SEED, _help_marked("u"), _help_mark("u")),
    "ctrl-o-m": ("t.txt", HELP_SEED, _help_marked("m"), _help_mark("m")),
    "ctrl-o-h": ("t.txt", HELP_SEED, _help_marked("h"), _help_mark("h")),
    "change-all": ("t.txt", REPLACE_ALL_SEED, REPLACE_ALL_SEED.replace("foo", "ZZZ"),
                   _change_all("foo", "ZZZ")),
}


# @scenario SC-buffer-01
@pytest.mark.parametrize("general", NUM_UNDO_LEVELS, ids=NUM_UNDO_IDS)
@pytest.mark.parametrize("action", sorted(ONE_STEP_ACTIONS))
def test_one_ctrl_u_and_one_ctrl_r_per_action(tmp_path, action, general):
    """Each of these actions changes the text in several pieces; one Ctrl-U
    gives back the whole text from before it and one Ctrl-R the whole text
    after it, whatever Num. Undo is (the original took it back one piece at
    a time, and Num. Undo 0 kept only the last piece)."""
    filename, seed, edited, do = ONE_STEP_ACTIONS[action]
    with _session_with_options(tmp_path, seed, general, filename=filename) as s:
        do(s)
        _assert_undo_and_redo_on_disk(s, seed, edited, action)
        assert s.alive(), "wpe died"


CHANGE_ALL_HITS = 50
CHANGE_ALL_SEED = "".join("foo %d\n" % i for i in range(CHANGE_ALL_HITS))


# @scenario SC-buffer-01
def test_a_change_all_of_fifty_hits_is_one_ctrl_u_under_num_undo_5(tmp_path):
    """Change All replaces 50 words with Num. Undo 5: one Ctrl-U gives all
    50 back and one Ctrl-R replaces all 50 again (Num. Undo once cut the
    action after its last six replacements)."""
    edited = CHANGE_ALL_SEED.replace("foo", "ZZZ")
    with _session_with_options(tmp_path, CHANGE_ALL_SEED, ["NumUndo : 5"],
                               filename="t.txt") as s:
        _change_all("foo", "ZZZ")(s)
        _assert_undo_and_redo_on_disk(s, CHANGE_ALL_SEED, edited, "Change All of 50")
        assert s.alive(), "wpe died"


SHIFT_LONG_ROWS = 30
SHIFT_LONG_SEED = "".join("line %02d\n" % i for i in range(SHIFT_LONG_ROWS)) + "end\n"


# @scenario SC-buffer-01
def test_a_ctrl_k_i_over_thirty_lines_is_one_ctrl_u(tmp_path):
    """Ctrl-K I over a block of 30 lines indents them all; one Ctrl-U takes
    all 30 back and one Ctrl-R indents all 30 again."""
    edited = "".join("    line %02d\n" % i for i in range(SHIFT_LONG_ROWS)) + "end\n"
    with WpeSession(str(tmp_path), SHIFT_LONG_SEED, filename="t.txt") as s:
        _shift_block("i", SHIFT_LONG_ROWS)(s)
        _assert_undo_and_redo_on_disk(s, SHIFT_LONG_SEED, edited, "Ctrl-K I over 30 lines")
        assert s.alive(), "wpe died"


# @scenario SC-buffer-01
def test_num_undo_counts_actions_not_their_pieces(tmp_path):
    """Num. Undo 1 keeps two actions: two Ctrl-K I over three lines are six
    pieces, and two Ctrl-U still give back the text from before both."""
    once = SHIFT_INDENTED
    twice = "        one\n        two\n        three\nfour\n"
    with _session_with_options(tmp_path, SHIFT_PLAIN, ["NumUndo : 1"], filename="t.txt") as s:
        _shift_block("i", SHIFT_ROWS)(s)
        s.key(CTRL_K, "i", delay=0.8)
        assert _saved(s) == twice, "two Ctrl-K I gave %r" % s.text()
        s.key(CTRL_U, delay=0.8)
        assert _saved(s) == once, "the first Ctrl-U gave %r" % s.text()
        s.key(CTRL_U, delay=0.8)
        assert _saved(s) == SHIFT_PLAIN, "the second Ctrl-U gave %r" % s.text()
        assert s.alive(), "wpe died"


def _indent_backspace_undone_with_fault(work, n):
    """The Ctrl-U of a Backspace in a tab indent with memory refused at its
    n-th allocation.  Return: WHOLE or NOTHING."""
    with WpeSession(str(work), INDENT_TABS_SEED,
                    env_extra=_fault_env(work, "undo:%d" % n)) as s:
        _backspace_behind_the_tabs(s)
        assert _saved(s) == INDENT_TABS_AFTER, "Backspace in the indent gave %r" % s.text()
        return _undo_whole_or_nothing(s, work, INDENT_TABS_SEED, INDENT_TABS_AFTER,
                                      "Backspace in an indent")


def _indent_backspace_undone_in(tmp_path, n):
    work = tmp_path / ("n%02d" % n)
    work.mkdir()
    return _indent_backspace_undone_with_fault(work, n)


# @scenario SC-buffer-08
def test_an_undo_of_a_backspace_in_an_indent_fault_sweep_ends_in_a_whole_undo(tmp_path):
    """Memory cut at the first allocation of the Undo of a Backspace that
    re-indented a line (three pieces of one action), then the second, and
    on: every refused Undo changed nothing and said so, and once the cut
    comes after its last allocation the whole action is taken back and one
    Ctrl-R gives it again."""
    outcomes = {}
    first = 1
    with concurrent.futures.ThreadPoolExecutor(FAULT_SWEEP_BATCH) as pool:
        while first <= UNDO_SWEEP_MAX and WHOLE not in outcomes.values():
            last = min(first + FAULT_SWEEP_BATCH, UNDO_SWEEP_MAX + 1)
            cuts = range(first, last)
            outcomes.update(zip(cuts, pool.map(
                lambda n: _indent_backspace_undone_in(tmp_path, n), cuts)))
            first = last
    whole = [n for n in sorted(outcomes) if outcomes[n] == WHOLE]
    assert outcomes[1] == NOTHING, "the first allocation refused still made the Undo"
    assert whole, "no cut up to %d allocations let the Undo through" % UNDO_SWEEP_MAX
    assert all(outcomes[n] == NOTHING for n in outcomes if n < whole[0]), \
        "a cut before the Undo's last allocation let it through: %s" % outcomes
    print("indent backspace undo: whole from allocation %d on" % whole[0])


# ---- an action of several pieces refused for want of memory ----
#
# Each action below changes the text in several pieces.  Refused for memory
# at any of its allocations it is made whole or not at all: refused, the
# file is as it was, Ctrl-U finds nothing to undo, and the status row says
# the action was not made.  (Each piece once went on without memory: most
# of the action was made with no Ctrl-U for it, or a Ctrl-U was kept for a
# piece never made, and that Ctrl-U then deleted the user's own text.)

ACTION_FAULT_ROWS = range(1, 9)
ACTION_SWEEP_MAX = 64
# name: (the fault point, the action in the status row's words)
ONE_STEP_FAULTS = {
    "tab-auto-indent": ("indent", "the indent"),
    "backspace-in-indent": ("indent", "the deletion"),
    "ctrl-k-i": ("shift", "the shift to the right"),
    "ctrl-k-u": ("shift", "the shift to the left"),
    "ctrl-o-u": ("help-marks", "the help marks"),
    "ctrl-o-m": ("help-marks", "the help marks"),
    "ctrl-o-h": ("help-marks", "the help marks"),
    "change-all": ("change-all", "Change All"),
}
REPLACED = "Replaced"                    # the box Change All ends with
ROWS_PAST = "names rows past"            # the owner's trace of an action past the text


def _close_the_replace_box(s):
    """Close Change All's "Found / Replaced" box when it shows: a Change All
    refused for memory shows none, and the key would be typed."""
    if REPLACED in "\n".join(s.display(drain=0.6)):
        s.key(ENTER, delay=0.6)


def _change_all_that_may_be_refused(search, text):
    def do(s):
        s.menu(ALT.SEARCH, "r")
        s.key(search)
        s.key(ALT_N)
        s.key(text)
        s.key(ALT_P)                         # no prompt per replacement
        s.key(ALT_A, delay=1.0)              # Change All
        _close_the_replace_box(s)
    return do


def _action_keys(action):
    """The keys of a one-step action, as a refused one must be able to run."""
    if action == "change-all":
        return _change_all_that_may_be_refused("foo", "ZZZ")
    return ONE_STEP_ACTIONS[action][3]


def _undo_finds_nothing(s):
    """Ctrl-U: did it say there is nothing to undo?  Its box is closed."""
    s.key(CTRL_U, delay=0.8)
    shown = "\n".join(s.display(drain=0.3))
    if NO_MORE_UNDO in shown:
        s.key(ENTER, delay=0.4)
        return True
    return False


def _action_whole_or_nothing(s, work, point, what, seed, edited, do):
    """Run the action @do with memory refused from an allocation on.
    Return: WHOLE when the file is @edited, nothing was said, and Ctrl-U and
    Ctrl-R give each text back; NOTHING when the file is still @seed, Ctrl-U
    finds nothing to undo and the status row says @what was not made."""
    do(s)
    _assert_fault_fired(work, point)
    said = _said_out_of_memory(s, work, what)
    got = _saved(s)
    if got == edited:
        assert ("buffer nomem %s" % what) not in _ui_trace(work), \
            "%s was made and still said it ran out of memory" % what
        _assert_undo_and_redo_on_disk(s, seed, edited, what)
        return WHOLE
    assert got == seed, "a refused %s changed the text: %r" % (what, got)
    nothing_to_undo = _undo_finds_nothing(s)
    assert _saved(s) == seed, "Ctrl-U after a refused %s changed the text: %r" % (what, s.text())
    assert nothing_to_undo, "Ctrl-U after a refused %s found something to undo" % what
    assert said, "the status row does not say %s was not made:\n%s" % (
        what, "\n".join(s.display()))
    return NOTHING


def _grouped_action_with_fault(work, action, n):
    """The one-step @action with memory refused from its n-th allocation.
    Return: WHOLE or NOTHING."""
    filename, seed, edited, _do = ONE_STEP_ACTIONS[action]
    point, what = ONE_STEP_FAULTS[action]
    env = _fault_env(work, "%s:%d" % (point, n))
    with _session_with_options(work, seed, [], filename=filename, env_extra=env) as s:
        outcome = _action_whole_or_nothing(s, work, point, what, seed, edited,
                                           _action_keys(action))
        assert s.alive(), "wpe died at n=%d" % n
        return outcome


def _cut_dir(tmp_path, n):
    work = tmp_path / ("n%02d" % n)
    work.mkdir()
    return work


def _fault_sweep(tmp_path, one):
    """Run @one(work, n) for n = 1, 2, ... in batches side by side until one
    is whole.  Return: {n: outcome}, checked: the first cut refused, every
    cut before the first whole one refused."""
    outcomes = {}
    first = 1
    with concurrent.futures.ThreadPoolExecutor(FAULT_SWEEP_BATCH) as pool:
        while first <= ACTION_SWEEP_MAX and WHOLE not in outcomes.values():
            last = min(first + FAULT_SWEEP_BATCH, ACTION_SWEEP_MAX + 1)
            cuts = range(first, last)
            outcomes.update(zip(cuts, pool.map(lambda n: one(_cut_dir(tmp_path, n), n), cuts)))
            first = last
    whole = [n for n in sorted(outcomes) if outcomes[n] == WHOLE]
    assert outcomes[1] == NOTHING, "the first allocation refused still made the action"
    assert whole, "no cut up to %d allocations let the action through" % ACTION_SWEEP_MAX
    assert all(outcomes[n] == NOTHING for n in outcomes if n < whole[0]), \
        "a cut before the action's last allocation let it through: %s" % outcomes
    return outcomes, whole[0]


# @scenario SC-buffer-10
@pytest.mark.parametrize("n", ACTION_FAULT_ROWS)
@pytest.mark.parametrize("action", sorted(ONE_STEP_FAULTS))
def test_a_grouped_action_refused_for_memory_changes_nothing(tmp_path, action, n):
    """Tab and Backspace with auto-indent, Ctrl-K I and U, Ctrl-O U, M and H
    and Change All with memory refused from their n-th allocation: made
    whole, with one Ctrl-U and one Ctrl-R for it, or not made at all, with
    nothing to undo and the status row saying so."""
    _grouped_action_with_fault(tmp_path, action, n)


# @scenario SC-buffer-10
@pytest.mark.parametrize("action", sorted(ONE_STEP_FAULTS))
def test_a_grouped_action_fault_sweep_ends_whole(tmp_path, action):
    """Memory cut at the first allocation of the action, then the second,
    and on: every refused action changed nothing, and once the cut comes
    after its last allocation it is made whole."""
    _outcomes, first_whole = _fault_sweep(
        tmp_path, lambda work, n: _grouped_action_with_fault(work, action, n))
    print("%s: whole from allocation %d on" % (action, first_whole))


def _enter_before_foo(s):
    _down(s, 2)
    s.key(RIGHT, RIGHT, delay=0.2)
    s.key(ENTER, delay=0.6)


def _enter_with_fault(work, n):
    """Enter before foo(); in a C file (a line break and its indent) with
    memory refused from its n-th allocation.  Return: WHOLE or NOTHING."""
    with WpeSession(str(work), INDENT_SEED, env_extra=_fault_env(work, "enter:%d" % n)) as s:
        outcome = _action_whole_or_nothing(s, work, "enter", "a line break", INDENT_SEED,
                                           INDENT_AFTER, _enter_before_foo)
        assert s.alive(), "wpe died at n=%d" % n
        return outcome


# @scenario SC-buffer-10
@pytest.mark.parametrize("n", ACTION_FAULT_ROWS)
def test_enter_with_auto_indent_refused_for_memory_changes_nothing(tmp_path, n):
    """Enter with auto-indent refused from its n-th allocation: the line is
    broken and indented, one Ctrl-U for both, or the line is as it was with
    nothing to undo and the status row saying so (the break was once made
    with no Ctrl-U, or the indent made without the record that takes it
    back)."""
    _enter_with_fault(tmp_path, n)


# @scenario SC-buffer-10
def test_enter_with_auto_indent_fault_sweep_ends_whole(tmp_path):
    """Memory cut at Enter's first allocation, then the second, and on:
    every refused Enter changed nothing, and from its last allocation on it
    is whole."""
    _outcomes, first_whole = _fault_sweep(tmp_path, _enter_with_fault)
    print("enter: whole from allocation %d on" % first_whole)


# @scenario SC-buffer-10
def test_a_refused_backspace_in_an_indent_keeps_the_cursor(tmp_path):
    """Backspace behind two tabs with memory refused from its first
    allocation: the line and the cursor are where the key found them."""
    with EditSession(str(tmp_path), INDENT_TABS_SEED, filename="t.c",
                     env_extra=_fault_env(tmp_path, "indent:1")) as s:
        d = s.send([DOWN] * INDENT_TABS_ROW + [HOME] + [RIGHT] * INDENT_TABS_COL)
        before = d.win.cursor
        d = s.send([BACKSPACE])
        _assert_fault_fired(tmp_path, "indent")
        assert d.win.disk() == INDENT_TABS_SEED.encode("utf-8"), \
            "a refused Backspace changed the text: %r" % d.win.disk()
        assert d.win.cursor == before, \
            "a refused Backspace moved the cursor from %s to %s" % (before, d.win.cursor)
        assert s.alive(), "wpe died"


# A row of exactly Max. Columns 20: the end mark Ctrl-O puts in makes it wrap.
HELP_FULL_SEED = "see the topic here!!\nnext\n"
HELP_FULL_RC = ["MaxColumn : %d" % NARROW]


def _help_full_marked(letter):
    b, e = HELP_MARKED
    return (HELP_FULL_SEED[:b] + HELP_STARTS[letter] + HELP_FULL_SEED[b:e] + HELP_END +
            HELP_FULL_SEED[e:])


# @scenario SC-buffer-10
@pytest.mark.parametrize("n", ACTION_FAULT_ROWS)
def test_a_help_mark_on_a_full_row_refused_for_memory_changes_nothing(tmp_path, n):
    """Ctrl-O U on a word of a row Max. Columns wide, refused from its n-th
    allocation: both marks in, the row wrapped, one Ctrl-U for both; or the
    row as it was, nothing to undo, said (a Ctrl-U was once kept for a mark
    never put in, and it deleted a character of the user's)."""
    env = _fault_env(tmp_path, "help-marks:%d" % n)
    with _session_with_options(tmp_path, HELP_FULL_SEED, HELP_FULL_RC, filename="t.txt",
                               env_extra=env) as s:
        _action_whole_or_nothing(s, tmp_path, "help-marks", "the help marks", HELP_FULL_SEED,
                                 _help_full_marked("u"), _help_mark("u"))
        assert s.alive(), "wpe died"


PROMPT_HITS = 3
PROMPT_QUESTION = "Replace this occurrence"
PROMPT_REFUSED = "Out of memory: the replacement was not made"


def _first_replaced(k):
    """REPLACE_ALL_SEED with its first @k "foo" replaced."""
    return REPLACE_ALL_SEED.replace("foo", "ZZZ", k)


def _change_all_answering_yes(s):
    """Search > Replace "foo" by "ZZZ" with Prompt on Replace, Change All,
    and Y to every question.  Return: the screen the questions end on."""
    s.menu(ALT.SEARCH, "r")
    s.key("foo")
    s.key(ALT_N)
    s.key("ZZZ")
    s.key(ALT_A, delay=1.0)
    shown = "\n".join(s.display(drain=0.4))
    for _ in range(PROMPT_HITS + 1):
        if PROMPT_QUESTION not in shown:
            break
        s.key("y", delay=0.8)
        shown = "\n".join(s.display(drain=0.4))
    return shown


# @scenario SC-buffer-10
@pytest.mark.parametrize("n", ACTION_FAULT_ROWS)
def test_a_change_all_with_a_prompt_refused_for_memory_stops_like_esc(tmp_path, n):
    """Change All asking before each of three replacements, Y to each, with
    memory refused from its n-th allocation: the replacements made are the
    first ones, whole; a refused one stops the command as Esc would and the
    box says why; one Ctrl-U takes back every replacement made."""
    with WpeSession(str(tmp_path), REPLACE_ALL_SEED, filename="t.txt",
                    env_extra=_fault_env(tmp_path, "change-all:%d" % n)) as s:
        shown = _change_all_answering_yes(s)
        _assert_fault_fired(tmp_path, "change-all")
        said = PROMPT_REFUSED in shown
        _close_the_replace_box(s)
        got = _saved(s)
        made = [k for k in range(PROMPT_HITS + 1) if got == _first_replaced(k)]
        assert made, "the replacements made are not the first ones: %r" % got
        assert (made[0] < PROMPT_HITS) == said, \
            "%d of %d replaced, and the box %s say one was refused:\n%s" % (
                made[0], PROMPT_HITS, "does" if said else "does not", shown)
        s.key(CTRL_U, delay=0.8)
        assert _saved(s) == REPLACE_ALL_SEED, \
            "one Ctrl-U did not take back the %d replacements made: %r" % (made[0], s.text())
        assert s.alive(), "wpe died"


# @scenario SC-buffer-10
def test_a_ctrl_k_i_over_a_block_inside_one_row_names_no_row(tmp_path):
    """Ctrl-K I over a block inside one row shifts no line: the text is as
    it was and nothing asks the owner for rows (an empty shift begins
    nothing)."""
    with WpeSession(str(tmp_path), HELP_SEED, filename="t.txt",
                    env_extra={"XWPE_UI_TRACE": str(tmp_path / "ui.trace")}) as s:
        _mark_in_line(s, 0, *HELP_MARKED)
        s.key(CTRL_K, "i", delay=0.8)
        assert _saved(s) == HELP_SEED, "Ctrl-K I inside one row changed the text: %r" % s.text()
        assert ROWS_PAST not in _ui_trace(tmp_path), _ui_trace(tmp_path)
        assert s.alive(), "wpe died"


# The indent of Tab, Backspace and Enter comes from lines above the cursor,
# however far up, past '#' lines: each is read from the whole text.
# Backspace behind the tabs of bar(); goes back to foo();'s indent, past
# the #define between them.
BACKSPACE_PAST_HASH_SEED = "    foo();\n#define X\n\t\tbar();\n"
BACKSPACE_PAST_HASH_AFTER = "    foo();\n#define X\n    bar();\n"
BACKSPACE_PAST_HASH_ROW = 2
# Enter behind the second #define is indented like x(); above both.
ENTER_PAST_HASH_SEED = "    x();\n#define A\n#define B\n"
ENTER_PAST_HASH_AFTER = "    x();\n#define A\n#define B\n    \n"
ENTER_PAST_HASH_ROW = 2
# The traces of an action the owner refused for the shape of the text.
SHAPE_REFUSALS = ("names rows past", "row wider", "span overrun", "shell recorded")


def _backspace_behind_the_tabs_past_hash(s):
    _down(s, BACKSPACE_PAST_HASH_ROW)
    s.key(HOME, delay=0.1)
    for _ in range(INDENT_TABS_COL):
        s.key(RIGHT, delay=0.1)
    s.key(BACKSPACE, delay=0.6)


def _enter_behind_the_second_define(s):
    _down(s, ENTER_PAST_HASH_ROW)
    s.key(END, delay=0.2)
    s.key(ENTER, delay=0.6)


# name: (seed, the text after the key, the keys)
INDENT_FROM_ABOVE = {
    "tab": (TAB_SEED, TAB_AFTER, _tab_behind_the_a),
    "backspace": (BACKSPACE_PAST_HASH_SEED, BACKSPACE_PAST_HASH_AFTER,
                  _backspace_behind_the_tabs_past_hash),
    "enter": (ENTER_PAST_HASH_SEED, ENTER_PAST_HASH_AFTER, _enter_behind_the_second_define),
}


# @scenario SC-buffer-10
@pytest.mark.parametrize("key", sorted(INDENT_FROM_ABOVE))
def test_an_indent_is_read_from_every_line_above(tmp_path, key):
    """Tab, Backspace in an indent and Enter in a C file take their indent
    from lines above the cursor, past '#' lines and more than one row up:
    the text is the one the key always made, one Ctrl-U gives the seed back,
    and the owner refused nothing for the shape of the text."""
    seed, after, do = INDENT_FROM_ABOVE[key]
    with WpeSession(str(tmp_path), seed,
                    env_extra={"XWPE_UI_TRACE": str(tmp_path / "ui.trace")}) as s:
        do(s)
        assert _saved(s) == after, "%s gave %r" % (key, s.text())
        _assert_undo_and_redo_on_disk(s, seed, after, key)
        trace = _ui_trace(tmp_path)
        assert not [t for t in SHAPE_REFUSALS if t in trace], trace
        assert s.alive(), "wpe died"


# Change All over two rows of a 30-column window; the last occurrence ends
# a row its replacement makes wrap.
SCROLL_WIDTH_RC = ["MaxColumn : 30"]
SCROLL_ROWS = ["row %02d" % i for i in range(60)]
SCROLL_ROWS[10] = "foo at ten"
SCROLL_ROWS[40] = "aaaa bbbb cccc dddd eeee foo"
SCROLL_SEED = "\n".join(SCROLL_ROWS) + "\n"
SCROLL_ROW_ABOVE_LAST_HIT = "row 39"
WINDOW_FIRST_TEXT_ROW = 2                # the menu bar, the window's frame, then its text


def test_change_all_ends_scrolled_to_the_row_of_its_last_occurrence(tmp_path):
    """Change All without a prompt leaves the window scrolled so that the
    row of its last occurrence is the second one shown, as it always did,
    also when that replacement wrapped onto the row below."""
    with _session_with_options(tmp_path, SCROLL_SEED, SCROLL_WIDTH_RC, filename="t.txt") as s:
        s.menu(ALT.SEARCH, "r")
        s.key("foo")
        s.key(ALT_N)
        s.key("REPLACEMENTTEXT")
        s.key(ALT_P)
        s.key(ALT_A, delay=1.2)
        _close_the_replace_box(s)
        top = s.display(drain=0.6)[WINDOW_FIRST_TEXT_ROW]
        assert SCROLL_ROW_ABOVE_LAST_HIT in top, "the window's first row is %r" % top
        assert s.alive(), "wpe died"
