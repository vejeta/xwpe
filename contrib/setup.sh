#!/bin/sh
# setup.sh -- one command to build, install and wire up xwpe.
#
# Does, in order, what the README's "Building & installing" section spells out
# step by step:
#   1. install the build dependencies   (Linux: apt/dnf/zypper/pacman/emerge;
#                                         macOS: brew; *BSD: pkg/pkg_add/pkgin)
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

# How to gain root for the system-wide package install and `make install`:
# nothing if we already are root, else sudo, else doas (the OpenBSD default).
if   [ "$(id -u)" -eq 0 ];                 then priv=''
elif command -v sudo >/dev/null 2>&1;      then priv='sudo'
elif command -v doas >/dev/null 2>&1;      then priv='doas'
else priv=''
fi

# deps      -- command that installs the build deps (empty = no auto-installer)
# deps_hint -- what to install by hand when there is no auto-installer
deps=''
deps_hint=''

os=$(uname -s)
case "$os" in
  Linux)
    # Native package manager: only the dep NAMES differ; build + install +
    # shell-wiring are identical. apt (Debian/Ubuntu), dnf (Fedora/RHEL),
    # zypper (openSUSE), pacman (Arch), emerge (Gentoo).
    if command -v apt-get >/dev/null 2>&1; then
      deps="$priv apt-get install -y build-essential autoconf automake pkg-config
            texinfo libncurses-dev libx11-dev libxft-dev libcairo2-dev
            libpango1.0-dev libvterm-dev libjson-c-dev libgpm-dev zlib1g-dev
            librsvg2-bin"
    elif command -v dnf >/dev/null 2>&1; then
      deps="$priv dnf install -y gcc make autoconf automake pkgconf-pkg-config
            texinfo ncurses-devel libX11-devel libXft-devel cairo-devel
            pango-devel libvterm-devel json-c-devel gpm-devel zlib-devel"
    elif command -v zypper >/dev/null 2>&1; then
      deps="$priv zypper install -y gcc make autoconf automake pkgconf-pkg-config
            texinfo ncurses-devel libX11-devel libXft-devel cairo-devel
            pango-devel libvterm-devel libjson-c-devel gpm-devel zlib-devel"
    elif command -v pacman >/dev/null 2>&1; then
      deps="$priv pacman -S --needed --noconfirm base-devel autoconf automake
            pkgconf texinfo ncurses libx11 libxft cairo pango libvterm json-c
            gpm zlib"
    elif command -v emerge >/dev/null 2>&1; then
      # @system already provides autoconf/automake/pkgconf; pull the libraries
      # (and texinfo). --noreplace is a no-op for anything already merged.
      deps="$priv emerge --noreplace --quiet sys-apps/texinfo sys-libs/ncurses
            x11-libs/libX11 x11-libs/libXft x11-libs/cairo x11-libs/pango
            dev-libs/libvterm dev-libs/json-c sys-libs/gpm sys-libs/zlib"
    else
      deps_hint='install a C toolchain + autoconf, automake, pkg-config, texinfo and the -devel packages for ncursesw, X11, Xft, cairo, pango, libvterm, json-c, gpm, zlib (see the README per-distro lists)'
    fi
    cfg_flags=''
    do_install="$priv make install"
    ;;
  Darwin)
    command -v brew >/dev/null 2>&1 || {
      echo "setup.sh: Homebrew is required on macOS -- https://brew.sh" >&2
      exit 1; }
    # XQuartz (https://www.xquartz.org) installs to /opt/X11 and is the only
    # X11 server on modern macOS; without it there is nothing for xwpe to draw
    # against, so we fall back to a terminal-only build.
    x11_root=
    if   [ -d /opt/X11/include/X11 ]; then x11_root=/opt/X11
    elif [ -d /usr/X11/include/X11  ]; then x11_root=/usr/X11
    fi
    deps='brew install autoconf automake pkg-config ncurses libvterm json-c texinfo'
    if [ -n "$x11_root" ]; then
      # XQuartz ships libX11/libXext/libXft; Homebrew provides the higher-level
      # cairo/pango stack that links on top of them.
      deps="$deps libxft cairo pango"
    fi
    # XQuartz ships fontconfig 2.14 / cairo 1.17, both older than what brewed
    # pangocairo requires (>= 2.17 / 1.18). If XQuartz's pkg-config dir wins
    # the search, configure silently falls back to HAVE_PANGO=no and the
    # Cairo backend (we_render_cairo.c) emits no symbols, breaking the link
    # with undefined wpe_render_chrome / wpe_chrome_hit_* references. So
    # brewed cairo/pango/fontconfig/freetype/libxft go first; XQuartz is
    # appended at the end to fill the X11 protocol gaps (x11.pc, ...).
    PKG_CONFIG_PATH="$(brew --prefix ncurses)/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
    LDFLAGS_EXTRA=
    if [ -n "$x11_root" ]; then
      for f in libxft freetype fontconfig pango cairo; do
        p=$(brew --prefix "$f" 2>/dev/null) || continue
        PKG_CONFIG_PATH="$p/lib/pkgconfig:$PKG_CONFIG_PATH"
      done
      PKG_CONFIG_PATH="$PKG_CONFIG_PATH:$x11_root/lib/pkgconfig"
      # AC_PATH_XTRA puts -L/opt/X11/lib in X_LIBS so XQuartz also wins at
      # link time, loading its older libcairo/libfontconfig/libfreetype at
      # runtime against a pango built for the newer brewed ABIs -- which
      # crashes (SIGSEGV in cairo). Prepend brewed lib dirs to LDFLAGS so
      # the linker (which honours -Wl,-search_paths_first) finds the brewed
      # copies first; XQuartz still resolves the pure-X11 libs (libSM, ICE,
      # Xext, Xrender) that have no newer brewed competitor in the link.
      for f in libxft freetype fontconfig pango cairo libx11; do
        p=$(brew --prefix "$f" 2>/dev/null) || continue
        LDFLAGS_EXTRA="-L$p/lib $LDFLAGS_EXTRA"
      done
    fi
    export PKG_CONFIG_PATH
    # Homebrew's prefix is user-writable AND already on PATH, so `wpe` works
    # with no sudo and no extra PATH step.
    if [ -n "$x11_root" ]; then
      # AC_PATH_XTRA does not probe /opt/X11; point it at XQuartz explicitly so
      # HAVE_X11 turns on and the install hook creates the xwpe/xwe symlinks.
      cfg_flags="--with-x --without-gpm --prefix=$(brew --prefix) --x-includes=$x11_root/include --x-libraries=$x11_root/lib"
    else
      cfg_flags="--without-x --without-gpm --prefix=$(brew --prefix)"
    fi
    do_install='make install'
    ;;
  FreeBSD)
    deps="$priv pkg install -y autoconf automake pkgconf cairo pango json-c libvterm"
    cfg_flags='--without-gpm'        # GPM is Linux-only; X11 is in the base system
    do_install="$priv make install"
    ;;
  OpenBSD)
    deps="$priv pkg_add autoconf automake cairo pango json-c libvterm"   # curses is base
    cfg_flags='--without-gpm'
    do_install="$priv make install"
    ;;
  NetBSD)
    deps="$priv pkgin -y install autoconf automake pkgconf cairo pango json-c libvterm03 ncurses"
    cfg_flags='--without-gpm'
    do_install="$priv make install"
    ;;
  *)
    echo "setup.sh: unsupported OS '$os'." >&2
    echo "Build manually -- see the README \"Building & installing\" section." >&2
    exit 1 ;;
esac

cd "$root"

# 1. dependencies
if [ "$skip_deps" = 1 ]; then
  say "Skipping dependencies (--skip-deps)"
elif [ -n "$deps" ]; then
  say "Installing build dependencies"
  # shellcheck disable=SC2086
  run $deps
else
  say "No automatic dependency installer for this system -- install the deps, then re-run with --skip-deps:"
  printf '    %s\n' "$deps_hint"
  exit 1
fi

# 2. build
if [ "$os" = Darwin ]; then
  if [ -n "${x11_root:-}" ]; then
    say "XQuartz found at $x11_root -- building with X11 (xwpe/xwe)"
  else
    say "XQuartz not found -- building terminal-only (wpe). Install https://www.xquartz.org and re-run for the X11 GUI."
  fi
fi
say "Building"
[ -x ./configure ] || run autoreconf -fi
# Wipe any prior build before reconfiguring: LIBRARY_DIR / XWPE_INFODIR are
# baked into the binary via -D flags in CFLAGS rather than config.h, so a tree
# previously built with a different --prefix has stale .o files that `make`
# considers up to date and the relink picks up the OLD install paths.
[ -f Makefile ] && run make clean
# On macOS the brewed lib dirs (cairo/pango/...) need to be searched before
# XQuartz's -L/opt/X11/lib (set by AC_PATH_XTRA from --x-libraries) so the
# linker resolves the same newer ABI that brewed pango was built against.
# Empty / unset on Linux, where this whole concern does not exist.
if [ -n "${LDFLAGS_EXTRA:-}" ]; then
  LDFLAGS="$LDFLAGS_EXTRA ${LDFLAGS:-}"
  export LDFLAGS
fi
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
