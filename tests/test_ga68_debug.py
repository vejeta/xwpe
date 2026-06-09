"""GNU Algol 68 (ga68) source-level debugging via gdb, in wpe.

A ga68 program compiles to a native binary, so xwpe debugs it with the EXISTING
gdb backend rather than the a68g monitor: the dialect sniff (we_prog.c) routes a
modern-stropped .a68 to ga68 for the build and to gdb for the debug, breaking at
__algol68_main (ga68's user entry -- "break main" fails on a ga68 binary, the C
main lives in the runtime).  This pins that Ctrl-G R starts gdb on the compiled
binary and F8 single-steps through the SOURCE lines via DWARF.

Requires ga68 and gdb.  Skips otherwise.
"""
import re
import shutil
import time

import pytest

from wpe_driver import WpeSession

# Modern (ga68) stropping: lowercase, { } comment, 'n newline.  Several simple
# statements so single-stepping has distinct source lines to land on.
PROG = (
    "begin\n"
    "   { ga68 step demo }\n"
    "   int a := 21;\n"
    "   int b := a * 2;\n"
    "   int c := b - 5;\n"
    "   puts (\"done!'n\")\n"
    "end\n"
)

CTRL_G = "\x07"
F8 = "\033[19~"     # Step (over)

pytestmark = pytest.mark.skipif(
    shutil.which("ga68") is None or shutil.which("gdb") is None,
    reason="ga68 and gdb required")


def _text(w):
    return "\n".join(w.display())


def _status_line_no(w):
    """The line number shown in the status bar (display row 20), or -1."""
    m = re.search(r"(\d+):", w.display()[20])
    return int(m.group(1)) if m else -1


def test_ga68_gdb_run_and_step(tmp_path):
    """Ctrl-G R starts gdb on the ga68 binary; F8 advances the source line."""
    with WpeSession(str(tmp_path), PROG, filename="step.a68", wait=2.0) as w:
        w.key(CTRL_G, "r", delay=3.0)        # compile with ga68 + start gdb
        time.sleep(2.0)
        w._drain(1.0)
        assert w.alive(), "wpe died starting gdb on the ga68 binary"
        start = _status_line_no(w)
        assert start > 0, \
            "debugger should stop on a source line (status:\n%s)" % _text(w)

        w.key(F8, delay=2.5)                 # single-step
        w._drain(1.0)
        assert w.alive(), "wpe died stepping the ga68 program under gdb"
        stepped = _status_line_no(w)
        assert stepped > start, \
            "F8 should advance to a later source line (%d -> %d):\n%s" \
            % (start, stepped, _text(w))
