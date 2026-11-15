"""The editor keeps no file open that it only needed at start-up.

The syntax definition (syntax_def, the user's own or the installed copy) is
read once, when the editor starts.  The file used to stay open for as long as
the editor ran, and so reached every program the editor started: a build, a
debugger, a language server.  Row 05 reads the editor's open descriptors from
/proc once it is up and asks that none of them names the syntax_def it read.

It is a lifetime row, not a rule that every descriptor is close-on-exec: a
descriptor the editor opens later (the assistant's connection, the debug
adapter's socket) is not close-on-exec, and does not need to be, because the
owner of child starts (we_child.c) closes whatever a program inherits and
tests/test_childfds.c is the row for that.

The graphical twins are tests/x11/test_child_fds.py and its Wayland twin.
"""
import os

import pytest

import procinfo
from wpe_driver import WpeSession

pytestmark = pytest.mark.skipif(
    not procinfo.has_procfs(),
    reason="reads the editor's open descriptors from /proc/<pid>/fd, which "
           "only Linux has")

SEED_C = "int main(void){return 0;}\n"
SYNTAX_DEF = "syntax_def"


def open_file_names(pid):
    """The path each descriptor of process `pid` names, by number.  A
    descriptor that closes while the listing is read is left out."""
    names = {}
    fd_dir = "/proc/%d/fd" % pid
    for entry in os.listdir(fd_dir):
        try:
            names[int(entry)] = os.readlink(os.path.join(fd_dir, entry))
        except OSError:
            continue
    return names


def syntax_files_held(pid):
    """The descriptors of `pid` that name a syntax_def, as "fd -> path"."""
    return ["%d -> %s" % (fd, path)
            for fd, path in sorted(open_file_names(pid).items())
            if os.path.basename(path) == SYNTAX_DEF]


# @scenario SC-child-fds-05
def test_after_start_up_no_descriptor_names_the_syntax_def_it_read(tmp_path):
    """The editor has started and read its syntax definition: no descriptor
    of its own still names that file."""
    with WpeSession(str(tmp_path), SEED_C, filename="t.c") as w:
        assert w.proc.poll() is None, "the editor did not start"
        held = syntax_files_held(w.proc.pid)
        assert held == [], \
            "the editor still holds the syntax definition it read: %s" % held
