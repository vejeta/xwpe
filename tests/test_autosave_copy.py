"""The copies the editor names itself (the autosave's .ASV, the emergency
save's .ESV) are made new and never written through a name somebody else
planted.

A link at t.c.ASV that leads to a program must not get the program's text
replaced and its setgid bit stripped by an autosave; the user is told on the
bottom row, once per interval, instead of in a box.  The emergency save
(SIGTERM) does the same: an old copy of ours is replaced by a new file, a
link is refused and named in the report printed when the editor is gone.
Closing a window removes its copies.

Run: tests/.venv/bin/python -m pytest -v tests/test_autosave_copy.py
"""
import os
import signal
import stat
import time

import pytest

pytest.importorskip("pyte")

from wpe_driver import WpeSession, WAIT_SCALE  # noqa: E402

CTRL_W = "\x17"                    # Close the active window
EXIT_DEADLINE_S = 10 * WAIT_SCALE  # the editor is gone this long after the signal
AUTOSAVE_WAIT_S = 6 * WAIT_SCALE   # the longest an autosave is waited for
WIDE = 150                        # wide enough for the whole status sentence
PROGRAM = "#!/bin/sh\necho program\n"
OLD_TEXT = "an old copy\n"
TYPED = "X"


def _options(tmp_path, lines):
    """The user's options file: the [General] lines @lines."""
    conf = tmp_path / ".config" / "xwpe"
    conf.mkdir(parents=True, exist_ok=True)
    (conf / "xwperc").write_text("[General]\n" + "\n".join(lines) + "\n")


def _autosave_session(tmp_path, seed="abc\n", filename="t.txt", cols=WIDE):
    """A session that autosaves after every two changes."""
    _options(tmp_path, ["Autosave : 2", "MaxChanges : 2"])
    return WpeSession(str(tmp_path), seed, filename=filename, cols=cols)


def _program(tmp_path):
    """A program somebody could be tricked into overwriting."""
    prog = tmp_path / "prog"
    prog.write_text(PROGRAM)
    prog.chmod(0o755)
    return prog


def _type_until_autosave(w, done=None):
    """Type until the autosave has run: @done(w) says it has (a loaded host
    can be slow), else a fixed wait."""
    for ch in "123":
        w.key(ch, delay=0.2)
    deadline = time.time() + AUTOSAVE_WAIT_S
    while True:
        w._drain(0.3)
        if time.time() >= deadline or (done is not None and done(w)):
            return


def _says(text):
    """A @done that waits for @text on the screen."""
    return lambda w: text in "\n".join(w.display(drain=0))


def _wait_for_exit(w, seconds):
    """The exit code, or None when the editor is still running."""
    deadline = time.time() + seconds
    while time.time() < deadline:
        code = w.proc.poll()
        if code is not None:
            return code
        w._drain(0.1)
    return None


def _end_by_sigterm(w):
    """SIGTERM to the exact process this row started, and wait for its end."""
    os.kill(w.proc.pid, signal.SIGTERM)
    code = _wait_for_exit(w, EXIT_DEADLINE_S)
    assert code is not None, "wpe still running %ds after SIGTERM" % EXIT_DEADLINE_S
    return bytes(w.raw)


# @scenario SC-chosen-names-08
def test_a_planted_autosave_link_is_refused_on_the_status_row(tmp_path):
    """The autosave never writes through a link at its name: the program
    keeps its text and its bit, and the bottom row says why."""
    prog = _program(tmp_path)
    os.symlink(str(prog), str(tmp_path / "t.txt.ASV"))
    with _autosave_session(tmp_path) as w:
        _type_until_autosave(w, _says("is in its place"))
        screen = "\n".join(w.display())
        assert "is in its place" in screen, screen
        assert w.alive(), "wpe died"
    assert prog.read_text() == PROGRAM, "the autosave wrote through the link"
    assert (prog.stat().st_mode & 0o7777) == 0o755
    assert os.path.islink(str(tmp_path / "t.txt.ASV"))


# @scenario SC-chosen-names-08
def test_the_emergency_copy_is_made_new(tmp_path):
    """An old .ESV of ours is replaced by a new file (a hard link made to the
    old one keeps the old text), and the report names the file written."""
    esv = tmp_path / "t.c.ESV"
    esv.write_text(OLD_TEXT)
    os.link(str(esv), str(tmp_path / "old.keep"))
    with WpeSession(str(tmp_path), "") as w:
        w.key(TYPED, delay=0.6)
        raw = _end_by_sigterm(w)
    assert TYPED in esv.read_text(), esv.read_text()
    assert (tmp_path / "old.keep").read_text() == OLD_TEXT, "the old copy was rewritten in place"
    assert b"t.c.ESV saved!" in raw, raw[-300:]


# @scenario SC-chosen-names-08
def test_the_emergency_copy_refuses_a_planted_link(tmp_path):
    """A link at t.c.ESV is refused: the program keeps its text and the
    report says the .ESV could not be saved."""
    prog = _program(tmp_path)
    os.symlink(str(prog), str(tmp_path / "t.c.ESV"))
    with WpeSession(str(tmp_path), "") as w:
        w.key(TYPED, delay=0.6)
        raw = _end_by_sigterm(w)
    assert prog.read_text() == PROGRAM, "the emergency save wrote through the link"
    assert (prog.stat().st_mode & 0o7777) == 0o755
    assert os.path.islink(str(tmp_path / "t.c.ESV"))
    assert b"t.c.ESV could not be saved!" in raw, raw[-300:]


# @scenario SC-chosen-names-08
def test_a_fifo_at_the_emergency_name_is_refused_and_not_waited_on(tmp_path):
    """A pipe nobody reads at t.c.ESV used to hold the exit in open() for
    ever.  The name is never opened existing now: it is refused at once, the
    editor ends on the one SIGTERM, and the report says it was not saved."""
    os.mkfifo(str(tmp_path / "t.c.ESV"))
    with WpeSession(str(tmp_path), "") as w:
        w.key(TYPED, delay=0.6)
        raw = _end_by_sigterm(w)
    assert b"t.c.ESV could not be saved!" in raw, raw[-300:]
    assert stat.S_ISFIFO(os.stat(str(tmp_path / "t.c.ESV")).st_mode)


# @scenario SC-chosen-names-08
def test_closing_the_window_removes_its_copy(tmp_path):
    """Closing a window leaves no .ASV of it beside the file (the removal
    used to be dead code)."""
    with _autosave_session(tmp_path) as w:
        asv = tmp_path / "t.txt.ASV"
        _type_until_autosave(w, lambda _w: asv.exists())
        assert asv.exists(), sorted(os.listdir(str(tmp_path)))
        w.key(CTRL_W, delay=0.8)
        w.key("n", delay=0.8)
        w._drain(0.5)
        assert not asv.exists(), "the closed window's copy is still there"


OLD_SESSION_TEXT = "work from an earlier session\n"
ALT_X = "\033x"                    # Quit


def _an_earlier_sessions_esv(tmp_path):
    """The .ESV a crashed session left, which "wpe -r" would bring back."""
    esv = tmp_path / "t.c.ESV"
    esv.write_text(OLD_SESSION_TEXT)
    return esv


# @scenario SC-chosen-names-08
def test_an_earlier_sessions_esv_survives_closing_the_window(tmp_path):
    """The window did not write that .ESV, so closing it must not delete it."""
    esv = _an_earlier_sessions_esv(tmp_path)
    with WpeSession(str(tmp_path), "int x;\n") as w:
        w.key(CTRL_W, delay=0.8)
        w._drain(0.5)
        assert esv.exists(), "closing the window deleted a copy it never wrote"
    assert esv.read_text() == OLD_SESSION_TEXT


# @scenario SC-chosen-names-08
def test_an_earlier_sessions_esv_survives_quitting(tmp_path):
    esv = _an_earlier_sessions_esv(tmp_path)
    with WpeSession(str(tmp_path), "int x;\n") as w:
        w.key(ALT_X, delay=1.0)
        _wait_for_exit(w, EXIT_DEADLINE_S)
    assert esv.exists(), "quitting deleted a copy the window never wrote"
    assert esv.read_text() == OLD_SESSION_TEXT


# @scenario SC-chosen-names-08
def test_closing_removes_the_copy_in_the_windows_own_folder(tmp_path):
    """A file opened from a subfolder: its .ASV goes, and a file of the same
    name in the working folder stays (the removal is by the window's folder,
    never the working folder)."""
    (tmp_path / "sub").mkdir()
    elsewhere = tmp_path / "t.txt.ASV"
    elsewhere.write_text("not this window's\n")
    with _autosave_session(tmp_path, filename="sub/t.txt") as w:
        asv = tmp_path / "sub" / "t.txt.ASV"
        _type_until_autosave(w, lambda _w: asv.exists())
        assert asv.exists(), sorted(os.listdir(str(tmp_path / "sub")))
        w.key(CTRL_W, delay=0.8)
        w.key("n", delay=0.8)
        w._drain(0.5)
        assert not asv.exists(), "the closed window's copy in its folder is still there"
    assert elsewhere.read_text() == "not this window's\n"


# @scenario SC-chosen-names-08
def test_the_autosave_sentence_leads_with_the_name_at_80_columns(tmp_path):
    """On an 80-column row the user reads that it was the autosave and which
    file, not only why."""
    prog = _program(tmp_path)
    os.symlink(str(prog), str(tmp_path / "t.txt.ASV"))
    with _autosave_session(tmp_path, cols=80) as w:
        _type_until_autosave(w, _says("Autosave: t.txt.ASV not written"))
        screen = "\n".join(w.display())
        assert "Autosave: t.txt.ASV not written" in screen, screen
    assert prog.read_text() == PROGRAM


RECOVERED_TEXT = "int recovered;\n"


def _recovery_setup(tmp_path):
    """t.c and the .ESV an earlier session left for it."""
    (tmp_path / "t.c").write_text("int x;\n")
    esv = tmp_path / "t.c.ESV"
    esv.write_text(RECOVERED_TEXT)
    return esv


def _recovered_session(tmp_path):
    """`wpe -r` with other.txt on the command line: the recovery opens t.c
    from its .ESV after other.txt."""
    return WpeSession(str(tmp_path), "other\n", filename="other.txt", args=("-r",))


# @scenario SC-chosen-names-08
def test_closing_the_recovered_window_removes_the_esv_it_read(tmp_path):
    """The window reopened from the .ESV is the one that read it, so closing
    it without saving removes that .ESV and leaves t.c alone."""
    esv = _recovery_setup(tmp_path)
    with _recovered_session(tmp_path) as w:
        assert "t.c" in "\n".join(w.display()), "t.c was not recovered"
        w.key(CTRL_W, delay=0.8)     # other.txt, the top window
        w.key(CTRL_W, delay=0.8)     # the recovered t.c
        w.key("n", delay=0.8)
        w._drain(0.5)
        assert not esv.exists(), "the recovered window's .ESV is still there"
        assert (tmp_path / "t.c").read_text() == "int x;\n"


# @scenario SC-chosen-names-08
def test_a_different_file_at_the_esv_name_survives_the_recovered_close(tmp_path):
    """Another file took the .ESV name while the recovered window was open:
    closing the window must not delete it."""
    esv = _recovery_setup(tmp_path)
    with _recovered_session(tmp_path) as w:
        w.key(CTRL_W, delay=0.8)     # other.txt
        os.rename(str(esv), str(tmp_path / "t.c.ESV.aside"))
        esv.write_text("a different file\n")
        w.key(CTRL_W, delay=0.8)     # the recovered t.c
        w.key("n", delay=0.8)
        w._drain(0.5)
        assert esv.exists(), "a different file at the .ESV name was deleted"
        assert esv.read_text() == "a different file\n"
