"""Rows of the macOS platform capability that need no Mac.

Each row holds a terminal or build contract that was first found on macOS but
does not depend on Darwin: the contract is the terminal's, the child
process's or the path length's, so it is reproducible here.  The scenario ids
are the ones of the macos-platform capability.

Run: WPE_BIN=$PWD/wpe tests/.venv/bin/python -m pytest -q tests/test_macos_platform.py
"""
import os
import re
import shutil
import subprocess
import time

import pytest

import procinfo
from wpe_driver import WpeSession, WAIT_SCALE, ALT

DEL = "\x7f"                 # what Terminal.app, iTerm2 and kitty send for Backspace
ALT_A = "\033a"              # Run > Execute Make


def _terminfo_where_backspace_is_ctrl_h(tmp_path):
    """Return the TERMINFO directory of an xterm-256color whose kbs is ^H.

    Older macOS terminfo databases say kbs=^H for xterm-256color while the
    terminal itself sends 0x7f, so ncurses never folds the byte into
    KEY_BACKSPACE.  The terminfo of a current Linux says kbs=^? and folds it,
    which hides the editor's own handling of the raw byte.  The entry is the
    system's own with that one capability changed, built in the test's
    directory.  The test skips when infocmp or tic is missing."""
    if not (shutil.which("infocmp") and shutil.which("tic")):
        pytest.skip("infocmp and tic are needed to build the macOS-style entry")
    source = subprocess.check_output(["infocmp", "-x", "xterm-256color"],
                                     text=True)
    # macOS' own database already says kbs=^H; a current one says kbs=^?.
    if "kbs=^?" in source:
        source = source.replace("kbs=^?", "kbs=^H")
    assert "kbs=^H" in source, "this host's xterm-256color has no kbs at all"
    src_file = tmp_path / "xterm-kbs-bs.ti"
    src_file.write_text(source)
    tidir = tmp_path / "terminfo"
    tidir.mkdir()
    subprocess.check_call(["tic", "-x", "-o", str(tidir), str(src_file)])
    shown = subprocess.check_output(
        ["infocmp", "-x", "xterm-256color"],
        env=dict(os.environ, TERMINFO=str(tidir)), text=True)
    assert "kbs=^H" in shown, "the scratch terminfo entry was not picked up"
    return str(tidir)


# @scenario SC-macos-06
def test_raw_del_deletes_when_terminfo_says_kbs_is_ctrl_h(tmp_path):
    """A raw 0x7f deletes backwards and is never inserted as ^? when the
    terminfo entry says Backspace is ^H (macOS Terminal.app with its older
    database), so ncurses leaves the byte raw.  Typing ABC, DEL, DEL leaves A."""
    tidir = _terminfo_where_backspace_is_ctrl_h(tmp_path)
    with WpeSession(str(tmp_path), "", env_extra={"TERMINFO": tidir}) as w:
        w.key("A", "B", "C")
        w.key(DEL, DEL)
        w.save()
        assert w.alive(), "wpe died on a raw DEL"
        assert w.text().strip() == "A", \
            "two raw DELs after ABC should leave A, got %r" % w.text()


# ---------------------------------------------------------------- helpers

IDLE_WINDOW_S = 2.0               # how long the editor's CPU is sampled
IDLE_TICKS_MAX_PER_S = 25         # a quarter of one core; a spin is a whole one
QUIT_WAIT_S = 15
SIGPIPE_NUMBER = 13


def _cpu_ticks_used_over(pid, seconds):
    """CPU ticks (user plus system) pid used while the test slept `seconds`."""
    before = procinfo.cpu_ticks(pid)
    time.sleep(seconds)
    after = procinfo.cpu_ticks(pid)
    assert before is not None and after is not None, "the editor is gone"
    return after - before


def _assert_editor_idle(pid, what):
    """The editor sits in its input loop: it used less than a quarter of a
    core over the sample window.  A busy loop uses the whole of one."""
    used = _cpu_ticks_used_over(pid, IDLE_WINDOW_S)
    limit = IDLE_TICKS_MAX_PER_S * IDLE_WINDOW_S
    assert used < limit, \
        "%s: the editor used %d ticks in %.0f s (limit %d): it is spinning" \
        % (what, used, IDLE_WINDOW_S, limit)


def _wait_for(session, predicate, seconds):
    """Drain the editor's output until predicate() is true or time is up."""
    deadline = time.time() + seconds * WAIT_SCALE
    while time.time() < deadline:
        session._drain(0.3)
        if predicate():
            return True
    return predicate()


def _screen_text(session):
    return "\n".join(session.screen.display)


def _fake_bin(tmp_path, name, script):
    """A directory with one executable `name` made of `script`, to put first
    on PATH.  Returns the PATH value."""
    bindir = tmp_path / "bin"
    bindir.mkdir(exist_ok=True)
    fake = bindir / name
    fake.write_text(script)
    fake.chmod(0o755)
    return str(bindir) + os.pathsep + os.environ.get("PATH", "")


# A language server that answers `initialize`, then closes its stdin and
# stays alive: every later write of the editor's to it fails with EPIPE.
FAKE_SERVER_THAT_CLOSES_ITS_INPUT = r'''#!/bin/sh
read -r header
length=${header#Content-Length: }
length=${length%?}
read -r blank
head -c "$length" > /dev/null
body='{"jsonrpc":"2.0","id":1,"result":{"capabilities":{}}}'
printf 'Content-Length: %d\r\n\r\n%s' "${#body}" "$body"
exec 0<&-
exec sleep 20
'''


# @scenario SC-macos-07
def test_write_to_a_server_that_closed_its_input_does_not_kill_the_editor(tmp_path):
    """A language server that closes its stdin but stays alive makes the
    editor's next write to it fail with EPIPE.  The editor ignores SIGPIPE, so
    quitting (which writes shutdown to the server) ends it by itself, not by
    the signal, which is what macOS does by default to a process that writes to
    a closed pipe."""
    path = _fake_bin(tmp_path, "clangd", FAKE_SERVER_THAT_CLOSES_ITS_INPUT)
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    filename="t.c", wait=1.5, env_extra={"PATH": path}) as w:
        w.key("\033q", delay=0.4)
        w.key("e", delay=2.0)               # Alt-Q E: start the server
        w.key("\033x", delay=0.5)           # Alt-X: quit, which tells the server
        _wait_for(w, lambda: w.proc.poll() is not None, QUIT_WAIT_S)
        code = w.proc.poll()
        assert code is not None, "the editor did not end on Quit:\n" + _screen_text(w)
        assert code != -SIGPIPE_NUMBER, \
            "the editor was killed by SIGPIPE writing to the dead server"


# @scenario SC-macos-08
def test_a_child_that_floods_past_the_cap_is_killed(tmp_path):
    """Run > Execute Make on a Makefile whose command prints without end.
    After 16 MiB the editor kills the child, says so in Messages, and goes
    back to its input loop instead of staying at 100% CPU.  The command
    ignores SIGPIPE and every write error, as a runaway interpreter does, so
    closing the pipe alone would not stop it: only the kill does."""
    chunk = "x" * 1000
    (tmp_path / "Makefile").write_text(
        "all:\n\t@echo $$$$ > flood.pid; trap '' PIPE; "
        "while :; do echo %s; done 2>/dev/null\n" % chunk)
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    filename="t.c", wait=1.5) as w:
        pid = w.proc.pid
        w.key(ALT_A, delay=1.0)
        child_pid_file = tmp_path / "flood.pid"
        try:
            ended = _wait_for(
                w, lambda: "exceeded runaway cap" in _screen_text(w), 90)
            assert ended, "no runaway notice in Messages:\n" + _screen_text(w)
            child = int(child_pid_file.read_text().split()[0])
            gone = _wait_for(
                w, lambda: procinfo.run_state(child) in
                (procinfo.GONE, procinfo.ZOMBIE), 10)
            assert gone, "the flooding child (pid %d) is still running" % child
            assert w.alive(), "the editor died with the flood"
            _assert_editor_idle(pid, "after the flood was cut")
        finally:
            _end_flood(child_pid_file)


def _end_flood(pid_file):
    """End the flooding child of the test above by the pid it wrote, if it
    still runs and is still the recipe's shell (its command line names the
    pid file)."""
    try:
        pid = int(pid_file.read_text().split()[0])
    except (OSError, ValueError, IndexError):
        return
    if procinfo.run_state(pid) in (procinfo.GONE, procinfo.ZOMBIE):
        return
    if pid_file.name in (procinfo.command_line(pid) or ""):
        try:
            os.kill(pid, 9)
        except OSError:
            pass


# @scenario SC-macos-09
def test_a_missing_language_server_does_not_spin(tmp_path):
    """A .scala file, no server on PATH, Alt-Q E: the editor stays idle.
    The smoke checklist's bar was under 50% CPU; this measures the CPU time
    the process used over two seconds, which a spin would fill."""
    empty = tmp_path / "empty-bin"
    empty.mkdir()
    with WpeSession(str(tmp_path), "object X{def main(args:Array[String])={println(\"h\")}}\n",
                    filename="dead.scala", wait=1.5,
                    env_extra={"PATH": str(empty)}) as w:
        w.key("\033q", delay=0.4)
        w.key("e", delay=1.0)
        assert w.alive(), "the editor died starting a server that is missing"
        _assert_editor_idle(w.proc.pid, "language server missing")


# macOS hands out per-user temporary folders of this shape, and a test or a
# user's HOME under one is longer than the 128 bytes the syntax-file path
# buffer once had.
PRIVATE_VAR_FOLDERS = os.path.join(
    "private", "var", "folders", "zz", "0123456789abcdefghijklmnopqrstuv",
    "T", "pytest-of-someone-with-a-long-name", "pytest-12345",
    "test_a_long_home_still_opens_the_syntax_file_0")
LONG_PATH_MIN = 200
# A Pascal line: only a syntax_def file knows Pascal (the editor's built-in
# fallback has C and C++ alone), so its keyword is coloured only when the
# personal file under the long HOME was read whole.
PASCAL_LINE = "begin x end.\n"
PASCAL_KEYWORD = "begin"
PASCAL_PLAIN = "x end"           # its first cell is the plain identifier x


def _fg_at(session, text):
    """The foreground colour pyte holds for the first cell of `text` on the
    screen, or None when the text is not shown."""
    for y, row in enumerate(session.screen.display):
        x = row.find(text)
        if x >= 0:
            return session.screen.buffer[y][x].fg
    return None


# @scenario SC-macos-10
def test_a_long_home_and_tmpdir_do_not_overflow_the_syntax_path(tmp_path):
    """HOME and TMPDIR under a /private/var/folders-shaped path, far past 128
    bytes.  The editor reads $HOME/.xwpe/syntax_def into a buffer that must
    hold it; under the sanitizer build an overflow aborts the editor, and on
    macOS the checked snprintf aborts it without one.  A buffer that cut the
    path short would miss the file instead, so the row also shows that a
    language only that file defines is highlighted, with the system copy out
    of reach."""
    home = tmp_path / PRIVATE_VAR_FOLDERS
    home.mkdir(parents=True)
    no_lib = tmp_path / "no-system-lib"
    no_lib.mkdir()
    assert len(str(home)) > LONG_PATH_MIN, "the path is too short to matter"
    with WpeSession(str(home), PASCAL_LINE, filename="t.p",
                    wait=2.0, cols=100,
                    env_extra={"TMPDIR": str(home),
                               "XWPE_LIB": str(no_lib)}) as w:
        assert w.alive(), "the editor died starting with a long HOME"
        assert os.path.exists(os.path.join(str(home), ".xwpe", "syntax_def")), \
            "the personal syntax file the editor reads is not in place"
        assert PASCAL_KEYWORD in _screen_text(w), \
            "the file did not open:\n" + _screen_text(w)
        keyword = _fg_at(w, PASCAL_KEYWORD)
        plain = _fg_at(w, PASCAL_PLAIN)
        assert keyword != plain, \
            "'%s' is drawn like plain text (%s): the personal syntax file " \
            "under the long HOME was not read" % (PASCAL_KEYWORD, keyword)


UTF8_ACCENT = "café"
UTF8_CYRILLIC = "пример"
UTF8_EMOJI = "\U0001f980"


# @scenario SC-macos-15
def test_accent_cyrillic_and_emoji_reach_the_screen(tmp_path):
    """A buffer holding a Latin accent, Cyrillic and an emoji shows all three
    on the terminal screen, each judged on its own so the failure names it."""
    seed = "%s %s %s\n" % (UTF8_ACCENT, UTF8_CYRILLIC, UTF8_EMOJI)
    with WpeSession(str(tmp_path), seed, filename="u.txt", wait=1.5,
                    cols=100) as w:
        shown = _screen_text(w)
        assert w.alive(), "the editor died opening a UTF-8 file"
        assert UTF8_ACCENT in shown, "accent missing:\n" + shown
        assert UTF8_CYRILLIC in shown, "Cyrillic missing:\n" + shown
        assert UTF8_EMOJI in shown, "emoji missing:\n" + shown


# Where the pointer is read from: the terminal's own reports through ncurses
# (the xterm-256color entry) and the raw SGR report of a terminfo that has no
# SGR mouse capability (vt220).  Neither goes through gpm.
MOUSE_TERMINALS = ["xterm-256color", "vt220"]
FILE_MENU_ITEM = "Save As"         # an entry only the open File menu shows


def _sgr_report(button, col, row, press):
    """An SGR 1006 mouse report at 1-based screen (col, row)."""
    return "\x1b[<%d;%d;%d%s" % (button, col, row, "M" if press else "m")


def _binary_links_gpm(wpe_bin):
    """True when the binary was built with gpm, as ldd shows; None when ldd
    cannot say (not Linux)."""
    if not shutil.which("ldd"):
        return None
    out = subprocess.run(["ldd", os.path.realpath(wpe_bin)],
                         capture_output=True, text=True).stdout
    return "libgpm" in out


# @scenario SC-macos-03
@pytest.mark.parametrize("term", MOUSE_TERMINALS)
def test_a_click_opens_the_menu_from_the_terminals_own_reports(tmp_path, term):
    """A left click on File opens the File menu.  On a binary configured
    --without-gpm (no libgpm in ldd) this is the only mouse path, which is
    the macOS build; the same row also runs on a gpm build, where the report
    still comes from the terminal, not from the console."""
    import wpe_driver
    gpm = _binary_links_gpm(wpe_driver.WPE_BIN)
    with WpeSession(str(tmp_path), "AAAAA\nBBBBB\n",
                    env_extra={"TERM": term}) as w:
        bar = w.display()[0]
        assert "File" in bar, "menu bar not drawn: %r" % bar
        col = bar.index("File") + 1
        assert FILE_MENU_ITEM not in _screen_text(w), \
            "the File menu is already open before the click"
        w.key(_sgr_report(0, col, 1, True), _sgr_report(0, col, 1, False),
              delay=0.5)
        screen = _screen_text(w)
        assert w.alive(), "the editor died on a mouse report"
        assert FILE_MENU_ITEM in screen, \
            "a click on File did not open the menu (TERM=%s, libgpm linked: " \
            "%s):\n%s" % (term, gpm, screen)
