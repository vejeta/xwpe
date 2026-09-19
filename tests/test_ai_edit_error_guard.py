"""A backend failure must NEVER be applied as file content.

When the `claude` CLI is not logged in it still exits with a JSON envelope whose
`is_error` is true and whose `result` holds the human message ("Not logged in -
Please run /login").  The Edit path used to treat that string as the whole new
file and overwrite the buffer with it.  This drives Edit against a stub `claude`
that returns exactly such an error envelope and asserts the file is untouched and
the error is reported instead of applied.

Red/green: without the guard the trace shows "edit applied" and the buffer's
first line becomes the error text; with it, "edit error (not applied)" and the
original code is intact.
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


def _stub_claude(dir_):
    d = os.path.join(dir_, "bin")
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, "claude")
    with open(p, "w") as f:
        f.write('#!/bin/sh\n'
                'cat >/dev/null 2>&1\n'   # consume the prompt on stdin
                'printf \'%s\' \'{"type":"result","subtype":"error_during_execution",'
                '"is_error":true,"result":"Not logged in - Please run /login",'
                '"session_id":"stub"}\'\n')
    os.chmod(p, 0o755)
    return d


def test_edit_refuses_backend_error(tmp_path):
    bindir = _stub_claude(str(tmp_path))
    trace = tmp_path / "e.trace"
    orig = "int main(void)\n{\n    return 0;\n}\n"
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "claudecli",
        "XWPE_AI_MODEL": "sonnet",
        "PATH": bindir + ":" + os.environ.get("PATH", ""),
        "XWPE_AI_TRACE": str(trace),
    }
    with WpeSession(str(tmp_path), orig, env_extra=env, filename="t.c") as s:
        s.key(ALT.AI); s.key("e"); s._drain(0.4)
        s.key("add a comment"); s.key("\r", delay=0.8)
        # wait for the edit turn to resolve one way or the other
        import time
        end = time.time() + 20
        while time.time() < end:
            s._drain(0.5)
            t = trace.read_text() if trace.exists() else ""
            if "edit error (not applied)" in t or "edit applied" in t:
                break
        disp = "\n".join(s.display())

    txt = trace.read_text() if trace.exists() else ""
    assert "edit error (not applied)" in txt, "error turn was not flagged:\n" + txt
    assert "edit applied" not in txt, "an error was APPLIED to the file:\n" + txt
    # the buffer still holds the original code, not the error string
    assert "int main(void)" in disp, "editor lost the original file:\n" + disp
    # the error is surfaced to the user
    assert "backend error" in disp, "error not reported to the user:\n" + disp
