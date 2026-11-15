"""Settings every test session shares.

A test starts the editor in an empty HOME, and an empty HOME is a new profile,
which the editor asks which keys it wants.  Nobody answers there, so a session
runs the Borland keys unless a row says otherwise: XWPE_KEYS=borland is in the
environment of every editor a test starts.  The two drivers (wpe_driver.py,
gui_owner.py) also set it themselves; this covers the tests that start the
editor on their own.  A row that asks the first-run question, or runs another
style, removes or changes the variable in its own environment (test_first_run.py,
test_keys_session.py).
"""
import os

import pytest

import build_config
import gui_owner

os.environ.setdefault("XWPE_KEYS", "borland")


def pytest_configure(config):
    config.addinivalue_line(
        "markers",
        "wpe_requires(*words): skip, by name, unless the binary under test was "
        "built with every word (tests/build_config.py, requires())")


@pytest.hookimpl(tryfirst=True)
def pytest_runtest_setup(item):
    """Resolve requires() when the row is about to run, never at collection.

    The binary is asked once (`--version`).  A row on a build without what it
    needs skips as "not built here: --enable-ai"; a binary that cannot say
    what it has is an error of the row, not a skip.  This runs before any
    fixture, so nothing is started for a row that cannot run.
    """
    for mark in item.iter_markers("wpe_requires"):
        try:
            lacking = build_config.missing(mark.args, mark.kwargs.get("binary"))
        except build_config.BuildUnknown as err:
            pytest.fail(str(err), pytrace=False)
        if lacking:
            pytest.skip(build_config.skip_reason(lacking))


# The two fixtures below are the process owner of every test (tests/gui_owner.py).
# They are written here and NOT installed: they are not autouse, because
# turning them on makes pytest the child subreaper for every terminal row,
# and the rows that ask kill(pid, 0) whether a process is gone are moved onto
# gui_owner.process_running first (the terminal suite's pass).  That pass adds
# autouse=True to both.

@pytest.fixture(scope="session")
def process_owner():
    """The one owner of every process a test starts, for the whole session.

    It ends whatever a row left, and errors the session, naming each
    process, on a leak no row reported.  Under xdist each worker is a
    process with its own owner; a worker that dies takes its children along
    by their death signal."""
    owner = gui_owner.Owner(gui_owner.OUT_DIR)
    gui_owner.install(owner)
    owner.begin()
    yield owner
    gui_owner.install(None)
    late = owner.finish()
    if late:
        raise gui_owner.LeakedProcesses(
            "left at the session's end, now ended:\n  %s"
            % "\n  ".join(leak.describe() for leak in late))


@pytest.fixture
def end_row_processes(process_owner):
    """End, when the row ends, every process the row started that its own
    fixtures did not end (a driver built in the test body), and error the
    row naming what it left."""
    before = process_owner.ledger.pids()
    yield
    process_owner.end_since(before)
