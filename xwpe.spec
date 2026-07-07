Name:           xwpe
Version:        1.6.9
Release:        1%{?dist}
Summary:        Borland-style programming environment and editor for console and X11

# Program is GPL-2.0-only; the bundled SVG icon is MIT (Expat) and the
# AppStream metainfo is FSFAP, and both get installed.
License:        GPL-2.0-only AND MIT AND FSFAP
URL:            https://codeberg.org/mendezr/xwpe
# Upstream "make dist" tarball, attached to the Codeberg release for this
# version's tag; it unpacks to the standard name-version directory.
Source0:        %{url}/releases/download/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconfig
BuildRequires:  ncurses-devel
BuildRequires:  libX11-devel
BuildRequires:  libXft-devel
BuildRequires:  cairo-devel
BuildRequires:  pango-devel
# Native Wayland backend (built unconditionally via --with-wayland below).
BuildRequires:  wayland-devel
BuildRequires:  wayland-protocols-devel
BuildRequires:  libxkbcommon-devel
BuildRequires:  gpm-devel
BuildRequires:  libvterm-devel
BuildRequires:  json-c-devel
BuildRequires:  zlib-devel
BuildRequires:  texinfo
BuildRequires:  desktop-file-utils
BuildRequires:  libappstream-glib

# Owns the /usr/share/icons/hicolor directory tree we drop icons into.
Requires:       hicolor-icon-theme

# Tools the IDE shells out to at runtime (F9 compile, Ctrl-G debug).  Pulled
# in by default but not hard requirements -- the editor runs without them.
Recommends:     gcc
Recommends:     gcc-c++
Recommends:     gdb
Recommends:     gpm

%description
xwpe (the X Windows Programming Environment) is a programming and text editor
in the style of the Borland Turbo C IDE of the early 1990s -- written by Fred
Kruse in 1993, maintained by Dennis Payne from 2000 to 2006, and revived in
2026.  A single binary runs in four modes chosen by the program name: wpe and
we in the console (ncurses), xwpe and xwe under X11 with anti-aliased Xft/Cairo
rendering, UTF-8 and color emoji.

It pairs a syntax-highlighting editor with a Borland-style menu and dialog
system, project management, compiler integration for many languages (C/C++,
Fortran, Pascal, Java, Python, Perl, COBOL, LaTeX) and source-level debugging
through gdb, jdb and pdb.

%prep
%autosetup

%build
# Build the native Wayland backend explicitly (not just auto-detected), so the
# package always ships Wayland support and the build fails loudly if a Wayland
# dependency is ever missing.
%configure --with-wayland
%make_build

%install
%make_install

# The info dir index is owned by the info system, not by this package.
rm -f %{buildroot}%{_infodir}/dir

desktop-file-validate %{buildroot}%{_datadir}/applications/xwpe.desktop
appstream-util validate-relax --nonet \
    %{buildroot}%{_metainfodir}/io.codeberg.mendezr.xwpe.metainfo.xml

%check
make check

%files
%license COPYING
%doc README.md CHANGELOG AUTHORS
%{_bindir}/we
%{_bindir}/wpe
%{_bindir}/xwe
%{_bindir}/xwpe
%dir %{_libdir}/xwpe
%{_libdir}/xwpe/help.key
%{_libdir}/xwpe/help.xwpe
%{_libdir}/xwpe/syntax_def
%{_datadir}/applications/xwpe.desktop
%{_datadir}/icons/hicolor/*/apps/xwpe.*
%{_metainfodir}/io.codeberg.mendezr.xwpe.metainfo.xml
%{_mandir}/man1/we.1*
%{_mandir}/man1/wpe.1*
%{_mandir}/man1/xwe.1*
%{_mandir}/man1/xwpe.1*
%{_infodir}/xwpe.info*

%changelog
* Tue Jul 07 2026 Juan Manuel Méndez Rey <juan.mendezr@proton.me> - 1.6.9-1
- New upstream release 1.6.9: robust native Wayland backend on real desktops.
- Build the Wayland backend explicitly (--with-wayland) with wayland-devel,
  wayland-protocols-devel and libxkbcommon-devel, so the package ships native
  Wayland support (server-side decorations, resizable window, key auto-repeat,
  pointer cursor) rather than falling back to XWayland.
- Real-desktop resize crashes fixed (extbyte overflow, re-entrant relayout
  use-after-free, stale menu bar), with new resize tests across X11, Wayland
  and ncurses run in %%check.

* Fri Jun 26 2026 Juan Manuel Méndez Rey <juan.mendezr@proton.me> - 1.6.7-1
- New upstream release 1.6.7: macOS console Backspace fix (DEL 0x7f -> Backspace);
  reproducible "make dist" release tarballs.

* Mon Jun 22 2026 Juan Manuel Méndez Rey <juan.mendezr@proton.me> - 1.6.6-2
- License: declare the full SPDX expression. The package ships the MIT-licensed
  SVG icon and the FSFAP-licensed AppStream metainfo next to the GPL-2.0-only
  program, so the tag is "GPL-2.0-only AND MIT AND FSFAP".

* Sun Jun 21 2026 Juan Manuel Méndez Rey <juan.mendezr@proton.me> - 1.6.6-1
- New upstream release.
- BSD portability: builds and links natively on FreeBSD, OpenBSD and NetBSD
  (base-curses fallback, SGR console mouse without terminfo, C-standard probe).
  On OpenBSD the console truecolor path is guarded on HAVE_INIT_EXTENDED_PAIR
  (its base curses lacks init_extended_pair/_color) and falls back to 16 colours.
- LSP truecolor semantic tokens (orange methods, teal types) on console and X11.
- Scala debug auto-pins the Bloop/BSP JVM, like Metals.
- Build: e_d_quit_basic() returns on every path, so GCC -Werror=return-type
  (default on openSUSE Tumbleweed) no longer rejects the build; and do not
  assign to stdscr (a non-lvalue macro in a reentrant-built ncursesw).

* Sat Jun 20 2026 Juan Manuel Méndez Rey <juan.mendezr@proton.me> - 1.6.5-1
- New upstream release (bundles 1.6.4 and 1.6.5).
- Embedded VT terminal (libvterm) with the Borland Alt-F5 User Screen; GNU
  Algol 68 (ga68) compiler and gdb debug.
- LSP client for C/C++, Python, Go, Rust and Scala (clangd / pyright-pylsp /
  gopls / rust-analyzer / Metals) and a DAP debugger client (Go via Delve,
  Rust via gdb, Scala via Bloop); async inlay hints and semantic colours.
- BuildRequires: add libvterm-devel and json-c-devel (embedded terminal and
  DAP/LSP JSON-RPC) and zlib-devel (xwpe links -lz).

* Sat Jun 06 2026 Juan Manuel Méndez Rey <juan.mendezr@proton.me> - 1.6.3-1
- New upstream release 1.6.3.
- Modernized the spec for the 1.6.1+ autotools build: single "we" binary
  (wpe/xwe/xwpe symlinks), no more split term/X11 shared libraries.
- Updated %%files for the current layout: hicolor icons, Texinfo manual,
  AppStream metainfo, man pages; drop the obsolete libxwpe-*.so packaging.
- Build with %%configure/%%make_build; run "make check" in %%check.
