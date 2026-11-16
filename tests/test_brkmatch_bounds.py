"""Run > Matching Bracket on a first row that starts #else and holds a later '#'.

The backward walk skips the #if side above an #else row.  On the first row
there is none, and the walk used to go on reading a row above the first one,
which killed the editor.  The user must see "No Matching Bracket!" and keep
the editor.  Launched as wpe: under the name we the Run menu's programming
commands are off.
"""
from wpe_driver import WpeSession, ALT, wait_screen

END = "\x1b[4~"

# @scenario SC-brkmatch-bounds-01
def test_else_first_row_later_hash_keeps_the_editor(tmp_path):
    with WpeSession(str(tmp_path), "#else # xyz\n") as w:
        w.key(END)
        w.menu(ALT.RUN, "k")
        assert w.alive(), "the editor died on Matching Bracket: exit %r" % w.exit_code()
        assert wait_screen(w, "No Matching Bracket", tmo=5), "\n".join(w.display())
