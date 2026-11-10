"""The screen has one owner: what arrives while a box is up waits for it.

A dialog, a dropdown or the assistant's review box owns the screen while it
is up.  Program output, a language server's diagnostics or the debugger's
word on a breakpoint that arrives meanwhile is not drawn under the box; it
is kept and drawn once the box has closed, on the window the user then sees.
These rows drive the real editor (wpe, terminal mode) with a program under
gdb, a stand-in language server and the assistant's deterministic backend,
and read the screen and the editor's trace (XWPE_UI_TRACE).
"""
import os
import re
import shutil
import signal
import stat
import sys
import time

import pytest

import procinfo
from wpe_driver import WpeSession, ALT, WAIT_SCALE, assistant_build

CTRL_G = "\x07"
DOWN = "\033OB"
ESC = "\033"
POLL_S = 0.2
START_TIMEOUT_S = 20          # build + gdb start, before scaling
OUTPUT_TIMEOUT_S = 10
HOLD_S = 2.0                  # long enough for a hundred lines of output
TRACE = "ui.trace"
DEFERRED_OUTPUT = "paint deferred program output"
REPLAYED_OUTPUT = "paint replay program output"

# A program that prints a numbered line every 20 ms, for ever.
PRINTER_PROG = (
    "#include <stdio.h>\n"
    "#include <unistd.h>\n"
    "int main(void)\n"
    "{\n"
    "  int i = 0;\n"
    "  setvbuf(stdout, NULL, _IONBF, 0);\n"
    "  for (;;) { printf(\"PRINTER%06d\\n\", i++); usleep(20000); }\n"
    "  return 0;\n"
    "}\n"
)
PRINTER_BREAK_LINE = 5
PRINTER_LINE = re.compile(r"PRINTER(\d{6})")
FIND_TITLE = "Find"


def _need(*tools):
    for tool in tools:
        if shutil.which(tool) is None:
            pytest.skip("no %s" % tool)


def _screen(w):
    return "\n".join(w.display(drain=0))


def _wait_for(w, predicate, timeout):
    deadline = time.time() + timeout * WAIT_SCALE
    while time.time() < deadline:
        w._drain(POLL_S)
        if predicate():
            return True
    return predicate()


def _read(path):
    try:
        with open(path) as fh:
            return fh.read()
    except OSError:
        return ""


def _last_printed(w):
    """The highest output line number on the screen, or -1."""
    numbers = [int(n) for n in PRINTER_LINE.findall(_screen(w))]
    return max(numbers) if numbers else -1


def _run_the_printer(w):
    """Breakpoint on the first line of main, Ctrl-G R to it, Ctrl-G R on:
    return once the program's lines show in Messages."""
    for _ in range(PRINTER_BREAK_LINE - 1):
        w.key(DOWN, delay=0.1)
    w.key(CTRL_G, "b", delay=0.4)
    w.key(CTRL_G, "r", delay=1.0)
    assert _wait_for(w, lambda: w.mark_line() == PRINTER_BREAK_LINE,
                     START_TIMEOUT_S), \
        "Ctrl-G R should stop on line %d:\n%s" % (PRINTER_BREAK_LINE, _screen(w))
    w.key(CTRL_G, "r", delay=0.5)
    assert _wait_for(w, lambda: _last_printed(w) >= 0, OUTPUT_TIMEOUT_S), \
        "the program's output should show in Messages:\n%s" % _screen(w)


# @scenario SC-screen-01
def test_program_output_under_a_dialog_paints_nothing_under_it_and_shows_when_it_closes(
        tmp_path):
    """The program prints all the time; the Find dialog opens over the
    editor.  While it is up the screen does not change at all (no output
    line is drawn, under the box or beside it), the trace says the output
    waited, and once Esc closes it the output goes on in Messages with lines
    printed while the dialog was up."""
    _need("gcc", "gdb")
    trace = tmp_path / TRACE
    with WpeSession(str(tmp_path), PRINTER_PROG, filename="p.c", wait=1.5,
                    env_extra={"XWPE_UI_TRACE": str(trace)}) as w:
        _run_the_printer(w)
        w.menu(ALT.SEARCH, "f", delay=0.6)
        assert _wait_for(w, lambda: FIND_TITLE in _screen(w), OUTPUT_TIMEOUT_S), \
            "Search > Find should open its dialog:\n%s" % _screen(w)
        w._drain(0.5)
        before = w.display(drain=0)
        last_before = _last_printed(w)
        w._drain(HOLD_S)
        after = w.display(drain=0)
        assert after == before, \
            "nothing may be drawn while the dialog is up:\nbefore:\n%s\nafter:\n%s" \
            % ("\n".join(before), "\n".join(after))
        assert DEFERRED_OUTPUT in _read(str(trace)), \
            "the output that came under the dialog should wait:\n%s" \
            % _read(str(trace))
        w.key(ESC, delay=0.6)
        assert _wait_for(w, lambda: _last_printed(w) > last_before + 1,
                         OUTPUT_TIMEOUT_S), \
            "once the dialog closes the output should show again:\n%s" \
            % _screen(w)
        assert REPLAYED_OUTPUT in _read(str(trace)), \
            "the waiting output should be drawn by the key loop:\n%s" \
            % _read(str(trace))
        assert w.alive(), "wpe died closing the dialog"


QUIT_TIMEOUT_S = 10


# @scenario SC-screen-02
def test_program_output_kept_under_the_debug_menu_survives_quitting_from_it(
        tmp_path):
    """The program prints all the time; Alt-D opens the Debug menu and its
    output waits.  Q in that menu ends the session (gdb and the program's
    terminal are gone) before the editor is back in its key loop, where the
    kept output is drawn: the paint looks Messages up again and touches
    nothing of the ended session, and the editor goes on editing."""
    _need("gcc", "gdb")
    trace = tmp_path / TRACE
    with WpeSession(str(tmp_path), PRINTER_PROG, filename="p.c", wait=1.5,
                    env_extra={"XWPE_UI_TRACE": str(trace)}) as w:
        _run_the_printer(w)
        w.key(ALT.DEBUG, delay=0.6)
        w._drain(HOLD_S)
        assert DEFERRED_OUTPUT in _read(str(trace)), \
            "the output that came under the menu should wait:\n%s" \
            % _read(str(trace))
        w.key("q", delay=0.6)
        assert _wait_for(w, lambda: REPLAYED_OUTPUT in _read(str(trace)),
                         QUIT_TIMEOUT_S), \
            "the kept output should be drawn once the menu has closed:\n%s" \
            % _read(str(trace))
        assert w.alive(), "wpe died drawing output kept for an ended session"
        assert _wait_for(w, lambda: not procinfo.children_named(w.proc.pid,
                                                                "gdb"),
                         QUIT_TIMEOUT_S), \
            "Debug > Quit should have ended gdb:\n%s" % _screen(w)
        w.key("Q", delay=0.3)
        w.save()
        assert "Q" in w.text(), "typing after the session should edit the source"


# A program that sleeps ten minutes after its first line; the line after the
# sleep is not reached while the test runs, so a breakpoint there is never a
# stop.  jdb takes a breakpoint while the program runs and answers on its
# own, without stopping the program (gdb's line interface stops it to set
# one, which is the stop's path, not this one).
QUIET_PROG = (
    "public class P {\n"
    "    public static void main(String[] a) throws Exception {\n"
    "        System.out.println(\"ready\");\n"
    "        Thread.sleep(600000);\n"
    "        int x = 3;\n"
    "    }\n}\n"
)
QUIET_BREAK_LINE = 3
QUIET_UNREACHED_LINE = 5
JDB_START_TIMEOUT_S = 30
RUNNING_BAR = "^Brk Pause"
SEARCH_MENU_ROW = "Go to Line"
SOLID_WAIT_S = 6
ANSWER_WAIT_S = 3


def _go_to_line(w, line):
    w.key(ALT.SEARCH, delay=0.5)
    w.key("g", delay=0.6)
    w.key(str(line))
    w.key("\r", delay=0.6)


# @scenario SC-screen-02
def test_a_breakpoint_confirmed_under_a_menu_is_solid_when_the_menu_closes(
        tmp_path):
    """While the program runs, Ctrl-G B on a line and, before jdb has
    answered, Alt-S opens the Search menu (both keys in one write).  jdb's
    word comes while the menu is up; once Esc closes the menu the line is
    the solid breakpoint bar, not the hollow mark of a breakpoint the
    debugger has not confirmed."""
    _need("javac", "jdb")
    trace = tmp_path / TRACE
    with WpeSession(str(tmp_path), QUIET_PROG, filename="P.java", wait=1.5,
                    env_extra={"XWPE_UI_TRACE": str(trace)}) as w:
        _go_to_line(w, QUIET_BREAK_LINE)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: w.mark_line() == QUIET_BREAK_LINE,
                         JDB_START_TIMEOUT_S), \
            "Ctrl-G R should stop on line %d:\n%s" % (QUIET_BREAK_LINE, _screen(w))
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: RUNNING_BAR in w.display(drain=0)[-1],
                         OUTPUT_TIMEOUT_S), \
            "the program should run on:\n%s" % _screen(w)
        _go_to_line(w, QUIET_UNREACHED_LINE)
        w.key(CTRL_G + "b" + ALT.SEARCH, delay=0.2)
        assert _wait_for(w, lambda: SEARCH_MENU_ROW in _screen(w), ANSWER_WAIT_S), \
            "Alt-S should open the Search menu:\n%s" % _screen(w)
        w._drain(ANSWER_WAIT_S)
        w.key(ESC, delay=0.6)
        assert _wait_for(w, lambda: QUIET_UNREACHED_LINE in w.breakpoint_lines(),
                         SOLID_WAIT_S), \
            "line %d should be the solid bar once the menu closes " \
            "(solid %s, hollow %s):\n%s" \
            % (QUIET_UNREACHED_LINE, w.breakpoint_lines(),
               w.unconfirmed_breakpoint_lines(), _screen(w))
        assert QUIET_UNREACHED_LINE not in w.unconfirmed_breakpoint_lines()
        assert "paint deferred a breakpoint's answer" in _read(str(trace)), \
            "jdb's word should have come while the menu was up (else this " \
            "row proved nothing):\n%s" % _read(str(trace))


# ---- the assistant's review box and a language server -------------------

STUB_LOG = "lsp-stub.log"
STUB_PID = "lsp-stub.pid"
STUB_RELEASE = "lsp-stub.release"
LSP_STARTED_S = 20
REVIEW_S = 10
PUBLISHED_S = 10
SEED = "int main(void){return 0;}\n"
REPLY = "int main(void){return 42;}"
LSP_LINES = ("LSP:", "Language server ready", "STUB-ERROR")

# A stand-in clangd: it answers every request at once with nothing, and
# publishes one error for the opened file only when the test writes to its
# release pipe.  It writes its PID and a log of what it did.
FAKE_CLANGD = r'''#!%(python)s -I
import json, os, select, sys
here = %(dir)r
inp, out = sys.stdin.buffer, sys.stdout.buffer

def note(text):
    with open(os.path.join(here, %(log)r), "a") as fh:
        fh.write(text + "\n")

with open(os.path.join(here, %(pid)r), "w") as fh:
    fh.write(str(os.getpid()))
release_path = os.path.join(here, %(release)r)
os.mkfifo(release_path)
release = os.open(release_path, os.O_RDWR | os.O_NONBLOCK)

def send(obj):
    body = json.dumps(obj).encode()
    out.write(b"Content-Length: %%d\r\n\r\n" %% len(body) + body)
    out.flush()

pending = b""
uri = {"doc": ""}

def messages():
    global pending
    got = []
    while True:
        head, sep, rest = pending.partition(b"\r\n\r\n")
        if not sep:
            return got
        length = 0
        for line in head.split(b"\r\n"):
            if line.lower().startswith(b"content-length:"):
                length = int(line.split(b":")[1])
        if len(rest) < length:
            return got
        got.append(json.loads(rest[:length]))
        pending = rest[length:]

def take(msg):
    method = msg.get("method")
    if method is None:
        return True
    if "id" not in msg:
        note("NOTE %%s" %% method)
        if method == "textDocument/didOpen":
            uri["doc"] = msg["params"]["textDocument"]["uri"]
        return method != "exit"
    result = {"capabilities": {}} if method == "initialize" else None
    send({"jsonrpc": "2.0", "id": msg["id"], "result": result})
    return True

def publish():
    send({"jsonrpc": "2.0", "method": "textDocument/publishDiagnostics",
          "params": {"uri": uri["doc"], "diagnostics": [
              {"range": {"start": {"line": 0, "character": 0},
                         "end": {"line": 0, "character": 3}},
               "severity": 1, "message": "STUB-ERROR"}]}})
    note("PUBLISHED")

going = True
while going:
    ready, _, _ = select.select([0, release], [], [])
    if release in ready:
        os.read(release, 64)
        publish()
        continue
    data = os.read(0, 65536)
    if not data:
        break
    pending += data
    for msg in messages():
        going = going and take(msg)
'''


def _write_executable(path, text):
    with open(path, "w") as fh:
        fh.write(text)
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)


def _stand_in_server(tmp_path):
    """The stand-in clangd on PATH, built in the test's own folder."""
    bindir = tmp_path / "bin"
    bindir.mkdir()
    _write_executable(str(bindir / "clangd"),
                      FAKE_CLANGD % {"python": sys.executable,
                                     "dir": str(tmp_path), "log": STUB_LOG,
                                     "pid": STUB_PID, "release": STUB_RELEASE})
    return {"PATH": "%s:%s" % (bindir, os.environ.get("PATH", "")),
            "XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
            "XWPE_AI_MOCK_REPLY": REPLY,
            "XWPE_AI_TRACE": str(tmp_path / "ai.trace"),
            "XWPE_UI_TRACE": str(tmp_path / TRACE)}


def _release_the_stand_in(tmp_path):
    fd = os.open(str(tmp_path / STUB_RELEASE), os.O_WRONLY | os.O_NONBLOCK)
    try:
        os.write(fd, b"go\n")
    finally:
        os.close(fd)


def _end_the_stand_in(tmp_path):
    try:
        pid = int(_read(str(tmp_path / STUB_PID)).strip() or 0)
    except ValueError:
        return
    if pid > 0:
        try:
            os.kill(pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass


def _box_rows(rows):
    """The rows of the review box: those that show the reply's new line."""
    return [i for i, row in enumerate(rows) if "42" in row]


# @scenario SC-screen-03
def test_a_review_box_stays_a_box_after_an_error_box_inside_it(tmp_path):
    """The assistant's review box is up when the language server publishes
    an error for the file.  Nothing is drawn while the box is up: not the
    file's window repainted with its marks over the box, not the
    "LSP: 1 error(s)" line, not "Language server ready" in Messages.  The
    review box owns the screen as every other box does, so a
    box opened and closed inside it (an error box) cannot hand the screen
    back either (the owner's one depth, tests/test_paint.c)."""
    if not assistant_build():
        pytest.skip("wpe built without the assistant")
    env = _stand_in_server(tmp_path)
    try:
        with WpeSession(str(tmp_path), SEED, filename="a.c", wait=1.5,
                        env_extra=env) as w:
            w.key("\033q", delay=0.4)
            w.key("e", delay=0.2)
            assert _wait_for(w, lambda: "NOTE textDocument/didOpen"
                             in _read(str(tmp_path / STUB_LOG)), LSP_STARTED_S), \
                "Alt-Q E should start the stand-in server:\n%s" % _screen(w)
            w._drain(1.0)
            w.key(ALT.AI)
            w.key("e")
            w.key("return 42 instead")
            w.key("\r", delay=1.3)
            assert _wait_for(w, lambda: _box_rows(w.display(drain=0)), REVIEW_S), \
                "the review box should open:\n%s" % _screen(w)
            w._drain(0.8)
            before = w.display(drain=0)
            _release_the_stand_in(tmp_path)
            assert _wait_for(w, lambda: "PUBLISHED" in _read(str(tmp_path / STUB_LOG)),
                             PUBLISHED_S), "the stand-in should have published"
            w._drain(HOLD_S)
            after = w.display(drain=0)
            for text in LSP_LINES:
                assert not any(text in row for row in after), \
                    "%r was drawn while the review box was up:\n%s" \
                    % (text, "\n".join(after))
            assert after == before, \
                "nothing may be drawn under the review box:\nbefore:\n%s\nafter:\n%s" \
                % ("\n".join(before), "\n".join(after))
            w.key("n", delay=0.8)
            assert w.alive(), "wpe died closing the review box"
    finally:
        _end_the_stand_in(tmp_path)
