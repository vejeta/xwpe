AC_PREREQ([2.69])
AC_INIT([xwpe], [1.6.4], [juan.mendezr@proton.me], [xwpe],
        [https://codeberg.org/mendezr/xwpe])
AC_CONFIG_SRCDIR([Xwpe.h])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_HEADERS([config.h])
AM_INIT_AUTOMAKE([foreign subdir-objects -Wall])

# Compiler
AC_PROG_CC
AC_PROG_LN_S

# --- Optional: documentation tools ---
AC_CHECK_PROG([TEXI2ANY], [texi2any], [texi2any], [:])
AC_CHECK_PROG([PANDOC], [pandoc], [pandoc], [:])

# --- Required: ncursesw (wide character curses) ---
PKG_CHECK_MODULES([NCURSESW], [ncursesw], [],
  [AC_MSG_ERROR([ncursesw is required. Install libncurses-dev or ncurses-devel.])])

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

# --- Optional: libvterm (embedded VT terminal for the xwpe Alt-F5 User Screen) ---
# Pure C ANSI/CSI parser + cell grid; the panel is rendered with the existing
# Cairo/Xft backend, so it is only useful when X11 is present.  Without it the
# Alt-F5 User Screen falls back to focusing the Messages panel (1.6.3 behaviour).
have_vterm=no
AS_IF([test "x$have_x11" = xyes], [
  PKG_CHECK_MODULES([VTERM], [vterm], [
    have_vterm=yes
    AC_DEFINE([HAVE_VTERM], [1], [Define if libvterm is available])
  ], [have_vterm=no])
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
    libvterm:  $have_vterm
    GPM:       $have_gpm
    PRNTCMD:   $PRNTCMD
    pandoc:    $PANDOC
])
