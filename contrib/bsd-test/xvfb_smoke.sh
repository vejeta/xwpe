#!/bin/sh
# Headless X11 smoke test for xwpe, shared by all three BSD provisioners
# (like sgr_mouse_probe.c is the shared console-mouse probe).
#
# Runs the built xwpe under Xvfb + a tiny window manager, captures its window
# through xwpe's own XWPE_X_DUMP hook (a PPM -- no external screenshot tool), and
# asserts it painted a real, multi-colour UI rather than a blank/black frame.
# This is the X11 analogue of driving `wpe` under a pty: it proves the X11 build
# actually RENDERS on the platform, not merely that it links.
#
# Usage: xvfb_smoke.sh /path/to/build-tree   (the dir holding the xwpe binary)
# Exits 0 = PASS or SKIP (tooling absent), 1 = FAIL (ran but did not paint).
#        xvfb_smoke.sh --selftest   (checks the PASS rule on generated frames)
set -u

D="${XWPE_SMOKE_DISPLAY:-:97}"
PPM="/tmp/xwpe-xsmoke.ppm"
XLOG=/tmp/xsmoke-xwpe.log

have() { command -v "$1" >/dev/null 2>&1; }

# ppm_check FILE | --selftest
#
# PASS rule: a painted UI, not a blank frame, independent of antialiasing.
#   1. at least 3 distinct colours (background, text, and a bar);
#   2. no single colour covers more than 95% of the pixels;
#   3. the frame is at least 480x144 px (80x24 cells of 6 px). The width is
#      not tied to a column count: a window manager may maximize the window
#      or resize it, and the dump is clamped to the screen;
#   4. the top band, the middle band and the bottom band (3% of the rows
#      each) must each hold more than one colour. A half-painted frame or
#      solid stripes fail here; a real frame has a menu row at the top, the
#      editor in the middle and a status row at the bottom.
# The flat core-font painter (no Xft font, e.g. FreeBSD without fonts) uses
# about 5 solid colours and passes. A black frame (1 colour) and a frame of
# background plus cursor (2 colours) fail. The count is not capped.
ppm_check() {
python3 - "$@" <<'PY'
import sys, os, tempfile

MIN_COLOURS = 3
MAX_SHARE = 0.95
MIN_W = 80 * 6
MIN_H = 24 * 6
BAND_SHARE = 0.03

def read_ppm(path):
    d = open(path, "rb").read()
    if d[:2] != b"P6":
        return None
    i = 2; tok = []
    while len(tok) < 3:
        while d[i] in b" \t\n\r": i += 1
        s = i
        while d[i] not in b" \t\n\r": i += 1
        tok.append(d[s:i])
    i += 1
    return int(tok[0]), int(tok[1]), d[i:]

def judge(w, h, px):
    counts = {}
    for p in range(0, w * h * 3, 3):
        c = px[p:p+3]
        counts[c] = counts.get(c, 0) + 1
    n = len(counts)
    total = w * h
    share = max(counts.values()) / total if total else 1.0
    why = []
    if n < MIN_COLOURS: why.append("only %d colours" % n)
    if share > MAX_SHARE: why.append("one colour covers %.1f%%" % (share * 100))
    if w < MIN_W or h < MIN_H:
        why.append("frame %dx%d is smaller than %dx%d" % (w, h, MIN_W, MIN_H))
    else:
        why.extend(band_problems(w, h, px))
    return not why, n, share, why

def band_colours(w, px, first_row, rows):
    start = first_row * w * 3
    end = (first_row + rows) * w * 3
    return len(set(px[p:p+3] for p in range(start, end, 3)))

def band_problems(w, h, px):
    rows = max(1, int(h * BAND_SHARE))
    bands = (("top", 0), ("middle", h // 2 - rows // 2), ("bottom", h - rows))
    why = []
    for name, first in bands:
        if band_colours(w, px, first, rows) < 2:
            why.append("%s band is one solid colour" % name)
    return why

def check(path):
    r = read_ppm(path)
    if r is None:
        print("xvfb-smoke: FAIL (dump is not a PPM)"); return False
    w, h, px = r
    ok, n, share, why = judge(w, h, px)
    print("xvfb-smoke: %s (%dx%d, %d colours, top colour %.1f%%%s)"
          % ("PASS" if ok else "FAIL", w, h, n, share * 100,
             "" if ok else "; " + ", ".join(why)))
    return ok

def make_row(w, spans):
    """spans: list of (rgb, fraction of the width); the last takes the rest."""
    row = b""
    for k, (rgb, frac) in enumerate(spans):
        n = w - len(row) // 3 if k == len(spans) - 1 else int(w * frac)
        row += bytes(rgb) * n
    return row

def make(path, w, h, bands):
    """bands: list of (fraction of rows, spans); the last takes the rest."""
    rows = []; used = 0
    for k, (frac, spans) in enumerate(bands):
        n = h - used if k == len(bands) - 1 else int(h * frac)
        rows.append(make_row(w, spans) * n); used += n
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h) + b"".join(rows))

BLACK = (0, 0, 0); WHITE = (255, 255, 255); GREY = (170, 170, 170)
BLUE = (0, 0, 170); CYAN = (0, 170, 170)

# A flat frame: menu row, body (background, window border, scrollbar), status row.
MENU = (.04, [(GREY, .9), (WHITE, .1)])
STATUS = (.04, [(GREY, .85), (WHITE, .15)])
BODY = (.92, [(BLACK, .95), (BLUE, .03), (CYAN, .02)])
FLAT = [MENU, BODY, STATUS]
# An antialiased frame: many shades in every band.
SHADES = [((i * 6, i * 5, 255 - i * 6), 1.0 / 40) for i in range(40)]
CAIRO = [(.04, SHADES), (.92, SHADES), (.04, SHADES)]
HALF = [MENU, (.46, [(BLACK, .95), (BLUE, .05)]), (.5, [(BLACK, 1.0)])]
STRIPES = [(.33, [((200, 0, 0), 1.0)]), (.33, [((0, 200, 0), 1.0)]),
           (.34, [((0, 0, 200), 1.0)])]

def selftest():
    tmp = tempfile.mkdtemp(prefix="xsmoke-self.")
    cases = [
        ("flat-5-colours-640x572", True, 640, 572, FLAT),
        ("wm-maximized-1024x768", True, 1024, 768, FLAT),
        ("81-columns-648x572", True, 648, 572, FLAT),
        ("cairo-many-colours", True, 640, 572, CAIRO),
        ("black-1-colour", False, 640, 572, [(1.0, [(BLACK, 1.0)])]),
        ("bg-plus-cursor-2-colours", False, 640, 572,
         [(1.0, [(BLACK, .99), (WHITE, .01)])]),
        ("half-painted", False, 640, 572, HALF),
        ("three-solid-stripes", False, 640, 572, STRIPES),
    ]
    bad = 0
    try:
        for name, want, w, h, bands in cases:
            p = os.path.join(tmp, name + ".ppm")
            make(p, w, h, bands)
            got = check(p)
            print("selftest %s: expected %s, got %s -> %s"
                  % (name, "PASS" if want else "FAIL", "PASS" if got else "FAIL",
                     "ok" if got == want else "WRONG"))
            if got != want: bad += 1
    finally:
        for fn in os.listdir(tmp): os.remove(os.path.join(tmp, fn))
        os.rmdir(tmp)
    return bad == 0

if sys.argv[1] == "--selftest":
    sys.exit(0 if selftest() else 1)
sys.exit(0 if check(sys.argv[1]) else 1)
PY
}

# print_xwpe_log: always show xwpe's own stderr and which painter ran, so a
# failure explains itself.
print_xwpe_log() {
  echo "xvfb-smoke: --- last lines of $XLOG ---"
  tail -n 10 "$XLOG" 2>/dev/null
  echo "xvfb-smoke: --- end of log ---"
  if grep -q "Xft font init failed" "$XLOG" 2>/dev/null; then
    echo "xvfb-smoke: painter: FLAT core X font (Xft font init failed; no fontconfig font installed?)"
  elif build_has_xft_and_cairo; then
    echo "xvfb-smoke: painter: Xft/cairo (build has both; no Xft failure message in the log)"
  else
    echo "xvfb-smoke: painter: no Xft in this build (core X font path)"
  fi
}

# build_has_xft_and_cairo: true when config.h defines HAVE_XFT and HAVE_CAIRO,
# or, without a config.h, when the binary links libXft and libcairo.
build_has_xft_and_cairo() {
  if [ -f "$BUILDDIR/config.h" ]; then
    grep -q '^#define HAVE_XFT 1' "$BUILDDIR/config.h" &&
      grep -q '^#define HAVE_CAIRO 1' "$BUILDDIR/config.h"
    return $?
  fi
  have ldd || return 1
  _libs=$(ldd "$XWPE" 2>/dev/null)
  echo "$_libs" | grep -q 'libXft' && echo "$_libs" | grep -q 'libcairo'
}

if [ "${1:-}" = "--selftest" ]; then
  ppm_check --selftest
  exit $?
fi

BUILDDIR="${1:?usage: xvfb_smoke.sh /path/to/build-tree | --selftest}"

# The mode is chosen by argv[0]: a name starting with 'x' selects X11.  Use the
# xwpe symlink if the build made one, else create it next to the binary.
XWPE="$BUILDDIR/xwpe"
if [ ! -e "$XWPE" ]; then
  [ -x "$BUILDDIR/we" ] && ln -sf we "$XWPE" 2>/dev/null
fi
[ -x "$XWPE" ] || { echo "xvfb-smoke: SKIP (no xwpe binary in $BUILDDIR)"; exit 0; }

for t in Xvfb xdpyinfo python3; do
  have "$t" || { echo "xvfb-smoke: SKIP (missing $t)"; exit 0; }
done
WM=""
for w in matchbox-window-manager twm; do
  have "$w" && { WM="$w"; break; }
done
[ -n "$WM" ] || { echo "xvfb-smoke: SKIP (no matchbox/twm window manager)"; exit 0; }

rm -f "$PPM" "$PPM.tmp" "/tmp/.X${D#:}-lock"
Xvfb "$D" -screen 0 1024x768x24 >/tmp/xsmoke-xvfb.log 2>&1 &
XVFB=$!
i=0
while [ "$i" -lt 25 ]; do
  DISPLAY="$D" xdpyinfo >/dev/null 2>&1 && break
  sleep 0.3; i=$((i + 1))
done
if ! DISPLAY="$D" xdpyinfo >/dev/null 2>&1; then
  echo "xvfb-smoke: FAIL (Xvfb did not come up on $D)"
  kill "$XVFB" 2>/dev/null
  exit 1
fi

if [ "$WM" = "matchbox-window-manager" ]; then
  DISPLAY="$D" "$WM" -use_titlebar no >/dev/null 2>&1 &
else
  # twm asks for interactive window placement by default (a rubber-band that
  # needs a mouse click), which never happens headless -- the window would stay
  # unmapped.  RandomPlacement makes it map windows immediately at a fixed spot.
  TWMRC=/tmp/xsmoke-twmrc
  printf 'NoTitle\nRandomPlacement\nNoGrabServer\n' > "$TWMRC"
  DISPLAY="$D" "$WM" -f "$TWMRC" >/dev/null 2>&1 &
fi
WMPID=$!
sleep 1

mkdir -p /tmp/xsmoke-home
printf 'int main(void){return 0;}\n' > /tmp/xsmoke-home/t.c
# This is an X11 test: xwpe picks the Wayland backend when WAYLAND_DISPLAY is
# set, so force X11 and hide the Wayland socket from the child.
# exec keeps $! the PID of xwpe itself.
(
  unset WAYLAND_DISPLAY
  XWPE_BACKEND=x11; export XWPE_BACKEND
  XWPE_LIB="$BUILDDIR"; HOME=/tmp/xsmoke-home; XWPE_FONT_SIZE=10
  XWPE_X_DUMP="$PPM"; DISPLAY="$D"
  export XWPE_LIB HOME XWPE_FONT_SIZE XWPE_X_DUMP DISPLAY
  exec "$XWPE" /tmp/xsmoke-home/t.c >/tmp/xsmoke-xwpe.log 2>&1
) &
XW=$!
sleep 1
# A window manager may place the window partly off-screen, which makes xwpe's
# XGetImage-based dump fail (BadMatch on a not-fully-viewable window).  If
# xdotool is available, move the window to 0,0 so it is fully on-screen.
if command -v xdotool >/dev/null 2>&1; then
  _wid=$(DISPLAY="$D" xdotool search --class xwpe 2>/dev/null | tail -1)
  if [ -n "$_wid" ]; then
    DISPLAY="$D" xdotool windowmove "$_wid" 0 0 2>/dev/null
    # A headless X server with no usable fonts can leave xwpe with a degenerate
    # font metric and thus an absurdly wide window (thousands of px).  Such a
    # window is wider than the 1024px screen, so it can never be fully on-screen
    # and XGetImage cannot capture it -- an environment limitation, not an xwpe
    # fault.  Skip rather than report a false failure (seen on OpenBSD/Xvfb).
    _ww=$(DISPLAY="$D" xdotool getwindowgeometry "$_wid" 2>/dev/null \
          | awk '/Geometry/ {split($2, a, "x"); print a[1]}')
    if [ -n "$_ww" ] && [ "$_ww" -gt 1024 ]; then
      echo "xvfb-smoke: SKIP (window ${_ww}px wider than the 1024px screen -- headless font metric; cannot capture)"
      kill "$XW" "$WMPID" "$XVFB" 2>/dev/null
      exit 0
    fi
  fi
fi
sleep 3
kill "$XW" 2>/dev/null
sleep 0.6
kill "$WMPID" "$XVFB" 2>/dev/null

print_xwpe_log

if [ ! -f "$PPM" ]; then
  echo "xvfb-smoke: FAIL (xwpe produced no XWPE_X_DUMP frame -- console fallback?)"
  exit 1
fi

ppm_check "$PPM"
exit $?
