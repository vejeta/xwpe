"""The Claude CLI backend must pre-allow the read-only web tools.

`claude -p` cannot prompt for permission, so a tool that is not pre-allowed is
denied -- which is why Claude used to answer "give me permission to the web
tool" when asked to look something up. xwpe now passes
`--allowedTools WebSearch WebFetch` on every claudecli launch so the model can
browse in any permission mode, without granting the file/shell tools.

This drives a chat with a fake `claude` on PATH that records its argv, and
asserts the web tools are allowed.
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


def test_claudecli_allows_web_tools(tmp_path):
    argv_file = tmp_path / "claude_argv.txt"
    bindir = tmp_path / "bin"
    bindir.mkdir()
    fake = bindir / "claude"
    # Record argv, then emit a minimal one-shot result so the turn can finish.
    fake.write_text(
        "#!/bin/sh\n"
        'echo "$*" > ' + str(argv_file) + "\n"
        "echo '{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,\"result\":\"ok\"}'\n"
    )
    fake.chmod(0o755)

    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "claudecli",
        "XWPE_AI_MODEL": "default",
        "PATH": str(bindir) + ":" + os.environ.get("PATH", ""),
    }
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key(ALT.AI); s.key("a")
        s.key("look something up"); s.key("\r", delay=1.5)
        # Give the fork/exec time to record argv.
        waited = 0.0
        while waited < 15 and not argv_file.exists():
            s._drain(1.0)
            waited += 1.0

    assert argv_file.exists(), "the claudecli backend never launched `claude`"
    argv = argv_file.read_text()
    assert "--allowedTools" in argv, "no --allowedTools passed:\n" + argv
    assert "WebSearch" in argv, "WebSearch not allowed:\n" + argv
    assert "WebFetch" in argv, "WebFetch not allowed:\n" + argv
    # The chat path must still keep the write/shell tools off the model.
    assert "--disallowedTools" in argv, "text-only guard dropped:\n" + argv
    assert "Bash" in argv, "Bash not disallowed:\n" + argv
