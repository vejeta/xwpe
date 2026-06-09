"""Scala source-level debugging via Bloop/scala-debug-adapter over DAP, in wpe.

A .scala file is debugged through the DAP bridge, but the JVM has no standalone
DAP server: xwpe runs the BSP bootstrap (we_bsp.c) against scala-cli (which
bundles Bloop, hosting scala-debug-adapter), gets back a tcp:// DAP endpoint,
and connects the existing reverse-TCP DAP engine to it.  Same editor keys as the
Go/Rust slices (Ctrl-G B/R/W, F8); the build server compiles, so there is no
xwpe compile step.

Requires `scala-cli`.  Skips otherwise.  Slow: the first Ctrl-G R starts a JVM
build server and (on a cold coursier cache) resolves the Scala toolchain.
"""
import re
import shutil
import time

import pytest

from wpe_driver import WpeSession

# Iterative factorial; `f` grows 1 -> 2 -> 6 -> 24 -> ...  Breakpoint on line 6
# (the multiply), reached every loop iteration.
PROG = (
    "object Factorial:\n"
    "  def main(args: Array[String]): Unit =\n"
    "    var f = 1L\n"
    "    var i = 1\n"
    "    while i <= 10 do\n"
    "      f = f * i\n"                 # line 6: breakpoint
    "      i = i + 1\n"
    "    println(s\"factorial(10) = $f\")\n"
)

CTRL_G = "\x07"
CTRL_N = "\x0e"     # Emacs "down a line" -- pyte delivers it reliably

pytestmark = pytest.mark.skipif(
    shutil.which("scala-cli") is None,
    reason="scala-cli required (bundles Bloop / scala-debug-adapter)")


def _text(w):
    return "\n".join(w.display())


def _status_line_no(w):
    m = re.search(r"(\d+):", w.display()[20])
    return int(m.group(1)) if m else -1


def _watch_val(w, name):
    pat = re.compile(re.escape(name) + r":\s*(-?\d+)")
    for line in w.display():
        m = pat.search(line)
        if m:
            return int(m.group(1))
    return None


def test_scala_dap_run_step_watch(tmp_path):
    """scala-cli/Bloop starts via BSP, scala-debug-adapter stops at the
    breakpoint over DAP, and a watch on `f` updates as the loop runs."""
    with WpeSession(str(tmp_path), PROG, filename="factorial.scala", wait=2.0) as w:
        for _ in range(5):                 # move the cursor to line 6
            w.key(CTRL_N, delay=0.12)
        w.key(CTRL_G, "b", delay=1.0)      # breakpoint on line 6
        # First Run boots a JVM build server (+ cold-cache coursier resolve):
        # give it plenty of time; wpe's UI blocks until the BSP bootstrap ends.
        w.key(CTRL_G, "r", delay=60.0)
        time.sleep(3.0)
        w._drain(3.0)
        assert w.alive(), "wpe died starting the Scala BSP/DAP session"
        assert _status_line_no(w) == 6, \
            "DAP should stop at the breakpoint on line 6 (status:\n%s)" % _text(w)

        w.key(CTRL_G, "w", delay=1.5)      # Make Watch dialog
        w.key("f", delay=0.1)
        w.key("\r", delay=2.5)             # JVM evaluate is slower than gdb's
        w._drain(1.5)

        # continue around the loop; f must appear and grow past 1 (factorial
        # 1,1,2,6,24,...).  The watch line refreshes on each stop, so poll it
        # across the iterations rather than insisting on one early value.
        seen = set()
        v = _watch_val(w, "f")
        if v is not None:
            seen.add(v)
        for _ in range(7):
            w.key(CTRL_G, "r", delay=2.5)
            w._drain(1.0)
            v = _watch_val(w, "f")
            if v is not None:
                seen.add(v)
        assert w.alive(), "wpe died continuing the Scala program"
        assert seen, "the watch on f never showed a numeric value:\n%s" % _text(w)
        assert any(v > 1 for v in seen), \
            "watch on `f` never grew past 1 (saw %s)" % sorted(seen)
