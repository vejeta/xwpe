"""Rust source-level debugging via gdb over DAP (stdio transport), in wpe.

A .rs file is compiled by xwpe (rustc -g) and then debugged through the DAP
bridge driving `gdb --interpreter=dap` -- gdb is a first-class Rust debugger
(rust-gdb IS gdb).  This is the stdio-transport counterpart to the Go/Delve
(reverse-TCP) slice: same editor keys (Ctrl-G B/R/W, F8), program output and
adapter chatter handled the same way.

Requires `rustc` and `gdb`.  Skips otherwise.
"""
import re
import time

import pytest

from wpe_driver import WpeSession, tool_usable

# Iterative factorial; `fact` grows 1 -> 2 -> 6 -> 24 -> 120.  The breakpoint
# goes on line 6 (the multiply), reached every loop iteration.
PROG = (
    "fn main() {\n"
    "    let n = 5;\n"
    "    let mut fact = 1;\n"
    "    let mut i = 1;\n"
    "    while i <= n {\n"
    "        fact = fact * i;\n"        # line 6: breakpoint
    "        i = i + 1;\n"
    "    }\n"
    "    println!(\"result: {}\", fact);\n"
    "}\n"
)

CTRL_G = "\x07"
CTRL_N = "\x0e"     # Emacs "down a line" -- pyte delivers it reliably (unlike \033[B)

pytestmark = pytest.mark.skipif(
    not tool_usable("rustc") or not tool_usable("gdb"),
    reason="a usable rustc toolchain and gdb are required "
           "(rustc/gdb must run, not just be on PATH)")


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


def test_rust_dap_run_step_watch(tmp_path):
    """rustc compiles, gdb/DAP stops at the breakpoint, and a watch updates as
    the loop runs.  Also guards that gdb's pending-breakpoint chatter ("No source
    file ... pending", category stdout) does NOT leak into Messages."""
    with WpeSession(str(tmp_path), PROG, filename="main.rs", wait=2.0) as w:
        for _ in range(5):                 # move the cursor to line 6
            w.key(CTRL_N, delay=0.12)
        w.key(CTRL_G, "b", delay=1.0)      # breakpoint on line 6
        w.key(CTRL_G, "r", delay=10.0)     # compile (rustc) + launch gdb -> stop
        time.sleep(2.0)
        w._drain(2.0)
        assert w.alive(), "wpe died compiling/starting the Rust DAP session"
        text = _text(w)
        assert "No source file" not in text and "pending" not in text, \
            "gdb pending-breakpoint chatter leaked into Messages:\n%s" % text
        assert _status_line_no(w) == 6, \
            "DAP should stop at the breakpoint on line 6 (status:\n%s)" % text

        w.key(CTRL_G, "w", delay=1.0)      # Make Watch dialog
        for ch in "fact":
            w.key(ch, delay=0.05)
        w.key("\r", delay=1.0)
        w._drain(1.0)
        assert _watch_val(w, "fact") is not None, \
            "the watch on fact shows no value:\n%s" % _text(w)

        # continue around the loop; fact must grow past 1.
        seen = set()
        for _ in range(6):
            w.key(CTRL_G, "r", delay=1.5)
            w._drain(0.8)
            v = _watch_val(w, "fact")
            if v is not None:
                seen.add(v)
        assert w.alive(), "wpe died continuing the Rust program"
        assert any(v > 1 for v in seen), \
            "watch on `fact` never grew past 1 (saw %s)" % sorted(seen)
