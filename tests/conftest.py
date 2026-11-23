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
import json
import os
import sys

import pytest

import build_config
import gui_owner
import host_tools
# A live file is not imported unless the run opened the gate (live_gate.py):
# pytest finds the hook of a conftest by its name.
from live_gate import pytest_pycollect_makemodule  # noqa: F401

HARNESS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "harness")
sys.path.insert(0, HARNESS_DIR)
try:
    import suite_plan
finally:
    sys.path.pop(0)

os.environ.setdefault("XWPE_KEYS", "borland")


_RECORD = []


def timing_record():
    """The record of how long each row took (tests/harness/suite_plan.py),
    read once per process."""
    if not _RECORD:
        _RECORD.append(suite_plan.load_record(suite_plan.default_record_path()))
    return _RECORD[0]


def row_budget_s(nodeid):
    """The seconds the row clock gives @nodeid: XWPE_ROW_BUDGET_S when the
    run names one, else a few times the row's recorded time."""
    fixed = os.environ.get(gui_owner.ROW_BUDGET_VAR)
    if fixed:
        return float(fixed)
    return suite_plan.row_budget_s(timing_record(), nodeid, gui_owner.wait_scale())


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_call(item):
    """Run the row under its clock: a row that hangs ends as RowOverBudget
    with its name and budget, and its editor ends through the owner's ladder.
    Setup and teardown are never cut."""
    gui_owner.ROW_CLOCK.arm(item.nodeid, row_budget_s(item.nodeid))
    try:
        yield
    finally:
        gui_owner.ROW_CLOCK.disarm()


def make_basetemp_parent(config):
    """Make the folder above the --basetemp a job named, when it is missing.

    A job that asks for `--basetemp wl-logs/gate` in a checkout with no
    `wl-logs` gets its run started instead of an internal error at the first
    line of the session: pytest makes the basetemp folder and never its
    parents.  Under xdist the controller makes it here, before the workers
    make theirs below it.
    """
    given = config.option.basetemp
    if given:
        parent = os.path.dirname(os.path.abspath(str(given)))
        os.makedirs(parent, exist_ok=True)


def pytest_sessionstart(session):
    """Put the owner's tool farm under the session's pytest basetemp: it goes
    with the session and is outside the working folder of every row."""
    make_basetemp_parent(session.config)
    factory = getattr(session.config, "_tmp_path_factory", None)
    if factory is not None:
        gui_owner.current().use_farm_base(str(factory.getbasetemp()))


def is_known_gap_hidden_failure(report, call):
    """True when a report reads as a known gap (an xfail) but what failed was
    the test owner or the row clock: that is never the known gap.

    A recorded incoherence (an xfail whose reason starts with
    INCOHERENCE_PREFIX) hides nothing the owner found (a timeout, a crash, a
    leak).  Any xfail hides nothing of a row the clock cut: a hung row is
    never a recorded gap."""
    reason = getattr(report, "wasxfail", None)
    if reason is None or call.excinfo is None:
        return False
    if isinstance(call.excinfo.value, gui_owner.RowOverBudget):
        return True
    if not isinstance(reason, str) or not reason.startswith(gui_owner.INCOHERENCE_PREFIX):
        return False
    return isinstance(call.excinfo.value, gui_owner.OwnerFailure)


@pytest.hookimpl(hookwrapper=True, tryfirst=True)
def pytest_runtest_makereport(item, call):
    """Make a failure of the owner in any phase (the row, its teardown) fail
    the run even when the row is marked as an incoherence.

    The owner's failures are assertion errors, which an xfail that names
    AssertionError would take for the known gap: an editor that crashed
    would read KNOWN and the gate would go green.  Only a failed assertion
    of the row itself is the recorded gap."""
    outcome = yield
    report = outcome.get_result()
    if is_known_gap_hidden_failure(report, call):
        report.outcome = "failed"
        report.longrepr = item.repr_failure(call.excinfo)
        del report.wasxfail
        tell_rerunfailures_the_row_failed(item, report, call)


def tell_rerunfailures_the_row_failed(item, report, call):
    """Make the rerun plugin judge the corrected report, not the xfail it read.

    The plugin's own makereport wrapper sits inside this one: it reads the
    report while it still says "xfailed", records that no failure of the row
    is one to stop at (--rerun-except), and so a row the clock cut would be
    run twice more.  Its two records are put right here, from the report as
    corrected, so --rerun-except RowOverBudget ends the row at the cut.  The
    plugin is pinned (tests/ci-versions.txt); without it nothing is to be told."""
    terminal = getattr(item, "_terminal_errors", None)
    xfailed = getattr(item, "_test_xfailed", None)
    if terminal is None or xfailed is None:
        return
    import pytest_rerunfailures
    terminal[report.when] = pytest_rerunfailures._should_hard_fail_on_error(
        item, report, call.excinfo)
    xfailed[report.when] = False


def pytest_configure(config):
    config.addinivalue_line(
        "markers",
        "wpe_requires(*words): skip, by name, unless the binary under test was "
        "built with every word (tests/build_config.py, requires())")
    config.addinivalue_line(
        "markers",
        "wpe_needs(*tools): skip, naming the tool, unless every host tool is "
        "usable and not switched off by this job (tests/host_tools.py, needs())")


NEEDS_DUMP_VARIABLE = "XWPE_NEEDS_DUMP"


def needs_of(item):
    """The host tools a collected row declares with needs(), in the order
    written ("a|b" stays one entry, an alternative)."""
    tools = []
    for mark in item.iter_markers("wpe_needs"):
        tools.extend(mark.args)
    return tools


def pytest_collection_finish(session):
    """When asked (tests/harness/harness.py check), write the host tools every
    collected row declares, as {node id: [tool, ...]}, to the file named by
    XWPE_NEEDS_DUMP.  The harness compares them with the needs its manifests
    declare for the row, so a row that needs a tool its scenario does not
    name is found at collection, not at the first run on a host without it."""
    path = os.environ.get(NEEDS_DUMP_VARIABLE)
    if not path:
        return
    declared = {item.nodeid: needs_of(item) for item in session.items}
    with open(path, "w", encoding="utf-8") as fh:
        json.dump({node: tools for node, tools in declared.items() if tools}, fh)


def pytest_report_header(config):
    """Say, at the top of the log, which host tools this job switched off."""
    line = host_tools.report_line()
    return [line] if line else []


@pytest.hookimpl(tryfirst=True)
def pytest_runtest_setup(item):
    """Resolve requires() and needs() when the row is about to run, never at
    collection.

    requires(): the binary is asked once (`--version`).  A row on a build
    without what it needs skips as "not built here: --enable-ai"; a binary
    that cannot say what it has is an error of the row, not a skip.
    needs(): a row whose host tool is missing, does not run, or is switched
    off by this job skips naming the tool.  This is the only place a
    declaration becomes a skip.  It runs before any fixture, so nothing is
    started for a row that cannot run.
    """
    # The owner learns which row and file run before any fixture starts a
    # process: the tool farm's live opt-out is allowed by the file's name.
    gui_owner.current().begin_row(item.nodeid, item.path)
    for mark in item.iter_markers("wpe_requires"):
        try:
            lacking = build_config.missing(mark.args, mark.kwargs.get("binary"))
        except build_config.BuildUnknown as err:
            pytest.fail(str(err), pytrace=False)
        if lacking:
            pytest.skip(build_config.skip_reason(lacking))
    for mark in item.iter_markers("wpe_needs"):
        reason = host_tools.unmet(mark.args)
        if reason:
            pytest.skip(reason)


# The two fixtures below are the process owner of every test (tests/gui_owner.py).
# They are autouse: pytest is the child subreaper for the whole session, so an
# orphan of an editor comes back here and is found and ended, and a row asks
# gui_owner.process_running, never kill(pid, 0), whether a process is gone (a
# zombie counts as ended there).

@pytest.fixture(scope="session", autouse=True)
def process_owner():
    """The one owner of every process a test starts, for the whole session.

    It ends whatever a row left, and errors the session, naming each
    process, on a leak no row reported.  Under xdist each worker is a
    process with its own owner; a worker that dies takes its children along
    by their death signal."""
    # current(): the owner is begun and installed, by this fixture or, when a
    # hook asked the editor something before the first row (build_config), by
    # that call; either way it is the one owner and is finished here.
    owner = gui_owner.current()
    yield owner
    gui_owner.install(None)
    late = owner.finish()
    error = gui_owner.session_error(owner.stray_engine_hits, late)
    if error is not None:
        raise error


@pytest.fixture(autouse=True)
def end_row_processes(process_owner):
    """End, when the row ends, every process the row started that its own
    fixtures did not end (a driver built in the test body), and error the
    row naming what it left."""
    before = process_owner.ledger.pids()
    yield
    process_owner.end_since(before)
