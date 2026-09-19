"""Cloud-backend TLS transport, tested against a local HTTPS server.

The OpenAI/Claude backends talk HTTPS and verify the server certificate against
the system trust store plus the hostname (SSL_set1_host).  There is no insecure
bypass, so this test exercises the REAL path: a self-signed "localhost"
certificate is generated, a tiny TLS server streams an OpenAI-style SSE reply,
and xwpe is pointed at it with SSL_CERT_FILE set to that certificate so
verification legitimately succeeds.  A marker token in the reply proves the TLS
handshake, the request, and the SSE framer all work end to end -- no cloud
account, key, or network required.

Skips when the binary was built without TLS (--enable-ai-tls) or openssl is not
available to mint the certificate.
"""
import os
import ssl
import time
import socket
import shutil
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

MARKER = "TLS_MARKER_OK"
SSE_BODY = (
    'data: {"choices":[{"delta":{"content":"%s"}}]}\n\n'
    'data: {"choices":[{"finish_reason":"stop","delta":{}}]}\n\n'
    'data: [DONE]\n\n'
) % MARKER


def _tls_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        # this string is compiled only in the OpenSSL TLS branch
        return b"TLS handshake/verify failed" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(
    not _tls_build() or shutil.which("openssl") is None,
    reason="wpe built without --enable-ai-tls, or openssl missing")


class _Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        if length:
            self.rfile.read(length)
        body = SSE_BODY.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


def _make_cert(dir_):
    cert = os.path.join(dir_, "cert.pem")
    key = os.path.join(dir_, "key.pem")
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
         "-keyout", key, "-out", cert, "-days", "1",
         "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost"],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return cert, key


def _serve(cert, key):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    httpd = ThreadingHTTPServer(("127.0.0.1", 0), _Handler)
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd, httpd.socket.getsockname()[1]


def _run(tmp_path, port, trust_cert):
    trace = tmp_path / "ai.trace"
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "openai",
        "XWPE_AI_ENDPOINT": "https://localhost:%d" % port,
        "XWPE_AI_MODEL": "gpt-test",
        "XWPE_AI_TRACE": str(trace),
    }
    if trust_cert:
        env["SSL_CERT_FILE"] = trust_cert       # make the self-signed cert trusted
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key(ALT.AI); s.key("a", delay=0.6)
        s.key("hello"); s.key("\r", delay=1.0)
        end = time.time() + 12
        while time.time() < end:
            s._drain(0.6)
            if MARKER in "\n".join(s.display()):
                break
        return "\n".join(s.display())


def test_openai_over_tls_trusted_cert_streams(tmp_path):
    # With the cert trusted, the handshake verifies and the SSE reply renders.
    cert, key = _make_cert(str(tmp_path))
    httpd, port = _serve(cert, key)
    try:
        disp = _run(tmp_path, port, trust_cert=cert)
    finally:
        httpd.shutdown()
    assert MARKER in disp, \
        "the TLS SSE reply did not render (transport/framer/verify):\n" + disp


def test_openai_over_tls_untrusted_cert_rejected(tmp_path):
    # The same server, but the cert is NOT trusted: verification must reject the
    # connection so nothing is exchanged.  Guards against the client silently
    # accepting any certificate (a MITM hole).
    cert, key = _make_cert(str(tmp_path))
    httpd, port = _serve(cert, key)
    try:
        disp = _run(tmp_path, port, trust_cert=None)
    finally:
        httpd.shutdown()
    assert MARKER not in disp, \
        "an UNTRUSTED certificate was accepted -- TLS verification is not enforced:\n" + disp
