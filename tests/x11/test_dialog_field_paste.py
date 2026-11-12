"""A paste into a nearly full dialog field stops at a whole character.

Twin of tests/test_dialog_field_paste.py under a real X server.  A euro sign
and an x are typed into the file and copied; Search > Replace's New Text field
(128 bytes) is filled to 2 bytes from full and the clipboard is pasted: the
euro sign needs 3 bytes, so none of it goes in, and the saved text must hold
no half of a character.
"""

EURO = "€"          # 3 bytes
FIELD_BYTES = 128


def test_x11_paste_keeps_only_whole_characters(xwpe):
    xwpe.type("x")
    xwpe.type(EURO, delay=0.9)
    xwpe.key("Home")
    xwpe.key("ctrl+k"); xwpe.key("b")
    xwpe.key("End")
    xwpe.key("ctrl+k"); xwpe.key("k")
    xwpe.key("ctrl+c")                   # the clipboard holds the euro sign and x
    xwpe.menu("s", "r")
    xwpe.type("int")
    xwpe.key("alt+n")
    xwpe.type("a" * (FIELD_BYTES - 2), delay=0.9)
    xwpe.key("ctrl+v")
    xwpe.key("alt+p")
    xwpe.key("alt+a")
    xwpe.key("Return")
    xwpe.save()
    assert xwpe.proc.poll() is None
    t = xwpe.saved_text()
    assert "a" * (FIELD_BYTES - 2) + "x" in t, t[:200]
    assert "a" * (FIELD_BYTES - 2) + "x" + EURO not in t, t[:200]
    assert "�" not in t, "half a character was written: %r" % t[:200]
