"""A folder of a given length, asked of the system that has to hold it.

Several rows make a folder far deeper than the buffer the editor once kept a
path in.  Linux holds 4096 bytes of path, macOS and the BSDs 1024, so a row
that makes a 4040 byte folder dies in os.makedirs on them before it reaches
the editor.  Each row goes through one of these, by what it is about:

- the editor's own old cut at N bytes (a 1105 byte folder against a 1023 byte
  buffer): held().  A system that holds no path of N bytes cannot have the
  defect, so the row skips by name there.
- a path no system holds: over_the_limit(), the length that this system
  refuses.
- a folder as deep as the system allows, because the subject is only that it
  is far over an old, smaller buffer: as_deep_as_allowed().
- a folder that fits with just too little room left for a name:
  under_the_limit().

The folder is made by folder_of_length().
"""
import os

import pytest

import procinfo

COMPONENT = 60          # bytes of each folder name in a deep folder
NAME_MARGIN = 100       # room left for the file names a row makes in the folder


def folder_of_length(base, total, component=COMPONENT):
    """Make a folder under @base whose absolute path is exactly @total bytes,
    its names @component bytes long, and return it.

    A row that needs a path of an exact length (one byte over a buffer) asks
    for it here and gets it on any system that can hold it; one that cannot
    holds it is refused by the system and raises OSError."""
    path = str(base)
    # Stop while at least two bytes are left, so the last name is never empty.
    while len(path) + 1 + component <= total - 2:
        path = os.path.join(path, "d" * component)
    last = total - len(path) - 1
    if last > 0:
        path = os.path.join(path, "e" * last)
    os.makedirs(path, exist_ok=True)
    assert len(path) == total
    return path


def over_the_limit(base, margin=COMPONENT):
    """A path length (in bytes) that this system refuses under @base: its
    limit plus @margin.

    A row whose subject is the system's refusal (the box that says a path is
    longer than the system allows) uses it, so the path is too long on every
    system and not only on the one with the smallest limit."""
    return procinfo.path_max(base) + margin


def held(base, total):
    """Skip the row, naming why, when this system cannot hold a path of
    @total bytes under @base.

    A row about the editor's old cut at N bytes calls it before it makes its
    folder: no folder of that length exists on a system that stops at less,
    and the cut cannot happen there."""
    limit = procinfo.path_max(base)
    if total >= limit:
        pytest.skip("needs a system path limit over %d bytes (this one: %d)"
                    % (total, limit))


def as_deep_as_allowed(base, wanted, margin=NAME_MARGIN):
    """The length of a folder that is @wanted bytes where the system holds
    it, and its limit less @margin where it does not.

    A row whose subject is only that the folder is far over a buffer of 255
    bytes runs on every system with the deepest folder the system can hold,
    with @margin left for the names the row makes in it."""
    return min(wanted, procinfo.path_max(base) - margin)


def under_the_limit(base, room):
    """The length of a folder that fits under this system's limit with only
    @room bytes left, for a row whose name makes the whole path too long."""
    return procinfo.path_max(base) - room
