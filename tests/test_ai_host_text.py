"""External agent host, `text` adapter: host any prompt-in/text-out CLI.

Besides the Claude Code stream-json adapter, the host can render a plain CLI
(aider --message, sgpt, a script, ...) whose stdout is the answer: the
adapter writes the prompt to the CLI's stdin, closes it (EOF), and streams the
CLI's output into the pane.  A one-shot per turn.  A mock text CLI stands in for
a real one so the test is deterministic and needs no tool installed.
"""
import os
import time
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN


def _host_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"AIHostAdapter text needs AIHostCommand" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _host_build(),
                                reason="built without --enable-ai-agent-host")


def test_text_adapter_streams_cli_output(tmp_path):
    # The mock CLI reads (and discards) the prompt on stdin, then prints an
    # answer and exits -- exactly the shape of a one-shot text CLI.
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_AGENT_ENGINE": "claude-code",       # Agent (Alt-G g) uses the host
        "XWPE_AI_HOST_ADAPTER": "text",              # ... via the plain-text adapter
        "XWPE_AI_HOST_CMD": "cat >/dev/null; printf 'TEXTADAPTER_OK line one\\nand line two\\n'",
        "XWPE_AI_TRACE": str(tmp_path / "h.trace"),
    }
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, filename="t.c") as s:
        s.key(ALT.AI); s.key("g"); s._drain(0.5)     # Alt-G g -> host engine
        s.key("say hello"); s.key("\r", delay=1.0)
        end = time.time() + 12
        while time.time() < end:
            s._drain(0.5)
            if "TEXTADAPTER_OK" in "\n".join(s.display()):
                break
        disp = "\n".join(s.display())
    assert "TEXTADAPTER_OK line one" in disp and "and line two" in disp, \
        "the text adapter did not stream the CLI's output:\n" + disp
