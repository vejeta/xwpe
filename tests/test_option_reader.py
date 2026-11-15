"""The reader of an option file frees what it takes and names what it cannot read.

The reader used to leave the open section's name behind at the end of every
file, and on a line it could not place it gave up for the whole file: the
box said only "Can't read Option-File", every setting after the bad line was
dropped, and the quit-time save then wrote the file again without them.  Now
each line is read whole into a buffer that is freed before the next, a line
that cannot be placed is counted and passed over, the box names the line and
the file, and a file with such a line is not rewritten at quit.

The leak rows run under the sanitizer build with leak detection on:
WPE_BIN=$PWD/wpe-asan (the binary must carry a wpe-prefixed name).  Each row
also asserts on the editor drawing or on a file on disk.
"""
import glob
import os
import shutil
import tempfile

from wpe_driver import ALT, WpeSession

SEED = "int main(void){return 0;}\n"
SEED_ON_SCREEN = "int main"
READER = "e_opt_read_stream"
READER_LINE = "e_opt_read_line"
READER_PREFIX = "e_opt_reader_"
# Frames that only allocate for their caller.
ALLOCATORS = ("malloc", "realloc", "calloc", "strdup", "getdelim", "getline",
              "WpeStrdup", "WpeMalloc", "WpeRealloc")
# A memory error (not a leak, which reads "ERROR: LeakSanitizer").
MEMORY_ERROR = "ERROR: AddressSanitizer"
ALT_X = "\033x"
TABSTOPS_LABEL = "Tabstops:"
TAB_SIZE = 3
BAD_LINE = "nocolon here"
BAD_LINE_NUMBER = 7
BOX_HEAD = "Can't read Option-File"
HELD_WORDS = "not saved at quit"
GOOD_RC = "[General]\nTab : 3\n[Language/C]\nCompiler : gcc\n"
# Line 7 cannot be read, line 8 sets the tab width, and the file asks to be
# saved at quit.
BAD_RC = ("# a project\n[General]\nAutosave : 1\n\n# one\n# two\n"
          + BAD_LINE + "\nTab : 3\n")


def _write_rc(tmp_path, text):
    """Write ./xwperc for a session started in tmp_path; return its path.
    @text is a str, or bytes for a file that holds a NUL."""
    path = os.path.join(str(tmp_path), "xwperc")
    data = text if isinstance(text, bytes) else text.encode()
    with open(path, "wb") as fh:
        fh.write(data)
    return path


def _write_user_rc(tmp_path, text):
    """Write the user's own option file under the session's HOME."""
    folder = os.path.join(str(tmp_path), ".config", "xwpe")
    os.makedirs(folder)
    path = os.path.join(folder, "xwperc")
    with open(path, "w") as fh:
        fh.write(text)
    return path


def _leak_env(tmp_path):
    """The environment that makes the sanitizer report leaks beside the test."""
    log = os.path.join(str(tmp_path), "asan")
    return {"ASAN_OPTIONS": "detect_leaks=1:abort_on_error=0:log_path=" + log}


def _sanitizer_report(tmp_path):
    """The text of every sanitizer log the editor wrote in tmp_path."""
    text = ""
    for name in glob.glob(os.path.join(str(tmp_path), "asan*")):
        with open(name) as fh:
            text += fh.read()
    return text


def _start_and_quit(tmp_path, rc_text):
    """Start on t.c with ./xwperc = rc_text, answer the box if one comes,
    quit with Alt-X; return the sanitizer text."""
    _write_rc(tmp_path, rc_text)
    with WpeSession(str(tmp_path), SEED, env_extra=_leak_env(tmp_path)) as w:
        if BOX_HEAD in "\n".join(w.display()):
            w.key("\r", delay=0.6)
        w.key(ALT_X)
    return _sanitizer_report(tmp_path)


def _leak_blocks(report):
    """The leak reports of the sanitizer text, each as its list of frames'
    function names, innermost first."""
    blocks, frames = [], None
    for row in report.splitlines():
        if "leak of" in row:
            frames = []
            blocks.append(frames)
        elif frames is not None and row.strip().startswith("#"):
            parts = row.split()
            in_library = (len(parts) < 4 or parts[2] != "in"
                          or parts[-1].startswith("("))
            frames.append(("" if in_library else parts[3]))
        elif frames is not None and not row.strip():
            frames = None
    return blocks


def _allocator_of(frames):
    """The first frame of a leak that is not an allocator or a library."""
    for name in frames:
        if name and name not in ALLOCATORS:
            return name
    return ""


def _reader_owned_leaks(report):
    """The leaks the reader itself made: its line and its section name.  A
    key's own reader (WpeReadGeneral and the like) allocates through the
    reader, and its leaks are not the reader's."""
    return [b for b in _leak_blocks(report)
            if _allocator_of(b) == READER_LINE
            or _allocator_of(b).startswith(READER_PREFIX)
            or _allocator_of(b) == READER]


def _assert_reader_leaks_nothing(report):
    owned = _reader_owned_leaks(report)
    assert not owned, "the reader leaks:\n" + report


def _tab_width_in_force(w):
    """The Tabstops value Options > Editor shows (the file's "Tab" key)."""
    w.key(ALT.OPTIONS, "e")
    screen = w.display()
    w.key("\033")
    for row in screen:
        if TABSTOPS_LABEL in row:
            return int(row.split(TABSTOPS_LABEL, 1)[1].split()[0])
    raise AssertionError("Options > Editor did not open:\n" + "\n".join(screen))


# @scenario SC-option-codec-06
def test_reader_frees_its_section_of_a_well_formed_file(tmp_path):
    _assert_reader_leaks_nothing(_start_and_quit(tmp_path, GOOD_RC))


# @scenario SC-option-codec-06
def test_reader_frees_its_section_after_a_line_without_a_colon(tmp_path):
    rc = GOOD_RC + BAD_LINE + "\n"
    _assert_reader_leaks_nothing(_start_and_quit(tmp_path, rc))


# @scenario SC-option-codec-06
def test_reader_survives_an_unclosed_header_then_an_option(tmp_path):
    rc = "[General]\nTab : 3\n[Compiler\nTab : 5\n[Language/C]\nCompiler : gcc\n"
    report = _start_and_quit(tmp_path, rc)
    assert MEMORY_ERROR not in report, "a memory report:\n" + report
    _assert_reader_leaks_nothing(report)


# @scenario SC-option-codec-06a
def test_box_names_the_line_and_the_file_and_the_rest_is_read(tmp_path):
    path = _write_rc(tmp_path, BAD_RC)
    with WpeSession(str(tmp_path), SEED) as w:
        screen = "\n".join(w.display())
        assert "line %d" % BAD_LINE_NUMBER in screen, screen
        assert os.path.basename(path) in screen, screen
        w.key("\r", delay=0.6)
        assert SEED_ON_SCREEN in "\n".join(w.display())
        assert _tab_width_in_force(w) == TAB_SIZE


# @scenario SC-option-codec-06b
def test_quit_does_not_rewrite_a_file_with_an_unreadable_line(tmp_path):
    path = _write_rc(tmp_path, BAD_RC)
    with WpeSession(str(tmp_path), SEED) as w:
        w.key("\r", delay=0.6)
        w.key(ALT_X)
    with open(path, newline="") as fh:
        assert fh.read() == BAD_RC


# @scenario SC-option-codec-06c
def test_unclosed_header_loses_its_lines_and_the_next_header_is_read(tmp_path):
    # Tab : 5 sits under a header that never closes: it belongs to no
    # section, and is not counted as a bad line of its own.  The good
    # [General] header after it is read, so its Tab : 4 is the one in force.
    rc = "[General]\nTab : 3\n[Compiler\nTab : 5\n[General]\nTab : 4\n"
    _write_rc(tmp_path, rc)
    with WpeSession(str(tmp_path), SEED) as w:
        screen = "\n".join(w.display())
        assert "line 3" in screen, screen
        assert "more" not in screen, screen
        w.key("\r", delay=0.6)
        assert _tab_width_in_force(w) == TAB_SIZE + 1


# @scenario SC-option-codec-06d
def test_control_good_file_gets_no_box_and_is_saved_at_quit(tmp_path):
    path = _write_rc(tmp_path, "[General]\nAutosave : 1\nTab : 3\n")
    with WpeSession(str(tmp_path), SEED) as w:
        assert BOX_HEAD not in "\n".join(w.display())
        w.key(ALT_X)
    with open(path) as fh:
        assert "[Language/C]" in fh.read()


# @scenario SC-option-codec-06e
def test_box_shows_a_short_path_whole_and_says_nothing_is_saved_at_quit(tmp_path):
    # A name that fits a row is not cut, and the box says why the options
    # will not be saved at quit (the screen is cleared when the editor ends,
    # so a status line then is never seen).
    short = tempfile.mkdtemp(prefix="o", dir="/var/tmp")
    try:
        path = _write_rc(short, BAD_RC)
        with WpeSession(short, SEED) as w:
            screen = "\n".join(w.display())
            assert path in screen, screen
            assert HELD_WORDS in screen, screen
    finally:
        shutil.rmtree(short)


# @scenario SC-option-codec-06f
def test_nul_byte_is_an_unreadable_line_and_does_not_join_the_next(tmp_path):
    # "Ta", NUL, "b : 5" used to read as the key "TaTab : 3" or lose the
    # line after it; now the line is refused as it stands and the next one
    # is read.
    _write_rc(tmp_path, b"[General]\nTa\0b : 5\nTab : 3\n")
    with WpeSession(str(tmp_path), SEED) as w:
        screen = "\n".join(w.display())
        assert "line 2" in screen, screen
        w.key("\r", delay=0.6)
        assert _tab_width_in_force(w) == TAB_SIZE


# @scenario SC-option-codec-06g
def test_new_keyboard_profile_does_not_rewrite_a_file_with_an_unreadable_line(tmp_path):
    # A 1.6 profile (Options2 : 1) is rewritten once at start to the new
    # keyboard style; a file with a line that cannot be read is left alone.
    rc = "[General]\nOptions2 : 1\n" + BAD_LINE + "\nTab : 3\n"
    path = _write_user_rc(tmp_path, rc)
    with WpeSession(str(tmp_path), SEED, env_extra={"XWPE_KEYS": None}) as w:
        assert BOX_HEAD in "\n".join(w.display())
        w.key("\r", delay=1.0)
    with open(path) as fh:
        assert fh.read() == rc


# @scenario SC-option-codec-06h
def test_options_save_lets_the_later_changes_be_saved_at_quit(tmp_path):
    # Control for the hold: once Options > Save has written the file whole,
    # the quit-time save is allowed again.
    path = _write_rc(tmp_path, BAD_RC)
    with WpeSession(str(tmp_path), SEED) as w:
        w.key("\r", delay=0.6)
        w.key(ALT.OPTIONS)
        w.key("s", delay=0.8)
        w.key("\r", delay=1.0)
        with open(path) as fh:
            assert BAD_LINE not in fh.read(), "Options > Save did not write the file"
        os.remove(path)
        w.key(ALT_X, delay=1.0)
    assert os.path.exists(path), "the quit-time save stayed held after a save"
