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
import os
import subprocess
import time

import pytest

from wpe_driver import WpeSession, ALT, WPE_BIN
import test_ai_edit_hook_contracts as hook_contracts

CTRL_G = "\x07"          # the debugger's prefix; Ctrl-G B toggles a breakpoint
CTRL_U = "\x15"          # Undo
CTRL_R = "\x12"          # Redo
CTRL_Y = "\x19"          # delete the line
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
