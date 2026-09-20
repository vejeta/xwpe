"""Copying a block copies WHOLE lines, including the part past the visible edge.

A line loaded from disk can be longer than the editor's column limit (mx.x = 120).
The block-copy path staged such a line through an mx.x-sized buffer, so the copy
(a) overflowed that buffer and (b) reached the OS clipboard split at ~column 120 --
pasting into another app showed only a fraction / a broken line.  Copy now serializes
the selection straight from the source buffer, full lines and all.

Drives Mark Whole + Copy on a 250-char line and asserts the OS-clipboard payload
(OSC 52) contains the entire line as one contiguous run, plus the following line.
"""
from wpe_driver import WpeSession

CK = "\x0b"   # Ctrl-K, WordStar block prefix
CC = "\x03"   # Ctrl-C = Copy

LONG = "L" + "".join("abcdefghij"[i % 10] for i in range(248)) + "Z"   # 250 chars


def test_copy_long_line_is_not_truncated_or_split(tmp_path):
    with WpeSession(str(tmp_path), LONG + "\nsecond line\n", filename="t.c") as w:
        w.key(CK, "x")          # Mark Whole
        w.key(CC)               # Copy -> OS clipboard via OSC 52
        payload = w.osc52_payload()
    assert payload, "no OSC 52 payload emitted"
    assert LONG in payload, \
        "the 250-char line was split or truncated in the clipboard:\n%r" % payload
    assert "second line" in payload, "the following line was lost:\n%r" % payload
