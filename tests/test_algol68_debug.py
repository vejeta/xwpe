"""Algol 68 (a68g --monitor) source-level debugger backend for wpe (DEB_A68G).

xwpe drives Algol 68 Genie's built-in monitor (`a68g --monitor`) as a debugger
backend, exactly like gdb/jdb/pdb: a .a68 file auto-selects a68g, Ctrl-G B sets
a breakpoint, Ctrl-G R runs to it, Ctrl-G S steps, Ctrl-G W inspects a variable,
Ctrl-G P shows the captured program output, and the program's end is detected so
the session quits cleanly.

These are full interactive sessions (compile + monitor startup), so timing is
generous.  Requires a68g installed; skips otherwise.

Notes learned empirically (kept here so the test is not fragile by accident):
  * wpe puts the terminal in application-cursor mode -- Down is ESC O B
    (``\\033OB``), not ESC [ B (which would insert a literal 'B').
  * Starting the debugger on a .a68 file pops a modal
    "Algol 68 file: switching to a68g monitor." notice (like pdb/jdb); it is
    dismissed with Return before the monitor runs.
"""
import shutil
import time

import pytest

from wpe_driver import WpeSession

pytestmark = pytest.mark.skipif(
    shutil.which("a68g") is None, reason="a68g (Algol 68 Genie) not installed")

# factorial(5) = 120; line 3 ("ELSE n * factorial(n - 1)") is the recursive
# body, hit with n = 5 on the first stop.
FACTORIAL = (
    "PROC factorial = (INT n) INT:\n"
    "  IF n <= 1 THEN 1\n"
    "  ELSE n * factorial(n - 1)\n"
    "  FI;\n"
    "\n"
    "INT result := factorial(5);\n"
    'print(("factorial(5) = ", result, newline))\n'
)

CTRL_G = "\x07"     # debugger command prefix
DOWN = "\033OB"     # application-cursor-mode Down (see module docstring)
F8 = "\033[19~"     # Step (over)


def _text(w):
    return "\n".join(w.display())


def _status(w):
    return w.display()[20]


def test_a68g_breakpoint_run_watch(tmp_path):
    """Set a breakpoint inside factorial, run to it, and inspect n (= 5).

    Exercises: auto-select a68g, Ctrl-G B (breakpoint %d), Ctrl-G R (compile +
    `a68g --monitor` + continue), the cursor-sync that jumps to the stopped
    line, and Ctrl-G W (`evaluate n` -> "(INT) +5") in the Watches window.

    Ctrl-G R must start the monitor immediately -- the a68g backend is
    auto-selected SILENTLY (no modal "switching to a68g" popup that would block
    the run and look like nothing happened), so no key is sent to dismiss one.

    The file is opened through a *subdirectory* ("src/factorial.a68") on
    purpose: a68g does not name the source file in its stop output, so the
    cursor-sync must locate the open .a68 window by extension, not by a path
    that would otherwise mismatch the working directory (regression guard for
    the "Can't find file" popup on a subdir-qualified path).
    """
    import os
    os.makedirs(os.path.join(str(tmp_path), "src"), exist_ok=True)
    with WpeSession(str(tmp_path), FACTORIAL, filename="src/factorial.a68",
                    wait=2.0) as w:
        w.key(DOWN, delay=0.4)
        w.key(DOWN, delay=0.4)            # cursor on line 3 (recursive body)
        w.key(CTRL_G, "b", delay=0.5)     # Ctrl-G B: toggle breakpoint
        w.key(CTRL_G, "r", delay=2.0)     # Ctrl-G R: run (no popup to dismiss)
        w._drain(5.0)                     # wait for compile + monitor + stop
        # Cursor must have jumped to the breakpoint line (line 3).
        assert w.alive(), "wpe died starting the a68g monitor"
        assert "3:" in _status(w), \
            "debugger should stop on line 3 (status shows L:C):\n%s" % _status(w)
        # Watch n -- a68g reports "(INT) +5".
        w.key(CTRL_G, "w", delay=1.0)     # Ctrl-G W: make watch
        w.key("n", delay=0.5)
        w.key("\r", delay=2.0)
        text = _text(w)
        assert w.alive(), "wpe died making a watch"
        assert "Watches" in text, "a Watches window should open:\n%s" % text
        assert "+5" in text, \
            "watch on n should show a68g's value (INT) +5:\n%s" % text
        # Stepping into the next recursive call (n - 1) must re-evaluate the
        # watch: n becomes +4.  (Regression: the Watches window used to show the
        # stale +5 because the a68g step path never refreshed it.)
        w.key(F8, delay=2.5)              # Step
        text = _text(w)
        assert w.alive(), "wpe died stepping with a watch set"
        assert "+4" in text, \
            "watch on n should update to (INT) +4 after stepping into " \
            "factorial(n - 1):\n%s" % text


def test_a68g_run_to_completion_captures_output(tmp_path):
    """Run a .a68 program under the debugger to completion.

    With no breakpoint, a68g auto-breaks at line 1 and Ctrl-G R continues to the
    end: the program's output ("factorial(5) = +120") is captured into the
    Messages window (Ctrl-G P), the end is detected ("End of code"), and the
    session quits cleanly without hanging.
    """
    with WpeSession(str(tmp_path), FACTORIAL, filename="factorial.a68",
                    wait=2.0) as w:
        w.key(CTRL_G, "r", delay=2.0)     # Ctrl-G R: run (no popup to dismiss)
        w._drain(6.0)                     # run to completion
        # The "End of code. Ctrl-G P for output." dialog is modal; dismiss it.
        w.key("\r", delay=1.0)
        w.key(CTRL_G, "p", delay=1.5)     # Ctrl-G P: program output -> Messages
        text = _text(w)
        assert w.alive(), "wpe died running the a68g program to completion"
        assert "120" in text, \
            "program output (factorial(5) = +120) should reach Messages:\n%s" \
            % text


def test_a68g_restart_after_finish_no_crash(tmp_path):
    """Re-run the debugger after the program finished -- must not crash.

    Regression for a SIGSEGV: a68g auto-breaks at line 1 and must never call
    e_mk_brk_main (the temp-breakpoint-at-main helper).  When it did, the helper
    allocated a phantom breakpoint slot and corrupted e_d_nbrpts to -1 across
    restarts, so the second Ctrl-G R after the program ended dereferenced a NULL
    breakpoint array.  The breakpoint must also still be armed on the re-run
    (the run stops at line 8 again), not run straight through.
    """
    with WpeSession(str(tmp_path), FACTORIAL, filename="factorial.a68",
                    wait=2.0) as w:
        w.key(DOWN, delay=0.4)
        w.key(DOWN, delay=0.4)            # cursor on line 3 (recursive body)
        w.key(CTRL_G, "b", delay=0.5)     # breakpoint
        w.key(CTRL_G, "r", delay=2.0)     # run (no popup to dismiss)
        w._drain(4.0)                     # stop at breakpoint
        # Continue until the program finishes (recursion depth 5 + tail).
        for _ in range(8):
            w.key(CTRL_G, "r", delay=2.0)
            if "End of code" in _text(w) or "120" in _text(w):
                break
        w.key("\r", delay=1.0)            # dismiss "End of code" dialog
        # Re-run twice: the original crash struck on the second Ctrl-G R.
        w.key(CTRL_G, "r", delay=2.0)
        w._drain(3.0)
        assert w.alive(), "wpe crashed re-running a68g after the program finished"
        w.key(CTRL_G, "r", delay=2.0)
        w._drain(4.0)
        assert w.alive(), \
            "wpe crashed on the second a68g re-run after finish (e_mk_brk_main)"
        # The breakpoint should re-arm: the re-run stops at line 3 again (the
        # status line "L:C" shows 3 somewhere on screen).  Layout can vary, so
        # scan all rows rather than pinning the status row.
        assert any(" 3:" in row for row in w.display()), \
            "re-run should re-stop at the breakpoint (line 3):\n%s" % _text(w)
