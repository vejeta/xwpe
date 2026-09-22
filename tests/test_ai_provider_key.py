"""A key typed in the AI settings dialog must not vanish when the active
provider is switched with the Provider picker (Alt-V).

Each OpenAI-compatible provider keeps its own key file
(~/.config/xwpe/openai-api-key-<name>).  Switching providers reloads the key
field from the picked provider's file, so a key that was typed but not yet
confirmed with Ok used to be clobbered and appear lost.  The dialog now
persists a typed key to the currently-active provider's file before the switch
reloads the field.  This test types a key while provider "prova" is active,
switches to "provb", and asserts the key was written to prova's file.
"""
import os
import tempfile

from wpe_driver import WpeSession


def _read_key_file(cfg, provider):
    path = os.path.join(cfg, "openai-api-key-" + provider)
    if not os.path.exists(path):
        return None
    with open(path) as fh:
        return fh.read().strip()


def test_typed_key_survives_provider_switch(tmp_path):
    home = tempfile.mkdtemp()
    cfg = os.path.join(home, ".config", "xwpe")
    os.makedirs(cfg)
    with open(os.path.join(cfg, "xwperc"), "w") as fh:
        fh.write(
            "[Programming]\n"
            "AIBackend : 1\n"                       # OpenAI-compatible
            "AIProviderName : prova\n"
            "AIProvider : prova|http://127.0.0.1:1111/v1|m|\n"
            "AIProvider : provb|http://127.0.0.1:2222/v1|m|\n"
        )
    # No OPENAI_API_KEY: the env var shadows the per-provider key files.
    env = {"XWPE_AI_ENABLE": "1", "HOME": home, "OPENAI_API_KEY": ""}

    secret = "sk-prova-SECRET-123"
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=dict(env)) as s:
        s.key("\033o", delay=0.5); s.key("i", delay=0.6)   # Options > AI
        s.key("\033k", delay=0.4)                          # focus API key field
        for ch in secret:
            s.key(ch, delay=0.02)
        s.key("\033v", delay=0.6)                          # Alt-V -> provider picker
        s.key("\033[B", delay=0.35)                        # Down: prova -> provb
        s.key("\r", delay=0.6)                             # select provb (pre-saves prova key)
        s.key("\033", delay=0.4)                           # leave the dialog

    saved = _read_key_file(cfg, "prova")
    assert saved == secret, (
        "the key typed for provider 'prova' was lost when switching to "
        "'provb'; openai-api-key-prova = %r" % saved)
