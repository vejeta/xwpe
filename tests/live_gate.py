"""live_gate.py -- a live test file is not even imported unless the run opted in.

A file named test_*_live*.py talks to a real engine with the user's login.
Importing one is already a risk (live_ai asks the local Ollama for its models
at import), so the gate is at collection: the hook below, installed by
tests/conftest.py, returns a stand-in file for every live file when the
environment of the test run does not have XWPE_TEST_LIVE=1.  The stand-in
yields one skipping item and never imports the module.

It is a collection hook, not ignore_collect or --ignore-glob, because both
are bypassed for a path given on the command line, and that is exactly how a
live file was run by mistake.
"""
import os

import pytest

import engine_guard

GATED_ITEM_NAME = "live file gated"
GATED_REASON = ("a live file reaches real engines with the user's login: "
                "set %s=%s in the environment of the test run to run it"
                % (engine_guard.LIVE_GATE_VAR, engine_guard.LIVE_GATE_ON))


class GatedLiveItem(pytest.Item):
    """The one item a gated live file yields: it skips, naming the gate."""

    def runtest(self):
        """Skip, saying how to open the gate."""
        pytest.skip(GATED_REASON)

    def reportinfo(self):
        """Where the item is reported: the live file itself."""
        return self.path, 0, self.name


class GatedLiveFile(pytest.File):
    """A live file that was not imported."""

    def collect(self):
        """One skipping item; the module is never imported."""
        yield GatedLiveItem.from_parent(self, name=GATED_ITEM_NAME)


def require_live_gate():
    """Skip the whole module being imported unless the run opened the gate.

    Every live file calls this as its first statement after its imports (and
    tests/live_ai.py at its top): a second gate that does not depend on
    conftest.py, so a run that drops the conftest (--noconftest, a
    --confcutdir above tests/) still never runs a live row or lets the module
    reach a real engine at import.  check_test_real_tools.py fails a live file
    that does not call it."""
    if not engine_guard.live_gate_open(os.environ):
        pytest.skip(GATED_REASON, allow_module_level=True)


@pytest.hookimpl(tryfirst=True)
def pytest_pycollect_makemodule(module_path, parent):
    """Return the stand-in for a live file while the gate is closed, so no
    code of it runs at collection or in a row.  Returning a node stops the
    hook (it is firstresult); None leaves every other file to pytest."""
    if not engine_guard.is_live_file(module_path):
        return None
    if engine_guard.live_gate_open(os.environ):
        return None
    return GatedLiveFile.from_parent(parent, path=module_path)
