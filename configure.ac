AC_PREREQ([2.69])
AC_INIT([xwpe], [1.6.8], [juan.mendezr@proton.me], [xwpe],
        [https://codeberg.org/mendezr/xwpe])
AC_CONFIG_SRCDIR([Xwpe.h])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_HEADERS([config.h])
AM_INIT_AUTOMAKE([foreign subdir-objects -Wall])

# Compiler
AC_PROG_CC
AC_PROG_LN_S

# Pick a C standard flag that keeps K&R unprototyped '()' semantics (so the
# 1993 code still compiles) and that the compiler actually supports. Modern
# GCC/Clang take -std=gnu17; NetBSD 9's base GCC 7 only knows up to gnu11,
# which has identical K&R-() semantics. Both avoid the C23/gnu23 default where
# 'f()' means 'f(void)'.
AC_MSG_CHECKING([for a usable C standard flag])
XWPE_STDFLAG=
for ace_f in -std=gnu17 -std=gnu11; do
  ace_save_CFLAGS="$CFLAGS"
  CFLAGS="$CFLAGS $ace_f"
  AC_COMPILE_IFELSE([AC_LANG_PROGRAM([], [])], [XWPE_STDFLAG="$ace_f"])
  CFLAGS="$ace_save_CFLAGS"
  test -n "$XWPE_STDFLAG" && break
done
AS_IF([test -z "$XWPE_STDFLAG"], [AC_MSG_RESULT([none])],
  [AC_MSG_RESULT([$XWPE_STDFLAG])])
AC_SUBST([XWPE_STDFLAG])

# --- Optional: documentation tools ---
AC_CHECK_PROG([TEXI2ANY], [texi2any], [texi2any], [:])
AC_CHECK_PROG([PANDOC], [pandoc], [pandoc], [:])

# --- Required: ncursesw (wide character curses) ---
# Probe, in order: an ncursesw.pc (Linux/FreeBSD); an ncurses.pc (NetBSD pkgsrc,
# whose wide-char build ships only 'ncurses.pc' and carries the crucial
# -I.../ncurses include path + -DNCURSES_WIDECHAR); finally a direct link check
# for OpenBSD, which keeps wide-char curses (with the mouse API) in its base
# system and provides no pkg-config file.
PKG_CHECK_MODULES([NCURSESW], [ncursesw], [],
  [PKG_CHECK_MODULES([NCURSESW], [ncurses], [],
    [ace_save_LIBS="$LIBS"
     AC_SEARCH_LIBS([get_wch], [ncursesw curses],
       [AS_IF([test "x$ac_cv_search_get_wch" = "xnone required"],
          [NCURSESW_LIBS=""], [NCURSESW_LIBS="$ac_cv_search_get_wch"])],
       [AC_MSG_ERROR([wide-character curses (ncursesw) is required. Install libncurses-dev or ncurses-devel.])])
     LIBS="$ace_save_LIBS"
     NCURSESW_CFLAGS=""
     AC_SUBST([NCURSESW_CFLAGS])
     AC_SUBST([NCURSESW_LIBS])])])

# --- Optional: ncurses extended-colour API (init_extended_pair/_color) ---
# Drives LSP semantic-token truecolor on the console.  Present in ncurses 6
# (Linux, FreeBSD ports, NetBSD pkgsrc) but absent from OpenBSD's base curses,
# where linking it fails; probe for it and fall back to the 16-colour path when
# it is missing (see e_t_sem_tc_set in we_term.c).
ace_save_LIBS="$LIBS"
LIBS="$NCURSESW_LIBS $LIBS"
AC_CHECK_FUNCS([init_extended_pair])
LIBS="$ace_save_LIBS"

# --- json-c: wire format for the DAP debug client (Go/Rust/Scala) ---
PKG_CHECK_MODULES([JSONC], [json-c], [],
  [AC_MSG_ERROR([json-c is required for the DAP debug client. Install libjson-c-dev.])])

# --- Optional: X11 ---
AC_ARG_WITH([x],
  [AS_HELP_STRING([--without-x], [Disable X11 support])],
  [], [with_x=yes])

have_x11=no
AS_IF([test "x$with_x" != xno], [
  AC_PATH_XTRA
  AS_IF([test "x$no_x" != xyes], [
    have_x11=yes
    X_LIBS="$X_LIBS $X_PRE_LIBS -lX11 $X_EXTRA_LIBS"
  ])
])

AS_IF([test "x$have_x11" != xyes], [
  AC_DEFINE([NO_XWINDOWS], [1], [Define if X11 support is not available])
])
AM_CONDITIONAL([HAVE_X11], [test "x$have_x11" = xyes])

# --- Optional: Xft (antialiased font rendering, requires X11) ---
have_xft=no
AS_IF([test "x$have_x11" = xyes], [
  PKG_CHECK_MODULES([XFT], [xft fontconfig], [
    have_xft=yes
    AC_DEFINE([HAVE_XFT], [1], [Define if Xft is available])
  ], [have_xft=no])
])

# --- Optional: Cairo + Pango (vector rendering, requires X11) ---
# freetype2 is listed explicitly: we_render_cairo.c calls the FreeType API
# directly (FT_Get_Char_Index, cairo_ft_scaled_font_lock_face), so libfreetype
# must be on the link line.  Xft/Cairo pull it transitively, but a strict
# linker (Fedora's -Wl,--no-undefined-ish ld) requires the DSO named directly.
have_cairo=no
AS_IF([test "x$have_x11" = xyes], [
  PKG_CHECK_MODULES([CAIRO], [cairo cairo-xlib freetype2], [
    have_cairo=yes
    AC_DEFINE([HAVE_CAIRO], [1], [Define if Cairo is available])
  ], [have_cairo=no])
])
have_pango=no
AS_IF([test "x$have_cairo" = xyes], [
  PKG_CHECK_MODULES([PANGO], [pangocairo], [
    have_pango=yes
    AC_DEFINE([HAVE_PANGO], [1], [Define if Pango is available])
  ], [have_pango=no])
])
AM_CONDITIONAL([HAVE_CAIRO], [test "x$have_cairo" = xyes])

# --- Optional: native Wayland backend ---
# Wayland exposes no server-side font protocol, so the client renders every
# glyph itself; the native backend reuses the same Cairo+Pango stack as the X11
# path (we_render_cairo.c / we_render_wayland.c).  It is therefore only offered
# when Cairo+Pango are present.  Needs the client libraries, xkbcommon (keymap
# translation), wayland-scanner (generates the xdg-shell glue) and the
# wayland-protocols data dir (ships the xdg-shell XML).  Default: auto -- build
# it when everything is found, otherwise quietly skip (X11/XWayland still works).
AC_ARG_WITH([wayland],
  [AS_HELP_STRING([--with-wayland],
     [Enable the native Wayland backend (needs Cairo+Pango; default: auto)])],
  [], [with_wayland=auto])

have_wayland=no
AS_IF([test "x$with_wayland" != xno], [
  AS_IF([test "x$have_pango" = xyes], [
    PKG_CHECK_MODULES([WAYLAND], [wayland-client wayland-cursor xkbcommon], [
      AC_CHECK_PROG([WAYLAND_SCANNER], [wayland-scanner], [wayland-scanner], [:])
      PKG_CHECK_VAR([WAYLAND_PROTOCOLS_DIR], [wayland-protocols], [pkgdatadir])
      AS_IF([test "x$WAYLAND_SCANNER" != "x:" && test -n "$WAYLAND_PROTOCOLS_DIR"], [
        have_wayland=yes
        AC_DEFINE([HAVE_WAYLAND], [1], [Define if the native Wayland backend is built])
      ], [AS_IF([test "x$with_wayland" = xyes],
            [AC_MSG_ERROR([Wayland requested but wayland-scanner or wayland-protocols not found])])])
    ], [AS_IF([test "x$with_wayland" = xyes],
          [AC_MSG_ERROR([Wayland requested but wayland-client/wayland-cursor/xkbcommon not found])])])
  ], [AS_IF([test "x$with_wayland" = xyes],
        [AC_MSG_ERROR([the Wayland backend requires Cairo+Pango, which were not found])])])
])
AM_CONDITIONAL([HAVE_WAYLAND], [test "x$have_wayland" = xyes])
AC_SUBST([WAYLAND_PROTOCOLS_DIR])

# --- Optional: libvterm (embedded VT terminal for the xwpe Alt-F5 User Screen) ---
# Pure C ANSI/CSI parser + cell grid; the panel is rendered with the existing
# Cairo/Xft backend, so it is only useful when X11 is present.  Without it the
# Alt-F5 User Screen falls back to focusing the Messages panel (1.6.3 behaviour).
have_vterm=no
AS_IF([test "x$have_x11" = xyes], [
  # NetBSD pkgsrc names the module 'vterm03' (versioned); everywhere else it is
  # 'vterm'. Try both.
  PKG_CHECK_MODULES([VTERM], [vterm], [
    have_vterm=yes
    AC_DEFINE([HAVE_VTERM], [1], [Define if libvterm is available])
  ], [PKG_CHECK_MODULES([VTERM], [vterm03], [
       have_vterm=yes
       AC_DEFINE([HAVE_VTERM], [1], [Define if libvterm is available])
     ], [have_vterm=no])])
])

# --- Optional: GPM mouse ---
AC_ARG_WITH([gpm],
  [AS_HELP_STRING([--without-gpm], [Disable GPM mouse support])],
  [], [with_gpm=auto])

have_gpm=no
AS_IF([test "x$with_gpm" != xno], [
  AC_CHECK_LIB([gpm], [Gpm_Open],
    [have_gpm=yes
     GPM_LIBS="-lgpm"
     AC_DEFINE([HAVE_LIBGPM], [1], [Define if libgpm is available])],
    [AS_IF([test "x$with_gpm" = xyes],
       [AC_MSG_ERROR([libgpm requested but not found])])])
])
AC_SUBST([GPM_LIBS])

# --- Required: openpty (debugger / program pty capture) ---
# openpty() lives in libutil on Linux and the BSDs, but is part of libc on
# macOS (no separate -lutil).  AC_SEARCH_LIBS tries libc first, then -lutil, so
# both platforms configure cleanly.  Save/restore LIBS so the link flag goes
# through UTIL_LIBS (the existing we_LDADD slot), not the global LIBS.
ace_save_LIBS="$LIBS"
AC_SEARCH_LIBS([openpty], [util],
  [AS_IF([test "x$ac_cv_search_openpty" = "xnone required"],
     [UTIL_LIBS=""], [UTIL_LIBS="$ac_cv_search_openpty"])],
  [AC_MSG_ERROR([openpty() is required (libutil on Linux/BSD, libc on macOS).])])
LIBS="$ace_save_LIBS"
AC_SUBST([UTIL_LIBS])

# openpty()'s declaration lives in a different header per platform:
#   <pty.h> on glibc, <util.h> on macOS/BSD, <libutil.h> on some BSDs.
# The sources pick the right one from these HAVE_*_H defines.
AC_CHECK_HEADERS([pty.h util.h libutil.h])

# --- backtrace() for the crash handler ---
# backtrace()/backtrace_symbols_fd() are in libc on glibc but in libexecinfo
# (base system) on the BSDs.  Route the flag through EXECINFO_LIBS like
# UTIL_LIBS.  Not fatal if absent -- the crash handler just degrades.
ace_save_LIBS="$LIBS"
AC_SEARCH_LIBS([backtrace], [execinfo],
  [AS_IF([test "x$ac_cv_search_backtrace" = "xnone required"],
     [EXECINFO_LIBS=""], [EXECINFO_LIBS="$ac_cv_search_backtrace"])])
LIBS="$ace_save_LIBS"
AC_SUBST([EXECINFO_LIBS])

# --- Required: zlib ---
AC_CHECK_LIB([z], [inflate], [],
  [AC_MSG_ERROR([zlib is required. Install zlib1g-dev or zlib-devel.])])

# --- Feature checks ---
AC_CHECK_FUNCS([mkdtemp])

# --- Optional: WPE_TRACE debug logging (off by default, zero-cost) ---
AC_ARG_ENABLE([trace],
  [AS_HELP_STRING([--enable-trace],
     [enable the WPE_TRACE debug logging facility (default: no)])],
  [], [enable_trace=no])
AS_IF([test "x$enable_trace" = "xyes"],
  [AC_DEFINE([WPE_TRACE], [1], [Enable the WPE_TRACE debug logging facility])])

# --- Unconditional defines ---
AC_DEFINE([UNIX], [1], [Building on a Unix system])
AC_DEFINE([NCURSES], [1], [Using ncurses library])
AC_DEFINE([_XOPEN_SOURCE_EXTENDED], [1], [Enable wide character curses support])
AC_DEFINE([NCURSES_WIDECHAR], [1], [Enable ncurses wide character API])
AC_DEFINE([SELECTION], [1], [Use X Selection events])
AC_DEFINE([MAN_S_OPT], [1], [man accepts -s to select section])

# --- Print command (configurable) ---
AC_ARG_VAR([PRNTCMD], [Print command (default: lpr)])
test -z "$PRNTCMD" && PRNTCMD="lpr"
AC_DEFINE_UNQUOTED([PRNTCMD], ["$PRNTCMD"], [Command to print files])

# --- Output ---
AC_CONFIG_FILES([Makefile docs/Makefile])
AC_OUTPUT

AC_MSG_RESULT([
  xwpe $PACKAGE_VERSION configuration:
    ncursesw:  yes (via pkg-config)
    X11:       $have_x11
    Xft:       $have_xft
    Cairo:     $have_cairo
    Pango:     $have_pango
    Wayland:   $have_wayland
    libvterm:  $have_vterm
    GPM:       $have_gpm
    PRNTCMD:   $PRNTCMD
    pandoc:    $PANDOC
])
