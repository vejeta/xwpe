"""A command prefix typed as the debug session ends never lands in the file.

The user presses Ctrl-G R (Continue) and, at once, the prefix of another
command: Ctrl-G I (Pause), Ctrl-G K (Stack), Alt-Q (language server) or
Alt-G (assistant).  The program ends while the keys are in flight.  The
editor's own waits for the debugger must leave the prefix byte for the
editor, so the pair is handled as a command with no session; if the waiting
loop took the first byte, the letter would be typed into the source.
Every row checks the file on disk, not only the screen.
"""
import os
import shutil
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from wpe_driver import WpeSession, WAIT_SCALE
import time

CTRL_G = "\x07"
ALT_Q = "\033q"
ALT_G = "\033g"
DOWN = "\033OB"
POLL_S = 0.2
END_TIMEOUT_S = 25
UNUSED_LETTER = "z"
PRINT_LINE = 4

C_PROG = (
    "#include <stdio.h>\n"
    "int main(void)\n"
    "{\n"
    "  printf(\"READY\\n\");\n"
    "  return 0;\n"
    "}\n"
)
PY_PROG = (
    "x = 3\n"
    "y = x + 1\n"
    "z = y + 1\n"
    "print('READY', z)\n"
)
GO_PROG = (
    "package main\n"
    "\n"
    "import \"fmt\"\n"
    "\n"
    "func main() {\n"
    "    fmt.Println(\"READY\")\n"
    "}\n"
)
GO_PRINT_LINE = 6
ASSISTANT_ENV = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
                 "XWPE_AI_MOCK_REPLY": "ok"}
READ_C_PROG = (
    "#include <stdio.h>\n"
    "int main(void)\n"
    "{\n"
    "  char line[100];\n"
    "  printf(\"READY\\n\");\n"
    "  if (fgets(line, sizeof line, stdin))\n"
    "    printf(\"GOT:%s\", line);\n"
    "  return 0;\n"
    "}\n"
)
READ_STOP_LINE = 5
TYPED_FOR_PROGRAM = "hi"
EDITOR_SEQUENCES = {
    "csi_up": "\033[A",
    "ss3_up": "\033OA",
    "sgr_click": "\033[<0;3;3M\033[<0;3;3m",
}


def _need(*tools):
    for tool in tools:
        if shutil.which(tool) is None:
            pytest.skip("no %s" % tool)


def _screen(w):
    return "\n".join(w.display(drain=0))


def _wait_for(w, predicate, timeout):
    deadline = time.time() + timeout * WAIT_SCALE
    while time.time() < deadline:
        w._drain(POLL_S)
        if predicate():
            return True
    return predicate()


def _stop_on(w, line):
    """Breakpoint on @line, Ctrl-G R, wait for the mark to reach it."""
    for _ in range(line - 1):
        w.key(DOWN, delay=0.1)
    w.key(CTRL_G, "b", delay=0.4)
    w.key(CTRL_G, "r", delay=1.0)
    assert _wait_for(w, lambda: w.mark_line() == line, END_TIMEOUT_S), \
        _screen(w)


def _continue_then(w, *prefix):
    """Ctrl-G R and, with no pause, the prefix keys of another command."""
    w.key(CTRL_G, "r", delay=0.0)
    w.key(*prefix, delay=0.5)


def _assert_untouched(w, seed, what):
    """The session has ended and the file on disk is the seed."""
    assert _wait_for(w, lambda: w.mark_line() is None, END_TIMEOUT_S), \
        _screen(w)
    w.save()
    assert w.text() == seed, \
        "%s was typed into the file:\n%s" % (what, _screen(w))


@pytest.mark.parametrize("letter", ["i", "k"])
def test_gdb_ctrl_g_letter_after_continue_is_a_command(tmp_path, letter):
    """Ctrl-G R then Ctrl-G I / Ctrl-G K at once: no session is left, the
    pair is refused and the letter is not text."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), C_PROG, filename="p.c", wait=1.5) as w:
        _stop_on(w, PRINT_LINE)
        _continue_then(w, CTRL_G, letter)
        _assert_untouched(w, C_PROG, "Ctrl-G " + letter)


@pytest.mark.parametrize("letter", ["i", "k"])
def test_pdb_ctrl_g_letter_after_continue_is_a_command(tmp_path, letter):
    """pdb is read through the editor's own descriptor loop, which never
    reads the keyboard: this row keeps it that way."""
    _need("python3")
    with WpeSession(str(tmp_path), PY_PROG, filename="p.py", wait=1.5) as w:
        _stop_on(w, PRINT_LINE)
        _continue_then(w, CTRL_G, letter)
        _assert_untouched(w, PY_PROG, "Ctrl-G " + letter)


@pytest.mark.parametrize("letter", ["i", "k"])
def test_dlv_ctrl_g_letter_after_continue_is_a_command(tmp_path, letter):
    """A debug adapter is served by the same loop: same guard."""
    _need("dlv", "go")
    (tmp_path / "go.mod").write_text("module dapdemo\n\ngo 1.21\n")
    with WpeSession(str(tmp_path), GO_PROG, filename="main.go",
                    wait=2.0) as w:
        _stop_on(w, GO_PRINT_LINE)
        _continue_then(w, CTRL_G, letter)
        _assert_untouched(w, GO_PROG, "Ctrl-G " + letter)


def test_gdb_alt_q_after_continue_is_a_prefix(tmp_path):
    """Alt-Q right after Ctrl-G R: its Escape byte is not taken by the wait."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), C_PROG, filename="p.c", wait=1.5) as w:
        _stop_on(w, PRINT_LINE)
        _continue_then(w, ALT_Q, UNUSED_LETTER)
        _assert_untouched(w, C_PROG, "the key after Alt-Q")


def test_gdb_alt_g_after_continue_is_a_prefix(tmp_path):
    """Alt-G right after Ctrl-G R: the assistant prefix arrives whole."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), C_PROG, filename="p.c", wait=1.5,
                    env_extra=ASSISTANT_ENV) as w:
        _stop_on(w, PRINT_LINE)
        _continue_then(w, ALT_G, UNUSED_LETTER)
        _assert_untouched(w, C_PROG, "the key after Alt-G")


@pytest.mark.parametrize("name", sorted(EDITOR_SEQUENCES))
def test_gdb_key_sequence_after_continue_arrives_whole(tmp_path, name):
    """An arrow key or a mouse click typed with Ctrl-G R reaches the editor
    whole, and the line typed after it still reaches the program waiting
    for input, in order.  If the wait took the Escape, the rest of the
    sequence was typed into the program's input; if it went on reading
    after leaving the Escape, the line overtook it the same way."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), READ_C_PROG, filename="p.c",
                    wait=1.5) as w:
        _stop_on(w, READ_STOP_LINE)
        w.key(CTRL_G, "r", delay=0.0)
        w.key(EDITOR_SEQUENCES[name] + TYPED_FOR_PROGRAM + "\r", delay=0.3)
        assert _wait_for(w, lambda: "GOT:" in _screen(w), END_TIMEOUT_S), \
            "the program never read its line:\n%s" % _screen(w)
        assert "GOT:" + TYPED_FOR_PROGRAM in _screen(w), \
            "the %s reached the program as text:\n%s" % (name, _screen(w))
