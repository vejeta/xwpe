"""The graphical editor keeps no file open that it only needed at start-up.

Twin of tests/test_child_fds.py: the syntax definition is read by the same
code in every backend, and the graphical editor's descriptors (its display
socket, the Wayland shm file) are its own, so the row is run here too.  After
start-up no descriptor of the editor names the syntax_def it read.

The wayland twin runs this same body.
"""
import os
import sys

import pytest

from conftest import _sleep

# The Wayland twin runs this module under its own conftest, which does not
# put tests/ on the path.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import procinfo  # noqa: E402  (the tests/ directory is added just above)

SETTLE_S = 1.5
SYNTAX_DEF = "syntax_def"

pytestmark = pytest.mark.skipif(
    not procinfo.has_procfs(),
    reason="reads the editor's open descriptors from /proc/<pid>/fd, which "
           "only Linux has")


def syntax_files_held(pid):
    """The descriptors of `pid` that name a syntax_def, as "fd -> path"."""
    held = []
    fd_dir = "/proc/%d/fd" % pid
    for entry in sorted(os.listdir(fd_dir), key=int):
        try:
            path = os.readlink(os.path.join(fd_dir, entry))
        except OSError:
            continue
        if os.path.basename(path) == SYNTAX_DEF:
            held.append("%s -> %s" % (entry, path))
    return held


# @scenario SC-child-fds-05x
def test_x11_after_start_up_no_descriptor_names_the_syntax_def_it_read(xwpe):
    """The graphical editor is up and has read its syntax definition: no
    descriptor of its own still names that file."""
    _sleep(SETTLE_S)
    assert xwpe.proc.poll() is None, "the editor did not start"
    held = syntax_files_held(xwpe.proc.pid)
    assert held == [], \
        "the editor still holds the syntax definition it read: %s" % held
