AC_PREREQ([2.69])
AC_INIT([xwpe], [1.6.2], [juan.mendezr@proton.me], [xwpe],
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

# --- Required: zlib ---
AC_CHECK_LIB([z], [inflate], [],
  [AC_MSG_ERROR([zlib is required. Install zlib1g-dev or zlib-devel.])])

# --- Feature checks ---
AC_CHECK_FUNCS([mkdtemp])

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
    GPM:       $have_gpm
    PRNTCMD:   $PRNTCMD
    pandoc:    $PANDOC
])
