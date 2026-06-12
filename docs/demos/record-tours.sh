#!/bin/sh
# Regenerate the per-language LSP "tour" GIFs (docs/demos/gifs/<lang>/tour.gif).
#
# Each tour walks one language's testbed (docs/examples/<lang>-lsp/) through the
# headline Alt-Q actions -- hover, inlay hints, document highlight, references,
# outline, and a rename refactor + Undo -- in that language's own code.
# c/go/python/rust use the fast servers (clangd / gopls / pyright /
# rust-analyzer), so the cold start is short (the tapes Hide it); rust-analyzer
# indexes std, so its warm-up is the longest.  `scala` (Metals) is the slow one:
# a JVM boots and the build is imported (~2 min, Hidden).  For scala, set
# JAVA_HOME to an LTS JDK 17/21 so Metals' presentation compiler does not crash
# on a too-new default JDK:
#   JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64 \
#     WPE=./wpe docs/demos/record-tours.sh scala
#
# Requirements on PATH:
#   vhs, ttyd, ffmpeg            (the recorder)
#   the language server per tour: clangd | gopls+go | pyright/pylsp |
#                                 rust-analyzer+cargo | metals+scala-cli (scala)
#
# Usage (from the repo root, with the editor built):
#   docs/demos/record-tours.sh [lang ...]      # default: c go python rust
#
# WPE MUST be the `wpe` name (programming mode): the Alt-Q LSP actions live in
# e_prog_switch, gated by WpeIsProg() on argv[0].  Launched as `we` the whole
# Alt-Q layer is silently off and the tour films a dead editor -- do NOT set
# WPE=./we.  The default (./wpe) is correct.
#
# WPE defaults to the ./wpe built in the repo root.  Each tour records against a
# fresh COPY of the testbed in a scratch dir, so the servers' caches
# (.cache/target/...) never land in the repo.

set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
: "${WPE:=$root/wpe}"
scratch="${TMPDIR:-/tmp}/xwpe-lsp-tours"

if [ ! -x "$WPE" ]; then
  echo "record-tours.sh: no wpe at $WPE -- build it first (make), or set WPE=" >&2
  exit 1
fi
for tool in vhs ttyd ffmpeg; do
  command -v "$tool" >/dev/null 2>&1 || { echo "record-tours.sh: $tool not on PATH" >&2; exit 1; }
done

cd "$root"
[ $# -gt 0 ] || set -- c go python rust

for lang in "$@"; do
  src="docs/examples/${lang}-lsp"
  tape="docs/demos/tapes/${lang}/tour.tape"
  [ -d "$src" ]  || { echo "record-tours.sh: no testbed $src" >&2; exit 1; }
  [ -f "$tape" ] || { echo "record-tours.sh: no tape $tape" >&2; exit 1; }
  ws="$scratch/$lang"
  rm -rf "$ws"; mkdir -p "$scratch"; cp -r "$src" "$ws"
  echo "== recording $tape (WPE=$WPE DEMO=$ws) =="
  WPE="$WPE" DEMO="$ws" vhs "$tape"
done
echo "done -> docs/demos/gifs/<lang>/tour.gif"
