"""A dialog closed with Esc and a File Manager visit cost no memory.

A text field left with Esc kept its working copy (e_schreib_leiste_lines
returned without freeing it), and every opening of the File Manager kept the
two buffers WpeCreateWorkingDirTree split the current folder in, because its
loop always left through an early return that skipped the frees.  Both showed
in the sanitizer's leak report at a normal quit.

The rows run under the sanitizer build with leak detection on:
WPE_BIN=$PWD/wpe-asan (the binary must carry a wpe-prefixed name).  Under a
plain build they skip by name.  Each row also asserts that the editor came
back to its text.
"""
import glob
import os

import pytest

from wpe_driver import WPE_BIN, WpeSession

SEED = "int main(void){return 0;}\n"
SEED_ON_SCREEN = "int main"
ALT_X = "\033x"
ESC = "\033"
F3 = "\033OR"
ALT_N = "\033n"
# The functions that own the memory these rows watch.
FIELD_OWNER = "e_schreib_leiste_lines"
FOLDER_TREE_OWNER = "WpeCreateWorkingDirTree"
# Frames that only allocate for their caller.
ALLOCATORS = ("malloc", "realloc", "calloc", "strdup", "WpeStrdup",
              "WpeMalloc", "WpeRealloc")
SANITIZER_BUILD = "asan" in os.path.basename(WPE_BIN)

pytestmark = pytest.mark.skipif(
    not SANITIZER_BUILD,
    reason="needs the sanitizer build: WPE_BIN must name wpe-asan")


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


def _leaks_made_by(report, owner):
    """The leaks whose memory @owner allocated."""
    return [b for b in _leak_blocks(report) if _allocator_of(b) == owner]


def _visit_and_quit(tmp_path, keys):
    """Start on t.c, send @keys, check the text is in front again, quit with
    Alt-X; return the sanitizer text."""
    with WpeSession(str(tmp_path), SEED, env_extra=_leak_env(tmp_path)) as w:
        for key in keys:
            w.key(key, delay=0.6)
        assert any(SEED_ON_SCREEN in row for row in w.display()), \
            "the text did not come back after the keys:\n" + "\n".join(w.display())
        w.key(ALT_X)
    return _sanitizer_report(tmp_path)


# @scenario SC-buffer-11
def test_a_dialog_field_left_with_esc_frees_its_copy(tmp_path):
    """F3 opens the File Manager, Alt-N goes to its name field, Esc leaves
    the field and Esc closes the box: the field's copy is freed."""
    report = _visit_and_quit(tmp_path, [F3, ALT_N, ESC, ESC])
    leaks = _leaks_made_by(report, FIELD_OWNER)
    assert not leaks, "a field left with Esc keeps its copy:\n" + report


# @scenario SC-buffer-11
def test_a_file_manager_visit_frees_the_folder_split(tmp_path):
    """Opening and closing the File Manager frees the two buffers the current
    folder was split in."""
    report = _visit_and_quit(tmp_path, [F3, ESC])
    leaks = _leaks_made_by(report, FOLDER_TREE_OWNER)
    assert not leaks, "a File Manager visit keeps the folder split:\n" + report
