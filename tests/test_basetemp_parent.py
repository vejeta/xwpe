"""A session started with --basetemp under a folder that does not exist.

A job names its test folder with `--basetemp wl-logs/gate` and the checkout
has no `wl-logs`.  pytest makes the basetemp folder itself, never its
parents, so the session must make them before it asks for the folder
(tests/conftest.py, pytest_sessionstart); otherwise the run dies at its
start with an internal error and no row runs, which is how the native
Wayland gate of the Linux job never ran.
"""
import os
import sys

import gui_owner

INNER = os.path.join(gui_owner.HERE, "one_row_basetemp.py")
INNER_TIMEOUT_S = 120


def _inner_run(root, basetemp):
    """Run the one-row file in its own pytest, with `basetemp` as the test
    folder, and return the finished process (output kept for a message)."""
    home = root / "home"
    home.mkdir()
    argv = [sys.executable, "-m", "pytest", "-q", "-p", "no:cacheprovider",
            "--basetemp", str(basetemp), INNER]
    return gui_owner.run_nested(argv, INNER_TIMEOUT_S, cwd=str(root),
                                extra_env={"HOME": str(home), "TERM": "xterm"})


def test_a_basetemp_under_a_missing_folder_is_made_with_its_parents(tmp_path):
    """The user's gate job names a test folder two levels below a folder the
    checkout does not have; the run must start, run its row and pass."""
    basetemp = tmp_path / "no" / "such" / "gate"
    done = _inner_run(tmp_path, basetemp)
    shown = done.stdout.decode("utf-8", "replace")[-1500:]
    assert done.returncode == 0, "exit %s:\n%s" % (done.returncode, shown)
    assert basetemp.is_dir(), "the run did not use the folder it was given"
