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

from wpe_driver import WpeSession

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

    a68g's diagnostic carries no file name and the line number sits mid-message
    ("... in line N"), so xwpe's error jump is best-effort -- but the failure
    IS recognised: Make enters error-navigation mode (the status bar gains
    Prev./Next Error), which only happens when the compiler reported errors.
    """
    bad = 'BEGIN\n  print(("oops", new line)\nEND\n'   # missing ')'
    with WpeSession(str(tmp_path), bad, filename="bad.a68", wait=2.0) as w:
        w.key("\033[20~", delay=1.0)     # F9 = Make (syntax check)
        time.sleep(2.0)
        w._drain(1.0)
        text = _screen_text(w).lower()
        assert w.alive(), "wpe died checking the Algol 68 program"
        assert "err." in text, \
            "a68g syntax error should put Make into error-navigation mode:\n%s" \
            % _screen_text(w)
        assert "return-code: 0" not in text, \
            "a syntax-checked file with an error must not report success:\n%s" \
            % _screen_text(w)
