"""Enter on a result list that has no entry (e_data_eingabe).

File > Grep and File > Find open a list window of the files that matched.
When nothing matched the list is empty, and Enter used to take the name of
an entry that does not exist: the editor died.  The list now says "No entry
in this list" and stays open until the user closes it.
"""
from wpe_driver import WpeSession, ALT

NO_MATCH = "zzqnomatchzzq"
NO_ENTRY = "No entry in this list"


def _open_empty_result(w, item):
    """Run File > <item> (g = Grep, f = Find) for text that is nowhere."""
    w.key(ALT.FILE, item, delay=1.2)
    w.key(NO_MATCH, delay=0.8)
    w.key("\r", delay=1.5)


def _screen(w):
    return "\n".join(w.display())


def _enter_says_no_entry(w, name):
    w.key("\r", delay=1.2)
    screen = _screen(w)
    assert w.alive(), "Enter in an empty %s window ended the editor" % name
    assert NO_ENTRY in screen, \
        "Enter in an empty %s window did not say so:\n%s" % (name, screen)
    w.key("\r", delay=0.8)                 # dismiss the note
    assert w.alive(), "dismissing the note in the %s window ended the editor" % name


# @scenario SC-empty-list-entry-01
def test_enter_in_an_empty_grep_result_leaves_the_editor_alive(tmp_path):
    """Grep with no match, then Enter in the empty result window."""
    with WpeSession(str(tmp_path), "int main(void) { return 0; }\n",
                    filename="t.c", wait=2.0) as w:
        _open_empty_result(w, "g")
        assert w.alive(), "wpe died while Grep listed no match"
        _enter_says_no_entry(w, "Grep")
        w.key("\x17", delay=0.8)           # Ctrl-W closes the list
        assert w.alive(), "closing the empty Grep window ended the editor"
        w.key("X", delay=0.5)
        w.save()
        assert w.text().startswith("X"), \
            "the editor window did not get the key back: %r" % w.text()


# @scenario SC-empty-list-entry-03
def test_enter_in_a_grep_result_with_a_match_still_opens_it(tmp_path):
    """Positive control: Grep that finds t.c, Enter opens the match and the
    list does not say it has no entry."""
    with WpeSession(str(tmp_path), "int zzqmatchzzq;\n", filename="t.c", wait=2.0) as w:
        w.key(ALT.FILE, "g", delay=1.2)
        w.key("zzqmatchzzq", delay=0.8)
        w.key("\r", delay=1.5)
        assert NO_ENTRY not in _screen(w), "a list with a match says it has no entry"
        w.key("\r", delay=1.5)
        screen = _screen(w)
        assert w.alive(), "Enter on a Grep match ended the editor"
        assert NO_ENTRY not in screen, "Enter on a Grep match said there is no entry"
        assert "Grep" not in screen.splitlines()[2], \
            "the Grep list is still on top after Enter:\n" + screen


# @scenario SC-empty-list-entry-02
def test_enter_in_an_empty_find_result_leaves_the_editor_alive(tmp_path):
    """Find (files by name) with no match, then Enter in the empty window."""
    with WpeSession(str(tmp_path), "int main(void) { return 0; }\n",
                    filename="t.c", wait=2.0) as w:
        _open_empty_result(w, "f")
        assert w.alive(), "wpe died while Find listed no match"
        _enter_says_no_entry(w, "Find")


def _key_says_no_entry(w, key, name):
    w.key(key, delay=1.5)
    screen = _screen(w)
    assert w.alive(), "%r in an empty %s list ended the editor" % (key, name)
    assert NO_ENTRY in screen, \
        "%r in an empty %s list did not say so:\n%s" % (key, name, screen)
    w.key("\r", delay=0.8)                 # dismiss the note
    assert w.alive(), "dismissing the note in the %s list ended the editor" % name


# @scenario SC-empty-list-entry-04
def test_alt_s_in_an_empty_grep_result_says_no_entry(tmp_path):
    """Alt-S is the list's other key for opening the selected entry."""
    with WpeSession(str(tmp_path), "int main(void) { return 0; }\n",
                    filename="t.c", wait=2.0) as w:
        _open_empty_result(w, "g")
        _key_says_no_entry(w, "\033s", "Grep")


# @scenario SC-empty-list-entry-05
def test_enter_in_an_empty_function_index_section_says_no_entry(tmp_path):
    """Help > Function Index with a MANPATH that holds no manual pages: the
    section lists nothing, and Enter or Alt-S says so."""
    (tmp_path / "noman").mkdir()
    with WpeSession(str(tmp_path), "int main(void) { return 0; }\n",
                    filename="t.c", wait=2.0,
                    env_extra={"MANPATH": str(tmp_path / "noman")}) as w:
        w.key(ALT.HELP, "u", delay=1.2)
        w.key("g", delay=2.0)
        assert "Function-Index" in _screen(w), _screen(w)
        _key_says_no_entry(w, "\r", "Function-Index")
        _key_says_no_entry(w, "\033s", "Function-Index")


# @scenario SC-empty-list-entry-06
def test_enter_in_a_project_list_whose_last_file_was_deleted(tmp_path):
    """Delete the only file of a project, then Enter or Alt-E: the deleted
    file is not opened again and the list says it has no entry."""
    (tmp_path / "demo.prj").write_text(
        "CMP=\tgcc\nCMPFLAGS=\t-g\nEXENAME=\tdemo\nFILES=\tmain.c\n")
    with WpeSession(str(tmp_path), "int main(void) { return 0; }\n",
                    filename="main.c", wait=2.0) as w:
        w.key(ALT.PROJECT, "p", delay=0.8)
        w.key("\r", delay=0.8)
        w.key("\r", delay=1.0)
        assert "main.c" in "\n".join(w.display()[4:20]), _screen(w)
        w.key("\033[3~", delay=1.0)        # Del: the only file
        _key_says_no_entry(w, "\r", "Project")
        _key_says_no_entry(w, "\033e", "Project")
