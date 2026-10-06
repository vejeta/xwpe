"""Options -> AI: how the settings dialog is DRAWN, as the user sees it.

The sibling test_ai_options.py covers what the dialog saves and applies.  This
file only reads the screen: the dialog opens even while the assistant is off,
the Provider button says "(none - pick/save)" until a profile is active and then
shows its name, the hint row names the keys, the whole dialog (title, Ok,
Cancel, bottom border) fits a 24-line terminal, and the model picker floated by
Alt-M stays on screen in a short terminal.

The model list for the picker comes from a throwaway local OpenAI-compatible
server (GET /v1/models), the same device test_ai_options.py uses: the mock
backend advertises a single model, and the picker test needs a long list.
"""
import os
import json
import threading
import subprocess
from http.server import BaseHTTPRequestHandler, HTTPServer

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"AI settings" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")

SEED = "int main(void){return 0;}\n"
TITLE = "AI settings"
HINT = "Tab/arrows move  Space selects  Alt-U/C/K/M/V edit"
NO_PROVIDER = "(none - pick/save)"
PICKER_HINT = "Up/Dn move  PgUp/PgDn page  Enter ok  Esc cancel"
ALT_M = "\033m"
ESC = "\033"

# The assistant is on, with no key in the environment so the Connection block
# starts empty and nothing reaches a real endpoint.
ENABLED = {"XWPE_AI_ENABLE": "1", "OPENAI_API_KEY": ""}


# -- helpers -----------------------------------------------------------------

def seed_config(home, body):
    """Write $HOME/.config/xwpe/xwperc with a [Programming] section holding
    `body` (one "Key : value" per line).  Returns the file's path."""
    cfg = os.path.join(home, ".config", "xwpe")
    os.makedirs(cfg, exist_ok=True)
    path = os.path.join(cfg, "xwperc")
    with open(path, "w") as fh:
        fh.write("[Programming]\n" + body)
    return path


def session_dir(tmp_path, name):
    """A fresh workdir (= HOME for that session) under tmp_path."""
    d = tmp_path / name
    d.mkdir()
    return str(d)


def open_dialog(s):
    """Options menu, then its AI entry: the AI settings dialog."""
    s.key(ALT.OPTIONS, delay=0.5)
    s.key("i", delay=0.6)
    return s


def find_row(rows, text):
    """Index of the first screen row containing `text`, or -1."""
    for i, ln in enumerate(rows):
        if text in ln:
            return i
    return -1


def row_with(rows, text):
    """The first screen row containing `text`, or ""."""
    i = find_row(rows, text)
    return rows[i] if i >= 0 else ""


def dialog_frame(rows):
    """(title_row, left_col, right_col) of the AI settings dialog's frame.

    pyte shows DEC line drawing as its ASCII slots: the frame's corners are
    'l' (top-left) and 'k' (top-right).  The dialog is nested in the editor
    window's own frame, so the dialog's top-left corner is the LAST 'l' before
    the title text and its top-right the first 'k' after it."""
    t = find_row(rows, TITLE)
    assert t >= 0, "no %r title on screen:\n%s" % (TITLE, "\n".join(rows))
    title_at = rows[t].index(TITLE)
    left = rows[t].rindex("l", 0, title_at)
    right = rows[t].index("k", title_at)
    return t, left, right


def inner_text(row, left, right):
    """The text drawn between the dialog's side borders, ends trimmed."""
    return row[left + 1:right].strip()


def bottom_border_row(rows, left, right, below):
    """Index of the dialog's bottom border row (the 'm ... j' corners on the
    dialog's columns) strictly after row `below`, or -1 if it is cut off."""
    for i in range(below + 1, len(rows)):
        ln = rows[i]
        if len(ln) > right and ln[left] == "m" and ln[right] == "j":
            return i
    return -1


def menu_lists_ai(rows):
    """True if the open Options menu has an "AI" item (a cell of its box that
    reads exactly AI, so the bottom bar's "Alt-G AI" cannot stand in)."""
    return any(p.strip() == "AI" for ln in rows for p in ln.split("x"))


def fake_models_server(models):
    """A local OpenAI-compatible server answering GET .../models with `models`.
    Returns (server, port); call server.shutdown() when done."""
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            if self.path.endswith("/models"):
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


# -- tests -------------------------------------------------------------------

# @scenario SC-ai-options-02
def test_dialog_opens_with_assistant_disabled(tmp_path):
    # No XWPE_AI_ENABLE, no config: the assistant is off.  The Options menu
    # still lists AI and the entry still opens the settings dialog -- that is
    # where the user turns the assistant ON in the first place.
    with WpeSession(session_dir(tmp_path, "off"), SEED) as s:
        s.key(ALT.OPTIONS, delay=0.5)
        menu = s.display()
        s.key("i", delay=0.6)
        dialog = s.display()
        s.key(ESC, delay=0.3)
    assert menu_lists_ai(menu), \
        "the Options menu does not list AI while the assistant is off:\n" + "\n".join(menu)
    assert TITLE in "\n".join(dialog), \
        "Options > AI did not open the settings dialog while the assistant is off:\n" \
        + "\n".join(dialog)


# @scenario SC-ai-options-07
def test_provider_caption_none_then_name(tmp_path):
    # Without an active provider the Provider (Alt-V) button invites the user
    # to pick or save one; with AIProviderName set in xwperc it shows the name.
    with WpeSession(session_dir(tmp_path, "none"), SEED, env_extra=dict(ENABLED)) as s:
        open_dialog(s)
        none_row = row_with(s.display(), "Provider (Alt-V)")
        s.key(ESC, delay=0.3)
    assert NO_PROVIDER in none_row, \
        "the Provider button does not read %r with no provider:\n%r" % (NO_PROVIDER, none_row)

    named = session_dir(tmp_path, "named")
    seed_config(named, "AIBackend : 1\n"
                       "AIProvider : myprov|http://127.0.0.1:1/v1|m|\n"
                       "AIProviderName : myprov\n"
                       "AIPolicy : ask\n")
    with WpeSession(named, SEED, env_extra=dict(ENABLED)) as s:
        open_dialog(s)
        named_row = row_with(s.display(), "Provider (Alt-V)")
        s.key(ESC, delay=0.3)
    assert "myprov" in named_row, \
        "the Provider button does not show the active provider's name:\n%r" % named_row
    assert NO_PROVIDER not in named_row, \
        "the Provider button still shows the no-provider hint next to the name:\n%r" % named_row


# @scenario SC-ai-options-08
def test_hint_line_names_keys(tmp_path):
    # The hint row tells the user how to move, select and which Alt keys edit
    # the fields -- exactly, so a reworded hint (or a dropped key) is caught.
    with WpeSession(session_dir(tmp_path, "hint"), SEED, env_extra=dict(ENABLED)) as s:
        open_dialog(s)
        rows = s.display()
        s.key(ESC, delay=0.3)
    _, left, right = dialog_frame(rows)
    hint_row = row_with(rows, "Tab/arrows")
    assert hint_row, "no hint row in the dialog:\n" + "\n".join(rows)
    assert inner_text(hint_row, left, right) == HINT, \
        "the hint row reads %r, expected %r" % (inner_text(hint_row, left, right), HINT)


# @scenario SC-ai-options-09
@pytest.mark.parametrize("rows", [24, 30])
def test_dialog_fits_24_lines(tmp_path, rows):
    # The dialog is sized to fit a classic 24-line terminal (30 is the control):
    # title, Ok and Cancel are on screen and the bottom border is drawn, so no
    # row is cut off.  A dialog taller than the screen is also NOT this dialog:
    # the dialog engine shows a clipped dialog as a view-only shell that answers
    # Esc alone, so Space toggling the Enable box proves it fits AND is live.
    with WpeSession(session_dir(tmp_path, "r%d" % rows), SEED, rows=rows,
                    env_extra=dict(ENABLED)) as s:
        open_dialog(s)
        before = s.display()
        s.key(" ", delay=0.4)            # Enable is focused first: Space toggles it
        after = s.display()
        s.key(ESC, delay=0.3)
    joined = "\n".join(before)
    t, left, right = dialog_frame(before)
    ok = find_row(before, "Ok")
    assert 0 <= t < rows and 0 <= ok < rows, \
        "title or Ok is off a %d-line screen:\n%s" % (rows, joined)
    assert "Cancel" in before[ok], \
        "Ok and Cancel are not on the same visible row:\n" + joined
    bottom = bottom_border_row(before, left, right, ok)
    assert 0 <= bottom < rows, \
        "the dialog's bottom border is cut off on a %d-line screen:\n%s" % (rows, joined)
    assert "[ ]" in row_with(before, "Enable AI") and "[X]" in row_with(after, "Enable AI"), \
        "Space did not toggle Enable: the dialog is a clipped, view-only shell:\n" \
        + "\n".join(after)


def row_bg(s, y, text):
    """Background colour of the first cell of `text` on screen row y."""
    return s.cell_bg(y, s.display(drain=0)[y].index(text))


# @scenario SC-ai-options-25
def test_picker_fits_short_terminal(tmp_path):
    # More models than the screen has rows, on the smallest terminal the dialog
    # fits (24 lines): Alt-M's picker caps its height so both borders and the
    # hint row stay on screen, and it scrolls the current model (the LAST one,
    # so a non-scrolling list could not show it) into view, highlighted.  Esc
    # then returns to the intact dialog.
    models = ["model-%02d" % i for i in range(30)]
    current = models[-1]
    rows = 24
    srv, port = fake_models_server(models)
    try:
        home = session_dir(tmp_path, "short")
        seed_config(home, "AIBackend : 1\n"
                          "AIEndpoint : http://127.0.0.1:%d/v1\n"
                          "AIModel : %s\n"
                          "AIPolicy : ask\n" % (port, current))
        with WpeSession(home, SEED, rows=rows, env_extra=dict(ENABLED)) as s:
            open_dialog(s)
            s.key(ALT_M, delay=1.5)      # Alt-M -> picker from the local server
            pick = s.display()
            cur_row = find_row(pick, current)
            cur_bg = row_bg(s, cur_row, current) if cur_row >= 0 else None
            # the row above holds the neighbouring (unselected) model
            other_bg = row_bg(s, cur_row - 1, "model-") if cur_row > 0 else None
            s.key(ESC, delay=0.6)        # close the picker only
            back = s.display()
    finally:
        srv.shutdown()
    joined = "\n".join(pick)
    top = find_row(pick, "Model (%d) at" % len(models))
    assert 0 <= top < rows, "the picker's top border/title is not on screen:\n" + joined
    hint = find_row(pick, PICKER_HINT)
    assert top < hint < rows, "the picker's hint row is not on screen:\n" + joined
    assert "j" in pick[hint][pick[hint].index(PICKER_HINT):], \
        "the picker's bottom border is not on the hint row:\n" + joined
    assert top < cur_row < hint, \
        "the current model %r was not scrolled into view:\n%s" % (current, joined)
    assert cur_bg != other_bg, \
        "the current model %r is not highlighted (bg %r, same as its neighbour):\n%s" \
        % (current, cur_bg, joined)
    assert TITLE in "\n".join(back) and "Cancel" in "\n".join(back), \
        "Esc did not return to the intact dialog:\n" + "\n".join(back)
