"""The debugger's other prompts and the lines of a start, said to the user.

A line of the start that the debugger answers with a prompt of its own (pdb's
(com), ((Pdb)), >>> and the statement continuation, gdb's >) used to wait for
the 30 second deadline.  Now the engine tells it after a quarter second of
silence, a .pdbrc commands block is sent with its end, and every line of the
start is read by the debugger's own refusal reader: a refusal of the editor's
own containment stops the start with a box that names the command and the
reason, and a refusal of a line of the user's own start-up file is said and
the start goes on, as the debugger itself does.

Each row's debugger is the real one.  The sentences are read from the box or
the status row; the box wraps, so a sentence is read with its line breaks
and the box's frame taken out.
"""
import os
import re
import shutil
import subprocess
import sys

import pytest

from wpe_driver import WpeSession, tool_usable, wait_screen
from test_startplan_debug import (CTRL_G, SEED_C, RUN_IT, USE_MINE, RAN_WAIT, SETTLE,
                                  _debug_box, _screen, _wait)
from test_startplan_pdb_jdb_apart import (PdbFolder, STOP_S, ENTER, _stopped,
                                          _traced_sends, needs_pdb)
from test_startplan_gdb_apart import GdbFolder, needs_gdb

PROMPT_S = 8               # a quarter second of silence is far inside this
ASAN_SLOW = 1.0


def _text(screen):
    """The words of the boxes and rows on @screen, the frames taken out and
    the line breaks of a wrapped sentence joined by a blank."""
    rows = [re.sub(r"^[x ]+|[x ]+$", "", r) for r in screen.splitlines()]
    return " ".join(" ".join(rows).split())


def _traced(folder, key):
    """The whole sentences the editor said under @key ("debugger said",
    "startplan stop"): the status row and the box crop a long one."""
    return [l.split(key + " ", 1)[1] for l in folder.traced().splitlines()
            if l.startswith(key + " ")]


def _python_has_statement_continuation():
    out = subprocess.run(["python3", "--version"], capture_output=True, text=True).stdout
    m = re.search(r"(\d+)\.(\d+)", out)
    return bool(m) and (int(m.group(1)), int(m.group(2))) >= (3, 13)


def _run_pdbrc(tmp_path, rc_text, wait_for, extra=None):
    """A .pdbrc in the folder, Ctrl-G R, Run it; the screen once @wait_for
    shows (or at the end of the wait), and what the editor sent."""
    folder = PdbFolder(tmp_path)
    (folder.ws / ".pdbrc").write_text(rc_text)
    with folder.session(extra=extra) as s:
        _debug_box(s, "Command from ./.pdbrc")
        s.key(RUN_IT, delay=0.8)
        shown = wait_screen(s, wait_for, tmo=PROMPT_S + STOP_S)
        screen = _screen(s)
    return folder, shown, screen


# @scenario SC-debug-138
@needs_pdb
def test_pdb_a_block_opened_by_an_alias_runs_under_run_it(tmp_path):
    """T2: an alias that expands to commands is no keyword the editor could
    see, and pdb answers it with the block's prompt like the word itself: the
    block is taken, sent with its end, and nothing waits for a deadline."""
    folder, shown, screen = _run_pdbrc(
        tmp_path,
        "break 2\nalias cm commands %1\ncm 1\nsilent\ncontinue\nend\n",
        "Program exited")
    assert shown, "the block did not run:\n" + folder.traced() + screen
    text = _text(screen)
    assert "did not answer" not in text and "prompt, not its own" not in text, text
    assert _traced_sends(folder)[-1] == "end", _traced_sends(folder)


# @scenario SC-debug-138
@needs_pdb
def test_pdb_a_file_that_ends_inside_a_block_stops_the_start_with_a_box(tmp_path):
    """T2b: the last line leaves pdb at (com): the box names the line, the
    file and the prompt, and says the file ends inside its block."""
    folder, shown, screen = _run_pdbrc(
        tmp_path, "break 2\ncommands 1\nsilent\n", "Debugger stopped: pdb answered")
    text = _text(screen)
    assert shown, "no box:\n" + screen
    assert "Debugger stopped: pdb answered `silent` (./.pdbrc) with its (com) prompt, " \
           "not its own; the file ends inside its block" in text, text
    assert "did not answer" not in text, text


# @scenario SC-debug-138
@needs_pdb
@pytest.mark.parametrize("line,prompt", [
    ("interact", ">>>"),
])
def test_pdb_a_line_it_reads_from_the_terminal_stops_the_start(tmp_path, line, prompt):
    """T3: pdb answers the line with another prompt that only the terminal
    can answer: the start stops, the box names the line and the prompt, and
    nothing waits for the deadline.  (T4, pdb's debug, is a unit row only:
    the nested debugger reads ./.pdbrc again, which holds the same line, so
    that file recurses in pdb itself and no editor row can use it.)"""
    folder, shown, screen = _run_pdbrc(tmp_path, "break 2\n%s\n" % line,
                                       "Debugger stopped: pdb answered")
    text = _text(screen)
    assert shown, "no box:\n" + screen
    assert ("Debugger stopped: pdb answered `%s` (./.pdbrc) with its %s prompt, not its own"
            % (line, prompt)) in text, text


# @scenario SC-debug-138
@needs_pdb
@pytest.mark.skipif(not _python_has_statement_continuation(),
                    reason="pdb's statement continuation needs Python 3.13 or later")
def test_pdb_an_open_statement_stops_the_start(tmp_path):
    """T5: a statement left open is answered with the continuation prompt."""
    folder, shown, screen = _run_pdbrc(tmp_path, "break 2\n!for _ in range(1):\n",
                                       "Debugger stopped: pdb answered")
    text = _text(screen)
    assert shown, "no box:\n" + screen
    assert "with its ... prompt, not its own" in text, text


# @scenario SC-debug-138
@needs_pdb
def test_pdb_a_refused_line_of_the_pdbrc_is_said_and_the_start_goes_on(tmp_path):
    """T7: pdb refuses a line of the user's own start-up file and goes on;
    the status row says which line and why, and the first stop comes."""
    folder = PdbFolder(tmp_path)
    (folder.ws / ".pdbrc").write_text("p nosuchname\nbreak 2\n")
    with folder.session() as s:
        _debug_box(s, "Command from ./.pdbrc")
        s.key(RUN_IT, delay=0.8)
        said = _wait(lambda: _traced(folder, "debugger said"), PROMPT_S + STOP_S)
        stopped = _wait(lambda: _stopped(s), STOP_S)
        screen = _screen(s)
    assert said, "nothing said which line pdb refused:\n" + screen
    assert _traced(folder, "debugger said") == [
        "pdb refused `p nosuchname` (./.pdbrc): NameError: name 'nosuchname' is not defined"], \
        _traced(folder, "debugger said")
    assert stopped, "the start did not go on:\n" + screen
    assert "Debugger stopped" not in _text(screen), screen


# @scenario SC-debug-138
@needs_pdb
def test_pdb_a_pdbrc_of_the_home_that_reads_the_terminal_is_said_at_the_start(tmp_path):
    """T16: ~/.pdbrc holds interact: pdb is at its >>> prompt before the
    editor's first command.  The banner wait names it instead of waiting for
    the start's deadline."""
    folder = PdbFolder(tmp_path)
    (folder.home / ".pdbrc").write_text("interact\n")
    with folder.session(extra={"XWPE_DEBUG_START_TIMEOUT": "20"}) as s:
        s.key(CTRL_G, "r", delay=1.0)
        said = wait_screen(s, "answered its start with its >>> prompt", tmo=PROMPT_S)
        screen = _screen(s)
    assert said, "the start did not say pdb is at another prompt:\n" + screen
    assert "A start-up file it reads itself left it there" in _text(screen), screen
    assert "did not answer within" not in _text(screen), screen


F7 = "\033[18~"
F8 = "\033[19~"
SEED_LOOKS_LIKE_A_PROMPT = (
    "def f():\n"
    "    print('>>> ', end='', flush=True)\n"
    "    __import__('time').sleep(1.5)\n"
    "    return 7\n"
    "x = 1\n"
    "x = 2\n"
    "x = 3\n")
STOP_LINE = 6


def _pdb_pids(root):
    """The pids of the pdb processes whose command line names @root."""
    found = []
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            with open("/proc/%s/cmdline" % pid, "rb") as fh:
                argv = fh.read().split(b"\0")
        except OSError:
            continue
        if b"pdb" in argv and any(root.encode() in a for a in argv):
            found.append(int(pid))
    return found


# @scenario SC-debug-138
@needs_pdb
def test_pdb_left_at_another_prompt_at_a_stop_refuses_every_resume(tmp_path):
    """T15: a watch whose code prints ">>> " and stays silent past the quiet
    time leaves the engine stranded at a stop.  Every resume key then says
    why it is refused, none reaches pdb (the mark stays), and Quit ends pdb.
    A key kept for later would never run, since the engine never answers
    again."""
    folder = PdbFolder(tmp_path)
    with folder.session(seed=SEED_LOOKS_LIKE_A_PROMPT) as s:
        for _ in range(STOP_LINE - 1):
            s.key("\033OB", delay=0.1)
        s.key(CTRL_G, "b", delay=0.4)
        s.key(CTRL_G, "r", delay=1.0)
        assert _wait(lambda: s.mark_line() == STOP_LINE, STOP_S), _screen(s)
        pids = _pdb_pids(str(tmp_path))
        s.key(CTRL_G, "w", delay=1.0)
        s.key("f()", delay=0.3)
        s.key(ENTER, delay=1.0)
        assert _wait(lambda: any("with its >>> prompt" in l
                                 for l in _traced(folder, "debugger said")), PROMPT_S), \
            _screen(s)
        s._drain(2.5)                      # pdb's own prompt comes late, unread
        for key in (F7, F8, CTRL_G + "r"):
            s.key(key, delay=0.3)
            row = _screen(s).splitlines()[-1]
            assert "pdb is at its >>> prompt after `p f()`" in row, (key, row)
            assert s.mark_line() == STOP_LINE, "a resume reached pdb"
        s.key(CTRL_G, "q", delay=2.0)
        s._drain(2.0)
    assert pids, "no pdb process was found"
    assert not [p for p in pids if os.path.exists("/proc/%d" % p)], "pdb outlived Quit"


# ---- gdb ----

def _gdb_box_session(folder, init_text, remove=None):
    """A folder with a .gdbinit (so the box opens); returns the session
    context after the box is up.  @remove: called while the box is open."""
    (folder.ws / ".gdbinit").write_text(init_text)
    return folder.session(SEED_C)


def _after_box(s, folder, key):
    _debug_box(s, "Command from ./.gdbinit")
    s.key(key, delay=0.8)


# @scenario SC-debug-138
@needs_gdb
def test_gdb_a_refused_cd_stops_the_start_with_gdbs_words_and_no_symbol(tmp_path):
    """T8: the program's folder is gone between the box and the start: gdb
    refuses the editor's cd, the start stops and the box gives gdb's reason,
    plain (gdb 18's error symbol is turned off by the start's style lines)."""
    folder = GdbFolder(tmp_path, "cli")
    (folder.ws / ".gdbinit").write_text("echo hello\\n\n")
    with folder.session(SEED_C) as s:
        _debug_box(s, "Command from ./.gdbinit")
        shutil.rmtree(str(folder.ws))
        s.key(USE_MINE, delay=0.8)
        shown = wait_screen(s, "gdb refused `cd ", tmo=PROMPT_S + STOP_S)
        screen = _screen(s)
    assert shown, "no box:\n" + screen
    text = _text(screen)
    assert "Debugger stopped: gdb refused `cd " in text, text
    stops = _traced(folder, "startplan stop")
    assert len(stops) == 1 and stops[0].endswith("No such file or directory."), stops
    assert "❌" not in stops[0] and "❌" not in screen, stops


# @scenario SC-debug-138
@needs_gdb
def test_gdb_mi_a_refused_cd_stops_the_start_with_gdbs_words(tmp_path):
    """T9: the same over gdb's machine interface: gdb's ^error names the
    folder, and the box ends with its reason however long the folder's
    path is."""
    folder = GdbFolder(tmp_path, "mi")
    (folder.ws / ".gdbinit").write_text("echo hello\\n\n")
    with folder.session(SEED_C) as s:
        _debug_box(s, "Command from ./.gdbinit")
        shutil.rmtree(str(folder.ws))
        s.key(USE_MINE, delay=0.8)
        shown = wait_screen(s, "gdb refused `-environment-cd ", tmo=PROMPT_S + STOP_S)
        screen = _screen(s)
    assert shown, "no box:\n" + screen
    stops = _traced(folder, "startplan stop")
    assert len(stops) == 1 and stops[0].endswith("No such file or directory."), stops


# @scenario SC-debug-138
@needs_gdb
def test_gdb_a_refused_file_stops_the_start(tmp_path):
    """T10: the program is gone between the box and the start: gdb refuses
    the editor's file line."""
    folder = GdbFolder(tmp_path, "cli")
    (folder.ws / ".gdbinit").write_text("echo hello\\n\n")
    with folder.session(SEED_C) as s:
        _debug_box(s, "Command from ./.gdbinit")
        os.unlink(str(folder.ws / "t.e"))
        s.key(USE_MINE, delay=0.8)
        shown = wait_screen(s, "gdb refused `file ", tmo=PROMPT_S + STOP_S)
        screen = _screen(s)
    assert shown, "no box:\n" + screen
    stops = _traced(folder, "startplan stop")
    assert len(stops) == 1 and stops[0].endswith("t.e: No such file or directory."), stops


# @scenario SC-debug-138
@needs_gdb
def test_gdb_a_program_without_debug_information_is_no_refusal(tmp_path):
    """T10c, the control: gdb notes "(No debugging symbols found ...)" on its
    output stream and takes the program; the start goes on and nothing is
    refused."""
    folder = GdbFolder(tmp_path, "cli")
    (folder.ws / ".gdbinit").write_text("echo hello\\n\n")
    with folder.session(SEED_C) as s:
        _debug_box(s, "Command from ./.gdbinit")
        shutil.copy("/usr/bin/true", str(folder.ws / "t.e"))
        s.key(USE_MINE, delay=0.8)
        assert _wait(lambda: any(l.startswith("file ") for l in _traced_sends(folder)), STOP_S), \
            folder.traced()
        s._drain(SETTLE)
        screen = _screen(s)
    assert _traced(folder, "debugger said") == [], _traced(folder, "debugger said")
    assert _traced(folder, "startplan stop") == [], screen


# @scenario SC-debug-138
@needs_gdb
def test_gdb_a_refused_source_is_said_and_the_start_goes_on(tmp_path):
    """T11: gdb stops reading the user's .gdbinit at its first bad line and
    goes on, as it does with its own start-up file; the status row says which
    file and why, and the start reaches its first stop."""
    folder = GdbFolder(tmp_path, "cli")
    (folder.ws / ".gdbinit").write_text("nosuchcmd\nshell touch %s\n" % folder.ran)
    with folder.session(SEED_C) as s:
        _debug_box(s, "Command from ./.gdbinit")
        s.key(RUN_IT, delay=0.8)
        said = _wait(lambda: _traced(folder, "debugger said"), PROMPT_S + STOP_S)
        screen = _screen(s)
        s.key(CTRL_G, "r", delay=1.0)
        stopped = _wait(lambda: _stopped(s), STOP_S)
        later = _screen(s)
    assert said, "the status row did not say gdb refused the file:\n" + screen
    sentence = _traced(folder, "debugger said")[0]
    assert sentence.startswith("gdb refused `source "), sentence
    assert "(./.gdbinit): " in sentence and "Undefined command" in sentence, sentence
    assert not folder.ran.exists(), "gdb read on past the bad line of the file"
    assert "Debugger stopped" not in _text(later), later
    assert stopped or "Program exited" in later, later


# @scenario SC-debug-138
@needs_gdb
def test_gdb_a_warning_is_no_refusal(tmp_path):
    """T12, the control: `directory` of a missing folder warns on gdb's error
    stream and succeeds; the start goes on and says nothing of a refusal."""
    folder = GdbFolder(tmp_path, "cli")
    (folder.ws / ".gdbinit").write_text("directory /nonexistent-xwpe-dir\n")
    with folder.session(SEED_C) as s:
        _debug_box(s, "Command from ./.gdbinit")
        s.key(RUN_IT, delay=0.8)
        assert _wait(lambda: any(l.startswith("file ") for l in _traced_sends(folder)), STOP_S), \
            folder.traced()
        s._drain(SETTLE)
        screen = _screen(s)
    assert _traced(folder, "debugger said") == [], _traced(folder, "debugger said")
    assert _traced(folder, "startplan stop") == [], screen

