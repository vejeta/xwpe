"""Options > AI: what the dialog persists, and what it must never persist.

The AI settings dialog is the one place a user configures the assistant, so
what it writes to the config file (xwperc) and to the key files is the whole
contract.  These tests cover that contract from the outside:

- Ok writes the Enable bit into Options2 and the radios (backend, policy, agent
  engine) into their [Programming] keys, exactly once per Ok.
- Cancel (Esc) leaves xwperc byte-identical and writes no key file, in the main
  dialog and in the Advanced sub-dialog alike.
- An API key goes to its key file and never into xwperc.
- XWPE_AI_* environment overrides are what the dialog shows and what the
  session uses; an Ok without edits keeps the file's own values, and only a
  value the user changed in the dialog is written.
- A provider name that cannot be stored (too long, '|' or '/') is refused.

Non-default radios are seeded through the config file written BEFORE launch
(HOME is redirected to a fresh directory, so ~/.config/xwpe/xwperc is ours),
because that is how a real user's saved choice reaches the dialog.  Every
assertion reads the file on disk or the rendered screen, never the sources.
"""
import ctypes
import os
import subprocess
import sys

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN, title_pos


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"AI settings" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")

SEED = "int main(void){return 0;}\n"
ENTER = "\r"
ESC = "\033"
OPTIONS2_AI_ENABLE = 0x0200          # the Enable box's bit in [General] Options2
IN_MODIFY = 0x00000002               # inotify: content changed (truncate, write)
IN_CLOSE_WRITE = 0x00000008          # inotify: a file opened for writing was closed
IN_OPEN = 0x00000020                 # inotify: the file was opened


def alt(ch):
    """The Alt-<ch> key sequence (ESC-prefixed, as the dialog accelerators are)."""
    return "\033" + ch


# -- config file on disk ------------------------------------------------------

def cfg_dir(home):
    d = os.path.join(home, ".config", "xwpe")
    os.makedirs(d, exist_ok=True)
    return d


def xwperc_path(home):
    return os.path.join(cfg_dir(home), "xwperc")


def write_xwperc(home, programming, general=()):
    """Seed ~/.config/xwpe/xwperc with the given [General] and [Programming]
    lines (each 'Key : value'), so the dialog opens on a non-default state."""
    lines = []
    if general:
        lines.append("[General]")
        lines.extend(general)
    lines.append("[Programming]")
    lines.extend(programming)
    with open(xwperc_path(home), "w") as fh:
        fh.write("\n".join(lines) + "\n")


def read_xwperc(home):
    with open(xwperc_path(home)) as fh:
        return fh.read()


def read_xwperc_bytes(home):
    with open(xwperc_path(home), "rb") as fh:
        return fh.read()


def xwperc_value(text, key):
    """The value of the first 'key : value' line, '' for an empty value, None
    when the key is absent."""
    for line in text.splitlines():
        if line.startswith(key + " :"):
            return line[len(key) + 2:].strip()
    return None


def key_file_path(home, provider=None):
    name = "openai-api-key" + ("-" + provider if provider else "")
    return os.path.join(cfg_dir(home), name)


def read_key_file(home, provider=None):
    path = key_file_path(home, provider)
    if not os.path.exists(path):
        return None
    with open(path) as fh:
        return fh.read().strip()


# -- driving the dialog -------------------------------------------------------

def session(tmp_path, home, **env_extra):
    """A wpe session whose config lives under `home`.  XDG_CONFIG_HOME is
    blanked so the file is ~/.config/xwpe/xwperc regardless of the runner's
    environment; OPENAI_API_KEY is blanked so no outside key shadows the key
    files under test."""
    env = {"HOME": home, "XDG_CONFIG_HOME": "", "OPENAI_API_KEY": ""}
    env.update(env_extra)
    return WpeSession(str(tmp_path), SEED, env_extra=env)


def dialog_open(disp):
    return any("AI settings" in ln for ln in disp)


def open_dialog(s):
    """Options > AI; returns the rendered dialog rows."""
    s.key(ALT.OPTIONS, delay=0.5)
    s.key("i", delay=0.6)
    disp = s.display()
    assert dialog_open(disp), "the AI settings dialog did not open:\n" + "\n".join(disp)
    return disp


def press_ok(s):
    s.key(alt("o"), delay=0.8)
    s._drain(0.4)


def press_cancel(s):
    s.key(ESC, delay=0.6)


def leave_dialog_if_open(s):
    """Esc the main dialog if it is still on screen (the Advanced sub-dialog
    takes the main one down with it, so a blind Esc would go to the editor)."""
    if dialog_open(s.display(drain=0.2)):
        press_cancel(s)


def type_text(s, text):
    for ch in text:
        s.key(ch, delay=0.03)


def radio_marked(disp, label):
    """True when the radio whose caption starts with `label` is the marked one."""
    return any(("(*) " + label) in ln for ln in disp)


def enable_row(disp):
    return next((ln for ln in disp if "Enable AI assistant" in ln), "")


def row_with(disp, text):
    return next((ln for ln in disp if text in ln), "")


def select_backend_radio(s, downs):
    """Tab from the Enable box into the Backend group (focus lands on its first
    entry, Claude CLI), move Down `downs` times and Space-select."""
    s.key("\t", delay=0.35)
    for _ in range(downs):
        s.key("\033[B", delay=0.35)
    s.key(" ", delay=0.35)


def count_close_writes(watch_dir, name, action):
    """Run action() and return how many times <watch_dir>/<name> was closed
    after being opened for writing meanwhile: one IN_CLOSE_WRITE per
    fopen(.., "w")/fclose rewrite.  Polling st_mtime_ns cannot tell one rewrite
    from two (the O_TRUNC and the final write each bump it), inotify can.

    The watch also takes IN_OPEN and IN_MODIFY, not because they are counted,
    but because the kernel merges IDENTICAL successive unread events: two
    back-to-back rewrites watched for IN_CLOSE_WRITE alone collapse into one.
    With open/modify events in between, every close-write stays distinct."""
    libc = ctypes.CDLL("libc.so.6", use_errno=True)
    fd = libc.inotify_init1(os.O_NONBLOCK)
    if fd < 0:
        pytest.skip("inotify unavailable on this kernel")
    try:
        mask = IN_OPEN | IN_MODIFY | IN_CLOSE_WRITE
        if libc.inotify_add_watch(fd, watch_dir.encode(), mask) < 0:
            pytest.skip("inotify watch refused for " + watch_dir)
        action()
        return sum(1 for ev_name, ev_mask in inotify_events(fd)
                   if ev_name == name and ev_mask & IN_CLOSE_WRITE)
    finally:
        os.close(fd)


def inotify_events(fd):
    """Drain the queued inotify events as (file name, mask) pairs."""
    import struct
    try:
        buf = os.read(fd, 65536)
    except BlockingIOError:
        return []
    events, i = [], 0
    while i < len(buf):
        _wd, ev_mask, _cookie, ln = struct.unpack_from("iIII", buf, i)
        i += 16
        events.append((buf[i:i + ln].split(b"\0", 1)[0].decode(), ev_mask))
        i += ln
    return events


# -- tests --------------------------------------------------------------------

# @scenario SC-ai-options-11
def test_enable_sets_options2_bit(tmp_path):
    # A fresh profile (no xwperc) saved as-is has the Enable bit clear; ticking
    # Enable (Alt-E shows [X]) and pressing Ok sets bit 0x0200 of Options2.
    home = str(tmp_path / "home")
    with session(tmp_path, home) as s:
        open_dialog(s)
        press_ok(s)                                  # save the defaults
    fresh = xwperc_value(read_xwperc(home), "Options2")
    assert fresh is not None and not (int(fresh) & OPTIONS2_AI_ENABLE), \
        "a fresh profile already has the Enable bit set: Options2 = %r" % fresh

    with session(tmp_path, home) as s:
        open_dialog(s)
        s.key(alt("e"), delay=0.4)                   # tick Enable
        row = enable_row(s.display())
        assert "[X]" in row, "Alt-E did not tick the Enable box: " + row
        press_ok(s)
    saved = xwperc_value(read_xwperc(home), "Options2")
    assert saved is not None and int(saved) & OPTIONS2_AI_ENABLE, \
        "Ok with Enable ticked did not set bit 0x0200: Options2 = %r (was %r)" \
        % (saved, fresh)


# @scenario SC-ai-options-12
def test_env_force_enable_overrides_file(tmp_path):
    # Options2 bit clear in the file, XWPE_AI_ENABLE=1 in the environment: the
    # assistant answers Alt-G (no "is off" refusal) and the dialog shows [X].
    home = str(tmp_path / "home")
    write_xwperc(home, ["AIBackend : 4", "AIPolicy : ask"], general=["Options2 : 152"])
    with session(tmp_path, home, XWPE_AI_ENABLE="1", XWPE_AI_BACKEND="mock") as s:
        s.key(ALT.AI, delay=0.8)
        menu = s.display()
        s.key(ESC, delay=0.4)
        disp = open_dialog(s)
        press_cancel(s)
    joined = "\n".join(menu)
    assert "AI assistant is off" not in joined, \
        "Alt-G refused although XWPE_AI_ENABLE=1:\n" + joined
    assert "AI settings..." in joined, "Alt-G did not open the AI menu:\n" + joined
    row = enable_row(disp)
    assert "[X]" in row, \
        "the dialog shows Enable unticked while XWPE_AI_ENABLE=1 forces it on: " + row


# @scenario SC-ai-options-15
def test_backend_switch_clears_model(tmp_path):
    # The file says Ollama with a model name; switching the Backend radio to
    # OpenAI-compatible and pressing Ok must drop that model (names are
    # backend-specific) so the saved AIModel is empty.
    home = str(tmp_path / "home")
    write_xwperc(home, ["AIBackend : 0", "AIModel : qwen3:8b", "AIPolicy : ask"])
    with session(tmp_path, home) as s:
        disp = open_dialog(s)
        assert radio_marked(disp, "Ollama"), "the seeded backend is not marked:\n" + "\n".join(disp)
        assert "qwen3:8b" in row_with(disp, "Model (Alt-M)"), \
            "the seeded model is not on the Model button:\n" + "\n".join(disp)
        select_backend_radio(s, downs=2)             # Claude CLI -> Ollama -> OpenAI
        moved = s.display()
        assert radio_marked(moved, "OpenAI-compatible"), \
            "the Backend radio did not move to OpenAI-compatible:\n" + "\n".join(moved)
        press_ok(s)
    saved = read_xwperc(home)
    assert xwperc_value(saved, "AIBackend") == "1", \
        "the switched-to backend was not saved:\n" + saved
    assert xwperc_value(saved, "AIModel") in ("", None), \
        "the old backend's model survived the backend switch:\n" + saved


@pytest.mark.parametrize("policy,label", [
    ("edits", "Auto-accept edits"),
    ("auto", "Auto (skip asks)"),
])
# @scenario SC-ai-options-17
def test_policy_change_via_dialog_writes_aipolicy(tmp_path, policy, label):
    # A saved non-default policy is the marked Permission radio, and Ok writes
    # it back as the same AIPolicy word.
    home = str(tmp_path / "home")
    write_xwperc(home, ["AIBackend : 4", "AIPolicy : " + policy])
    with session(tmp_path, home) as s:
        disp = open_dialog(s)
        assert radio_marked(disp, label), \
            "AIPolicy : %s did not mark %r:\n%s" % (policy, label, "\n".join(disp))
        press_ok(s)
    saved = read_xwperc(home)
    assert xwperc_value(saved, "AIPolicy") == policy, \
        "Ok did not write AIPolicy : %s:\n%s" % (policy, saved)


# @scenario SC-ai-options-33
def test_key_never_written_to_xwperc(tmp_path):
    # A key typed in the API key field (Alt-K) lands in the 0600 key file and
    # nowhere in xwperc, which is world-readable configuration.
    home = str(tmp_path / "home")
    secret = "sk-never-in-xwperc-42"
    write_xwperc(home, ["AIBackend : 1", "AIEndpoint : http://127.0.0.1:1/v1",
                        "AIPolicy : ask"])
    with session(tmp_path, home) as s:
        open_dialog(s)
        s.key(alt("k"), delay=0.5)
        type_text(s, secret)
        assert secret in row_with(s.display(), "API key"), "the key was not typed into the field"
        press_ok(s)
    assert read_key_file(home) == secret, \
        "the typed key was not persisted to the key file: %r" % read_key_file(home)
    saved = read_xwperc(home)
    assert secret not in saved, "the API key leaked into xwperc:\n" + saved


# @scenario SC-ai-options-38
def test_save_current_as_moves_session_key(tmp_path):
    # Type a key, Alt-V, pick "[ + Save current as... ]", name it, Enter, Ok:
    # the key must end up in openai-api-key-<name>.
    home = str(tmp_path / "home")
    secret = "sk-moved-key-777"
    write_xwperc(home, ["AIBackend : 1", "AIEndpoint : https://api.groq.com/openai/v1",
                        "AIModel : gpt-x", "AIPolicy : ask"])
    with session(tmp_path, home) as s:
        open_dialog(s)
        s.key(alt("k"), delay=0.5)
        type_text(s, secret)
        s.key(alt("v"), delay=1.0)                   # provider picker
        pick = s.display()
        assert "Save current" in "\n".join(pick), "the provider picker did not open:\n" + "\n".join(pick)
        s.key(ENTER, delay=0.7)                      # the only entry: Save current as...
        type_text(s, "mygroq")
        s.key(ENTER, delay=0.7)                      # confirm the name
        press_ok(s)
    saved = read_xwperc(home)
    assert xwperc_value(saved, "AIProviderName") == "mygroq", \
        "the saved provider was not made active:\n" + saved
    assert read_key_file(home, "mygroq") == secret, \
        "the typed key was not moved to openai-api-key-mygroq: %r (files: %s)" \
        % (read_key_file(home, "mygroq"), sorted(os.listdir(cfg_dir(home))))


# @scenario SC-ai-options-42
def test_advanced_cancel_changes_nothing(tmp_path):
    # Typing into the Advanced sub-dialog and cancelling it, then leaving the
    # main dialog, must not touch xwperc at all.
    home = str(tmp_path / "home")
    write_xwperc(home, ["AIBackend : 4", "AIPolicy : ask"])
    before = read_xwperc_bytes(home)
    with session(tmp_path, home) as s:
        open_dialog(s)
        s.key(alt("a"), delay=0.8)
        adv = s.display()
        assert "Advanced AI settings" in "\n".join(adv), \
            "Alt-A did not open the Advanced sub-dialog:\n" + "\n".join(adv)
        type_text(s, "fallback-x")                   # the Model fallback field has focus
        assert "fallback-x" in row_with(s.display(), "Model fallback"), \
            "the text did not land in the Model fallback field"
        press_cancel(s)                              # Esc the sub-dialog
        leave_dialog_if_open(s)                      # Esc the main one
    after = read_xwperc_bytes(home)
    assert after == before, \
        "cancelling the Advanced dialog rewrote xwperc:\n" + after.decode(errors="replace")


# @scenario SC-ai-options-44
def test_host_command_persists(tmp_path):
    # Advanced > Host command (Alt-D) + Ok writes AIHostCommand to xwperc.
    home = str(tmp_path / "home")
    command = "agent-host --stdio"
    write_xwperc(home, ["AIBackend : 4", "AIPolicy : ask"])
    with session(tmp_path, home) as s:
        open_dialog(s)
        s.key(alt("a"), delay=0.8)
        adv = s.display()
        if "Host command" not in "\n".join(adv):
            press_cancel(s)
            pytest.skip("wpe built without the agent host: no Host command field")
        s.key(alt("d"), delay=0.5)
        type_text(s, command)
        assert command in row_with(s.display(), "Host command"), \
            "the command did not land in the Host command field"
        press_ok(s)                                  # Ok the sub-dialog (saves)
        leave_dialog_if_open(s)
    saved = read_xwperc(home)
    assert xwperc_value(saved, "AIHostCommand") == command, \
        "the host command was not persisted:\n" + saved


# @scenario SC-ai-options-45
def test_agent_engine_radio_persists(tmp_path):
    # AIAgentEngine : claude-code marks the second Agent radio; Ok keeps it.
    home = str(tmp_path / "home")
    write_xwperc(home, ["AIBackend : 4", "AIPolicy : ask", "AIAgentEngine : claude-code"])
    with session(tmp_path, home) as s:
        disp = open_dialog(s)
        if "Agent (" not in "\n".join(disp):
            press_cancel(s)
            pytest.skip("wpe built without the agent host: no Agent radio")
        assert radio_marked(disp, "Claude Code"), \
            "AIAgentEngine : claude-code did not mark the Agent radio:\n" + "\n".join(disp)
        press_ok(s)
    saved = read_xwperc(home)
    assert xwperc_value(saved, "AIAgentEngine") == "claude-code", \
        "Ok did not keep AIAgentEngine : claude-code:\n" + saved


@pytest.mark.skipif(sys.platform != "linux", reason="rewrite counting uses inotify")
# @scenario SC-ai-options-47
def test_ok_rewrites_xwperc_once(tmp_path):
    # One Ok = exactly one rewrite of xwperc.  Counted with inotify
    # (IN_CLOSE_WRITE per fopen/fclose), since mtime alone cannot separate one
    # rewrite from two; the content and mtime checks prove a rewrite happened.
    home = str(tmp_path / "home")
    write_xwperc(home, ["AIBackend : 4", "AIPolicy : ask"])
    before = read_xwperc_bytes(home)
    mtime_before = os.stat(xwperc_path(home)).st_mtime_ns
    with session(tmp_path, home) as s:
        open_dialog(s)
        rewrites = count_close_writes(cfg_dir(home), "xwperc", lambda: press_ok(s))
    after = read_xwperc_bytes(home)
    assert after != before and b"[General]" in after, \
        "Ok did not rewrite xwperc:\n" + after.decode(errors="replace")
    assert os.stat(xwperc_path(home)).st_mtime_ns != mtime_before, \
        "xwperc's mtime did not change although its content did"
    assert rewrites == 1, "Ok rewrote xwperc %d times, expected exactly once" % rewrites


# @scenario SC-ai-options-49
def test_cancel_leaves_xwperc_byte_identical(tmp_path):
    # Edit Endpoint, CA file and key, toggle Enable, then Esc: nothing written,
    # neither to xwperc nor to a key file.
    home = str(tmp_path / "home")
    write_xwperc(home, ["AIBackend : 1", "AIEndpoint : http://file.test:1111/v1",
                        "AIPolicy : ask"])
    before = read_xwperc_bytes(home)
    with session(tmp_path, home) as s:
        open_dialog(s)
        s.key(alt("u"), delay=0.4); type_text(s, "/zzz")
        s.key(alt("c"), delay=0.4); type_text(s, "/tmp/ca.pem")
        s.key(alt("k"), delay=0.4); type_text(s, "sk-cancelled")
        s.key(alt("e"), delay=0.4)
        edited = s.display()
        assert "/zzz" in row_with(edited, "Endpoint") and "/tmp/ca.pem" in row_with(edited, "CA file") \
            and "sk-cancelled" in row_with(edited, "API key") and "[X]" in enable_row(edited), \
            "the edits did not land in the fields:\n" + "\n".join(edited)
        press_cancel(s)
    assert read_xwperc_bytes(home) == before, \
        "Esc rewrote xwperc:\n" + read_xwperc(home)
    assert read_key_file(home) is None, "Esc wrote a key file"


# @scenario SC-ai-options-51
def test_env_endpoint_policy_cafile_override_file(tmp_path):
    # XWPE_AI_ENDPOINT / _POLICY / _CAFILE beat the file for the session: the
    # dialog shows the environment's values.  They are a temporary override, so
    # an Ok without edits leaves the file's own values in xwperc; only a value
    # the user changes in the dialog is written.
    home = str(tmp_path / "home")
    file_values = {"AIEndpoint": "http://file.test:1111/v1",
                   "AICAFile": "/file/ca.pem", "AIPolicy": "ask"}
    write_xwperc(home, ["AIBackend : 1"] + ["%s : %s" % kv for kv in file_values.items()])
    env = {"XWPE_AI_ENDPOINT": "http://env.test:2222/v1",
           "XWPE_AI_POLICY": "auto", "XWPE_AI_CAFILE": "/env/ca.pem"}
    with session(tmp_path, home, **env) as s:
        disp = open_dialog(s)
        assert env["XWPE_AI_ENDPOINT"] in row_with(disp, "Endpoint"), \
            "the dialog does not show the env endpoint:\n" + "\n".join(disp)
        assert env["XWPE_AI_CAFILE"] in row_with(disp, "CA file"), \
            "the dialog does not show the env CA file:\n" + "\n".join(disp)
        assert radio_marked(disp, "Auto (skip asks)"), \
            "the dialog does not mark the env policy:\n" + "\n".join(disp)
        press_ok(s)
    saved = read_xwperc(home)
    for key, value in file_values.items():
        assert xwperc_value(saved, key) == value, \
            "an edit-free Ok replaced the file's %s with the environment override:\n%s" \
            % (key, saved)
    assert "env.test" not in saved and "/env/ca.pem" not in saved, \
        "an environment override was written into xwperc:\n" + saved


def test_env_backend_and_model_not_persisted_by_edit_free_ok(tmp_path):
    # XWPE_AI_BACKEND / _MODEL pick the session's backend and model (the dialog
    # marks Claude CLI and shows the env model) but an Ok without edits keeps
    # the file's AIBackend / AIModel: a one-session override must not replace
    # the configuration the next plain launch starts from.
    home = str(tmp_path / "home")
    write_xwperc(home, ["AIBackend : 1", "AIModel : gpt-x",
                        "AIEndpoint : http://file.test:1111/v1", "AIPolicy : ask"])
    with session(tmp_path, home, XWPE_AI_BACKEND="claudecli", XWPE_AI_MODEL="sonnet") as s:
        disp = open_dialog(s)
        assert radio_marked(disp, "Claude CLI"), \
            "the dialog does not mark the env backend:\n" + "\n".join(disp)
        assert "sonnet" in row_with(disp, "Model (Alt-M)"), \
            "the dialog does not show the env model:\n" + "\n".join(disp)
        press_ok(s)
    saved = read_xwperc(home)
    assert xwperc_value(saved, "AIBackend") == "1", \
        "an edit-free Ok replaced the file's AIBackend with the env backend:\n" + saved
    assert xwperc_value(saved, "AIModel") == "gpt-x", \
        "an edit-free Ok replaced the file's AIModel:\n" + saved


def test_env_backend_keeps_file_model_that_it_cannot_use(tmp_path):
    # The file says Ollama with an Ollama model; XWPE_AI_BACKEND=claudecli for
    # one session drops that model at run time (a Claude backend cannot use it).
    # An edit-free Ok must still keep AIModel in the file: the next plain launch
    # is an Ollama session again and needs its model.
    home = str(tmp_path / "home")
    write_xwperc(home, ["AIBackend : 0", "AIModel : qwen3:8b", "AIPolicy : ask"])
    with session(tmp_path, home, XWPE_AI_BACKEND="claudecli") as s:
        disp = open_dialog(s)
        assert radio_marked(disp, "Claude CLI"), \
            "the dialog does not mark the env backend:\n" + "\n".join(disp)
        press_ok(s)
    saved = read_xwperc(home)
    assert xwperc_value(saved, "AIBackend") == "0", \
        "an edit-free Ok replaced the file's AIBackend with the env backend:\n" + saved
    assert xwperc_value(saved, "AIModel") == "qwen3:8b", \
        "an edit-free Ok under an env backend dropped the file's model:\n" + saved


def test_env_override_edit_persists_only_the_edited_key(tmp_path):
    # With XWPE_AI_ENDPOINT and XWPE_AI_POLICY set, the user retypes only the
    # Endpoint field: Ok writes the typed endpoint (a value the user changed)
    # and keeps the file's AIPolicy (still only overridden by the environment).
    home = str(tmp_path / "home")
    write_xwperc(home, ["AIBackend : 1", "AIEndpoint : http://file.test:1111/v1",
                        "AIPolicy : ask"])
    env = {"XWPE_AI_ENDPOINT": "http://env.test:2222/v1", "XWPE_AI_POLICY": "auto"}
    typed = "http://typed.test:3333/v1"
    with session(tmp_path, home, **env) as s:
        open_dialog(s)
        s.key(alt("u"), delay=0.4)                   # typing replaces the field
        type_text(s, typed)
        assert typed in row_with(s.display(), "Endpoint"), \
            "the endpoint was not typed into the field"
        press_ok(s)
    saved = read_xwperc(home)
    assert xwperc_value(saved, "AIEndpoint") == typed, \
        "Ok did not save the endpoint the user edited:\n" + saved
    assert xwperc_value(saved, "AIPolicy") == "ask", \
        "Ok replaced the file's AIPolicy with the untouched env policy:\n" + saved


def test_advanced_close_repaints_main_dialog(tmp_path):
    # Tick Enable, open Advanced (Alt-A) and leave it with Esc: the main dialog
    # must be painted again where it was, Enable still ticked.  The same after
    # Advanced Ok.  The main Ok then applies the tick.  (Before the fix the
    # main dialog stayed modal but invisible: the editor showed through and the
    # pending tick could only be saved blind.)
    home = str(tmp_path / "home")
    write_xwperc(home, ["AIBackend : 4", "AIPolicy : ask"], general=["Options2 : 152"])
    with session(tmp_path, home) as s:
        open_dialog(s)
        where = title_pos(s, "AI settings")
        s.key(alt("e"), delay=0.4)
        assert "[X]" in enable_row(s.display()), "Alt-E did not tick the Enable box"
        for leave in (press_cancel, press_ok):
            s.key(alt("a"), delay=0.8)
            assert "Advanced AI settings" in "\n".join(s.display()), \
                "Alt-A did not open the Advanced sub-dialog"
            leave(s)
            disp = s.display()
            assert "Advanced AI settings" not in "\n".join(disp), \
                "the Advanced sub-dialog did not close"
            assert dialog_open(disp) and "Backend:" in "\n".join(disp), \
                "after leaving Advanced with %s the main dialog is not painted:\n%s" \
                % (leave.__name__, "\n".join(disp))
            assert title_pos(s, "AI settings") == where, \
                "the main dialog moved after Advanced closed: %r -> %r" \
                % (where, title_pos(s, "AI settings"))
            assert "[X]" in enable_row(disp), \
                "the Enable tick was lost when Advanced closed:\n" + "\n".join(disp)
        press_ok(s)
        assert not dialog_open(s.display()), "the main Ok did not close the dialog"
    saved = xwperc_value(read_xwperc(home), "Options2")
    assert saved is not None and int(saved) & OPTIONS2_AI_ENABLE, \
        "the Enable tick made before Advanced was not applied by Ok: Options2 = %r" % saved


def provider_lines(text):
    return [ln for ln in text.splitlines() if ln.startswith("AIProvider :")]


def save_provider_as(s, name):
    """Alt-V, '[ + Save current as... ]' (the only entry of a fresh profile),
    type the name, Enter."""
    s.key(alt("v"), delay=1.0)
    assert "Save current" in "\n".join(s.display()), "the provider picker did not open"
    s.key(ENTER, delay=0.7)
    assert "Save provider as" in "\n".join(s.display()), "the name prompt did not open"
    type_text(s, name)
    s.key(ENTER, delay=0.8)


@pytest.mark.parametrize("name", [
    "p" * 100,                          # longer than a provider name may be
    "groq|http://evil",                 # '|' separates the AIProvider fields
    "groq/sub",                         # '/' would put the key file in a subdir
], ids=["too-long", "pipe", "slash"])
def test_provider_name_refused_when_unusable(tmp_path, name):
    # A name that cannot be stored (over the limit, or carrying the '|' field
    # separator of the AIProvider line or a '/' that breaks the key file path)
    # is refused with a visible message; nothing is saved, the editor stays up
    # and the dialog is back for another try.
    home = str(tmp_path / "home")
    write_xwperc(home, ["AIBackend : 1", "AIEndpoint : http://127.0.0.1:1/v1",
                        "AIPolicy : ask"])
    with session(tmp_path, home) as s:
        open_dialog(s)
        save_provider_as(s, name)
        assert s.alive(), "the editor died on the provider name"
        shown = "\n".join(s.display())
        assert "Provider name" in shown, "no message refused the name:\n" + shown
        s.key(ENTER, delay=0.6)                       # dismiss the message
        disp = s.display()
        assert dialog_open(disp), "the dialog is not back after the message:\n" + "\n".join(disp)
        assert "(none" in row_with(disp, "Provider (Alt-V)"), \
            "the refused name became the active provider:\n" + "\n".join(disp)
        press_ok(s)
        assert s.alive(), "the editor died on Ok"
    saved = read_xwperc(home)
    assert provider_lines(saved) == [], \
        "an unusable provider name was written to xwperc:\n" + saved
    assert xwperc_value(saved, "AIProviderName") == "", \
        "the refused name was made the active provider:\n" + saved
    assert sorted(os.listdir(cfg_dir(home))) == ["xwperc"], \
        "a stray file appeared next to xwperc: %s" % sorted(os.listdir(cfg_dir(home)))
