"""Alt-G B: the "fix the build" loop.

Fix-build launches the agent with NO prompt: it detects the build command
(here `make`, because a Makefile is present), then runs it, reads the errors,
edits/creates files, and re-runs until it builds.

The Makefile fails until `ok.flag` exists.  The scripted agent runs make (fails),
creates the flag, runs make again (passes), and reports DONE -- a genuine
fail -> fix -> pass loop.  Asserts: the auto goal was seeded (no prompt was
possible in this drive), the build command was detected as `make`, make ran more
than once (it looped after the fix), and the flag now exists.
"""
import os
import time
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

MAKEFILE = ("all:\n"
            "\t@test -f ok.flag && echo BUILD OK || "
            "{ echo 'error: ok.flag missing' >&2; exit 1; }\n")

# scripted agent turns: run build (fails), create the fix, run build (passes), done
TURNS = ("TOOL run_command make"
         "@@TURN@@TOOL run_command touch ok.flag"
         "@@TURN@@TOOL run_command make"
         "@@TURN@@DONE created ok.flag so the build passes")


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def test_fix_build_loop(tmp_path):
    (tmp_path / "Makefile").write_text(MAKEFILE)
    trace = tmp_path / "b.trace"
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_POLICY": "auto",
           "XWPE_AI_MOCK_REPLY": TURNS, "XWPE_AI_TRACE": str(trace)}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, filename="t.c") as s:
        s._drain(0.5)
        s.key(ALT.AI); s.key("b"); s._drain(0.5)     # Alt-G B: fix the build
        end = time.time() + 20
        while time.time() < end:
            s._drain(0.6)
            t = trace.read_text() if trace.exists() else ""
            if "conv error" in t or "agent done" in t or (tmp_path / "ok.flag").exists():
                break
        s.key("a", delay=0.4); s.key("\033", delay=0.3)   # dismiss any changeset review
        s._drain(0.5)
    txt = trace.read_text()
    assert "agent task=Fix the failing build." in txt, \
        "fix-build did not auto-seed the agent goal (it should not prompt):\n" + txt
    assert "fix-build cmd=make" in txt, "build command not detected as make:\n" + txt
    assert txt.count("agent tool=run_command") >= 2, \
        "the build was not re-run after the fix (no loop):\n" + txt
    assert (tmp_path / "ok.flag").exists(), "the fix (ok.flag) was never created"
