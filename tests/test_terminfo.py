"""The editor and the test read one terminfo database (tests/terminfo.py).

A row that compares the strings the editor wrote at its end with the ones
tput gives used two databases on macOS: the editor's own (Homebrew's ncurses,
whose rmcup also pops xterm's title stack) and the system's tput.  These rows
give the editor a private database whose rmcup carries that pop and show
that the strings the row builds come from that database, and that a program
started with TERMINFO set reads it, as the editor does.  The stand-in for the
editor is tput itself, which reads TERMINFO the way ncurses does.

Run: tests/.venv/bin/python -m pytest -q tests/test_terminfo.py
"""
import os
import subprocess

import pytest

import terminfo
from host_tools import needs

pytestmark = needs("tput", "infocmp", "tic")

TITLE_STACK_POP = b"\x1b[23;0;0t"       # what Homebrew's rmcup adds to xterm's
OWN_MARK = "\\E[99;99;99t"                # a string no real terminal has


def _rmcup_with(extra):
    """An edit that makes rmcup end with @extra, as a newer ncurses does."""
    def edit(source):
        marker = "rmcup="
        at = source.index(marker)
        end = source.index(",", at)
        return source[:end] + extra + source[end:]
    return edit


def test_the_strings_come_from_the_database_the_row_was_given(tmp_path):
    database = terminfo.private_database(tmp_path, edit=_rmcup_with(OWN_MARK))
    host = terminfo.string("rmcup")
    private = terminfo.string("rmcup", database=database)
    assert private == host + b"\x1b[99;99;99t", (host, private)


def test_a_program_started_with_terminfo_set_reads_that_database(tmp_path):
    """The editor reads TERMINFO as tput does: what it writes at its end is
    the private database's string and not the host's."""
    database = terminfo.private_database(tmp_path, edit=_rmcup_with(OWN_MARK))
    ran = subprocess.run(["tput", "-T", terminfo.TERM_NAME, "rmcup"],
                         env=dict(os.environ, TERMINFO=database),
                         stdout=subprocess.PIPE, check=True).stdout
    assert ran == terminfo.string("rmcup", database=database)
    assert ran != terminfo.string("rmcup")


def test_with_the_host_database_alone_the_two_disagree_about_a_changed_entry(tmp_path):
    """The defect, put in a row: the editor given a database with the title
    stack pop in rmcup and the row asking the host's tput compared two
    databases and failed (macOS id 23).  Asking the private one agrees."""
    database = terminfo.private_database(tmp_path, edit=_rmcup_with(OWN_MARK))
    written_by_the_editor = subprocess.run(
        ["tput", "-T", terminfo.TERM_NAME, "rmcup"],
        env=dict(os.environ, TERMINFO=database), stdout=subprocess.PIPE,
        check=True).stdout
    assert written_by_the_editor != terminfo.string("rmcup"), \
        "asked of the host's database the row would fail on this entry"
    assert written_by_the_editor == terminfo.string("rmcup", database=database)


def test_the_crash_rows_release_string_is_built_from_the_database_it_is_given(tmp_path):
    """The caller of the helper: test_terminal_owner.release_string, which
    the two crash rows compare the editor's bytes with, must end with the
    private database's strings (what the stand-in editor, tput under
    TERMINFO, writes), not the host's."""
    import test_terminal_owner as owner
    database = terminfo.private_database(tmp_path, edit=_rmcup_with(OWN_MARK))
    written = b"".join(
        subprocess.run(["tput", "-T", terminfo.TERM_NAME, cap],
                       env=dict(os.environ, **terminfo.environment(database)),
                       stdout=subprocess.PIPE, check=True).stdout
        for cap in ("sgr0", "cnorm", "rmkx", "rmcup"))
    built = owner.release_string(database)
    assert built == owner.CRASH_OFF + owner.POINTER_RESET + written
    assert built.endswith(b"\x1b[99;99;99t"), "the changed rmcup is last"


def test_an_unchanged_private_database_gives_the_hosts_strings(tmp_path):
    database = terminfo.private_database(tmp_path)
    for cap in ("sgr0", "cnorm", "rmkx", "rmcup"):
        assert terminfo.string(cap, database=database) == terminfo.string(cap), cap


def test_the_title_stack_pop_is_what_the_hosts_rmcup_may_carry(tmp_path):
    """A control for the number above: where the host's rmcup already has the
    pop (a current xterm entry), the private database keeps it."""
    database = terminfo.private_database(tmp_path)
    host = terminfo.string("rmcup")
    if TITLE_STACK_POP not in host:
        pytest.skip("this host's rmcup has no title-stack pop")
    assert TITLE_STACK_POP in terminfo.string("rmcup", database=database)


def test_an_edit_that_the_database_does_not_keep_is_an_error_of_the_row(tmp_path, monkeypatch):
    """A changed entry that does not come back from the new database (the
    host's was read instead) stops the row, instead of running on the
    host's entry and passing for the wrong reason."""
    read = terminfo._source_of
    monkeypatch.setattr(terminfo, "_source_of",
                        lambda term, database=None: read(term))
    with pytest.raises(AssertionError, match="not picked up"):
        terminfo.private_database(tmp_path, edit=_rmcup_with(OWN_MARK))


def test_has_entry_knows_a_terminal_it_has_and_one_it_has_not():
    assert terminfo.has_entry(terminfo.TERM_NAME)
    assert not terminfo.has_entry("no-such-terminal-at-all")
