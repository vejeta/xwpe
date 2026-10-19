"""The changeset review at its edges: no review under ask, a created file, the
window after a revert, the review while another window is active, and the
pane's words when no checkpoint could be taken.

Each test makes the editor do one thing the contract names and reads what it
did: the trace for what ran, the Messages window for the rows, the file
window's status row for where the cursor is, the pane for what the user was
told.  The trace and the snapshot folder stay outside the workspace, or the
editor's own state would be listed as one of the run's changes.

Scenarios: SC-ai-agent-33, 34, 35, 59, 65.
"""
import os
import re
import shutil
import subprocess

import pytest
from wpe_driver import (ALT, FRAME_HLINE_RUN, WPE_BIN, WpeSession, pane_rows,
                        pane_text_with_history, title_row, wait_screen)
from test_ai_changeset import (CHANGED_LINE, ORIG, TEN_CHANGED, TWENTY, U_ORIG,
                               _git_commit, _git_repo, _trace_text, _wait_trace)


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"AI changeset" in out
    except Exception:
        return False


pytestmark = [
    pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai"),
    pytest.mark.skipif(shutil.which("git") is None, reason="no git"),
]

FOLLOWUP_LINE = "type a follow-up"
WRITE_BOX = " Write "
PROMPT_LINE = "a = keep all"
NEW_FILE = "[AI] new file"
ALT_T = "\033t"
F3 = "\033OR"                    # File -> File-Manager


def _trace_path(tmp_path):
    """The trace's path, outside the workspace: a trace inside it is one of the
    run's own changes (see the note in test_ai_changeset.py)."""
    return tmp_path.parent / (tmp_path.name + ".trace")


def _env(tmp_path, reply, policy="auto"):
    """The mock backend's environment, with the trace and the snapshot folder
    both outside the workspace so the changeset stays the run's own work."""
    return {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_POLICY": policy,
        "XWPE_AI_TRACE": str(_trace_path(tmp_path)),
        "XDG_STATE_HOME": str(tmp_path.parent / (tmp_path.name + ".state")),
    }


def _start_agent(s, task):
    s.key(ALT.AI)
    s.key("g")
    s.key(task)
    s.key("\r", delay=1.0)


def _screen(s):
    return [r.rstrip() for r in s.display(drain=0)]


def _messages(rows):
    """The Messages window's rows, [] when it is not open.  Read through the
    window rather than the screen, because at 80 columns a screen row carries a
    Messages row and a pane row side by side."""
    if title_row(rows, "Messages") < 0:
        return []
    return [r.rstrip() for r in pane_rows(rows, "Messages")]


def _row_for(rows, rel):
    """The Messages row naming `rel` as `<rel>:<line>: ...`, "" when there is none."""
    for r in _messages(rows):
        if r.startswith(rel + ":"):
            return r
    return ""


def _status_lines(rows):
    """Every line:column a status row on screen reports, as (line, column)."""
    out = []
    for r in rows:
        if r.startswith("m"):
            out += [(int(m.group(1)), int(m.group(2)))
                    for m in re.finditer(r"\s(\d+):(\d+)\s", r)]
    return out


def _file_cursor_line(rows, name):
    """The line the status row of window `name` reports, None without one.

    Read below that window's own title, because once the review has opened the
    Messages window has a status row of its own and the first one on screen is
    no longer the file's.
    """
    top = title_row(rows, name)
    if top < 0:
        return None
    for r in rows[top + 1:]:
        if r.startswith("m"):
            m = re.search(r"\s(\d+):(\d+)\s", r)
            return int(m.group(1)) if m else None
    return None


def _open_file(s, name):
    """Open `name` through the File-Manager, the way the user opens a second file."""
    s.key(F3, delay=1.2)
    s.key(name, delay=0.6)
    s.key("\r", delay=1.2)


def _evidence(rows, txt):
    return "Messages:\n%s\ntrace:\n%s" % ("\n".join(_messages(rows)), txt)


# @scenario SC-ai-agent-35
def test_an_approved_write_under_ask_leaves_no_changeset(tmp_path):
    """Policy ask, one write, approved with `y` in its own box.

    No review opens and the write stands as approved.  The three parts are
    read separately: the trace showing the write, the trace carrying no
    `changeset n=` line, and the screen carrying no `=== AI changeset` header.
    """
    reply = "TOOL write_file out.txt\nhello\n@@END@@TURN@@DONE wrote it"
    env = _env(tmp_path, reply, policy="ask")
    trace = _trace_path(tmp_path)
    with WpeSession(str(tmp_path), ORIG, env_extra=env) as s:
        _start_agent(s, "write a file")
        assert wait_screen(s, WRITE_BOX, 30), "\n".join(_screen(s))
        s.key("y", delay=1.2)                       # allow the write in its box
        assert wait_screen(s, FOLLOWUP_LINE, 30), "\n".join(_screen(s))
        rows = _screen(s)
        alive = s.alive()

    txt = _trace_text(trace)
    written = tmp_path / "out.txt"
    assert written.read_text() == "hello\n", \
        "the approved write is not on disk: %r\ntrace:\n%s" % (
            written.read_text() if written.exists() else None, txt)
    assert "agent tool=write_file" in txt, \
        "the trace does not show the write:\n" + txt
    assert "changeset n=" not in txt, \
        "a review opened over writes that were each approved:\n" + txt
    assert "=== AI changeset" not in "\n".join(rows), \
        "Messages carries the review's header:\n" + "\n".join(rows)
    assert alive, "an approved write under ask ended the editor"


def _run_created_file(tmp_path, reply, cols=80, rows=30):
    """Run `reply` under auto in a snapshot workspace, let the review open,
    answer it with `r`.

    Returns (the review's screen rows, the rows after `r`, the trace text,
    whether the review was waiting for a key, whether the editor is alive).
    """
    trace = _trace_path(tmp_path)
    env = _env(tmp_path, reply)
    with WpeSession(str(tmp_path), ORIG, env_extra=env, cols=cols, rows=rows) as s:
        _start_agent(s, "create a file")
        assert _wait_trace(trace, "agent done", 90), \
            "the run never finished:\n" + _trace_text(trace)
        _wait_trace(trace, "changeset", 15)          # the review opens after it
        s._drain(1.0)
        review = _screen(s)
        waiting = any(PROMPT_LINE in r for r in review)
        if waiting:
            s.key("r", delay=1.5)
            _wait_trace(trace, "changeset revert", 30)
        after = _screen(s)
        alive = s.alive()
    return review, after, _trace_text(trace), waiting, alive


# @scenario SC-ai-agent-59
def test_a_file_created_in_a_deeper_directory_is_listed_and_removed(tmp_path):
    """Snapshot mode: the run creates `src/deep/x.c` through a command.

    The snapshot used to watch the root and each open file's directory only,
    so a file created two directories down was neither listed nor removed and
    the summary still read "ALL changes reverted".  The snapshot now walks the
    workspace root as deep as it goes, the set the git mode has always seen,
    so the file is listed as `src/deep/x.c:1: [AI] new file` and `r` removes
    it.  The file is created through `run_command`, which the auto dial
    passes, because `write_file` makes no directories on purpose.
    """
    deep = tmp_path / "src" / "deep" / "x.c"
    reply = ("TOOL run_command mkdir -p src/deep && "
             "echo 'int deep(void){return 1;}' > src/deep/x.c"
             "@@TURN@@DONE created it")
    review, after, txt, waiting, alive = _run_created_file(tmp_path, reply)

    assert waiting, "the review never asked for a key:\n" + _evidence(review, txt)
    row = _row_for(review, "src/deep/x.c")
    assert row == "src/deep/x.c:1: " + NEW_FILE, \
        "Messages shows %r for a file the run created two directories down:\n%s" % (
            row, _evidence(review, txt))
    assert not deep.exists(), \
        "`r` left the created file on disk:\n%s" % _evidence(after, txt)
    assert alive, "reverting a deep created file ended the editor"


# @scenario SC-ai-agent-59
def test_the_editors_own_state_directory_inside_the_workspace_is_not_a_change(tmp_path):
    """The editor's own files under the workspace are not the run's changes.

    With the state directory placed inside the workspace, the snapshot's walk
    would meet its own checkpoint copies and the session file and list them
    as files the run created.  They are the editor's bookkeeping: the review
    lists the one file the run rewrote and nothing under the state directory.
    """
    t = tmp_path / "t.c"
    trace = _trace_path(tmp_path)
    reply = "TOOL write_file %s\n%s@@END@@TURN@@DONE rewrote it" % (t, TEN_CHANGED)
    env = _env(tmp_path, reply)
    env["XDG_STATE_HOME"] = str(tmp_path / "state")     # inside the workspace
    with WpeSession(str(tmp_path), TWENTY, env_extra=env, cols=160, rows=40) as s:
        _start_agent(s, "rewrite it")
        assert _wait_trace(trace, "changeset n=", 90), \
            "the review never opened:\n" + _trace_text(trace)
        s._drain(1.0)
        review = _screen(s)
        s.key("a", delay=0.8)
        alive = s.alive()

    txt = _trace_text(trace)
    rows = [r for r in _messages(review) if ": [AI]" in r]
    assert "changeset n=1" in txt, \
        "the editor's own files were counted as the run's:\n" + _evidence(review, txt)
    assert rows and all(r.startswith("t.c:") for r in rows), \
        "a row names something other than the rewritten file:\n" + _evidence(review, txt)
    assert alive, "a state directory inside the workspace ended the editor"


WATCH_BOUND = 2000
WATCH_BOUND_WORDS = "is unseen"


# @scenario SC-ai-agent-59
def test_a_workspace_beyond_the_watch_bound_is_said_in_the_pane(tmp_path):
    """When the workspace holds more files than the snapshot watches, the pane
    says so, with the count, instead of leaving later new files unseen.

    The review lists a new file by comparing what exists now with what existed
    at the checkpoint; a file beyond the bound is in neither list, so it would
    be kept without ever being shown.  A workspace that fits is told nothing.
    """
    many = tmp_path / "many"
    many.mkdir()
    for i in range(WATCH_BOUND + 50):
        (many / ("f%04d.txt" % i)).write_text("x\n")
    trace = _trace_path(tmp_path)
    reply = "TOOL write_file new.c\nint new(void){return 1;}\n@@END@@TURN@@DONE created it"
    env = _env(tmp_path, reply)
    with WpeSession(str(tmp_path), ORIG, env_extra=env) as s:
        _start_agent(s, "create a file")
        assert _wait_trace(trace, "changeset n=", 90), \
            "the review never opened:\n" + _trace_text(trace)
        s.key("a", delay=0.8)                # the review's loop eats PgUp: answer first
        pane = pane_text_with_history(s, ["the snapshot watches", WATCH_BOUND_WORDS])
        alive = s.alive()

    text = " ".join(" ".join(pane).split())   # the pane wraps the line over rows
    assert "the snapshot watches %d files under %s" % (WATCH_BOUND, tmp_path) in text, \
        "the pane does not say the snapshot's bound was reached, or how many it watches:\n" + text
    assert "a file the run creates beyond them %s" % WATCH_BOUND_WORDS in text, \
        "the pane does not say what the bound costs:\n" + text
    assert alive, "a workspace beyond the bound ended the editor"


# @scenario SC-ai-agent-59
def test_a_file_created_beside_the_open_file_is_listed_and_removed(tmp_path):
    """Snapshot mode: the run creates `new.c` beside the open file.

    The review lists it as `new.c:1: [AI] new file` and `r` removes it.  The
    workspace is not a git tree, so the checkpoint is snapshot copies and
    putting a created file back means unlinking it.
    """
    new = tmp_path / "new.c"
    reply = ("TOOL write_file new.c\nint new(void){return 1;}\n@@END"
             "@@TURN@@DONE created it")
    review, after, txt, waiting, alive = _run_created_file(tmp_path, reply)

    assert waiting, "the review never asked for a key:\n" + _evidence(review, txt)
    assert "changeset n=1" in txt, _evidence(review, txt)
    row = _row_for(review, "new.c")
    assert row == "new.c:1: " + NEW_FILE, \
        "Messages shows %r for the file the run created:\n%s" % (
            row, _evidence(review, txt))
    assert not new.exists(), \
        "`r` left the created file on disk:\n%s" % _evidence(after, txt)
    assert "changeset revert all failed=0" in txt, txt
    assert alive, "reverting a created file ended the editor"


# ---- SC-ai-agent-34: what a revert does to the window -----------------------

FORTY = "\n".join("int l%02d = %d;" % (i, i) for i in range(1, 41)) + "\n"
CURSOR_AT = 30
CHANGED_AT = 35
FORTY_CHANGED = FORTY.replace("int l%02d = %d;" % (CHANGED_AT, CHANGED_AT),
                              "int l35 = 3535;")


def _forty_line_repo(tmp_path):
    _git_repo(tmp_path)
    (tmp_path / "t.c").write_text(FORTY)
    _git_commit(tmp_path, "forty lines")


# @scenario SC-ai-agent-34
def test_a_revert_leaves_the_cursor_and_the_viewport_where_they_were(tmp_path):
    """A forty-line file, the cursor on line 30, the window scrolled to show it.

    The run rewrites line 35, the review opens, `r`.  The file is back to its
    checkpoint content on disk and in the window, and the user's own place in
    the file is kept: the status row still says line 30 and the window still
    shows the lines around it.  The seed for this row was "revert put me back
    at the top of the file".
    """
    _forty_line_repo(tmp_path)
    t = tmp_path / "t.c"
    trace = _trace_path(tmp_path)
    reply = ("TOOL write_file %s\n%s@@END@@TURN@@DONE changed line %d"
             % (t, FORTY_CHANGED, CHANGED_AT))
    env = _env(tmp_path, reply)
    with WpeSession(str(tmp_path), FORTY, env_extra=env, cols=80, rows=30) as s:
        s.key(ALT.SEARCH, "g")                 # Search -> Go to Line
        s.key(str(CURSOR_AT))
        s.key("\r", delay=1.0)
        before = _screen(s)
        _start_agent(s, "change line thirty five")
        assert _wait_trace(trace, "changeset n=", 90), \
            "the review never opened:\n" + _trace_text(trace)
        s.key("r", delay=1.5)                  # revert all
        assert _wait_trace(trace, "changeset revert", 30), \
            "the revert never ran:\n" + _trace_text(trace)
        # the review left Messages active, and only the active window's frame
        # carries line:column, so Window -> Next until the file is active again
        for _ in range(3):
            if _file_cursor_line(_screen(s), "t.c") is not None:
                break
            s.key(ALT.WINDOW, "x", delay=0.8)
        after = _screen(s)
        alive = s.alive()

    txt = _trace_text(trace)
    assert "auto-approve write" in txt, "the run never wrote the file:\n" + txt
    window_before = "\n".join(pane_rows(before, "t.c"))
    assert _file_cursor_line(before, "t.c") == CURSOR_AT, \
        "the test did not start with the cursor on line %d:\n%s" % (
            CURSOR_AT, window_before)
    assert "int l30 = 30;" in window_before, \
        "the window was not scrolled to line %d before the run:\n%s" % (
            CURSOR_AT, window_before)

    assert t.read_text() == FORTY, \
        "the revert did not restore the file:\n" + t.read_text()
    window_after = "\n".join(pane_rows(after, "t.c"))
    at = _file_cursor_line(after, "t.c")
    assert at == CURSOR_AT, \
        "after the revert the status row reads %r, the cursor was on %d:\n%s" % (
            at, CURSOR_AT, window_after)
    assert "int l30 = 30;" in window_after, \
        "after the revert the window no longer shows the lines around %d:\n%s" % (
            CURSOR_AT, window_after)
    assert alive, "reverting ended the editor"


# ---- SC-ai-agent-33: the review while another window is active --------------


def _two_file_repo_twenty(tmp_path):
    """A git workspace with t.c holding twenty lines and u.c holding one."""
    _git_repo(tmp_path)
    (tmp_path / "t.c").write_text(TWENTY)
    (tmp_path / "u.c").write_text(U_ORIG)
    _git_commit(tmp_path, "two files")


# @scenario SC-ai-agent-33
def test_the_review_names_the_changed_file_whatever_window_is_active(tmp_path):
    """Two files open, the run rewrites the first, the second is active.

    The review opens and lists the change as `t.c:10:`, and Alt-T opens the
    changed file at that line, whichever window the user was looking at when
    the run ended.
    """
    _two_file_repo_twenty(tmp_path)
    t = tmp_path / "t.c"
    trace = _trace_path(tmp_path)
    reply = ("TOOL write_file %s\n%s@@END@@TURN@@DONE changed line ten"
             % (t, TEN_CHANGED))
    env = _env(tmp_path, reply)
    with WpeSession(str(tmp_path), TWENTY, env_extra=env, filename="t.c",
                    cols=120, rows=34) as s:
        _open_file(s, "u.c")                   # u.c is the active window now
        opened = _screen(s)
        assert title_row(opened, "u.c") >= 0, \
            "the second file never opened:\n" + "\n".join(opened)
        _start_agent(s, "change line ten of t.c")
        assert _wait_trace(trace, "changeset n=", 90), \
            "the review never opened:\n" + _trace_text(trace)
        review = _screen(s)
        s.key("a", delay=1.2)                  # keep all: the review closes
        closed = _screen(s)
        s.key(ALT_T, delay=1.2)                # the jump the row registers
        jumped = _screen(s)
        alive = s.alive()

    txt = _trace_text(trace)
    assert "changeset n=1" in txt, _evidence(review, txt)
    row = _row_for(review, "t.c")
    assert row == "t.c:%d: [AI] +1 -1" % CHANGED_LINE, \
        "Messages shows %r while the other file was the active window:\n%s" % (
            row, _evidence(review, txt))
    assert CHANGED_LINE not in [l for l, _c in _status_lines(closed)], \
        "the cursor was already on line %d before the jump:\n%s" % (
            CHANGED_LINE, "\n".join(closed))
    assert CHANGED_LINE in [l for l, _c in _status_lines(jumped)], \
        "Alt-T did not open the changed file at line %d:\n%s" % (
            CHANGED_LINE, "\n".join(jumped))
    assert "int l10 = 1010;" in "\n".join(pane_rows(jumped, "t.c")), \
        "Alt-T did not leave the changed file showing its changed line:\n%s" % (
            "\n".join(jumped))
    assert alive, "stepping the changeset ended the editor"


# ---- SC-ai-agent-65: the pane's words when a checkpoint is refused ----------


# @scenario SC-ai-agent-65
def test_a_refused_checkpoint_says_the_folder_and_promises_no_revert(tmp_path):
    """What the user reads when no checkpoint could be taken.

    XDG_STATE_HOME points below a regular file, so the folder the snapshot
    copies would go into cannot be created.  The folder is reported with the
    system's reason, nothing promises a revert, no review opens, and the run's
    write still lands.
    """
    blocker = tmp_path / "blocker"
    blocker.write_text("a regular file, where a directory was needed\n")
    t = tmp_path / "t.c"
    trace = _trace_path(tmp_path)
    reply = ("TOOL write_file %s\nint main(void){return 99;}\n@@END"
             "@@TURN@@DONE rewrote it" % t)
    env = _env(tmp_path, reply)
    env["XDG_STATE_HOME"] = str(blocker / "state")
    with WpeSession(str(tmp_path), ORIG, env_extra=env) as s:
        _start_agent(s, "rewrite it")
        assert _wait_trace(trace, "agent done", 90), \
            "the run never finished:\n" + _trace_text(trace)
        pane = pane_text_with_history(s, ["could not create the folder",
                                          "Not a directory", "revertible"])
        disp = "\n".join(s.display(drain=0))
        alive = s.alive()

    assert "return 99" in t.read_text(), t.read_text()
    text = "\n".join(pane)
    assert "could not create the folder" in text, \
        "the pane does not say the checkpoint folder is missing:\n" + text
    assert "Not a directory" in text, \
        "the pane does not say why it is missing:\n" + text
    assert "revertible" not in text, \
        "the pane still promises a revert that cannot happen:\n" + text
    assert "ALL changes reverted" not in disp, \
        "the screen claims a revert that cannot happen:\n" + disp
    assert "changeset" not in _trace_text(trace), \
        "a review opened over a checkpoint that holds nothing:\n" + _trace_text(trace)
    assert alive, "a checkpoint that could not be saved ended the editor"
