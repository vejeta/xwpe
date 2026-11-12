"""Fill the default colormap of an 8-bit display, as a busy desktop does.

Run as an X client on the display under test:

    colormap_filler.py full   every free cell taken read/write
    colormap_filler.py near   first, for each of the editor's colours that
                              the colormap does not hold already, one
                              read-only colour 8 to 24 away from it per
                              channel; then every free cell read/write

It prints one line, "filler: <n> cells" ("filler: <n> cells, <m> near" in
the second mode), once the colormap is full, and keeps its connection (so
its cells stay taken) until it is ended with SIGTERM.

Only libX11 is used, through ctypes, so the rows need no extra package.
"""
import ctypes
import ctypes.util
import itertools
import signal
import sys

# The editor's 16 colour names, in its palette order.
PALETTE_NAMES = [
    "Black", "Red3", "Forest Green", "Brown", "Dark Slate Blue",
    "Violet Red", "Turquoise3", "Light Gray", "Dark Slate Grey", "Red1",
    "Green", "Yellow", "Blue", "Violet", "Turquoise1", "White",
]
# A near colour lies this far from its entry in every channel ...
NEAR_STEPS = (8, 16, 24)
# ... and further than this from every other entry in at least one, so the
# editor can only ever take it for the entry it was made for.
NEAREST_MAX = 32
CHANNEL_SHIFT = 8
CHANNEL_MAX = 255
DO_RGB = 1 | 2 | 4


class XColor(ctypes.Structure):
    _fields_ = [("pixel", ctypes.c_ulong), ("red", ctypes.c_ushort),
                ("green", ctypes.c_ushort), ("blue", ctypes.c_ushort),
                ("flags", ctypes.c_char), ("pad", ctypes.c_char)]


def _xlib():
    lib = ctypes.CDLL(ctypes.util.find_library("X11"))
    lib.XOpenDisplay.restype = ctypes.c_void_p
    lib.XOpenDisplay.argtypes = [ctypes.c_char_p]
    lib.XDefaultScreen.argtypes = [ctypes.c_void_p]
    lib.XDefaultColormap.restype = ctypes.c_ulong
    lib.XDefaultColormap.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.XDisplayCells.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.XAllocColorCells.argtypes = [
        ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int,
        ctypes.POINTER(ctypes.c_ulong), ctypes.c_uint,
        ctypes.POINTER(ctypes.c_ulong), ctypes.c_uint]
    lib.XAllocColor.argtypes = [ctypes.c_void_p, ctypes.c_ulong,
                                ctypes.POINTER(XColor)]
    lib.XParseColor.argtypes = [ctypes.c_void_p, ctypes.c_ulong,
                                ctypes.c_char_p, ctypes.POINTER(XColor)]
    lib.XQueryColors.argtypes = [ctypes.c_void_p, ctypes.c_ulong,
                                 ctypes.POINTER(XColor), ctypes.c_int]
    lib.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
    return lib


def _rgb8(color):
    return (color.red >> CHANNEL_SHIFT, color.green >> CHANNEL_SHIFT,
            color.blue >> CHANNEL_SHIFT)


def _within(a, b):
    return all(abs(x - y) <= NEAREST_MAX for x, y in zip(a, b))


def _palette(lib, dpy, cmap):
    """The editor's colours as the server's colour database gives them."""
    out = []
    for name in PALETTE_NAMES:
        color = XColor()
        if not lib.XParseColor(dpy, cmap, name.encode(), ctypes.byref(color)):
            raise SystemExit("filler: the server does not know %r" % name)
        out.append(_rgb8(color))
    return out


def _cells_held(lib, dpy, cmap, ncells):
    """The colours the colormap's cells hold now."""
    cells = (XColor * ncells)()
    for i in range(ncells):
        cells[i].pixel = i
        cells[i].flags = bytes([DO_RGB])
    lib.XQueryColors(dpy, cmap, cells, ncells)
    return {_rgb8(c) for c in cells}


def _offsets():
    """Per-channel offsets 8..24 either way, the smallest first."""
    steps = [s * sign for s in NEAR_STEPS for sign in (1, -1)]
    return sorted(itertools.product(steps, repeat=3),
                  key=lambda o: (max(map(abs, o)), sum(map(abs, o))))


def _near_colour(entry, others):
    """A colour 8..24 from entry per channel and not near any other entry."""
    for offset in _offsets():
        rgb = tuple(c + d for c, d in zip(entry, offset))
        if any(c < 0 or c > CHANNEL_MAX for c in rgb):
            continue
        if any(_within(rgb, other) for other in others):
            continue
        return rgb
    raise SystemExit("filler: no near colour for %r" % (entry,))


def _alloc_near(lib, dpy, cmap, ncells):
    """One read-only near colour for each entry the colormap lacks."""
    palette = _palette(lib, dpy, cmap)
    held = _cells_held(lib, dpy, cmap, ncells)
    made = 0
    for i, entry in enumerate(palette):
        if entry in held:
            continue
        others = [p for j, p in enumerate(palette) if j != i]
        rgb = _near_colour(entry, others)
        color = XColor()
        color.red, color.green, color.blue = (c * 257 for c in rgb)
        color.flags = bytes([DO_RGB])
        if not lib.XAllocColor(dpy, cmap, ctypes.byref(color)):
            raise SystemExit("filler: could not allocate %r" % (rgb,))
        made += 1
    return made


def _fill(lib, dpy, cmap):
    """Take every free cell read/write; return how many."""
    pixel = ctypes.c_ulong()
    taken = 0
    while lib.XAllocColorCells(dpy, cmap, 0, None, 0, ctypes.byref(pixel), 1):
        taken += 1
    return taken


def main(argv):
    mode = argv[1] if len(argv) > 1 else "full"
    if mode not in ("full", "near"):
        raise SystemExit("usage: colormap_filler.py full|near")
    lib = _xlib()
    dpy = lib.XOpenDisplay(None)
    if not dpy:
        raise SystemExit("filler: cannot open the display")
    screen = lib.XDefaultScreen(dpy)
    cmap = lib.XDefaultColormap(dpy, screen)
    ncells = lib.XDisplayCells(dpy, screen)
    near = 0
    if mode == "near":
        near = _alloc_near(lib, dpy, cmap, ncells)
    taken = _fill(lib, dpy, cmap)
    lib.XSync(dpy, 0)
    if mode == "near":
        print("filler: %d cells, %d near" % (taken, near), flush=True)
    else:
        print("filler: %d cells" % taken, flush=True)
    while True:
        signal.pause()


if __name__ == "__main__":
    main(sys.argv)
