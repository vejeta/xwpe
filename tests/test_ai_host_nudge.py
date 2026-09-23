"""With the Claude CLI backend on Auto, the Agent nudges toward the host engine.

The built-in agent runs claudecli text-only and never grants the model
--dangerously-skip-permissions, so "Auto" does not give the model its own
autonomy there -- that is the Claude Code agent engine. When a user picks
claudecli + Auto and runs the Agent, a one-time tip points them at it.
"""
import os
import subprocess
import tempfile

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN


def _host_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out and b"For full autonomy" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(
    not _host_build(), reason="wpe built without --enable-ai-agent-host")


def test_agent_nudges_to_host_engine(tmp_path):
    bindir = tmp_path / "bin"
    bindir.mkdir()
    fake = bindir / "claude"
    fake.write_text(
        "#!/bin/sh\n"
        "echo '{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,\"result\":\"ok\"}'\n"
    )
    fake.chmod(0o755)

    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "claudecli",
        "XWPE_AI_POLICY": "auto",
        "XWPE_AI_MODEL": "default",
        "PATH": str(bindir) + ":" + os.environ.get("PATH", ""),
    }
    with WpeSession(tempfile.mkdtemp(), "int main(void){return 0;}\n",
                    env_extra=env, filename="a.c") as s:
        s.key(ALT.AI, delay=0.5); s.key("g", delay=0.7)   # Agent -> prompt popup
        s.key("do a thing", delay=0.4)
        s.key("\033s", delay=1.5)                          # Alt-S = Send
        s._drain(1.0)
        disp = "\n".join(s.display())

    assert "full autonomy" in disp, \
        "no host-engine tip shown for claudecli + Auto:\n" + disp
