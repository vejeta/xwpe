"""A configured post-edit hook runs after the agent writes a file.

AIEditHook (e.g. "clang-format -i") runs on a file the AI just wrote, then the
buffer reloads, so AI edits stay consistent with the repo's tooling -- like
format-on-save or a git hook.  Here the hook appends a marker so the test can see
it ran, after an agent write_file, under unattended policy.
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


def test_edit_hook_runs_after_write(tmp_path):
    home = str(tmp_path / "home")
    cfg = os.path.join(home, ".config", "xwpe")
    os.makedirs(cfg)
    # the hook appends a marker to whatever path it is given
    with open(os.path.join(cfg, "xwperc"), "w") as fh:
        fh.write("[Programming]\nAIEditHook : printf HOOKRAN99 >>\n")
    turns = ("TOOL write_file t.c\nint main(void){return 1;}\n@@END"
             "@@TURN@@DONE wrote it")
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_POLICY": "auto",
           "XWPE_AI_MOCK_REPLY": turns, "XWPE_AI_TRACE": str(tmp_path / "a.trace"),
           "HOME": home}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, filename="t.c") as s:
        s.key(ALT.AI); s.key("g"); s._drain(0.5)
        s.key("rewrite t.c"); s.key("\r", delay=0.8)
        end = time.time() + 15
        while time.time() < end:
            s._drain(0.5)
            t = (tmp_path / "a.trace").read_text() if (tmp_path / "a.trace").exists() else ""
            if "changeset" in t:
                break
        s.key("a", delay=0.5)                      # keep all -> leave the review
        s._drain(0.5)
    with open(os.path.join(str(tmp_path), "t.c")) as fh:
        text = fh.read()
    assert "return 1;" in text, "the agent write did not land:\n" + text
    assert "HOOKRAN99" in text, \
        "the post-edit hook did not run on the written file:\n" + text
