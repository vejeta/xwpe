"""GNU Algol 68 (ga68) compiler support for wpe.

ga68 is the GCC Algol 68 front-end -- a NATIVE compiler -- distinct from a68g
(the Genie interpreter).  The two use opposite, source-incompatible stropping
regimes, so xwpe detects each .a68 file's dialect from its content
(e_algol68_use_ga68 in we_prog.c) and drives the matching toolchain: a file in
the modern regime (lowercase begin/end, { } comments) is built and run with
ga68; one in the classic regime (UPPER BEGIN/END, # comments) with a68g.

This test pins the ga68 side: a modern-stropped program must compile to a
native binary and run, even when a68g is ALSO installed (the dialect sniff,
not install order, must decide).  Requires ga68; skips otherwise.
"""
import shutil
import time

import pytest

from wpe_driver import WpeSession

# Modern (ga68) stropping: lowercase keywords, { } comment, puts with 'n newline.
HELLO = 'begin\n   { GNU Algol 68 -- modern stropping }\n   puts ("Hello, ga68!\'n")\nend\n'

pytestmark = pytest.mark.skipif(
    shutil.which("ga68") is None, reason="ga68 (GNU Algol 68) not installed")


def _screen_text(w):
    return "\n".join(w.display())


def test_ga68_run_compiles_native_and_runs(tmp_path):
    """Run (Alt-U) a modern .a68: ga68 compiles a native binary and runs it,
    its output reaching Messages -- proving the dialect sniff picked ga68 over
    a68g and the GCC compile+run path (comp_sw 0) was taken."""
    with WpeSession(str(tmp_path), HELLO, filename="hello.a68", wait=2.0) as w:
        w.key("\033u", delay=1.0)        # Alt-U = Run (compile + run)
        time.sleep(2.5)
        w._drain(1.0)
        text = _screen_text(w)
        assert w.alive(), "wpe died running the ga68 program"
        assert "Hello, ga68!" in text, \
            "ga68-compiled program output should appear in Messages:\n%s" % text
        assert "Return-Code: 0" in text, \
            "a successful ga68 run should report Return-Code 0:\n%s" % text
