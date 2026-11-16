"""What a row that sets a special bit assumes about the user running it.

The pytest side of tests/testdir.h.  A row that gives a folder or a file a
setgid or setuid bit assumes that the folder is in a group the user is in
(on the BSDs a new file takes its folder's group, and the temporary folder
is not the user's) and that the user may set the bit there at all.  When the
assumption fails the row must say so by name, not die in its setup: it is
skipped for a user who cannot be expected to meet it, and failed when the run
says it is unprivileged (WPE_TEST_UNPRIVILEGED=1, set by the BSD CI).

Run both before the first chmod of the row:

    workdir_in_my_group(wd)
    special_bits_settable(wd, stat.S_ISGID)
"""
import os
import stat

import pytest

UNPRIVILEGED_ENV = "WPE_TEST_UNPRIVILEGED"
PROBE_NAME = "probe_bits"


def workdir_in_my_group(path):
    """Give the work folder this user's group, and check that it has it.

    Any user may give a folder it owns a group it is in.  A refusal is a
    failure by name: every setgid row in the folder would otherwise fail for
    the system's group rule and not for the editor.
    """
    me = os.getegid()
    try:
        os.chown(path, -1, me)
        got = os.stat(path).st_gid
    except OSError as err:
        pytest.fail("the work folder %s cannot be given this user's group %d: %s" % (path, me, err))
    if got != me:
        pytest.fail("the work folder %s has group %d, not this user's %d" % (path, got, me))


def _bits_stay_on(path, bits):
    """True when a new file of `path` keeps every bit of `bits` after a chmod."""
    probe = os.path.join(path, PROBE_NAME)
    want = 0o644 | bits
    try:
        with open(probe, "w") as fh:
            fh.write("x\n")
        os.chmod(probe, want)
        return stat.S_IMODE(os.stat(probe).st_mode) == want
    except OSError:
        return False
    finally:
        try:
            os.unlink(probe)
        except OSError:
            pass


def special_bits_settable(path, bits):
    """Skip by name when this user cannot keep `bits` in `path`; fail in an
    unprivileged run, where it must hold.

    A refusal (the BSDs, setgid outside the file's group) and a silent drop
    (Linux) both read as "cannot keep".
    """
    if _bits_stay_on(path, bits):
        return
    why = "this user cannot keep special bits %04o in %s" % (bits, path)
    if os.environ.get(UNPRIVILEGED_ENV) == "1":
        pytest.fail("the run is unprivileged, so this must hold: " + why)
    pytest.skip(why)
