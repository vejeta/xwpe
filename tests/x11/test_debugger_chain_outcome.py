"""A debugger that stands at another prompt before the editor's first command
is said at the start, on X11.

pdb reads ~/.pdbrc itself.  With `interact` in it, pdb stops at its >>>
prompt before the editor writes a word, and the start used to wait out its
whole 30 second deadline.  Now the start's heartbeat sees the banner standing
at a prompt of the debugger's own table and says which one.  The terminal row
(tests/test_debugger_chain_outcome.py) reads Messages; this one reads the
trace line the start writes for the same sentence, since it is the same
decision reached through the graphical editor's start.  The editor's home is
the folder of the program, so ./.pdbrc is ~/.pdbrc and nothing is asked.
The Wayland twin runs this same body.
"""
import atexit
import os
import shutil
import tempfile
import time

import pytest

from conftest import _sleep

TRACE_DIR = tempfile.mkdtemp(prefix="xwpe-chainout-")
atexit.register(shutil.rmtree, TRACE_DIR, True)
TRACE = os.path.join(TRACE_DIR, "ui.trace")
SESSION = {
    "XWPE_TEST_FILE": "t.py",
    "XWPE_TEST_SOURCE": "x = 1\nprint(x)\n",
    "XWPE_UI_TRACE": TRACE,
    "XWPE_DEBUG_START_TIMEOUT": "20",
}
PROMPT_WAIT_S = 12         # a quarter second after the banner, far inside this


def _said():
    try:
        with open(TRACE) as fh:
            return [l.split("start said ", 1)[1].strip() for l in fh if l.startswith("start said ")]
    except OSError:
        return []


def _wait(predicate, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        _sleep(0.2)
    return predicate()


@pytest.fixture
def home_pdbrc(tmp_path):
    """A ~/.pdbrc (the editor's home is tmp_path) that leaves pdb at its >>>
    prompt, written before the editor starts."""
    (tmp_path / ".pdbrc").write_text("interact\n")
    try:
        os.remove(TRACE)
    except OSError:
        pass


# @scenario SC-debug-138
@pytest.mark.skipif(not shutil.which("python3"), reason="python3 not on PATH")
@pytest.mark.parametrize("xwpe", [SESSION], indirect=True)
def test_pdb_left_at_another_prompt_by_the_home_pdbrc_is_said(home_pdbrc, xwpe):
    """Ctrl-G R with an interact in ~/.pdbrc: the start says pdb answered with
    its >>> prompt, not its own, long before its deadline."""
    _sleep(1.0)
    xwpe.key("ctrl+g", "r", delay=0.6)
    assert _wait(lambda: len(_said()) >= 2, PROMPT_WAIT_S), \
        "the start said %r" % (_said(),)
    said = _said()
    assert said[0] == "pdb answered its start with its >>> prompt, not its own", said
    assert said[1] == "A start-up file it reads itself left it there", said
    assert xwpe.proc.poll() is None, "xwpe died on the stuck banner"
