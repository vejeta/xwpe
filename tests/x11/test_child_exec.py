"""A program the graphical editor starts is started next to Pango's thread.

Twin of tests/test_child_exec.py.  The graphical editor runs a font thread
of its own, so a child of a build step is forked while another thread may
hold a lock; the child must do nothing between the fork and the exec that
waits on one (tests/check-child-exec.sh fixes the calls, tests/test_childsafe.c
holds the mechanism).  These rows run the whole start, the failed start
included, in the editor that has that thread.

The editor can write the text of its windows to a test (XWPE_TEST_DUMP_FD),
but the graphical launcher does not pass that descriptor on, so what the
user reads in Messages (the command line, the compiler's name in the error
row) is asserted in the terminal twin.  Here the rows read what the editor
did: the owner's trace (XWPE_UI_TRACE) says each program ran or why it did
not, the build linked, and the editor takes keys after a failed start.  The
Wayland twin runs these same bodies.
"""
import glob
import os
import sys
import time

import pytest

from conftest import _sleep

# The Wayland twin runs this module under its own conftest, which does not
# put tests/ on the path.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import procinfo  # noqa: E402  (the tests/ directory is added just above)
from host_tools import needs  # noqa: E402

TRACE = "ui.trace"
SESSION = {"XWPE_UI_TRACE": TRACE, "XDG_STATE_HOME": ""}
BUILD_WAIT_S = 30          # the build has run (or failed to start) by then
SETTLE_S = 1.5
RAN_LINE = "child ran the build"
REPORT_LINE = "child report the build exec 2"      # exec failed with ENOENT
# The kernel cuts a thread's name to 15 characters.
COMM_MAX = 15
FONT_THREAD = "[pango] fontconfig"[:COMM_MAX]
# A PATH that names no folder: the compiler is not found, the editor is not
# otherwise affected.
EMPTY_PATH = "/nonexistent-path-for-test"

needs_procfs = pytest.mark.skipif(
    not procinfo.has_procfs(),
    reason="reads the editor's threads from /proc, which only Linux has")
needs_gcc = needs("gcc")


def _wait(predicate, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        _sleep(0.2)
    return predicate()


def _trace_lines(tmp_path):
    try:
        return (tmp_path / TRACE).read_text().splitlines()
    except OSError:
        return []


def _thread_names(pid):
    """The names of the threads of process `pid`, from /proc."""
    names = []
    for comm in glob.glob("/proc/%d/task/*/comm" % pid):
        try:
            with open(comm) as fh:
                names.append(fh.read().strip())
        except OSError:
            continue
    return names


# @scenario SC-child-exec-07x
@needs_gcc
@pytest.mark.parametrize("xwpe", [dict(SESSION)], indirect=True)
def test_x11_f9_starts_the_compiler_and_the_build_links(xwpe, tmp_path):
    """F9 on a C file: the compiler is started and the program is built,
    with the font thread running.  The command line in Messages is the
    terminal twin's row; this one holds the same start to the same result
    in the graphical editor: the owner traces that the build ran, and the
    program linked."""
    xwpe.key("F9", delay=0.5)
    linked = _wait(lambda: (tmp_path / "t.e").exists(), BUILD_WAIT_S)
    _sleep(SETTLE_S)
    assert xwpe.proc.poll() is None, "xwpe died building"
    assert linked, "the build did not link"
    assert RAN_LINE in _trace_lines(tmp_path), \
        "the owner did not trace the start: %r" % _trace_lines(tmp_path)


# @scenario SC-child-exec-08x
@pytest.mark.parametrize("xwpe", [dict(SESSION, PATH=EMPTY_PATH)],
                         indirect=True)
def test_x11_a_missing_compiler_is_traced_and_the_editor_takes_keys(xwpe,
                                                                    tmp_path):
    """F9 with no compiler to be found: the owner traces why nothing ran (the
    exec failed with "no such file", error 2), and the editor goes on taking
    keys.  This is the row that runs the failed-start path next to the font
    thread."""
    xwpe.key("F9", delay=0.5)
    told = _wait(lambda: REPORT_LINE in _trace_lines(tmp_path), BUILD_WAIT_S)
    assert xwpe.proc.poll() is None, "xwpe died on the failed start"
    assert told, "the owner did not trace the failed start: %r" % \
        _trace_lines(tmp_path)
    xwpe.key("F6", delay=0.5)
    xwpe.key("Z", delay=0.5)
    xwpe.save()
    assert xwpe.saved_text().startswith("Z"), \
        "the editor stopped taking keys after the failed start"


# @scenario SC-child-exec-09x
@needs_procfs
def test_x11_the_graphical_editor_has_the_font_thread(xwpe):
    """The graphical editor has Pango's font thread when it is up: more
    than one thread, one of them named for fontconfig.  This is the fact
    that makes the child's restriction a present requirement; if Pango ever
    stops threading, this row says the hazard is gone."""
    _sleep(SETTLE_S)
    assert xwpe.proc.poll() is None, "the editor did not start"
    names = _thread_names(xwpe.proc.pid)
    assert len(names) >= 2, names
    assert FONT_THREAD in names, names
