"""What a request carries when a piece of it is long.

Two builders write a prompt from pieces: the multi-file run's user turn
(ai_plan_launch) and the chat's system prompt (ai_build_system).  Both used
to keep their place with arithmetic done by hand beside each write, and a
long piece broke it:

* a task of over 4000 bytes ended the first write with the buffer's NUL in
  the middle of the task, so the workspace's file list and the current file
  were silently missing from the request;
* a workspace file whose name does not fit the room left was written cut, so
  the model was given a path that names nothing.

The request is observable without any engine change: the Claude CLI backend
writes the flattened request to the child's stdin, and a stub `claude` on PATH
saves it before it answers.  The assertions are on that saved file.
"""
import os
import subprocess

import pytest

from wpe_driver import WpeSession, ALT, WPE_BIN, wait_until

SEED = "int main(void){return 0;}\n"
# A task over the 4096 bytes the first write of the user turn used to have.
TASK_HEAD = "rename the helper in "
TASK_TAIL = "ENDOFTASK"
TASK = TASK_HEAD + "x" * 5000 + TASK_TAIL
REPLY = "@@PLAN-DONE nothing to change"


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _stub_claude_capturing(dir_, capture, reply):
    """A fake `claude` CLI that saves its stdin (the request) to `capture` and
    answers with a success envelope whose result is `reply`."""
    d = os.path.join(dir_, "bin")
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, "claude")
    envelope = ('{"type":"result","subtype":"success","is_error":false,'
                '"result":"%s","session_id":"stub"}' % reply.replace("\n", "\\n"))
    with open(p, "w") as f:
        f.write("#!/bin/sh\n"
                "cat > '" + capture + "'\n"
                "printf '%s' '" + envelope + "'\n")
    os.chmod(p, 0o755)
    return d


def _env(tmp_path, capture):
    bindir = _stub_claude_capturing(str(tmp_path), capture, REPLY)
    return {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "claudecli",
        "XWPE_AI_MODEL": "sonnet",
        "PATH": bindir + ":" + os.environ.get("PATH", ""),
        "XWPE_AI_CONTAIN_PATH_ALLOW": bindir,
        "XWPE_AI_TRACE": str(tmp_path / "f.trace"),
    }


def _request_is_saved(capture):
    """True once the stub has bytes of the request on disk."""
    return os.path.exists(capture) and os.path.getsize(capture) > 0


def _read_saved_request(capture):
    """What the stub saved, or "" when it saved nothing."""
    if not os.path.exists(capture):
        return ""
    with open(capture) as saved:
        return saved.read()


def _wait_for_request(s, capture, timeout=20):
    """Drain the editor until the stub has saved the request, or time runs out.

    The stub is still writing when its file first has bytes in it, so one more
    turn of the owner's drain follows the first sight of it.  What is on disk
    is returned either way: a row that got nothing, or only part of a request,
    says so with the text it did get."""
    if wait_until(s, lambda: _request_is_saved(capture), timeout,
                  what="the request the stub saved"):
        wait_until(s, lambda: _request_is_saved(capture), 1,
                   what="the stub to finish saving the request")
    return _read_saved_request(capture)


# @scenario SC-ai-plan-request-long-task
def test_plan_request_keeps_its_file_list_and_current_file_after_a_long_task(tmp_path):
    capture = str(tmp_path / "request.txt")
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path, capture),
                    filename="t.c") as s:
        s.key(ALT.AI); s.key("f"); s._drain(0.5)
        s.key(TASK); s.key("\r", delay=0.8)
        body = _wait_for_request(s, capture)

    assert body, "no request reached the engine"
    # The whole task is in the request, to its last byte.
    assert TASK_TAIL in body, "the task was cut: " + body[-200:]
    # And what follows the task in the same turn is still there.
    assert "WORKSPACE FILES:" in body, body[-600:]
    assert os.path.join(str(tmp_path), "t.c") in body.split("WORKSPACE FILES:", 1)[1]
    assert "CURRENT FILE (" in body, body[-600:]
    assert SEED.strip() in body.split("CURRENT FILE (", 1)[1]
    # The order is task, file list, current file.
    assert body.index(TASK_TAIL) < body.index("WORKSPACE FILES:") < body.index("CURRENT FILE (")


# The room of the chat's system prompt: AI_CHAT_SYSTEM_BASE plus
# AI_PROJECT_BLOCK_EXTRA plus the bound of the project instructions, which the
# row sets so the room does not depend on the default.
PROJECT_MAX = 1000
PROMPT_ROOM = 8192 + 400 + PROJECT_MAX
# The list stops once less than this is left (AI_PROMPT_WORKSPACE_LINE_ROOM).
LIST_STOP_ROOM = 256
HEADING = "WORKSPACE FILES:\n"
FILES_ADDED_MAX = 59          # the folder's listing takes at most 60 entries


def _chat_request(tmp_path, capture):
    """Ask the chat "hi" in tmp_path and return the request the engine got."""
    if os.path.exists(capture):
        os.remove(capture)
    env = _env(tmp_path, capture)
    env["XWPE_AI_PROJECT_INSTRUCTIONS_MAX"] = str(PROJECT_MAX)
    with WpeSession(str(tmp_path), SEED, env_extra=env, filename="t.c") as s:
        s.key(ALT.AI, delay=0.6); s.key("a", delay=0.6)
        s.key("hi", delay=0.1); s.key("\r", delay=1.0)
        return _wait_for_request(s, capture)


def _listed_paths(body):
    """The paths of the request's WORKSPACE FILES list, in order."""
    listing = body.split(HEADING, 1)[1]
    seen = []
    for line in listing.split("\n"):
        if not line.startswith("  "):
            break
        seen.append(line[2:])
    return seen


def _name_length_that_fills(list_start, first_line, folder_len):
    """A file name length for which the entry that is the last one the list's
    room test lets in does not fit the room it finds.

    The entries of the folder have one length, so where each starts is known
    from where the list starts and from the room: pick the length for which
    the last entry the test admits (it starts below the room less
    LIST_STOP_ROOM) needs more than is left, which is what used to cut a path."""
    for name_len in range(255, 120, -1):
        # Two blanks, the folder, the second "/" the folder's listing adds to
        # a folder that already ends in one, the name, the newline.
        line = 2 + folder_len + 1 + name_len + 1
        for k in range(FILES_ADDED_MAX):
            start = list_start + first_line + k * line
            if start < PROMPT_ROOM - LIST_STOP_ROOM and start + line >= PROMPT_ROOM:
                return name_len
    raise AssertionError("no file name length fills the prompt")


# @scenario SC-ai-chat-request-no-cut-path
def test_chat_request_never_carries_half_a_path(tmp_path):
    capture = str(tmp_path / "request.txt")
    # First ask with the workspace of one file: that tells where the list
    # starts in the prompt and how a path of this folder is written.
    probe = _chat_request(tmp_path, capture)
    assert HEADING in probe, probe[-800:]
    list_start = probe.index(HEADING) + len(HEADING)
    t_path = _listed_paths(probe)[0]
    folder_len = len(t_path) - len("t.c")
    name_len = _name_length_that_fills(list_start, 2 + len(t_path) + 1, folder_len)

    names = []
    for i in range(FILES_ADDED_MAX):
        name = ("g%02d_" % i).ljust(name_len - 2, "a") + ".c"
        (tmp_path / name).write_text("int x%d;\n" % i)
        names.append(name)

    body = _chat_request(tmp_path, capture)
    assert body, "no request reached the engine"
    seen = _listed_paths(body)
    assert len(seen) > 10, "the list did not fill the prompt:\n" + body[-400:]
    # Every listed path names a file of the workspace: none is a prefix of one.
    real = {os.path.join(str(tmp_path), n) for n in names}
    real.add(os.path.join(str(tmp_path), "t.c"))
    for path in seen:
        # The editor joins a folder that ends in "/" with another "/".
        assert os.path.normpath(path) in real, \
            "a cut path went into the request: " + path[-60:]
