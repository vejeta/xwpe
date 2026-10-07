"""Options > AI: the clauses the other suites left unguarded.

test_ai_options.py drives the dialog, test_ai_options_persist.py reads what it
writes and test_ai_options_render.py checks how it is drawn.  Each of those
left a clause of the spec asserted by nobody: the Enable box reading the SAVED
bit (not an environment override), the environment beating the file for the
backend and model, every radio label on screen, the "(backend default)" model
caption, the picker's hint and highlight, the dialog staying put under the
picker, text fields surviving a model pick, the Alt-G menu having no Model row,
the key field pre-filled from the key file, the key file's 0600 mode, a
provider pick moving the Backend radio, and the Advanced dialog's fields.
This file holds exactly those, one small test per clause.

Every test seeds $HOME/.config/xwpe/xwperc (and a key file where needed)
before launch, then reads the rendered screen or the files on disk.  The
backends used are Claude CLI (static model list, no network) and the mock.
"""
import os
import stat
import subprocess

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN, row_with, title_pos, checkbox_mark


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
DOWN = "\033[B"
TITLE = "AI settings"
ENABLE_LABEL = "Enable AI assistant"
MODEL_ROW = "Model (Alt-M)"
NO_MODEL = "(backend default)"
PICKER_HINT = "Up/Dn move  PgUp/PgDn page  Enter ok  Esc cancel"
BACKEND_LABELS = ("Claude CLI", "Ollama", "OpenAI-compatible", "Claude API")
PERMISSION_LABELS = ("Ask each action", "Auto-accept edits", "Auto (skip asks)")
OPTIONS2_AI_ENABLE = 0x0200          # the Enable box's bit in [General] Options2
OPTIONS2_BASE = 152                  # a saved Options2 with the Enable bit clear


def alt(ch):
    """The Alt-<ch> key sequence (ESC-prefixed, as the dialog accelerators are)."""
    return "\033" + ch


# -- config on disk -----------------------------------------------------------

def cfg_dir(home):
    d = os.path.join(home, ".config", "xwpe")
    os.makedirs(d, exist_ok=True)
    return d


def xwperc_path(home):
    return os.path.join(cfg_dir(home), "xwperc")


def write_xwperc(home, programming, general=()):
    """Seed ~/.config/xwpe/xwperc with [General] and [Programming] lines."""
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


def xwperc_value(text, key):
    """The value of the first 'key : value' line, '' if empty, None if absent."""
    for line in text.splitlines():
        if line.startswith(key + " :"):
            return line[len(key) + 2:].strip()
    return None


def key_file_path(home, provider=None):
    name = "openai-api-key" + ("-" + provider if provider else "")
    return os.path.join(cfg_dir(home), name)


def write_key_file(home, secret, provider=None):
    with open(key_file_path(home, provider), "w") as fh:
        fh.write(secret + "\n")


# -- driving the dialog -------------------------------------------------------

def session(tmp_path, home, **env_extra):
    """A wpe session whose config lives under `home`; XDG_CONFIG_HOME blanked
    so the file is ~/.config/xwpe/xwperc, OPENAI_API_KEY blanked so no outside
    key shadows the key files under test.  No XWPE_AI_* unless passed."""
    env = {"HOME": home, "XDG_CONFIG_HOME": "", "OPENAI_API_KEY": ""}
    env.update(env_extra)
    return WpeSession(str(tmp_path), SEED, env_extra=env)


def open_dialog(s):
    """Options > AI; returns the rendered rows."""
    s.key(ALT.OPTIONS, delay=0.5)
    s.key("i", delay=0.6)
    disp = s.display()
    assert any(TITLE in ln for ln in disp), "the AI settings dialog did not open:\n" + "\n".join(disp)
    return disp


def press_ok(s):
    s.key(alt("o"), delay=0.8)
    s._drain(0.4)


def press_cancel(s):
    s.key(ESC, delay=0.6)


def type_text(s, text):
    for ch in text:
        s.key(ch, delay=0.03)


def radio_marked(disp, label):
    """True when the radio whose caption starts with `label` is the marked one."""
    return any(("(*) " + label) in ln for ln in disp)


def marked_count(disp, labels):
    """How many of `labels` are drawn as the marked radio."""
    return sum(1 for lb in labels if radio_marked(disp, lb))


def text_bg(s, needle):
    """(row, background) of the first cell of `needle` on screen; the row index
    is -1 when the text is absent."""
    rows = s.display(drain=0)
    for y, ln in enumerate(rows):
        x = ln.find(needle)
        if x >= 0:
            return y, s.cell_bg(y, x)
    return -1, None


# -- tests --------------------------------------------------------------------

# @scenario SC-ai-options-05
@pytest.mark.parametrize("bit_set", [True, False], ids=["set", "clear"])
def test_enable_box_reflects_saved_bit(tmp_path, bit_set):
    # No XWPE_AI_ENABLE: the box can only be reading bit 0x0200 of the saved
    # Options2.  With the bit set it opens [X]; with it clear, [ ].
    home = str(tmp_path / "home")
    value = OPTIONS2_BASE | OPTIONS2_AI_ENABLE if bit_set else OPTIONS2_BASE
    write_xwperc(home, ["AIBackend : 4", "AIPolicy : ask"],
                 general=["Options2 : %d" % value])
    with session(tmp_path, home) as s:
        open_dialog(s)
        mark = checkbox_mark(s, ENABLE_LABEL)
        press_cancel(s)
    want = "[X]" if bit_set else "[ ]"
    assert mark == want, \
        "Options2 = %d (bit 0x0200 %s) opened the Enable box as %r, expected %r" \
        % (value, "set" if bit_set else "clear", mark, want)


# @scenario SC-ai-options-50
def test_env_backend_and_model_override_file(tmp_path):
    # The file says Ollama + qwen3:8b; XWPE_AI_BACKEND / XWPE_AI_MODEL say Claude
    # CLI + sonnet.  The dialog shows the environment's values, not the file's.
    home = str(tmp_path / "home")
    write_xwperc(home, ["AIBackend : 0", "AIModel : qwen3:8b", "AIPolicy : ask"])
    with session(tmp_path, home, XWPE_AI_ENABLE="1",
                 XWPE_AI_BACKEND="claudecli", XWPE_AI_MODEL="sonnet") as s:
        disp = open_dialog(s)
        model_row = row_with(s, MODEL_ROW)
        press_cancel(s)
    joined = "\n".join(disp)
    assert radio_marked(disp, "Claude CLI"), \
        "XWPE_AI_BACKEND=claudecli did not win over AIBackend : 0:\n" + joined
    assert not radio_marked(disp, "Ollama"), \
        "the file's backend (Ollama) is still the marked radio:\n" + joined
    assert "sonnet" in model_row, \
        "XWPE_AI_MODEL=sonnet is not on the Model button: %r" % model_row
    assert "qwen3:8b" not in joined, \
        "the file's model (qwen3:8b) is still shown:\n" + joined


# @scenario SC-ai-options-04
def test_dialog_lists_every_backend_and_permission_label(tmp_path):
    # All four backend labels and all three permission labels are on screen,
    # Backend and Permission head the same row (side by side), and exactly one
    # radio of each group is marked: the saved one.
    home = str(tmp_path / "home")
    write_xwperc(home, ["AIBackend : 4", "AIPolicy : edits"])
    with session(tmp_path, home) as s:
        disp = open_dialog(s)
        press_cancel(s)
    joined = "\n".join(disp)
    for label in BACKEND_LABELS + PERMISSION_LABELS:
        assert label in joined, "the dialog does not list %r:\n%s" % (label, joined)
    assert any("Backend:" in ln and "Permission:" in ln for ln in disp), \
        "Backend and Permission are not side by side:\n" + joined
    assert radio_marked(disp, "Claude CLI") and marked_count(disp, BACKEND_LABELS) == 1, \
        "AIBackend : 4 should mark Claude CLI and nothing else:\n" + joined
    assert radio_marked(disp, "Auto-accept edits") and marked_count(disp, PERMISSION_LABELS) == 1, \
        "AIPolicy : edits should mark Auto-accept edits and nothing else:\n" + joined


# @scenario SC-ai-options-06
def test_model_caption_is_backend_default_without_a_model(tmp_path):
    # No AIModel in the file and no XWPE_AI_MODEL: the Model button says
    # "(backend default)".  With a model saved it shows that name instead.
    home = str(tmp_path / "none")
    write_xwperc(home, ["AIBackend : 4", "AIPolicy : ask"])
    with session(tmp_path, home) as s:
        open_dialog(s)
        none_row = row_with(s, MODEL_ROW)
        press_cancel(s)
    assert NO_MODEL in none_row, \
        "the Model button does not read %r with no model: %r" % (NO_MODEL, none_row)

    named = str(tmp_path / "named")
    write_xwperc(named, ["AIBackend : 4", "AIModel : sonnet", "AIPolicy : ask"])
    with session(tmp_path, named) as s:
        open_dialog(s)
        named_row = row_with(s, MODEL_ROW)
        press_cancel(s)
    assert "sonnet" in named_row and NO_MODEL not in named_row, \
        "the Model button does not show the saved model alone: %r" % named_row


# @scenario SC-ai-options-19
def test_picker_hint_and_in_use_model_highlighted(tmp_path):
    # Alt-M on Claude CLI with opus in use: the picker's hint row reads exactly
    # the documented keys, and the opus row is drawn with a background that
    # differs from its neighbour's, i.e. it is the highlighted (preselected) one.
    home = str(tmp_path / "home")
    write_xwperc(home, ["AIBackend : 4", "AIModel : opus", "AIPolicy : ask"])
    with session(tmp_path, home) as s:
        open_dialog(s)
        s.key(alt("m"), delay=0.8)
        pick = s.display()
        cur_row, cur_bg = text_bg(s, "opus")
        other_row, other_bg = text_bg(s, "sonnet")
        s.key(ESC, delay=0.4)
        press_cancel(s)
    joined = "\n".join(pick)
    assert "Model (" in joined and "available)" in joined, "the picker did not open:\n" + joined
    assert PICKER_HINT in joined, \
        "the picker's hint does not read %r:\n%s" % (PICKER_HINT, joined)
    assert cur_row >= 0 and other_row >= 0 and abs(cur_row - other_row) == 1, \
        "opus and sonnet are not adjacent picker rows (%d, %d):\n%s" % (cur_row, other_row, joined)
    assert cur_bg != other_bg, \
        "the in-use model opus is not highlighted (bg %r, same as sonnet):\n%s" % (cur_bg, joined)


# @scenario SC-ai-options-20
def test_picker_leaves_dialog_where_it_was(tmp_path):
    # The title's (row, col) is where the dialog IS.  It must be identical after
    # the picker is closed with Esc and again after a pick with Enter, and the
    # dialog's own content stays visible under the open picker.
    home = str(tmp_path / "home")
    write_xwperc(home, ["AIBackend : 4", "AIModel : sonnet", "AIPolicy : ask"])
    with session(tmp_path, home) as s:
        open_dialog(s)
        before = title_pos(s, TITLE)
        s.key(alt("m"), delay=0.8)
        under = "\n".join(s.display())
        s.key(ESC, delay=0.5)
        after_esc = title_pos(s, TITLE)
        s.key(alt("m"), delay=0.8)
        s.key(DOWN, delay=0.3)
        s.key(ENTER, delay=0.7)
        after_pick = title_pos(s, TITLE)
        picked = row_with(s, MODEL_ROW)
        press_cancel(s)
    assert before is not None, "no dialog title before the picker"
    assert "Connection" in under and "Cancel" in under, \
        "the dialog was erased under the picker:\n" + under
    assert after_esc == before, \
        "the dialog moved after Esc closed the picker: %r -> %r" % (before, after_esc)
    assert after_pick == before, \
        "the dialog moved after a pick: %r -> %r" % (before, after_pick)
    assert "sonnet" not in picked, "Down + Enter did not pick a different model: %r" % picked


# @scenario SC-ai-options-23
def test_model_pick_keeps_text_fields(tmp_path):
    # Enable ticked, Endpoint / CA file / key filled from disk: after Alt-M, Down,
    # Enter every one of them still shows its value and Enable is still [X].
    home = str(tmp_path / "home")
    endpoint = "http://keep.test:1234/v1"
    cafile = "/keep/ca.pem"
    secret = "sk-keep-me-1"
    write_xwperc(home, ["AIBackend : 4", "AIModel : sonnet", "AIEndpoint : " + endpoint,
                        "AICAFile : " + cafile, "AIPolicy : ask"])
    write_key_file(home, secret)
    with session(tmp_path, home) as s:
        open_dialog(s)
        s.key(alt("e"), delay=0.4)
        assert checkbox_mark(s, ENABLE_LABEL) == "[X]", "Alt-E did not tick Enable"
        s.key(alt("m"), delay=0.8)
        s.key(DOWN, delay=0.3)
        s.key(ENTER, delay=0.7)
        after = s.display()
        fields = {"Endpoint": row_with(s, "Endpoint (Alt-U)"),
                  "CA file": row_with(s, "CA file (Alt-C)"),
                  "API key": row_with(s, "API key (Alt-K)"),
                  "Model": row_with(s, MODEL_ROW)}
        mark = checkbox_mark(s, ENABLE_LABEL)
        press_cancel(s)
    joined = "\n".join(after)
    assert "sonnet" not in fields["Model"], \
        "the pick did not change the model: %r" % fields["Model"]
    assert mark == "[X]", "the pick cleared the Enable box:\n" + joined
    assert endpoint in fields["Endpoint"], \
        "the pick lost the Endpoint field: %r" % fields["Endpoint"]
    assert cafile in fields["CA file"], \
        "the pick lost the CA file field: %r" % fields["CA file"]
    assert secret in fields["API key"], \
        "the pick lost the API key field: %r" % fields["API key"]


# @scenario SC-ai-options-28
def test_ai_menu_has_no_model_row(tmp_path):
    # The Alt-G menu offers "AI settings..." and no row of its own for the
    # model: the picker is reached only through the dialog's Alt-M.
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock"}
    with WpeSession(str(tmp_path), "hello\n", filename="notes.txt", env_extra=env) as s:
        s.key(ALT.AI, delay=0.8)
        disp = s.display()
        s.key(ESC, delay=0.3)
    first = next((i for i, ln in enumerate(disp) if "Ask (chat)" in ln), -1)
    last = next((i for i, ln in enumerate(disp) if "AI settings..." in ln), -1)
    assert 0 <= first < last, "the Alt-G menu did not open:\n" + "\n".join(disp)
    menu = disp[first:last + 1]
    assert any("Permissions:" in ln for ln in menu), "no Permissions row:\n" + "\n".join(menu)
    assert not any("Model" in ln or "model" in ln for ln in menu), \
        "the Alt-G menu still has a Model row:\n" + "\n".join(menu)


# @scenario SC-ai-options-32
def test_key_field_prefilled_with_stored_key(tmp_path):
    # A key already in openai-api-key is shown in the API key field on open, so
    # the user can see that a key is set.
    home = str(tmp_path / "home")
    secret = "sk-stored-prefill-1"
    write_xwperc(home, ["AIBackend : 1", "AIEndpoint : http://127.0.0.1:1/v1", "AIPolicy : ask"])
    write_key_file(home, secret)
    with session(tmp_path, home) as s:
        open_dialog(s)
        row = row_with(s, "API key (Alt-K)")
        press_cancel(s)
    assert secret in row, "the API key field is not pre-filled with the stored key: %r" % row


# @scenario SC-ai-options-33
def test_key_file_written_with_mode_0600(tmp_path):
    # The key file a typed key lands in is private to the user: mode 0600.
    home = str(tmp_path / "home")
    secret = "sk-mode-check-600"
    write_xwperc(home, ["AIBackend : 1", "AIEndpoint : http://127.0.0.1:1/v1", "AIPolicy : ask"])
    with session(tmp_path, home) as s:
        open_dialog(s)
        s.key(alt("k"), delay=0.5)
        type_text(s, secret)
        assert secret in row_with(s, "API key"), "the key was not typed into the field"
        press_ok(s)
    path = key_file_path(home)
    assert os.path.exists(path), "Ok did not write the key file"
    mode = stat.S_IMODE(os.stat(path).st_mode)
    assert mode == 0o600, "the key file has mode %o, expected 0600" % mode


# @scenario SC-ai-options-36
def test_provider_pick_switches_backend_radio_and_fields(tmp_path):
    # Seeded on Ollama, picking a profile moves the Backend radio to
    # OpenAI-compatible and fills Endpoint, Model and CA file from the profile;
    # Ok then saves AIBackend : 1.
    home = str(tmp_path / "home")
    endpoint = "https://api.groq.com/openai/v1"
    model = "openai/gpt-oss-120b"
    cafile = "/ca/groq.pem"
    write_xwperc(home, ["AIBackend : 0", "AIEndpoint : http://localhost:11434",
                        "AIProvider : groq|%s|%s|%s" % (endpoint, model, cafile),
                        "AIPolicy : ask"])
    with session(tmp_path, home) as s:
        disp = open_dialog(s)
        assert radio_marked(disp, "Ollama"), "the seeded backend is not Ollama:\n" + "\n".join(disp)
        s.key(alt("v"), delay=1.0)
        s.key(ENTER, delay=0.8)                      # the first entry: groq
        picked = s.display()
        fields = {"Endpoint": row_with(s, "Endpoint (Alt-U)"),
                  "CA file": row_with(s, "CA file (Alt-C)"),
                  "Model": row_with(s, MODEL_ROW),
                  "Provider": row_with(s, "Provider (Alt-V)")}
        press_ok(s)
    joined = "\n".join(picked)
    assert radio_marked(picked, "OpenAI-compatible") and not radio_marked(picked, "Ollama"), \
        "picking a profile did not move the Backend radio to OpenAI-compatible:\n" + joined
    assert "groq" in fields["Provider"], "the Provider button does not name groq: %r" % fields["Provider"]
    assert endpoint in fields["Endpoint"], "the profile's endpoint is not shown: %r" % fields["Endpoint"]
    assert model in fields["Model"], "the profile's model is not shown: %r" % fields["Model"]
    assert cafile in fields["CA file"], "the profile's CA file is not shown: %r" % fields["CA file"]
    saved = read_xwperc(home)
    assert xwperc_value(saved, "AIBackend") == "1", \
        "Ok did not save the switched-to backend:\n" + saved
    assert xwperc_value(saved, "AIProviderName") == "groq", \
        "Ok did not save the active provider:\n" + saved


def open_advanced(s):
    """Alt-A from the open AI dialog; returns the Advanced dialog's rows."""
    s.key(alt("a"), delay=0.8)
    adv = s.display()
    assert "Advanced AI settings" in "\n".join(adv), \
        "Alt-A did not open the Advanced dialog:\n" + "\n".join(adv)
    return adv


# @scenario SC-ai-options-40
def test_advanced_dialog_shows_fields_and_buttons(tmp_path):
    # Alt-A opens "Advanced AI settings" with its labelled fields (Host command
    # only in a host build), the Fallback explanation, Ok and Cancel.  Each
    # label is asserted in full: the fields start after the longest label, so
    # no label is overwritten by its own field.
    home = str(tmp_path / "home")
    write_xwperc(home, ["AIBackend : 4", "AIPolicy : ask"])
    with session(tmp_path, home) as s:
        open_dialog(s)
        adv = open_advanced(s)
        press_cancel(s)                              # Esc the sub-dialog
        press_cancel(s)                              # Esc the main one
    joined = "\n".join(adv)
    for want in ("Model fallback (Alt-M):", "Edit hook (Alt-H):", "Fallback: a model retried once"):
        assert want in joined, "the Advanced dialog lacks %r:\n%s" % (want, joined)
    host_build = any("Host command" in ln for ln in adv)
    if host_build:
        assert "Host command (Alt-D):" in joined, "the Host command label is incomplete:\n" + joined
    ok_row = next((ln for ln in adv if "Ok" in ln and "Cancel" in ln), "")
    assert ok_row, "Ok and Cancel are not both on one row of the Advanced dialog:\n" + joined


# @scenario SC-ai-options-40
def test_advanced_dialog_shows_both_explanatory_lines(tmp_path):
    # The spec promises two explanatory lines under the fields: one for the
    # fallback model and one for the edit hook.
    home = str(tmp_path / "home")
    write_xwperc(home, ["AIBackend : 4", "AIPolicy : ask"])
    with session(tmp_path, home) as s:
        open_dialog(s)
        adv = open_advanced(s)
        press_cancel(s)
        press_cancel(s)
    joined = "\n".join(adv)
    assert "Fallback: a model retried once" in joined, \
        "the Fallback explanation is missing:\n" + joined
    assert "Edit hook: a command run" in joined, \
        "the Edit hook explanation is missing:\n" + joined
