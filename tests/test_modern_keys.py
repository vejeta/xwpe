"""Every key of the Modern table drives its action, and the keys that matter
do what they say.

The first group is generated from the table: tools/keymap-texi --dump modern
lists every Modern row, and one row of this file per key sends it as a
terminal would and reads, from the editor's own trace (XWPE_UI_TRACE), which
action the key table gave the dispatcher.  A binding added to the table is
tested the day it is added; a key the table binds and the editor never reads
shows here.

The second group runs the keys on text: Ctrl-S saves, Ctrl-Z and Ctrl-Y undo
and redo, Ctrl-A, Ctrl-L, Ctrl-D and Esc select and drop a selection, and
the keys the Modern style gives up (Ctrl-P, Ctrl-K Ctrl-X) do what the
Borland style's did only in the Borland style.
"""
import os
import re
import subprocess
import time

import pytest

from wpe_driver import WPE_BIN, WpeSession

TOOL = os.path.join(os.path.dirname(os.path.abspath(WPE_BIN)), "tools", "keymap-texi")
DIRECT_CONTEXTS = ("Windows and menus", "Programming", "Debugger", "Editing")
MODERN = {"XWPE_KEYS": "modern"}
BORLAND = {"XWPE_KEYS": "borland"}
SEED_C = "int main(void) { return 0; }\n"
QUIT_WAIT_S = 6.0

# What a terminal sends: the xterm forms, and the modifyOtherKeys form for a
# key that has no legacy byte of its own.
FIRST_CODE_CTRL_SHIFT = 1100
CTRL_SHIFT_LAST = 1125
SPECIAL = {
    "Ctrl-H": "\x1b[27;5;104~",       # the key of its own, not Backspace
    "Ctrl-Tab": "\x1b[27;5;9~",
    "Ctrl-Space": "\x1b[27;5;32~",
    "Ctrl-`": "\x1b[27;5;96~",
    "Ctrl-Del": "\x1b[3;5~",
    "Shift-Del": "\x1b[3;2~",
    "Ctrl-Ins": "\x1b[2;5~",
    "Shift-Ins": "\x1b[2;2~",
    "Alt-Backspace": "\x1b\x7f",
    "Alt-Space": "\x1b ",
    "Alt-0": "\x1b0",
}
FUNCTION = re.compile(r"^(?:(Shift|Ctrl|Alt)-)?F(\d+)$")
CTRL_LETTER = re.compile(r"^Ctrl-([A-Z])$")
ALT_LETTER = re.compile(r"^Alt-([A-Z])$")
MODIFIER = {None: 0, "Shift": 2, "Alt": 3, "Ctrl": 5}
TILDE_CODE = {5: 15, 6: 17, 7: 18, 8: 19, 9: 20, 10: 21, 11: 23, 12: 24}


def key_bytes(code, name):
    """The bytes a terminal sends for a table row, or None for a row no
    terminal sends (a click, a Sun key, a range)."""
    if name in SPECIAL:
        return SPECIAL[name]
    m = FUNCTION.match(name)
    if m:
        mod, n = MODIFIER[m.group(1)], int(m.group(2))
        if n <= 4:
            letter = "PQRS"[n - 1]
            return "\x1b[1;%d%s" % (mod, letter) if mod else "\x1bO" + letter
        return ("\x1b[%d;%d~" % (TILDE_CODE[n], mod)) if mod else "\x1b[%d~" % TILDE_CODE[n]
    if name.startswith("Ctrl-Shift-") and FIRST_CODE_CTRL_SHIFT <= code <= CTRL_SHIFT_LAST:
        return "\x1b[27;6;%d~" % ord(name[-1])
    m = CTRL_LETTER.match(name)
    if m and code < 32:
        return chr(code)
    m = ALT_LETTER.match(name)
    if m:
        return "\x1b" + m.group(1).lower()
    return None


def modern_rows():
    """(context, code, key name, key bytes, the actions the table gives that
    code in that context) for every Modern row a terminal can send."""
    if not os.path.exists(TOOL):
        return []
    out = subprocess.run([TOOL, "--dump", "modern"], capture_output=True, text=True,
                         check=True).stdout
    by_key = {}
    for line in out.splitlines():
        ctx, code, name, action = [f.strip() for f in line.split(" | ")]
        if ctx not in DIRECT_CONTEXTS:
            continue
        by_key.setdefault((int(code), name), []).append((ctx, action))
    rows = []
    for (code, name), where in sorted(by_key.items()):
        data = key_bytes(code, name)
        if data is None:
            continue
        rows.append((code, name, data, sorted({a for _c, a in where})))
    return rows


ROWS = modern_rows()


def _trace_actions(path):
    if not os.path.exists(path):
        return set()
    found = set()
    with open(path) as fh:
        for line in fh:
            m = re.match(r"binding context=.* code=(\d+) action=(.*)$", line.strip())
            if m:
                found.add((int(m.group(1)), m.group(2)))
    return found


# @scenario SC-modern-keymap-05
@pytest.mark.parametrize("code, name, data, actions", ROWS,
                         ids=["%s" % r[1] for r in ROWS])
def test_the_key_reaches_its_action(tmp_path, code, name, data, actions):
    trace = os.path.join(str(tmp_path), "trace.txt")
    extra = dict(MODERN, XWPE_UI_TRACE=trace)
    with WpeSession(str(tmp_path), SEED_C, filename="t.c", env_extra=extra, wait=1.0) as w:
        w.key(data, delay=0.6)
    got = _trace_actions(trace)
    names = {a for c, a in got if c == code}
    assert names & set(actions), \
        "%s (%r) should reach one of %s, the table gave %s" % (name, data, actions, sorted(got))


def test_the_table_has_rows_to_test():
    assert len(ROWS) > 40, "the generated rows are missing: is tools/keymap-texi built?"


def _edit(tmp_path, seed, keys, extra=MODERN, save=True, filename="t.txt"):
    with WpeSession(str(tmp_path), seed, filename=filename, env_extra=extra) as w:
        w.key(*keys, delay=0.4)
        if save:
            w.save()
            return w.text()
        return w


CTRL_A, CTRL_D, CTRL_L, CTRL_Q, CTRL_S, CTRL_X, CTRL_Y, CTRL_Z = (
    chr(1), chr(4), chr(12), chr(17), chr(19), chr(24), chr(25), chr(26))
CTRL_K, CTRL_P = chr(11), chr(16)
DOWN = "\x1b[B"


# @scenario SC-modern-keymap-06
def test_ctrl_s_saves(tmp_path):
    with WpeSession(str(tmp_path), "abc\n", filename="t.txt", env_extra=MODERN) as w:
        w.key("X", CTRL_S, delay=0.5)
        assert w.text() == "Xabc\n"


# @scenario SC-modern-keymap-06
def test_ctrl_z_undoes_and_ctrl_y_redoes(tmp_path):
    assert _edit(tmp_path, "abc\n", ["X", CTRL_Z]) == "abc\n"
    other = tmp_path / "redo"
    other.mkdir()
    assert _edit(other, "abc\n", ["X", CTRL_Z, CTRL_Y]) == "Xabc\n"


# @scenario SC-modern-keymap-07
def test_ctrl_a_selects_everything_and_typing_replaces_it(tmp_path):
    # the file had a final line break, and Modern keeps it
    assert _edit(tmp_path, "one\ntwo\n", [CTRL_A, "Z"]) == "Z\n"


# @scenario SC-modern-keymap-07
def test_ctrl_l_selects_the_line_and_delete_takes_it(tmp_path):
    assert _edit(tmp_path, "one\ntwo\n", [CTRL_L, "\x1b[3~"]) == "two\n"


# @scenario SC-modern-keymap-07
def test_ctrl_d_selects_the_word_and_typing_replaces_it(tmp_path):
    assert _edit(tmp_path, "hello world\n", [CTRL_D, "X"]) == "X world\n"


# @scenario SC-modern-keymap-07
def test_esc_drops_the_selection_and_is_used_up_by_it(tmp_path):
    assert _edit(tmp_path, "one\ntwo\n", [CTRL_A, "\x1b", "\x1b[H", "Z"], extra=MODERN) \
        == "Zone\ntwo\n"


# @scenario SC-modern-keymap-08
def test_ctrl_k_then_a_control_letter_is_a_chord_in_modern_and_a_letter_in_borland(tmp_path):
    """Borland reads Ctrl-K Ctrl-X as Ctrl-K X (mark all) and Ctrl-X cuts it;
    Modern keeps the chord apart from the letter, so nothing is marked."""
    assert _edit(tmp_path, "abc\n", [CTRL_K, CTRL_X, CTRL_X], extra=BORLAND) == "\n"
    other = tmp_path / "modern"
    other.mkdir()
    assert _edit(other, "abc\n", [CTRL_K, CTRL_X, CTRL_X], extra=MODERN) == "abc\n"


# @scenario SC-modern-keymap-09
def test_ctrl_p_moves_up_in_borland_and_is_free_in_modern(tmp_path):
    assert _edit(tmp_path, "a\nb\n", [DOWN, CTRL_P, "X"], extra=BORLAND) == "Xa\nb\n"
    other = tmp_path / "modern"
    other.mkdir()
    assert _edit(other, "a\nb\n", [DOWN, CTRL_P, "X"], extra=MODERN) == "a\nXb\n"


# @scenario SC-modern-keymap-09
def test_the_borland_control_letters_still_work_in_borland(tmp_path):
    """Ctrl-Z deletes to the end of the row and Ctrl-Y the line, as before."""
    assert _edit(tmp_path, "abc\ndef\n", [CTRL_Z], extra=BORLAND) == "\ndef\n"
    other = tmp_path / "line"
    other.mkdir()
    assert _edit(other, "abc\ndef\n", [CTRL_Y], extra=BORLAND) == "def\n"


# @scenario SC-modern-keymap-10
def test_ctrl_q_quits(tmp_path):
    with WpeSession(str(tmp_path), "abc\n", filename="t.txt", env_extra=MODERN) as w:
        w.key(CTRL_Q, delay=0.5)
        deadline = time.time() + QUIT_WAIT_S
        while w.alive() and time.time() < deadline:
            w.key(delay=0.2)
        assert not w.alive(), "Ctrl-Q left the editor running"


# @scenario SC-modern-keymap-10
def test_f10_opens_the_menu_when_no_session_is_open(tmp_path):
    with WpeSession(str(tmp_path), "abc\n", filename="t.txt", env_extra=MODERN) as w:
        # F10 enters the menu bar at its first entry; Right then Enter opens File
        w.key("\x1b[21~", "\x1b[C", "\r", delay=0.6)
        assert "Save As" in "\n".join(w.display()), "\n".join(w.display())


# @scenario SC-modern-keymap-10
def test_the_borland_f2_still_saves(tmp_path):
    with WpeSession(str(tmp_path), "abc\n", filename="t.txt", env_extra=BORLAND) as w:
        w.key("X", "\x1bOQ", delay=0.5)
        assert w.text() == "Xabc\n"
