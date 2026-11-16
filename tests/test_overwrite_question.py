"""The question asked before Save As or Write Block replaces a file that
exists (we_fl_unix.c), and what it says about a setuid, setgid or sticky bit.

A copy never gets these bits, so overwriting a setgid program with a copy
turns it into an ordinary file.  The question names the bits before the
answer, not after.  Save As onto the window's own file is a save, which
keeps the bits, so there is nothing to say; and a file with no such bit has
nothing to say either.  No and Esc write nothing.

The editor is started as `wpe` (WPE_BIN), the name that keeps the whole
programming layer on.

Run: WPE_BIN=$PWD/wpe tests/.venv/bin/python -m pytest -q tests/test_overwrite_question.py
"""
import os
import stat
import time

import pytest

pytest.importorskip("pyte")

from mode_premise import special_bits_settable, workdir_in_my_group   # noqa: E402
from wpe_driver import ALT, WpeSession                                # noqa: E402

ESC = "\033"
SEED = "alpha\n"
OLD = "old text\n"
QUESTION = "already exists. Overwrite it?"
BIT_LINE = "It has the setgid bit; the copy will not keep that bit."
SETTLE_S = 0.5


def _setup(tmp_path):
    """A work folder in this user's group where setgid can be kept, and a
    state folder outside it."""
    work = str(tmp_path / "work")
    state = str(tmp_path / "state")
    os.mkdir(work)
    os.mkdir(state)
    workdir_in_my_group(work)
    special_bits_settable(work, stat.S_ISGID)
    return work, {"XDG_STATE_HOME": state}


def _put(work, name, text, mode):
    path = os.path.join(work, name)
    with open(path, "w") as fh:
        fh.write(text)
    os.chmod(path, mode)
    return path


def _special(path):
    """The setuid, setgid and sticky bits of a file.  The nine permission
    bits of a write are the window's own, which these rows do not judge."""
    return stat.S_IMODE(os.stat(path).st_mode) & 0o7000


def _read(path):
    with open(path) as fh:
        return fh.read()


def _screen(s):
    return "\n".join(s.display())


def _save_as(s, name):
    """File > Save As, the name typed, Alt-S: the question is on screen."""
    s.key("X")
    s.menu(ALT.FILE, "a")
    s.key(name, "\033s")
    time.sleep(SETTLE_S)


def _write_block(s, name):
    """Block > Mark whole, Block > Write, the name typed, Alt-W."""
    s.menu(ALT.BLOCK, "o")
    s.menu(ALT.BLOCK, "w")
    s.key(name, "\033w")
    time.sleep(SETTLE_S)


# @scenario SC-unsaved-text-71
def test_save_as_over_a_setgid_file_names_the_bit(tmp_path):
    """Save As over another file that is setgid: the question says so."""
    work, env = _setup(tmp_path)
    target = _put(work, "x.c", OLD, 0o2755)
    with WpeSession(work, SEED, env_extra=env) as s:
        _save_as(s, "x.c")
        shown = _screen(s)
        assert QUESTION in shown, shown
        assert BIT_LINE in shown, shown
        s.key("y")
        time.sleep(SETTLE_S)
    assert _read(target) == "Xalpha\n"
    assert _special(target) == 0, "the line promised the copy would not keep the bit"


# @scenario SC-unsaved-text-71
def test_save_as_over_a_plain_file_has_no_bit_line(tmp_path):
    """Control: a target with no special bit gets the question and no line."""
    work, env = _setup(tmp_path)
    target = _put(work, "x.c", OLD, 0o644)
    with WpeSession(work, SEED, env_extra=env) as s:
        _save_as(s, "x.c")
        shown = _screen(s)
        assert QUESTION in shown, shown
        assert "setgid" not in shown and "will not keep" not in shown, shown
        s.key("y")
        time.sleep(SETTLE_S)
    assert _read(target) == "Xalpha\n"
    assert _special(target) == 0


# @scenario SC-unsaved-text-71
def test_save_as_onto_the_windows_own_setgid_file_has_no_bit_line(tmp_path):
    """Save As onto the window's own file is a save: the bit is kept, so the
    question must not say it will be lost."""
    work, env = _setup(tmp_path)
    with WpeSession(work, SEED, env_extra=env) as s:
        own = os.path.join(work, "t.c")
        os.chmod(own, 0o2755)
        _save_as(s, "t.c")
        shown = _screen(s)
        assert QUESTION in shown, shown
        assert "setgid" not in shown and "will not keep" not in shown, shown
        s.key("y")
        time.sleep(SETTLE_S)
    assert _read(own) == "Xalpha\n"
    assert _special(own) == stat.S_ISGID, "a save keeps the bit"


# @scenario SC-unsaved-text-71
def test_write_block_over_a_setgid_file_names_the_bit(tmp_path):
    """Write Block is always a copy: the question names the bit, and the file
    ends without it, as the line said."""
    work, env = _setup(tmp_path)
    target = _put(work, "x.c", OLD, 0o2755)
    with WpeSession(work, SEED, env_extra=env) as s:
        _write_block(s, "x.c")
        shown = _screen(s)
        assert QUESTION in shown, shown
        assert BIT_LINE in shown, shown
        s.key("y")
        time.sleep(SETTLE_S)
    assert _read(target) == SEED.rstrip("\n")
    assert _special(target) == 0


# @scenario SC-unsaved-text-71
def test_the_ai_pane_save_as_over_a_setgid_file_names_the_bit(tmp_path):
    """The assistant pane's transcript is written as a new file, never as a
    backed-up save, so over a setgid file it is a copy: the question names
    the bit, and the file ends without it, as the line said."""
    work, env = _setup(tmp_path)
    env.update({"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
                "XWPE_AI_MOCK_REPLY": "REPLYONE"})
    target = _put(work, "x.txt", OLD, 0o2755)
    with WpeSession(work, SEED, env_extra=env) as s:
        s.key(ALT.AI)
        s.key("a", delay=0.8)
        s.key("hello", delay=0.3)
        s.key("\r", delay=1.5)
        s.key(ALT.FILE, delay=0.6)
        s.key("a", delay=1.2)
        s.key("\033n", delay=0.4)
        s.key("x.txt", delay=0.4)
        s.key("\r", delay=1.0)
        shown = _screen(s)
        assert QUESTION in shown, shown
        assert BIT_LINE in shown, shown
        s.key("y")
        time.sleep(SETTLE_S)
    assert "REPLYONE" in _read(target)
    assert _special(target) == 0, "the line promised the copy would not keep the bit"


# @scenario SC-unsaved-text-71
@pytest.mark.parametrize("answer", ["n", ESC], ids=["No", "Esc"])
def test_no_and_esc_write_nothing(tmp_path, answer):
    """No or Esc leaves the target as it was: its text and its bit."""
    work, env = _setup(tmp_path)
    target = _put(work, "x.c", OLD, 0o2755)
    with WpeSession(work, SEED, env_extra=env) as s:
        _save_as(s, "x.c")
        assert BIT_LINE in _screen(s)
        s.key(answer)
        time.sleep(SETTLE_S)
        assert s.alive()
    assert _read(target) == OLD
    assert stat.S_IMODE(os.stat(target).st_mode) == 0o2755
