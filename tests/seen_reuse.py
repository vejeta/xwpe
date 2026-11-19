"""A file system gives an inode number back after its object is gone.

The rows that prove the editor does not take a new object for the one it saw
need that to happen, so they make the number come back on purpose and they
run on a disk file system: tmpfs never gives a number back, and a row that
met it would pass without testing anything.  Three helpers:

- fs_type(folder): the name of the file system a folder is on, for the row's
  message, so a skipped or passed row says where it ran;
- fs_gives_numbers_back(folder): a one-shot probe, in a scratch file of its
  own, that tells "this file system never gives a number back" (a skip with
  that reason) from "the number was not reached" (a failure);
- make_file_with_number(folder, name, want, ...): remove and make again,
  keeping every object that landed below the wanted number under a filler
  name, until the new file has the wanted number or one lands above it.
"""
import os
import subprocess
import tempfile

import pytest

PROBE_ROUNDS = 8            # how many times the probe removes and makes a file again
FILLER_TRIES = 64           # the most objects a reach for one number makes


def fs_type(folder):
    """The file system type of `folder`, as `stat -f` names it ("ext2/ext3"
    for ext4: the same magic number)."""
    out = subprocess.run(["stat", "-f", "-c", "%T", str(folder)], stdout=subprocess.PIPE,
                         universal_newlines=True, timeout=30)
    return out.stdout.strip() or "unknown"


def fs_gives_numbers_back(folder):
    """True when a file removed and made again in `folder` ever has the old
    number.  The probe makes its own scratch folder under `folder` and removes
    it by that exact path."""
    scratch = tempfile.mkdtemp(prefix="reuse-probe.", dir=str(folder))
    try:
        probe = os.path.join(scratch, "p")
        for _ in range(PROBE_ROUNDS):
            with open(probe, "w") as f:
                f.write("x")
            ino = os.stat(probe).st_ino
            os.unlink(probe)
            with open(probe, "w") as f:
                f.write("y")
            if os.stat(probe).st_ino == ino:
                return True
            os.unlink(probe)
        return False
    finally:
        for name in os.listdir(scratch):
            os.unlink(os.path.join(scratch, name))
        os.rmdir(scratch)


def reached_or_say(folder, got, want):
    """Pass when `got` is the wanted number.  Otherwise a skip that names the
    cause when the file system never gives a number back, and a failure when
    it does and the number was not reached."""
    if got == want:
        return
    if not fs_gives_numbers_back(folder):
        pytest.skip("this file system (%s) does not give an inode number back" % fs_type(folder))
    pytest.fail("the number %d was not reached on %s (got %d)" % (want, fs_type(folder), got))


def make_folder_with_number(path, want):
    """Make the folder `path` so that it has inode number `want`, as
    make_file_with_number does for a file.  The folder is made even when the
    number is not reached (the last one made, above the wanted number, is
    left in place).  Returns the fillers' paths and whether it was reached.

    A folder that something still holds open (a descriptor, a working folder)
    never gives its number back, so a row that holds one cannot reach it, and
    says so by the False it gets."""
    fillers = []
    path = str(path)
    for n in range(FILLER_TRIES):
        os.mkdir(path)
        ino = os.stat(path).st_ino
        if ino == want or ino > want:
            return fillers, ino == want
        filler = "%s.filler%d" % (path, n)
        os.rename(path, filler)
        fillers.append(filler)
    os.mkdir(path)
    return fillers, False


def make_file_with_number(folder, name, want, text="x"):
    """Make `folder/name` so that it has inode number `want`.  An object that
    lands below the number is kept under a filler name and the next one is
    tried; one that lands above stops the reach.  Returns the fillers' paths
    (the caller removes them) and whether the number was reached."""
    fillers = []
    path = os.path.join(str(folder), name)
    for n in range(FILLER_TRIES):
        with open(path, "w") as f:
            f.write(text)
        ino = os.stat(path).st_ino
        if ino == want:
            return fillers, True
        if ino > want:
            os.unlink(path)
            return fillers, False
        filler = "%s.filler%d" % (path, n)
        os.rename(path, filler)
        fillers.append(filler)
    return fillers, False
