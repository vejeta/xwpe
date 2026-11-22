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

The folder is made by folder_of_length() (exact length) or by at_least()
(a path of at least N bytes, whatever the base is, one short child when the
base is already longer).  Nothing outside this module grows a path in a loop.
"""
import os

import pytest

import procinfo

COMPONENT = 60          # bytes of each folder name in a deep folder
NAME_MARGIN = 100       # room left for the file names a row makes in the folder


class BaseTooLong(ValueError):
    """The folder a row builds under is already longer than the length the
    row asked for.  The row's premise (a path of exactly N bytes) cannot be
    made there; a row that only needs a path of at least N bytes asks for
    at_least() instead."""


def at_least(base, total, leaf="", component=COMPONENT):
    """Make a fresh folder under @base such that the path of @leaf inside it
    is at least @total bytes, and return the folder.

    A row about a deep path ("a path over 200 bytes") asks here, whatever
    the test's temporary folder happens to be: when @base is already that
    long, one short child is made and the path is long all the same, so the
    row never fails in its own setup on a long base (os.makedirs of a folder
    that exists).  The path is exactly @total bytes when @base is shorter, and
    it is made of names of @component bytes.  A system whose path limit is
    under @total holds no such path: the row skips, naming the limit."""
    path = str(base)
    wanted = total
    if leaf:
        wanted = total - len(os.sep) - len(leaf)
    limit = procinfo.path_max(path)
    if total >= limit:
        pytest.skip("needs a system path limit over %d bytes (this one: %d)"
                    % (total, limit))
    if len(path) >= wanted - 1:
        path = os.path.join(path, "d")
    else:
        # Stop while at least two bytes are left, so the last name is never
        # empty and the path comes to exactly @wanted bytes.
        while len(path) + 1 + component <= wanted - 2:
            path = os.path.join(path, "d" * component)
        path = os.path.join(path, "e" * (wanted - len(path) - 1))
    os.makedirs(path, exist_ok=True)
    return path


def folder_of_length(base, total, component=COMPONENT):
    """Make a folder under @base whose absolute path is exactly @total bytes,
    its names @component bytes long, and return it.

    A row that needs a path of an exact length (one byte over a buffer) asks
    for it here and gets it on any system that can hold it; one that cannot
    holds it is refused by the system and raises OSError.  A @base already
    longer than @total raises BaseTooLong, by name."""
    path = str(base)
    if len(path) > total:
        raise BaseTooLong("the base %s (%d bytes) is already longer than the "
                          "%d bytes asked for" % (path, len(path), total))
    # Stop while at least two bytes are left, so the last name is never empty.
    while len(path) + 1 + component <= total - 2:
        path = os.path.join(path, "d" * component)
    last = total - len(path) - 1
    if last > 0:
        path = os.path.join(path, "e" * last)
    os.makedirs(path, exist_ok=True)
    assert len(path) == total
    return path


def premise_not_past(base, total):
    """Skip the row, naming why, when the folder @base is already as long as
    @total.

    A row about a path NEAR a length (a path just over a buffer, with an upper
    bound it asserts) cannot make it under a folder that is longer than that
    length already: its premise is unreachable there, and the row says so
    instead of passing on a path far past the length it is about.  A row that
    only needs "at least @total" does not call this."""
    if len(str(base)) >= total:
        pytest.skip("the test folder is %d bytes, already past the %d bytes "
                    "this row is about" % (len(str(base)), total))


FPC_PATH_LIMIT = 255   # fpc keeps a file's path in a 255 byte string


def tool_takes(tool, limit, path):
    """Skip the row, naming why, when @path is longer than the @limit bytes
    the third-party program @tool takes.

    A row that runs a compiler on a file in the test's folder has no meaning
    under a folder that program cannot open: the refusal is the program's, not
    the editor's.  The row says so by name instead of passing on a short
    folder only, so a long folder shows the skip."""
    if len(str(path)) > limit:
        pytest.skip("%s takes a path of at most %d bytes; this one is %d"
                    % (tool, limit, len(str(path))))


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
