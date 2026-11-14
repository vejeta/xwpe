"""The X11 suite's session: what two runs at once, a leftover and a rerun see.

The suite starts its own X server on a display number the server picks, so
two runs of it at once each have a display of their own.  Before, both
wanted the same fixed number and the second run was refused.  A row that
starts an editor while another is still running is refused with the name of
the row that left it, and a run asked to retry failed rows is refused.

The first row starts two runs of this directory, each holding its session
open until the other has its own; the rows they run are the probe at the
end of this file, which skips itself unless a parent asks for it.
"""
import os
import shutil
import subprocess
import sys

import pytest

from conftest import PreviousRowLeft, launch_editor
from gui_owner import scaled, wait_for

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.dirname(HERE)
TOP = os.path.dirname(TESTS)

PROBE_OUT = "XWPE_SESSION_PROBE_OUT"
PROBE_RELEASE = "XWPE_SESSION_PROBE_RELEASE"
PROBE_NODE = "tests/x11/test_session_owner.py::test_probe_holds_a_session"
RUN_START_S = 90.0
RUN_END_S = 60.0
HOLD_S = 120.0
RERUNS_REFUSED = "--reruns is refused"
# The unix socket a live X server has: the lock file next to it is not made
# by every Xvfb build.
X_SOCKET = "/tmp/.X11-unix/X%s"


def _child_env(out, release):
    """The environment of one run of the probe: the machine's, asked to
    write its display to `out` and to hold its session until `release`."""
    env = dict(os.environ)
    env[PROBE_OUT] = str(out)
    env[PROBE_RELEASE] = str(release)
    return env


def _start_run(tmp_path, name, release):
    """Start one run of the probe in a pytest of its own."""
    out = tmp_path / (name + ".display")
    log = open(tmp_path / (name + ".log"), "wb")
    try:
        proc = subprocess.Popen(
            [sys.executable, "-m", "pytest", "-q", "-p", "no:cacheprovider",
             "--basetemp", str(tmp_path / ("base-" + name)), PROBE_NODE],
            cwd=TOP, env=_child_env(out, release), stdin=subprocess.DEVNULL,
            stdout=log, stderr=subprocess.STDOUT)
    finally:
        log.close()
    return proc, out, tmp_path / (name + ".log")


def _said_or_ended(proc, out):
    """True once the run wrote its display or has ended."""
    return out.exists() or proc.poll() is not None


def _describe(proc, out, log):
    """What the run did, for a failure message."""
    text = log.read_text(errors="replace")[-1500:]
    return "exit %s, display %s, output:\n%s" % (
        proc.poll(), out.read_text().strip() if out.exists() else None, text)


def _end_runs(runs):
    """End the runs this row started, by their own process objects."""
    for proc, _, _ in runs:
        if proc.poll() is None:
            proc.terminate()
    for proc, _, _ in runs:
        try:
            proc.wait(timeout=scaled(RUN_END_S))
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


# @scenario SC-harness-owner-22
def test_two_runs_at_once_each_have_a_display_of_their_own(tmp_path):
    release = tmp_path / "release"
    runs = []
    try:
        for name in ("first", "second"):
            runs.append(_start_run(tmp_path, name, release))
            proc, out, log = runs[-1]
            wait_for(lambda: _said_or_ended(proc, out),
                     "the %s run to say which display it has" % name,
                     RUN_START_S, state_fn=lambda: _describe(proc, out, log))
            assert out.exists(), "the %s run ended without a display: %s" % (
                name, _describe(proc, out, log))
        displays = [out.read_text().strip() for _, out, _ in runs]
        assert displays[0] != displays[1], \
            "both runs have the display %s" % displays[0]
        for display in displays:
            assert os.path.exists(X_SOCKET % display.lstrip(":")), \
                "the server of %s is gone while its run holds its session" % display
        release.write_text("")
        for name, (proc, out, log) in zip(("first", "second"), runs):
            assert proc.wait(timeout=scaled(RUN_END_S)) == 0, \
                "the %s run failed: %s" % (name, _describe(proc, out, log))
    finally:
        _end_runs(runs)


# @scenario SC-harness-owner-25
def test_a_run_asked_to_rerun_failed_rows_is_refused(tmp_path):
    done = subprocess.run(
        [sys.executable, "-m", "pytest", "-q", "-p", "no:cacheprovider",
         "--basetemp", str(tmp_path / "base"), "--reruns", "1",
         "--collect-only", "tests/x11/test_session_owner.py"],
        cwd=TOP, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=scaled(RUN_END_S))
    text = done.stdout.decode(errors="replace")
    assert done.returncode != 0 and RERUNS_REFUSED in text, \
        "a run with --reruns 1 was not refused (exit %d): %s" % (
            done.returncode, text[-800:])


# @scenario SC-harness-owner-24
def test_a_second_editor_while_one_runs_is_refused_naming_the_first_row(
        xwpe, gui_owner, tmp_path, request):
    with pytest.raises(PreviousRowLeft) as raised:
        launch_editor(gui_owner, tmp_path, request, [str(tmp_path / "t.c")])
    assert request.node.nodeid in str(raised.value)
    assert xwpe.proc.poll() is None, "the refusal ended the running editor"


sys.path.insert(0, HERE)
import test_debug_answer_wait as jdb_shapes  # noqa: E402
import test_debugger_command_length as pdb_shapes  # noqa: E402

# The fixture the pdb shape asks for, made available to the rows below.
cjk_folder = pdb_shapes.cjk_folder

DETACHED = {"XWPE_TEST_DETACHED": "1"}
DETACHED_REASON = ("editor without a controlling terminal: tty ownership, "
                   "product design pending")


# An editor started the way a desktop launcher starts it is a session leader
# with no controlling terminal.  The debugger starts below fail then (the
# program is refused a terminal, or the editor is hung up) though they pass
# when it is started from a terminal.  This is a product defect; the rows are
# strict expected failures, so the day the product is fixed they turn red and
# the marks come off.
@pytest.mark.xfail(strict=True, reason=DETACHED_REASON)
@pytest.mark.skipif(not shutil.which("jdb") or not shutil.which("javac"),
                    reason="no jdb or javac")
@pytest.mark.parametrize("xwpe", [{**jdb_shapes.JDB_STUB_ENV, **DETACHED}],
                         indirect=True)
# @scenario SC-harness-owner-29
def test_a_detached_editor_runs_the_jdb_keys_shape(xwpe):
    jdb_shapes.test_jdb_keys_reach_the_editor_while_it_answers(xwpe)


@pytest.mark.xfail(strict=True, reason=DETACHED_REASON)
@pytest.mark.skipif(not shutil.which("python3"), reason="python3 not on PATH")
@pytest.mark.parametrize("xwpe", [{**pdb_shapes.PDB_SESSION, **DETACHED}],
                         indirect=True)
# @scenario SC-harness-owner-30
def test_a_detached_editor_runs_the_pdb_start_shape(cjk_folder, tmp_path, xwpe):
    pdb_shapes.test_pdb_starts_in_a_folder_named_in_cjk_letters(
        cjk_folder, tmp_path, xwpe)


@pytest.mark.skipif(PROBE_OUT not in os.environ,
                    reason="run by test_two_runs_at_once_each_have_a_display_of_their_own")
def test_probe_holds_a_session(xwpe):
    """One run of the suite: write the display, hold the session open until
    the parent says the other run has its own."""
    import conftest
    with open(os.environ[PROBE_OUT], "w") as fh:
        fh.write(conftest.display_name() + "\n")
    release = os.environ[PROBE_RELEASE]
    wait_for(lambda: os.path.exists(release),
             "the parent to release the probe", HOLD_S)
