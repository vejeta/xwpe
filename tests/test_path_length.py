"""A file name, a folder or a typed entry longer than the buffer that held it
no longer ends the editor.

Every row here crashed the editor, or corrupted its memory, because a name
or a path was copied into a fixed buffer with strcpy, strcat or sprintf:
the file manager's picked name (127 bytes), a tree row (255), Show
Definition's file names and its entry (127 and 79), the include folders of an
option file (255), the Directory field (one byte past its buffer), and the
text fields of Find, Replace, Grep and Info-Path (128 bytes into 80 or 128).
Each row asserts that the editor is alive and that no sanitizer report is on
the screen, so it is red on a plain build when the damage kills the editor
(the Find dialog, Show Definition, the tree) and red only under the
sanitizer build when the damage is silent (the file manager's name, the
include folder, the Directory field).

Run both: WPE_BIN=$PWD/wpe pytest tests/test_path_length.py, and with
WPE_BIN=$PWD/wpe-asan ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 for the
sanitizer build (the binary must carry a wpe-prefixed name).

Not driven here: the mouse drag of a file onto another file manager whose
folder plus the name passes the system's limit (we_mouse.c,
WpeMouseInFileDirList).  It needs two file managers laid out side by side and
a drag between their rows; the manual line for it is in
WAYLAND-TESTING.txt.  The refusal it says is covered at the C level by
tests/test_path.c (a 4000 byte folder plus a 200 byte name).
"""
import os

import pytest

from wpe_driver import WpeSession, ALT

CTRL_O = "\x0f"
F3 = "\033[13~"
ALT_D = "\033d"
DOWN = "\033[B"
SANITIZER = "AddressSanitizer"
NAME_BYTES = 200            # a name over the 127 the old buffers held
NAME_MAX_BYTES = 255        # the longest name the system allows
ENTRY_BYTES = 120           # typed into a field that is 128 wide
FIELD_BYTES = 128           # what a text field of Find or Grep takes
WIDE_TEXT_BYTES = 110       # over the 80 bytes of FIND's buffers, and a line
                            # the editor still searches whole in a 120 column window
TYPED_PAST_FIELD = 4300     # more than the Directory field takes
ASAN_ENV = {"ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1"}


def _screen(w):
    """The rows of the screen that have something on them."""
    return [r.rstrip() for r in w.display() if r.strip()]


def _raw(w):
    return bytes(w.raw).decode("utf-8", "replace")


def _intact(w, what):
    """The editor is alive and no sanitizer report was printed."""
    assert w.alive(), "the editor died: %s\n%s" % (what, "\n".join(_screen(w)[-8:]))
    assert SANITIZER not in _raw(w), "sanitizer report: %s\n%s" % (what, _raw(w)[-1500:])


def _show_definition(w, word):
    w.key(CTRL_O, delay=0.6)
    w.key("s", delay=1.2)
    w.key(word, delay=0.6)
    w.key("\r", delay=2.5)


def _not_found(w):
    return any("not found" in r for r in _screen(w))


def _deep_folder(base, total):
    """A folder under @base whose path is at least @total bytes, from names of
    60 bytes."""
    path = str(base)
    while len(path) < total:
        path = os.path.join(path, "d" * 60)
    os.makedirs(path, exist_ok=True)
    return path


# The file manager lists, picks and opens a file with a long name.
# @scenario SC-path-buffers-01
@pytest.mark.parametrize("name_bytes", [NAME_BYTES, NAME_MAX_BYTES])
def test_file_manager_opens_a_file_with_a_long_name(tmp_path, name_bytes):
    """F3, Enter, Enter on a file whose name is 200 bytes, or the 255 the
    system allows, opens it.  The picked name was kept in 128 bytes by
    strcpy, and the list's row of mode, name, size and date was built in 256
    bytes, which a name over about 214 bytes ran past on F3 itself."""
    name = "a" * (name_bytes - 2) + ".c"
    (tmp_path / name).write_text("int picked_from_the_manager;\n")
    with WpeSession(str(tmp_path), "int x;\n", filename="t.c", wait=2.0,
                    cols=120, rows=40, env_extra=ASAN_ENV) as w:
        w.key(F3, delay=1.5)
        w.key("\r", delay=1.0)
        w.key("\r", delay=2.5)
        _intact(w, "F3, Enter, Enter on a %d-byte name" % name_bytes)
        assert any("picked_from_the_manager" in r for r in _screen(w)), \
            "the file was not opened:\n" + "\n".join(_screen(w))


# The directory tree draws a row for a folder with a long name.
# @scenario SC-path-buffers-02
def test_file_manager_draws_a_tree_with_a_long_folder_name(tmp_path):
    """F3 in a folder four levels under a 250-byte name: a tree row is two
    blanks per level plus the name, which passed the 255 bytes it was built
    in."""
    folder = tmp_path / ("a" * 250) / "s1" / "s2" / "s3" / "s4"
    folder.mkdir(parents=True)
    with WpeSession(str(folder), "int x;\n", filename="t.c", wait=2.0,
                    cols=120, rows=40, env_extra=ASAN_ENV) as w:
        w.key(F3, delay=3.0)
        _intact(w, "F3 in a folder under a 250-byte name")
        assert any("aaaaaaaa" in r for r in _screen(w)), \
            "the manager did not open:\n" + "\n".join(_screen(w))


# Show Definition in a window with a long name.
# @scenario SC-path-buffers-03
def test_show_definition_in_a_window_with_a_long_name(tmp_path):
    """Ctrl-O S on a symbol in a window whose name is 200 bytes finds it; the
    window's name was copied into 128 bytes."""
    name = "b" * (NAME_BYTES - 2) + ".c"
    with WpeSession(str(tmp_path), "#define ZZ 1\nint x = ZZ;\n", filename=name,
                    wait=2.0, cols=120, rows=40, env_extra=ASAN_ENV) as w:
        _show_definition(w, "ZZ")
        _intact(w, "Show Definition in a 200-byte window")
        assert not _not_found(w), "\n".join(_screen(w))


# A folder's own xwperc and project name a long file.
# @scenario SC-path-buffers-04
def test_show_definition_through_a_project_with_a_long_file_name(tmp_path):
    """A start folder whose xwperc names a project, whose FILES holds a
    200-byte name: Ctrl-O S QQ and nothing else typed but the symbol.  The
    entry was copied into 128 bytes, so a cloned folder could end the
    editor."""
    name = "f" * (NAME_BYTES - 2) + ".c"
    (tmp_path / name).write_text("#define QQ 1\n")
    (tmp_path / "p.prj").write_text(
        "#\n# xwpe - project-file: p.prj\nCMP=\tgcc\nEXENAME=\tp\nFILES=\t%s\n" % name)
    (tmp_path / "xwperc").write_text("[Programming]\nProject : p.prj\n")
    with WpeSession(str(tmp_path), "int x = QQ;\n", filename="t.c", wait=2.0,
                    cols=120, rows=40, env_extra=ASAN_ENV) as w:
        _show_definition(w, "QQ")
        _intact(w, "Show Definition through a project with a 200-byte FILES entry")
        assert not _not_found(w), "\n".join(_screen(w))
        assert not any("longer than the system allows" in r for r in _screen(w)), \
            "a FILES entry the system allows was skipped as too long:\n" + "\n".join(_screen(w))
        assert any("#define QQ 1" in r for r in _screen(w)), \
            "the project file was not opened at the definition:\n" + "\n".join(_screen(w))


# A folder's own xwperc sets the include folders.
# @scenario SC-path-buffers-05
def test_show_definition_through_a_long_include_folder(tmp_path):
    """The start folder's xwperc sets IncludePath to a folder of over 300
    bytes holding h.h; the source includes <h.h>.  The folder and the name
    were joined in 256 bytes, and the found path written over the caller's
    buffer."""
    inc = _deep_folder(tmp_path / "inc", 320)
    with open(os.path.join(inc, "h.h"), "w") as fh:
        fh.write("#define ZZ 1\n")
    work = tmp_path / "work"
    work.mkdir()
    (work / "xwperc").write_text("[Programming]\nIncludePath : %s\n" % inc)
    with WpeSession(str(work), "#include <h.h>\nint x = ZZ;\n", filename="t.c",
                    wait=2.0, cols=120, rows=40, env_extra=ASAN_ENV) as w:
        _show_definition(w, "ZZ")
        _intact(w, "Show Definition through a 320-byte include folder")
        assert not _not_found(w), "\n".join(_screen(w))
        assert not any("longer than the system allows" in r for r in _screen(w)), \
            "a folder the system allows was skipped as too long:\n" + "\n".join(_screen(w))
        assert any("#define ZZ 1" in r for r in _screen(w)), \
            "h.h was not opened at the definition:\n" + "\n".join(_screen(w))


# @scenario SC-path-buffers-06
def test_an_include_folder_longer_than_the_system_allows_is_said(tmp_path):
    """An include folder of 4300 bytes cannot name a folder: the editor skips
    it and says so, instead of cutting it to the path of another folder or
    saying only that the symbol was not found."""
    inc = "/" + "i" * (TYPED_PAST_FIELD - 1)
    (tmp_path / "xwperc").write_text("[Programming]\nIncludePath : %s\n" % inc)
    with WpeSession(str(tmp_path), "#include <h.h>\nint x = ZZ;\n", filename="t.c",
                    wait=2.0, cols=120, rows=40, env_extra=ASAN_ENV) as w:
        _show_definition(w, "ZZ")
        _intact(w, "Show Definition through a 4300-byte include folder")
        assert any("longer than the system allows" in r for r in _screen(w)), \
            "\n".join(_screen(w))


# The Find dialogs take what they show.
# @scenario SC-path-buffers-07
def test_find_file_field_keeps_what_was_typed(tmp_path):
    """File > Find with 120 bytes typed into File:, then the dialog again.
    The field is 128 wide and its text went into 80 bytes, past the next
    member of the structure."""
    with WpeSession(str(tmp_path), "int x;\n", filename="t.c", wait=2.0,
                    cols=120, rows=40, env_extra=ASAN_ENV) as w:
        w.key(ALT.FILE, "f", delay=1.2)
        w.key("q" * ENTRY_BYTES, delay=1.0)
        w.key("\r", delay=2.5)
        _intact(w, "File > Find with %d bytes in File:" % ENTRY_BYTES)
        w.key("\033", delay=1.0)
        w.key(ALT.FILE, "f", delay=1.5)
        _intact(w, "File > Find opened again")
        assert any("qqqqqqqq" in r for r in _screen(w)), \
            "the field lost what was typed:\n" + "\n".join(_screen(w))


def _new_text_row(w):
    """The row of the Replace dialog that carries the New Text field."""
    for r in _screen(w):
        if "New Text:" in r:
            return r.split("New Text:", 1)[1]
    return None


# @scenario SC-path-buffers-08
@pytest.mark.parametrize("menu,item,name,leave", [
    (ALT.SEARCH, "f", "Search > Find", "\033"),       # Esc ends the "not found" note
    (ALT.FILE, "g", "File > Grep", "\x17"),           # Ctrl-W closes the results window
])
def test_find_and_grep_text_stays_inside_its_own_buffer(tmp_path, menu, item, name, leave):
    """110 bytes typed into Text to Find of Find or Grep (a field of 128 over
    a buffer of 80), then the Replace dialog: its New Text field is still
    empty.  The overflow went into the next member of the structure, which is
    the Replace text, so no memory checker sees it."""
    with WpeSession(str(tmp_path), "a" * WIDE_TEXT_BYTES + "\n", filename="t.c", wait=2.0,
                    cols=120, rows=40, env_extra=ASAN_ENV) as w:
        w.key(menu, item, delay=1.2)
        w.key("a" * WIDE_TEXT_BYTES, delay=1.0)
        w.key("\r", delay=2.5)
        _intact(w, name + " with %d bytes" % WIDE_TEXT_BYTES)
        w.key(leave, delay=0.8)
        w.key(ALT.SEARCH, "r", delay=1.5)
        _intact(w, "Replace opened after " + name)
        new_text = _new_text_row(w)
        assert new_text is not None, "no Replace dialog:\n" + "\n".join(_screen(w))
        assert "aaaa" not in new_text, \
            "the Replace text holds what Find overflowed into it:\n" + "\n".join(_screen(w))


# @scenario SC-path-buffers-09
def test_replace_changes_all_of_a_whole_width_search_text(tmp_path):
    """Replace with 110 bytes in Text to Find and a short New Text, Change
    All.  The New Text was copied after the search text had run into its
    buffer, so the search then read as 80 bytes plus the New Text and matched
    nothing."""
    target = "a" * WIDE_TEXT_BYTES
    with WpeSession(str(tmp_path), target + "\n", filename="t.c", wait=2.0,
                    cols=120, rows=40, env_extra=ASAN_ENV) as w:
        w.key(ALT.SEARCH, "r", delay=1.2)
        w.key(target, delay=1.0)
        w.key("\033n", delay=0.5)
        w.key("NEW", delay=0.5)
        w.key("\033p", delay=0.5)         # Prompt on Replace off
        w.key("\033a", delay=2.0)         # Change All
        w.key("\r", delay=1.0)            # the "Replaced N" note
        w.save()
        _intact(w, "Replace with %d bytes" % WIDE_TEXT_BYTES)
        assert w.text().strip() == "NEW", \
            "Change All did not replace the %d byte text: %r" % (WIDE_TEXT_BYTES, w.text())


# @scenario SC-path-buffers-10
def test_show_definition_entry_longer_than_79_bytes(tmp_path):
    """Show Definition's field takes 128 bytes and the text went into 80."""
    with WpeSession(str(tmp_path), "int x;\n", filename="t.c", wait=2.0,
                    cols=120, rows=40, env_extra=ASAN_ENV) as w:
        _show_definition(w, "Q" * 110)
        _intact(w, "Show Definition with 110 bytes typed")


# @scenario SC-path-buffers-11
def test_info_path_with_a_whole_field(tmp_path):
    """Options > Help (Info-Path) with 128 bytes typed: the field takes 128
    and its text went into 128 bytes, one short of its terminator."""
    with WpeSession(str(tmp_path), "int x;\n", filename="t.c", wait=2.0,
                    cols=120, rows=40, env_extra=ASAN_ENV) as w:
        w.key(ALT.OPTIONS, "h", delay=1.2)
        w.key("p" * FIELD_BYTES, delay=1.0)
        w.key("\r", delay=2.0)
        _intact(w, "Info-Path with %d bytes" % FIELD_BYTES)


# A history entry from a roomier field is not copied past a narrower one.
# @scenario SC-path-buffers-13
def test_history_entry_longer_than_the_name_field_keeps_the_field(tmp_path):
    """File > Find with 120 bytes in File:, then the file manager's Name
    field (79 bytes) picks that entry from the list the two fields share.
    The entry was copied whole into the 80-byte name buffer."""
    with WpeSession(str(tmp_path), "int x;\n", filename="t.c", wait=2.0,
                    cols=120, rows=40, env_extra=ASAN_ENV) as w:
        w.key(ALT.FILE, "f", delay=1.2)
        w.key("q" * ENTRY_BYTES, delay=1.0)
        w.key(DOWN, delay=1.0)            # the list: the typed text joins it
        w.key("\033", delay=0.8)
        w.key("\033", delay=0.8)          # leave the Find dialog
        w.key(F3, delay=1.5)
        w.key(DOWN, delay=1.0)            # the Name field's list
        w.key(DOWN, delay=0.5)
        w.key("\r", delay=1.0)            # pick the 120-byte entry
        _intact(w, "a 120-byte history entry picked into the Name field")
        w.key("\r", delay=1.5)
        _intact(w, "Enter in the file manager after the pick")


# The Directory field of the file manager holds the longest path.
# @scenario SC-path-buffers-12
def test_directory_field_takes_the_longest_path_and_no_more(tmp_path):
    """F3, Alt-D, then more bytes than the field takes.  The field's buffer
    held its capacity and not the terminator after it: one byte of heap past
    the end once the field was full."""
    with WpeSession(str(tmp_path), "int x;\n", filename="t.c", wait=2.0,
                    cols=120, rows=40, env_extra=ASAN_ENV) as w:
        w.key(F3, delay=1.5)
        w.key(ALT_D, delay=1.0)
        w.key("/" + "z" * (TYPED_PAST_FIELD - 1), delay=2.0)
        _intact(w, "the Directory field filled past its capacity")
        w.key("\r", delay=2.5)
        _intact(w, "Enter on a Directory field filled past its capacity")
