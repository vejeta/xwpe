"""A command too long for dbx is said and the start goes on, on X11.

dbx keeps the 255-byte command line it always had.  A breakpoint set on a
source whose name makes the command longer than that is not sent to it: the
start used to end there with nothing said, and dbx was never told to run.
The terminal row (tests/test_legacy_debuggers.py) reads the status row; this
one reads what dbx was given, since it is the same decision reached through
the graphical editor's start (a helper script reading a named pipe).  The
debugger is the stub tests/stubs/legacy/legacy-debugger under the name dbx.
The Wayland twin runs this same body.
"""
import atexit
import os
import shutil
import sys
import tempfile
import time

import pytest

from conftest import _sleep

STUB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "stubs",
                    "legacy", "legacy-debugger")
STUB_DIR = tempfile.mkdtemp(prefix="xwpe-dbx-stub-")
atexit.register(shutil.rmtree, STUB_DIR, True)
STUB_LOG = os.path.join(STUB_DIR, "dbx.log")
LONG_NAME = "d" * 250 + ".c"       # `stop at "<name>":8` is over 255 bytes
SOURCE = (
    "#include <stdio.h>\n"
    "int sq(int v)\n"
    "{\n"
    "  return v * v;\n"
    "}\n"
    "int main(void)\n"
    "{\n"
    "  int x = 3;\n"
    "  int y = sq(x);\n"
    '  printf("OUTMARK %d\\n", y);\n'
    "  return 0;\n"
    "}\n")
BREAK_LINE = 8
DBX_TYPE = 2               # the options file's number for dbx
RUN_WAIT_S = 40            # a build, the debugger's start and its first lines
SETTLE_S = 1.5


def _install_stub():
    with open(STUB) as fh:
        body = fh.read().split("\n", 1)[1]
    path = os.path.join(STUB_DIR, "dbx")
    with open(path, "w") as fh:
        fh.write("#!%s\n%s" % (sys.executable, body))
    os.chmod(path, 0o755)


_install_stub()
SESSION = {
    "XWPE_TEST_FILE": LONG_NAME,
    "XWPE_TEST_SOURCE": SOURCE,
    "PATH": STUB_DIR + ":" + os.environ.get("PATH", ""),
    "XWPE_STUB_LOG": STUB_LOG,
    "XWPE_STUB_RELEASE": os.path.join(STUB_DIR, "dbx.release"),
    "XWPE_STUB_SOURCE": LONG_NAME,
}


@pytest.fixture
def dbx_chosen(tmp_path):
    """The options file that makes dbx the debugger, written before the
    editor starts (the editor's home is tmp_path)."""
    cfg = tmp_path / ".config" / "xwpe"
    cfg.mkdir(parents=True)
    (cfg / "xwperc").write_text("[Programming]\nDebugger : %d\n" % DBX_TYPE)
    try:
        os.remove(STUB_LOG)
    except OSError:
        pass


def _commands():
    try:
        with open(STUB_LOG) as fh:
            return [l[4:].strip() for l in fh if l.startswith("CMD ")]
    except OSError:
        return []


def _wait(predicate, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        _sleep(0.2)
    return predicate()


# @scenario SC-debug-135
@pytest.mark.skipif(not shutil.which("gcc"), reason="gcc not on PATH")
@pytest.mark.parametrize("xwpe", [SESSION], indirect=True)
def test_dbx_is_run_after_a_breakpoint_too_long_to_send(dbx_chosen, xwpe):
    """Ctrl-G R with a breakpoint on line 8 of a source named 252 bytes:
    dbx is not sent the breakpoint (the command is over 255 bytes) and is
    told to run.  The start used to end after the refusal with dbx never
    told to run.

    Limit: the stub proves xwpe's half of the protocol, not a real dbx."""
    _sleep(SETTLE_S)
    for _ in range(BREAK_LINE - 1):
        xwpe.key("Down", delay=0.1)
    xwpe.key("ctrl+g", "b", delay=0.4)
    xwpe.key("ctrl+g", "r", delay=0.6)
    assert _wait(lambda: "run" in [c.split(" > ")[0] for c in _commands()],
                 RUN_WAIT_S), \
        "dbx was never told to run; it was sent %r" % (_commands(),)
    _sleep(SETTLE_S)
    assert not any(c.startswith("stop at ") for c in _commands()), \
        "the command that does not fit was sent: %r" % (_commands(),)
    assert xwpe.proc.poll() is None, "xwpe died on the refusal"
