"""What the assistant's checkpoint does with a name that is not a plain file.

A run that may write takes a checkpoint first: a before image of every file in
its scope, so a revert can put the files back.  The scope lists the files open
in the editor, the project's members and the source-like names of the folder,
and the folder is the user's, so anything can be under a name there: a pipe
nobody writes to, a socket, a link to a file elsewhere, a file too large to
hold whole, a folder called d.c.  Every file the checkpoint reads goes through
the read owner (we_read.c), which refuses what is not a regular file by what it
is, never waits, never follows a link and never cuts a file short.

What the user sees, and the rows below hold:

  - a pipe, a socket, a link or a file over the bound that sits in the scope is
    left out of the checkpoint and named in the pane with the reason, and the
    run, the review and the revert of every other file go on;
  - a folder whose name looks like a source file is not listed as a file;
  - nothing waits: a checkpoint over a pipe used to stop the whole editor.

The before image of a file whose bytes the owner read whole is the only copy a
revert has, so a file over the bound is refused rather than saved cut short.
That a file swapped for another between the look and the read is refused is
held by tests/test_read_owner.c (the identity rows), where the open can be
hooked; a terminal row cannot swap a name between two system calls.

Run: WPE_BIN=$PWD/wpe pytest tests/test_ai_read_owner.py (as wpe, never we).
"""
import os
import re
import socket

import pytest

from test_ai_changeset import (_ai_build, _auto_run, _revert_all, _trace_text,
                               ORIG, WRITE_REPLY)
from wpe_driver import pane_text_with_history

pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")

LEFT_OUT = "left out of the checkpoint"
BEFORE_IMAGE_BOUND = 4000000      # the most bytes one before image holds
TOO_LARGE = "it is larger than %d bytes" % BEFORE_IMAGE_BOUND
VICTIM_TEXT = "USER DATA\n"
# The images a run over a folder with only t.c keeps (measured by the control
# row below, green on every tree): the number the other rows compare with, so
# a change in how the scope lists t.c cannot make a row pass for the wrong reason.
PLAIN_RUN_IMAGES = 2


def _images_kept(trace_text):
    """How many before images the checkpoint kept, from its trace line."""
    found = re.search(r"checkpoint snapshot n=(\d+) failed=0", trace_text)
    return int(found.group(1)) if found else None


def _plant_socket(folder, name):
    """A Unix socket called `name` in `folder`.  The name is bound relative to
    the folder, so the length of the folder's path does not matter."""
    sock = socket.socket(socket.AF_UNIX)
    here = os.getcwd()
    os.chdir(folder)
    try:
        sock.bind(name)
    finally:
        os.chdir(here)
    sock.close()


def _left_out_run(tmp_path, plant, needles):
    """A run that rewrites t.c, in a plain folder where `plant(folder)` has put
    the odd name.  Returns (the pane's text, t.c after a revert of the whole
    run, the trace, whether the editor still answers)."""
    reply = WRITE_REPLY % (tmp_path / "t.c")
    with _auto_run(tmp_path, "snapshot", reply,
                   prepare=lambda t: plant(t.parent)) as (s, t, trace):
        _revert_all(s, trace)
        pane = "\n".join(pane_text_with_history(s, needles))
        alive = s.alive()
    return pane, t.read_text(), _trace_text(trace), alive


def _assert_named_and_reverted(pane, after_revert, alive, name, reason):
    assert LEFT_OUT in pane, "the pane does not say %s was left out:\n%s" % (name, pane)
    assert name in pane, "the pane does not name %s:\n%s" % (name, pane)
    assert reason in pane, "the pane does not say why (%s):\n%s" % (reason, pane)
    assert after_revert == ORIG, "the revert did not put t.c back: %r" % after_revert
    assert alive, "the editor stopped answering"


def test_a_run_over_a_plain_folder_keeps_the_images_the_other_rows_expect(tmp_path):
    """The control: nothing odd in the folder, nothing left out, and the number
    of images the other rows compare with."""
    pane, after, txt, alive = _left_out_run(tmp_path, lambda d: None, [LEFT_OUT])
    assert LEFT_OUT not in pane, "a plain folder had a file left out:\n" + pane
    assert _images_kept(txt) == PLAIN_RUN_IMAGES, txt
    assert after == ORIG and alive


# @scenario SC-read-owner-20
def test_a_pipe_in_the_scope_is_left_out_of_the_checkpoint_and_named(tmp_path):
    """A pipe called pipe.c in the folder.  The checkpoint used to open it and
    wait for a writer for ever; now the run goes on, the pane names the pipe
    and says it is one, and the revert puts t.c back."""
    pane, after, txt, alive = _left_out_run(
        tmp_path, lambda d: os.mkfifo(d / "pipe.c"), [LEFT_OUT, "pipe.c", "it is a pipe"])
    _assert_named_and_reverted(pane, after, alive, "pipe.c", "it is a pipe")


# @scenario SC-read-owner-21
def test_a_link_in_the_scope_is_left_out_and_never_followed(tmp_path):
    """l.c is a link to a file of the user's outside the folder.  The checkpoint
    does not follow it: no before image of the outside file is taken, the pane
    says the name is a link, and the outside file is as it was."""
    victim = tmp_path.parent / (tmp_path.name + ".victim")
    victim.write_text(VICTIM_TEXT)

    def plant(d):
        os.symlink(victim, d / "l.c")

    pane, after, txt, alive = _left_out_run(tmp_path, plant, [LEFT_OUT, "l.c", "it is a link"])
    _assert_named_and_reverted(pane, after, alive, "l.c", "it is a link")
    assert victim.read_text() == VICTIM_TEXT, "the outside file changed"
    assert _images_kept(txt) == PLAIN_RUN_IMAGES, \
        "a before image was taken of the file the link leads to:\n" + txt


# @scenario SC-read-owner-22
def test_a_file_over_the_bound_is_left_out_and_never_cut(tmp_path):
    """big.c is one byte over what a before image holds.  An image cut short
    would be restored short, so the file is left out whole and the pane names
    it and the bound; no image of it exists."""
    def plant(d):
        (d / "big.c").write_bytes(b"x" * (BEFORE_IMAGE_BOUND + 1))

    pane, after, txt, alive = _left_out_run(tmp_path, plant, [LEFT_OUT, "big.c", TOO_LARGE])
    _assert_named_and_reverted(pane, after, alive, "big.c", TOO_LARGE)
    assert _images_kept(txt) == PLAIN_RUN_IMAGES, \
        "a cut before image of the large file was kept:\n" + txt


# @scenario SC-read-owner-23
def test_a_socket_in_the_scope_is_left_out_of_the_checkpoint_and_named(tmp_path):
    """A socket called s.c.  It is not a file: the pane names it and says so."""
    pane, after, txt, alive = _left_out_run(
        tmp_path, lambda d: _plant_socket(d, "s.c"), [LEFT_OUT, "s.c", "it is a socket"])
    _assert_named_and_reverted(pane, after, alive, "s.c", "it is a socket")


# @scenario SC-read-owner-24
def test_a_folder_called_like_a_source_file_is_not_a_file_of_the_scope(tmp_path):
    """A folder called d.c.  It is not listed as a file, so no before image of
    it (an empty one, read from a folder) is kept and nothing is left out."""
    pane, after, txt, alive = _left_out_run(
        tmp_path, lambda d: os.mkdir(d / "d.c"), [LEFT_OUT])
    assert LEFT_OUT not in pane, "the pane left out a folder as if it were a file:\n" + pane
    assert _images_kept(txt) == PLAIN_RUN_IMAGES, \
        "the folder d.c got a before image:\n" + txt
    assert after == ORIG, "the revert did not put t.c back: %r" % after
    assert alive, "the editor stopped answering"
