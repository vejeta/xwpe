"""The editor paints its own colours on an 8-bit palette display.

On a palette (PseudoColor) display a pixel is an index into a colormap, and
only the cells the editor allocated hold its 16 colours.  The rows start
their own 8-bit X server, with no window manager, run the editor on it with
and without a private colormap (-pcmap), and look at the window's pixels:
the desk must come out in the editor's Dark Slate Blue, and the window must
not be mostly black.

Without a window manager nobody installs a private colormap, so the screen
itself keeps showing the default one.  The capture is taken from the window
and carries the window's own colormap, so the row checks the pixels the
editor wrote decoded through the colormap it allocated them in, not the
colours the screen happens to show.
"""
import os
import re
import select
import sys
import time

import pytest

TESTS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if TESTS not in sys.path:
    sys.path.insert(0, TESTS)

import gui_owner as owner_api  # noqa: E402
from gui_owner import gui_owner, wait_for  # noqa: E402,F401  (the fixture)

import conftest  # noqa: E402

SCREEN_8_BIT = "1024x768x8"
TRACE = "ui.trace"
CAPTURE = "window.xwd"
FRAME = "frame.ppm"
SOURCE = "t.c"
FILLER = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      "colormap_filler.py")
# The editor's desk colour, Dark Slate Blue, and how far a channel may be off.
DESK_RGB = (72, 61, 139)
DESK_TOLERANCE = 6
# How far the desk may be off when the editor took the nearest cell of a
# full colormap instead of its own colour.
DESK_NEAR_TOLERANCE = 32
ALLOC_LINE = re.compile(r"^palette alloc idx=(\d+) level=(\d+)$", re.M)
LEVEL_EXACT, LEVEL_NEAREST, LEVEL_PRIVATE = 1, 2, 3
PALETTE_SIZE = 16
# Red3, the first of the editor's colours a bare 8-bit server holds nothing
# close to.
FIRST_FAR_ENTRY = 1
COLORMAP_LINE = re.compile(r"Colormap:\s*(0x[0-9a-f]+)")
ROUTE_LINE = "palette route="
REFUSED_LINE = "palette image refused"
MADE_LINE = "palette image made"
UNKNOWN_COLOUR = "No Such Colour"
PRESENT_WORD = "palette present"
OUT_OF_MEMORY = "out of memory for a"
# The test fault that refuses the indexed route's second picture memory:
# the one the first resize asks for, which the window gets right after it
# is mapped, and which is followed by a refresh.
FAULT_RESIZE_IMAGE = "palette-image:2"
# The size the row resizes the window to, in pixels.
RESIZED_W, RESIZED_H = "720", "520"
# A pixel counts as black when every channel is below this.
BLACK_BELOW = 16
DESK_MIN_SHARE = 0.25
BLACK_MAX_SHARE = 0.40
ROUTE_INDEXED = "palette route=indexed"
PRESENT_LINE = re.compile(r"^palette present rects=\d+ bytes=(\d+)$", re.M)
# The editor's window as large as the screen, and a point inside it.  The
# geometry counts cells: at the 10-point font the owner pins a cell is 8x17
# pixels, so 127x44 cells are 1016x748, which with the window's border still
# lies wholly on the screen (the editor's own frame dump, which the rows wait
# on, needs that).  The row checks the window really came out that wide
# before it measures anything.
FULL_SCREEN_GEOMETRY = "127x44+0+0"
FULL_SCREEN_MIN_WIDTH = 960
POINTER_X, POINTER_Y = "512", "384"
GEOMETRY_LINE = re.compile(r"Geometry:\s*(\d+)x(\d+)")
# What one typed character may send: a few cells and the scrollbars beside
# them, far below the whole window.
KEYSTROKE_MAX_BYTES = 64 * 1024
# Bounds of the waits, before XWPE_TEST_WAIT_SCALE.
SERVER_START_S = 15.0
WINDOW_S = 20.0
FRAME_S = 20.0
CLIENT_S = 15.0
# The picture counts as finished when no frame was written for this long.
FRAME_QUIET_S = 1.0


def _display_number(fd, chunks):
    """The display number the server wrote to fd, once its line is whole."""
    if not select.select([fd], [], [], 0)[0]:
        return None
    data = os.read(fd, 64)
    chunks.append(data)
    text = b"".join(chunks)
    if not data or not text.endswith(b"\n"):
        return None
    return int(text.strip())


def _start_8_bit_display(owner, tmp_path, request):
    """An 8-bit Xvfb of the row's own, on the display number it picks."""
    read_fd, write_fd = os.pipe()
    try:
        server = owner.start_service(
            ["Xvfb", "-displayfd", str(write_fd), "-screen", "0",
             SCREEN_8_BIT, "-nolisten", "tcp"],
            "8-bit Xvfb for %s" % request.node.nodeid, str(tmp_path),
            str(tmp_path), kind="server", request=request,
            pass_fds=(write_fd,))
    finally:
        os.close(write_fd)
    chunks = []
    try:
        number = wait_for(lambda: _display_number(read_fd, chunks),
                          "the 8-bit Xvfb to print its display number",
                          SERVER_START_S, state_fn=server.log_text)
    finally:
        os.close(read_fd)
    return owner.register_x_display(":%d" % number, server)


def _run_client(owner, display, tmp_path, request, argv, what):
    """Run one X client on the row's display to its end; return it."""
    client = owner_api.Client.start(owner, argv, display, str(tmp_path),
                                    str(tmp_path), label=request.node.nodeid,
                                    request=request)
    wait_for(lambda: not client.running(), what, CLIENT_S,
             state_fn=client.log_text)
    client.end()
    return client


def _editor_window(owner, display, tmp_path, request):
    """The id of the editor's window, once it exists."""
    found = _run_client(owner, display, tmp_path, request,
                        ["xdotool", "search", "--sync", "--classname",
                         conftest.WINDOW_CLASS],
                        "xdotool to find the editor's window")
    ids = [word for word in found.log_text().split() if word.isdigit()]
    assert ids, "no editor window: %r" % found.log_text()
    return ids[0]


def _frame_settled(path, last):
    """True once the editor has written a frame and then none for a while."""
    try:
        stamp = os.stat(path).st_mtime_ns
    except OSError:
        return False
    now = time.monotonic()
    if last.get("stamp") != stamp:
        last["stamp"] = stamp
        last["since"] = now
        return False
    return now - last["since"] >= FRAME_QUIET_S


def _capture(owner, display, tmp_path, request, window):
    """The editor's window as an RGB image, decoded through its colormap."""
    path = tmp_path / CAPTURE
    _run_client(owner, display, tmp_path, request,
                ["xwd", "-id", window, "-nobdrs", "-silent", "-out", str(path)],
                "xwd to capture the editor's window")
    return conftest.xwd_to_image(path.read_bytes())


def _is_desk(rgb, tolerance=DESK_TOLERANCE):
    return all(abs(c - d) <= tolerance for c, d in zip(rgb, DESK_RGB))


def _is_black(rgb):
    return all(c < BLACK_BELOW for c in rgb)


def _shares(image, desk_tolerance=DESK_TOLERANCE):
    """The share of desk pixels and of black pixels in image."""
    raw = image.tobytes()
    pixels = [tuple(raw[i:i + 3]) for i in range(0, len(raw), 3)]
    desk = sum(1 for rgb in pixels if _is_desk(rgb, desk_tolerance))
    black = sum(1 for rgb in pixels if _is_black(rgb))
    return desk / len(pixels), black / len(pixels)


def _trace(tmp_path):
    try:
        return (tmp_path / TRACE).read_text()
    except OSError:
        return ""


def _start_editor(owner, display, tmp_path, request, options, extra_env=None):
    """The editor on the row's display, tracing to TRACE."""
    src = tmp_path / SOURCE
    src.write_text("int main(void){return 0;}\n")
    env = {"XWPE_UI_TRACE": str(tmp_path / TRACE)}
    env.update(extra_env or {})
    return owner_api.Editor.start(
        owner, conftest.XWPE_BIN, options + [str(src)], "x11", display,
        str(tmp_path), str(tmp_path), extra_env=env,
        label=request.node.nodeid, request=request)


def _start_filler(owner, display, tmp_path, request, mode):
    """The colormap filler on the row's display, once the colormap is full.

    It keeps its cells until the row ends it."""
    filler = owner_api.Client.start(
        owner, [sys.executable, "-I", FILLER, mode], display, str(tmp_path),
        str(tmp_path), label=request.node.nodeid, request=request)
    wait_for(lambda: "filler:" in filler.log_text() or not filler.running(),
             "the filler to fill the default colormap", CLIENT_S,
             state_fn=filler.log_text)
    assert filler.running() and "filler:" in filler.log_text(), \
        "the filler did not fill the colormap: %r" % filler.log_text()
    return filler


def _wait_started_or_ended(editor, tmp_path):
    """Wait until the editor has chosen its colours, or has ended."""
    wait_for(lambda: ROUTE_LINE in _trace(tmp_path) or not editor.running(),
             "the editor to allocate its colours or end", WINDOW_S,
             state_fn=editor.log_text)


def _alloc_levels(trace):
    """The level each palette entry got its cell at, by entry."""
    return {int(m.group(1)): int(m.group(2)) for m in ALLOC_LINE.finditer(trace)}


def _colormap_of(owner, display, tmp_path, request, target):
    """The colormap xwininfo reports for a window (or "-root")."""
    argv = ["xwininfo", "-root"] if target == "-root" else \
        ["xwininfo", "-id", target]
    info = _run_client(owner, display, tmp_path, request, argv,
                       "xwininfo to report a colormap")
    found = COLORMAP_LINE.search(info.log_text())
    assert found, "no colormap in %r" % info.log_text()
    return found.group(1)


def _assert_alive_after_start(editor, tmp_path):
    _wait_started_or_ended(editor, tmp_path)
    assert editor.running(), "the editor ended: %s" % editor.log_text()


def _wait_picture_settled(editor, tmp_path, what):
    """Wait until the editor has painted and then stopped painting, or has
    ended (the row's own assertion then reports how it ended)."""
    frame = os.path.join(str(tmp_path), FRAME)
    last = {}
    wait_for(lambda: _frame_settled(frame, last) or not editor.running(),
             what, FRAME_S, state_fn=editor.log_text)


def _presented_bytes(trace):
    """The bytes each refresh in trace sent to the window, in order."""
    return [int(m.group(1)) for m in PRESENT_LINE.finditer(trace)]


# @scenario SC-display-05
@pytest.mark.parametrize("options", [["-pcmap"], []],
                         ids=["pcmap_on", "pcmap_off"])
def test_the_editor_paints_its_own_colours_on_an_8_bit_display(
        gui_owner, tmp_path, request, options):
    display = _start_8_bit_display(gui_owner, tmp_path, request)
    editor = _start_editor(gui_owner, display, tmp_path, request, options)
    window = _editor_window(gui_owner, display, tmp_path, request)
    _wait_picture_settled(editor, tmp_path,
                          "the editor to finish painting its first picture")
    image = _capture(gui_owner, display, tmp_path, request, window)
    desk, black = _shares(image)

    shares = "desk %.1f%%, black %.1f%%" % (100 * desk, 100 * black)

    assert editor.running(), "the editor ended: %s" % editor.log_text()
    assert desk >= DESK_MIN_SHARE, \
        "too little of the window is the desk colour %r: %s" % (DESK_RGB, shares)
    assert black <= BLACK_MAX_SHARE, "the window is mostly black: %s" % shares
    assert ROUTE_INDEXED in _trace(tmp_path), \
        "the palette display did not get the indexed route: %r" % _trace(tmp_path)


# @scenario SC-display-05
def test_a_typed_character_sends_a_few_cells_on_an_8_bit_display(
        gui_owner, tmp_path, request):
    """Typing over a slow link to a palette X terminal stays quick: one
    character in a full-screen edit window sends the cells it changed, not
    the whole 1024x768 window (786 KB at one byte a pixel)."""
    display = _start_8_bit_display(gui_owner, tmp_path, request)
    editor = _start_editor(gui_owner, display, tmp_path, request,
                           ["-geometry", FULL_SCREEN_GEOMETRY])
    window = _editor_window(gui_owner, display, tmp_path, request)
    geometry = _run_client(gui_owner, display, tmp_path, request,
                           ["xdotool", "getwindowgeometry", window],
                           "xdotool to measure the editor's window")
    size = GEOMETRY_LINE.search(geometry.log_text())
    assert size and int(size.group(1)) >= FULL_SCREEN_MIN_WIDTH, \
        "the window is not full-screen: %r" % geometry.log_text()
    _run_client(gui_owner, display, tmp_path, request,
                ["xdotool", "mousemove", POINTER_X, POINTER_Y],
                "xdotool to put the pointer on the editor")
    _wait_picture_settled(editor, tmp_path,
                          "the editor to finish painting its first picture")
    before = len(_presented_bytes(_trace(tmp_path)))
    _run_client(gui_owner, display, tmp_path, request,
                ["xdotool", "key", "x"], "xdotool to type one character")
    wait_for(lambda: len(_presented_bytes(_trace(tmp_path))) > before,
             "the editor to send the typed character", FRAME_S,
             state_fn=lambda: _trace(tmp_path))
    _wait_picture_settled(editor, tmp_path,
                          "the editor to finish painting the character")
    sent = sum(_presented_bytes(_trace(tmp_path))[before:])

    assert editor.running(), "the editor ended: %s" % editor.log_text()
    assert sent < KEYSTROKE_MAX_BYTES, \
        "one typed character sent %d bytes to the window" % sent


# @scenario SC-display-07
def test_a_full_default_colormap_does_not_end_the_editor(
        gui_owner, tmp_path, request):
    """Another program holds every free cell of the default colormap
    writable, as a busy 8-bit desktop does.  The original editor ended
    with "all colorcells allocated".  Black is still there to share, but
    nothing near Red3 is, so the editor moves to a colormap of its own
    once, at Red3, and allocates Red3 and every later colour exactly
    there; the desk is its own Dark Slate Blue."""
    display = _start_8_bit_display(gui_owner, tmp_path, request)
    _start_filler(gui_owner, display, tmp_path, request, "full")
    editor = _start_editor(gui_owner, display, tmp_path, request, [])
    _assert_alive_after_start(editor, tmp_path)
    window = _editor_window(gui_owner, display, tmp_path, request)
    _wait_picture_settled(editor, tmp_path,
                          "the editor to finish painting its first picture")
    desk, black = _shares(_capture(gui_owner, display, tmp_path, request,
                                   window))
    levels = _alloc_levels(_trace(tmp_path))
    window_cmap = _colormap_of(gui_owner, display, tmp_path, request, window)
    root_cmap = _colormap_of(gui_owner, display, tmp_path, request, "-root")

    assert editor.running(), "the editor ended: %s" % editor.log_text()
    assert sorted(levels) == list(range(PALETTE_SIZE)), levels
    assert [i for i, lv in levels.items() if lv == LEVEL_PRIVATE] == \
        [FIRST_FAR_ENTRY], levels
    assert all(lv == LEVEL_EXACT for i, lv in levels.items()
               if i != FIRST_FAR_ENTRY), levels
    assert window_cmap != root_cmap, \
        "the window kept the full default colormap %s" % root_cmap
    assert desk >= DESK_MIN_SHARE, \
        "too little of the window is the desk colour: desk %.1f%%, black %.1f%%" \
        % (100 * desk, 100 * black)
    assert editor.log_text().count("using a private one") == 1, \
        editor.log_text()


# @scenario SC-display-07
def test_a_full_colormap_of_near_colours_keeps_the_default_one(
        gui_owner, tmp_path, request):
    """The default colormap is full, but holds, for each of the editor's
    colours it lacks, one colour a little off it.  The editor takes those
    nearest cells and stays in the screen's colormap, so no colormap
    flash: the desk is within a small distance of Dark Slate Blue."""
    display = _start_8_bit_display(gui_owner, tmp_path, request)
    _start_filler(gui_owner, display, tmp_path, request, "near")
    editor = _start_editor(gui_owner, display, tmp_path, request, [])
    _assert_alive_after_start(editor, tmp_path)
    window = _editor_window(gui_owner, display, tmp_path, request)
    _wait_picture_settled(editor, tmp_path,
                          "the editor to finish painting its first picture")
    desk, black = _shares(_capture(gui_owner, display, tmp_path, request,
                                   window), DESK_NEAR_TOLERANCE)
    levels = _alloc_levels(_trace(tmp_path))
    window_cmap = _colormap_of(gui_owner, display, tmp_path, request, window)
    root_cmap = _colormap_of(gui_owner, display, tmp_path, request, "-root")

    assert editor.running(), "the editor ended: %s" % editor.log_text()
    assert sorted(levels) == list(range(PALETTE_SIZE)), levels
    assert LEVEL_PRIVATE not in levels.values(), levels
    assert LEVEL_NEAREST in levels.values(), levels
    assert window_cmap == root_cmap, \
        "the window left the default colormap: %s, root %s" \
        % (window_cmap, root_cmap)
    assert desk >= DESK_MIN_SHARE, \
        "too little of the window is near the desk colour: desk %.1f%%, " \
        "black %.1f%%" % (100 * desk, 100 * black)


# @scenario SC-display-05
@pytest.mark.parametrize("options", [["-pcmap"], []],
                         ids=["pcmap_on", "pcmap_off"])
def test_the_frame_dump_on_an_8_bit_display_is_what_the_window_shows(
        gui_owner, tmp_path, request, options):
    """The editor's own frame dump (XWPE_X_DUMP), which the graphical rows
    read, decodes a palette window through the window's colormap: it is
    pixel for pixel what xwd captures of the window."""
    from PIL import Image

    display = _start_8_bit_display(gui_owner, tmp_path, request)
    editor = _start_editor(gui_owner, display, tmp_path, request, options)
    window = _editor_window(gui_owner, display, tmp_path, request)
    _wait_picture_settled(editor, tmp_path,
                          "the editor to finish painting its first picture")
    captured = _capture(gui_owner, display, tmp_path, request, window)
    dumped = Image.open(str(tmp_path / FRAME)).convert("RGB")
    a, b = captured.tobytes(), dumped.tobytes()
    differing = sum(1 for i in range(0, min(len(a), len(b)), 3)
                    if a[i:i + 3] != b[i:i + 3])

    assert editor.running(), "the editor ended: %s" % editor.log_text()
    assert dumped.size == captured.size, (dumped.size, captured.size)
    assert differing == 0, "%d of %d pixels differ between the dump and xwd" \
        % (differing, len(a) // 3)


# @scenario SC-display-05
def test_a_window_without_picture_memory_stays_alive_and_draws_after_a_resize(
        gui_owner, tmp_path, request):
    """When there is no memory for the picture the indexed route sends, the
    editor keeps running, draws nothing, says so once on stderr, and the
    next resize makes the picture again and draws it."""
    display = _start_8_bit_display(gui_owner, tmp_path, request)
    editor = _start_editor(gui_owner, display, tmp_path, request, [],
                           {"XWPE_TEST_FAULT": FAULT_RESIZE_IMAGE})
    _assert_alive_after_start(editor, tmp_path)
    window = _editor_window(gui_owner, display, tmp_path, request)
    wait_for(lambda: REFUSED_LINE in _trace(tmp_path) or not editor.running(),
             "the editor to report the refused picture memory", FRAME_S,
             state_fn=editor.log_text)
    _wait_picture_settled(editor, tmp_path,
                          "the editor to finish its first refresh")
    assert editor.running(), "the editor ended: %s" % editor.log_text()
    _run_client(gui_owner, display, tmp_path, request,
                ["xdotool", "windowsize", window, RESIZED_W, RESIZED_H],
                "xdotool to resize the editor's window")
    wait_for(lambda: PRESENT_WORD in _after_refusal(_trace(tmp_path))
             or not editor.running(),
             "the editor to draw after a resize", FRAME_S,
             state_fn=lambda: _trace(tmp_path))
    _wait_picture_settled(editor, tmp_path,
                          "the editor to finish painting after the resize")
    without_memory, _, with_memory = \
        _after_refusal(_trace(tmp_path)).partition(MADE_LINE)
    desk, black = _shares(_capture(gui_owner, display, tmp_path, request,
                                   window))

    assert editor.running(), "the editor ended: %s" % editor.log_text()
    assert editor.log_text().count(OUT_OF_MEMORY) == 1, editor.log_text()
    assert PRESENT_WORD not in without_memory, \
        "the editor drew without picture memory: %r" % without_memory
    assert PRESENT_WORD in with_memory, \
        "the editor did not draw once a resize gave it memory: %r" % with_memory
    assert desk >= DESK_MIN_SHARE, \
        "the window was not drawn after the resize: desk %.1f%%, black %.1f%%" \
        % (100 * desk, 100 * black)


# @scenario SC-display-07
def test_an_unknown_colour_name_costs_only_that_colour(
        gui_owner, tmp_path, request):
    """A colour resource the server does not know (here the desk's,
    color5) is reported on stderr and replaced by the built-in colour; the
    original ended the editor with "unable to find color"."""
    (tmp_path / ".Xdefaults").write_text("xwpe.color5: %s\n" % UNKNOWN_COLOUR)
    display = _start_8_bit_display(gui_owner, tmp_path, request)
    editor = _start_editor(gui_owner, display, tmp_path, request, [])
    _assert_alive_after_start(editor, tmp_path)
    window = _editor_window(gui_owner, display, tmp_path, request)
    _wait_picture_settled(editor, tmp_path,
                          "the editor to finish painting its first picture")
    desk, black = _shares(_capture(gui_owner, display, tmp_path, request,
                                   window))

    assert editor.running(), "the editor ended: %s" % editor.log_text()
    assert editor.log_text().count(UNKNOWN_COLOUR) == 1, editor.log_text()
    assert desk >= DESK_MIN_SHARE, \
        "the desk is not the built-in colour: desk %.1f%%, black %.1f%%" \
        % (100 * desk, 100 * black)


def _after_refusal(trace):
    """The trace from the refused picture memory on ("" before it)."""
    _, found, rest = trace.partition(REFUSED_LINE)
    return found + rest
