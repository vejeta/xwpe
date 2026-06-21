"""Truecolor semantic tokens on the console (1.6.6), end to end with clangd.

The semantic-tokens overlay (Alt-Q M) paints method/function names in a real
24-bit orange (#FF8C00) on terminals that can show it -- the 16-colour palette
has no orange, so before this they fell back to bright red.

This drives the WHOLE chain: clangd classifies `add` as a function token ->
e_lsp_sem_color_at flags the cell ATTR_TC (we_debug.c) -> fk_colset selects an
ncurses extended pair (we_term.c).  Under TERM=xterm-direct the terminfo
advertises RGB, so ncurses emits the exact "ESC[38:2::255:140:0m" SGR; we assert
that byte sequence appears in wpe's raw output once the overlay is on.  Under a
non-direct terminal the same cells fall back to the 16-colour red, so xterm-direct
is what exercises the truecolor path.

Skips when clangd is absent (same as the other clangd bridge tests).
"""
import shutil
import time

import pytest

from wpe_driver import WpeSession

PROG = (
    "#include <stdio.h>\n"
    "\n"
    "int add(int a, int b) { return a + b; }\n"
    "\n"
    "int main(void) {\n"
    "    int total = add(2, 3);\n"
    "    printf(\"%d\\n\", total);\n"
    "    return total;\n"
    "}\n"
)

ALT_Q = "\033q"     # the LSP prefix (Alt-Q)
DIRECT_TERM = {"TERM": "xterm-direct"}   # terminfo with the RGB capability

# ncurses emits SGR truecolor in the colon form with an empty colour-space
# field: ESC [ 38 : 2 : : R : G : B m.  #FF8C00 == 255;140;0.
ORANGE_SGR = b"38:2::255:140:0"

pytestmark = pytest.mark.skipif(
    shutil.which("clangd") is None, reason="clangd required")


def test_function_token_is_truecolor_orange(tmp_path):
    with WpeSession(str(tmp_path), PROG, filename="demo.c", wait=2.0,
                    env_extra=DIRECT_TERM) as w:
        time.sleep(1.0)
        w._drain(1.0)
        # Eager-start began clangd on open; give it a moment to be ready, then
        # toggle the semantic-colours overlay (Alt-Q M), which fetches tokens
        # and repaints.
        w.key(ALT_Q, delay=0.4)
        w.key("e", delay=6.0)           # ensure clangd is up (diagnostics path)
        time.sleep(1.5)
        w._drain(1.5)
        del w.raw[:]                    # only look at bytes emitted AFTER this
        w.key(ALT_Q, delay=0.4)
        w.key("m", delay=2.0)           # Alt-Q M: semantic colours on -> repaint
        time.sleep(1.5)
        w._drain(2.0)
        assert w.alive(), "wpe died enabling semantic truecolor"
        assert ORANGE_SGR in bytes(w.raw), (
            "expected the function token painted in 24-bit orange "
            "(%r) under xterm-direct; semantic overlay did not emit it" %
            ORANGE_SGR.decode())
