#!/bin/sh
# setup.sh -- one command to build, install and wire up xwpe.
#
# Does, in order, what the README's "Building & installing" section spells out
# step by step:
#   1. install the build dependencies   (Debian/Ubuntu via apt, macOS via brew)
#   2. autoreconf + configure + make
#   3. make install   (sudo on Linux's /usr/local; no sudo on macOS, where it
#                       installs into Homebrew's prefix, already on PATH)
#   4. add the environment helper to your shell profile (xwpe-env --persist)
#
# It does NOT install the optional compilers / language servers -- those are a
# per-language choice; the README "External tools" section lists them.
#
# Usage:
#   sh contrib/setup.sh              # do it
#   sh contrib/setup.sh --dry-run    # print every command, run nothing
#   sh contrib/setup.sh --skip-deps  # skip step 1 (deps already installed)
#   sh contrib/setup.sh --help

set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)   # contrib/
root=$(CDPATH= cd -- "$here/.." && pwd)             # checkout root

dry=0
skip_deps=0
for a in "$@"; do
  case "$a" in
    --dry-run)   dry=1 ;;
    --skip-deps) skip_deps=1 ;;
    -h|--help)
      sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) echo "setup.sh: unknown option '$a' (try --help)" >&2; exit 2 ;;
  esac
done

say() { printf '\n==> %s\n' "$*"; }
run() {                       # run, or just print under --dry-run
  if [ "$dry" = 1 ]; then printf '    %s\n' "$*"; else ( set -x; "$@" ); fi
}

os=$(uname -s)
case "$os" in
  Linux)
    deps='sudo apt-get install -y build-essential autoconf automake pkg-config
          texinfo libncurses-dev libx11-dev libxft-dev libcairo2-dev
          libpango1.0-dev libvterm-dev libjson-c-dev libgpm-dev zlib1g-dev
          librsvg2-bin'
    have_apt=1; command -v apt-get >/dev/null 2>&1 || have_apt=0
    cfg_flags=''
    do_install='sudo make install'
    ;;
  Darwin)
    command -v brew >/dev/null 2>&1 || {
      echo "setup.sh: Homebrew is required on macOS -- https://brew.sh" >&2
      exit 1; }
    deps='brew install autoconf automake pkg-config ncurses libvterm json-c texinfo'
    have_apt=1
    PKG_CONFIG_PATH="$(brew --prefix ncurses)/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
    export PKG_CONFIG_PATH
    # Homebrew's prefix is user-writable AND already on PATH, so `wpe` works
    # with no sudo and no extra PATH step.
    cfg_flags="--without-x --without-gpm --prefix=$(brew --prefix)"
    do_install='make install'
    ;;
  *)
    echo "setup.sh: unsupported OS '$os' (handles Linux/apt and macOS/brew)." >&2
    echo "Build manually -- see the README \"Building & installing\" section." >&2
    exit 1 ;;
esac

cd "$root"

# 1. dependencies
if [ "$skip_deps" = 1 ]; then
  say "Skipping dependencies (--skip-deps)"
elif [ "$os" = Linux ] && [ "$have_apt" = 0 ]; then
  say "No apt here -- install the Debian deps' equivalent (see README), then re-run with --skip-deps"
  exit 1
else
  say "Installing build dependencies"
  # shellcheck disable=SC2086
  run $deps
fi

# 2. build
say "Building"
[ -x ./configure ] || run autoreconf -fi
# Wipe any prior build before reconfiguring: LIBRARY_DIR / XWPE_INFODIR are
# baked into the binary via -D flags in CFLAGS rather than config.h, so a tree
# previously built with a different --prefix has stale .o files that `make`
# considers up to date and the relink picks up the OLD install paths.
[ -f Makefile ] && run make clean
# shellcheck disable=SC2086
run ./configure $cfg_flags
run make

# 3. install
say "Installing"
# shellcheck disable=SC2086
run $do_install

# 4. wire the shell
say "Wiring your shell profile"
run sh "$here/xwpe-env" --persist

say "Done."
printf 'Open a NEW terminal (so the environment loads), then try:\n'
printf '    wpe %s/docs/examples/c-lsp/main.cpp\n' "$root"
printf 'For the Alt-Q IDE features, install the language servers you want\n'
printf '(README "External tools"); they appear once you reopen the terminal.\n'
