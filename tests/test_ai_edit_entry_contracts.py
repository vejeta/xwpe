"""Entry-point contracts for AI Edit and Complete that had no test.

Covers the prompt popup, cancelling it, a failed preflight, an empty reply, the
"apply the rest unseen" behaviour of `a`, and the Complete guards -- all driven
with the deterministic mock backend (or a local stub `claude`, never a real
network service).  Every assertion below is pinned against the exact string or
trace line the source prints, grepped before writing the test; see the report
for the file:line of each.
"""
import os
import time
import subprocess

import pytest

from wpe_driver import WpeSession, ALT, WPE_BIN

ORIG = "int a = 1;\n"
BOX = "Proposed change"

# A waiting sign lives only while the backend is working, so the poll below has
# to be finer than the sign is short, and the backend has to be held for longer
# on a loaded machine, the way test_ai_edit_waiting_line.py holds it.
WAIT_SCALE = float(os.environ.get("XWPE_TEST_WAIT_SCALE", "1"))
SPIN_DELAY_MS = int(1500 * WAIT_SCALE)


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _env(tmp_path, **extra):
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": "ok",
        "XWPE_AI_TRACE": str(tmp_path / "e.trace"),
    }
    env.update(extra)
    return env


def _trace(tmp_path):
    p = tmp_path / "e.trace"
    return p.read_text() if p.exists() else ""


def _wait_screen(s, needle, tmo=12):
    """Poll the screen for `needle` until `tmo` runs out.

    `display(drain=0)` reads the screen as it stands instead of draining for
    another 0.6 s first, which makes one turn of this loop cost 0.4 s rather
    than 1.0 s.  A reply box stays up and is found either way; a waiting line
    that lives as long as the backend takes is not, and the coarse grid cannot
    look until the sign is already gone.
    """
    end = time.time() + tmo
    while time.time() < end:
        s._drain(0.4)
        if needle in "\n".join(s.display(drain=0)):
            return True
    return False


def _stub_claude(dir_, body):
    """A fake `claude` CLI that ignores its prompt and prints `body` on stdout,
    the same technique test_ai_edit_error_guard.py uses: no real network, no
    real model, just a deterministic backend-error envelope."""
    d = os.path.join(dir_, "bin")
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, "claude")
    with open(p, "w") as f:
        f.write("#!/bin/sh\ncat >/dev/null 2>&1\nprintf '%s' '" + body + "'\n")
    os.chmod(p, 0o755)
    return d


# ===========================================================================
# SC-ai-edit-02 -- cancelling the prompt sends nothing.
# we_ai_ui.c:3966-3968 (e_ai_edit): Esc/Alt-C cancel e_ai_prompt1 (returns 0),
# and an empty instruction also returns 0 via "|| !instr[0]" -- in every case
# ai_edit_launch (and its "edit instr=%s" trace, we_ai_ui.c:3916, and the
# "stream start" trace, we_ai.c:1557/1567/1595) is never reached.
# ===========================================================================
@pytest.mark.parametrize("case,keys,type_instr", [
    ("esc", ("\033",), True),
    ("altc", ("\033c",), True),
    ("empty", ("\r",), False),
])
def test_cancelling_the_prompt_sends_nothing(tmp_path, case, keys, type_instr):
    with WpeSession(str(tmp_path), ORIG, env_extra=_env(tmp_path), filename="t.c") as s:
        s.key(ALT.AI); s.key("e"); s._drain(0.5)
        if type_instr:
            s.key("do it", delay=0.3)
        for k in keys:
            s.key(k, delay=0.6)
        s._drain(0.6)
        disp = "\n".join(s.display())
        assert "AI edit instruction" not in disp, \
            "case %s: the prompt is still open:\n%s" % (case, disp)
        assert "[AI edit] working" not in disp, \
            "case %s: the spinner showed up:\n%s" % (case, disp)
        assert "[AI" not in disp, \
            "case %s: an AI pane line appeared on screen:\n%s" % (case, disp)
        s.save()
        assert s.text() == ORIG, "case %s: the file changed" % case
    txt = _trace(tmp_path)
    assert "edit instr=" not in txt, "case %s: an instruction was sent:\n%s" % (case, txt)
    assert "stream start" not in txt, "case %s: a stream was opened:\n%s" % (case, txt)


# ===========================================================================
# SC-ai-edit-03 -- a failed preflight refuses to start, no stream opens.
# we_ai_ui.c:3891 (ai_edit_launch): wpe_ai_preflight() is checked before the
# "edit instr=" trace and before wpe_ai_stream_start(); on failure ai_pane()
# shows its message and the function returns. The ollama preflight text is
# we_ai.c:1082-1084: "Ollama not reachable at %s - is it running? (try
# `ollama serve`)".
# ===========================================================================
def test_preflight_failure_refuses_to_start(tmp_path):
    env = _env(tmp_path, XWPE_AI_BACKEND="ollama", XWPE_AI_ENDPOINT="http://127.0.0.1:1")
    with WpeSession(str(tmp_path), ORIG, env_extra=env, filename="t.c") as s:
        s.key(ALT.AI); s.key("e"); s._drain(0.4)
        s.key("do it"); s.key("\r", delay=0.8)
        assert _wait_screen(s, "Ollama not reachable"), \
            "preflight error never reached the pane:\n" + "\n".join(s.display())
        disp = "\n".join(s.display())
        # The pane is narrower than the message, so it wraps across rows; each
        # row keeps a border glyph past its last real column, so joined rows
        # cannot be searched as one phrase.  Check the two halves of the
        # source string (we_ai.c:1082-1084) separately instead.
        assert "is it running?" in disp, \
            "preflight message text does not match the source:\n" + disp
        assert "ollama" in disp and "serve" in disp, \
            "preflight message text does not match the source:\n" + disp
        s.save()
        assert s.text() == ORIG, "file changed despite a failed preflight"
    txt = _trace(tmp_path)
    assert "edit instr=" not in txt, "an instruction was sent after a failed preflight:\n" + txt
    assert "stream start" not in txt, "a stream opened after a failed preflight:\n" + txt


# ===========================================================================
# SC-ai-edit-10 -- an empty reply applies nothing and says so.
# we_ai_ui.c:3633-3636 (ai_edit_done): op->acc_len == 0 -> ai_pane(f,
# "[AI edit] no response", 0); nothing is diffed or applied.
# ===========================================================================
def test_empty_reply_says_no_response(tmp_path):
    env = _env(tmp_path, XWPE_AI_MOCK_REPLY="")
    with WpeSession(str(tmp_path), ORIG, env_extra=env, filename="t.c") as s:
        s.key(ALT.AI); s.key("e"); s._drain(0.4)
        s.key("do it"); s.key("\r", delay=0.8)
        assert _wait_screen(s, "[AI edit] no response"), \
            "the exact 'no response' line never appeared:\n" + "\n".join(s.display())
        assert BOX not in "\n".join(s.display()), "a review box opened for an empty reply"
        s.save()
        assert s.text() == ORIG, "disk changed for an empty reply"


# ===========================================================================
# SC-ai-edit-20 -- `a` applies the rest of the hunks unseen.
# we_ai_ui.c:2889-2902 (ai_hunk_apply): once decision=='A' sets all=1, every
# later change hunk is accepted via "if (all) { ...; continue; }" WITHOUT
# calling ai_diff_review_hunk again, so its box never opens (unless a later
# hunk deletes more than it adds, not the case here). we_ai_ui.c:3683 traces
# "edit applied %d/%d".
# ===========================================================================
def test_a_applies_the_rest_unseen(tmp_path):
    seed = "".join("int v%d = %d;\n" % (i, i) for i in range(1, 10))
    reply_lines = ["int v%d = %d;" % (i, i) for i in range(1, 10)]
    for i in (0, 4, 8):          # three separated hunks (lines 1, 5, 9)
        reply_lines[i] = "int v%d = %d;" % (i + 1, (i + 1) * 100)
    reply = "\n".join(reply_lines) + "\n"
    env = _env(tmp_path, XWPE_AI_MOCK_REPLY=reply)
    with WpeSession(str(tmp_path), seed, env_extra=env, filename="t.c") as s:
        s.key(ALT.AI); s.key("e"); s._drain(0.4)
        s.key("do it"); s.key("\r", delay=0.8)
        assert _wait_screen(s, "Proposed change 1/3"), "first of three hunks not shown"
        s.key("a", delay=0.7)
        saw_second = False
        end = time.time() + 8
        while time.time() < end:
            s._drain(0.3)
            if "Proposed change 2/3" in "\n".join(s.display()):
                saw_second = True
                break
            if "edit applied" in _trace(tmp_path):
                break
        assert not saw_second, "hunk 2/3 was shown after 'a' -- it must apply unseen"
        s.save()
        out = s.text()
    assert "edit applied 3/3" in _trace(tmp_path), \
        "trace does not show all three hunks applied:\n" + _trace(tmp_path)
    assert out == reply, "disk does not hold all three accepted changes:\n" + out


# ===========================================================================
# SC-ai-edit-01, sharper -- the Edit prompt shows its buttons and the
# selection-variant title.
# we_ai_ui.c:3966-3967 (e_ai_edit): title "AI edit instruction" or "AI edit
# instruction (selection)"; we_ai_ui.c:1427-1429 (e_ai_prompt): the buttons
# " Send ", "Multi-line", "Cancel".
# ===========================================================================
def test_edit_prompt_shows_its_buttons(tmp_path):
    with WpeSession(str(tmp_path), "int a = 1;\nint b = 2;\n",
                    env_extra=_env(tmp_path), filename="t.c") as s:
        s.key(ALT.AI); s.key("e"); s._drain(0.5)
        disp = "\n".join(s.display())
        assert "AI edit instruction" in disp, "popup title missing:\n" + disp
        assert "(selection)" not in disp, "whole-file popup wrongly says (selection):\n" + disp
        for label in ("Send", "Multi-line", "Cancel"):
            assert label in disp, "button %r missing from the popup:\n%s" % (label, disp)
        s.key("\033", delay=0.4)


def test_edit_prompt_selection_title(tmp_path):
    with WpeSession(str(tmp_path), "int a = 1;\nint b = 2;\nint c = 3;\n",
                    env_extra=_env(tmp_path), filename="t.c") as s:
        s.key(ALT.BLOCK, "b", delay=0.3)
        s.key("\033[B", delay=0.2)            # widen the mark to a second line
        s.key(ALT.BLOCK, "e", delay=0.3)
        s.key(ALT.AI); s.key("e"); s._drain(0.5)
        disp = "\n".join(s.display())
        assert "AI edit instruction (selection)" in disp, \
            "selection-variant popup title missing:\n" + disp
        s.key("\033", delay=0.4)


# ===========================================================================
# SC-ai-edit-37, sharper -- Complete's spinner in a text window, and the
# guard in a real non-text window.
#
# we_ai_ui.c: ai_spin_begin(f, "[AI] completing").
# we_ai_ui.c: ai_refuse_non_file_window() shows AI_NO_FILE_LINE, the one pane
# line Edit, Complete and Multi-file share for a window that is not a file.
#
# The File-Manager window (dtmd = DTMD_FILEMANAGER = 'F', edit.h, not a text
# window) is the non-file window driven here; the AI pane case, where the
# window IS a text buffer but not a file, is pinned by
# test_ai_edit_blocked_contracts.py.
# ===========================================================================
def test_complete_spinner_in_text_window(tmp_path):
    env = _env(tmp_path, XWPE_AI_MOCK_DELAY_MS=str(SPIN_DELAY_MS))
    with WpeSession(str(tmp_path), ORIG, env_extra=env, filename="t.c") as s:
        s.key(ALT.AI); s.key("c", delay=0.6)
        assert _wait_screen(s, "[AI] completing"), \
            "the completing spinner never showed:\n" + "\n".join(s.display())


def test_complete_refuses_a_real_non_text_window(tmp_path):
    F3 = "\033OR"      # File -> File-Manager (macos_key_route maps this on macOS)
    with WpeSession(str(tmp_path), ORIG, env_extra=_env(tmp_path), filename="t.c") as s:
        s.key(F3, delay=0.8)
        disp = "\n".join(s.display())
        assert "File-Manager" in disp, "the File Manager did not open:\n" + disp
        s.key(ALT.AI); s.key("c", delay=0.8)
        s._drain(0.5)
        disp = "\n".join(s.display())
        assert "[AI] no file here" in disp, \
            "the non-file-window guard message is missing:\n" + disp
    txt = _trace(tmp_path)
    assert "stream start" not in txt, "a completion stream opened on a non-text window:\n" + txt


# ===========================================================================
# SC-ai-edit-40 -- Complete guards: backend error and no suggestion.
# we_ai_ui.c:4218: `if (op->had_error) { ai_pane(f, "[AI complete] backend
# error", 1); ... }`.
# we_ai_ui.c:4219/4223: `ai_pane(f, "[AI complete] no suggestion", 0);` for an
# empty or blank reply.
# ===========================================================================
def test_complete_no_suggestion_on_empty_reply(tmp_path):
    env = _env(tmp_path, XWPE_AI_MOCK_REPLY="")
    with WpeSession(str(tmp_path), ORIG, env_extra=env, filename="t.c") as s:
        s.key(ALT.AI); s.key("c", delay=0.8)
        assert _wait_screen(s, "[AI complete] no suggestion"), \
            "the exact 'no suggestion' line never appeared:\n" + "\n".join(s.display())
        s.save()
        assert s.text() == ORIG, "disk changed when there was no suggestion"


def test_complete_backend_error(tmp_path):
    bindir = _stub_claude(
        str(tmp_path),
        '{"type":"result","subtype":"error_during_execution",'
        '"is_error":true,"result":"Not logged in - Please run /login",'
        '"session_id":"stub"}')
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "claudecli",
        "XWPE_AI_MODEL": "sonnet",
        "PATH": bindir + ":" + os.environ.get("PATH", ""),
        "XWPE_AI_TRACE": str(tmp_path / "e.trace"),
    }
    with WpeSession(str(tmp_path), ORIG, env_extra=env, filename="t.c") as s:
        s.key(ALT.AI); s.key("c", delay=0.8)
        assert _wait_screen(s, "[AI complete] backend error", tmo=20), \
            "the exact 'backend error' line never appeared:\n" + "\n".join(s.display())
        s.save()
        assert s.text() == ORIG, "disk changed when the backend errored"
