"""Search-menu (Alt-S) tests for xwpe (X11/Xft) -- twin of
tests/test_menu_search.py for wpe.

Find / Goto move the cursor (verified by typing a marker and asserting on the
saved file); Replace + Change All edits the file directly.  Run under xwpe as
well as wpe because the X11 dialog field-editor and key decode are a separate
path, where search/replace behaviour could diverge.

The shared `xwpe` fixture seeds a multi-line C file.
"""
from conftest import incoherence


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


def test_replace_one_occurrence(xwpe):
    """Replace a single (unique) match and confirm it at the Yes/No prompt.

    Fields are switched with Tab: in xwpe the Alt-<letter> field hotkey does
    NOT move between dialog text fields (see the incoherence test below).
    The "replace ALL" path needs Alt-P / Change-All, which the same Alt bug
    breaks, so the all-occurrences case is the xfail below; here we verify the
    core replace works on one match via the per-occurrence prompt."""
    xwpe.menu("s", "r")                  # Replace dialog (Find field active)
    xwpe.type("main")                    # a token that occurs exactly once
    xwpe.key("Tab"); xwpe.type("ZZZ")    # Tab -> New Text
    xwpe.key("Return")                   # OK -> finds the match, prompts
    xwpe.key("y")                        # "Replace this occurrence?" -> Yes
    xwpe.key("Return")                   # dismiss any trailing note
    xwpe.save()
    assert xwpe.proc.poll() is None, "xwpe died during Search -> Replace"
    t = xwpe.saved_text()
    assert "main" not in t and "ZZZ" in t, \
        "Replace should change the 'main' match to 'ZZZ', got %r" % t


@incoherence("Alt-<letter> does not switch dialog text fields in xwpe (X11); "
             "Tab does.  Works in wpe (ncurses).  The Replace 'New Text' field "
             "is unreachable via its Alt-N hotkey, so this Replace yields no "
             "change (search text gets both words).")
def test_replace_via_alt_hotkey_switches_fields(xwpe):
    """EXPECTED: Alt-N jumps from 'Text to Find' to 'New Text', exactly as it
    does in wpe, so the replace succeeds.  Currently xwpe ignores the Alt
    hotkey inside the field editor -> xfail until fixed."""
    xwpe.menu("s", "r")                  # Replace dialog (Find field active)
    xwpe.type("int")                     # Text to Find
    xwpe.key("alt+n"); xwpe.type("ZZZ")  # Alt-N should move to New Text
    xwpe.key("alt+p")                    # Prompt off
    xwpe.key("alt+a")                    # Change All
    xwpe.key("Return")                   # dismiss "Replaced N"
    xwpe.save()
    assert xwpe.proc.poll() is None, "xwpe died during Search -> Replace"
    t = xwpe.saved_text()
    assert "int" not in t and t.count("ZZZ") >= 7, \
        "Alt-N should reach New Text so Change All replaces every 'int', got %r" % t
