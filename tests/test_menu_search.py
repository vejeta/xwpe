"""Search-menu (Alt-S) regression tests for wpe (terminal mode).

Search items and bindings (we_menue.c):
  Find        -> e_find        (Text-to-Find dialog)
  Replace     -> e_replace     (Find/New-Text + Change All)
  Search again-> e_rep_search
  Go to Line  -> e_goto_line   (number dialog)

Find / Goto move the cursor but do not change the file, so we verify them by
typing a marker at the new cursor position and asserting on the saved file.
Replace changes the file directly.  Search/Replace is historically buggy
(amagnasco) so incoherences get @incoherence(...) for manual review.
"""
from wpe_driver import WpeSession, ALT


def test_goto_line_moves_cursor(tmp_path):
    """Go to Line N then typing edits line N."""
    with WpeSession(str(tmp_path), "L1\nL2\nL3\nL4\nL5\n") as w:
        w.menu(ALT.SEARCH, "g")              # Go to Line (number dialog)
        w.key("3", "\r")                     # line 3
        w.key("X")                           # insert marker at the cursor
        w.save()
        assert w.alive(), "wpe died during Search -> Go to Line"
        lines = w.text().splitlines()
        assert len(lines) >= 3 and lines[2].startswith("X"), \
            "Go to Line 3 should put the cursor on line 3, got %r" % w.text()


def test_find_moves_cursor_to_match(tmp_path):
    """Find a term, then typing inserts next to that match.

    The Text-to-Find field is active as soon as the dialog opens, so we type
    directly; Enter is the OK accelerator (crsw = AltO) and runs the search."""
    with WpeSession(str(tmp_path), "apple\nbanana\nCHERRY\ndate\n") as w:
        w.menu(ALT.SEARCH, "f")              # Find dialog (field active)
        w.key("CHERRY")                      # type into Text to Find
        w.key("\r")                          # OK -> search
        w.key("X")                           # marker at the match
        w.save()
        assert w.alive(), "wpe died during Search -> Find"
        t = w.text()
        assert "XCHERRY" in t or "CHERRYX" in t, \
            "Find should move the cursor to the match, got %r" % t


def test_replace_all_changes_every_occurrence(tmp_path):
    """Replace + Change All replaces every occurrence (prompt off).

    Field active on open -> type the search; Alt-N (NOT Enter, which would OK
    the dialog) moves to New Text; Alt-P clears the prompt; Alt-A is Change
    All; a "Replaced N" note is then dismissed with Enter."""
    with WpeSession(str(tmp_path), "foo 1\nfoo 2\nbar foo\n") as w:
        w.menu(ALT.SEARCH, "r")              # Replace dialog (Find field active)
        w.key("foo")                         # Text to Find
        w.key("\033n"); w.key("ZZZ")         # Alt-N -> New Text
        w.key("\033p")                       # turn OFF Prompt on Replace
        w.key("\033a")                       # Change All
        w.key("\r")                          # dismiss the "Replaced N" note
        w.save()
        assert w.alive(), "wpe died during Search -> Replace"
        t = w.text()
        assert "foo" not in t and t.count("ZZZ") == 3, \
            "Change All should replace every 'foo' with 'ZZZ', got %r" % t


# --- shortcut path (#159): Search accelerators, advertised in we_menue.c ---

F4 = "\033OS"            # kf4  -> Find
GOTO = "\033g"           # Alt-G -> Go to Line
SEARCH_AGAIN = "\x0c"    # ^L   -> Search again


def test_goto_line_via_alt_g(tmp_path):
    """Alt-G (advertised) opens Go to Line; line N then typing edits line N."""
    with WpeSession(str(tmp_path), "L1\nL2\nL3\nL4\nL5\n") as w:
        w.key(GOTO)                          # Alt-G
        w.key("3", "\r")                     # line 3
        w.key("X")                           # marker at the cursor
        w.save()
        assert w.alive(), "wpe died on Alt-G"
        lines = w.text().splitlines()
        assert len(lines) >= 3 and lines[2].startswith("X"), \
            "Alt-G then 3 should put the cursor on line 3, got %r" % w.text()


def test_find_via_f4(tmp_path):
    """F4 (advertised) opens Find; the cursor lands on the match."""
    with WpeSession(str(tmp_path), "apple\nbanana\nCHERRY\ndate\n") as w:
        w.key(F4)                            # F4 -> Find dialog (field active)
        w.key("CHERRY")
        w.key("\r")                          # OK -> search
        w.key("X")                           # marker at the match
        w.save()
        assert w.alive(), "wpe died on F4 (Find)"
        t = w.text()
        assert "XCHERRY" in t or "CHERRYX" in t, \
            "F4 Find should move the cursor to the match, got %r" % t


def test_search_again_via_ctrl_l(tmp_path):
    """^L (advertised Search again) repeats the last Find to the next match."""
    with WpeSession(str(tmp_path), "tag\nmid\ntag\nend\n") as w:
        w.key(F4)                            # Find
        w.key("tag"); w.key("\r")            # first match: line 1
        w.key(SEARCH_AGAIN)                  # ^L -> next match: line 3
        w.key("Z")                           # marker at the second match
        w.save()
        assert w.alive(), "wpe died on ^L (Search again)"
        lines = w.text().splitlines()
        assert len(lines) >= 3 and "Z" in lines[2], \
            "^L should advance to the 2nd 'tag' (line 3), got %r" % w.text()
