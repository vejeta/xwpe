"""Does a file here have a birth time the editor can read?

The editor tells "the file the window read, touched since" from "another file
that was given its inode number" by the time the file was made, where the
system keeps one.  Where it does not (OpenBSD, ext4 with 128-byte inodes, ext3,
most FUSE) a file touched since the read is not proven to be the same one, and
the editor asks or refuses instead of writing.  A row that touches a file after
the window read it therefore has two outcomes, and it must branch on what is
OBSERVED, never skip: reports_birth_time() says which one this run has.

The answer is read in the order the editor's own build reads it (we_platform.h):
the test hook first, then how the editor was built, then the file system.
"""
import os
import re
import subprocess
import tempfile

import pytest

import wpe_driver

HOOK_VARIABLE = "XWPE_TEST_FAIL"
HOOK_NO_BIRTH = "seen-no-birth"
CONFIG_NAME = "config.h"
# stat -c %W prints the birth as seconds since the epoch, 0 or -1 where the
# system has none, and "-" in older tools.
NO_BIRTH_WORDS = ("0", "-1", "-")


def _built_with(config_text, name):
    """Is `name` defined, and not as 0, in the editor's config.h?"""
    found = re.search(r"^#define\s+%s\s*(\S*)" % re.escape(name), config_text, re.M)
    return found is not None and found.group(1) != "0"


def _build_route(config_text):
    """How the editor was built to read a birth time: "statx", "member" or None."""
    if _built_with(config_text, "HAVE_STATX") and _built_with(config_text, "HAVE_DECL_STATX_BTIME"):
        return "statx"
    if (_built_with(config_text, "HAVE_STRUCT_STAT_ST_BIRTHTIM")
            or _built_with(config_text, "HAVE_STRUCT_STAT_ST_BIRTHTIMESPEC")):
        return "member"
    return None


def _config_text():
    """The text of the config.h beside the editor under test.  A missing file
    fails the row and names it: guessing the build would hide a premise."""
    path = os.path.join(os.path.dirname(os.path.abspath(wpe_driver.WPE_BIN)), CONFIG_NAME)
    if not os.path.exists(path):
        pytest.fail("cannot tell how the editor reads a birth time: %s is missing" % path)
    with open(path) as fh:
        return fh.read()


def _folder_gives_birth(folder, route):
    """Does a file made in `folder` report a birth time by `route`?  The probe
    file is made and removed by its exact path."""
    fd, probe = tempfile.mkstemp(prefix="birth-probe.", dir=str(folder))
    os.close(fd)
    try:
        if route == "statx":
            out = subprocess.run(["stat", "-c", "%W", probe], stdout=subprocess.PIPE,
                                 universal_newlines=True, timeout=30)
            return out.stdout.strip() not in NO_BIRTH_WORDS
        return getattr(os.stat(probe), "st_birthtime", 0) not in (0, -1)
    finally:
        os.unlink(probe)


def reports_birth_time(folder, env_extra=None):
    """True when the editor started with `env_extra` in a session working in
    `folder` can read the time its files were made: the no-birth hook is not
    set, the editor was built to read one, and the file system keeps one."""
    hook = wpe_driver.session_env_extra(env_extra).get(HOOK_VARIABLE) or ""
    if HOOK_NO_BIRTH in hook:
        return False
    route = _build_route(_config_text())
    if route is None:
        return False
    return _folder_gives_birth(folder, route)
