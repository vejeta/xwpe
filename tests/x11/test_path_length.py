"""A file name, a folder or a typed entry longer than the buffer that held it
no longer ends the graphical editor.

Twin of tests/test_path_length.py: the file manager, Show Definition and the
Find dialogs are shared code, but the graphical editor reads its keys and
draws its lists by another road, so the rows that crashed the terminal
editor are run here too.  A row asserts that the editor is alive afterwards
(an overflow caught by the sanitizer build aborts the process, and some
kill the plain build), and where the row can read an effect from a file it
does: the Replace row reads the saved text.

The wayland twin runs this same body.
"""
import pytest

from conftest import _sleep

SETTLE_S = 1.5
NAME_BYTES = 200            # a name over the 127 the old buffers held
NAME_MAX_BYTES = 255        # the longest name the system allows
ENTRY_BYTES = 120           # typed into a field that is 128 wide
WIDE_TEXT_BYTES = 110       # over the 80 bytes of the Find buffers
LETTERS_LOST_MAX = 10       # typed letters a compositor's synthetic keyboard may drop
LONG_WINDOW_NAME = "b" * (NAME_BYTES - 2) + ".c"
SHDEF_SOURCE = "#define ZZ 1\nint x = ZZ;\n"
SHDEF_SESSION = {"XWPE_TEST_FILE": LONG_WINDOW_NAME, "XWPE_TEST_SOURCE": SHDEF_SOURCE}
WIDE_SESSION = {"XWPE_TEST_SOURCE": "a" * WIDE_TEXT_BYTES + "\n"}
TREE_NAME = "a" * 250
TREE_SESSION = {"XWPE_TEST_FILE": TREE_NAME + "/s1/s2/s3/s4/t.c",
                "XWPE_TEST_SOURCE": "int x;\n"}


def _alive(xwpe, what):
    assert xwpe.proc.poll() is None, "xwpe died: " + what


@pytest.fixture
def long_named_file(tmp_path):
    """A second file in the editor's folder whose name is the 255 bytes the
    system allows; made before the file manager lists the folder."""
    name = "a" * (NAME_MAX_BYTES - 2) + ".c"
    (tmp_path / name).write_text("int picked_from_the_manager;\n")
    return name


@pytest.fixture
def tree_folder(tmp_path):
    """A folder four levels under a 250-byte name, where the file is."""
    folder = tmp_path / TREE_NAME / "s1" / "s2" / "s3" / "s4"
    folder.mkdir(parents=True)
    return folder


# @scenario SC-path-buffers-01x
def test_x11_file_manager_opens_a_file_with_a_long_name(long_named_file, xwpe):
    """F3, Enter, Enter on a file whose name is 255 bytes opens it."""
    _sleep(SETTLE_S)
    xwpe.key("F3", delay=1.5)
    xwpe.key("Return", delay=1.0)
    xwpe.key("Return", delay=2.5)
    _alive(xwpe, "F3, Enter, Enter on a 255-byte name")


# @scenario SC-path-buffers-02x
@pytest.mark.parametrize("xwpe", [TREE_SESSION], indirect=True)
def test_x11_file_manager_draws_a_tree_with_a_long_folder_name(tree_folder, xwpe):
    """F3 in a folder four levels under a 250-byte name draws its tree."""
    _sleep(SETTLE_S)
    xwpe.key("F3", delay=3.0)
    _alive(xwpe, "F3 under a 250-byte folder name")


# @scenario SC-path-buffers-03x
@pytest.mark.parametrize("xwpe", [SHDEF_SESSION], indirect=True)
def test_x11_show_definition_in_a_window_with_a_long_name(xwpe):
    """Ctrl-O S on a symbol in a window whose name is 200 bytes."""
    _sleep(SETTLE_S)
    xwpe.key("ctrl+o", delay=0.6)
    xwpe.key("s", delay=1.2)
    xwpe.type("ZZ", delay=0.6)
    xwpe.key("Return", delay=2.5)
    _alive(xwpe, "Show Definition in a 200-byte window")


# @scenario SC-path-buffers-07x
def test_x11_find_file_field_takes_what_was_typed(xwpe):
    """File > Find with 120 bytes in File:, then the dialog again."""
    _sleep(SETTLE_S)
    xwpe.menu("f", "f", delay=1.2)
    xwpe.type("q" * ENTRY_BYTES, delay=1.0)
    xwpe.key("Return", delay=2.5)
    _alive(xwpe, "File > Find with %d bytes in File:" % ENTRY_BYTES)
    xwpe.key("Escape", delay=1.0)
    xwpe.menu("f", "f", delay=1.5)
    _alive(xwpe, "File > Find opened again")


# @scenario SC-path-buffers-09x
@pytest.mark.parametrize("xwpe", [WIDE_SESSION], indirect=True)
def test_x11_replace_changes_all_of_a_wide_search_text(xwpe):
    """Replace with 110 bytes in Text to Find and a short New Text, Change
    All: the search text read as 80 bytes plus the New Text before."""
    target = "a" * WIDE_TEXT_BYTES
    _sleep(SETTLE_S)
    xwpe.menu("s", "r", delay=1.2)
    xwpe.type(target, delay=1.0)
    xwpe.key("alt+n", delay=0.5)
    xwpe.type("NEW", delay=0.5)
    xwpe.key("alt+p", delay=0.5)
    xwpe.key("alt+a", delay=2.0)
    xwpe.key("Return", delay=1.0)
    xwpe.save()
    _alive(xwpe, "Replace with %d bytes" % WIDE_TEXT_BYTES)
    saved = xwpe.saved_text().strip()
    # The synthetic keyboard under a compositor sometimes drops a typed
    # letter, which leaves a few of the text's letters behind the New Text;
    # the row asks that the search text matched, which it did not before.
    assert saved.startswith("NEW") and len(saved) < len("NEW") + LETTERS_LOST_MAX, saved[:200]
