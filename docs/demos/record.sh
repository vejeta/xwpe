#!/bin/sh
# Regenerate the xwpe LSP demo GIFs from the .tape scripts in tapes/.
#
# Requirements on PATH:
#   vhs    (charmbracelet/vhs)  -- drives the terminal and renders the GIF
#   ttyd   (tsl0922/ttyd)       -- the headless terminal vhs records
#   ffmpeg                      -- vhs uses it to encode the GIF
# For the Metals-backed tapes (semantic, hover, ...): metals + scala-cli, and
# an already-indexed Scala workspace so the recording is not dominated by the
# ~1 min cold start (the tapes Hide the indexing wait, but it still has to run).
#
# Usage:
#   docs/demos/record.sh [tape ...]      # default: every tapes/*.tape
#   WPE=/path/to/wpe DEMO=/path/to/scala-workspace docs/demos/record.sh menu
#
# WPE  defaults to the ./wpe built in the repo root.
# DEMO defaults to ../scala-demo relative to the repo (override to your own).

set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)

: "${WPE:=$root/wpe}"
: "${DEMO:=$root/../scala-demo}"
export WPE DEMO

if [ ! -x "$WPE" ]; then
  echo "record.sh: no wpe at $WPE -- build it first (make), or set WPE=" >&2
  exit 1
fi
for tool in vhs ttyd ffmpeg; do
  command -v "$tool" >/dev/null 2>&1 || { echo "record.sh: $tool not on PATH" >&2; exit 1; }
done

cd "$root"
mkdir -p docs/demos/gifs

if [ $# -gt 0 ]; then
  set -- "$@"
else
  set -- $(cd "$here/tapes" && ls *.tape | sed 's/\.tape$//')
fi

for name in "$@"; do
  tape="docs/demos/tapes/${name%.tape}.tape"
  [ -f "$tape" ] || { echo "record.sh: no such tape: $tape" >&2; exit 1; }
  echo "== recording $tape (WPE=$WPE DEMO=$DEMO) =="
  vhs "$tape"
done
echo "done -> docs/demos/gifs/"
