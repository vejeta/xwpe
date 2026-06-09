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


def test_ga68_gdb_debug_from_subdirectory(tmp_path):
    """Debug a modern .a68 that lives in a SUBDIRECTORY (cwd != file's dir).

    Regression for the "No symbol table is loaded" bug: e_start_debug re-detected
    the dialect from the bare window name, so for a file outside the cwd the sniff
    failed, e_s_prog fell back to a68g (comp_sw 1) while the debugger was gdb, and
    the gdb branch then dropped the ".e" suffix -- gdb was launched on a missing
    binary.  Here the ga68 binary must load (no such error) and the debugger must
    stop on a source line that F8 then advances."""
    (tmp_path / "src").mkdir()
    with WpeSession(str(tmp_path), PROG, filename="src/step.a68", wait=2.0) as w:
        w.key(CTRL_G, "r", delay=3.0)
        time.sleep(2.0)
        w._drain(1.0)
        assert w.alive(), "wpe died starting gdb on the subdir ga68 binary"
        text = _text(w)
        assert "No symbol table" not in text, \
            "gdb was launched on a missing binary:\n%s" % text
        start = _status_line_no(w)
        assert start > 0, \
            "debugger should stop on a source line (status:\n%s)" % _text(w)

        w.key(F8, delay=2.5)
        w._drain(1.0)
        assert w.alive(), "wpe died stepping the subdir ga68 program"
        stepped = _status_line_no(w)
        assert stepped > start, \
            "F8 should advance to a later source line (%d -> %d):\n%s" \
            % (start, stepped, _text(w))


# Iterative factorial: `fact` grows 1 -> 1 -> 2 -> 6 -> 24 -> 120 as the loop
# runs, so a watch on it must show several distinct values while stepping.
FACT = (
    "begin\n"
    "   int n := 5;\n"
    "   int fact := 1;\n"
    "   for i from 1 to n do\n"
    "      fact := fact * i\n"
    "   od;\n"
    "   puts (\"done'n\")\n"
    "end\n"
)


def _watch_val(w, name):
    """Current value shown for the named watch in the Watches window, or None."""
    pat = re.compile(re.escape(name) + r":\s*(-?\d+)")
    for line in w.display():
        m = pat.search(line)
        if m:
            return int(m.group(1))
    return None


def test_ga68_gdb_watch_updates_on_step(tmp_path):
    """A watch must RE-EVALUATE on every Step, not stay frozen at its add-time
    value.  Regression: a plain gdb Step is resolved by e_d_pr_sig (via
    e_d_trd_check), which positioned the source line but never refreshed the
    Watches window -- so a ga68 watch on `fact` read 0 forever no matter how
    far you stepped."""
    with WpeSession(str(tmp_path), FACT, filename="fact.a68", wait=2.0) as w:
        w.key(CTRL_G, "r", delay=3.0)
        time.sleep(1.5)
        w._drain(1.0)
        w.key(CTRL_G, "w", delay=1.0)         # Make Watch dialog
        for ch in "fact":
            w.key(ch, delay=0.05)
        w.key("\r", delay=1.0)                # confirm
        w._drain(1.0)
        seen = set()
        for _ in range(14):                   # step through the whole loop
            w.key(F8, delay=1.2)
            w._drain(0.6)
            v = _watch_val(w, "fact")
            if v is not None:
                seen.add(v)
        assert w.alive(), "wpe died stepping with a watch set"
        assert any(v > 1 for v in seen), \
            "watch on `fact` never updated past its initial value (saw %s) -- " \
            "the Watches window is not refreshing on Step" % sorted(seen)
