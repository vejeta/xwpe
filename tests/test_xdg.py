"""Config and state follow the XDG Base Directory spec.

- The user config goes to $XDG_CONFIG_HOME/xwpe/xwperc (default ~/.config/xwpe/).
- A legacy ~/.xwpe/xwperc is migrated (copied) the first time so nothing is lost.
- AI runtime state (sessions, checkpoints) goes to $XDG_STATE_HOME/xwpe/
  (default ~/.local/state/xwpe/).

HOME is redirected to a tmp dir so each case is isolated.
"""
import glob
import os
import subprocess
import tempfile

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


# Force the XDG defaults (~/.config, ~/.local/state) regardless of the runner's
# environment: an empty value makes xwpe fall back to the default location.
_XDG = {"XDG_CONFIG_HOME": "", "XDG_STATE_HOME": ""}


def _cfg(home):
    return os.path.join(home, ".config", "xwpe", "xwperc")


def test_config_written_under_xdg(tmp_path):
    home = tempfile.mkdtemp()
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "HOME": home, **_XDG}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=dict(env), filename="t.c") as s:
        s.key(ALT.AI); s._drain(0.4)
        s.key("y"); s._drain(0.6)                 # cycle permissions -> auto-saves
    assert os.path.exists(_cfg(home)), \
        "config was not written under ~/.config/xwpe/"
    assert "AIPolicy : edits" in open(_cfg(home)).read()


def test_legacy_config_is_migrated(tmp_path):
    # make a real config, relocate it to the legacy ~/.xwpe/ path, then a fresh
    # run must migrate it to XDG and load its value.
    src_home = tempfile.mkdtemp()
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "HOME": src_home, **_XDG}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=dict(env), filename="t.c") as s:
        s.key(ALT.AI); s._drain(0.4); s.key("y"); s._drain(0.6)
    cfg = open(_cfg(src_home)).read().replace("AIPolicy : edits", "AIPolicy : auto")

    home = tempfile.mkdtemp()
    os.makedirs(os.path.join(home, ".xwpe"))
    open(os.path.join(home, ".xwpe", "xwperc"), "w").write(cfg)
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra={"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
                               "HOME": home, **_XDG}, filename="t.c") as s:
        s.key(ALT.AI); s._drain(0.6)
        disp = "\n".join(s.display())
    assert os.path.exists(_cfg(home)), "legacy config was not migrated to XDG"
    assert "Permissions: auto" in disp, "migrated config value was not loaded"
    # the old directory is kept as a backup, with a note pointing to the new path
    note = os.path.join(home, ".xwpe", "MOVED_TO_XDG.txt")
    assert os.path.exists(os.path.join(home, ".xwpe", "xwperc")), \
        "the legacy config was not kept as a backup"
    assert os.path.exists(note), "no migration note was left in the old directory"
    assert "XDG" in open(note).read() and ".config/xwpe" in open(note).read(), \
        "the migration note does not explain the new location"


def test_ai_state_under_xdg_state(tmp_path):
    home = tempfile.mkdtemp()
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": "hi", "HOME": home, **_XDG}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=dict(env), filename="t.c") as s:
        s.key(ALT.AI); s.key("a"); s.key("hola"); s.key("\r", delay=1.0)
        s._drain(1.0)
    sessions = glob.glob(os.path.join(home, ".local", "state", "xwpe", "ai", "*.session"))
    assert sessions, "AI session was not written under ~/.local/state/xwpe/"
    assert not os.path.exists(os.path.join(home, ".xwpe", "ai")), \
        "AI state still went to the legacy ~/.xwpe/ path"
