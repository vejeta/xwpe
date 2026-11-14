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
