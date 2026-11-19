"""The log and exit rules of the graphical owner, checked without a display.

A graphical row used to pass over an editor that its compositor or X server
had cut off: the editor lingered with no window, or exited, and a row that
looked only at the saved file or at "still running" was green.  The owner
now reads the editor's own log for a lost-display line and looks at how the
editor ended.  The rows here feed the scanner planted logs, and end small
stand-in processes (python, no display) as an editor to see the two rules
fire, stay quiet, and obey a declaration.

The libwayland text the scanner matches is pinned to libwayland-client 1.26.
"""
import os
import pty
import signal
import sys

import pytest

import gui_owner

PLANT_PROTOCOL = ("xdg_toplevel#9: error 4: buffer size (1024 x 768) does not "
                  "match the configured size (1000 x 750)")
PLANT_DESTROYED = "[destroyed object]: error 1: invalid method 3"
PLANT_X_BROKEN = ("X connection to :94 broken (explicit kill or server "
                  "shutdown).")
PLANT_XIO = 'XIO:  fatal IO error 11 (Resource temporarily unavailable) on X server ":94"'
PLANT_OWN = "xwpe: connection to the Wayland compositor lost: broken pipe"
# What a clipboard helper that inherited the editor's standard error would
# print about its own connection; it is not the editor's own failure.
PLANT_SEND = "Error sending request for wl_surface.damage_buffer: Broken pipe"
PLANT_MARSHAL = "Error marshalling request for wl_surface.commit: Broken pipe"
PLANT_CHILD ="wl-copy: failed to connect to a Wayland server"
CLEAN_LOG = "xwpe: started\nredraw 80x30\nFile t.c saved!\n"
READY_S = 20.0
SEGV_AT_ONCE = "import os, signal\nos.kill(os.getpid(), signal.SIGSEGV)\n"
# The stand-ins here die of crash signals to test the owner, never to leave
# a core file on the machine.
NO_CORE = 0


# @scenario SC-harness-owner-70
def test_each_lost_display_line_form_is_found():
    """The libwayland protocol error (and its destroyed-object form), the
    two Xlib forms and the editor's own line are each reported."""
    for planted in (PLANT_PROTOCOL, PLANT_DESTROYED, PLANT_X_BROKEN,
                    PLANT_XIO, PLANT_OWN, PLANT_SEND, PLANT_MARSHAL):
        text = "first\n%s\nlast\n" % planted
        assert gui_owner.display_errors(text) == [planted]


# @scenario SC-harness-owner-70
def test_a_clean_log_and_lines_that_only_mention_an_error_are_clean():
    """A normal log, a child's own line and a line that has the words in the
    middle are not reported: the rule matches whole forms at the line start."""
    text = CLEAN_LOG + PLANT_CHILD + "\nnote: error 4: not a protocol line\n"
    text += "saw xdg_toplevel#9: error 4: indented\n"
    assert gui_owner.display_errors(text) == []
    assert gui_owner.display_errors("") == []


# @scenario SC-harness-owner-70
def test_several_lines_come_back_in_order():
    text = "\n".join([PLANT_OWN, CLEAN_LOG.strip(), PLANT_PROTOCOL])
    assert gui_owner.display_errors(text) == [PLANT_OWN, PLANT_PROTOCOL]


# @scenario SC-harness-owner-71
def test_the_exit_rule_flags_crashes_and_failure_codes_only():
    """Crash signals and a non-zero code are flagged; exit 0, SIGTERM,
    SIGHUP and an editor that has not ended are not."""
    flagged = gui_owner.unexpected_exit_text
    for crash in (signal.SIGSEGV, signal.SIGABRT, signal.SIGBUS,
                  signal.SIGFPE, signal.SIGILL):
        assert crash.name in flagged(-crash)
    assert "code 1" in flagged(1)
    for quiet in (0, -signal.SIGTERM, -signal.SIGHUP, None):
        assert flagged(quiet) is None
    for other in (signal.SIGQUIT, signal.SIGKILL, signal.SIGPIPE):
        assert other.name in flagged(-other)
    assert flagged(-signal.SIGKILL, by_ladder=True) is None
    assert "SIGSEGV" in flagged(-signal.SIGSEGV, by_ladder=True)


@pytest.fixture
def owner(tmp_path):
    """An owner of the row's own, begun as a session fixture would."""
    own = gui_owner.Owner(str(tmp_path / "out"))
    own.begin()
    yield own
    late = own.finish()
    assert not late, "left at the owner's end: %s" % [l.describe() for l in late]


def _standin_editor(owner, tmp_path, name, code):
    """Start a python stand-in as an "xwpe" process whose log gets one
    line, ready for the row to end or find ended.  code is its program."""
    log_dir = str(tmp_path / name)
    os.makedirs(log_dir)
    return owner.launch([sys.executable, "-E", "-s", "-c", code],
                        {"PATH": os.environ["PATH"]}, log_dir, log_dir,
                        "xwpe", name, core_limit=NO_CORE)


def _wait_log(editor, text):
    """Wait until the stand-in has written text, so the row reads a log
    that has it."""
    gui_owner.wait_for(lambda: text in editor.log_text(),
                       "the stand-in to write its line", READY_S,
                       state_fn=editor.log_text)


def _print_then_sleep(line):
    return ("import sys, time\nprint(%r); sys.stdout.flush()\n"
            "time.sleep(60)\n" % line)


def _end_by_itself(owner, tmp_path, name, body):
    """A stand-in that runs body and ends; returned once it has ended."""
    editor = _standin_editor(owner, tmp_path, name, body)
    gui_owner.wait_for(lambda: not editor.running(),
                       "the stand-in to end by itself", READY_S,
                       state_fn=editor.log_text)
    return editor


# @scenario SC-harness-owner-72
def test_a_protocol_line_in_the_editors_log_fails_the_row(owner, tmp_path):
    editor = _standin_editor(owner, tmp_path, "m1",
                             _print_then_sleep(PLANT_PROTOCOL))
    _wait_log(editor, PLANT_PROTOCOL)
    with pytest.raises(gui_owner.DisplayProtocolError) as raised:
        editor.end()
    assert PLANT_PROTOCOL in str(raised.value)
    assert editor.log in str(raised.value)


# @scenario SC-harness-owner-72
def test_a_declared_display_loss_passes(owner, tmp_path):
    editor = _standin_editor(owner, tmp_path, "declared",
                             _print_then_sleep(PLANT_OWN))
    editor.expect_display_loss()
    _wait_log(editor, PLANT_OWN)
    editor.end()


# @scenario SC-harness-owner-72
def test_a_clean_log_passes(owner, tmp_path):
    editor = _standin_editor(owner, tmp_path, "clean",
                             _print_then_sleep("redraw 80x30"))
    _wait_log(editor, "redraw")
    editor.end()


# @scenario SC-harness-owner-73
def test_a_child_line_in_the_editors_log_is_the_editors(owner, tmp_path):
    """A helper that inherits the editor's standard error writes into the
    editor's log, so a protocol line it prints is found there and must be
    declared by the row that expects it; the helper's own log, when the
    owner keeps one, is never scanned."""
    child = "print(%r)" % PLANT_PROTOCOL
    code = ("import subprocess, sys, time\n"
            "subprocess.run([sys.executable, '-E', '-s', '-c', %r], check=True)\n"
            "time.sleep(60)\n" % child)
    editor = _standin_editor(owner, tmp_path, "child", code)
    _wait_log(editor, PLANT_PROTOCOL)
    with pytest.raises(gui_owner.DisplayProtocolError):
        editor.end()
    helper = _standin_editor(owner, tmp_path, "helper",
                             _print_then_sleep(PLANT_PROTOCOL))
    helper.kind = "client"
    _wait_log(helper, PLANT_PROTOCOL)
    helper.end()


# @scenario SC-harness-owner-74
def test_a_crash_the_row_did_not_ask_for_fails_it(owner, tmp_path):
    editor = _end_by_itself(owner, tmp_path, "crash",
                            "import os, signal\nos.kill(os.getpid(), signal.SIGSEGV)\n")
    with pytest.raises(gui_owner.UnexpectedExit) as raised:
        editor.end()
    assert "SIGSEGV" in str(raised.value)


# @scenario SC-harness-owner-74
def test_a_failure_exit_fails_unless_declared(owner, tmp_path):
    undeclared = _end_by_itself(owner, tmp_path, "code3", "raise SystemExit(3)\n")
    with pytest.raises(gui_owner.UnexpectedExit) as raised:
        undeclared.end()
    assert "code 3" in str(raised.value)
    declared = _end_by_itself(owner, tmp_path, "declared3", "raise SystemExit(3)\n")
    declared.expect_exit(code=3)
    declared.end()
    wrong = _end_by_itself(owner, tmp_path, "wrong", "raise SystemExit(4)\n")
    wrong.expect_exit(code=3)
    with pytest.raises(gui_owner.UnexpectedExit):
        wrong.end()
    anything = _end_by_itself(owner, tmp_path, "any", "raise SystemExit(5)\n")
    anything.expect_exit()
    anything.end()


# @scenario SC-harness-owner-74
def test_a_quiet_exit_and_a_row_sent_sigterm_pass(owner, tmp_path):
    quit_zero = _end_by_itself(owner, tmp_path, "zero", "raise SystemExit(0)\n")
    quit_zero.end()
    termed = _standin_editor(owner, tmp_path, "term",
                             _print_then_sleep("redraw"))
    _wait_log(termed, "redraw")
    termed.watch.signal(signal.SIGTERM)
    gui_owner.wait_for(lambda: not termed.running(), "the SIGTERM to end it",
                       READY_S, state_fn=termed.log_text)
    termed.end()


# @scenario SC-harness-owner-75
def test_a_second_end_after_a_failed_one_does_nothing(owner, tmp_path):
    """The row's own end() raising must not make the finalizer's end() raise
    again as a teardown error."""
    editor = _end_by_itself(owner, tmp_path, "twice", "raise SystemExit(3)\n")
    with pytest.raises(gui_owner.UnexpectedExit):
        editor.end()
    editor.end()


# @scenario SC-harness-owner-75
def test_a_declaration_never_accepts_a_signal(owner, tmp_path):
    editor = _end_by_itself(owner, tmp_path, "declared-crash", SEGV_AT_ONCE)
    editor.expect_exit()
    with pytest.raises(gui_owner.UnexpectedExit) as raised:
        editor.end()
    assert "SIGSEGV" in str(raised.value)


# @scenario SC-harness-owner-75
@pytest.mark.parametrize("sig", [signal.SIGQUIT, signal.SIGKILL])
def test_a_death_by_another_signal_fails_the_row(owner, tmp_path, sig):
    editor = _end_by_itself(owner, tmp_path, "sig%d" % sig,
                            "import os\nos.kill(os.getpid(), %d)\n" % sig)
    with pytest.raises(gui_owner.UnexpectedExit) as raised:
        editor.end()
    assert sig.name in str(raised.value)


# @scenario SC-harness-owner-75
def test_a_crash_while_the_owner_ends_the_editor_fails_the_row(owner, tmp_path):
    """A stand-in that crashes when the owner's SIGTERM arrives."""
    code = ("import os, signal, time\n"
            "def crash(*args):\n"
            "    signal.signal(signal.SIGSEGV, signal.SIG_DFL)\n"
            "    os.kill(os.getpid(), signal.SIGSEGV)\n"
            "signal.signal(signal.SIGTERM, crash)\n"
            "print('ready', flush=True)\ntime.sleep(60)\n")
    editor = _standin_editor(owner, tmp_path, "ladder-crash", code)
    _wait_log(editor, "ready")
    with pytest.raises(gui_owner.UnexpectedExit) as raised:
        editor.end()
    assert "SIGSEGV" in str(raised.value)


# ---------------------------------------------------------------------------
# The terminal editor: every exit is judged where the row ends it.

# A stand-in that gives itself no core file, for the rows that crash it
# without declaring it (the owner must not be the one to leave a dump).
SELF_NO_CORE = ("import resource\n"
                "resource.setrlimit(resource.RLIMIT_CORE, (0, 0))\n")
SLEEP_FOR_A_WHILE = "import time\ntime.sleep(60)\n"


def _terminal_standin(owner, tmp_path, name, code, expect_crash=None,
                      extra_env=None):
    """Start a python stand-in through Terminal.start, as a terminal row
    starts the editor; return (owned, master side of its pty)."""
    log_dir = str(tmp_path / name)
    os.makedirs(log_dir)
    master, slave = pty.openpty()
    owned = gui_owner.Terminal.start(
        owner, sys.executable, ["-E", "-s", "-c", code], log_dir, log_dir,
        master, slave, extra_env=extra_env, label=name,
        expect_crash=expect_crash)
    os.close(slave)
    return owned, master


def _end_terminal_standin(owner, owned, master, before):
    """End the stand-in as a driver's close() does, then give the row's last
    word; return the error it raised, or None."""
    owned.end_for_row()
    os.close(master)
    try:
        owner.end_since(before)
    except AssertionError as error:
        return error
    return None


def _run_terminal_standin(owner, tmp_path, name, code, expect_crash=None,
                          wait_for_end=True):
    """Start, let it end by itself (unless it is made to run), end it as a
    driver does; return the row's error or None."""
    before = owner.ledger.pids()
    owned, master = _terminal_standin(owner, tmp_path, name, code,
                                      expect_crash=expect_crash)
    if wait_for_end:
        gui_owner.wait_for(lambda: not owned.running(),
                           "the stand-in to end by itself", READY_S)
    return _end_terminal_standin(owner, owned, master, before)


# @scenario SC-harness-owner-88
def test_a_terminal_editor_that_crashes_undeclared_fails_the_row(owner, tmp_path):
    error = _run_terminal_standin(owner, tmp_path, "t-crash",
                                  SELF_NO_CORE + SEGV_AT_ONCE)
    assert isinstance(error, gui_owner.UnexpectedExit)
    assert "SIGSEGV" in str(error)


# @scenario SC-harness-owner-88
@pytest.mark.parametrize("sig", [signal.SIGQUIT, signal.SIGTRAP, signal.SIGSYS])
def test_every_signal_the_editor_handles_as_a_crash_is_one(owner, tmp_path, sig):
    code = SELF_NO_CORE + "import os\nos.kill(os.getpid(), %d)\n" % sig
    error = _run_terminal_standin(owner, tmp_path, "t-sig%d" % sig, code)
    assert isinstance(error, gui_owner.UnexpectedExit)
    assert sig.name in str(error)


# @scenario SC-harness-owner-88
def test_a_declared_crash_passes_and_leaves_no_core_allowed(owner, tmp_path):
    """The declared stand-in dies of SIGSEGV with nothing set by itself: the
    owner started it with RLIMIT_CORE 0."""
    code = ("import resource\n"
            "limit = resource.getrlimit(resource.RLIMIT_CORE)\n"
            "import os, signal, sys\n"
            "if limit != (0, 0):\n"
            "    sys.exit(7)\n"
            "os.kill(os.getpid(), signal.SIGSEGV)\n")
    error = _run_terminal_standin(owner, tmp_path, "t-declared", code,
                                  expect_crash=signal.SIGSEGV)
    assert error is None, error


# @scenario SC-harness-owner-88
def test_a_declared_crash_that_does_not_happen_fails_the_row(owner, tmp_path):
    ended = _run_terminal_standin(owner, tmp_path, "t-quiet",
                                  "raise SystemExit(0)\n",
                                  expect_crash=signal.SIGSEGV)
    assert isinstance(ended, gui_owner.UnexpectedExit)
    assert "expected SIGSEGV" in str(ended)
    running = _run_terminal_standin(owner, tmp_path, "t-running",
                                    SLEEP_FOR_A_WHILE,
                                    expect_crash=signal.SIGSEGV,
                                    wait_for_end=False)
    assert isinstance(running, gui_owner.UnexpectedExit)
    assert "expected SIGSEGV" in str(running)
    assert "still running" in str(running)


# @scenario SC-harness-owner-88
def test_a_declared_crash_by_another_signal_fails_the_row(owner, tmp_path):
    error = _run_terminal_standin(
        owner, tmp_path, "t-other",
        "import os, signal\nos.kill(os.getpid(), signal.SIGABRT)\n",
        expect_crash=signal.SIGSEGV)
    assert isinstance(error, gui_owner.UnexpectedExit)
    assert "expected SIGSEGV" in str(error)


# @scenario SC-harness-owner-88
@pytest.mark.skipif(not sys.platform.startswith("linux"),
                    reason="reads /proc/<pid>/limits")
def test_a_declared_editor_starts_with_no_core_file_allowed(owner, tmp_path):
    before = owner.ledger.pids()
    owned, master = _terminal_standin(owner, tmp_path, "t-limits",
                                      SLEEP_FOR_A_WHILE,
                                      expect_crash=signal.SIGSEGV)
    with open("/proc/%d/limits" % owned.pid) as fh:
        line = [l for l in fh if l.startswith("Max core file size")][0]
    _end_terminal_standin(owner, owned, master, before)
    assert line.split()[4:6] == ["0", "0"], line


# @scenario SC-harness-owner-88
def test_a_terminal_editor_may_be_killed_or_lose_its_input(owner, tmp_path):
    """Ordinary ends of a terminal row, which the crash rule leaves alone:
    SIGKILL (a row that kills the editor to see its debugger go) and exit
    code 1 (the editor's way to end when its input is gone)."""
    killed = _run_terminal_standin(
        owner, tmp_path, "t-kill",
        "import os, signal\nos.kill(os.getpid(), signal.SIGKILL)\n")
    assert killed is None, killed
    code_one = _run_terminal_standin(owner, tmp_path, "t-one",
                                     "raise SystemExit(1)\n")
    assert code_one is None, code_one


# @scenario SC-harness-owner-88
def test_a_self_crash_variable_without_a_declaration_is_refused(owner, tmp_path):
    with pytest.raises(gui_owner.OwnerRefused) as raised:
        _terminal_standin(owner, tmp_path, "t-refused", SLEEP_FOR_A_WHILE,
                          extra_env={"XWPE_TEST_CRASH": "1"})
    assert "expect_crash" in str(raised.value)
