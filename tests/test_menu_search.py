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
from wpe_driver import WpeSession, ALT, incoherence


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


GOTO_SEED = "L1\nL2\nL3\nL4\nL5\n"


def _goto_field_line(w):
    """The screen line of the Go to Line dialog that holds its number field."""
    return [l for l in w.display() if "Goto Line Number" in l and "q" not in l]


def _goto_typed_then_leave_field(w, typed):
    """Open Go to Line, type @typed, move the focus to Ok so the field is
    drawn again from the value the dialog holds, and return that line."""
    w.menu(ALT.SEARCH, "g")
    w.key(*typed)
    w.key("\t")
    return _goto_field_line(w)


def _marker_lines(w):
    """Type a marker at the cursor, save, and return the 1-based lines it is on,
    after checking the rest of the file is untouched."""
    w.key("X")
    w.save()
    text = w.text()
    assert [l.replace("X", "") for l in text.splitlines()] == \
        ["L1", "L2", "L3", "L4", "L5"], "file damaged: %r" % text
    return [i + 1 for i, l in enumerate(text.splitlines()) if "X" in l]


def test_goto_line_field_keeps_a_ten_digit_number_whole(tmp_path):
    """A ten digit line number fills the Go to Line field: it still reads
    whole once the focus leaves the field, and Go to Line takes the cursor to
    the last line, as any number past the end does.

    The field has ten places: drawing it used to work out 10**10 in an int,
    which overflows (the sanitizer build stops on it when the dialog opens,
    and a plain build drew 1999999999 as 1410065407).  Moving the focus to Ok
    draws the field again from the value the dialog holds, so a field that
    cut the number short shows it here."""
    with WpeSession(str(tmp_path), GOTO_SEED) as w:
        field = _goto_typed_then_leave_field(w, "1999999999")
        assert field and "1999999999" in field[0], \
            "the field should still read 1999999999, screen:\n%s" % \
            "\n".join(w.display())
        w.key("\r")
        assert _marker_lines(w) == [5], "Go to Line 1999999999 missed the last line"
        assert w.alive(), "wpe died on Go to Line 1999999999"


def test_goto_line_past_the_int_range_goes_to_the_last_line(tmp_path):
    """Twelve digits typed into the ten place field leave its last ten,
    3456789012, which is past the end of any file: the field should keep
    showing a number once the focus leaves it, and Go to Line should take the
    cursor to the last line, as it does for 1999999999.  Which number the
    field then shows (the digits typed, or the largest it can hold) is left
    to the fix; only digits are required."""
    with WpeSession(str(tmp_path), GOTO_SEED) as w:
        field = _goto_typed_then_leave_field(w, "123456789012")
        after_label = field[0].split("Goto Line Number", 1)[1] if field else ""
        shown = (after_label.replace(":", " ").split() or [""])[0]
        assert shown.isdigit(), \
            "the field shows %r, not a number, screen:\n%s" % (
                shown, "\n".join(w.display()))
        w.key("\r")
        assert _marker_lines(w) == [5], "Go to Line 3456789012 missed the last line"
        assert w.alive(), "wpe died on Go to Line 3456789012"


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
GOTO = "\033g"           # Alt-G: Go to Line in `we`, the assistant in `wpe`
SEARCH_AGAIN = "\x0c"    # ^L   -> Search again


# @scenario SC-ai-menu-02
def test_goto_line_does_not_advertise_alt_g_in_programming_mode(tmp_path):
    """The programming editor gives Alt-G to the assistant, so the Search menu
    must not offer it for Go to Line here.

    The whole defect was that it did: the item read "Go to Line        Alt G"
    in both modes, and under `wpe` the key reached the assistant instead, so
    the editor answered an advertised shortcut with nothing at all.  The item
    is still reachable by its letter with the menu open, which the test above
    drives.  The plain editor keeps the accelerator and still honours it; this
    module cannot assert that, because the driver takes one binary for the
    whole file and that binary is `wpe`.
    """
    with WpeSession(str(tmp_path), "L1\nL2\nL3\nL4\nL5\n") as w:
        w.key(ALT.SEARCH)                    # open the menu, do not pick
        rows = [r for r in w.display() if "Go to Line" in r]
        assert w.alive(), "wpe died opening the Search menu"
        assert rows, "the Search menu has no Go to Line item:\n%s" % "\n".join(w.display())
        assert "Alt G" not in rows[0], \
            "the item still offers a key this mode cannot honour: %r" % rows[0]


# @scenario SC-ai-menu-02
def test_alt_g_leaves_the_buffer_alone_in_programming_mode(tmp_path):
    """Alt-G belongs to the assistant here, so it must not reach the file.

    A swallowed accelerator is one failure; a swallowed accelerator that types
    its letter into the buffer is a worse one, and that is what the old
    per-mode confusion risked.
    """
    with WpeSession(str(tmp_path), "L1\nL2\nL3\nL4\nL5\n") as w:
        w.key(GOTO)                          # Alt-G: the assistant's prefix
        w.key("\033")                        # leave whatever it opened
        w.save()
        assert w.alive(), "wpe died on Alt-G"
        assert w.text().splitlines()[:5] == ["L1", "L2", "L3", "L4", "L5"], \
            "Alt-G changed the buffer: %r" % w.text()


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
