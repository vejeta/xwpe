"""What a whole-file AI Edit sends to the model.

The request body itself is observable without any engine change: the Claude CLI
backend writes the flattened request to the child `claude` on its stdin
(ai_flatten_prompt, we_ai.c), and a stub `claude` on PATH, the same technique
`test_ai_edit_error_guard.py` uses, can save that stdin to a file before it
answers.  The assertions below are on that saved file, so they check the
request the editor really built, in the order it builds it
(e_ai_edit, we_ai_ui.c: instruction, diagnostics block, open-windows block,
scope header, file text).

What would make this test fail: dropping the open-windows block or the
"--- file ---" header from the user message, sending the window around the
cursor instead of the whole file for a small file, or sending the instruction
after the text.  The diagnostics block is empty by design when no language
server reports anything (ai_diag_block returns "" then); a stub claude cannot
make a language server speak, so the positive case is not asserted here and
the test states that: it asserts the block is absent, exactly as the code
promises when there are no diagnostics.
"""
import os
import time
import subprocess

import pytest

from wpe_driver import WpeSession, ALT, WPE_BIN

ORIG = "int a = 1;\nint b = 2;\nint c = 3;\n"
INSTRUCTION = "rename a to alpha"


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _stub_claude_capturing(dir_, capture, reply):
    """A fake `claude` CLI that saves its stdin (the request body) to `capture`
    and answers with a success envelope whose result is `reply`."""
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


def _wait_screen(s, needle, tmo=15):
    end = time.time() + tmo
    while time.time() < end:
        s._drain(0.4)
        if needle in "\n".join(s.display()):
            return True
    return False


# @scenario SC-ai-edit-06
def test_whole_file_edit_sends_instruction_windows_header_and_file(tmp_path):
    capture = str(tmp_path / "request.txt")
    # The reply is the file unchanged, so the editor reports "[AI edit] no change"
    # (we_ai_ui.c, ai_pane(f, "[AI edit] no change", 0)) and no review box opens:
    # the stream completes and the request is on disk.
    bindir = _stub_claude_capturing(str(tmp_path), capture, ORIG)
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "claudecli",
        "XWPE_AI_MODEL": "sonnet",
        "PATH": bindir + ":" + os.environ.get("PATH", ""),
        "XWPE_AI_TRACE": str(tmp_path / "e.trace"),
    }
    with WpeSession(str(tmp_path), ORIG, env_extra=env, filename="t.c") as s:
        s.key(ALT.AI); s.key("e"); s._drain(0.5)
        s.key(INSTRUCTION); s.key("\r", delay=0.8)
        assert _wait_screen(s, "[AI edit] no change"), \
            "the edit did not complete:\n" + "\n".join(s.display())

    body = open(capture).read()
    file_path = os.path.join(str(tmp_path), "t.c")

    # 1. The instruction opens the user message (ai_flatten_prompt labels it).
    assert "User: " + INSTRUCTION in body, body
    # 2. The open-windows block names the file being edited as the focused one.
    assert "FILES OPEN IN THE EDITOR" in body, body
    assert file_path + "   <- focused" in body, body
    # 3. The scope header says the WHOLE file follows, not a window of it, and
    #    the whole file does follow.
    assert "--- file ---\n" + ORIG in body, body
    # 4. The order is instruction, open windows, header, text.
    i_instr = body.index(INSTRUCTION)
    i_open = body.index("FILES OPEN IN THE EDITOR")
    i_hdr = body.index("--- file ---")
    i_text = body.index(ORIG)
    assert i_instr < i_open < i_hdr < i_text, (i_instr, i_open, i_hdr, i_text)
    # 5. No language server is running, so the diagnostics block is empty by
    #    design (ai_diag_block returns "" when there is nothing to report).
    assert "DIAGNOSTICS reported for the current file" not in body, body
    # The trace line the editor writes before starting the stream.
    assert "edit instr=" + INSTRUCTION in open(str(tmp_path / "e.trace")).read()
