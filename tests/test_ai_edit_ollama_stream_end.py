"""An Ollama Edit reply that streams to its end opens the review, and says so.

The live Ollama Edit test once waited eleven minutes for "edit applied" with a
trace that stopped at "edit stream fd=8 (async)".  The reply had in fact come
whole, "done":true and all, and the hunk review box stood on the screen waiting
for a key: the test's "a", typed while the reply was still streaming, had gone
to the file window, which is locked during an Edit and refuses the key.  The
trace said nothing between the stream start and the user's answer, so a review
waiting for the user read exactly like a stream that never ended.

The editor now traces "edit review" with the number of changes when the box
opens.  This test plays Ollama's /api/chat from a local server: chunked
NDJSON, every object split across two chunks so no read ends on a line
boundary, "done":true in the last object.  It checks that the review opens and
is traced, that a key typed during the stream applies nothing, and that "a"
in the box applies the change.

Red/green: before the trace line, the wait for "edit review" times out.
"""
import json
import os
import socket
import subprocess
import threading
import time

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN, WAIT_SCALE

BUGGY = ("#include <stdio.h>\nint main(void)\n{\n    int x = 40 + 2\n"
         "    printf(\"%d\\n\", x);\n    return 0;\n}\n")
FIXED = BUGGY.replace("40 + 2\n", "40 + 2;\n")
BOX = "Proposed change"
STEP_S = 0.5
WAIT_S = 20


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _objects(text, piece=7):
    """Ollama's /api/chat stream for a reply of `text`, one line per object."""
    out = [{"model": "fake", "message": {"role": "assistant",
                                         "content": text[i:i + piece]},
            "done": False}
           for i in range(0, len(text), piece)]
    out.append({"model": "fake", "message": {"role": "assistant", "content": ""},
                "done_reason": "stop", "done": True})
    return [(json.dumps(o) + "\n").encode() for o in out]


def _chunk(data):
    return b"%x\r\n" % len(data) + data + b"\r\n"


class _FakeOllama(threading.Thread):
    """Answer each POST /api/chat with a chunked NDJSON stream of `reply`.

    The stream starts after `delay` seconds and every object is cut in two
    chunks sent apart, the way a slow model's tokens arrive.
    """

    def __init__(self, reply, delay):
        super().__init__(daemon=True)
        self._reply = reply
        self._delay = delay
        self._sock = socket.socket()
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind(("127.0.0.1", 0))
        self._sock.listen(4)
        self._sock.settimeout(STEP_S)
        self._stop = threading.Event()
        self.sent_done = threading.Event()

    @property
    def url(self):
        return "http://127.0.0.1:%d" % self._sock.getsockname()[1]

    def _serve(self, conn):
        data = b""
        while b"\r\n\r\n" not in data:
            got = conn.recv(65536)
            if not got:
                return
            data += got
        head, body = data.split(b"\r\n\r\n", 1)
        length = 0
        for line in head.split(b"\r\n")[1:]:
            name, _, value = line.partition(b":")
            if name.strip().lower() == b"content-length":
                length = int(value.strip())
        while len(body) < length:
            got = conn.recv(65536)
            if not got:
                break
            body += got
        conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/x-ndjson\r\n"
                     b"Transfer-Encoding: chunked\r\n\r\n")
        time.sleep(self._delay)
        for obj in _objects(self._reply):
            half = len(obj) // 2
            conn.sendall(_chunk(obj[:half]))
            time.sleep(0.02)
            conn.sendall(_chunk(obj[half:]))
        conn.sendall(b"0\r\n\r\n")
        self.sent_done.set()

    def run(self):
        while not self._stop.is_set():
            try:
                conn, _ = self._sock.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            with conn:
                try:
                    self._serve(conn)
                except OSError:
                    pass

    def stop(self):
        self._stop.set()
        self.join(timeout=5)
        self._sock.close()


def _trace(path):
    return path.read_text() if path.exists() else ""


def _wait_trace(session, path, needle, budget=WAIT_S):
    deadline = time.monotonic() + budget * WAIT_SCALE
    while time.monotonic() < deadline:
        session._drain(STEP_S)
        if needle in _trace(path):
            return True
    return False


def test_streamed_edit_reply_opens_a_traced_review(tmp_path):
    trace = tmp_path / "ai.trace"
    srv = _FakeOllama(FIXED, delay=2.0 * WAIT_SCALE)
    srv.start()
    try:
        env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "ollama",
               "XWPE_AI_ENDPOINT": srv.url, "XWPE_AI_MODEL": "fake",
               "XWPE_AI_TRACE": str(trace)}
        with WpeSession(str(tmp_path), BUGGY, filename="bug.c",
                        env_extra=env) as s:
            s.key(ALT.AI)
            s.key("e")
            s.key("Fix the compile error.")
            s.key("\r", delay=0.5)
            s.key("a", delay=0.5)        # typed while the reply streams
            assert _wait_trace(s, trace, "edit review"), \
                "the review never opened (or was not traced):\n" + _trace(trace)
            assert srv.sent_done.is_set(), "the review opened before the reply ended"
            assert "edit review 1 change" in _trace(trace), _trace(trace)
            assert BOX in "\n".join(s.display()), "the review box is not shown"
            assert "edit applied" not in _trace(trace), \
                "the key typed during the stream applied the edit"
            s.key("a", delay=0.5)        # accept all, now that the box is up
            assert _wait_trace(s, trace, "edit applied 1/1"), _trace(trace)
            s._drain(0.5)
            s.save()
            assert s.text() == FIXED, s.text()
    finally:
        srv.stop()
