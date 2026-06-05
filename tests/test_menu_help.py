"""Help-menu (Alt-H) regression tests for wpe (terminal mode).

Help items (we_menue.c, the last main menu):
  Editor (F1)    -> e_help (the manual viewer)
  Topic Search   -> e_topic_search     Function Index -> e_funct_in
  Info / Goto / Back / Next / Prev (navigation within the viewer)

The help viewer renders text, so these are checked on the rendered screen.
"""
from wpe_driver import WpeSession, ALT


def _screen(w):
    return "\n".join(w.display())


def test_help_editor_opens_the_manual(tmp_path):
    """Help -> Editor opens the manual viewer with the XWPE overview text."""
    with WpeSession(str(tmp_path), "AAA\n") as w:
        w.menu(ALT.HELP, "e")            # Help -> Editor
        scr = _screen(w)
        assert w.alive(), "wpe died opening Help -> Editor"
        assert "Help" in scr and "Programming Environment" in scr, \
            "Help -> Editor should open the manual, got:\n%s" % scr


def test_help_topic_search_opens_a_prompt(tmp_path):
    """Help -> Topic Search opens an input (search the help index)."""
    with WpeSession(str(tmp_path), "AAA\n") as w:
        before = _screen(w)
        w.menu(ALT.HELP, "t")            # Help -> Topic Search
        after = _screen(w)
        assert w.alive(), "wpe died opening Help -> Topic Search"
        assert after != before, \
            "Help -> Topic Search should change the screen (open a prompt)"
