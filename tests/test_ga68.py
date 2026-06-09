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


def test_ga68_run_from_subdirectory(tmp_path):
    """Run (Alt-U) a modern .a68 that lives in a SUBDIRECTORY (cwd != file's
    dir).  The dialect sniff opens the file to read its stropping, so it must
    use the file's FULL path: a bare window name fails to open from a different
    cwd, detection comes back undecided, and the source falls through to a68g
    -- whose UPPER-stropping parser rejects the modern program.  This pins that
    a subdir modern file still builds and runs with ga68.

    (The compile path that originally misdetected this -- Make/Compile via
    e_comp -- was verified with an instrumented red/green run; here we assert
    the user-visible outcome through Run, which must not regress.)"""
    if shutil.which("a68g") is None:
        pytest.skip("needs BOTH ga68 and a68g installed to exercise the choice")
    (tmp_path / "src").mkdir()
    with WpeSession(str(tmp_path), HELLO, filename="src/hello.a68", wait=2.0) as w:
        w.key("\033u", delay=1.0)        # Alt-U = Run (compile + run)
        time.sleep(2.5)
        w._drain(1.0)
        text = _screen_text(w)
        assert w.alive(), "wpe died running the subdir ga68 program"
        assert "Hello, ga68!" in text, \
            "ga68 output missing -- dialect sniff failed on the subdir path:\n%s" % text
        assert "Return-Code: 0" in text, \
            "a successful ga68 run should report Return-Code 0:\n%s" % text
