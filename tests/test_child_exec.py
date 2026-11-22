"""A program the editor starts is started without waiting on another thread.

Every program the editor starts (a build, the Run program, a debugger, a
language server) goes through one fork and one exec, and the child does
nothing between the two that can wait for a lock another thread held when
the fork happened.  The graphical editor runs a font thread of its own
(Pango's), so there that is a present requirement and not a habit: a child
that waits on such a lock never runs, and Messages stays empty.

The calls the child may make are fixed by tests/check-child-exec.sh and the
mechanism by tests/test_childsafe.c.  These rows are what a user of each
backend sees: the build's command line in Messages, a missing compiler
named while the editor goes on taking keys, the owner's account of why a
program did not start (the trace line the editor writes when
XWPE_UI_TRACE is set), and the font thread that makes the requirement real.

The graphical twins are tests/x11/test_child_exec.py and its Wayland twin.
"""
import glob
import os

import pytest

import procinfo
from host_tools import needs
from wpe_driver import COMPILER_LINE_TAIL, WpeSession, cols_for, wait_screen

F9 = "\033[20~"            # Make: compile and link
F6 = "\033[17~"            # next window
BUILD_WAIT = 4.0
FIND_TIMEOUT = 20
WIDE = 200                 # the least width; cols_for widens it for a long folder
TRACE = "ui.trace"
NO_SUCH_COMPILER = "nosuch_cc_for_test"
REPORT_LINE = "child report the build exec 2"      # exec failed with ENOENT
# The kernel cuts a thread's name to 15 characters.
COMM_MAX = 15
FONT_THREAD = "[pango] fontconfig"[:COMM_MAX]
SEED_C = "int main(void){return 0;}\n"
# A source gcc complains about, and a window tall enough to keep the command
# line, the diagnostics and the summary in Messages together.
BAD_C = "int main(void){ undeclared_name = 1; return 0; }\n"
DIAGNOSTIC = "undeclared"
SUMMARY = "0 warnings"      # the editor's own last line: "1 error, 0 warnings"
TALL = 70
MESSAGES_TITLE =" Messages "
BORDER_ROW = "m"           # a window's bottom border starts with this glyph


def session(workdir, src=SEED_C, rows=None):
    """A wide editor session on t.c that writes its trace to ui.trace in
    the session's folder, with no saved assistant state.  `rows` makes it
    taller than the default."""
    env = {"XWPE_UI_TRACE": os.path.join(str(workdir), TRACE),
           "XDG_STATE_HOME": os.path.join(str(workdir), "state")}
    width = cols_for(os.path.join(str(workdir), "t.c"), tail=COMPILER_LINE_TAIL,
                     at_least=WIDE)
    if rows is None:
        return WpeSession(str(workdir), src, filename="t.c", cols=width,
                          env_extra=env)
    return WpeSession(str(workdir), src, filename="t.c", cols=width,
                      rows=rows, env_extra=env)


def trace_text(workdir):
    """What the editor traced so far, "" before it traced anything."""
    try:
        with open(os.path.join(str(workdir), TRACE)) as fh:
            return fh.read()
    except OSError:
        return ""


def messages_rows(w):
    """The text rows of the Messages window as painted: the rows below its
    title and above its bottom border, side borders and padding removed."""
    rows = w.display()
    start = next(i for i, r in enumerate(rows) if MESSAGES_TITLE in r) + 1
    out = []
    for r in rows[start:]:
        if r.startswith(BORDER_ROW):
            break
        out.append(r[1:].rstrip().rstrip("x").rstrip())
    return out


def thread_names(pid):
    """The names of the threads of process `pid`, from /proc."""
    names = []
    for comm in glob.glob("/proc/%d/task/*/comm" % pid):
        try:
            with open(comm) as fh:
                names.append(fh.read().strip())
        except OSError:
            continue
    return names


# @scenario SC-child-exec-07
@needs("gcc")
def test_f9_puts_the_command_line_first_in_messages(tmp_path):
    """F9 on a C file the compiler complains about: the command the
    compiler was started with is in Messages, ahead of the compiler's own
    diagnostic, and that is ahead of the editor's summary.  The child
    writes the command line before the program runs; a line written after
    the program's output would leave the user unable to tell which command
    said what.  The window is tall enough to keep every row on screen."""
    with session(tmp_path, BAD_C, rows=TALL) as w:
        w.key(F9, delay=BUILD_WAIT)
        assert wait_screen(w, SUMMARY, tmo=FIND_TIMEOUT), \
            "\n".join(w.display())
        rows = messages_rows(w)
    commands = [i for i, r in enumerate(rows) if r.startswith("gcc ")]
    diagnostics = [i for i, r in enumerate(rows) if DIAGNOSTIC in r]
    summary = [i for i, r in enumerate(rows) if SUMMARY in r]
    assert commands and diagnostics and summary, rows
    assert commands[0] < diagnostics[0] < summary[-1], \
        "Messages is not command line, diagnostic, summary: %r" % rows


# @scenario SC-child-exec-08
def test_a_missing_compiler_is_named_and_the_editor_takes_keys(tmp_path):
    """The C compiler names a program that does not exist: F9 names it in
    the error row, the editor goes on taking keys, and the owner traces why
    nothing ran (the exec failed with "no such file", error 2)."""
    cfg = tmp_path / ".config" / "xwpe"
    cfg.mkdir(parents=True)
    (cfg / "xwperc").write_text(
        "[Language C]\nCompiler : %s\n" % NO_SUCH_COMPILER)
    with session(tmp_path) as w:
        w.key(F9, delay=BUILD_WAIT)
        assert wait_screen(w, "Error at Command", tmo=FIND_TIMEOUT), \
            "\n".join(w.display())
        row = next(r for r in w.display() if "Error at Command" in r)
        assert NO_SUCH_COMPILER in row, row
        w.key(F6, delay=0.5)
        w.key("Z", delay=0.5)
        w.save()
        assert w.alive()
    assert (tmp_path / "t.c").read_text().startswith("Z"), \
        "the editor stopped taking keys after the failed start"
    assert REPORT_LINE in trace_text(tmp_path).splitlines(), \
        "the owner did not trace the failed start: %r" % trace_text(tmp_path)


# @scenario SC-child-exec-09
@pytest.mark.skipif(not procinfo.has_procfs(),
                    reason="reads the editor's threads from /proc, which "
                           "only Linux has")
def test_the_console_editor_has_no_font_thread(tmp_path):
    """The terminal editor is one thread: it has no Pango font thread,
    which is why the graphical twins are the rows that run the child's path
    next to one."""
    with session(tmp_path) as w:
        assert w.proc.poll() is None, "the editor did not start"
        names = thread_names(w.proc.pid)
    assert names, "no threads read from /proc"
    assert FONT_THREAD not in names, names
    assert len(names) == 1, names
