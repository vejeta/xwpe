"""A language server that is slow to answer never holds the editor.

Every Alt-Q request goes out at once and is answered on the editor's loop:
the editor takes keys meanwhile, the status row says what it waits for
after a quiet second, Esc gives the answer up, and the answer is shown when
it comes, unless the file changed since it was asked (the server's
positions would point elsewhere).  A request of another kind goes out while
one is out.  The inlay hints and semantic colours are fetched in the
background: the editor never waits for them, and an answer for text that
has changed since is never painted.  The start is the same: a server that
boots slowly leaves the editor working, and Esc ends it.

A stand-in `clangd` on PATH plays the server.  It answers at once, except
the methods named in XWPE_STUB_LSP_HOLD, which it keeps until the test
writes to its release pipe (each later request of a held method is held
again); with XWPE_STUB_LSP_APPLY it asks the editor to apply an edit
(workspace/applyEdit) while a held request is out, and notes the reply.  A
workspace/executeCommand is answered as a server runs a command: it first
asks the editor to apply its edit, and answers once the editor replied.  It
writes its PID and a log of what it read, so every check is on an exact
process and on what the server saw.
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
WAITED_FOR = "clangd to answer `%s`"
START_WAITING = "waiting for clangd to start"
GAVE_UP = "Gave up waiting for clangd"
START_CANCELLED = "Language server start cancelled."
STALE_LINE = "came after the file changed"
TYPED = "zq"
DOWN = "\033[B"
SIG_TEXT = "SIG-LABEL-OK(int a, int b)"
CALLER = "CALLER-OK"
CALLEE = "CALLEE-OK"
SUPER = "SUPER-OK"
SUB = "SUB-OK"
SYMBOL = "SYMBOL-OK"
WSYM = "WSYM-OK"
LENS = "LENS-OK"
INLAY = "INLAYOK"
RENAMED = "plus"
ACTION_EDIT = "ACTION-EDIT-OK"
ACTION_CMD = "ACTION-CMD-OK"
ACT_MARK = "/*act*/ "
CMD_MARK = "/*cmd*/ "
TEXT_ROW = 2                      # screen row of the seed's first line
HL_LINE = 2                       # 0-based: "  return a + b;", whose "return"
HL_ROW = TEXT_ROW + HL_LINE       # the stand-in highlights and selects
HL_COL = 3                        # screen column of that "r"
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
chatty = os.environ.get("XWPE_STUB_LSP_CHATTY") == "1"
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

doc = {"uri": ""}                 # the file the editor opened
command_out = []                  # an executeCommand waiting for its edit's reply

def span(line, start, end):
    return {"start": {"line": line, "character": start},
            "end": {"line": line, "character": end}}

def item(name, line):
    return {"name": name, "kind": 12, "uri": doc["uri"],
            "range": span(line, 0, 1), "selectionRange": span(line, 0, 1)}

def edit_at_start(text):
    return {"changes": {doc["uri"]: [{"range": span(0, 0, 0), "newText": text}]}}

ANSWERS = {
    "textDocument/signatureHelp": lambda msg: {
        "signatures": [{"label": %(sig)r}], "activeSignature": 0},
    "textDocument/references": lambda msg: [
        {"uri": doc["uri"], "range": span(%(defline)d, 0, 3)},
        {"uri": doc["uri"], "range": span(0, 4, 7)}],
    "textDocument/prepareCallHierarchy": lambda msg: [item("add", 0)],
    "textDocument/prepareTypeHierarchy": lambda msg: [item("add", 0)],
    "callHierarchy/incomingCalls": lambda msg: [
        {"from": item(%(caller)r, 5), "fromRanges": []}],
    "callHierarchy/outgoingCalls": lambda msg: [
        {"to": item(%(callee)r, 0), "fromRanges": []}],
    "typeHierarchy/supertypes": lambda msg: [item(%(super)r, %(defline)d)],
    "typeHierarchy/subtypes": lambda msg: [item(%(sub)r, %(defline)d)],
    "textDocument/documentHighlight": lambda msg: [
        {"range": span(%(hl_line)d, 2, 8), "kind": 1}],
    "textDocument/selectionRange": lambda msg: [
        {"range": span(%(hl_line)d, 2, 8),
         "parent": {"range": span(%(hl_line)d, 0, 15)}}],
    "textDocument/documentSymbol": lambda msg: [
        {"name": %(symbol)r, "kind": 12, "range": span(%(defline)d, 0, 1),
         "selectionRange": span(%(defline)d, 0, 1)}],
    "workspace/symbol": lambda msg: [
        {"name": %(wsym)r, "kind": 12,
         "location": {"uri": doc["uri"], "range": span(%(defline)d, 0, 1)}}],
    "textDocument/codeLens": lambda msg: [{"range": span(0, 0, 1), "data": 1}],
    "codeLens/resolve": lambda msg: {
        "range": span(0, 0, 1),
        "command": {"title": %(lens)r, "command": "stub.lens"}},
    "textDocument/codeAction": lambda msg: [
        {"title": %(action_edit)r, "edit": edit_at_start(%(act_mark)r)},
        {"title": %(action_cmd)r,
         "command": {"title": %(action_cmd)r, "command": "stub.fix",
                     "arguments": []}}],
    "textDocument/rename": lambda msg: {"changes": {doc["uri"]: [
        {"range": span(0, 4, 7), "newText": %(renamed)r}]}},
    "textDocument/inlayHint": lambda msg: [
        {"position": {"line": 0, "character": 2}, "label": %(inlay)r,
         "kind": 1}],
    "textDocument/semanticTokens/full": lambda msg: {"data": [0, 0, 2, 0, 0]},
}

def result(msg):
    m = msg["method"]
    if m == "initialize":
        return {"capabilities": {"semanticTokensProvider": {
            "legend": {"tokenTypes": ["parameter"], "tokenModifiers": []},
            "full": True}}}
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
    if m in ANSWERS:
        return ANSWERS[m](msg)
    return None

def answer(msg):
    if msg["method"] == "workspace/executeCommand":
        # A server runs the command and sends its change back first; the
        # command is answered once the editor has replied.
        command_out.append(msg)
        send({"jsonrpc": "2.0", "id": 901, "method": "workspace/applyEdit",
              "params": {"edit": edit_at_start(%(cmd_mark)r)}})
        note("ASKED workspace/applyEdit")
        return
    if chatty:
        # Servers log as they work (pyright, rust-analyzer, Metals): a
        # notification that says nothing about the file comes with the
        # answer.
        send({"jsonrpc": "2.0", "method": "window/logMessage",
              "params": {"type": 4, "message": "served " + msg["method"]}})
    send({"jsonrpc": "2.0", "id": msg["id"], "result": result(msg)})
    note("ANSWERED %%s" %% msg["method"])

def finish_command():
    """The editor replied to the command's edit: the command is answered."""
    while command_out:
        msg = command_out.pop(0)
        send({"jsonrpc": "2.0", "id": msg["id"], "result": None})
        note("ANSWERED %%s" %% msg["method"])

def take(msg):
    """Note one message and answer, hold or follow it; False at `exit`."""
    method = msg.get("method")
    if method is None:
        note("REPLY %%s" %% json.dumps(msg.get("result"), sort_keys=True))
        finish_command()
        return True
    if "id" not in msg:
        note("NOTE %%s" %% method)
        if method == "textDocument/didOpen":
            doc["uri"] = position(msg)
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


def _server_env(tmp_path, hold, apply_edit=False, chatty=False):
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
                                     "mark": FORMAT_MARK,
                                     "sig": SIG_TEXT, "caller": CALLER,
                                     "callee": CALLEE, "super": SUPER,
                                     "sub": SUB, "symbol": SYMBOL,
                                     "wsym": WSYM, "lens": LENS,
                                     "inlay": INLAY, "renamed": RENAMED,
                                     "action_edit": ACTION_EDIT,
                                     "action_cmd": ACTION_CMD,
                                     "act_mark": ACT_MARK,
                                     "cmd_mark": CMD_MARK,
                                     "hl_line": HL_LINE})
    env = {"PATH": "%s:%s" % (bindir, os.environ.get("PATH", "")),
           "XWPE_STUB_LSP_HOLD": ",".join(hold)}
    if apply_edit:
        env["XWPE_STUB_LSP_APPLY"] = "1"
    if chatty:
        env["XWPE_STUB_LSP_CHATTY"] = "1"
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


def _names_the_wait(w, method):
    """The status row names the server and the request it waits for; with
    a long method name the start of the line is cut on an 80-column row."""
    return _wait_for(w, lambda: (WAITED_FOR % method) in _screen(w), SAID_S)


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


def _ask_through_dialog(w, tmp_path, letter, method):
    """Alt-Q <letter>, whose dialog (a name, a query) is taken as offered
    with Enter; wait until the stand-in has the request."""
    w.key(ALT_Q, delay=0.4)
    w.key(letter, delay=0.8)
    w.key(ENTER, delay=0.1)
    return _wait_for(w, lambda: ("REQ %s" % method) in _stub_log(tmp_path),
                     SAID_S)


def _asks(w, tmp_path, letter, method, dialog):
    """Ask with Alt-Q <letter>, through its dialog when it has one."""
    if dialog:
        return _ask_through_dialog(w, tmp_path, letter, method)
    return _ask(w, tmp_path, letter, method)


def _held_answer_shows(tmp_path, letter, method, shown, dialog=False,
                       close=None):
    """Alt-Q <letter> to a server that holds `method`: the status row names
    the request while the editor waits on its loop, and once the server
    answers, `shown(w)` holds; `close` is typed afterwards (a picker's Esc)."""
    try:
        with WpeSession(str(tmp_path), SEED, filename="a.c", wait=1.5,
                        env_extra=_server_env(tmp_path, [method])) as w:
            assert _started(w, tmp_path), \
                "Alt-Q E should start the server:\n%s" % _screen(w)
            assert _asks(w, tmp_path, letter, method, dialog), \
                "Alt-Q %s should ask for %s:\n%s" % (letter.upper(), method,
                                                     _screen(w))
            assert _names_the_wait(w, method), \
                "the status row should name the request waited for:\n%s" \
                % _screen(w)
            _release_the_stub(tmp_path)
            assert _wait_for(w, lambda: shown(w), SAID_S), \
                "the answer to %s should show once answered:\n%s" \
                % (method, _screen(w))
            if close:
                w.key(close, delay=0.6)
            assert w.alive(), "the editor ended"
    finally:
        _release_the_stub(tmp_path)
        _end_the_stub(tmp_path)


def _on_screen(text):
    """A predicate: `text` is on the screen."""
    return lambda w: text in _screen(w)


def _colours(w):
    """The colours of the "r" of the seed's "return"."""
    cell = w.screen.buffer[HL_ROW][HL_COL]
    return (cell.fg, cell.bg)


def _painted(before):
    """A predicate: the "r" of "return" is painted otherwise than `before`
    (the highlight's colour, or the grown mark)."""
    return lambda w: _colours(w) != before


def _colours_now(tmp_path):
    """The colours of the "r" of "return" on a fresh editor, which a
    highlight or a mark must change."""
    plain = tmp_path / "plain"
    plain.mkdir()
    with WpeSession(str(plain), SEED, filename="a.c", wait=1.5) as w:
        w._drain(SETTLE_S)
        return _colours(w)


# @scenario SC-debug-95
def test_signature_help_waits_on_the_loop_and_shows_when_answered(tmp_path):
    """Alt-Q S to a server that holds its answer: the editor waits on its
    loop and says for what, and the signature pops up once it comes."""
    _held_answer_shows(tmp_path, "s", "textDocument/signatureHelp",
                       _on_screen(SIG_TEXT))


# @scenario SC-debug-95
def test_references_wait_on_the_loop_and_list_when_answered(tmp_path):
    """Alt-Q R: the references are listed in Messages once they come."""
    _held_answer_shows(tmp_path, "r", "textDocument/references",
                       _on_screen("2 reference(s)"))


# @scenario SC-debug-95
def test_callers_wait_on_the_loop_and_list_when_answered(tmp_path):
    """Alt-Q B: the prepare and the incoming calls (two requests, one after
    the other) end in the callers' list."""
    _held_answer_shows(tmp_path, "b", "textDocument/prepareCallHierarchy",
                       _on_screen(CALLER))


# @scenario SC-debug-95
def test_callees_wait_on_the_loop_and_list_when_answered(tmp_path):
    """Alt-Q G: the second step of the call hierarchy is held this time,
    and the callees are listed once it comes."""
    _held_answer_shows(tmp_path, "g", "callHierarchy/outgoingCalls",
                       _on_screen(CALLEE))


# @scenario SC-debug-95
def test_supertypes_wait_on_the_loop_and_list_when_answered(tmp_path):
    """Alt-Q K: the supertypes are listed once they come."""
    _held_answer_shows(tmp_path, "k", "textDocument/prepareTypeHierarchy",
                       _on_screen(SUPER))


# @scenario SC-debug-95
def test_subtypes_wait_on_the_loop_and_list_when_answered(tmp_path):
    """Alt-Q J: the subtypes are listed once they come."""
    _held_answer_shows(tmp_path, "j", "typeHierarchy/subtypes",
                       _on_screen(SUB))


# @scenario SC-debug-95
def test_outline_waits_on_the_loop_and_picks_when_answered(tmp_path):
    """Alt-Q O: the file's symbols drop down in a picker once they come."""
    _held_answer_shows(tmp_path, "o", "textDocument/documentSymbol",
                       _on_screen(SYMBOL), close=ESC)


# @scenario SC-debug-95
def test_workspace_symbols_wait_on_the_loop_and_pick_when_answered(tmp_path):
    """Alt-Q W: the query is asked first, and the matching symbols drop down
    in a picker once they come."""
    _held_answer_shows(tmp_path, "w", "workspace/symbol", _on_screen(WSYM),
                       dialog=True, close=ESC)


# @scenario SC-debug-95
def test_code_lenses_wait_on_the_loop_and_pick_when_answered(tmp_path):
    """Alt-Q L: the lenses come unresolved, each is resolved in turn on the
    loop, and the picker shows their titles."""
    _held_answer_shows(tmp_path, "l", "textDocument/codeLens",
                       _on_screen(LENS), close=ESC)


# @scenario SC-debug-95
def test_highlight_waits_on_the_loop_and_paints_when_answered(tmp_path):
    """Alt-Q U: every occurrence is painted once the server answers."""
    before = _colours_now(tmp_path)
    _held_answer_shows(tmp_path, "u", "textDocument/documentHighlight",
                       _painted(before))


# @scenario SC-debug-95
def test_expand_selection_waits_on_the_loop_and_marks_when_answered(tmp_path):
    """Alt-Q V: the mark grows to the server's tightest range once it
    comes."""
    before = _colours_now(tmp_path)
    _held_answer_shows(tmp_path, "v", "textDocument/selectionRange",
                       _painted(before))


# @scenario SC-debug-95
def test_rename_applies_when_answered(tmp_path):
    """Alt-Q N: the name is asked first; the buffer takes the rename once
    the server answers."""
    method = "textDocument/rename"
    try:
        with WpeSession(str(tmp_path), SEED, filename="a.c", wait=1.5,
                        env_extra=_server_env(tmp_path, [method])) as w:
            assert _started(w, tmp_path), \
                "Alt-Q E should start the server:\n%s" % _screen(w)
            assert _ask_through_dialog(w, tmp_path, "n", method), \
                "Alt-Q N should ask for the rename:\n%s" % _screen(w)
            assert _names_the_wait(w, method), \
                "the status row should name the request waited for:\n%s" \
                % _screen(w)
            _release_the_stub(tmp_path)
            assert _wait_for(w, lambda: ("int %s(" % RENAMED) in _screen(w),
                             SAID_S), \
                "the rename should be applied once answered:\n%s" % _screen(w)
            w.save()
            assert w.text().startswith("int %s(int a" % RENAMED), \
                "the rename lands where the server said:\n%s" % w.text()
            assert w.alive(), "the editor ended"
    finally:
        _release_the_stub(tmp_path)
        _end_the_stub(tmp_path)


# @scenario SC-debug-95
def test_code_action_list_waits_on_the_loop_and_its_edit_applies(tmp_path):
    """Alt-Q A: the actions drop down once they come; Enter applies the
    first, whose edit came with the list."""
    method = "textDocument/codeAction"
    try:
        with WpeSession(str(tmp_path), SEED, filename="a.c", wait=1.5,
                        env_extra=_server_env(tmp_path, [method])) as w:
            assert _started(w, tmp_path), \
                "Alt-Q E should start the server:\n%s" % _screen(w)
            assert _ask(w, tmp_path, "a", method), \
                "Alt-Q A should ask for code actions:\n%s" % _screen(w)
            assert _names_the_wait(w, method), \
                "the status row should name the request waited for:\n%s" \
                % _screen(w)
            _release_the_stub(tmp_path)
            assert _wait_for(w, lambda: ACTION_EDIT in _screen(w), SAID_S), \
                "the actions should drop down once answered:\n%s" % _screen(w)
            w.key(ENTER, delay=0.8)
            w.save()
            assert w.text().startswith(ACT_MARK + "int add"), \
                "the chosen action's edit should be applied:\n%s" % w.text()
            assert w.alive(), "the editor ended"
    finally:
        _release_the_stub(tmp_path)
        _end_the_stub(tmp_path)


def _run_the_command_action(w, tmp_path):
    """Alt-Q A, then the second action (a command): wait until the stand-in
    has the workspace/executeCommand."""
    assert _ask(w, tmp_path, "a", "textDocument/codeAction"), \
        "Alt-Q A should ask for code actions:\n%s" % _screen(w)
    assert _wait_for(w, lambda: ACTION_CMD in _screen(w), SAID_S), \
        "the actions should drop down:\n%s" % _screen(w)
    w.key(DOWN, delay=0.4)
    w.key(ENTER, delay=0.1)
    return _wait_for(w, lambda: "REQ workspace/executeCommand"
                     in _stub_log(tmp_path), SAID_S)


# @scenario SC-debug-96
def test_a_command_action_applies_the_edit_the_server_sends_back(tmp_path):
    """A command-based action runs as workspace/executeCommand on the loop;
    the edit the server sends back (workspace/applyEdit) is accepted while
    the command is out and lands in the buffer once the command answers."""
    method = "workspace/executeCommand"
    try:
        with WpeSession(str(tmp_path), SEED, filename="a.c", wait=1.5,
                        env_extra=_server_env(tmp_path, [method])) as w:
            assert _started(w, tmp_path), \
                "Alt-Q E should start the server:\n%s" % _screen(w)
            assert _run_the_command_action(w, tmp_path), \
                "choosing the command should run it:\n%s" % _screen(w)
            assert _names_the_wait(w, method), \
                "the status row should name the command waited for:\n%s" \
                % _screen(w)
            _release_the_stub(tmp_path)
            assert _wait_for(w, lambda: 'REPLY {"applied": true}'
                             in _stub_log(tmp_path), SAID_S), \
                "the command's edit should be accepted:\n%s" \
                % _stub_log(tmp_path)
            assert _wait_for(w, lambda: CMD_MARK.strip() in _screen(w),
                             SAID_S), \
                "the command's edit should land once it answers:\n%s" \
                % _screen(w)
            w.save()
            assert w.text().startswith(CMD_MARK + "int add"), \
                "the edit lands where the server said:\n%s" % w.text()
            assert w.alive(), "the editor ended"
    finally:
        _release_the_stub(tmp_path)
        _end_the_stub(tmp_path)


# @scenario SC-debug-97
def test_a_late_command_edit_after_typing_is_not_applied(tmp_path):
    """Keys typed while the command runs reach the file at once; the edit
    the server sends for the text the command was run on is then not
    applied to the changed text, and the status row says why."""
    method = "workspace/executeCommand"
    try:
        with WpeSession(str(tmp_path), SEED, filename="a.c", wait=1.5,
                        env_extra=_server_env(tmp_path, [method])) as w:
            assert _started(w, tmp_path), \
                "Alt-Q E should start the server:\n%s" % _screen(w)
            assert _run_the_command_action(w, tmp_path), \
                "choosing the command should run it:\n%s" % _screen(w)
            w.key(TYPED, delay=0.6)
            assert _wait_for(w, lambda: (TYPED + "int add") in _screen(w),
                             SAID_S), \
                "keys typed while the command runs should edit the file:\n%s" \
                % _screen(w)
            _release_the_stub(tmp_path)
            assert _answered(w, tmp_path, method), \
                "the stand-in should answer once released"
            assert _wait_for(w, lambda: STALE_LINE in _screen(w), SAID_S), \
                "the status row should say the edit came too late:\n%s" \
                % _screen(w)
            w.save()
            assert CMD_MARK.strip() not in w.text(), \
                "an edit for the old text must not be applied:\n%s" % w.text()
            assert w.text().startswith(TYPED + "int add"), \
                "the typed keys stay:\n%s" % w.text()
            assert w.alive(), "the editor ended"
    finally:
        _release_the_stub(tmp_path)
        _end_the_stub(tmp_path)


# @scenario SC-debug-97
def test_a_late_rename_after_typing_is_not_applied(tmp_path):
    """Keys typed while the rename is out reach the file at once; the
    rename, computed for the text sent, is then not applied."""
    method = "textDocument/rename"
    try:
        with WpeSession(str(tmp_path), SEED, filename="a.c", wait=1.5,
                        env_extra=_server_env(tmp_path, [method])) as w:
            assert _started(w, tmp_path), \
                "Alt-Q E should start the server:\n%s" % _screen(w)
            assert _ask_through_dialog(w, tmp_path, "n", method), \
                "Alt-Q N should ask for the rename:\n%s" % _screen(w)
            w.key(TYPED, delay=0.6)
            assert _wait_for(w, lambda: (TYPED + "int add") in _screen(w),
                             SAID_S), \
                "keys typed while the rename is out should edit the file:\n%s" \
                % _screen(w)
            _release_the_stub(tmp_path)
            assert _answered(w, tmp_path, method), \
                "the stand-in should answer once released"
            assert _wait_for(w, lambda: STALE_LINE in _screen(w), SAID_S), \
                "the status row should say the rename came too late:\n%s" \
                % _screen(w)
            w.save()
            assert RENAMED + "(" not in w.text(), \
                "a rename for the old text must not be applied:\n%s" % w.text()
            assert w.text().startswith(TYPED + "int add"), \
                "the typed keys stay:\n%s" % w.text()
            assert w.alive(), "the editor ended"
    finally:
        _release_the_stub(tmp_path)
        _end_the_stub(tmp_path)


# @scenario SC-debug-97
def test_a_late_list_after_typing_is_dropped(tmp_path):
    """Keys typed while the references are out reach the file at once; the
    list, whose positions are for the text sent, is dropped with a line."""
    method = "textDocument/references"
    try:
        with WpeSession(str(tmp_path), SEED, filename="a.c", wait=1.5,
                        env_extra=_server_env(tmp_path, [method])) as w:
            assert _started(w, tmp_path), \
                "Alt-Q E should start the server:\n%s" % _screen(w)
            assert _ask(w, tmp_path, "r", method), \
                "Alt-Q R should ask for the references:\n%s" % _screen(w)
            w.key(TYPED, delay=0.6)
            assert _wait_for(w, lambda: (TYPED + "int add") in _screen(w),
                             SAID_S), \
                "keys typed while the references are out should edit the " \
                "file:\n%s" % _screen(w)
            _release_the_stub(tmp_path)
            assert _answered(w, tmp_path, method), \
                "the stand-in should answer once released"
            assert _wait_for(w, lambda: STALE_LINE in _screen(w), SAID_S), \
                "the status row should say the list came too late:\n%s" \
                % _screen(w)
            assert "reference(s)" not in _screen(w), \
                "a list for the old text must not be shown:\n%s" % _screen(w)
            assert w.alive(), "the editor ended"
    finally:
        _release_the_stub(tmp_path)
        _end_the_stub(tmp_path)


# @scenario SC-debug-98
def test_esc_gives_up_a_list_and_its_late_answer_is_dropped(tmp_path):
    """Esc while the references are out: the status row says the answer was
    given up, and when the server answers later nothing is listed."""
    method = "textDocument/references"
    try:
        with WpeSession(str(tmp_path), SEED, filename="a.c", wait=1.5,
                        env_extra=_server_env(tmp_path, [method])) as w:
            assert _started(w, tmp_path), \
                "Alt-Q E should start the server:\n%s" % _screen(w)
            assert _ask(w, tmp_path, "r", method), \
                "Alt-Q R should ask for the references:\n%s" % _screen(w)
            assert _names_the_wait(w, method), \
                "the status row should name the request waited for:\n%s" \
                % _screen(w)
            w.key(ESC, delay=0.6)
            assert _wait_for(w, lambda: GAVE_UP in _screen(w), SAID_S), \
                "Esc should give the answer up and say so:\n%s" % _screen(w)
            _release_the_stub(tmp_path)
            assert _answered(w, tmp_path, method), \
                "the stand-in should answer once released"
            w._drain(SETTLE_S)
            assert "reference(s)" not in _screen(w), \
                "a late answer should not be listed:\n%s" % _screen(w)
            assert w.alive(), "the editor ended"
    finally:
        _release_the_stub(tmp_path)
        _end_the_stub(tmp_path)


# @scenario SC-debug-99
def test_a_request_of_another_kind_is_served_while_one_is_out(tmp_path):
    """While the hover is out, the outline (another kind) is asked and its
    picker shows at once; the same action again is refused (the status row
    names the hover still out, and the server is not asked twice); the
    hover still shows when it comes."""
    method = "textDocument/hover"
    try:
        with WpeSession(str(tmp_path), SEED, filename="a.c", wait=1.5,
                        env_extra=_server_env(tmp_path, [method])) as w:
            assert _started(w, tmp_path), \
                "Alt-Q E should start the server:\n%s" % _screen(w)
            assert _ask(w, tmp_path, "h", method), \
                "Alt-Q H should ask for the hover:\n%s" % _screen(w)
            assert _ask(w, tmp_path, "o", "textDocument/documentSymbol"), \
                "Alt-Q O should go out while the hover is out:\n%s" \
                % _screen(w)
            assert _wait_for(w, lambda: SYMBOL in _screen(w), SAID_S), \
                "the outline should show while the hover is out:\n%s" \
                % _screen(w)
            w.key(ESC, delay=0.6)
            w.key(ALT_Q, delay=0.4)
            w.key("h", delay=0.6)
            w._drain(SETTLE_S)
            assert _names_the_wait(w, method), \
                "the status row should name the hover still out:\n%s" \
                % _screen(w)
            assert _stub_log(tmp_path).count("REQ %s" % method) == 1, \
                "the refused hover must not reach the server:\n%s" \
                % _stub_log(tmp_path)
            _release_the_stub(tmp_path)
            assert _wait_for(w, lambda: HOVER_TEXT in _screen(w), SAID_S), \
                "the hover should show once answered:\n%s" % _screen(w)
            assert w.alive(), "the editor ended"
    finally:
        _release_the_stub(tmp_path)
        _end_the_stub(tmp_path)


def _fetches(tmp_path, method):
    """How many times the stand-in was asked for `method`."""
    return _stub_log(tmp_path).count("REQ %s" % method)


def _type_a_new_line(w, tmp_path):
    """Type keys and Enter: the keys land at once, and Enter hands the
    server the changed text, so an overlay fetched before is for other
    text."""
    w.key(TYPED, delay=0.3)
    assert _wait_for(w, lambda: (TYPED + "int add") in _screen(w), SAID_S), \
        "keys typed while an overlay is fetched should edit the file at " \
        "once:\n%s" % _screen(w)
    w.key(ENTER, delay=0.3)
    assert _wait_for(w, lambda: "NOTE textDocument/didChange"
                     in _stub_log(tmp_path), SAID_S), \
        "Enter should hand the server the changed text:\n%s" \
        % _stub_log(tmp_path)


def _stale_overlay_is_refetched(w, tmp_path, method):
    """Release the held fetch: its answer is for the text before Enter, so
    the editor asks again."""
    _release_the_stub(tmp_path)
    assert _answered(w, tmp_path, method), \
        "the stand-in should answer once released"
    assert _wait_for(w, lambda: _fetches(tmp_path, method) == 2, SAID_S), \
        "a stale answer should be fetched again:\n%s" % _stub_log(tmp_path)


# @scenario SC-debug-100
def test_inlay_hints_never_hold_the_editor_and_a_stale_answer_never_paints(
        tmp_path):
    """Alt-Q Y to a server that holds the hints: the editor goes on taking
    keys; the answer, for the text before the keys, is dropped without
    being painted and fetched again, and the new answer is painted."""
    method = "textDocument/inlayHint"
    try:
        with WpeSession(str(tmp_path), SEED, filename="a.c", wait=1.5,
                        env_extra=_server_env(tmp_path, [method])) as w:
            assert _started(w, tmp_path), \
                "Alt-Q E should start the server:\n%s" % _screen(w)
            assert _ask(w, tmp_path, "y", method), \
                "Alt-Q Y should fetch the hints:\n%s" % _screen(w)
            _type_a_new_line(w, tmp_path)
            _stale_overlay_is_refetched(w, tmp_path, method)
            assert INLAY not in _screen(w), \
                "hints for the old text must not be painted:\n%s" % _screen(w)
            _release_the_stub(tmp_path)
            assert _wait_for(w, lambda: INLAY in _screen(w), SAID_S), \
                "the hints for the text as it is should be painted:\n%s" \
                % _screen(w)
            assert w.alive(), "the editor ended"
    finally:
        _release_the_stub(tmp_path)
        _end_the_stub(tmp_path)


def _first_cell_fg(w):
    """The foreground of the first cell of the file's first line."""
    w._drain(POLL_S)
    return w.screen.buffer[TEXT_ROW][1].fg


# @scenario SC-debug-100
def test_semantic_colours_never_hold_the_editor_and_a_stale_answer_never_paints(
        tmp_path):
    """Alt-Q M to a server that holds the tokens: the editor goes on taking
    keys; the answer, for the text before the keys, does not recolour the
    file and is fetched again, and the new answer recolours it."""
    method = "textDocument/semanticTokens/full"
    try:
        with WpeSession(str(tmp_path), SEED, filename="a.c", wait=1.5,
                        env_extra=_server_env(tmp_path, [method])) as w:
            assert _started(w, tmp_path), \
                "Alt-Q E should start the server:\n%s" % _screen(w)
            assert _ask(w, tmp_path, "m", method), \
                "Alt-Q M should fetch the tokens:\n%s" % _screen(w)
            _type_a_new_line(w, tmp_path)
            plain = _first_cell_fg(w)
            _stale_overlay_is_refetched(w, tmp_path, method)
            w._drain(SETTLE_S)
            assert _first_cell_fg(w) == plain, \
                "tokens for the old text must not recolour the file"
            _release_the_stub(tmp_path)
            assert _wait_for(w, lambda: _first_cell_fg(w) != plain, SAID_S), \
                "the tokens for the text as it is should recolour it (still " \
                "%r)" % plain
            assert w.alive(), "the editor ended"
    finally:
        _release_the_stub(tmp_path)
        _end_the_stub(tmp_path)


OVERLAY_METHODS = ("textDocument/inlayHint", "textDocument/semanticTokens/full")
# Fetches an overlay may take for one toggle and the news that follows it:
# the toggle's own, and one more for the first diagnostics.
OVERLAY_FETCHES_MAX = 2
QUIET_S = 3                       # an idle editor asks nothing in this time
INPUT_LOST = "stopped reading its input"


def _overlay_fetches(tmp_path):
    return sum(_fetches(tmp_path, m) for m in OVERLAY_METHODS)


# @scenario SC-debug-100
def test_a_server_that_logs_as_it_answers_is_not_asked_again_and_again(
        tmp_path):
    """A server that sends a log line with every answer (as pyright,
    rust-analyzer and Metals do): the overlays are painted, and the log
    line their own fetch provoked is not taken for news, so an idle editor
    stops asking."""
    try:
        with WpeSession(str(tmp_path), SEED, filename="a.c", wait=1.5,
                        env_extra=_server_env(tmp_path, [],
                                              chatty=True)) as w:
            assert _started(w, tmp_path), \
                "Alt-Q E should start the server:\n%s" % _screen(w)
            assert _ask(w, tmp_path, "y", OVERLAY_METHODS[0]), \
                "Alt-Q Y should fetch the hints:\n%s" % _screen(w)
            assert _ask(w, tmp_path, "m", OVERLAY_METHODS[1]), \
                "Alt-Q M should fetch the tokens:\n%s" % _screen(w)
            assert _wait_for(w, lambda: INLAY in _screen(w), SAID_S), \
                "the hints should be painted:\n%s" % _screen(w)
            w._drain(SETTLE_S)
            asked = _overlay_fetches(tmp_path)
            w._drain(QUIET_S)
            assert _overlay_fetches(tmp_path) == asked, \
                "an idle editor should stop fetching the overlays (%d, then " \
                "%d)" % (asked, _overlay_fetches(tmp_path))
            assert asked <= OVERLAY_FETCHES_MAX * len(OVERLAY_METHODS), \
                "each overlay should be fetched about once per toggle, not " \
                "%d times in all" % asked
            assert INPUT_LOST not in _screen(w), \
                "the server should never be flooded:\n%s" % _screen(w)
            assert w.alive(), "the editor ended"
    finally:
        _end_the_stub(tmp_path)
