"""Line endings of a saved file, in xwpe (X11/Xft): twin of
tests/test_line_endings.py for wpe.

The reader and the writer are shared by every display, but the file is
opened and saved through xwpe's own window code, so the rows that matter
most are held to the same bytes here: a CRLF file saved CRLF, a mixed file
with the lines the user did not touch as they were, a lone carriage return
kept.  The file is read with "rb": saved_text() reads in text mode and
turns CR LF into LF.
"""
import pytest

CRLF = {"XWPE_TEST_SOURCE": "a\r\nb\r\n"}
MIXED = {"XWPE_TEST_SOURCE": "a\r\nb\nc\r\n"}
LONE_CR = {"XWPE_TEST_SOURCE": "a\rb\n"}


def _saved_bytes(xwpe):
    with open(xwpe.srcfile, "rb") as fh:
        return fh.read()


# @scenario SC-line-endings-19
@pytest.mark.parametrize("xwpe", [CRLF], indirect=True)
def test_a_crlf_file_is_saved_crlf(xwpe):
    xwpe.type("X")
    xwpe.key("F2")
    assert xwpe.proc.poll() is None, "xwpe died on F2 (Save)"
    assert _saved_bytes(xwpe) == b"Xa\r\nb\r\n", _saved_bytes(xwpe)


# @scenario SC-line-endings-19
@pytest.mark.parametrize("xwpe", [MIXED], indirect=True)
def test_a_mixed_file_keeps_each_line_as_it_was(xwpe):
    xwpe.type("X")
    xwpe.key("F2")
    assert xwpe.proc.poll() is None, "xwpe died on F2 (Save)"
    assert _saved_bytes(xwpe) == b"Xa\r\nb\nc\r\n", _saved_bytes(xwpe)


# @scenario SC-line-endings-19
@pytest.mark.parametrize("xwpe", [LONE_CR], indirect=True)
def test_a_lone_carriage_return_is_kept_as_text(xwpe):
    xwpe.type("X")
    xwpe.key("F2")
    assert xwpe.proc.poll() is None, "xwpe died on F2 (Save)"
    assert _saved_bytes(xwpe) == b"Xa\rb\n", _saved_bytes(xwpe)
