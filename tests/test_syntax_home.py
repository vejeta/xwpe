"""With HOME unset, the personal syntax file is looked for in the root folder.

The editor used to join an unset HOME as an empty folder, which named
".xwpe/syntax_def" under the folder the editor was started in.  That folder
may belong to a cloned project, so its file would have been read as the
user's own.  The row starts the editor with HOME unset in a folder that holds
such a file defining Pascal, which only a syntax file knows, and the system
copy out of reach: the keyword must be drawn like plain text.

Run: WPE_BIN=$PWD/wpe pytest tests/test_syntax_home.py (and wpe-asan).
"""
import os

from wpe_driver import WpeSession

PASCAL_LINE = "begin x end.\n"
PASCAL_KEYWORD = "begin"
PASCAL_PLAIN = "x end"           # its first cell is the plain identifier x


def _fg_at(session, text):
    """The foreground colour pyte holds for the first cell of `text`, or
    None when the text is not shown."""
    for y, row in enumerate(session.screen.display):
        x = row.find(text)
        if x >= 0:
            return session.screen.buffer[y][x].fg
    return None


def test_no_home_does_not_read_the_start_folders_syntax_file(tmp_path):
    no_lib = tmp_path / "no-system-lib"
    no_lib.mkdir()
    work = tmp_path / "project"
    work.mkdir()
    with WpeSession(str(work), PASCAL_LINE, filename="t.p", wait=2.0,
                    cols=100,
                    env_extra={"HOME": None, "XWPE_LIB": str(no_lib)}) as w:
        assert w.alive(), "the editor died starting without HOME"
        assert os.path.exists(os.path.join(str(work), ".xwpe", "syntax_def")), \
            "the start folder's syntax file is not in place"
        assert PASCAL_KEYWORD in "\n".join(w.display()), \
            "the file did not open"
        keyword = _fg_at(w, PASCAL_KEYWORD)
        plain = _fg_at(w, PASCAL_PLAIN)
        assert keyword == plain, \
            "'%s' is coloured (%s): the start folder's syntax file was " \
            "read as the user's" % (PASCAL_KEYWORD, keyword)
