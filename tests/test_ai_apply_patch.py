"""Agent apply_patch: a precise SEARCH/REPLACE edit lands and is reviewable.

Instead of rewriting the whole file (write_file), the agent can change an
existing file with a SEARCH/REPLACE block; the matched text is replaced and the
result is written through the SAME review/checkpoint path as write_file (so the
open buffer refreshes and the changeset lists it).  A SEARCH that does not match
comes back as a TOOL ERROR the model can retry, rather than a silent bad write.

Unattended (auto) policy so the write needs no approval; mock backend so no
model/network is involved.
"""
import os
import time
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def test_agent_apply_patch_edits_in_place(tmp_path):
    turns = ("TOOL apply_patch t.c\n"
             "<<<<<<< SEARCH\n"
             "  return 0;\n"
             "=======\n"
             "  return 42;\n"
             ">>>>>>> REPLACE\n"
             "@@END"
             "@@TURN@@DONE changed t.c")
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_POLICY": "auto",
           "XWPE_AI_MOCK_REPLY": turns, "XWPE_AI_TRACE": str(tmp_path / "a.trace")}
    seed = "int main(void)\n{\n  return 0;\n}\n"
    with WpeSession(str(tmp_path), seed, env_extra=env, filename="t.c") as s:
        s.key(ALT.AI); s.key("g"); s._drain(0.5)
        s.key("make main return 42"); s.key("\r", delay=0.8)
        end = time.time() + 15
        while time.time() < end:
            s._drain(0.5)
            t = (tmp_path / "a.trace").read_text() if (tmp_path / "a.trace").exists() else ""
            if "changeset" in t:
                break
        disp = "\n".join(s.display())
        assert "return 42;" in disp, \
            "open buffer did not show the patched line:\n" + disp
        s.key("a", delay=0.5)                      # keep all -> leave the review
        s._drain(0.5)
    with open(os.path.join(str(tmp_path), "t.c")) as fh:
        text = fh.read()
    assert "return 42;" in text and "return 0;" not in text, \
        "apply_patch did not edit the file on disk:\n" + text


def test_apply_patch_unmatched_search_is_tool_error(tmp_path):
    # A SEARCH that is not in the file must NOT write; it returns a TOOL ERROR so
    # the model can retry.  Turn 2 ends the run.  The file stays unchanged.
    turns = ("TOOL apply_patch t.c\n"
             "<<<<<<< SEARCH\n"
             "return 999;\n"                        # not present in the file
             "=======\n"
             "return 42;\n"
             ">>>>>>> REPLACE\n"
             "@@END"
             "@@TURN@@DONE nothing matched")
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_POLICY": "auto",
           "XWPE_AI_MOCK_REPLY": turns, "XWPE_AI_TRACE": str(tmp_path / "b.trace")}
    seed = "int main(void)\n{\n  return 0;\n}\n"
    with WpeSession(str(tmp_path), seed, env_extra=env, filename="t.c") as s:
        s.key(ALT.AI); s.key("g"); s._drain(0.5)
        s.key("patch it"); s.key("\r", delay=0.8)
        end = time.time() + 12
        while time.time() < end:
            s._drain(0.5)
            t = (tmp_path / "b.trace").read_text() if (tmp_path / "b.trace").exists() else ""
            if "agent done" in t or "agent answer" in t:
                break
        s._drain(0.5)
    with open(os.path.join(str(tmp_path), "t.c")) as fh:
        text = fh.read()
    assert "return 0;" in text and "return 42;" not in text, \
        "an unmatched SEARCH should not have modified the file:\n" + text
