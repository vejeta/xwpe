#!/bin/sh
# Burn per-action key captions onto a demo GIF, reproducibly, from a .cue file.
#
# VHS cannot overlay the pressed keys itself, so this is a post-process pass:
# ffmpeg drawtext paints a lower-third caption (the key chord + what it does,
# e.g. "Alt-Q H   Hover") during each action's time window.  The cue file lives
# next to the tapes so the overlay is reproducible and easy to retime.
#
# Usage:
#   docs/demos/captions.sh <in.gif> <cue-file> [out.gif]
# (out.gif defaults to in.gif, i.e. caption in place.)
#
# Cue format -- one action per line, '#' comments and blank lines ignored:
#   START  DURATION  LABEL TEXT...
# START/DURATION are seconds (floats ok).  Example:
#   4.9   2.6   Alt-Q H   Hover -- type of the symbol
#
# Requires: ffmpeg with drawtext (libfreetype).  Picks a common monospace font.

set -e
in=$1; cue=$2; out=${3:-$1}
[ -n "$in" ] && [ -n "$cue" ] || { echo "usage: captions.sh <in.gif> <cue> [out.gif]" >&2; exit 2; }
[ -f "$in" ]  || { echo "captions.sh: no gif: $in" >&2; exit 1; }
[ -f "$cue" ] || { echo "captions.sh: no cue: $cue" >&2; exit 1; }

# A monospace font that ships almost everywhere; override with CAPTION_FONT.
font=${CAPTION_FONT:-/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf}
[ -f "$font" ] || font=$(fc-match -f '%{file}' monospace 2>/dev/null || true)
[ -n "$font" ] && [ -f "$font" ] || { echo "captions.sh: no usable font; set CAPTION_FONT" >&2; exit 1; }

# Build one drawtext per cue line.  The caption is a pill near the bottom centre:
# bold white text on a translucent dark box, shown only between START and END.
filters=""
while read -r start dur rest; do
  case "$start" in ''|'#'*) continue;; esac
  [ -n "$rest" ] || continue
  end=$(awk "BEGIN{printf \"%.3f\", $start + $dur}")
  # escape ffmpeg drawtext metachars in the label
  txt=$(printf '%s' "$rest" | sed -e "s/\\\\/\\\\\\\\/g" -e "s/:/\\\\:/g" -e "s/'/\\\\\\\\'/g")
  d="drawtext=fontfile='$font':text='$txt':fontcolor=white:fontsize=22"
  d="$d:box=1:boxcolor=0x000000AA:boxborderw=12:x=(w-text_w)/2:y=h-text_h-28"
  d="$d:enable='between(t,$start,$end)'"
  if [ -z "$filters" ]; then filters="$d"; else filters="$filters,$d"; fi
done < "$cue"

[ -n "$filters" ] || { echo "captions.sh: cue produced no captions" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# drawtext, then regenerate a palette so the GIF stays small and clean.
ffmpeg -v error -i "$in" -vf "$filters,split[a][b];[a]palettegen=stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=3" -y "$tmp/out.gif"
mv "$tmp/out.gif" "$out"
echo "captioned -> $out"
