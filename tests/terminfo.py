"""One terminfo database for the editor and for the test that reads it.

The editor writes the strings its terminal database gives (the ones that
leave the alternate screen, the keypad and so on), and a row that checks
those bytes asks the host's tput for them.  On a host whose database the
editor does not read (macOS, where Homebrew's ncurses has its own, newer
entries than the system's) the two disagree about the same terminal and the
row fails on a comparison between two databases, not on the editor.

A row gets one database, builds the editor's environment from it
(TERMINFO=<database>) and asks for the strings from the same one, so what is
compared is a database with itself.  The database is the host's own entry
for the terminal, copied with infocmp and compiled with tic into the row's
folder, optionally changed by a function on its source (a row about a
terminal whose Backspace is ^H changes that one capability).
"""
import os
import subprocess

TERM_NAME = "xterm-256color"       # the terminal every row runs under
SOURCE_FILE = "entry.ti"
DATABASE_FOLDER = "terminfo"
TOOL_TIMEOUT_S = 30


def has_entry(term):
    """True when this host has a terminfo entry for @term.

    A row about a terminal the host may not know (rxvt-unicode) asks first
    and skips by name instead of failing on an editor that fell back to
    another terminal."""
    try:
        found = subprocess.run(["infocmp", term], stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, timeout=TOOL_TIMEOUT_S)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return found.returncode == 0


def _source_of(term, database=None):
    """The source text infocmp prints for @term, from @database when given,
    else from the host's own."""
    env = dict(os.environ)
    if database is not None:
        env["TERMINFO"] = database
    return subprocess.check_output(["infocmp", "-x", term], env=env, text=True,
                                   timeout=TOOL_TIMEOUT_S)


def _capabilities(source):
    """The capability words of a terminfo source, without its comments."""
    words = set()
    for line in source.splitlines():
        if line.startswith("#"):
            continue
        words.update(word.strip() for word in line.split(",") if word.strip())
    return words


def private_database(base, term=TERM_NAME, edit=None):
    """Build a terminfo database for @term under the folder @base and return
    the folder to give as TERMINFO.

    @edit, when given, is a function that takes the entry's source text and
    returns the changed text; the rows that need a terminal the host does not
    have ask for it here.  The entry is read back through the new database,
    so a database the editor and tput would not pick up is an error of the
    row, not a silent fallback to the host's."""
    source = _source_of(term)
    if edit is not None:
        source = edit(source)
    base = str(base)
    os.makedirs(base, exist_ok=True)
    source_file = os.path.join(base, SOURCE_FILE)
    with open(source_file, "w") as fh:
        fh.write(source)
    database = os.path.join(base, DATABASE_FOLDER)
    os.makedirs(database, exist_ok=True)
    subprocess.check_call(["tic", "-x", "-o", database, source_file],
                          timeout=TOOL_TIMEOUT_S)
    shown = _source_of(term, database)
    assert _capabilities(source) <= _capabilities(shown), \
        "the private terminfo entry was not picked up: %s" % sorted(
            _capabilities(source) - _capabilities(shown))
    return database


def environment(database):
    """The environment entry that makes the editor read @database: the
    row's env_extra, built from the same database its strings come from."""
    return {"TERMINFO": database}


def string(cap, term=TERM_NAME, database=None):
    """The bytes tput gives for the capability @cap of @term, from @database
    when given, else from the host's own.

    A row that checks what the editor wrote at its end compares it with
    these, so the database must be the one the editor was started with."""
    env = dict(os.environ)
    if database is not None:
        env["TERMINFO"] = database
    return subprocess.run(["tput", "-T", term, cap], check=True, env=env,
                          stdout=subprocess.PIPE, timeout=TOOL_TIMEOUT_S).stdout
