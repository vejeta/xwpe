"""The menus, as they are drawn, in every key style and block mode.

The menus are the same in both key styles and in every block mode; only the
key written beside an item follows the style, and the Block menu shows
`MarK Word` in the Modern style and in the Selection block mode.  Each row
opens every menu of one combination and compares the rows the dropdown
draws, and the help line it puts on the status row, with the file under
tests/menus/<style>/<mode>/<menu>.txt.

The Borland files in anchor and WordStar blocks are what the editor drew
before the menus became data, so the data cannot change a Borland user's
menus.  Set XWPE_MENU_GOLDEN_RECORD=1 to write the files instead of
comparing; a recorded file is read by a person before it is committed.

test_we_* is the one place that runs the editor under the name `we` on
purpose: that is the mode without the programming layer, whose menu bar has
eight entries and whose Window menu has a User Screen row, and the rewrite
of the menus moved exactly those decisions.
"""
import os
import re

import pytest

import wpe_driver
from wpe_driver import ALT, WpeSession

HERE = os.path.dirname(os.path.abspath(__file__))
GOLDEN_DIR = os.path.join(HERE, "menus")
ROOT = os.path.dirname(HERE)
RECORD = os.environ.get("XWPE_MENU_GOLDEN_RECORD") == "1"

FRESH = 0x98          # a fresh profile's Options2
BLOCK_BITS = {"anchor": 0, "wordstar": 0x0004, "selection": 0x0400}
STYLES = ("borland", "modern")
SEED = "hello world\nsecond line\nthird\n"
ESC = "\033"
MENU_OPEN_WAIT = 0.5
MENU_CLOSE_WAIT = 0.3

WPE_MENUS = [("system", ALT.SYSTEM), ("file", ALT.FILE), ("edit", ALT.EDIT),
             ("search", ALT.SEARCH), ("block", ALT.BLOCK), ("run", ALT.RUN),
             ("debug", ALT.DEBUG), ("project", ALT.PROJECT),
             ("options", ALT.OPTIONS), ("window", ALT.WINDOW),
             ("help", ALT.HELP)]
WE_MENUS = [m for m in WPE_MENUS if m[0] not in ("run", "debug", "project")]


def golden_path(style, mode, menu):
    return os.path.join(GOLDEN_DIR, style, mode, menu + ".txt")


def read_golden(style, mode, menu):
    with open(golden_path(style, mode, menu)) as fh:
        return fh.read()


def _dropdown(base, shown):
    """The rows that differ from the screen with no menu open."""
    return ["%2d|%s" % (y, b.rstrip())
            for y, (a, b) in enumerate(zip(base, shown)) if a != b]


def _write_rc(workdir, style, blocks):
    rc_dir = os.path.join(workdir, ".config", "xwpe")
    os.makedirs(rc_dir, exist_ok=True)
    with open(os.path.join(rc_dir, "xwperc"), "w") as fh:
        fh.write("[General]\nOptions2 : %d\n" % (FRESH | BLOCK_BITS[blocks]))


def dump_menus(workdir, style, blocks, menus):
    """Open every menu of the bar in one session; {menu: text}."""
    _write_rc(workdir, style, blocks)
    out = {}
    with WpeSession(workdir, SEED, filename="t.c",
                    env_extra={"XWPE_KEYS": style}) as s:
        base = s.display()
        out["bar"] = base[0].rstrip() + "\n"
        for name, seq in menus:
            s.key(seq, delay=MENU_OPEN_WAIT)
            out[name] = "\n".join(_dropdown(base, s.display())) + "\n"
            s.key(ESC, delay=MENU_CLOSE_WAIT)
            s.key(ESC, delay=MENU_CLOSE_WAIT)
        assert s.alive()
    return out


def _check_or_record(style, mode, dumped):
    wrong = []
    for menu, text in dumped.items():
        path = golden_path(style, mode, menu)
        if RECORD:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as fh:
                fh.write(text)
        elif read_golden(style, mode, menu) != text:
            wrong.append(menu)
    assert not wrong, "menus that differ from tests/menus/%s/%s: %s" % (
        style, mode, ", ".join(wrong))


# @scenario SC-menus-01
@pytest.mark.parametrize("blocks", sorted(BLOCK_BITS))
@pytest.mark.parametrize("style", STYLES)
def test_every_menu_matches_its_golden(tmp_path, style, blocks):
    """Eleven menus and the bar of `wpe`, in each style and block mode."""
    dumped = dump_menus(str(tmp_path), style, blocks, WPE_MENUS)
    _check_or_record(style, blocks, dumped)


# @scenario SC-menus-02
@pytest.mark.parametrize("style", STYLES)
def test_we_menus_match_their_golden(tmp_path, monkeypatch, style):
    """The mode without the programming layer: a bar of eight, Go to Line
    with Alt G in Borland, User Screen in the Window menu.  Run under the
    name `we` on purpose (see the module docstring)."""
    monkeypatch.setattr(wpe_driver, "WPE_BIN",
                        os.environ.get("WE_BIN") or os.path.join(ROOT, "we"))
    dumped = dump_menus(str(tmp_path), style, "anchor", WE_MENUS)
    _check_or_record(style, "we", dumped)


def _lines(style, mode, menu):
    return read_golden(style, mode, menu).splitlines()


def _block_menu_rows(style, mode):
    """The Block menu's item rows: what is drawn between the frame lines."""
    return [l for l in _lines(style, mode, "block") if "|" in l]


# @scenario SC-menus-03
@pytest.mark.parametrize("menu", [m for m, _ in WPE_MENUS if m != "block"])
def test_block_mode_changes_no_menu_but_block(menu):
    """Anchor, WordStar and Selection blocks draw the same menu, for every
    menu but Block, in both styles."""
    for style in STYLES:
        anchor = read_golden(style, "anchor", menu)
        assert read_golden(style, "wordstar", menu) == anchor
        assert read_golden(style, "selection", menu) == anchor


# @scenario SC-menus-04
def test_mark_word_is_the_one_row_the_block_mode_adds():
    """Borland keys: Selection blocks show Mark Word, anchor and WordStar do
    not, and nothing else in the Block menu differs.  Modern keys show it in
    every block mode."""
    anchor = _lines("borland", "anchor", "block")
    assert _lines("borland", "wordstar", "block") == anchor
    selection = _lines("borland", "selection", "block")
    assert len(selection) == len(anchor) + 1
    assert sum("MarK Word" in l for l in anchor) == 0
    assert sum("MarK Word" in l for l in selection) == 1
    for mode in sorted(BLOCK_BITS):
        modern = _lines("modern", mode, "block")
        assert sum("MarK Word" in l for l in modern) == 1
        assert _lines("modern", mode, "block") == _lines("modern", "anchor", "block")


# @scenario SC-menus-05
def test_mark_word_is_the_fourth_row_after_mark_line():
    rows = _block_menu_rows("modern", "anchor")
    mark = [i for i, t in enumerate(rows) if "Mark Line" in t][0]
    assert "MarK Word" in rows[mark + 1]
    assert "^D" in rows[mark + 1]
    assert "Goto Begin" in rows[mark + 2]


KEY_TOKEN = re.compile(r"^(\^\S+|F\d+|Alt|Shift|Del|Ins|/|[A-Z]|<\S+>)$")


def _words_without_keys(line):
    """The row with the key column taken out: what the menu says, not which
    key it names."""
    words = []
    for t in line.split("|", 1)[1].split():
        # a key that runs to the frame has the frame's `x` stuck to it
        bare = t[:-1] if len(t) > 1 and t.endswith("x") else t
        if KEY_TOKEN.match(t):
            continue
        words.append("x" if bare != t and KEY_TOKEN.match(bare) else t)
    return words


# @scenario SC-menus-06
def test_modern_menus_differ_from_borland_only_by_key_text():
    """Every item word of every menu is the same in both styles, and so are
    the menus' sizes; only the key beside an item differs."""
    for mode in sorted(BLOCK_BITS):
        for menu, _ in WPE_MENUS:
            if menu == "block" and mode != "selection":
                continue   # Borland has no Mark Word here: its own test
            b = _lines("borland", mode, menu)
            m = _lines("modern", mode, menu)
            assert len(b) == len(m), (mode, menu)
            for bl, ml in zip(b, m):
                assert _words_without_keys(bl) == _words_without_keys(ml), (mode, menu, bl, ml)


# @scenario SC-menus-07
def test_go_to_line_key_follows_the_mode_and_the_style():
    """Borland `wpe` shows no key (Alt-G is the assistant there), `we` shows
    Alt G, and Modern shows ^G in both."""
    def go_to_line(style, mode):
        rows = [l for l in _lines(style, mode, "search") if "Go to Line" in l]
        assert len(rows) == 1
        return rows[0]
    assert "Alt G" not in go_to_line("borland", "anchor")
    assert "Alt G" in go_to_line("borland", "we")
    assert "^G" in go_to_line("modern", "anchor")
    assert "^G" in go_to_line("modern", "we")
