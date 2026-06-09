"""Go source-level debugging via the Debug Adapter Protocol (dlv dap), in wpe.

A `.go` file is built and debugged by the adapter, not by xwpe's compiler/gdb
pipeline: e_start_debug detects the extension and routes to the DAP bridge
(we_debug.c), which opens a reverse-TCP `dlv dap` session, stops at main.main,
and maps Ctrl-G R / F8 / the Watches window onto DAP continue/next/evaluate.

This pins the end-to-end editor flow: Ctrl-G R stops on a source line, F8
advances it through the DWARF line table, and a watch RE-EVALUATES on each step
(the same refresh-on-stop contract the text backends honour).

Requires `dlv` (delve) and `go`.  Skips otherwise.
"""
import re
import shutil
import time

import pytest

from wpe_driver import WpeSession

# Iterative factorial: `fact` grows 1 -> 1 -> 2 -> 6 -> 24 -> 120 as the loop
# runs, so a watch on it shows several distinct values while stepping.
PROG = (
    "package main\n"
    "\n"
    "import \"fmt\"\n"
    "\n"
    "func main() {\n"
    "\tn := 5\n"
    "\tfact := 1\n"
    "\tfor i := 1; i <= n; i++ {\n"
    "\t\tfact = fact * i\n"
    "\t}\n"
    "\tfmt.Println(fact)\n"
    "}\n"
)

CTRL_G = "\x07"
F8 = "\033[19~"     # Step (over)

pytestmark = pytest.mark.skipif(
    shutil.which("dlv") is None or shutil.which("go") is None,
    reason="dlv (delve) and go required")


def _text(w):
    return "\n".join(w.display())


def _status_line_no(w):
    """The line number shown in the status bar (display row 20), or -1."""
    m = re.search(r"(\d+):", w.display()[20])
    return int(m.group(1)) if m else -1


def _watch_val(w, name):
    """Current value shown for the named watch in the Watches window, or None."""
    pat = re.compile(re.escape(name) + r":\s*(-?\d+)")
    for line in w.display():
        m = pat.search(line)
        if m:
            return int(m.group(1))
    return None


def _write_go_module(tmp_path):
    """dlv builds the package, so the source dir needs a go.mod (Go >= 1.16)."""
    (tmp_path / "go.mod").write_text("module dapdemo\n\ngo 1.21\n")


def test_go_dap_run_and_step(tmp_path):
    """Ctrl-G R starts dlv on the Go package; F8 advances the source line."""
    _write_go_module(tmp_path)
    with WpeSession(str(tmp_path), PROG, filename="main.go", wait=2.0) as w:
        w.key(CTRL_G, "r", delay=5.0)        # build + launch dlv dap, stop at main
        time.sleep(2.0)
        w._drain(1.5)
        assert w.alive(), "wpe died starting dlv dap on the Go package"
        start = _status_line_no(w)
        assert start > 0, \
            "DAP should stop on a source line (status:\n%s)" % _text(w)

        w.key(F8, delay=2.5)                 # step over one source line
        w._drain(1.0)
        assert w.alive(), "wpe died stepping the Go program under dlv"
        stepped = _status_line_no(w)
        assert stepped > start, \
            "F8 should advance to a later source line (%d -> %d):\n%s" \
            % (start, stepped, _text(w))


def test_go_dap_watch_updates_on_step(tmp_path):
    """A watch must RE-EVALUATE on every Step via DAP evaluate(context=watch),
    not stay frozen at its add-time value.  Same contract as the gdb/ga68
    watch-refresh regression, exercised through the DAP bridge."""
    _write_go_module(tmp_path)
    with WpeSession(str(tmp_path), PROG, filename="main.go", wait=2.0) as w:
        w.key(CTRL_G, "r", delay=5.0)
        time.sleep(1.5)
        w._drain(1.5)
        assert w.alive(), "wpe died starting dlv dap"
        w.key(CTRL_G, "w", delay=1.0)         # Make Watch dialog
        for ch in "fact":
            w.key(ch, delay=0.05)
        w.key("\r", delay=1.0)                # confirm
        w._drain(1.0)
        seen = set()
        for _ in range(12):                   # step through the whole loop
            w.key(F8, delay=1.2)
            w._drain(0.6)
            v = _watch_val(w, "fact")
            if v is not None:
                seen.add(v)
        assert w.alive(), "wpe died stepping with a watch set"
        assert any(v > 1 for v in seen), \
            "watch on `fact` never updated past its initial value (saw %s) -- " \
            "the Watches window is not refreshing on Step via DAP" % sorted(seen)
