"""Options > AI dialog under X11: clicking a radio's LABEL selects it.

The checkbox and radio widgets of the dialog engine used to answer a mouse
click only on their three-cell "( )" glyph; a click on the words next to it
("Ollama (local)", "Auto-accept edits", "Claude Code") did nothing, although
that is where a user aims.  e_opt_glyph_hit_width (we_opt.c) now extends the
clickable strip over the whole label.

These tests open Options > AI under a real headless X server, click the LAST
character of a label (far from the glyph, so a glyph-only hit test misses), press
Ok and read the config file xwpe writes.  The file on disk is the ground truth:
the same session that took the click persisted the choice, or it did not.

Click targets are computed from the dialog's own geometry, not from a font
table: the dialog is located as the block of pixels that changed when it
opened, its cell size follows from the known 65-cell width and row count of
the layout in e_ai_options (we_ai_ui.c), and each label's cell comes from the
x/y the layout gives its radio.  The Wayland suite runs this same module under
its native-Wayland fixture (tests/wayland/test_ai_options.py).
"""
import os
import subprocess
import time

import pytest

pytest.importorskip("PIL")  # python3-pil; skip cleanly if absent
from PIL import ImageChops

from conftest import changed_pixels, XWPE_BIN

# Per-test environment: an AI build with the assistant switched on and the
# in-process mock backend (no network).  XDG_CONFIG_HOME is blanked so the
# config file is $HOME/.config/xwpe/xwperc, with HOME the fixture's temp dir.
AI_ENV = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
          "XDG_CONFIG_HOME": ""}

# --- the dialog layout, as e_ai_options (we_ai_ui.c) builds it -----------------
# The box is 64 wide (xa..xe inclusive = 65 cells) and 20 rows tall (21 cells),
# one row more in a build with the Agent engine radio.  Widget x/y below are
# dialog-relative cells; a radio's label is drawn OPT_GLYPH_WIDTH cells past its
# "( )" glyph column.
DIALOG_COLS = 65
DIALOG_ROWS_PLAIN = 21
DIALOG_ROWS_WITH_AGENT = 22
GLYPH_WIDTH = 4

BACKEND_GLYPH_X, BACKEND_FIRST_ROW = 5, 5        # 4 items, one per row
BACKEND_LABELS = ["Claude CLI (login)", "Ollama (local)",
                  "OpenAI-compatible", "Claude API (key)"]
# AIBackend numbers in the order the dialog lists them (g_ai_bk_order ->
# the enum in we_ai.h): Claude CLI=4, Ollama=0, OpenAI-compatible=1, Claude API=2.
BACKEND_NUMBERS = [4, 0, 1, 2]

POLICY_GLYPH_X, POLICY_FIRST_ROW = 38, 5          # 3 items, one per row
POLICY_LABELS = ["Ask each action", "Auto-accept edits", "Auto (skip asks)"]
POLICY_NAMES = ["ask", "edits", "auto"]           # wpe_ai_policy_name

AGENT_ROW = 17
AGENT_CLAUDE_CODE_GLYPH_X = 35
AGENT_CLAUDE_CODE_LABEL = "Claude Code"
AGENT_ROW_MARKER = "Agent (Alt-G g):"             # only in an agent-host build

# The frame is this many rows/columns tall once the dialog has been found; a
# band of changed pixels shorter than this is menu-bar or status-line noise.
MIN_BAND_PIXELS = 100
# A cell size must come out as a whole number of pixels; a larger remainder
# means the dialog was not located (or the row count assumed is wrong).
CELL_SIZE_TOLERANCE = 0.25


def _binary_has(text):
    """True if the xwpe binary under test carries `text` (a dialog label)."""
    try:
        out = subprocess.run(["strings", XWPE_BIN],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return text.encode() in out
    except Exception:
        return False


HAS_AI = _binary_has("Alt-G AI")
HAS_AGENT_RADIO = _binary_has(AGENT_ROW_MARKER)
DIALOG_ROWS = DIALOG_ROWS_WITH_AGENT if HAS_AGENT_RADIO else DIALOG_ROWS_PLAIN

needs_ai_build = pytest.mark.skipif(not HAS_AI, reason="xwpe built without --enable-ai")
ai_session = pytest.mark.parametrize("xwpe", [AI_ENV], indirect=True)


# --- locating the dialog on screen ---------------------------------------------

class DialogGeometry:
    """Where the dialog sits on screen and how big its cells are."""

    def __init__(self, x0, y0, cell_w, cell_h):
        self.x0, self.y0, self.cell_w, self.cell_h = x0, y0, cell_w, cell_h

    def cell_centre(self, col, row):
        """Screen pixel at the middle of the dialog-relative cell (col, row)."""
        return (self.x0 + col * self.cell_w + self.cell_w // 2,
                self.y0 + row * self.cell_h + self.cell_h // 2)

    def __repr__(self):
        return "DialogGeometry(x0=%d y0=%d cell=%dx%d)" % (
            self.x0, self.y0, self.cell_w, self.cell_h)


def _changed_mask(before, after, thresh=40):
    """Black/white image: white where `after` differs from `before`."""
    diff = ImageChops.difference(before, after).convert("L")
    return diff.point(lambda p: 255 if p > thresh else 0)


def _band_counts(mask, axis):
    """Changed-pixel count per row (axis "y") or per column (axis "x")."""
    w, h = mask.size
    if axis == "y":
        return [mask.crop((0, y, w, y + 1)).histogram()[255] for y in range(h)]
    return [mask.crop((x, 0, x + 1, h)).histogram()[255] for x in range(w)]


def _longest_run(counts, minimum):
    """(first, last) of the longest run of consecutive indices with
    counts >= minimum, or None.  The dialog is the longest such band; a
    highlighted menu title or a rewritten status line is a short one."""
    best, start = None, None
    for i, n in enumerate(counts + [0]):
        if n >= minimum and start is None:
            start = i
        elif n < minimum and start is not None:
            if best is None or (i - start) > (best[1] - best[0] + 1):
                best = (start, i - 1)
            start = None
    return best


def _whole_cells(pixels, cells, what):
    """Pixels per cell for a band of `pixels` covering `cells` cells; fails
    loudly when the division is not (close to) a whole number."""
    size = pixels / float(cells)
    rounded = int(round(size))
    assert abs(size - rounded) <= CELL_SIZE_TOLERANCE, (
        "%s: %d px is not a whole number of %d cells (%.2f px per cell) -- "
        "the dialog was not located, or the row count assumed for this build "
        "(%d) is wrong" % (what, pixels, cells, size, DIALOG_ROWS))
    return rounded


def locate_dialog(before, after):
    """Geometry of the dialog that appeared between the two screenshots.

    The dialog is the largest block of pixels that changed when it opened; its
    width is DIALOG_COLS cells and its height DIALOG_ROWS cells, which gives the
    cell size without assuming any font metric."""
    mask = _changed_mask(before, after)
    cols = _longest_run(_band_counts(mask, "x"), MIN_BAND_PIXELS)
    rows = _longest_run(_band_counts(mask, "y"), MIN_BAND_PIXELS)
    assert cols and rows, "no dialog-sized block of pixels changed on screen"
    width = cols[1] - cols[0] + 1
    height = rows[1] - rows[0] + 1
    cell_w = _whole_cells(width, DIALOG_COLS, "dialog width")
    cell_h = _whole_cells(height, DIALOG_ROWS, "dialog height")
    return DialogGeometry(cols[0], rows[0], cell_w, cell_h)


# --- driving the dialog ---------------------------------------------------------

def open_ai_options(xwpe):
    """Options (Alt-O) > AI (i); returns the dialog's on-screen geometry."""
    before = xwpe.screenshot()
    xwpe.menu("o", "i", delay=1.0)
    after = xwpe.screenshot()
    assert changed_pixels(before, after) > 2000, "the AI settings dialog did not appear"
    return locate_dialog(before, after)


def label_last_cell(glyph_x, label):
    """Dialog-relative column of the label's last visible character."""
    return glyph_x + GLYPH_WIDTH + len(label.rstrip()) - 1


def click_label_end(xwpe, geometry, glyph_x, row, label):
    """Click the last character of a radio's label: the far end of the text,
    well clear of the "( )" glyph, so only a label-wide hit test can take it."""
    col = label_last_cell(glyph_x, label)
    px, py = geometry.cell_centre(col, row)
    xwpe.click(px, py, delay=0.8)
    return (px, py)


def press_ok(xwpe):
    """Ok (Alt-O): applies every field and writes the config file."""
    xwpe.key("alt+o", delay=1.0)


# --- the config file on disk ----------------------------------------------------

def xwperc_path(xwpe):
    """$HOME/.config/xwpe/xwperc for this session: the fixture sets HOME to
    the directory holding the edited file."""
    home = os.path.dirname(xwpe.srcfile)
    return os.path.join(home, ".config", "xwpe", "xwperc")


def read_xwperc(path):
    try:
        with open(path) as fh:
            return fh.read()
    except OSError:
        return ""


def wait_for_xwperc_key(path, key, timeout=6.0):
    """Text of the config file once it holds a `key : ...` line (Ok writes the
    file after the dialog closes), or whatever is there when time runs out."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        text = read_xwperc(path)
        if any(line.startswith(key + " :") for line in text.splitlines()):
            return text
        time.sleep(0.1)
    return read_xwperc(path)


def assert_xwperc_has(xwpe, key, value, clicked_at, geometry):
    """The config file must hold exactly `key : value`; the failure message
    carries the whole file so a wrong value is read in context."""
    path = xwperc_path(xwpe)
    text = wait_for_xwperc_key(path, key)
    wanted = "%s : %s" % (key, value)
    assert wanted in text.splitlines(), (
        "%s not found in %s after clicking the label at pixel %r (%r)\n"
        "---- xwperc ----\n%s---- end ----" % (wanted, path, clicked_at,
                                                geometry, text or "(no file)\n"))


# --- scenarios --------------------------------------------------------------------

# @scenario SC-ai-options-13x
@needs_ai_build
@ai_session
def test_backend_label_click_applies_on_ok(xwpe):
    """Click the end of the "OpenAI-compatible" label (not its glyph), Ok:
    xwperc says AIBackend : 1.  The session starts on the mock backend, so the
    dialog marks Claude CLI; a missed click would persist 4 (Claude CLI)."""
    geometry = open_ai_options(xwpe)
    index = BACKEND_LABELS.index("OpenAI-compatible")
    clicked_at = click_label_end(xwpe, geometry, BACKEND_GLYPH_X,
                                 BACKEND_FIRST_ROW + index, BACKEND_LABELS[index])
    press_ok(xwpe)
    assert xwpe.proc.poll() is None, "xwpe died in the AI settings dialog"
    assert_xwperc_has(xwpe, "AIBackend", BACKEND_NUMBERS[index], clicked_at, geometry)


# @scenario SC-ai-options-17x
@needs_ai_build
@ai_session
def test_policy_label_click_applies_on_ok(xwpe):
    """Click the end of the "Auto-accept edits" label, Ok: xwperc says
    AIPolicy : edits (the session starts on "ask")."""
    geometry = open_ai_options(xwpe)
    index = POLICY_LABELS.index("Auto-accept edits")
    clicked_at = click_label_end(xwpe, geometry, POLICY_GLYPH_X,
                                 POLICY_FIRST_ROW + index, POLICY_LABELS[index])
    press_ok(xwpe)
    assert xwpe.proc.poll() is None, "xwpe died in the AI settings dialog"
    assert_xwperc_has(xwpe, "AIPolicy", POLICY_NAMES[index], clicked_at, geometry)


# @scenario SC-ai-options-45x
@needs_ai_build
@pytest.mark.skipif(not HAS_AGENT_RADIO,
                    reason="xwpe built without --enable-ai-agent-host: the dialog "
                           "has no Agent engine radio")
@ai_session
def test_agent_engine_label_click_applies_on_ok(xwpe):
    """Click the end of the "Claude Code" label of the Agent engine radio, Ok:
    xwperc says AIAgentEngine : claude-code (the session starts on builtin)."""
    geometry = open_ai_options(xwpe)
    clicked_at = click_label_end(xwpe, geometry, AGENT_CLAUDE_CODE_GLYPH_X,
                                 AGENT_ROW, AGENT_CLAUDE_CODE_LABEL)
    press_ok(xwpe)
    assert xwpe.proc.poll() is None, "xwpe died in the AI settings dialog"
    assert_xwperc_has(xwpe, "AIAgentEngine", "claude-code", clicked_at, geometry)
