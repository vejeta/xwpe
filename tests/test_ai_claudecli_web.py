"""The Claude CLI backend follows the web switch.

`claude -p` cannot prompt for permission, so a tool that is not pre-allowed is
denied -- which is why Claude used to answer "give me permission to the web
tool" when asked to look something up. With web access on, xwpe passes
`--allowedTools WebSearch WebFetch` so the model can browse without being
granted the file or shell tools. With it off, the two names go on the deny
list instead, so the switch means the same thing on this backend as on the
editor's own tools; before this it was handed the web whatever the switch said.

Both tests drive a chat with a fake `claude` on PATH that records its argv.
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


def _argv_of_one_turn(tmp_path, web):
    """Ask the claudecli backend one question and return the argv it launched
    `claude` with, as a string. @web is what XWPE_AI_WEB is set to."""
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
        "XWPE_AI_WEB": web,
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
    return argv_file.read_text()


# @scenario SC-ai-chat-52
def test_claudecli_allows_web_tools_when_the_switch_is_on(tmp_path):
    argv = _argv_of_one_turn(tmp_path, "1")
    allowed = argv.split("--allowedTools", 1)
    assert len(allowed) == 2, "no --allowedTools passed:\n" + argv
    granted = allowed[1].split("--", 1)[0]
    assert "WebSearch" in granted, "WebSearch not allowed:\n" + argv
    assert "WebFetch" in granted, "WebFetch not allowed:\n" + argv
    # The chat path must still keep the write/shell tools off the model.
    assert "--disallowedTools" in argv, "text-only guard dropped:\n" + argv
    assert "Bash" in argv, "Bash not disallowed:\n" + argv


# @scenario SC-ai-chat-52
def test_claudecli_denies_web_tools_when_the_switch_is_off(tmp_path):
    argv = _argv_of_one_turn(tmp_path, "0")
    assert "--allowedTools" not in argv, "the web was pre-approved while off:\n" + argv
    denied = argv.split("--disallowedTools", 1)
    assert len(denied) == 2, "nothing was disallowed:\n" + argv
    refused = denied[1]
    assert "WebSearch" in refused, "WebSearch not denied:\n" + argv
    assert "WebFetch" in refused, "WebFetch not denied:\n" + argv
    assert "Bash" in refused, "Bash not disallowed:\n" + argv
