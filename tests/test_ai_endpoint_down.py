"""When an OpenAI-compatible endpoint is unreachable, say what to do.

The OpenAI-compatible backend is almost always a local server or bridge (a
Lumo/Proton bridge, vLLM, LM Studio, a Groq proxy...).  A refused connection
used to surface only the bare words "connection refused", which does not tell
the user that the process serving the endpoint is down or where to fix it.  The
message must now name the address and the real reason and point at the fix.
"""
import os
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


def test_openai_endpoint_down_message(tmp_path):
    # Port 1 (tcpmux) is virtually never listening -> connect() is refused.
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "openai",
           "XWPE_AI_ENDPOINT": "http://127.0.0.1:1", "XWPE_AI_MODEL": "m"}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, filename="a.c") as s:
        s.key(ALT.AI); s.key("a")
        s.key("hi"); s.key("\r", delay=1.0)
        s._drain(1.0)
        disp = "\n".join(s.display())
    # The real reason and the address, not a bare "connection refused":
    assert "cannot connect to 127.0.0.1:1" in disp, \
        "the endpoint address / real reason is not shown:\n" + disp
    # ...and an actionable hint about the local server/bridge and where to fix it:
    assert "server/bridge" in disp, \
        "no actionable 'is the server/bridge running?' hint:\n" + disp
