"""Options -> AI: the AI settings dialog (enable + backend/model/policy radios).

The dialog is the config home the user chose.  It shows radios for the backend,
the backend's live model list and the permission policy -- each marking the
value in use -- plus an Enable checkbox.  Ok applies the choices to the session
(Save Options persists).  Uses claudecli so the model list is deterministic.
"""
import os
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"AI settings" in out
    except Exception:
        return False


def _tls_build():
    try:
        out = subprocess.run(["ldd", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"libssl" in out or b"libtls" in out
    except Exception:
        return False


def _have_openssl():
    try:
        return subprocess.run(["openssl", "version"], stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL, timeout=10).returncode == 0
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")

ENV = {
    "XWPE_AI_ENABLE": "1",
    "XWPE_AI_BACKEND": "claudecli",
    "XWPE_AI_MODEL": "sonnet",
    "XWPE_AI_POLICY": "ask",
}


def test_ai_options_dialog_renders_current(tmp_path):
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=dict(ENV)) as s:
        s.key("\033o", delay=0.5)        # Options menu
        assert "AI" in "\n".join(s.display()), "no AI entry in Options"
        s.key("i", delay=0.6)            # AI -> dialog
        disp = "\n".join(s.display())
        s.key("\033", delay=0.3)
    # backend + policy are radios; the model is a button showing the current
    # choice (the full list lives in the scrollable picker behind it).  The
    # Model field spells out its Alt-M shortcut so the picker is discoverable.
    for want in ("AI settings", "Backend", "Model (Alt-M)", "Permission",
                 "Claude CLI", "Ollama", "sonnet"):
        assert want in disp, "AI dialog missing %r:\n%s" % (want, disp)
    # current backend is the marked radio
    assert "(*) Claude CLI" in disp.replace("  ", " ").replace(" (", " (") \
        or "(*)Claude CLI" in disp.replace(" ", "") \
        or any("(*)" in ln and "Claude CLI" in ln for ln in disp.splitlines()), \
        "current backend not pre-marked:\n" + disp


def test_ai_options_model_picker_scrolls_and_selects(tmp_path):
    # Alt-M floats the scrollable model picker; Down + Enter selects a different
    # model and the settings dialog reopens -- centred on screen, with the new
    # model on the button.  claudecli's list is deterministic
    # (default/sonnet/opus/haiku), no network.
    trace = tmp_path / "ai.trace"
    env = dict(ENV); env["XWPE_AI_TRACE"] = str(trace)
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key("\033o", delay=0.5)
        s.key("i", delay=0.6)
        s.key("\033m", delay=0.8)        # Alt-M -> scrollable picker
        pick = "\n".join(s.display())
        assert "Model (" in pick and "available)" in pick, \
            "scrollable model picker did not open:\n" + pick
        assert "PgUp/PgDn" in pick, "picker is not the scrollable overlay:\n" + pick
        # the picker floats OVER the settings dialog -- the dialog is NOT erased,
        # its Permission section and Ok/Cancel still show around the picker.
        assert "Permission" in pick and "Cancel" in pick, \
            "the settings dialog was erased instead of drawn under the picker:\n" + pick
        s.key("\033[B", delay=0.3)       # Down: sonnet -> opus
        s.key("\r", delay=0.7)           # Enter -> select, dialog reopens
        back = s.display()
        s.key("\033", delay=0.4)         # leave the reopened dialog
    joined = "\n".join(back)
    txt = trace.read_text() if trace.exists() else ""
    assert "model set" in txt, "picking a model was not recorded:\n" + txt
    # the settings dialog is back (picker closed) with the new model on the
    # button, and it is centred on screen -- the "AI settings" title, the radios
    # and Ok/Cancel are all present, not squeezed into a pane.
    assert "AI settings" in joined and "Permission" in joined and "Cancel" in joined, \
        "the settings dialog did not reopen cleanly:\n" + joined
    assert "PgUp/PgDn" not in joined, "the picker did not close:\n" + joined
    assert any("opus" in ln for ln in back), \
        "the Model button did not show the newly picked model:\n" + joined
    # centred: the dialog's left border is indented well off column 0 (it is not
    # jammed into a corner or the bottom AI pane).
    title_row = next((ln for ln in back if "AI settings" in ln), "")
    assert title_row.index("AI settings") > 20, \
        "the dialog is not centred on screen:\n" + joined


def _ollama_up():
    import urllib.request
    try:
        urllib.request.urlopen("http://localhost:11434/api/tags", timeout=3).read()
        return True
    except Exception:
        return False


@pytest.mark.skipif(not _ollama_up(), reason="no local Ollama for the model list")
def test_model_picker_follows_selected_backend_radio(tmp_path):
    # The exact reported bug: on Claude CLI, switch the Backend radio to Ollama,
    # then Alt-M must list OLLAMA's models -- not Claude's -- WITHOUT pressing Ok
    # first.  Claude CLI's aliases are static (default/sonnet/opus/haiku); their
    # absence from the picker proves the picker followed the selected backend.
    env = dict(ENV); env["XWPE_AI_MODEL"] = ""     # ENV starts on claudecli
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key("\033o", delay=0.5)
        s.key("i", delay=0.6)
        s.key("\t", delay=0.35)          # Enable -> Backend group (on Claude CLI)
        s.key("\033[B", delay=0.35)      # down -> Ollama
        s.key(" ", delay=0.35)           # select Ollama
        s.key("\033m", delay=1.0)        # Alt-M -> picker for the SELECTED backend
        pick = "\n".join(s.display())
    # the Ollama/OpenAI picker title names its endpoint ("Model (N) at <url>")
    assert "Model (" in pick, "the model picker did not open:\n" + pick
    for claude_only in ("sonnet", "opus", "haiku"):
        assert claude_only not in pick, \
            "Alt-M still listed Claude's models after selecting Ollama:\n" + pick


@pytest.mark.skipif(not _ollama_up(), reason="no local Ollama for the model list")
def test_ollama_ignores_leftover_openai_endpoint(tmp_path):
    # Regression: the endpoint field is shared, so switching the Backend radio to
    # Ollama leaves an OpenAI base (e.g. Groq's https .../openai/v1) in it.  Ollama
    # serves /api/... at the host root, so that base would 404.  Alt-M must fall
    # back to the local Ollama server (and the picker title must name it).
    import tempfile
    home = tempfile.mkdtemp()
    cfg = os.path.join(home, ".config", "xwpe")
    os.makedirs(cfg)
    with open(os.path.join(cfg, "xwperc"), "w") as fh:
        fh.write("[Programming]\nAIBackend : 1\n"
                 "AIEndpoint : https://api.groq.com/openai/v1\nAIPolicy : ask\n")
    env = {"XWPE_AI_ENABLE": "1", "HOME": home, "OPENAI_API_KEY": ""}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=dict(env)) as s:
        s.key("\033o", delay=0.5); s.key("i", delay=0.6)
        s.key("\t", delay=0.35)          # Enable -> Backend group (lands on Claude CLI)
        s.key("\033[B", delay=0.35)      # down -> Ollama
        s.key(" ", delay=0.35)           # select Ollama
        s.key("\033m", delay=2.5)        # Alt-M -> picker (lists from the fixed endpoint)
        s.key("\033", delay=0.6)         # close the picker; the field now shows the fix
        disp = s.display()
    field = next((r for r in disp if "http://" in r or "https://" in r), "")
    assert "localhost:11434" in field, \
        "Ollama did not fall back to the local endpoint:\n" + "\n".join(disp)
    assert "api.groq.com" not in field, \
        "Ollama kept the leftover Groq endpoint:\n" + "\n".join(disp)


def test_model_pick_keeps_other_fields(tmp_path):
    # Picking a model must not disturb other fields.  The in-place redraw after
    # the picker used to re-fire the focused field's accelerator and clear the
    # Enable checkbox; it must stay as the user left it.
    def enable(disp):
        return next((ln for ln in disp if "Enable AI" in ln), "")
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=dict(ENV)) as s:
        s.key("\033o", delay=0.5)
        s.key("i", delay=0.6)
        s.key(" ", delay=0.4)            # toggle Enable (focused first)
        assert "[X]" in enable(s.display()), \
            "the Enable checkbox did not toggle on:\n" + "\n".join(s.display())
        s.key("\033m", delay=0.8)        # Alt-M -> picker (claudecli, static list)
        s.key("\033[B", delay=0.3)       # Down
        s.key("\r", delay=0.7)           # Enter -> pick, dialog repaints in place
        after = enable(s.display())
    assert "[X]" in after, \
        "picking a model cleared the Enable checkbox:\n" + after


def test_ai_options_ok_reads_radios(tmp_path):
    trace = tmp_path / "ai.trace"
    env = dict(ENV); env["XWPE_AI_TRACE"] = str(trace)
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key("\033o", delay=0.5)
        s.key("i", delay=0.6)
        s.key(" ", delay=0.3)            # toggle Enable (focused first)
        s.key("\033o", delay=0.6)        # Ok
        s._drain(0.4)
    txt = trace.read_text() if trace.exists() else ""
    line = next((l for l in txt.splitlines() if l.startswith("options ")), "")
    assert "backend=claudecli" in line and "model=sonnet" in line \
        and "policy=ask" in line and "enable=1" in line, \
        "AI dialog did not apply the settings:\n" + txt


def test_ai_options_keyboard_navigation(tmp_path):
    # Tab/arrows must move between the radio groups and Space must select -- the
    # widgets carry unique non-zero sw ids so e_opt_kst can focus them (a zero sw
    # made them unreachable).  Tab to the Backend group, arrow to Ollama, select,
    # Ok -- one Ok APPLIES the choice and closes (no second-Ok reopen).
    trace = tmp_path / "ai.trace"
    env = dict(ENV); env["XWPE_AI_TRACE"] = str(trace)
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key("\033o", delay=0.5)
        s.key("i", delay=0.6)
        s.key("\t", delay=0.35)          # Enable -> Backend group
        s.key("\033[B", delay=0.35)      # down -> Ollama
        s.key(" ", delay=0.35)           # select
        s.key("\033o", delay=0.6)        # Ok -> applies Ollama and CLOSES
        s._drain(0.4)
        closed = "AI settings" not in "\n".join(s.display())
    txt = trace.read_text() if trace.exists() else ""
    line = next((l for l in txt.splitlines() if l.startswith("options ")), "")
    assert "backend=ollama" in line, \
        "Tab/arrow/Space navigation did not reach/select the Ollama radio:\n" + txt
    assert "reload" not in txt, \
        "Ok reopened the dialog instead of saving on the first Ok:\n" + txt
    assert closed, "Ok did not close the dialog on the first press"


def test_ai_options_backend_change_applies_on_ok(tmp_path):
    # Switching the Backend radio and pressing Ok ONCE applies the new backend
    # (no reopen, no lost choice).  Regression guard for the bug where Ok merely
    # reopened the dialog to "reload models", so a user who did not press Ok a
    # second time kept the old backend.
    trace = tmp_path / "ai.trace"
    env = dict(ENV); env["XWPE_AI_TRACE"] = str(trace)   # starts on claudecli
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key("\033o", delay=0.5)
        s.key("i", delay=0.6)
        s.key("\t", delay=0.35)          # Backend group
        s.key("\033[B", delay=0.35)      # Ollama
        s.key(" ", delay=0.35)
        s.key("\033o", delay=0.8)        # Ok -> applies + closes
        s._drain(0.4)
        disp = "\n".join(s.display())
    txt = trace.read_text() if trace.exists() else ""
    line = next((l for l in txt.splitlines() if l.startswith("options ")), "")
    assert "backend=ollama" in line, \
        "Ok did not apply the switched-to backend:\n" + txt
    assert "AI settings" not in disp, \
        "Ok reopened the dialog instead of applying + closing:\n" + disp


def test_ai_options_backend_persists_across_relaunch(tmp_path):
    # The end-to-end guard for the reported bug: change the backend in Options>AI,
    # press Ok, close xwpe, reopen -- the chosen backend must still be marked.  No
    # XWPE_AI_BACKEND override here, so the saved config is authoritative; the
    # config is seeded on claudecli so switching to Ollama is a real change.
    import tempfile
    home = tempfile.mkdtemp()
    cfg = os.path.join(home, ".config", "xwpe")
    os.makedirs(cfg)
    with open(os.path.join(cfg, "xwperc"), "w") as fh:
        fh.write("[Programming]\nAIBackend : 4\nAIModel : sonnet\nAIPolicy : ask\n")
    env = {"XWPE_AI_ENABLE": "1", "HOME": home}     # NB: no XWPE_AI_BACKEND

    # session 1: switch Claude CLI -> Ollama and Ok
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=dict(env)) as s:
        s.key("\033o", delay=0.5); s.key("i", delay=0.6)
        s.key("\t", delay=0.35)          # Enable -> Backend group (Claude CLI)
        s.key("\033[B", delay=0.35)      # down -> Ollama
        s.key(" ", delay=0.35)           # select Ollama
        s.key("\033o", delay=0.8)        # Ok -> applies + saves + closes

    # session 2: same HOME -> the Backend radio must show Ollama, not Claude CLI
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=dict(env)) as s:
        s.key("\033o", delay=0.5); s.key("i", delay=0.6)
        disp = s.display()
    joined = "\n".join(disp)
    assert any("(*) Ollama" in ln for ln in disp) \
        or "(*)Ollama" in joined.replace(" ", ""), \
        "the chosen backend (Ollama) did not persist across relaunch:\n" + joined
    assert not any("(*) Claude CLI" in ln for ln in disp), \
        "the backend reverted to Claude CLI after relaunch:\n" + joined


def _fake_openai_server(models):
    """A throwaway local OpenAI-compatible server: GET /v1/models returns the
    given model ids.  Returns (server, port); call server.shutdown() when done."""
    import threading, json
    from http.server import BaseHTTPRequestHandler, HTTPServer

    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            if self.path.rstrip("/").endswith("/v1/models"):
                body = json.dumps(
                    {"object": "list", "data": [{"id": m} for m in models]}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            else:
                self.send_response(404)
                self.end_headers()

    srv = HTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]


def test_openai_endpoint_lists_from_that_server(tmp_path):
    # The Endpoint field makes OpenAI-compatible usable against ANY server: point
    # it at a local fake OpenAI server and Alt-M lists THAT server's /v1/models --
    # proving the path is not hard-wired to the Ollama endpoint.  No network, no
    # Ollama: the returned ids are distinctive so they cannot come from elsewhere.
    srv, port = _fake_openai_server(["fake-model-alpha", "fake-model-beta"])
    try:
        home = str(tmp_path / "home")
        os.makedirs(os.path.join(home, ".config", "xwpe"))
        with open(os.path.join(home, ".config", "xwpe", "xwperc"), "w") as fh:
            fh.write("[Programming]\nAIBackend : 1\n"
                     "AIEndpoint : http://127.0.0.1:%d\nAIPolicy : ask\n" % port)
        env = {"XWPE_AI_ENABLE": "1", "HOME": home}
        with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                        env_extra=env) as s:
            s.key("\033o", delay=0.5); s.key("i", delay=0.6)
            s.key("\033m", delay=1.3)         # Alt-M -> picker from the fake server
            pick = "\n".join(s.display())
    finally:
        srv.shutdown()
    assert "fake-model-alpha" in pick and "fake-model-beta" in pick, \
        "OpenAI-compatible did not list the endpoint's own models:\n" + pick
    assert ("127.0.0.1:%d" % port) in pick, \
        "the picker did not name the configured endpoint:\n" + pick


def test_ai_options_endpoint_persists(tmp_path):
    # A non-default Endpoint URL survives Ok + relaunch: the field reads back into
    # AIEndpoint, and neither Ok nor a backend change wipes it.
    import tempfile
    home = tempfile.mkdtemp()
    cfg = os.path.join(home, ".config", "xwpe")
    os.makedirs(cfg)
    url = "http://example.test:9999/v1"
    with open(os.path.join(cfg, "xwperc"), "w") as fh:
        fh.write("[Programming]\nAIBackend : 1\nAIEndpoint : %s\nAIPolicy : ask\n" % url)
    env = {"XWPE_AI_ENABLE": "1", "HOME": home}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=dict(env)) as s:
        s.key("\033o", delay=0.5); s.key("i", delay=0.6)
        s.key("\033o", delay=0.6)             # Ok (no edits) -> must keep the URL
    saved = open(os.path.join(cfg, "xwperc")).read()
    assert ("AIEndpoint : " + url) in saved, \
        "the endpoint was not preserved through Ok:\n" + saved


def _path_recording_server():
    """A local OpenAI-compatible server that records the exact request path of
    the last GET .../models it served.  Returns (server, port, seen) where
    seen['path'] is that path.  It answers a /models request at ANY prefix."""
    import threading, json
    from http.server import BaseHTTPRequestHandler, HTTPServer
    seen = {"path": None}

    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            if self.path.endswith("/models"):
                seen["path"] = self.path
                body = json.dumps(
                    {"object": "list", "data": [{"id": "srv-model-1"}]}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            else:
                self.send_response(404)
                self.end_headers()

    srv = HTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1], seen


@pytest.mark.parametrize("suffix,expected", [
    ("",           "/v1/models"),          # bare host -> default /v1 (OpenAI, local)
    ("/v1",        "/v1/models"),           # explicit /v1 (lumo-tamer, OpenAI)
    ("/openai/v1", "/openai/v1/models"),    # Groq
    ("/api/v1",    "/api/v1/models"),       # OpenRouter
])
def test_openai_endpoint_honors_path_prefix(tmp_path, suffix, expected):
    # The OpenAI-compatible path is taken relative to the endpoint's own path, so
    # a base URL may carry the version segment (Groq /openai/v1, OpenRouter
    # /api/v1); a bare host still defaults to /v1.  The fake server records the
    # exact path xwpe requested so the mapping is asserted, not assumed.
    import tempfile
    srv, port, seen = _path_recording_server()
    try:
        endpoint = "http://127.0.0.1:%d%s" % (port, suffix)
        home = tempfile.mkdtemp()
        cfg = os.path.join(home, ".config", "xwpe")
        os.makedirs(cfg)
        with open(os.path.join(cfg, "xwperc"), "w") as fh:
            fh.write("[Programming]\nAIBackend : 1\nAIEndpoint : %s\nAIPolicy : ask\n"
                     % endpoint)
        env = {"XWPE_AI_ENABLE": "1", "HOME": home}
        with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                        env_extra=dict(env)) as s:
            s.key("\033o", delay=0.5); s.key("i", delay=0.6)
            s.key("\033m", delay=1.1)
            listed = "srv-model-1" in "\n".join(s.display())
    finally:
        srv.shutdown()
    assert seen["path"] == expected, \
        "endpoint %s requested %r, expected %r" % (endpoint, seen["path"], expected)
    assert listed, "the model from %s was not listed" % endpoint


def _self_signed_https_server(tmp_path, models):
    """A local HTTPS OpenAI server with a fresh self-signed cert for 127.0.0.1
    (like a local Proton bridge).  Returns (server, port, cert_path)."""
    import threading, json, ssl
    from http.server import BaseHTTPRequestHandler, HTTPServer
    cert = str(tmp_path / "cert.pem")
    key = str(tmp_path / "key.pem")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048",
                    "-keyout", key, "-out", cert, "-days", "2", "-nodes",
                    "-subj", "/CN=127.0.0.1",
                    "-addext", "subjectAltName=IP:127.0.0.1"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            if self.path.endswith("/models"):
                b = json.dumps({"object": "list",
                                "data": [{"id": m} for m in models]}).encode()
                self.send_response(200)
                self.send_header("Content-Length", str(len(b)))
                self.end_headers()
                self.wfile.write(b)
            else:
                self.send_response(404)
                self.end_headers()

    srv = HTTPServer(("127.0.0.1", 0), H)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1], cert


@pytest.mark.skipif(not _tls_build(), reason="wpe built without TLS")
@pytest.mark.skipif(not _have_openssl(), reason="openssl CLI not available")
def test_aicafile_trusts_a_self_signed_https_endpoint(tmp_path):
    # A local HTTPS bridge (proton-cli serves Proton Lumo on 127.0.0.1 with a
    # self-signed cert) is REJECTED by default -- xwpe verifies certs -- and
    # accepted only when AICAFile points at that cert.  Verification still runs;
    # this is not a blanket skip-verify.
    srv, port, cert = _self_signed_https_server(tmp_path, ["selfsigned-model-1"])
    endpoint = "https://127.0.0.1:%d/v1" % port

    def run(with_cafile):
        home = str(tmp_path / ("home_%d" % with_cafile))
        os.makedirs(os.path.join(home, ".config", "xwpe"))
        conf = ("[Programming]\nAIBackend : 1\nAIEndpoint : %s\n" % endpoint)
        if with_cafile:
            conf += "AICAFile : %s\n" % cert
        conf += "AIPolicy : ask\n"
        with open(os.path.join(home, ".config", "xwpe", "xwperc"), "w") as fh:
            fh.write(conf)
        env = {"XWPE_AI_ENABLE": "1", "HOME": home, "OPENAI_API_KEY": ""}
        with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                        env_extra=dict(env)) as s:
            s.key("\033o", delay=0.5); s.key("i", delay=0.6)
            s.key("\033m", delay=3.0)
            return "selfsigned-model-1" in "\n".join(s.display())

    try:
        without = run(0)
        withca = run(1)
    finally:
        srv.shutdown()
    assert not without, \
        "the self-signed endpoint was trusted WITHOUT AICAFile (verify bypassed!)"
    assert withca, \
        "AICAFile did not let xwpe verify and use the self-signed endpoint"


def test_provider_profiles_pick_and_persist(tmp_path):
    # Provider profiles: Options > AI lists saved OpenAI-compatible providers
    # (Alt-V), and picking one loads its endpoint/model and switches the backend
    # to OpenAI-compatible.  The choice AND the provider list persist.
    import tempfile
    home = tempfile.mkdtemp()
    cfg = os.path.join(home, ".config", "xwpe")
    os.makedirs(cfg)
    cfgfile = os.path.join(cfg, "xwperc")
    with open(cfgfile, "w") as fh:
        fh.write("[Programming]\nAIBackend : 0\nAIEndpoint : http://localhost:11434\n"
                 "AIProvider : groq|https://api.groq.com/openai/v1|openai/gpt-oss-120b|\n"
                 "AIProvider : local8080|http://localhost:8080/v1||\n"
                 "AIPolicy : ask\n")
    env = {"XWPE_AI_ENABLE": "1", "HOME": home, "OPENAI_API_KEY": ""}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=dict(env)) as s:
        s.key("\033o", delay=0.5); s.key("i", delay=0.6)
        s.key("\033v", delay=1.0)         # Alt-V -> provider picker
        pick = "\n".join(s.display())
        s.key("\r", delay=0.7)            # pick the first (groq)
        s.key("\033o", delay=0.8)         # Ok
    for want in ("groq", "local8080", "Save current"):
        assert want in pick, "provider picker missing %r:\n%s" % (want, pick)
    saved = open(cfgfile).read()
    assert "AIProviderName : groq" in saved, "active provider not saved:\n" + saved
    assert "AIEndpoint : https://api.groq.com/openai/v1" in saved, \
        "the provider's endpoint was not applied/saved:\n" + saved
    assert ("AIProvider : groq|https://api.groq.com/openai/v1|openai/gpt-oss-120b|"
            in saved and "AIProvider : local8080|" in saved), \
        "the provider list did not persist:\n" + saved


def _auth_recording_server():
    """A local OpenAI server that records the Authorization header of the last
    GET .../models it served.  Returns (server, port, seen)."""
    import threading, json
    from http.server import BaseHTTPRequestHandler, HTTPServer
    seen = {"auth": None}

    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            if self.path.endswith("/models"):
                seen["auth"] = self.headers.get("Authorization")
                b = json.dumps({"object": "list", "data": [{"id": "m1"}]}).encode()
                self.send_response(200)
                self.send_header("Content-Length", str(len(b)))
                self.end_headers()
                self.wfile.write(b)
            else:
                self.send_response(404)
                self.end_headers()

    srv = HTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1], seen


def test_provider_uses_its_own_key_file(tmp_path):
    # A per-provider key file (openai-api-key-<name>) is used when that provider
    # is active, so Groq/Proton/... can each carry a distinct key.
    srv, port, seen = _auth_recording_server()
    try:
        ep = "http://127.0.0.1:%d/v1" % port
        home = str(tmp_path / "home")
        cfg = os.path.join(home, ".config", "xwpe")
        os.makedirs(cfg)
        with open(os.path.join(cfg, "xwperc"), "w") as fh:
            fh.write("[Programming]\nAIBackend : 1\nAIEndpoint : %s\n"
                     "AIProviderName : myprov\nAIProvider : myprov|%s||\n"
                     "AIPolicy : ask\n" % (ep, ep))
        with open(os.path.join(cfg, "openai-api-key-myprov"), "w") as fh:
            fh.write("secret-key-123\n")
        env = {"XWPE_AI_ENABLE": "1", "HOME": home, "OPENAI_API_KEY": ""}
        with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                        env_extra=dict(env)) as s:
            s.key("\033o", delay=0.5); s.key("i", delay=0.6)
            s.key("\033m", delay=1.5)     # Alt-M -> lists models (sends the key)
    finally:
        srv.shutdown()
    assert seen["auth"] == "Bearer secret-key-123", \
        "the per-provider key file was not used: %r" % seen["auth"]


def test_provider_save_current_as_new(tmp_path):
    # "[ + Save current as... ]" in the provider picker prompts for a name and
    # saves the current endpoint/model as a new profile, which then persists.
    import tempfile
    home = tempfile.mkdtemp()
    cfg = os.path.join(home, ".config", "xwpe")
    os.makedirs(cfg)
    cfgfile = os.path.join(cfg, "xwperc")
    with open(cfgfile, "w") as fh:
        fh.write("[Programming]\nAIBackend : 1\n"
                 "AIEndpoint : https://api.groq.com/openai/v1\nAIModel : gpt-x\n"
                 "AIPolicy : ask\n")
    env = {"XWPE_AI_ENABLE": "1", "HOME": home, "OPENAI_API_KEY": ""}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=dict(env)) as s:
        s.key("\033o", delay=0.5); s.key("i", delay=0.6)
        s.key("\033v", delay=1.0)         # Alt-V -> picker (only the Save entry)
        s.key("\r", delay=0.7)            # pick "[ + Save current as... ]"
        for ch in "mygroq":
            s.key(ch, delay=0.04)         # type the name
        s.key("\r", delay=0.7)            # Enter confirms the name
        s.key("\033o", delay=0.8)         # Ok
    saved = open(cfgfile).read()
    assert "AIProviderName : mygroq" in saved, \
        "the saved provider was not made active:\n" + saved
    assert "AIProvider : mygroq|https://api.groq.com/openai/v1|gpt-x|" in saved, \
        "the current settings were not saved as a named provider:\n" + saved


def test_cafile_field_edits_and_persists(tmp_path):
    # The CA file is editable in the dialog (Alt-C), so a self-signed HTTPS bridge
    # (Proton Lumo) is configurable from the UI without hand-editing xwperc.
    import tempfile
    home = tempfile.mkdtemp()
    cfg = os.path.join(home, ".config", "xwpe")
    os.makedirs(cfg)
    cfgfile = os.path.join(cfg, "xwperc")
    with open(cfgfile, "w") as fh:
        fh.write("[Programming]\nAIBackend : 1\n"
                 "AIEndpoint : https://127.0.0.1:8443/v1\nAIPolicy : ask\n")
    env = {"XWPE_AI_ENABLE": "1", "HOME": home, "OPENAI_API_KEY": ""}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=dict(env)) as s:
        s.key("\033o", delay=0.5); s.key("i", delay=0.6)
        s.key("\033c", delay=0.5)         # Alt-C -> focus the CA file field
        for ch in "/tmp/proton.pem":
            s.key(ch, delay=0.03)
        s.key("\r", delay=0.4)            # commit the field
        s.key("\033o", delay=0.8)         # Ok
    saved = open(cfgfile).read()
    assert "AICAFile : /tmp/proton.pem" in saved, \
        "the CA file typed in the dialog was not saved:\n" + saved
