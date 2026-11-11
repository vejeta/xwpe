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
