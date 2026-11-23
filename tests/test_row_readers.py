"""Walks over the rows of a text stay inside the text.

Run > Beautify walks the rows of a C text, and the syntax colours of a file
with a continue column (Fortran) look at one column of the row below.  Each
used to read bytes the row does not own: the byte before a row, the byte
after its NUL, a column past its end.  On a plain build the answer then
depends on what the allocator left there; under a sanitizer the editor dies.
The user must get the same text every time and keep the editor.

Each row asserts, in this order:
  * the editor answered and is alive (a sanitizer report ends it);
  * the folder holds no file asan* (the report of a sanitizer build, whose
    options the row hands to the editor: the driver passes only its own
    environment, so ASAN_OPTIONS goes through env_extra with a log_path);
  * the saved text equals the text a plain build of the tree before the
    readers saved for the same keys (frozen below, since the answer must
    not change).

Driving conditions: the editor is started as wpe, never we: under the name we
the Run menu's programming commands are off.  Ctrl-O B opens the Beautify
dialog, Enter takes its defaults (the whole text).  The red proof of each row
is under a sanitizer build launched as wpe-asan; a plain build cannot see
these reads, which is why test_rowread.c holds the plain-build rows.

Run: WPE_BIN=$PWD/wpe tests/.venv/bin/python -m pytest -q tests/test_row_readers.py
(and WPE_BIN=$PWD/wpe-asan).
"""
import glob
import os
import time

import pytest

from wpe_driver import WpeSession

CTRL_O = "\x0f"
ENTER = "\r"
BEAUTIFY_DIALOG_S = 0.8
BEAUTIFY_RUN_S = 1.0
ASAN_OPTIONS = ("allow_user_segv_handler=0:detect_leaks=0:abort_on_error=1"
                ":log_path=%s/asan")
MAX_COLUMN = 120          # MaxColumn's default: the rows of a window are as wide
DIRECTIVE_FILL = "a" * (MAX_COLUMN - 3)     # "#" + fill + "/*" is MaxColumn bytes
SYNTAX_FILE = "syntax_def"

# (seed, text the plain build of the tree before the readers saved)
CONTINUED_DEFINE = [
    # the row after the continued #define is empty, so it has no last byte
    ("#define A \\\n\nx\n", "#define A \\\n\nx\n"),
    # the same inside a block, where the indentation of x is the answer
    ("int f()\n{\n#define A \\\n\nx;\ny;\n}\n",
     "int f()\n{\n#define A \\\n\n   x;\n   y;\n}\n"),
]
CONTINUED_DEFINE_BLANK = [
    ("#define A \\\n   \nx\n", "#define A \\\n   \nx\n"),
    ("int f()\n{\n#define A \\\n  \nx;\ny;\n}\n",
     "int f()\n{\n#define A \\\n  \n   x;\n   y;\n}\n"),
]
WIDE_DIRECTIVE = "#" + DIRECTIVE_FILL + "/*"
WIDE_DIRECTIVE_SEEDS = [
    (WIDE_DIRECTIVE + "\n", WIDE_DIRECTIVE + "\n"),
    (WIDE_DIRECTIVE, WIDE_DIRECTIVE + "\n"),
    (WIDE_DIRECTIVE + "\n*/ int  x;\n", WIDE_DIRECTIVE + "\n*/ int  x;\n"),
]


def _options_for(workdir):
    """The sanitizer options of a row: a report goes to a file in its folder."""
    return {"ASAN_OPTIONS": ASAN_OPTIONS % workdir}


def _sanitizer_reports(workdir):
    return glob.glob(os.path.join(workdir, "asan*"))


def _beautify(session):
    """Run > Beautify over the whole text, as the user does."""
    session.key(CTRL_O, "b", delay=BEAUTIFY_DIALOG_S)
    session.key(ENTER, delay=BEAUTIFY_RUN_S)


def _saved_after(session):
    """The file's bytes once the editor has saved it, None when it is gone.
    A sanitizer report ends the editor a moment after the key, so the row
    gives it that moment before it asks."""
    time.sleep(BEAUTIFY_RUN_S)
    if not session.alive():
        return None
    session.save()
    time.sleep(BEAUTIFY_RUN_S)
    if not session.alive():
        return None
    return session.disk().decode()


def _beautified(workdir, seed):
    """Open seed as t.c, Beautify it, and return (text saved or None)."""
    with WpeSession(workdir, seed, filename="t.c",
                    env_extra=_options_for(workdir)) as s:
        _beautify(s)
        return _saved_after(s)


def _assert_answer(workdir, saved, frozen, what):
    assert not _sanitizer_reports(workdir), \
        "%s: the sanitizer reported a read outside the text: %s" % (
            what, _sanitizer_reports(workdir))
    assert saved is not None, "%s: the editor did not answer" % what
    assert saved == frozen, "%s: Beautify gave another text than before" % what


def _run_seeds(tmp_path, seeds, what):
    for number, (seed, frozen) in enumerate(seeds):
        workdir = str(tmp_path / ("case%d" % number))
        os.makedirs(workdir)
        saved = _beautified(workdir, seed)
        _assert_answer(workdir, saved, frozen, "%s %r" % (what, seed[:24]))


# @scenario SC-row-readers-01
def test_beautify_after_continued_define_and_empty_row(tmp_path):
    """A continued #define followed by an empty row: the beautifier looks at
    the last character of the row above the next one, and an empty row has
    none.  The next row is indented as a plain statement, every time."""
    _run_seeds(tmp_path, CONTINUED_DEFINE, "continued define, empty row")


# @scenario SC-row-readers-02
def test_beautify_after_continued_define_and_blank_row(tmp_path):
    """The same with a row of blanks, which also has no last character."""
    _run_seeds(tmp_path, CONTINUED_DEFINE_BLANK, "continued define, blank row")


# @scenario SC-row-readers-03
def test_beautify_directive_row_as_wide_as_the_rows(tmp_path):
    """A directive row exactly as wide as MaxColumn that ends in an open
    comment, with and without a line break after it: the comment scan ends at
    the row's end and reads nothing behind it."""
    _run_seeds(tmp_path, WIDE_DIRECTIVE_SEEDS, "wide directive")


def _personal_home(tmp_path, continue_column):
    """A home folder whose syntax_def is the checkout's with the continue
    column of the .f rule set to continue_column; None when there is no
    syntax_def beside the binary."""
    shipped = os.path.join(os.path.dirname(os.path.abspath(
        os.environ.get("WPE_BIN", "wpe"))), SYNTAX_FILE)
    if not os.path.exists(shipped):
        return None
    with open(shipped) as fh:
        rules = fh.read()
    fortran_tail = "'    1  0 5 72"
    assert fortran_tail in rules, "the .f rule of syntax_def changed"
    personal = rules.replace(fortran_tail, "'    1  0 %d 72" % continue_column)
    home = tmp_path / ("home%d" % continue_column)
    (home / ".xwpe").mkdir(parents=True)
    (home / ".xwpe" / SYNTAX_FILE).write_text(personal)
    return str(home)


FORTRAN_SEED = "      A=1\n\nB\n"


# @scenario SC-row-readers-04
@pytest.mark.parametrize("column", [122, 5000, 5])
def test_syntax_rule_with_a_continue_column_past_the_rows(tmp_path, column):
    """A personal syntax_def whose .f rule has a continue column as wide as
    the rows, or wider: the file opens and stays open, and the rows that are
    shorter than the column carry the state of the row above.  Column 5 is the
    shipped rule and the control that always worked."""
    home = _personal_home(tmp_path, column)
    if home is None:
        pytest.skip("no syntax_def beside the binary")
    workdir = str(tmp_path / "work")
    os.makedirs(workdir)
    env = _options_for(workdir)
    env["HOME"] = home
    with WpeSession(workdir, FORTRAN_SEED, filename="t.f", env_extra=env) as s:
        time.sleep(BEAUTIFY_RUN_S)
        shown = "\n".join(s.display(drain=0))
        assert not _sanitizer_reports(workdir), \
            "continue column %d: the sanitizer reported a read outside " \
            "the text: %s" % (column, _sanitizer_reports(workdir))
        assert s.alive(), "continue column %d: the editor died" % column
        assert "A=1" in shown, "continue column %d: the file did not open" % column
