"""Search-menu (Alt-S) tests for xwpe (X11/Xft) -- twin of
tests/test_menu_search.py for wpe.

Find / Goto move the cursor (verified by typing a marker and asserting on the
saved file); Replace + Change All edits the file directly.  Run under xwpe as
well as wpe because the X11 dialog field-editor and key decode are a separate
path.

Note: the suite runs matchbox with an EMPTY kbdconfig (see
tests/x11/matchbox-kbdconfig); the default config grabs <Alt>n/p/c/d at the
root, which are xwpe's own dialog hotkeys, and would otherwise make Alt-N etc.
look broken.  With that fixed, the natural Alt-hotkey flow works in xwpe just
as in wpe.

The shared `xwpe` fixture seeds a multi-line C file.
"""


def test_goto_line_moves_cursor(xwpe):
    """Go to Line 3 then typing edits line 3."""
    xwpe.menu("s", "g")                  # Search -> Go to Line (number dialog)
    xwpe.type("3"); xwpe.key("Return")
    xwpe.type("X")                       # marker at the cursor
    xwpe.save()
    assert xwpe.proc.poll() is None, "xwpe died during Search -> Go to Line"
    lines = xwpe.saved_text().splitlines()
    assert len(lines) >= 3 and lines[2].startswith("X"), \
        "Go to Line 3 should put the cursor on line 3, got %r" % xwpe.saved_text()


def test_find_moves_cursor_to_match(xwpe):
    """Find a term, then typing inserts next to that match."""
    xwpe.menu("s", "f")                  # Find dialog (field active on open)
    xwpe.type("return")                  # Text to Find
    xwpe.key("Return")                   # OK -> search
    xwpe.type("X")                       # marker at the match
    xwpe.save()
    assert xwpe.proc.poll() is None, "xwpe died during Search -> Find"
    t = xwpe.saved_text()
    assert "Xreturn" in t or "returnX" in t, \
        "Find should move the cursor to the match, got %r" % t


def test_replace_all_changes_every_occurrence(xwpe):
    """Replace + Change All replaces every occurrence, driven by the same
    Alt-hotkey choreography as the wpe twin (Alt-N to New Text, Alt-P prompt
    off, Alt-A Change All)."""
    xwpe.menu("s", "r")                  # Replace dialog (Find field active)
    xwpe.type("int")                     # Text to Find
    xwpe.key("alt+n"); xwpe.type("ZZZ")  # Alt-N -> New Text
    xwpe.key("alt+p")                    # turn OFF Prompt on Replace
    xwpe.key("alt+a")                    # Change All
    xwpe.key("Return")                   # dismiss the "Replaced N" note
    xwpe.save()
    assert xwpe.proc.poll() is None, "xwpe died during Search -> Replace"
    t = xwpe.saved_text()
    assert "int" not in t and t.count("ZZZ") >= 7, \
        "Change All should replace every 'int' with 'ZZZ', got %r" % t


# --- shortcut path (#159): Search accelerators in the X11 input path ---
# Alt-G (Go to Line) and F4 (Find) are both asserted here.  F4 is a bare
# function key: it reaches xwpe only because conftest injects function keys BY
# KEYCODE (xdotool bug #491 would otherwise deliver Alt-F4) -- see the README
# "Function keys are injected by keycode" note.

def test_goto_line_via_alt_g(xwpe):
    """Alt-G (advertised) opens Go to Line; line 3 then typing edits line 3."""
    xwpe.key("alt+g")                    # Go to Line
    xwpe.type("3"); xwpe.key("Return")
    xwpe.type("X")
    xwpe.save()
    assert xwpe.proc.poll() is None, "xwpe died on Alt-G"
    lines = xwpe.saved_text().splitlines()
    assert len(lines) >= 3 and lines[2].startswith("X"), \
        "Alt-G then 3 should put the cursor on line 3, got %r" % xwpe.saved_text()


def test_find_via_f4(xwpe):
    """F4 (advertised Find accelerator) opens Find; the match moves the cursor,
    verified by typing a marker next to it."""
    xwpe.key("F4")                       # Find dialog (field active on open)
    xwpe.type("return")                  # Text to Find
    xwpe.key("Return")                   # OK -> search
    xwpe.type("X")                       # marker at the match
    xwpe.save()
    assert xwpe.proc.poll() is None, "xwpe died on F4 (Find)"
    t = xwpe.saved_text()
    assert "Xreturn" in t or "returnX" in t, \
        "F4 Find should move the cursor to the match, got %r" % t
