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
