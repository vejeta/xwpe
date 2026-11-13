"""The language server's columns, on the graphical display (X11 and, through
its twin, native Wayland).

The cells a server's highlight covers are drawn by the shared painters, which
compare the byte index of a cell with bytes, so one row per backend proves
parity: after Alt-Q U on `total`, the painted cells are those of `total` on a
line with an accented letter, CJK and an emoji before it (F1 of
tests/test_lsp_positions.py), and not the cells of the comment's tail.

The stand-in server is the pyte test's own (tests/test_lsp_positions.py), run
here as a program on PATH.  The geometry is measured, not assumed: the same
answer paints `total` on the ASCII line below, whose cells start at a known
column, so that band gives the cell width and the left edge of the window's
text, and the band on the wide line must start where visual column 27 does.
"""
import atexit
import importlib.util
import os
import pathlib
import shutil
import sys
import tempfile
import time

import pytest

pytest.importorskip("PIL")  # python3-pil; skip cleanly if absent
from PIL import ImageChops

_TESTS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CELL_THRESHOLD = 40               # a grey level: how much a changed pixel differs
TOTAL_VISUAL_COL = 27             # F1: the cells before `total` on its line
USE_VISUAL_COL = 19               # the ASCII line: `total` after `printf("%d\n", `
TOTAL_LEN = 5
PIXEL_SLACK = 2
ANSWER_WAIT_S = 15


def _load_support():
    """The pyte test's module, under a name of its own: pytest has this
    file loaded as test_lsp_positions, and importing that name would give
    this file back."""
    if _TESTS_DIR not in sys.path:
        sys.path.insert(0, _TESTS_DIR)
    spec = importlib.util.spec_from_file_location(
        "lsp_positions_support", os.path.join(_TESTS_DIR, "test_lsp_positions.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


lp = _load_support()
_STUB = tempfile.mkdtemp(prefix="xwpe-lsppos-")
atexit.register(shutil.rmtree, _STUB, True)
_ENV = lp._env(pathlib.Path(_STUB), lp.fake_config("F1", "utf-16"))
PARAM = {"XWPE_TEST_SOURCE": lp.source("F1"), "XWPE_TEST_FILE": "demo.c",
         "PATH": _ENV["PATH"], "XWPE_LSP_TRACE": _ENV["XWPE_LSP_TRACE"]}


def _stub_has(text):
    return text in lp._read(os.path.join(_STUB, lp.FAKE_LOG))


def _wait_stub(text, timeout=ANSWER_WAIT_S):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if _stub_has(text):
            return True
        time.sleep(0.2)
    return _stub_has(text)


def _tallest_band(mask):
    """(top, bottom) of the tallest run of consecutive pixel rows that
    changed, bottom exclusive."""
    runs, start, prev = [], None, None
    for y in range(mask.height):
        if not mask.crop((0, y, mask.width, y + 1)).getbbox():
            continue
        if start is None or y != prev + 1:
            if start is not None:
                runs.append((start, prev + 1))
            start = y
        prev = y
    if start is not None:
        runs.append((start, prev + 1))
    assert runs, "the highlight changed nothing on the screen"
    return max(runs, key=lambda run: run[1] - run[0])


def _band_extents(mask, y0, y1):
    """(left, right) of the changed pixels between rows y0 and y1."""
    box = mask.crop((0, y0, mask.width, y1)).getbbox()
    assert box, "nothing changed between rows %d and %d" % (y0, y1)
    return box[0], box[2]


@pytest.mark.parametrize("xwpe", [PARAM], indirect=True)
def test_highlight_paints_the_cells_of_the_symbol(xwpe):
    """Alt-Q U with the cursor on the ASCII `total` (line 6): the server's
    answer paints it and the `total` of line 5, after the wide characters.
    The band on line 5 starts at the pixel column of visual column 27."""
    xwpe.key("alt+q", delay=0.5)
    xwpe.key("e", delay=0.3)
    assert _wait_stub("NOTE textDocument/didOpen"), "the server did not start"
    time.sleep(2.0)
    xwpe.key("Down", delay=0.05)
    for _ in range(lp.USE_LINE - 1):
        xwpe.key("Down", delay=0.05)
    for _ in range(lp.USE_BYTE):
        xwpe.key("Right", delay=0.05)
    time.sleep(0.5)
    before = xwpe.screenshot()
    xwpe.key("alt+q", delay=0.5)
    xwpe.key("u", delay=0.3)
    assert _wait_stub("REQ textDocument/documentHighlight"), \
        "Alt-Q U did not reach the server:\n%s" % lp._read(
            os.path.join(_STUB, lp.FAKE_LOG))
    time.sleep(1.5)
    after = xwpe.screenshot()
    mask = ImageChops.difference(before, after).convert("L").point(
        lambda p: 255 if p > CELL_THRESHOLD else 0)
    # the two rows (line 5 and line 6) are painted: the tallest run of
    # changed pixel rows is one band of two cell rows (the lines the frame
    # and the status row redraw are one pixel high)
    top, bottom = _tallest_band(mask)
    middle = (top + bottom) // 2
    left6, right6 = _band_extents(mask, middle, bottom)
    left5, right5 = _band_extents(mask, top, middle)
    cell = (right6 - left6) / TOTAL_LEN
    origin = left6 - USE_VISUAL_COL * cell
    want5 = origin + TOTAL_VISUAL_COL * cell
    assert abs(left5 - want5) <= PIXEL_SLACK, (
        "`total` on the wide line should start at pixel %d (visual column %d), "
        "the painted band starts at %d" % (want5, TOTAL_VISUAL_COL, left5))
    assert abs((right5 - left5) - TOTAL_LEN * cell) <= PIXEL_SLACK, \
        "the band should be %d cells wide, it is %d pixels" \
        % (TOTAL_LEN, right5 - left5)
