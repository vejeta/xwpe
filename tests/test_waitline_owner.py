"""Rows for the owner of the waiting lines at the foot of Messages.

A build, a starting debugger, a compile for a debug session and a scala-cli
setup each show a line there that says they wait, for how long, and that Esc
ends them.  The owner (we_waitline.c) keeps those lines as the last rows of
the Messages text, below every row of output, one row each, the bottom one
being the line the next Esc ends.

The pair every row starts from is G18's: a debug adapter that never answers
(a stand-in `dlv`) and a build held by a fifo.  No row sleeps: each waits for
a state (both lines on the screen, a trace line, a row of the screen) with
wait_until, and the build ends only when the row writes to its fifo.
"""
import os
import re
import shutil

import pytest

import gui_owner
from host_tools import needs
from wpe_driver import (ALT, WpeSession, wait_screen, wait_until)
from test_ai_plan_checklist import (_ai_build, _checklists, _evidence,
                                    _run_plan, LAID_OUT)
from test_ai_plan_checklist import _env as _plan_env
from test_ai_plan_checklist import _start_agent

CTRL_G = "\x07"
CTRL_W = "\x17"
ALT_A = "\033a"
ESC = "\033"
GO_SRC = "package main\n\nfunc main() {\n    x := 3\n    _ = x\n}\n"
DLV_LINE = "starting dlv"
BUILD_LINE = "building ..."
SECONDS = re.compile(r"(\d+)s")
# What a program prints in two pieces, spelled so that its source does not
# hold the word and the screen shows it only once the program ran.
HALF = "PROGHALF"


def _row_index(rows, needle):
    """The first screen row that holds @needle, -1 when none does."""
    for i, row in enumerate(rows):
        if needle in row:
            return i
    return -1


def _rows_with(rows, needle):
    """How many screen rows hold @needle."""
    return sum(1 for row in rows if needle in row)


def _seconds_of(rows, needle):
    """The seconds the line holding @needle shows, None when it is not there."""
    at = _row_index(rows, needle)
    if at < 0:
        return None
    found = SECONDS.search(rows[at])
    return int(found.group(1)) if found else None


def _both_lines(w):
    """(row of the adapter's line, row of the build's line) as the screen
    stands, None unless both are there."""
    rows = w.display(drain=0)
    dlv, build = _row_index(rows, DLV_LINE), _row_index(rows, BUILD_LINE)
    if dlv < 0 or build < 0:
        return None
    return dlv, build


def _release(path):
    """Let whatever waits on this fifo go on; a fifo with no reader is the wait
    being over already."""
    try:
        fd = os.open(str(path), os.O_WRONLY | os.O_NONBLOCK)
    except OSError:
        return
    try:
        os.write(fd, b"go\n")
    finally:
        os.close(fd)


def _pair_env(tmp_path, say=False):
    """The workspace of the pair: a Makefile whose build waits on a fifo and a
    `dlv` that never answers.  With @say the stand-in first waits on a second
    fifo, then prints one line on the adapter's terminal.
    Return: (env, the build's fifo, the stand-in's fifo or None)."""
    release = tmp_path / "build.release"
    os.mkfifo(release)
    (tmp_path / "Makefile").write_text(
        "all:\n\t@echo BUILD_START\n\t@echo $$PPID > build.pid\n"
        "\t@read _ < " + str(release) + "\n\t@echo BUILD_DONE\n\t@touch prog\n")
    bindir = tmp_path / "bin"
    bindir.mkdir()
    say_fifo = None
    body = "#!/bin/sh\nexec 3>&1\n"
    if say:
        say_fifo = tmp_path / "dlv.say"
        os.mkfifo(say_fifo)
        body += "read _ < %s\necho STUB_SAYS\n" % say_fifo
    body += "exec cat 3>&3 > %s\n" % (tmp_path / "adapter-stdin.log")
    (bindir / "dlv").write_text(body)
    (bindir / "dlv").chmod(0o755)
    env = {"PATH": str(bindir) + ":" + os.environ.get("PATH", ""),
           **gui_owner.product_env("XWPE_DEBUG_START_TIMEOUT", 600),
           "XWPE_UI_TRACE": str(tmp_path / "ui.trace")}
    return env, release, say_fifo


def _start_pair(w):
    """The adapter first, then the build; return once both lines are on the
    screen together."""
    w.key(CTRL_G, "r", delay=1.5)
    assert wait_screen(w, DLV_LINE, tmo=15), "the adapter's line is not shown"
    w.key(ALT_A, delay=0.8)
    assert wait_screen(w, "BUILD_START", tmo=10), "the build did not start"
    assert wait_until(w, lambda: _both_lines(w) is not None, tmo=10,
                      what="both waiting lines on the screen"), \
        "the two waiting lines are never on the screen together:\n%s" % \
        "\n".join(w.display(drain=0))


def _trace_text(tmp_path):
    try:
        with open(str(tmp_path / "ui.trace"), errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def _trace_count(tmp_path, needle):
    return _trace_text(tmp_path).count(needle)


def _session(tmp_path, say=False, **kw):
    env, release, say_fifo = _pair_env(tmp_path, say)
    return WpeSession(str(tmp_path), GO_SRC, filename="main.go", wait=1.5,
                      env_extra=env, **kw), release, say_fifo


# @scenario SC-waitline-01
def test_both_lines_turn_their_seconds_while_both_waits_live(tmp_path):
    """Both lines are on the screen at once, the build's at the bottom, and
    each one's seconds go on counting while the other lives."""
    sess, release, _ = _session(tmp_path)
    with sess as w:
        _start_pair(w)
        dlv, build = _both_lines(w)
        assert build == dlv + 1, \
            "the line Esc ends first is not the bottom one: %r" % ((dlv, build),)
        rows = w.display(drain=0)
        first = (_seconds_of(rows, DLV_LINE), _seconds_of(rows, BUILD_LINE))

        def turned():
            now = w.display(drain=0)
            d, b = _seconds_of(now, DLV_LINE), _seconds_of(now, BUILD_LINE)
            return (d is not None and b is not None and
                    d >= first[0] + 2 and b >= first[1] + 2)
        assert wait_until(w, turned, tmo=15,
                          what="both lines to count two more seconds"), \
            "the seconds of a line stopped while both waits live:\n%s" % \
            "\n".join(w.display(drain=0))
        _release(release)


# @scenario SC-waitline-02
def test_output_lands_above_both_lines(tmp_path):
    """What the adapter's terminal prints while both lines wait is a row above
    both, and neither line is lost or moved into it."""
    sess, release, say_fifo = _session(tmp_path, say=True)
    with sess as w:
        _start_pair(w)
        _release(say_fifo)
        assert wait_screen(w, "STUB_SAYS", tmo=10), \
            "the adapter's output never reached Messages"
        rows = w.display(drain=0)
        says = _row_index(rows, "STUB_SAYS")
        dlv, build = _both_lines(w)
        assert says < dlv < build, \
            "the output is not above the two lines: %r" % ((says, dlv, build),)
        assert build == dlv + 1, "the foot is not two adjacent rows"
        _release(release)


# @scenario SC-waitline-10
def test_program_bytes_and_a_waiting_line_stay_in_their_own_rows(tmp_path):
    """A program Run started prints half a line while the debugger's start
    still waits: the half line is a row of the program's, the waiting line is
    below it and is not written into, and the rest of the line, written after
    the line turned again, lands on the same row of the program's."""
    if shutil.which("go") is None:
        pytest.skip("go is not installed")
    env, _build_fifo, _ = _pair_env(tmp_path)
    program_fifo = tmp_path / "program.release"
    hold_fifo = tmp_path / "program.hold"
    os.mkfifo(program_fifo)
    os.mkfifo(hold_fifo)
    # The program prints half a line, waits for the row, prints the rest, and
    # then waits again so that its line is the last output Messages shows.
    src = ('package main\n\nimport (\n\t"bufio"\n\t"fmt"\n\t"os"\n)\n\n'
           'func main() {\n\tfmt.Print("PRO" + "GHALF")\n'
           '\tf, _ := os.Open("%s")\n\tbufio.NewReader(f).ReadString(\'\\n\')\n'
           '\tfmt.Println("WH" + "OLE")\n'
           '\tg, _ := os.Open("%s")\n\tbufio.NewReader(g).ReadString(\'\\n\')\n}\n'
           % (program_fifo, hold_fifo))
    env["GOFLAGS"] = "-mod=mod"
    env["GOCACHE"] = str(tmp_path / "gocache")
    env["GOPATH"] = str(tmp_path / "gopath")
    with WpeSession(str(tmp_path), src, filename="main.go", wait=1.5,
                    env_extra=env) as w:
        w.key(CTRL_G, "r", delay=1.5)
        assert wait_screen(w, DLV_LINE, tmo=15), "the adapter's line is not shown"
        w.key("\033[20;5~", delay=0.8)           # Run
        assert wait_screen(w, HALF, tmo=60), \
            "the program's half line is not shown:\n%s" % \
            "\n".join(w.display(drain=0))
        rows = w.display(drain=0)
        part = _row_index(rows, HALF)
        assert DLV_LINE not in rows[part], \
            "the program's bytes were written into the waiting line"
        assert _row_index(rows, DLV_LINE) > part, \
            "the waiting line is not below the program's half line"
        _release(program_fifo)
        assert wait_screen(w, HALF + "WHOLE", tmo=15), \
            "the rest of the line did not join the half line:\n%s" % \
            "\n".join(w.display(drain=0))
        rows = w.display(drain=0)
        assert DLV_LINE in "\n".join(rows), "the waiting line was deleted"
        assert _row_index(rows, HALF + "WHOLE") < _row_index(rows, DLV_LINE), \
            "the program's line is not above the waiting line"
        assert DLV_LINE not in rows[_row_index(rows, HALF + "WHOLE")]
        _release(hold_fifo)


# @scenario SC-waitline-03
def test_closing_messages_during_a_start_leaves_one_line(tmp_path):
    """Messages is closed while the debugger starts and opened again: the
    window shows one waiting line, never the old one and a new one."""
    sess, release, _ = _session(tmp_path)
    with sess as w:
        w.key(CTRL_G, "r", delay=1.5)
        assert wait_screen(w, DLV_LINE, tmo=15), "the line is not shown"
        w.key(CTRL_W, delay=0.8)
        assert wait_until(w, lambda: DLV_LINE not in "\n".join(w.display(drain=0)),
                          tmo=10, what="Messages to close"), \
            "Messages did not close"
        w.menu(ALT.WINDOW, "m", delay=0.8)
        assert wait_screen(w, DLV_LINE, tmo=10), \
            "the line is not in Messages after it opened again"
        rows = w.display(drain=0)
        seen = _seconds_of(rows, DLV_LINE)

        def turned():
            return (_seconds_of(w.display(drain=0), DLV_LINE) or 0) >= seen + 1
        assert wait_until(w, turned, tmo=10, what="the line to turn"), \
            "the line does not turn after Messages opened again"
        rows = w.display(drain=0)
        assert _rows_with(rows, DLV_LINE) == 1, \
            "Messages shows the debugger's line %d times:\n%s" % (
                _rows_with(rows, DLV_LINE), "\n".join(rows))


# @scenario SC-waitline-04
def test_a_box_keeps_the_screen_still_while_both_lines_turn(tmp_path):
    """A dialog is open while both heartbeats run: the lines are not painted
    under it (the trace says the paint was kept), and when it closes both lines
    show the seconds they have reached."""
    sess, release, _ = _session(tmp_path)
    kept = "paint deferred the Messages rows (already kept)"
    with sess as w:
        _start_pair(w)
        w.key(ALT.FILE, "o", delay=0.8)
        base = _trace_count(tmp_path, kept)
        assert wait_until(w, lambda: _trace_count(tmp_path, kept) >= base + 2,
                          tmo=15, what="the heartbeats to ask for paints under the box"), \
            "no paint was kept under the box:\n%s" % _trace_text(tmp_path)[-1500:]
        still = list(w.display(drain=0))
        base = _trace_count(tmp_path, kept)
        assert wait_until(w, lambda: _trace_count(tmp_path, kept) >= base + 2,
                          tmo=15, what="two more paints to be kept"), \
            "the heartbeats stopped under the box"
        assert list(w.display(drain=0)) == still, \
            "the screen changed under the box: a waiting line was painted"
        w.key(ESC, delay=0.8)
        seconds = _seconds_of(still, DLV_LINE)

        def current():
            now = w.display(drain=0)
            d = _seconds_of(now, DLV_LINE)
            return d is not None and (seconds is None or d >= seconds + 2)
        assert wait_until(w, current, tmo=15, what="the lines to be drawn again"), \
            "the lines are not current after the box closed:\n%s" % \
            "\n".join(w.display(drain=0))
        assert _both_lines(w) is not None, "a line is gone after the box closed"
        _release(release)


# @scenario SC-waitline-05
def test_a_build_that_ends_under_a_box_loses_its_line_when_the_box_closes(tmp_path):
    """The build ends while a dialog is open: its result waits for the dialog,
    and when the dialog closes the line is gone and the verdict is shown."""
    env, release, _ = _pair_env(tmp_path)
    with WpeSession(str(tmp_path), GO_SRC, filename="main.go", wait=1.5,
                    env_extra=env) as w:
        w.key(ALT_A, delay=0.8)
        assert wait_screen(w, "BUILD_START", tmo=10), "the build did not start"
        assert wait_screen(w, BUILD_LINE, tmo=10), "the build's line is not shown"
        w.key(ALT.FILE, "o", delay=0.8)
        _release(release)
        pid_file = tmp_path / "build.pid"
        w.key(ESC, delay=0.8)
        assert wait_screen(w, "Success", tmo=15), \
            "the build's verdict is not shown once the box closed:\n%s" % \
            "\n".join(w.display(drain=0))
        assert BUILD_LINE not in "\n".join(w.display(drain=0)), \
            "the build's line stayed after its end"
        assert pid_file.exists()


# @scenario SC-waitline-06
def test_the_scala_setup_line_goes_when_its_wait_is_cancelled(tmp_path):
    """The line that says scala-cli setup-ide runs is a waiting line: Esc ends
    the setup, and the line, whose "Esc cancels" is false by then, is gone."""
    from test_child_ends import (_scala_setup_env, SCALA_SEED, SETUP_START_S,
                                 SETUP_CANCELLED, _screen, _wait_for,
                                 _read_pid, _gone, SERVER_GONE_S)
    env = _scala_setup_env(tmp_path)
    setup = 0
    try:
        with WpeSession(str(tmp_path), SCALA_SEED, filename="A.scala", wait=1.5,
                        env_extra=env, answer_server_start=True) as w:
            w.key("\033q", delay=0.4)
            w.key("e", delay=0.3)
            assert _wait_for(w, lambda: _read_pid(str(tmp_path / "setup.pid")),
                             SETUP_START_S), "the setup did not start"
            setup = _read_pid(str(tmp_path / "setup.pid"))
            assert _wait_for(w, lambda: "Preparing the Scala build" in _screen(w),
                             SETUP_START_S), \
                "the waiting line is not shown:\n%s" % _screen(w)
            w.key(ESC, delay=0.3)
            assert _wait_for(w, lambda: SETUP_CANCELLED in _screen(w),
                             SETUP_START_S), _screen(w)
            assert _wait_for(w, lambda: _gone(setup), SERVER_GONE_S)
            assert "Preparing the Scala build" not in _screen(w), \
                "the setup's waiting line stayed after the wait ended:\n%s" % _screen(w)
    finally:
        if setup and not _gone(setup):
            os.kill(setup, 15)


# @scenario SC-waitline-07
def test_the_compile_for_debug_line_goes_when_its_wait_is_cancelled(tmp_path):
    """The compile for a debug session says it waits, in a line that goes with
    the compile when Esc ends it."""
    for tool in ("rustc", "gdb"):
        if shutil.which(tool) is None:
            pytest.skip("%s is not installed" % tool)
    release = tmp_path / "release"
    os.mkfifo(str(release))
    stub_dir = tmp_path / "stub"
    stub_dir.mkdir()
    stub = stub_dir / "rustc"
    stub.write_text('#!/bin/sh\nread _ < "%s"\nexec "%s" "$@"\n'
                    % (release, shutil.which("rustc")))
    stub.chmod(0o755)
    env = {"PATH": "%s:%s" % (stub_dir, os.environ["PATH"])}
    with WpeSession(str(tmp_path), 'fn main() {\n    println!("x");\n}\n',
                    filename="main.rs", wait=1.5, env_extra=env) as w:
        w.key(CTRL_G, "r", delay=0.5)
        assert wait_screen(w, "Compiling for debug", tmo=15), \
            "the compile's line is not shown"
        w.key(ESC, delay=0.8)
        assert wait_screen(w, "compile for debug cancelled", tmo=10), \
            "the cancel is not said in Messages"
        assert "Compiling for debug" not in "\n".join(w.display(drain=0)), \
            "the compile's waiting line stayed after the wait ended"
        _release(release)


# @scenario SC-waitline-08
@pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")
def test_a_checklist_with_a_wrapped_step_is_one_checklist_after_a_tick(tmp_path):
    """A step wider than the window takes two rows; a tick redraws the
    checklist and the old drawing, the wrapped rest of the step included, is
    gone."""
    long = "wrapped step " + "abcdefghij" * 12      # 133 bytes, wider than 100 columns
    reply = "@@TURN@@".join(["TOOL plan\n- " + long + "\n- second\n@@END",
                             "TOOL step_done second", "DONE finished"])
    messages, pane, txt, scr, alive = _run_plan(
        tmp_path, reply, [LAID_OUT], cols=100, rows=40)
    lists = _checklists(messages)
    assert len(lists) == 1, \
        "Messages holds %d checklists after one tick:\n%s" % (
            len(lists), _evidence(messages, pane, txt))
    assert sum(1 for r in messages if "abcdefghijabcdefghij" in r) <= 2, \
        "the wrapped rest of the step was left behind by the redraw:\n%s" % \
        _evidence(messages, pane, txt)
    assert alive


COUNT_ROW = re.compile(r"\d+ errors?, \d+ warnings?")


# @scenario SC-waitline-13
def test_the_error_list_counts_output_rows_when_the_debugger_line_ends_mid_build(tmp_path):
    """A debugger start waits when a build begins, and its line ends while the
    build runs (the adapter exits).  The build then prints a compiler error
    and exits 0: with make's own row that is two rows of output.  The verdict
    reads the error list when a build printed two rows, whether or not a
    waiting line sat at the foot of Messages when it began: the build did not
    succeed.  Counting the debugger's row as one of the build's output rows
    hid one row of the build's, the list was not read and the build said
    Success."""
    release = tmp_path / "build.release"
    die = tmp_path / "dlv.die"
    os.mkfifo(release)
    os.mkfifo(die)
    (tmp_path / "Makefile").write_text(
        "all:\n\t@touch build.started\n\t@read _ < %s\n"
        "\t@echo 'main.go:3:1: error: first problem'\n" % release)
    bindir = tmp_path / "bin"
    bindir.mkdir()
    (bindir / "dlv").write_text("#!/bin/sh\nread _ < %s\nexit 0\n" % die)
    (bindir / "dlv").chmod(0o755)
    env = {"PATH": str(bindir) + ":" + os.environ.get("PATH", ""),
           **gui_owner.product_env("XWPE_DEBUG_START_TIMEOUT", 600),
           "XWPE_UI_TRACE": str(tmp_path / "ui.trace")}
    with WpeSession(str(tmp_path), GO_SRC, filename="main.go", wait=1.5,
                    env_extra=env, rows=40) as w:
        w.key(CTRL_G, "r", delay=1.5)
        assert wait_screen(w, DLV_LINE, tmo=15), "the adapter's line is not shown"
        w.key(ALT_A, delay=0.8)
        assert wait_until(w, lambda: (tmp_path / "build.started").exists(),
                          tmo=15, what="the build to start"), "the build did not start"
        assert wait_until(w, lambda: _both_lines(w) is not None, tmo=10,
                          what="both waiting lines"), "the two lines are not shown"
        _release(die)
        assert wait_screen(w, "Could not start the debug adapter", tmo=15), \
            "the adapter's end is not told:\n%s" % "\n".join(w.display(drain=0))
        w.key("\r", delay=0.5)
        assert wait_until(w, lambda: DLV_LINE not in "\n".join(w.display(drain=0)),
                          tmo=15, what="the debugger's line to end"), \
            "the debugger's line did not end:\n%s" % "\n".join(w.display(drain=0))
        _release(release)
        assert wait_until(w, lambda: COUNT_ROW.search("\n".join(w.display(drain=0))),
                          tmo=30, what="the build's count row"), \
            "the build did not end:\n%s" % "\n".join(w.display(drain=0))
        rows = w.display(drain=0)
        assert _rows_with(rows, "Success") == 0, \
            "a build that printed a compiler error said Success:\n%s" % \
            "\n".join(rows)


CLEARED_BACK = "waitline cleared, 1 line(s) laid again"
PLAN_REPLY = "@@TURN@@".join(["TOOL plan\n- first\n- second\n@@END", "DONE finished"])


# @scenario SC-waitline-11
@pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")
def test_an_assistant_run_that_empties_messages_leaves_the_waiting_line(tmp_path):
    """The debugger's start still waits when an assistant run empties Messages
    for its checklist.  Emptying goes through the owner, which puts the line
    back as part of the same call (not at the line's next tick, 120 ms later,
    which would hide a missing put-back from any screen row): the trace says
    one line was laid again, the checklist is above the line, and the line is
    on the screen once."""
    env, release, _ = _pair_env(tmp_path)
    env.update(_plan_env(tmp_path, PLAN_REPLY))
    with WpeSession(str(tmp_path), GO_SRC, filename="main.go", wait=1.5,
                    env_extra=env, cols=120, rows=40) as w:
        w.key(CTRL_G, "r", delay=1.5)
        assert wait_screen(w, DLV_LINE, tmo=15), "the adapter's line is not shown"
        _start_agent(w, "lay out a plan")
        assert wait_until(w, lambda: CLEARED_BACK in _trace_text(tmp_path),
                          tmo=30, what="the run to empty Messages"), \
            "Messages was emptied without the line laid again:\n%s" % \
            _trace_text(tmp_path)[-1500:]
        assert wait_screen(w, "=== Agent plan", tmo=15), \
            "the checklist is not shown:\n%s" % "\n".join(w.display(drain=0))
        rows = w.display(drain=0)
        assert _rows_with(rows, DLV_LINE) == 1, \
            "the debugger's line is shown %d times:\n%s" % (
                _rows_with(rows, DLV_LINE), "\n".join(rows))
        assert _row_index(rows, "=== Agent plan") < _row_index(rows, DLV_LINE), \
            "the checklist is not above the waiting line"
        _release(release)


VM_WORDS = "Picked up JDK_JAVA_OPTIONS"
JAVA_SRC = "class T { public static void main(String[] a) { } }\n"
DEBUGGER_START_ENDED = "waitline end the debugger start"


# @scenario SC-waitline-12
@needs("javac", "jdb")
def test_the_vm_words_of_a_starting_jdb_land_above_its_waiting_line(tmp_path):
    """The program's VM prints a line on the session's terminal while jdb's
    start still waits (the java launcher notes JDK_JAVA_OPTIONS on its
    terminal as it starts, before it joins jdb).  The words are a row of
    their own above the waiting line.  When jdb answers, the start ends and
    the line goes, the words stay.  Written into or below the line, the words
    would leave it behind: no row would read as its text any more, and the
    line would stay for good.  The row reads Messages once the start ended,
    as the user does; it is red when the bracket is taken out of
    e_d_pty_append_to_messages."""
    env = {**gui_owner.product_env("XWPE_DEBUG_START_TIMEOUT", 600),
           "JDK_JAVA_OPTIONS": "-Dwaitline=1",
           "XWPE_UI_TRACE": str(tmp_path / "ui.trace")}
    with WpeSession(str(tmp_path), JAVA_SRC, filename="T.java", wait=1.5,
                    env_extra=env, rows=50) as w:
        w.key(CTRL_G, "r", delay=1.5)
        assert wait_until(w, lambda: DEBUGGER_START_ENDED in _trace_text(tmp_path),
                          tmo=90, what="the start to end"), \
            "the start never ended:\n%s" % _trace_text(tmp_path)[-1500:]
        assert wait_screen(w, VM_WORDS, tmo=15), \
            "the VM's words never reached Messages:\n%s\n%s" % (
                "\n".join(w.display(drain=0)), _trace_text(tmp_path)[-1500:])
        rows = w.display(drain=0)
        assert _rows_with(rows, "Esc cancels") == 0, \
            "the waiting line was left behind after its wait ended:\n%s" % \
            "\n".join(rows)
