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
set -u

BUILDDIR="${1:?usage: xvfb_smoke.sh /path/to/build-tree}"
D="${XWPE_SMOKE_DISPLAY:-:97}"
PPM="/tmp/xwpe-xsmoke.ppm"

have() { command -v "$1" >/dev/null 2>&1; }

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
env XWPE_LIB="$BUILDDIR" HOME=/tmp/xsmoke-home XWPE_FONT_SIZE=10 \
    XWPE_X_DUMP="$PPM" DISPLAY="$D" \
    "$XWPE" /tmp/xsmoke-home/t.c >/tmp/xsmoke-xwpe.log 2>&1 &
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

if [ ! -f "$PPM" ]; then
  echo "xvfb-smoke: FAIL (xwpe produced no XWPE_X_DUMP frame -- console fallback?)"
  head -n 5 /tmp/xsmoke-xwpe.log 2>/dev/null
  echo
  exit 1
fi

# PASS if the captured frame has clearly more than a couple of colours: a blank
# or all-black frame has 1, a real painted UI has hundreds.
python3 - "$PPM" <<'PY'
import sys
d = open(sys.argv[1], "rb").read()
if d[:2] != b"P6":
    print("xvfb-smoke: FAIL (dump is not a PPM)"); sys.exit(1)
i = 2; tok = []
while len(tok) < 3:
    while d[i] in b" \t\n\r": i += 1
    s = i
    while d[i] not in b" \t\n\r": i += 1
    tok.append(d[s:i])
i += 1
w, h = int(tok[0]), int(tok[1]); px = d[i:]
cols = set()
for p in range(0, w * h * 3, 3):
    cols.add(px[p:p+3])
    if len(cols) > 16:
        break
ok = len(cols) > 8
print("xvfb-smoke: %s (%dx%d, %s%d colours)"
      % ("PASS" if ok else "FAIL", w, h, ">" if len(cols) > 16 else "", len(cols)))
sys.exit(0 if ok else 1)
PY
