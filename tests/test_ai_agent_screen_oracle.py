"""The screen invariant that must hold at every drain of an agent run.

Whatever the pane is doing -- a tool box, a review, the Alt-G y dial flash --
the chrome around it must not come apart: row 0 stays the menu bar, and the
bottom key-hint bar ("F1 Help ...") appears on exactly one row, the last
one. A second copy of it anywhere else means some repaint left stale rows
on screen. The invariant is checked after every single drain across two
long, varied runs (boxes, a reload, a denial, the dial flash, and a full
auto review), not just at the end, because a one-frame glitch between two
screens the test never samples is a bug the test would otherwise miss.
"""
import os
import subprocess
import time

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

SEED = "int main(void){return 0;}\n"
ESC = "\033"
MENU_BAR = "  #   File  Edit  Search  Block   Run   Debug  Project  Options  Window   Help"
BAR_WORD = "F1 Help"


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"autonomous coding agent" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _env(tmp_path, reply, policy, **more):
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_POLICY": policy,
        "XWPE_AI_TRACE": str(tmp_path / "ai.trace"),
    }
    env.update(more)
    return env


def _trace(tmp_path):
    t = tmp_path / "ai.trace"
    return t.read_text() if t.exists() else ""


def _screen(s, drain=0.2):
    return "\n".join(s.display(drain=drain))


def screen_invariant_violations(rows, label):
    """The one oracle every screen of an agent run must pass: row 0 is the
    menu bar, and the bottom key-hint bar ("F1 Help ...") appears on exactly
    one row, the last one.  A second copy on another row means a stale
    repaint left old chrome on screen; this is what every drain is checked
    against.  Returns a list of violation strings (empty = screen is fine).

    Importable by other test files that want the same oracle on their own
    screens."""
    out = []
    if not rows[0].startswith(MENU_BAR):
        out.append("%s: row 0 is not the menu bar: %r" % (label, rows[0]))
    bar_rows = [i for i, r in enumerate(rows) if BAR_WORD in r]
    # During the Alt-G y flash the bottom row reads "Permissions: ask edits
    # auto" instead of the key hints by design; that is not a violation, it
    # is the ABSENCE of a second copy that the oracle actually hunts for.
    if bar_rows == [] and rows[-1].startswith(" Permissions:"):
        return out
    if bar_rows != [len(rows) - 1]:
        out.append("%s: '%s' on rows %r (expected only %d)" % (label, BAR_WORD, bar_rows, len(rows) - 1))
    return out


def _watch_invariant(s, seconds, label, found, step=0.1):
    """Drain for `seconds`, checking the oracle after every drain; append
    any violations (with the screen that showed them) to `found`."""
    end = time.time() + seconds
    while time.time() < end:
        s._drain(step)
        rows = s.display(drain=0)
        v = screen_invariant_violations(rows, label)
        if v:
            found.append((v, list(rows)))


# @scenario SC-ai-agent-48
def test_invariant_holds_under_ask_with_boxes_reload_denial_and_flash(tmp_path):
    marker = tmp_path / "CMD_MARKER"
    turns = "@@TURN@@".join([
        "Let me look.\nTOOL list_dir .",
        "TOOL grep main",
        "TOOL read_file t.c",
        "TOOL write_file t.c\nint main(void){return 1;} /* AGENTEDIT */\n@@END",
        "TOOL run_command touch %s" % marker,
        "TOOL write_file new.txt\nfresh\n@@END",
        "TOOL frobnicate x",
        "DONE finished the varied run\nwith a two-line answer"])
    env = _env(tmp_path, turns, "ask")
    del env["XWPE_AI_POLICY"]
    found = []
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        # a status flash from Alt-G y, three times round the dial back to ask
        for _ in range(3):
            s.key(ALT.AI); s.key("y", delay=0.3)
            _watch_invariant(s, 0.4, "flash", found)
        s.key("\033[B", delay=0.3)
        _watch_invariant(s, 0.3, "after flash", found)
        s.key(ALT.AI); s.key("g"); s.key("varied run"); s.key("\r", delay=0.1)
        end = time.time() + 15
        while time.time() < end and " Write " not in _screen(s, 0):
            _watch_invariant(s, 0.1, "turns 1-3", found)
        _watch_invariant(s, 0.5, "write box", found)
        s.key("y", delay=0.1)
        end = time.time() + 15
        while time.time() < end and "APPROVE?" not in _screen(s, 0):
            _watch_invariant(s, 0.1, "after write t.c", found)
        after_write = _screen(s, 0)              # the wrote-line is still in view here
        _watch_invariant(s, 0.5, "command box", found)
        s.key("n", delay=0.1)
        end = time.time() + 15
        while time.time() < end and " Write " not in _screen(s, 0):
            _watch_invariant(s, 0.1, "after deny", found)
        _watch_invariant(s, 0.3, "write box new.txt", found)
        s.key("y", delay=0.1)
        end = time.time() + 20
        while time.time() < end and "type a follow-up" not in _screen(s, 0):
            _watch_invariant(s, 0.1, "to the end", found)
        _watch_invariant(s, 1.0, "at the end", found)
        rows = s.display()
        s.key(ESC, delay=0.5)
        _watch_invariant(s, 0.5, "after Esc from the pane", found)
    text = "\n".join(rows)
    assert "AGENTEDIT" in text, "the written t.c was not reloaded into its window:\n" + text
    assert "[agent] wrote t.c - Ctrl-U to undo (buffer only)" in after_write, after_write
    assert "[agent] denied: run_command touch" in text, text
    assert (tmp_path / "new.txt").read_text() == "fresh\n"
    assert not found, "%d screen-invariant violations, first: %s" % (len(found), found[0][0])


# @scenario SC-ai-agent-48
def test_invariant_holds_under_auto_with_a_plan_and_full_review(tmp_path):
    turns = "@@TURN@@".join([
        "TOOL plan\nread\nwrite\n@@END",
        "TOOL list_dir .",
        "TOOL step_done read",
        "TOOL write_file a.txt\nAAA\n@@END",
        "TOOL apply_patch a.txt\n<<<<<<< SEARCH\nAAA\n=======\nBBB\n>>>>>>> REPLACE\n@@END",
        "TOOL step_done write",
        "DONE done"])
    env = _env(tmp_path, turns, "auto")
    found = []
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        s.key(ALT.AI); s.key("g"); s.key("auto run"); s.key("\r", delay=0.1)
        end = time.time() + 25
        while time.time() < end and "a = keep all" not in _screen(s, 0):
            _watch_invariant(s, 0.1, "auto turns", found)
        _watch_invariant(s, 0.5, "review open", found)
        s.key("a", delay=0.1)
        end = time.time() + 10
        while time.time() < end and "type a follow-up" not in _screen(s, 0):
            _watch_invariant(s, 0.1, "review closing", found)
        _watch_invariant(s, 1.0, "end", found)
    assert (tmp_path / "a.txt").read_text() == "BBB\n"
    assert not found, "%d screen-invariant violations, first: %s" % (len(found), found[0][0])
