"""Algol 68 (a68g) compiler/runner support for wpe.

xwpe registers Algol 68 Genie (a68g) as a language backend (we_prog.c): a
.a68 / .alg file is auto-detected, Make/Run drives a68g, and "Run" (Alt-U /
Ctrl-F9) executes the program with its output captured in Messages.  Make (F9)
passes --norun so a68g only checks the syntax.

Requires a68g installed.  Skips otherwise.
"""
import shutil
import time

import pytest

from wpe_driver import WpeSession, wait_screen
from test_compile_and_messages import active_position, active_title

HELLO = 'BEGIN\n  print(("Hello, Algol 68!", new line))\nEND\n'

pytestmark = pytest.mark.skipif(
    shutil.which("a68g") is None, reason="a68g (Algol 68 Genie) not installed")


def _screen_text(w):
    return "\n".join(w.display())


def test_algol68_run_shows_output(tmp_path):
    """Run (Alt-U) a .a68 file: a68g executes it and the output reaches Messages."""
    with WpeSession(str(tmp_path), HELLO, filename="hello.a68", wait=2.0) as w:
        w.key("\033u", delay=1.0)        # Alt-U = Run (compile + run)
        time.sleep(2.5)
        w._drain(1.0)
        text = _screen_text(w)
        assert w.alive(), "wpe died running the Algol 68 program"
        assert "Hello, Algol 68!" in text, \
            "a68g program output should appear in Messages:\n%s" % text
        assert "Return-Code: 0" in text, \
            "a successful a68g run should report Return-Code 0:\n%s" % text


def test_algol68_syntax_check_reports_error(tmp_path):
    """Make (F9) runs a68g --norun; a syntax error is detected, not executed.

    a68g echoes the offending source line with its number and names no file,
    so the row is filed under the file being compiled: the build ends with
    the count row, the cursor lands on the offending line of bad.a68, and no
    run is reported.
    """
    bad = 'BEGIN\n  print(("oops", new line)\nEND\n'   # missing ')' on line 2
    with WpeSession(str(tmp_path), bad, filename="bad.a68", wait=2.0) as w:
        w.key("\033[20~", delay=1.0)     # F9 = Make (syntax check)
        assert wait_screen(w, "1 error, 0 warnings", tmo=20), \
            "a68g's syntax error did not end the build with its count:\n%s" \
            % _screen_text(w)
        text = _screen_text(w).lower()
        assert w.alive(), "wpe died checking the Algol 68 program"
        assert active_title(w) == "bad.a68", \
            "the jump did not land in bad.a68:\n%s" % _screen_text(w)
        line, _ = active_position(w)
        assert line == 2, "the jump went to line %d, not 2:\n%s" % (line, _screen_text(w))
        assert "return-code: 0" not in text, \
            "a syntax-checked file with an error must not report success:\n%s" \
            % _screen_text(w)
