"""What the user types reaches a program the Run menu started, on X11.

Ctrl-F9 runs the program with its output in Messages; a line typed in the
editor's window is the program's input, and Ctrl-C stops it as on a
terminal.  The program here reads one line and then waits for SIGINT; it
writes what it received into files, so the row asserts on disk.  ASCII only:
the Wayland harness cannot type characters above Latin-1.

The Wayland twin (tests/wayland/test_run_typing.py) runs the same body.
"""
import os
import shutil
import time

import pytest

from conftest import _sleep

READER_PROG = (
    "#include <signal.h>\n"
    "#include <stdio.h>\n"
    "#include <unistd.h>\n"
    "static volatile sig_atomic_t interrupted;\n"
    "static void on_int(int sig) { (void)sig; interrupted = 1; }\n"
    "int main(void)\n"
    "{\n"
    "  char line[128];\n"
    "  FILE *o;\n"
    "  signal(SIGINT, on_int);\n"
    "  alarm(60);\n"
    "  setvbuf(stdout, NULL, _IONBF, 0);\n"
    "  o = fopen(\"ready.txt\", \"w\");\n"
    "  fclose(o);\n"
    "  printf(\"READY\\n\");\n"
    "  if (!fgets(line, sizeof line, stdin)) return 2;\n"
    "  o = fopen(\"line.txt\", \"w\");\n"
    "  fputs(line, o);\n"
    "  fclose(o);\n"
    "  while (!interrupted) pause();\n"
    "  o = fopen(\"sigint.txt\", \"w\");\n"
    "  fclose(o);\n"
    "  return 0;\n"
    "}\n"
)
TYPED = "hello run"
BUILD_RUN_S = 30
ANSWER_S = 8


def _wait(predicate, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        _sleep(0.3)
    return predicate()


def _in_dir(xwpe, name):
    return os.path.join(os.path.dirname(xwpe.srcfile), name)


def _read(path):
    try:
        with open(path) as fh:
            return fh.read()
    except OSError:
        return ""


# @scenario SC-screen-07
@pytest.mark.skipif(not shutil.which("gcc"), reason="no gcc")
@pytest.mark.parametrize("xwpe", [{"XWPE_TEST_SOURCE": READER_PROG}],
                         indirect=True)
def test_a_line_and_ctrl_c_typed_for_a_running_program_reach_it(xwpe):
    """Ctrl-F9 builds and runs the program; the line typed in the window
    is what the program reads, and Ctrl-C then reaches it as SIGINT."""
    xwpe.key("ctrl+F9")
    assert _wait(lambda: os.path.exists(_in_dir(xwpe, "ready.txt")),
                 BUILD_RUN_S), "Ctrl-F9 should build and start the program"
    _sleep(0.5)
    xwpe.type(TYPED, delay=0.3)
    xwpe.key("Return", delay=0.5)
    assert _wait(lambda: os.path.exists(_in_dir(xwpe, "line.txt")), ANSWER_S), \
        "the program should have read the line typed for it"
    assert _read(_in_dir(xwpe, "line.txt")).rstrip("\r\n") == TYPED, \
        "the program read %r, not the line typed" \
        % _read(_in_dir(xwpe, "line.txt"))
    xwpe.key("ctrl+c", delay=0.5)
    assert _wait(lambda: os.path.exists(_in_dir(xwpe, "sigint.txt")), ANSWER_S), \
        "Ctrl-C typed in the window should reach the program as SIGINT"
    assert xwpe.proc.poll() is None, "xwpe died running the program"


# A program that reads its terminal byte by byte (no line editing) and writes
# the bytes it got, as numbers, once it reads a 'q'.
RAW_READER_PROG = (
    "#include <stdio.h>\n"
    "#include <termios.h>\n"
    "#include <unistd.h>\n"
    "int main(void)\n"
    "{\n"
    "  struct termios t;\n"
    "  unsigned char b[128];\n"
    "  int n = 0, i;\n"
    "  FILE *o;\n"
    "  alarm(60);\n"
    "  tcgetattr(0, &t);\n"
    "  t.c_lflag &= ~(ICANON | ECHO);\n"
    "  tcsetattr(0, TCSANOW, &t);\n"
    "  o = fopen(\"ready.txt\", \"w\");\n"
    "  fclose(o);\n"
    "  while (n < (int)sizeof b && read(0, b + n, 1) == 1 && b[n++] != 'q')\n"
    "    ;\n"
    "  o = fopen(\"bytes.txt\", \"w\");\n"
    "  for (i = 0; i < n; i++)\n"
    "    fprintf(o, \"%d \", b[i]);\n"
    "  fclose(o);\n"
    "  return 0;\n"
    "}\n"
)
# Up, F5, Delete and q, as the xterm sequences a program's terminal reads.
RAW_KEYS_WANTED = "27 91 65 27 91 49 53 126 27 91 51 126 113"


# @scenario SC-keymap-17
@pytest.mark.skipif(not shutil.which("gcc"), reason="no gcc")
@pytest.mark.parametrize("xwpe", [{"XWPE_TEST_SOURCE": RAW_READER_PROG}],
                         indirect=True)
def test_the_cursor_and_function_keys_reach_a_running_program(xwpe):
    """The keys typed for a program are decoded once, as the editor reads
    them, and sent as its terminal expects: Up, F5 and Delete arrive as
    their xterm sequences.  The window's key read used to drop them."""
    xwpe.key("ctrl+F9")
    assert _wait(lambda: os.path.exists(_in_dir(xwpe, "ready.txt")),
                 BUILD_RUN_S), "Ctrl-F9 should build and start the program"
    _sleep(0.5)
    xwpe.key("Up", "F5", "Delete", "q", delay=0.4)
    assert _wait(lambda: os.path.exists(_in_dir(xwpe, "bytes.txt")), ANSWER_S), \
        "the program never read its q"
    got = _read(_in_dir(xwpe, "bytes.txt")).strip()
    assert got == RAW_KEYS_WANTED, \
        "the program read %r, not Up, F5, Delete and q (%r)" \
        % (got, RAW_KEYS_WANTED)
