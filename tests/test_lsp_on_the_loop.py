"""A language server that is slow to answer never holds the editor.

Every Alt-Q request the user meets most (hover, the jumps, completion,
format) goes out at once and is answered on the editor's loop: the editor
takes keys meanwhile, the status row says what it waits for after a quiet
second, Esc gives the answer up, and the answer is shown when it comes,
unless the file changed since it was asked (the server's positions would
point elsewhere).  The start is the same: a server that boots slowly leaves
the editor working, and Esc ends it.

A stand-in `clangd` on PATH plays the server.  It answers at once, except
the methods named in XWPE_STUB_LSP_HOLD, which it keeps until the test
writes to its release pipe; with XWPE_STUB_LSP_APPLY it asks the editor
to apply an edit (workspace/applyEdit) while a held request is out, and
notes the reply.  It writes its PID and a log of what it read, so every
check is on an exact process and on what the server saw.
"""
import os
import signal
import stat
import sys
import time

import procinfo
from wpe_driver import WpeSession, WAIT_SCALE

ALT_Q = "\033q"                   # the language server prefix
ESC = "\033"
ENTER = "\r"
POLL_S = 0.2
STARTED_S = 20                    # the stand-in answers initialize at once
SAID_S = 10
GONE_S = 8                        # the owner's grace, the kill grace, slack
SETTLE_S = 1.5                    # long enough for an answer to be painted
STUB_PID = "lsp-stub.pid"
STUB_LOG = "lsp-stub.log"
STUB_RELEASE = "lsp-stub.release"
RELEASE_LINE = b"go\n"
HOVER_TEXT = "HOVER-TEXT-OK"
COMPLETED = "zzcompleted"
FORMAT_MARK = "/*fmt*/ "
DEF_LINE = 4                      # 0-based: the fifth line of the seed
WAITING_FOR = "waiting for clangd to answer `%s`"
START_WAITING = "waiting for clangd to start"
GAVE_UP = "Gave up waiting for clangd"
START_CANCELLED = "Language server start cancelled."
STALE_LINE = "came after the file changed"
TYPED = "zq"
SEED = ("int add(int a, int b)\n"
        "{\n"
        "  return a + b;\n"
        "}\n"
        "int target_line;\n"
        "int main(void){return add(1, 2);}\n")

FAKE_CLANGD = r'''#!%(python)s -I
import json, os, select, sys
here = %(dir)r
hold = set(m for m in os.environ.get("XWPE_STUB_LSP_HOLD", "").split(",") if m)
apply_edit = os.environ.get("XWPE_STUB_LSP_APPLY") == "1"
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

# The editor may write several messages at once; they are split here, from
# raw reads, so select() never waits while a message sits in a read buffer.
pending = b""

def messages():
    """Every whole message read so far."""
    global pending
    out = []
    while True:
        head, sep, rest = pending.partition(b"\r\n\r\n")
        if not sep:
            return out
        length = 0
        for line in head.split(b"\r\n"):
            if line.lower().startswith(b"content-length:"):
                length = int(line.split(b":")[1])
        if len(rest) < length:
            return out
        out.append(json.loads(rest[:length]))
        pending = rest[length:]

def position(msg):
    return msg.get("params", {}).get("textDocument", {}).get("uri", "")

def result(msg):
    m = msg["method"]
    if m == "initialize":
        return {"capabilities": {}}
    if m == "textDocument/hover":
        return {"contents": {"kind": "plaintext", "value": %(hover)r}}
    if m == "textDocument/definition":
        return [{"uri": position(msg), "range": {
            "start": {"line": %(defline)d, "character": 0},
            "end": {"line": %(defline)d, "character": 1}}}]
    if m == "textDocument/completion":
        return [{"label": %(completed)r, "insertText": %(completed)r}]
    if m == "textDocument/formatting":
        return [{"range": {"start": {"line": 0, "character": 0},
                           "end": {"line": 0, "character": 0}},
                 "newText": %(mark)r}]
    return None

def answer(msg):
    send({"jsonrpc": "2.0", "id": msg["id"], "result": result(msg)})
    note("ANSWERED %%s" %% msg["method"])

def take(msg):
    """Note one message and answer, hold or follow it; False at `exit`."""
    method = msg.get("method")
    if method is None:
        note("REPLY %%s" %% json.dumps(msg.get("result"), sort_keys=True))
        return True
    if "id" not in msg:
        note("NOTE %%s" %% method)
        if method == "textDocument/didOpen":
            send({"jsonrpc": "2.0", "method": "textDocument/publishDiagnostics",
                  "params": {"uri": position(msg), "diagnostics": []}})
        return method != "exit"
    note("REQ %%s" %% method)
    if method in hold:
        held.append(msg)
        if apply_edit:
            send({"jsonrpc": "2.0", "id": 900, "method": "workspace/applyEdit",
                  "params": {"edit": {"changes": {}}}})
        return True
    answer(msg)
    return True

held = []
going = True
while going:
    ready, _, _ = select.select([0, release], [], [])
    if release in ready:
        os.read(release, 64)
        note("RELEASE")
        for msg in held:
            answer(msg)
        held = []
        continue
    data = os.read(0, 65536)
    if not data:
        note("EOF")
        break
    pending += data
    for msg in messages():
        going = going and take(msg)
'''


def _write_executable(path, text):
    with open(path, "w") as fh:
        fh.write(text)
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)


def _screen(w):
    return "\n".join(w.display(drain=0))


def _wait_for(w, predicate, timeout):
    deadline = time.monotonic() + timeout * WAIT_SCALE
    while time.monotonic() < deadline:
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


def _stub_log(tmp_path):
    return _read(str(tmp_path / STUB_LOG))


def _stub_pid(tmp_path):
    try:
        return int(_read(str(tmp_path / STUB_PID)).strip() or 0)
    except ValueError:
        return 0


def _server_env(tmp_path, hold, apply_edit=False):
    bindir = tmp_path / "bin"
    bindir.mkdir()
    _write_executable(str(bindir / "clangd"),
                      FAKE_CLANGD % {"python": sys.executable,
                                     "dir": str(tmp_path),
                                     "log": STUB_LOG, "pid": STUB_PID,
                                     "release": STUB_RELEASE,
                                     "hover": HOVER_TEXT,
                                     "defline": DEF_LINE,
                                     "completed": COMPLETED,
                                     "mark": FORMAT_MARK})
    env = {"PATH": "%s:%s" % (bindir, os.environ.get("PATH", "")),
           "XWPE_STUB_LSP_HOLD": ",".join(hold)}
    if apply_edit:
        env["XWPE_STUB_LSP_APPLY"] = "1"
    return env


def _release_the_stub(tmp_path):
    """Let the stand-in answer what it holds; nothing when it is gone."""
    try:
        fd = os.open(str(tmp_path / STUB_RELEASE), os.O_WRONLY | os.O_NONBLOCK)
    except OSError:
        return
    try:
        os.write(fd, RELEASE_LINE)
    finally:
        os.close(fd)


def _end_the_stub(tmp_path):
    pid = _stub_pid(tmp_path)
    if pid <= 0:
        return
    try:
        os.kill(pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass


def _started(w, tmp_path):
    """Alt-Q E starts the server; wait until it has the document."""
    w.key(ALT_Q, delay=0.4)
    w.key("e", delay=0.1)
    return _wait_for(w, lambda: "NOTE textDocument/didOpen"
                     in _stub_log(tmp_path), STARTED_S)


def _ask(w, tmp_path, letter, method):
    """Alt-Q <letter>; wait until the stand-in has the request."""
    w.key(ALT_Q, delay=0.4)
    w.key(letter, delay=0.1)
    return _wait_for(w, lambda: ("REQ %s" % method) in _stub_log(tmp_path),
                     SAID_S)


def _says_it_waits(w, method):
    return _wait_for(w, lambda: (WAITING_FOR % method) in _screen(w), SAID_S)


def _answered(w, tmp_path, method):
    return _wait_for(w, lambda: ("ANSWERED %s" % method) in _stub_log(tmp_path),
                     SAID_S)


# @scenario SC-debug-89
def test_lsp_start_takes_keys_while_the_server_boots(tmp_path):
    """A server that has not answered `initialize`: the editor takes keys
    (the typed text lands in the file), the status row counts the start
    and names Esc, and Esc ends the server through the owner and says so."""
    try:
        with WpeSession(str(tmp_path), SEED, filename="a.c", wait=1.5,
                        env_extra=_server_env(tmp_path, ["initialize"])) as w:
            w.key(ALT_Q, delay=0.4)
            w.key("e", delay=0.1)
            assert _wait_for(w, lambda: "REQ initialize" in _stub_log(tmp_path),
                             STARTED_S), \
                "Alt-Q E should start the server:\n%s" % _screen(w)
            pid = _stub_pid(tmp_path)
            w.key(TYPED, delay=0.6)
            assert _wait_for(w, lambda: (TYPED + "int add") in _screen(w),
                             SAID_S), \
                "keys typed while the server boots should edit the file:\n%s" \
                % _screen(w)
            assert _wait_for(w, lambda: START_WAITING in _screen(w), SAID_S), \
                "the status row should say the editor waits for the start:\n%s" \
                % _screen(w)
            w.key(ESC, delay=0.6)
            assert _wait_for(w, lambda: START_CANCELLED in _screen(w), SAID_S), \
                "Esc should cancel the start and say so:\n%s" % _screen(w)
            assert _wait_for(w, lambda: procinfo.run_state(pid) == procinfo.GONE,
                             GONE_S), \
                "the server (pid %d) should be ended: %s" \
                % (pid, procinfo.run_state(pid))
            assert w.alive(), "the editor ended"
    finally:
        _release_the_stub(tmp_path)
        _end_the_stub(tmp_path)


# @scenario SC-debug-90
def test_hover_waits_on_the_loop_and_shows_when_answered(tmp_path):
    """Alt-Q H to a server that holds its answer: the status row says what
    the editor waits for (the screen is alive), and the popup shows the
    hover's text once the server answers."""
    method = "textDocument/hover"
    try:
        with WpeSession(str(tmp_path), SEED, filename="a.c", wait=1.5,
                        env_extra=_server_env(tmp_path, [method])) as w:
            assert _started(w, tmp_path), \
                "Alt-Q E should start the server:\n%s" % _screen(w)
            assert _ask(w, tmp_path, "h", method), \
                "Alt-Q H should ask for the hover:\n%s" % _screen(w)
            assert _says_it_waits(w, method), \
                "the status row should name the request waited for:\n%s" \
                % _screen(w)
            _release_the_stub(tmp_path)
            assert _wait_for(w, lambda: HOVER_TEXT in _screen(w), SAID_S), \
                "the hover should show once answered:\n%s" % _screen(w)
            assert w.alive(), "the editor ended"
    finally:
        _release_the_stub(tmp_path)
        _end_the_stub(tmp_path)


# @scenario SC-debug-90
def test_esc_gives_up_a_hover_and_its_late_answer_is_dropped(tmp_path):
    """Esc while the hover is out: the status row says the answer was given
    up, and when the server answers later nothing pops up."""
    method = "textDocument/hover"
    try:
        with WpeSession(str(tmp_path), SEED, filename="a.c", wait=1.5,
                        env_extra=_server_env(tmp_path, [method])) as w:
            assert _started(w, tmp_path), \
                "Alt-Q E should start the server:\n%s" % _screen(w)
            assert _ask(w, tmp_path, "h", method), \
                "Alt-Q H should ask for the hover:\n%s" % _screen(w)
            assert _says_it_waits(w, method), \
                "the status row should name the request waited for:\n%s" \
                % _screen(w)
            w.key(ESC, delay=0.6)
            assert _wait_for(w, lambda: GAVE_UP in _screen(w), SAID_S), \
                "Esc should give the answer up and say so:\n%s" % _screen(w)
            _release_the_stub(tmp_path)
            assert _answered(w, tmp_path, method), \
                "the stand-in should answer once released"
            w._drain(SETTLE_S)
            assert HOVER_TEXT not in _screen(w), \
                "a late answer should not pop up:\n%s" % _screen(w)
            assert w.alive(), "the editor ended"
    finally:
        _release_the_stub(tmp_path)
        _end_the_stub(tmp_path)


# @scenario SC-debug-90
def test_definition_jumps_when_answered(tmp_path):
    """Alt-Q D to a server that holds its answer: the status row says what
    the editor waits for, and the cursor goes to the line the answer names
    once it comes (the next key typed lands there)."""
    method = "textDocument/definition"
    try:
        with WpeSession(str(tmp_path), SEED, filename="a.c", wait=1.5,
                        env_extra=_server_env(tmp_path, [method])) as w:
            assert _started(w, tmp_path), \
                "Alt-Q E should start the server:\n%s" % _screen(w)
            assert _ask(w, tmp_path, "d", method), \
                "Alt-Q D should ask for the definition:\n%s" % _screen(w)
            assert _says_it_waits(w, method), \
                "the status row should name the request waited for:\n%s" \
                % _screen(w)
            _release_the_stub(tmp_path)
            assert _answered(w, tmp_path, method), \
                "the stand-in should answer once released"
            w._drain(SETTLE_S)
            w.key("Q", delay=0.4)
            w.save()
            lines = w.text().split("\n")
            assert lines[DEF_LINE].startswith("Q"), \
                "the cursor should be on line %d after the jump: %r" \
                % (DEF_LINE + 1, lines)
            assert w.alive(), "the editor ended"
    finally:
        _release_the_stub(tmp_path)
        _end_the_stub(tmp_path)


# @scenario SC-debug-90
def test_completion_list_shows_when_answered(tmp_path):
    """Alt-Q C to a server that holds its answer: the status row says what
    the editor waits for, and the list drops down once it comes; Enter
    inserts the candidate."""
    method = "textDocument/completion"
    try:
        with WpeSession(str(tmp_path), SEED, filename="a.c", wait=1.5,
                        env_extra=_server_env(tmp_path, [method])) as w:
            assert _started(w, tmp_path), \
                "Alt-Q E should start the server:\n%s" % _screen(w)
            assert _ask(w, tmp_path, "c", method), \
                "Alt-Q C should ask for completions:\n%s" % _screen(w)
            assert _says_it_waits(w, method), \
                "the status row should name the request waited for:\n%s" \
                % _screen(w)
            _release_the_stub(tmp_path)
            assert _wait_for(w, lambda: COMPLETED in _screen(w), SAID_S), \
                "the list should show once answered:\n%s" % _screen(w)
            w.key(ENTER, delay=0.6)
            w.save()
            assert COMPLETED in w.text(), \
                "Enter should insert the candidate:\n%s" % w.text()
            assert w.alive(), "the editor ended"
    finally:
        _release_the_stub(tmp_path)
        _end_the_stub(tmp_path)


# @scenario SC-debug-90
def test_format_applies_when_answered(tmp_path):
    """Alt-Q F to a server that holds its answer: the status row says what
    the editor waits for, and the buffer takes the server's edit once it
    comes."""
    method = "textDocument/formatting"
    try:
        with WpeSession(str(tmp_path), SEED, filename="a.c", wait=1.5,
                        env_extra=_server_env(tmp_path, [method])) as w:
            assert _started(w, tmp_path), \
                "Alt-Q E should start the server:\n%s" % _screen(w)
            assert _ask(w, tmp_path, "f", method), \
                "Alt-Q F should ask for a format:\n%s" % _screen(w)
            assert _says_it_waits(w, method), \
                "the status row should name the request waited for:\n%s" \
                % _screen(w)
            _release_the_stub(tmp_path)
            assert _wait_for(w, lambda: FORMAT_MARK.strip() in _screen(w),
                             SAID_S), \
                "the format should be applied once answered:\n%s" % _screen(w)
            w.save()
            assert w.text().startswith(FORMAT_MARK + "int add"), \
                "the edit should land at its position:\n%s" % w.text()
            assert w.alive(), "the editor ended"
    finally:
        _release_the_stub(tmp_path)
        _end_the_stub(tmp_path)


# @scenario SC-debug-91
def test_a_late_format_after_typing_is_not_applied(tmp_path):
    """Keys typed while the format is out reach the file at once; the
    server's edits, computed for the text sent, are then not applied to
    the changed text, and the status row says why."""
    method = "textDocument/formatting"
    try:
        with WpeSession(str(tmp_path), SEED, filename="a.c", wait=1.5,
                        env_extra=_server_env(tmp_path, [method])) as w:
            assert _started(w, tmp_path), \
                "Alt-Q E should start the server:\n%s" % _screen(w)
            assert _ask(w, tmp_path, "f", method), \
                "Alt-Q F should ask for a format:\n%s" % _screen(w)
            w.key(TYPED, delay=0.6)
            assert _wait_for(w, lambda: (TYPED + "int add") in _screen(w),
                             SAID_S), \
                "keys typed while the server formats should edit the " \
                "file:\n%s" % _screen(w)
            _release_the_stub(tmp_path)
            assert _answered(w, tmp_path, method), \
                "the stand-in should answer once released"
            assert _wait_for(w, lambda: STALE_LINE in _screen(w), SAID_S), \
                "the status row should say the answer came too late:\n%s" \
                % _screen(w)
            w.save()
            assert FORMAT_MARK.strip() not in w.text(), \
                "a format for the old text must not be applied:\n%s" % w.text()
            assert w.text().startswith(TYPED + "int add"), \
                "the typed keys stay:\n%s" % w.text()
            assert w.alive(), "the editor ended"
    finally:
        _release_the_stub(tmp_path)
        _end_the_stub(tmp_path)


# @scenario SC-debug-92
def test_a_server_request_is_served_while_a_request_is_out(tmp_path):
    """While the hover is out the server asks the editor to apply an edit
    (workspace/applyEdit): the editor answers it at once (declining an edit
    nobody asked for) and keeps waiting on the loop; the hover still shows
    when it comes."""
    method = "textDocument/hover"
    try:
        with WpeSession(str(tmp_path), SEED, filename="a.c", wait=1.5,
                        env_extra=_server_env(tmp_path, [method],
                                              apply_edit=True)) as w:
            assert _started(w, tmp_path), \
                "Alt-Q E should start the server:\n%s" % _screen(w)
            assert _ask(w, tmp_path, "h", method), \
                "Alt-Q H should ask for the hover:\n%s" % _screen(w)
            assert _wait_for(w, lambda: 'REPLY {"applied": false}'
                             in _stub_log(tmp_path), SAID_S), \
                "the server's request should be answered while the hover is " \
                "out:\n%s" % _stub_log(tmp_path)
            assert _says_it_waits(w, method), \
                "the status row should name the request waited for:\n%s" \
                % _screen(w)
            _release_the_stub(tmp_path)
            assert _wait_for(w, lambda: HOVER_TEXT in _screen(w), SAID_S), \
                "the hover should show once answered:\n%s" % _screen(w)
            assert w.alive(), "the editor ended"
    finally:
        _release_the_stub(tmp_path)
        _end_the_stub(tmp_path)
