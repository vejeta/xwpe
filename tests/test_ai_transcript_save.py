"""The AI pane's transcript is written by the write owner (we_write.c).

Save As on the pane writes the transcript into the file the user names.  The
file is written as the editor writes any file the user names: over a file that
has a setuid, setgid or sticky bit the copy does not keep that bit, and the
question before the write says so (we_fl_unix.c).  Before this the transcript
was opened with fopen "wb", which leaves a sticky bit on the file, so the
question promised something the write did not do.

Not covered here, on purpose: the status row sentence for a bit that STAYS
("... written, but it keeps the sticky bit: ...") needs a file the user does
not own or a refused chmod, which a pyte session cannot make.  The C rows
test_a_text_over_a_file_whose_bit_cannot_be_dropped_says_it_stays and
test_a_text_written_over_a_setgid_file_of_ours_drops_the_bit in
tests/test_write_owner.c guard the stream face the transcript goes through.

The editor is started as `wpe` (WPE_BIN) with the mock backend: nothing here
reaches a live provider.

Run: WPE_BIN=$PWD/wpe tests/.venv/bin/python -m pytest -q tests/test_ai_transcript_save.py
"""
import os
import stat
import subprocess

import pytest

pytest.importorskip("pyte")

from mode_premise import special_bits_settable, workdir_in_my_group   # noqa: E402
from wpe_driver import ALT, WPE_BIN, WpeSession                       # noqa: E402

SEED = "int main(void){return 0;}\n"
F2 = "\033OQ"
ALT_NAME = "\033n"                      # the Save As dialog's Name field
QUESTION = "already exists. Overwrite it?"
BIT_LINE = "It has the sticky bit; the copy will not keep that bit."
REPLY = "REPLYONE"


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"autonomous coding agent" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _env(tmp_path):
    return {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": REPLY,
        "XWPE_AI_TRACE": str(tmp_path / "ai.trace"),
        "XDG_STATE_HOME": str(tmp_path / "state"),
    }


def _work(tmp_path):
    work = tmp_path / "work"
    work.mkdir()
    (tmp_path / "state").mkdir()
    workdir_in_my_group(str(work))
    special_bits_settable(str(work), stat.S_ISVTX)
    return str(work)


def _put(work, name, text, mode):
    path = os.path.join(work, name)
    with open(path, "w") as fh:
        fh.write(text)
    os.chmod(path, mode)
    return path


def _special(path):
    return stat.S_IMODE(os.stat(path).st_mode) & 0o7000


def _read(path):
    with open(path) as fh:
        return fh.read()


def _screen(s):
    return "\n".join(s.display())


def _open_chat_with_turn(s):
    s.key(ALT.AI)
    s.key("a", delay=0.8)
    s.key("hello", delay=0.3)
    s.key("\r", delay=1.5)


def _save_as_name(s, name):
    """F2 in the pane, the name typed into the dialog, Enter: what follows is
    on screen when this returns."""
    s.key(F2, delay=1.0)
    s.key(ALT_NAME, delay=0.4)
    s.key(name, delay=0.4)
    s.key("\r", delay=1.0)


# @scenario SC-ai-chat-61
def test_save_as_of_the_transcript_over_a_sticky_file_drops_the_bit(tmp_path):
    """Save As of the transcript over a 01644 file of ours: the question names
    the bit, and after Yes the file holds the transcript with mode 0644.

    Red on the fopen "wb" writer: the sticky bit survives the write.
    """
    work = _work(tmp_path)
    target = _put(work, "pane.txt", "old\n", 0o1644)
    assert _special(target) == stat.S_ISVTX, "the setup could not set the sticky bit"
    with WpeSession(work, SEED, env_extra=_env(tmp_path)) as s:
        _open_chat_with_turn(s)
        _save_as_name(s, "pane.txt")
        asking = _screen(s)
        s.key("y", delay=1.2)
        after = s.display()
    assert QUESTION in asking, "no overwrite question:\n" + asking
    assert BIT_LINE in asking, "the question does not name the bit:\n" + asking
    assert "AI: " + REPLY in _read(target), "the transcript was not written: %r" % _read(target)
    assert _special(target) == 0, \
        "the question said the copy would not keep the bit; mode is %o" % stat.S_IMODE(
            os.stat(target).st_mode)
    assert "saved pane.txt" in after[-1], "the status row does not say the save: %r" % after[-1]


# @scenario SC-ai-chat-61
def test_save_as_of_the_transcript_over_a_plain_file_says_saved(tmp_path):
    """Control: a file with no special bit gets the question without a bit
    line, the transcript, and the plain "saved" row."""
    work = _work(tmp_path)
    target = _put(work, "pane.txt", "old\n", 0o644)
    with WpeSession(work, SEED, env_extra=_env(tmp_path)) as s:
        _open_chat_with_turn(s)
        _save_as_name(s, "pane.txt")
        asking = _screen(s)
        s.key("y", delay=1.2)
        after = s.display()
    assert QUESTION in asking, asking
    assert "sticky" not in asking and "will not keep" not in asking, asking
    assert "AI: " + REPLY in _read(target)
    assert _special(target) == 0
    assert "saved pane.txt" in after[-1], after[-1]



# @scenario SC-ai-chat-61
@pytest.mark.skipif(os.geteuid() == 0, reason="root can write a 0444 file")
def test_a_second_f2_onto_a_file_made_read_only_names_the_reason(tmp_path):
    """The first F2 saves; the file is then made 0444 and the second F2 cannot
    write it: the status row says "could not save pane.txt: Permission
    denied", never "saved", and the file keeps the first transcript."""
    work = _work(tmp_path)
    target = os.path.join(work, "pane.txt")
    with WpeSession(work, SEED, env_extra=_env(tmp_path)) as s:
        _open_chat_with_turn(s)
        _save_as_name(s, "pane.txt")
        first = _read(target)
        os.chmod(target, 0o444)
        s.key(F2, delay=1.2)
        after = s.display()
    assert "AI: " + REPLY in first, "the first save wrote nothing: %r" % first
    assert "could not save pane.txt: Permission denied" in after[-1], \
        "the failure is not named on the status row:\n" + "\n".join(after)
    assert "saved pane.txt" not in after[-1], after[-1]
    assert _read(target) == first, "the read-only file was changed"
