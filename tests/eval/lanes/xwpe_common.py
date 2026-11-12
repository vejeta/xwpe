"""Drive the editor's agent through a terminal, the way a user does.

Both xwpe lanes share this: they differ only in the environment they give the
editor (a scripted backend, or a local model server).  The editor is driven
with ``tests/wpe_driver.WpeSession`` so there is one code path and no product
flag for the harness: Alt-G, ``g``, the prompt, Enter, then watch the trace and
the pane.

The editor is always launched as ``wpe`` (terminal): under the name ``we`` the
programming layer, including the assistant prefix, is off.
"""
import os
import re
import sys
import time

PROMPT = ("Read TASK.md in this folder and do what it says. "
          "Reply DONE with a two-line summary when finished.")

TERMINAL_COLS = 132
TERMINAL_ROWS = 44
# One look at the screen and the trace every POLL_S seconds.
POLL_S = 1.5
# A run that shows an error row ends once the spinner has been gone this many
# looks in a row: the row is the final word, not a message in passing.
IDLE_LOOKS = 3
# How many pages of the pane's history are read back to find a reply's start.
HISTORY_PAGES = 8
# After the end is seen, let the last lines of the answer reach the screen.
SETTLE_S = 2.0
# What the pane shows while the agent is working.
WORKING_MARK = "(Esc cancels)"
STEP_LIMIT_MARK = "reached the step limit"
# "[AI]" rows that inform and do not end a run.
INFO_ROWS = [
    re.compile(r"reached the step limit"),
    re.compile(r"earlier .* not sent"),
    re.compile(r"model .* is not .*; using"),
    re.compile(r"\d+ of \d+ lines sent"),
]
# "[AI unavailable]" is the row of a server that cannot be reached at all.
ERROR_ROW = re.compile(r"\[AI(?: unavailable)?\] (.+)")
# A pane line the editor writes about the run, as opposed to the model's words.
MACHINERY_LINE = re.compile(r"^\[(?:agent|AI|plan)[^\]]*\] ")
# The protocol line that ends a run, at the start of a line of the reply.
DONE_LINE = re.compile(r"^DONE\b")

END_DONE = "done"
END_NO_MARKER = "no-marker"
END_STEP_LIMIT = "step-limit"
END_ERROR = "error"
END_DEADLINE = "deadline"
END_EDITOR_EXIT = "editor-exit"
END_SERVER_UNREACHABLE = "server-unreachable"
END_EXCEPTION = "exception"


class LaneResult:
    """What a lane hands back: how the run ended and what was on record."""

    def __init__(self):
        self.end_reason = None
        self.wall_s = 0.0
        self.trace_text = ""
        self.answer_text = ""
        self.pane_text = ""
        self.notes = []


def import_driver(tests_dir, wpe_bin):
    """Import the terminal driver with the binary under test named first."""
    os.environ["WPE_BIN"] = wpe_bin
    if tests_dir not in sys.path:
        sys.path.insert(0, tests_dir)
    import wpe_driver
    return wpe_driver


def read_text(path):
    try:
        with open(path, errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def error_row(display):
    """The text of an "[AI]" row that is an error, or None."""
    for row in display:
        m = ERROR_ROW.search(row)
        if not m:
            continue
        text = m.group(1)
        if any(p.search(text) for p in INFO_ROWS):
            continue
        return text.strip(" |│")
    return None


def pane_lines(driver, session):
    """The pane's lines as the user reads them; empty if it is closed."""
    try:
        rows = driver.pane_rows(session.display())
        return driver.pane_logical_lines(rows)
    except AssertionError:
        return []


def last_done_index(lines):
    """Index of the last line that starts with DONE, or None."""
    for i in range(len(lines) - 1, -1, -1):
        if DONE_LINE.search(lines[i]):
            return i
    return None


def final_reply(lines):
    """(text, complete): the model's last reply and whether its start is seen.

    The reply is the lines around its DONE line, up to the editor's own lines
    on either side.  An answer is scored on what the model finally said, not on
    a guess it wrote before a tool call earlier in the pane.  Its start is seen
    only when an editor line of its own stands above it: a reply that runs up
    to the first line we hold may have more above, scrolled out of the pane,
    and ``complete`` is then False.  Without a DONE line the run is not done
    anyway, and every line is returned as complete."""
    at = last_done_index(lines)
    if at is None:
        return "\n".join(lines), True
    start = at
    while start > 0 and not MACHINERY_LINE.search(lines[start - 1]):
        start -= 1
    end = at + 1
    while end < len(lines) and not MACHINERY_LINE.search(lines[end]):
        end += 1
    return "\n".join(lines[start:end]), start > 0


def pane_rows_now(driver, session):
    """The pane's rows as drawn now; empty if the pane is closed."""
    try:
        return driver.pane_rows(session.display())
    except AssertionError:
        return []


def read_final_reply(driver, session, result):
    """The final reply, read with the pane's history.

    The pane shows one screen of the transcript.  When the reply's first line
    is not in view, the pane is clicked into and paged up (the driver's own
    way of reading history), up to HISTORY_PAGES pages.  A reply whose start
    still cannot be seen is not scored: the text is empty and ``notes`` says
    so, so a long reply never passes or fails on half its text."""
    rows = pane_rows_now(driver, session)
    text, complete = final_reply(driver.pane_logical_lines(rows))
    pages = 0
    while not complete and pages < HISTORY_PAGES:
        if pages == 0:
            driver.click_into_pane(session)
        session.key(driver.KEY_PGUP, delay=0.4)
        rows = pane_rows_now(driver, session) + rows
        text, complete = final_reply(driver.pane_logical_lines(rows))
        pages += 1
    result.pane_text = "\n".join(driver.pane_logical_lines(rows))
    if not complete:
        result.notes.append("final reply start not visible after %d pages "
                            "of history; answer not scored" % pages)
        return ""
    return text


def build_env(home, trace_path, policy, backend_env):
    """The environment of one run: HOME and the XDG roots under ``home``."""
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_POLICY": policy,
        "XWPE_AI_TRACE": trace_path,
        "HOME": home,
        "XDG_STATE_HOME": os.path.join(home, ".local", "state"),
        "XDG_CONFIG_HOME": os.path.join(home, ".config"),
    }
    env.update(backend_env)
    return env


def start_agent(session, driver):
    """Alt-G, g, the prompt, Enter: the agent starts on TASK.md."""
    session.key(driver.ALT.AI)
    session.key("g")
    session.key(PROMPT)
    session.key("\r", delay=1.6)


def last_pane_line(lines):
    """The last line of the pane that holds text, or "" when none does."""
    for line in reversed(lines):
        if line.strip():
            return line
    return ""


def judge_screen(display, lines, idle_looks):
    """(end_reason, idle_looks) from one look at the screen, or (None, n).

    ``lines`` are the pane's logical lines.  An error counts only when it is
    the pane's last line: an old "[AI]" row still in view while a long command
    runs is not the end of the run."""
    text = "\n".join(display)
    if STEP_LIMIT_MARK in text:
        return END_STEP_LIMIT, idle_looks
    if WORKING_MARK in text:
        return None, 0
    if error_row([last_pane_line(lines)]):
        idle_looks += 1
        if idle_looks >= IDLE_LOOKS:
            return END_ERROR, idle_looks
    return None, idle_looks


def wait_for_end(driver, session, trace_path, deadline_s, result):
    """Look every POLL_S seconds until the run ends; set result.end_reason.

    Every wait ends when the editor has exited, and at the deadline."""
    from score import end_reason_from_trace
    start = time.time()
    idle = 0
    while True:
        display = session.display(drain=POLL_S)
        reason = end_reason_from_trace(read_text(trace_path))
        lines = []
        if reason is None:
            lines = pane_lines(driver, session)
            reason, idle = judge_screen(display, lines, idle)
        if reason is not None:
            result.end_reason = reason
            if reason == END_ERROR:
                result.notes.append("[AI] "
                                    + (error_row([last_pane_line(lines)]) or ""))
            return
        if not session.alive():
            result.end_reason = END_EDITOR_EXIT
            return
        if time.time() - start > deadline_s:
            result.end_reason = END_DEADLINE
            return


def run_agent(driver, repo, home, trace_path, env_extra, deadline_s, task_text):
    """One agent run in the copy ``repo``; returns a LaneResult.

    The editor's folder is the copy, TASK.md is open in it, HOME and the XDG
    roots are under ``home`` so nothing of the copy's score depends on them."""
    result = LaneResult()
    started = time.time()
    session = driver.WpeSession(repo, task_text, filename="TASK.md",
                                cols=TERMINAL_COLS, rows=TERMINAL_ROWS,
                                env_extra=env_extra)
    try:
        start_agent(session, driver)
        wait_for_end(driver, session, trace_path, deadline_s, result)
        session.display(drain=SETTLE_S)
        result.answer_text = read_final_reply(driver, session, result)
    finally:
        session.close()
    result.wall_s = time.time() - started
    result.trace_text = read_text(trace_path)
    return result
