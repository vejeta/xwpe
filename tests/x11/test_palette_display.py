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
SOURCE = "t.c"
# The editor's desk colour, Dark Slate Blue, and how far a channel may be off.
DESK_RGB = (72, 61, 139)
DESK_TOLERANCE = 6
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


def _is_desk(rgb):
    return all(abs(c - d) <= DESK_TOLERANCE for c, d in zip(rgb, DESK_RGB))


def _is_black(rgb):
    return all(c < BLACK_BELOW for c in rgb)


def _shares(image):
    """The share of desk pixels and of black pixels in image."""
    raw = image.tobytes()
    pixels = [tuple(raw[i:i + 3]) for i in range(0, len(raw), 3)]
    desk = sum(1 for rgb in pixels if _is_desk(rgb))
    black = sum(1 for rgb in pixels if _is_black(rgb))
    return desk / len(pixels), black / len(pixels)


def _trace(tmp_path):
    try:
        return (tmp_path / TRACE).read_text()
    except OSError:
        return ""


def _start_editor(owner, display, tmp_path, request, options):
    """The editor on the row's display, tracing to TRACE."""
    src = tmp_path / SOURCE
    src.write_text("int main(void){return 0;}\n")
    return owner_api.Editor.start(
        owner, conftest.XWPE_BIN, options + [str(src)], "x11", display,
        str(tmp_path), str(tmp_path),
        extra_env={"XWPE_UI_TRACE": str(tmp_path / TRACE)},
        label=request.node.nodeid, request=request)


def _wait_picture_settled(editor, tmp_path, what):
    """Wait until the editor has painted and then stopped painting."""
    frame = os.path.join(str(tmp_path), "frame.ppm")
    last = {}
    wait_for(lambda: _frame_settled(frame, last), what, FRAME_S,
             state_fn=editor.log_text)


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
