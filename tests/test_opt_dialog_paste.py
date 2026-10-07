"""A terminal paste never drives a modal dialog.

With bracketed paste (xterm mode 2004) the terminal brackets pasted text in
ESC [ 200 ~ ... ESC [ 201 ~.  The option-dialog engine reads keys through one
reader (e_opt_getch) that swallows the pasted bytes while the focus is on a
checkbox, a radio or a button, so a pasted letter cannot press an accelerator
("o" pressing Ok) and a pasted newline cannot submit through the default
button.  A focused single-line field keeps the printable characters and drops
the control bytes (e_schreib_leiste, e_schreib_zif).

Whether Ok was applied is read from the AI trace: Ok writes an "options
backend=..." line, Cancel writes nothing.

Driven on the AI settings dialog (Options > AI), 64x21 cells, and on the
Editor options dialog for the numeric field.
"""
import os
import subprocess

import pytest

from wpe_driver import WpeSession, ALT, WPE_BIN, checkbox_mark, row_with


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"AI settings" in out
    except Exception:
        return False


ENV = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock"}
SEED = "int main(void){return 0;}\n"
TITLE = "AI settings"
EDITOR_TITLE = "Editor-Options"
ESC = "\033"
TAB = "\t"
ALT_T = "\033t"              # Tabstops, the numeric field of Editor options
PASTE_BEGIN, PASTE_END = "\033[200~", "\033[201~"
needs_ai = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _paste(text):
    return PASTE_BEGIN + text + PASTE_END


def _open_ai_settings(tmp_path, trace):
    env = dict(ENV, XWPE_AI_TRACE=str(trace))
    s = WpeSession(str(tmp_path), SEED, env_extra=env)
    s.key(ALT.OPTIONS, delay=0.5)
    s.key("i", delay=0.7)
    return s


def _is_open(s, title=TITLE):
    return any(title in row for row in s.display())


def _options_applied(trace):
    text = trace.read_text() if trace.exists() else ""
    return any(line.startswith("options ") for line in text.splitlines())


# @scenario SC-dialog-engine-39
@needs_ai
def test_pasted_accelerator_letter_does_not_press_widget(tmp_path):
    # Focus on the Enable checkbox; typed, "e" is its accelerator and "a" opens
    # the Advanced sub-dialog.  Pasted, neither may act.
    trace = tmp_path / "ai.trace"
    with _open_ai_settings(tmp_path, trace) as s:
        before = checkbox_mark(s, "Enable AI")
        s.key(_paste("ea"), delay=0.7)
        after = checkbox_mark(s, "Enable AI")
        still_open = _is_open(s)
        advanced = any("Advanced" in row and TITLE not in row and "(Alt-A)" not in row
                       for row in s.display())
        rows = s.display()
        s.key(ESC, delay=0.6)
    assert still_open, \
        "a pasted letter closed the dialog:\n" + "\n".join(rows)
    assert before == after, \
        "a pasted 'e' toggled the Enable checkbox: %r -> %r" % (before, after)
    assert not advanced, \
        "a pasted 'a' opened the Advanced sub-dialog:\n" + "\n".join(rows)
    assert not _options_applied(trace), "a pasted letter applied the options like Ok"


# @scenario SC-dialog-engine-40
@needs_ai
def test_pasted_newline_outside_a_field_does_not_submit(tmp_path):
    # Tab moves the focus to the Backend radio group; a pasted line there
    # must neither select a radio nor submit through the default button.
    trace = tmp_path / "ai.trace"
    with _open_ai_settings(tmp_path, trace) as s:
        s.key(TAB, delay=0.3)
        before = row_with(s, "Ollama")
        s.key(_paste("some text\n"), delay=0.7)
        still_open = _is_open(s)
        after = row_with(s, "Ollama")
        rows = s.display()
        s.key(ESC, delay=0.6)
    assert still_open, \
        "a pasted newline submitted the dialog:\n" + "\n".join(rows)
    assert not _options_applied(trace), "a pasted newline applied the options like Ok"
    assert before == after, \
        "the paste changed the radio row:\nBEFORE: %r\nAFTER:  %r" % (before, after)


# @scenario SC-dialog-engine-41
def test_paste_into_numeric_field_keeps_digits_drops_newline(tmp_path):
    with WpeSession(str(tmp_path), SEED) as s:
        s.key(ALT.OPTIONS, delay=0.5)
        s.key("e", delay=0.7)
        assert _is_open(s, EDITOR_TITLE), \
            "Editor options did not open:\n" + "\n".join(s.display())
        s.key(ALT_T, delay=0.4)                  # the Tabstops field
        s.key(_paste("7\n"), delay=0.7)
        still_open = _is_open(s, EDITOR_TITLE)
        field = row_with(s, "Tabstops")
        rows = s.display()
        s.key(ESC, delay=0.6)
    assert still_open, \
        "the pasted newline submitted the dialog:\n" + "\n".join(rows)
    assert "7" in field.split("Tabstops")[1], \
        "the pasted digit did not reach the Tabstops field: %r" % field
