"""The keys the editor names are the keys the table binds.

The key table (we_bindings.c) is the one place that says what a key does, and
the menus, the bottom bars and the key reference are made from its rows.  These
rows prove it from the outside, in both key styles:

  * every key a drop-down menu shows beside an item is a key the style binds;
  * the Help file names no function key that the style does not bind;
  * the Window menu: neither style binds a key for Tile and Cascade (the
    1.6 CUA style bound Shift-F5 and Shift-F4; the Modern style leaves them to
    the menu), so the menu names none.

The table is read from the dump of tools/keymap-texi, so a row added to the
table is checked without a line of this file changing.
"""
import os
import re
import subprocess

import pytest

from wpe_driver import WpeSession, ALT

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
TOOL = os.path.join(ROOT, "tools", "keymap-texi")
HELP = os.path.join(ROOT, "help.xwpe_eng")
ESC = "\033"

# xwperc Options2: the default editor options; the second style has the style
# bit and the three bits Modern brings, and says it was upgraded.
OPTIONS2 = {"borland": 152, "modern": 153 + 0x400 + 0x800 + 0x1000}

# Keys a menu may show that are not bindings: the Help viewer's own.
VIEWER_KEYS = {"<CR>", "<BSP>"}

MENUS = [("file", ALT.FILE), ("edit", ALT.EDIT), ("search", ALT.SEARCH),
         ("block", ALT.BLOCK), ("run", ALT.RUN), ("debug", ALT.DEBUG),
         ("window", ALT.WINDOW), ("help", ALT.HELP)]


def bound_keys(style):
    """Every key the style binds, as the doc names of the table's dump."""
    out = subprocess.run([TOOL, "--dump", style], check=True, capture_output=True,
                         text=True).stdout
    names = set()
    for line in out.splitlines():
        parts = line.split(" | ")
        names.add(parts[2])
    return names


def menu_spelling(name):
    """A doc name as a menu writes it: Ctrl- is ^, Alt and Shift lose the hyphen."""
    if name.startswith("Ctrl-"):
        return "^" + name[5:]
    if name.startswith("Alt-"):
        return "Alt " + name[4:]
    if name.startswith("Shift-"):
        return "Shift " + name[6:]
    return name


def open_session(tmp_path, style):
    conf = tmp_path / ".config" / "xwpe"
    conf.mkdir(parents=True, exist_ok=True)
    (conf / "xwperc").write_text("[General]\nOptions2 : %d\nKeysNotice : 1\n" % OPTIONS2[style])
    # the style comes from the file, so the drivers' default style is removed
    return WpeSession(str(tmp_path), "int main(void)\n{\n    return 0;\n}\n",
                      env_extra={"XWPE_KEYS": None})


def drop_down_rows(screen):
    """The text of each item of the drop-down on screen: the rows between the
    frame's top corners (l..k) and its bottom corners (m..j)."""
    top = None
    for y, row in enumerate(screen):
        m = re.search(r"l(q+)k", row)
        if m and top is None:
            top = (y, m.start(), m.end())
    if top is None:
        return []
    y0, c0, c1 = top
    rows = []
    for row in screen[y0 + 1:]:
        if row[c0:c0 + 1] == "m":
            break
        rows.append(row[c0 + 1:c1 - 1].strip())
    return rows


def split_item(text):
    """An item's words and its keys: the words end at the first run of two spaces."""
    parts = re.split(r"\s{2,}", text, maxsplit=1)
    words = parts[0]
    keys = []
    if len(parts) == 2:
        keys = [k.strip() for k in re.split(r"\s*/\s*", parts[1]) if k.strip()]
    return words, keys


@pytest.mark.skipif(not os.access(TOOL, os.X_OK), reason="tools/keymap-texi is not built")
@pytest.mark.parametrize("style", ["borland", "modern"])
def test_every_key_a_menu_names_is_bound_in_that_style(tmp_path, style):
    bound = {menu_spelling(n) for n in bound_keys(style)} | VIEWER_KEYS
    unbound = []
    with open_session(tmp_path, style) as s:
        for name, alt in MENUS:
            s.key(alt, delay=0.6)
            for text in drop_down_rows(s.display()):
                words, keys = split_item(text)
                for key in keys:
                    if key not in bound:
                        unbound.append("%s menu, item %r names %r" % (name, words, key))
            s.key(ESC, delay=0.4)
            s.key(ESC, delay=0.4)
    assert not unbound, "a menu names a key the %s style does not bind:\n%s" % (
        style, "\n".join(unbound))


@pytest.mark.skipif(not os.access(TOOL, os.X_OK), reason="tools/keymap-texi is not built")
@pytest.mark.parametrize("style,tile,cascade", [("modern", None, None),
                                                ("borland", None, None)])
def test_the_window_menu_names_the_keys_of_tile_and_cascade(tmp_path, style, tile, cascade):
    """The Window menu used to say Shift-F4 for Tile and Shift-F5 for Cascade
    in both styles, keys nobody bound the way it said; neither style binds
    them now, so the menu names none."""
    with open_session(tmp_path, style) as s:
        s.key(ALT.WINDOW, delay=0.6)
        items = {split_item(t)[0]: split_item(t)[1] for t in drop_down_rows(s.display())}
    assert items.get("Tile") == ([tile] if tile else []), items
    assert items.get("Cascade") == ([cascade] if cascade else []), items


def help_cua_section():
    """The lines of the Help file's page on the second key style."""
    with open(HELP, "rb") as fh:
        lines = fh.read().decode("utf-8", "replace").split("\n")
    start = next(i for i, l in enumerate(lines) if "Common User Access" in l and "(" in l
                 and "Alt-C" not in l)
    start = max(start, next(i for i, l in enumerate(lines) if "The keys have a second form" in l) - 3)
    end = next(i for i in range(start, len(lines)) if lines[i].startswith("\x06"))
    return lines[start:end]


@pytest.mark.skipif(not os.access(TOOL, os.X_OK), reason="tools/keymap-texi is not built")
def test_the_help_page_on_the_second_style_names_no_key_the_table_does_not_bind():
    """The page used to list the changed keys by hand, and a hand list drifts:
    it said Replace was Ctrl-F3 where the code binds Shift-F3.  The page now
    points to the generated tables; a key named there again must be one the
    table binds in some style."""
    bound = bound_keys("borland") | bound_keys("modern")
    spelled = {n.replace("-", " ") for n in bound}
    named = []
    for line in help_cua_section():
        for m in re.finditer(r"\b(?:(?:Ctrl|Alt|Shift) )?F[0-9]{1,2}\b", line):
            named.append(m.group(0))
    wrong = [k for k in named if k not in spelled]
    assert not wrong, "the Help page names keys the table does not bind: %r" % wrong
