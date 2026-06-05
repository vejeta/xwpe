"""Help-menu (Alt-H) tests for xwpe (X11/Xft) -- twin of
tests/test_menu_help.py for wpe.

The Help viewer renders text we can't read back from an Xft screenshot, so we
assert on a screenshot diff that the viewer / prompt window opened.
"""
from conftest import changed_pixels


def test_help_editor_opens_a_window(xwpe):
    """Help -> Editor opens the manual viewer."""
    before = xwpe.screenshot()
    xwpe.menu("h", "e")                  # Help -> Editor
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died opening Help -> Editor"
    assert changed_pixels(before, after) > 3000, \
        "Help -> Editor should open the manual viewer (screen barely changed)"


def test_help_topic_search_opens_a_prompt(xwpe):
    """Help -> Topic Search opens an input prompt."""
    before = xwpe.screenshot()
    xwpe.menu("h", "t")                  # Help -> Topic Search
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died opening Help -> Topic Search"
    assert changed_pixels(before, after) > 1000, \
        "Help -> Topic Search should open a prompt (screen barely changed)"
