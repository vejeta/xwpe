"""A bracketed paste with CR LF line endings: one line break per line.

The terminal reader (we_term.c, e_t_getch) turns a pasted LF into Enter so
the editor inserts a real line break.  Text copied from a CRLF source (a
Windows application, a DOS-format file) ends each line with CR *and* LF;
each of the two used to be an Enter of its own, so every pasted line was
followed by a blank one.  The reader now folds the LF that follows a CR
into that CR's line break (e_t_paste_swallows_lf), while a lone LF -- or
two of them, a blank line in a Unix paste -- still breaks the line as
before.
"""
from wpe_driver import WpeSession

ESC = "\033"
PASTE_BEG = ESC + "[200~"
PASTE_END = ESC + "[201~"


def test_pasted_crlf_is_one_line_break(tmp_path):
    text = "one\r\ntwo\n\nthree"
    with WpeSession(str(tmp_path), "", filename="t.txt") as s:
        s.key(PASTE_BEG + text + PASTE_END, delay=0.5)
        s.save()
        disk = s.text()
    assert disk.startswith("one\ntwo\n\nthree"), \
        "a pasted CR LF did not give one line break -- disk=%r" % disk
