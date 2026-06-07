Name:           xwpe
Version:        1.6.3
Release:        1%{?dist}
Summary:        Borland-style programming environment and editor for console and X11

License:        GPL-2.0-only
URL:            https://codeberg.org/mendezr/xwpe
# Produced by "make dist" from the autotools tree (or the Codeberg release
# tarball for tag v%{version}, which unpacks to %{name}-%{version}/).
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconfig
BuildRequires:  ncurses-devel
BuildRequires:  libX11-devel
BuildRequires:  libXft-devel
BuildRequires:  cairo-devel
BuildRequires:  pango-devel
BuildRequires:  gpm-devel
BuildRequires:  texinfo
BuildRequires:  desktop-file-utils
BuildRequires:  libappstream-glib

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
%configure
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
* Sat Jun 06 2026 Juan Manuel Mendez Rey <juan.mendezr@proton.me> - 1.6.3-1
- New upstream release 1.6.3.
- Modernized the spec for the 1.6.1+ autotools build: single "we" binary
  (wpe/xwe/xwpe symlinks), no more split term/X11 shared libraries.
- Updated %%files for the current layout: hicolor icons, Texinfo manual,
  AppStream metainfo, man pages; drop the obsolete libxwpe-*.so packaging.
- Build with %%configure/%%make_build; run "make check" in %%check.
