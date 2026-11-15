AC_PREREQ([2.70])
AC_INIT([xwpe], [1.7.0], [juan.mendezr@proton.me], [xwpe],
        [https://codeberg.org/mendezr/xwpe])
AC_CONFIG_SRCDIR([Xwpe.h])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_HEADERS([config.h])
AC_CANONICAL_HOST
AM_INIT_AUTOMAKE([foreign subdir-objects -Wall])

# Compiler
AC_PROG_CC
# The feature macros (_GNU_SOURCE, _DARWIN_C_SOURCE, _NETBSD_SOURCE, ...)
# are decided here and written into config.h, which is force-included before
# every source file.  No source file sets a feature macro of its own.
AC_USE_SYSTEM_EXTENSIONS
AC_PROG_LN_S
# nm of the toolchain that built the objects (a cross or LTO build's own),
# for tests/check-one-owner.sh.
AC_CHECK_TOOL([NM], [nm], [:])

# Whether the linker can take over malloc and realloc (GNU ld and lld:
# -Wl,--wrap=).  Apple's ld cannot.  tests/test_startplan.c uses it to make
# allocations fail on purpose; where it is missing those rows report SKIP.
AC_MSG_CHECKING([whether the linker supports -Wl,--wrap])
ace_save_LDFLAGS="$LDFLAGS"
LDFLAGS="$LDFLAGS -Wl,--wrap=malloc -Wl,--wrap=realloc"
AC_LINK_IFELSE(
  [AC_LANG_PROGRAM([[#include <stdlib.h>
void *__real_malloc(size_t n);
void *__wrap_malloc(size_t n) { return __real_malloc(n); }
void *__real_realloc(void *p, size_t n);
void *__wrap_realloc(void *p, size_t n) { return __real_realloc(p, n); }]],
     [[free(malloc(1));]])],
  [have_ld_wrap=yes], [have_ld_wrap=no])
LDFLAGS="$ace_save_LDFLAGS"
AC_MSG_RESULT([$have_ld_wrap])
AM_CONDITIONAL([HAVE_LD_WRAP], [test "x$have_ld_wrap" = xyes])

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

# Warning flags.  The 1993 code needs three -Wno- flags.  A compiler that
# does not know a -Wno- flag accepts it silently (gcc) or warns on every
# compile (clang: "unknown warning option"), so each one is probed in its
# positive form with -Werror and the -Wno- form is kept only when the
# compiler knows the warning (the idiom of meson's get_supported_arguments).
AC_MSG_CHECKING([which warning flags the compiler knows])
XWPE_WARNFLAGS=
for ace_w in incompatible-pointer-types int-conversion old-style-declaration; do
  ace_save_CFLAGS="$CFLAGS"
  CFLAGS="$CFLAGS -Werror -W$ace_w"
  AC_COMPILE_IFELSE([AC_LANG_PROGRAM([], [])],
    [XWPE_WARNFLAGS="$XWPE_WARNFLAGS -Wno-$ace_w"])
  CFLAGS="$ace_save_CFLAGS"
done
XWPE_WARNFLAGS=${XWPE_WARNFLAGS# }
AC_MSG_RESULT([${XWPE_WARNFLAGS:-none}])
xwpe_say_warn=${XWPE_WARNFLAGS:-"none (every compile of the 1993 code prints its warnings)"}
AC_SUBST([XWPE_WARNFLAGS])

# Format checking.  -Wformat is what catches a %ld given an off_t (clang has
# it on by default; gcc only with -Wall).  On gcc it also turns on the
# truncation and overflow estimates of fixed-size string writes (22 in the
# editor, 273 in the tests, and one unbounded sprintf at we_edit.c:4162),
# which are another class of finding, so those two are left off the same
# probed way as above until that class is worked through.
AC_MSG_CHECKING([which format flags the compiler knows])
XWPE_FORMATFLAGS=-Wformat
for ace_w in format-truncation format-overflow; do
  ace_save_CFLAGS="$CFLAGS"
  CFLAGS="$CFLAGS -Werror -W$ace_w"
  AC_COMPILE_IFELSE([AC_LANG_PROGRAM([], [])],
    [XWPE_FORMATFLAGS="$XWPE_FORMATFLAGS -Wno-$ace_w"])
  CFLAGS="$ace_save_CFLAGS"
done
AC_MSG_RESULT([$XWPE_FORMATFLAGS])
AC_SUBST([XWPE_FORMATFLAGS])

# A call to an undeclared function is an error, whatever the compiler's
# default: gcc 14 and clang 16 already refuse it, but NetBSD 9's gcc 7 only
# warns, and a build that only warns hides the missing header on exactly the
# system that lacks it.
AC_MSG_CHECKING([for a flag that makes an implicit declaration an error])
XWPE_DECLFLAG=
ace_save_CFLAGS="$CFLAGS"
CFLAGS="$CFLAGS -Werror=implicit-function-declaration"
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([], [])],
  [XWPE_DECLFLAG=-Werror=implicit-function-declaration])
CFLAGS="$ace_save_CFLAGS"
AC_MSG_RESULT([${XWPE_DECLFLAG:-none}])
xwpe_say_decl=${XWPE_DECLFLAG:-"none (a missing header is only a warning)"}
AC_SUBST([XWPE_DECLFLAG])

# What this system provides, as facts.  Each fact is probed once here, written
# into config.h as WPE_HAS_<NAME> (always defined, to 0 or 1) and printed in
# the summary with what its absence turns off.  Sources read the fact through
# we_platform.h and never name an operating system or a system header's
# presence themselves.
AC_DEFUN([WPE_FACT],
  [AS_IF([$2], [wpe_fact_$1=1], [wpe_fact_$1=0])
   AC_DEFINE_UNQUOTED([WPE_HAS_$1], [$wpe_fact_$1],
     [1 when $3, 0 when it does not])])

# The system's own timer descriptors: the Wayland key-repeat timer is one.
AC_CACHE_CHECK([for timerfd], [xwpe_cv_timerfd],
  [AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <time.h>
#include <sys/timerfd.h>]], [[return timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);]])],
    [xwpe_cv_timerfd=yes], [xwpe_cv_timerfd=no])])
WPE_FACT([TIMERFD], [test "x$xwpe_cv_timerfd" = xyes], [the system has timerfd])
xwpe_say_timerfd=$xwpe_cv_timerfd
AS_IF([test "x$xwpe_cv_timerfd" != xyes],
  [xwpe_say_timerfd="no (the Wayland backend is not built)"])

# A memory file with no name in the file system: the Wayland shm pool.
AC_CACHE_CHECK([for memfd_create], [xwpe_cv_memfd_create],
  [AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <sys/mman.h>]],
     [[return memfd_create("x", MFD_CLOEXEC);]])],
    [xwpe_cv_memfd_create=yes], [xwpe_cv_memfd_create=no])])
WPE_FACT([MEMFD_CREATE], [test "x$xwpe_cv_memfd_create" = xyes], [the system has memfd_create])
xwpe_say_memfd=$xwpe_cv_memfd_create
AS_IF([test "x$xwpe_cv_memfd_create" != xyes],
  [xwpe_say_memfd="no (the Wayland shm pool is an unlinked file in XDG_RUNTIME_DIR)"])

# Closing the descriptors a program inherits: close_range (Linux 5.9 with
# glibc 2.34, FreeBSD 13) closes a span in one call, closefrom (the BSDs,
# glibc 2.34 as a library routine) everything above a number.  The probe
# compiles against the declaration, so a library that has the symbol and no
# prototype counts as "no".
AC_CACHE_CHECK([for close_range], [xwpe_cv_close_range],
  [AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <unistd.h>]],
     [[return close_range(3, ~0U, 0);]])],
    [xwpe_cv_close_range=yes], [xwpe_cv_close_range=no])])
WPE_FACT([CLOSE_RANGE], [test "x$xwpe_cv_close_range" = xyes], [the system has close_range])
xwpe_say_close_range=$xwpe_cv_close_range
AS_IF([test "x$xwpe_cv_close_range" != xyes],
  [xwpe_say_close_range="no (a loop over the descriptor table closes a child's inherited descriptors, closefrom for its tail where the system has it)"])

# closefrom counts only where it is a thin call into the kernel: on Solaris
# and illumos it walks /proc and may allocate memory, which a child of a
# threaded process must not do between fork and exec; Linux has no such
# system call, and glibc's library routine lists /proc and aborts the child
# when it cannot close.
AC_CACHE_CHECK([for closefrom], [xwpe_cv_closefrom],
  [AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <unistd.h>]],
     [[closefrom(3); return 0;]])],
    [xwpe_cv_closefrom=yes], [xwpe_cv_closefrom=no])])
AS_CASE([$host_os],
  [solaris*|illumos*|*linux*], [xwpe_closefrom_thin=no],
  [xwpe_closefrom_thin=yes])
WPE_FACT([CLOSEFROM],
  [test "x$xwpe_cv_closefrom" = xyes && test "x$xwpe_closefrom_thin" = xyes],
  [the system has a closefrom that is a thin call into the kernel])
xwpe_say_closefrom=$xwpe_cv_closefrom
AS_IF([test "x$xwpe_cv_closefrom" != xyes],
  [xwpe_say_closefrom="no (a loop over the descriptor table closes a child's inherited descriptors)"],
  [test "x$xwpe_closefrom_thin" != xyes],
  [xwpe_say_closefrom="not used (a library routine that walks /proc here; close_range, else a loop, closes a child's inherited descriptors)"])

# The Linux virtual console's ioctls: the keyboard-state probe and GPM's
# console check.  The headers are the Linux kernel's own.
AC_CACHE_CHECK([for the Linux console ioctls], [xwpe_cv_linux_console],
  [AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[#include <sys/ioctl.h>
#include <linux/kd.h>
#include <linux/vt.h>]], [[struct vt_stat s; char c = 6;
 (void) s; (void) c; return KDGETMODE + VT_GETSTATE + TIOCLINUX;]])],
    [xwpe_cv_linux_console=yes], [xwpe_cv_linux_console=no])])
WPE_FACT([LINUX_CONSOLE], [test "x$xwpe_cv_linux_console" = xyes], [the system has the Linux console ioctls])
xwpe_say_console=$xwpe_cv_linux_console
AS_IF([test "x$xwpe_cv_linux_console" != xyes],
  [xwpe_say_console="no (no console keyboard probe, no GPM console check)"])

# The time fields of struct stat: POSIX 2008 names them st_mtim and st_ctim,
# macOS st_mtimespec and st_ctimespec.  The built-outputs store keeps the
# nanoseconds; where neither exists it keeps whole seconds.
AC_CHECK_MEMBERS([struct stat.st_mtim, struct stat.st_ctim,
                  struct stat.st_mtimespec, struct stat.st_ctimespec],
                 [], [], [[#include <sys/types.h>
#include <sys/stat.h>]])
AS_IF([test "x$ac_cv_member_struct_stat_st_mtim" = xyes],
  [xwpe_say_stat="st_mtim (nanoseconds)"],
  [test "x$ac_cv_member_struct_stat_st_mtimespec" = xyes],
  [xwpe_say_stat="st_mtimespec (nanoseconds)"],
  [xwpe_say_stat="whole seconds only (a build in the same second as an edit looks current)"])

# Whether a regular expression understands \< \> and \b.  glibc does by
# itself; macOS needs REG_ENHANCED and some others REG_GNU; FreeBSD's and
# OpenBSD's engines have no such escapes.  WPE_REGEX_WORD_ESCAPES is the
# flag the search owner passes (0 where none is needed or none exists) and
# WPE_HAS_REGEX_WORD_ESCAPES says whether the engine then understands the
# escape, measured by running it.
AC_CHECK_DECLS([REG_ENHANCED, REG_GNU], [], [], [[#include <sys/types.h>
#include <regex.h>]])
AS_IF([test "x$ac_cv_have_decl_REG_ENHANCED" = xyes],
  [xwpe_regex_flag=REG_ENHANCED],
  [test "x$ac_cv_have_decl_REG_GNU" = xyes],
  [xwpe_regex_flag=REG_GNU],
  [xwpe_regex_flag=0])
AC_DEFINE_UNQUOTED([WPE_REGEX_WORD_ESCAPES], [$xwpe_regex_flag],
  [The regcomp flag that turns on the word-boundary escapes, or 0])
AC_CACHE_CHECK([whether regcomp has word-boundary escapes (flag $xwpe_regex_flag)],
  [xwpe_cv_regex_word_escapes],
  [AC_RUN_IFELSE([AC_LANG_PROGRAM([[#include <sys/types.h>
#include <regex.h>]], [[regex_t re; regmatch_t m[1];
 if (regcomp(&re, "\\\\<ab", REG_EXTENDED | $xwpe_regex_flag)) return 1;
 if (regexec(&re, " ab", 1, m, 0)) return 1;
 return regexec(&re, "xab", 1, m, 0) == 0;]])],
    [xwpe_cv_regex_word_escapes=yes], [xwpe_cv_regex_word_escapes=no],
    [AS_IF([test "x$xwpe_regex_flag" = x0],
       [xwpe_cv_regex_word_escapes="no (cross build, no flag known)"],
       [xwpe_cv_regex_word_escapes=yes])])])
WPE_FACT([REGEX_WORD_ESCAPES], [test "x$xwpe_cv_regex_word_escapes" = xyes],
  [the regular expression engine has word-boundary escapes])
AS_IF([test "x$xwpe_cv_regex_word_escapes" = xyes],
  [xwpe_say_regex="yes (flag: $xwpe_regex_flag)"],
  [xwpe_say_regex="no (a word-boundary escape in Find matches nothing)"])

# The math library.  we_render_cairo.c calls floor and ceil.  Linux and
# FreeBSD find them through libcairo's own dependency on libm, but OpenBSD's
# linker does not look through a dependency (the same reason freetype2 is
# named below), so libm is named directly where it is a library of its own.
# It goes into LIBM (a we_LDADD slot), not the global LIBS.
ace_save_LIBS="$LIBS"
LIBM=
AC_SEARCH_LIBS([floor], [m],
  [xwpe_say_libm="in libc"
   AS_IF([test "x$ac_cv_search_floor" != "xnone required"],
     [LIBM="$ac_cv_search_floor"; xwpe_say_libm="$LIBM"])],
  [xwpe_say_libm="not found: the link fails on floor and ceil"])
LIBS="$ace_save_LIBS"
AC_SUBST([LIBM])

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
# keybound() (an ncurses extension) gives the bytes behind a key code, so a
# modified key that terminfo files under another key's capability (st's
# Ctrl-End is ked) is decoded by its bytes; without it those keys do nothing.
AC_CHECK_FUNCS([keybound])
LIBS="$ace_save_LIBS"

# --- json-c: wire format for the DAP debug client (Go/Rust/Scala) ---
PKG_CHECK_MODULES([JSONC], [json-c], [],
  [AC_MSG_ERROR([json-c is required for the DAP debug client. Install libjson-c-dev.])])

# --- Optional: build the X11 binaries against the libx11-compat shim ---
# libx11-compat (codeberg.org/mendezr/xwpe's upstream shim; canonical at
# github.com/sysprog21/libx11-compat) is a drop-in X11->SDL translation layer:
# it provides libX11/libXft/libXext on top of SDL2, so the ordinary X11 build
# runs natively on macOS (pixel-exact Retina, live-resize, mixed-DPI) with NO
# XQuartz, and on any host with SDL2.  Point --with-libx11-compat at a prefix
# whose include/ has the X11 + Xft headers and lib/ has libX11.so, libXft.so and
# libXext.so (the shim installs these as aliases of libX11-compat.so etc.).  The
# shim renders through Xft only, so this path skips the Cairo/Pango/vterm stack
# (and, transitively, the native Wayland backend).  xwpe already carries the
# optional shim hooks (the live-resize reflow registration in we_xterm.c and the
# _LIBX11_COMPAT_HIDPI_SCALE atom read in WeXterm.c); they are inert against a
# real libX11 and activate automatically when the shim provides the symbols.
AC_ARG_WITH([libx11-compat],
  [AS_HELP_STRING([--with-libx11-compat@<:@=PREFIX@:>@],
     [Build the X11 binaries against the libx11-compat X11->SDL shim (native macOS, no XQuartz)])],
  [], [with_libx11_compat=no])

libx11_compat_prefix=
AS_IF([test "x$with_libx11_compat" != xno], [
  AS_IF([test "x$with_libx11_compat" = xyes],
    [AC_MSG_ERROR([--with-libx11-compat needs a prefix: --with-libx11-compat=/path/to/libx11-compat-install])],
    [libx11_compat_prefix="$with_libx11_compat"])
  AS_IF([test ! -d "$libx11_compat_prefix/include" -o ! -d "$libx11_compat_prefix/lib"],
    [AC_MSG_ERROR([libx11-compat prefix '$libx11_compat_prefix' must contain include/ and lib/])])
])

# --- Optional: X11 ---
AC_ARG_WITH([x],
  [AS_HELP_STRING([--without-x], [Disable X11 support])],
  [], [with_x=yes])

have_x11=no
x11_backend="none (X11 disabled)"
AS_IF([test "x$with_x" != xno], [
  AS_IF([test "x$with_libx11_compat" != xno], [
    # Link the X11 build against the shim instead of the system/XQuartz libX11.
    have_x11=yes
    x11_backend="libx11-compat shim ($libx11_compat_prefix)"
    X_CFLAGS="-I$libx11_compat_prefix/include"
    X_LIBS="-L$libx11_compat_prefix/lib -Wl,-rpath,$libx11_compat_prefix/lib -lX11 -lXext"
  ], [
    AC_PATH_XTRA
    AS_IF([test "x$no_x" != xyes], [
      have_x11=yes
      x11_backend="system libX11"
      X_LIBS="$X_LIBS $X_PRE_LIBS -lX11 $X_EXTRA_LIBS"
    ])
  ])
])
AC_SUBST([X_CFLAGS])
AC_SUBST([X_LIBS])

AS_IF([test "x$have_x11" != xyes], [
  AC_DEFINE([NO_XWINDOWS], [1], [Define if X11 support is not available])
])
AM_CONDITIONAL([HAVE_X11], [test "x$have_x11" = xyes])

# --- Optional: Xft (antialiased font rendering, requires X11) ---
have_xft=no
AS_IF([test "x$have_x11" = xyes], [
  AS_IF([test "x$with_libx11_compat" != xno], [
    # The shim provides Xft (libXft.so -> libXft-compat.so); take it from there
    # instead of pkg-config, which would find the system/XQuartz Xft.
    have_xft=yes
    XFT_CFLAGS="-I$libx11_compat_prefix/include"
    XFT_LIBS="-L$libx11_compat_prefix/lib -Wl,-rpath,$libx11_compat_prefix/lib -lXft"
    AC_DEFINE([HAVE_XFT], [1], [Define if Xft is available])
    AC_SUBST([XFT_CFLAGS])
    AC_SUBST([XFT_LIBS])
  ], [
    PKG_CHECK_MODULES([XFT], [xft fontconfig], [
      have_xft=yes
      AC_DEFINE([HAVE_XFT], [1], [Define if Xft is available])
    ], [have_xft=no])
  ])
])

# --- Optional: Cairo + Pango (vector rendering, requires X11) ---
# freetype2 is listed explicitly: we_render_cairo.c calls the FreeType API
# directly (FT_Get_Char_Index, cairo_ft_scaled_font_lock_face), so libfreetype
# must be on the link line.  Xft/Cairo pull it transitively, but a strict
# linker (Fedora's -Wl,--no-undefined-ish ld) requires the DSO named directly.
have_cairo=no
AS_IF([test "x$have_x11" = xyes && test "x$with_libx11_compat" = xno], [
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
# The key-repeat timer is a timerfd: a system without one cannot build the
# backend, and says so by name instead of failing in the compiler.
xwpe_wayland_probe=$with_wayland
AS_IF([test "x$with_wayland" != xno && test "x$xwpe_cv_timerfd" != xyes], [
  AS_IF([test "x$with_wayland" = xyes],
    [AC_MSG_ERROR([Wayland requested but this system has no timerfd (sys/timerfd.h)])],
    [AC_MSG_NOTICE([the native Wayland backend is skipped: this system has no timerfd])])
  xwpe_wayland_probe=no
])
AS_IF([test "x$xwpe_wayland_probe" != xno], [
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
# Cairo/Xft backend, so it is only useful on a graphical display (X11 or
# Wayland; a Wayland build also links the X11 objects).  Without it the
# Alt-F5 User Screen falls back to focusing the Messages panel (1.6.3 behaviour).
have_vterm=no
xwpe_say_vterm=no
AS_IF([test "x$have_x11" = xyes && test "x$with_libx11_compat" = xno], [
  # NetBSD pkgsrc names the module 'vterm03' (versioned); everywhere else it is
  # 'vterm'. Try both.
  PKG_CHECK_MODULES([VTERM], [vterm], [
    have_vterm=yes
    AC_DEFINE([HAVE_VTERM], [1], [Define if libvterm is available])
  ], [PKG_CHECK_MODULES([VTERM], [vterm03], [
       have_vterm=yes
       AC_DEFINE([HAVE_VTERM], [1], [Define if libvterm is available])
     ], [have_vterm=no])])
  xwpe_say_vterm=$have_vterm
], [
  # A probe that did not run says why, instead of printing a "no" that
  # looks like a library that is missing.
  AS_IF([test "x$have_x11" != xyes],
    [xwpe_say_vterm="not probed (needs X11)"],
    [xwpe_say_vterm="not probed (the libx11-compat build has no Cairo stack)"])
])
AM_CONDITIONAL([HAVE_VTERM], [test "x$have_vterm" = xyes])

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
AM_CONDITIONAL([HAVE_LIBGPM], [test "x$have_gpm" = xyes])

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
# we_platform.h picks the right one from these HAVE_*_H defines, so no
# other file names a header that exists on only some systems.
AC_CHECK_HEADERS([pty.h util.h libutil.h])
AS_IF([test "x$ac_cv_header_pty_h" = xyes], [xwpe_say_pty="pty.h"],
  [test "x$ac_cv_header_util_h" = xyes], [xwpe_say_pty="util.h"],
  [test "x$ac_cv_header_libutil_h" = xyes], [xwpe_say_pty="libutil.h"],
  [xwpe_say_pty="none (openpty is used undeclared)"])

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
AC_CHECK_HEADERS([execinfo.h])
WPE_FACT([BACKTRACE],
  [test "x$ac_cv_header_execinfo_h" = xyes && test "x$ac_cv_search_backtrace" != xno],
  [the system can write a backtrace (execinfo.h and backtrace)])
AS_IF([test "x$wpe_fact_BACKTRACE" = x1],
  [xwpe_say_backtrace="yes${EXECINFO_LIBS:+ ($EXECINFO_LIBS)}"],
  [xwpe_say_backtrace="no (the crash file carries the signal only)"])

# --- Required: pthread_kill() / pthread_sigmask() ---
# The SIGCHLD handler hands a signal that a library's thread took (Pango's
# font loader) to the editor's own thread.  Both calls are in libc on glibc
# 2.34+, macOS and the BSDs (FreeBSD's libc stubs give way to libthr when a
# library pulls it in), and in libpthread on older glibc.  Routed through
# PTHREAD_LIBS like UTIL_LIBS.
ace_save_LIBS="$LIBS"
AC_SEARCH_LIBS([pthread_kill], [pthread],
  [AS_IF([test "x$ac_cv_search_pthread_kill" = "xnone required"],
     [PTHREAD_LIBS=""], [PTHREAD_LIBS="$ac_cv_search_pthread_kill"])],
  [AC_MSG_ERROR([pthread_kill() is required (libc, or libpthread on older glibc).])])
LIBS="$ace_save_LIBS"
AC_SUBST([PTHREAD_LIBS])

# --- Required: zlib ---
AC_CHECK_LIB([z], [inflate], [],
  [AC_MSG_ERROR([zlib is required. Install zlib1g-dev or zlib-devel.])])

# --- A name put where none is (we_fileop.c) ---
# A copy or a move that is not to replace a name must not replace one that
# appeared meanwhile.  The owner asks the system for a rename that refuses:
# renameat2 with RENAME_NOREPLACE (Linux, glibc 2.28 and musl) or
# renameatx_np with RENAME_EXCL (macOS); where neither exists it links and
# unlinks, and as the last resort looks before it renames.
AC_CACHE_CHECK([for renameat2 with RENAME_NOREPLACE], [xwpe_cv_renameat2_noreplace],
  [AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <stdio.h>
#include <fcntl.h>]], [[return renameat2(AT_FDCWD, "a", AT_FDCWD, "b", RENAME_NOREPLACE);]])],
    [xwpe_cv_renameat2_noreplace=yes], [xwpe_cv_renameat2_noreplace=no])])
AS_IF([test "x$xwpe_cv_renameat2_noreplace" = xyes],
  [AC_DEFINE([HAVE_RENAMEAT2_NOREPLACE], [1], [Define if renameat2 takes RENAME_NOREPLACE])])
AC_CACHE_CHECK([for renameatx_np with RENAME_EXCL], [xwpe_cv_renameatx_np_excl],
  [AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <stdio.h>
#include <fcntl.h>]], [[return renameatx_np(AT_FDCWD, "a", AT_FDCWD, "b", RENAME_EXCL);]])],
    [xwpe_cv_renameatx_np_excl=yes], [xwpe_cv_renameatx_np_excl=no])])
AS_IF([test "x$xwpe_cv_renameatx_np_excl" = xyes],
  [AC_DEFINE([HAVE_RENAMEATX_NP_EXCL], [1], [Define if renameatx_np takes RENAME_EXCL])])

# --- The search over a row (we_rowfind.c) ---
# REG_STARTEND lets the engine see the whole row and start inside it, so a
# word boundary looks at the byte before the start; musl has no such flag and
# the owner then matches on a copy of the row.  newlocale/uselocale let the
# owner run the engine under a UTF-8 ctype when the process locale is not
# UTF-8, so the engine and the editor agree on what a character is.
AC_CHECK_DECLS([REG_STARTEND], [], [], [[#include <sys/types.h>
#include <regex.h>]])
AC_CHECK_FUNCS([newlocale uselocale])

# --- A debugger that dies with the editor ---
# Linux has prctl(PR_SET_PDEATHSIG) and FreeBSD procctl(PROC_PDEATHSIG_CTL):
# the system sends the child a signal when its parent is killed.  The other
# systems have no such call.  we_platform.h picks the call from the
# declarations found here.
AC_CHECK_HEADERS([sys/prctl.h sys/procctl.h])
AC_CHECK_DECLS([PR_SET_PDEATHSIG], [], [],
  [[#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif]])
AC_CHECK_DECLS([PROC_PDEATHSIG_CTL], [], [],
  [[#include <sys/types.h>
#ifdef HAVE_SYS_PROCCTL_H
#include <sys/procctl.h>
#endif]])
AS_IF([test "x$ac_cv_have_decl_PR_SET_PDEATHSIG" = xyes],
  [xwpe_say_pdeath=prctl],
  [test "x$ac_cv_have_decl_PROC_PDEATHSIG_CTL" = xyes],
  [xwpe_say_pdeath=procctl],
  [xwpe_say_pdeath="none (a debugger outlives an editor that is killed outright)"])
WPE_FACT([PARENT_DEATH_SIGNAL],
  [test "x$xwpe_say_pdeath" = xprctl || test "x$xwpe_say_pdeath" = xprocctl],
  [the system signals a child when its parent dies])

# --- Optional: WPE_TRACE debug logging (off by default, zero-cost) ---
AC_ARG_ENABLE([trace],
  [AS_HELP_STRING([--enable-trace],
     [enable the WPE_TRACE debug logging facility (default: no)])],
  [], [enable_trace=no])
AS_IF([test "x$enable_trace" = "xyes"],
  [AC_DEFINE([WPE_TRACE], [1], [Enable the WPE_TRACE debug logging facility])])

# --- Optional: AI assistant integration (off by default) ---
# A local-first (Ollama / OpenAI-compatible / Claude) assistant grafted onto the
# existing fd-loop + LSP spine.  Default: no -- a build without it is byte-for-byte
# the classic editor, so distributions and AI-averse users are unaffected.  json-c
# (already required above) covers the wire format and the HTTP client is in-tree,
# so the plain-HTTP backends (Ollama and localhost OpenAI-compatible servers) add
# no new dependency.
AC_ARG_ENABLE([ai],
  [AS_HELP_STRING([--enable-ai],
     [build the optional AI assistant (Chat/Edit/Agent; default: no)])],
  [], [enable_ai=no])
AS_IF([test "x$enable_ai" = "xyes"],
  [AC_DEFINE([WPE_AI], [1], [Enable the optional AI assistant integration])])
AM_CONDITIONAL([WPE_AI], [test "x$enable_ai" = "xyes"])

# --- TLS for the cloud AI backends (Claude / remote OpenAI / Groq / ...) ---
# Real OpenAI-compatible providers are HTTPS, so TLS is ON BY DEFAULT whenever
# the AI assistant is built and a TLS library is present: it is auto-detected
# (libtls -- LibreSSL, the BSD default -- preferred, OpenSSL fallback).  Without
# it the assistant reaches only plain-HTTP backends (Ollama, a local bridge).
# --disable-ai-tls builds without it; --enable-ai-tls forces it (and errors if no
# library is found, rather than silently dropping HTTPS).
have_ai_tls=no
AITLS_CFLAGS=""
AITLS_LIBS=""
AS_IF([test "x$enable_ai" = "xyes"], [
  AC_ARG_ENABLE([ai-tls],
    [AS_HELP_STRING([--enable-ai-tls],
       [TLS for HTTPS AI backends like Claude/Groq/OpenAI (default: auto -- on when libtls or openssl is present; use --disable-ai-tls to force off)])],
    [], [enable_ai_tls=auto])
  AS_IF([test "x$enable_ai_tls" != "xno"], [
    PKG_CHECK_MODULES([AITLS], [libtls], [
      have_ai_tls=yes; ai_tls_impl=libtls
      AC_DEFINE([WPE_AI_TLS], [1], [Enable TLS for cloud AI backends])
      AC_DEFINE([WPE_AI_TLS_LIBTLS], [1], [Use libtls (LibreSSL) for AI TLS])
    ], [PKG_CHECK_MODULES([AITLS], [openssl], [
         have_ai_tls=yes; ai_tls_impl=openssl
         AC_DEFINE([WPE_AI_TLS], [1], [Enable TLS for cloud AI backends])
         AC_DEFINE([WPE_AI_TLS_OPENSSL], [1], [Use OpenSSL for AI TLS])
       ], [
         AS_IF([test "x$enable_ai_tls" = "xyes"],
           [AC_MSG_ERROR([--enable-ai-tls requires libtls (LibreSSL/libretls) or openssl])],
           [AC_MSG_WARN([no libtls/openssl found -- the AI assistant will reach only plain-HTTP backends; install libtls-dev or libssl-dev for HTTPS (Claude/Groq/OpenAI)])])
       ])])
  ])
])
AM_CONDITIONAL([WPE_AI_TLS], [test "x$have_ai_tls" = xyes])
AC_SUBST([AITLS_CFLAGS])
AC_SUBST([AITLS_LIBS])

# --- Optional: external agent host (run an agent CLI like Claude Code in-editor) ---
# A generic host for an external coding-agent CLI that speaks stream-json (Claude
# Code first; aider/opencode/etc. as adapters later).  The real agent runs its own
# tools; xwpe renders its stream, reflects its on-disk edits into open buffers, and
# gates it via the permission dial.  Nested under --enable-ai, default no; needs no
# extra library (the agent CLI is spawned as a subprocess).
have_ai_agent_host=no
AS_IF([test "x$enable_ai" = "xyes"], [
  AC_ARG_ENABLE([ai-agent-host],
    [AS_HELP_STRING([--enable-ai-agent-host],
       [host an external agent CLI (Claude Code) inside the editor (default: no)])],
    [], [enable_ai_agent_host=no])
  AS_IF([test "x$enable_ai_agent_host" = "xyes"], [
    have_ai_agent_host=yes
    AC_DEFINE([WPE_AI_AGENT_HOST], [1], [Enable hosting an external agent CLI])
  ])
])
AM_CONDITIONAL([WPE_AI_AGENT_HOST], [test "x$have_ai_agent_host" = "xyes"])

# --- Optional: the xdb debugger backend (off by default) ---
# xdb ships with HP-UX and runs nowhere else: it is not in Debian and there is
# no free implementation, so the row it would take in the debugger dialog names
# a backend no user of this build can reach.  Off by default it leaves that row
# to dbx; --with-xdb builds the backend and gives both a row of their own.
AC_ARG_WITH([xdb],
  [AS_HELP_STRING([--with-xdb],
     [offer HP-UX's xdb as a debugger backend (default: no -- xdb is HP-UX's and unavailable elsewhere)])],
  [], [with_xdb=no])
AS_IF([test "x$with_xdb" = "xyes"],
  [AC_DEFINE([XDB], [1], [Offer HP-UX's xdb as a debugger backend])])

# --- What this build contains, as facts every reader sees the same way ---
# config.h defines each WPE_BUILT_* to 0 or 1 (never left undefined), the
# editor prints them (--version), and the feature lists of Makefile.am and the
# test suite use the same words.  The word is the configure option without its
# leading dashes, or the library's name where configure finds it on its own.
# Test a fact with "#if WPE_BUILT_X", never "#ifdef": it is always defined, so
# a misspelt name is a -Wundef warning and never a silent zero.
AC_DEFUN([WPE_BUILT],
  [AS_IF([$3], [wpe_built_$1=1; wpe_word_$1=yes], [wpe_built_$1=0; wpe_word_$1=no])
   AC_DEFINE_UNQUOTED([WPE_BUILT_$1], [$wpe_built_$1],
     [1 when this build has $2, 0 when it does not])
   wpe_built_line="$wpe_built_line $2=$wpe_word_$1"])
wpe_built_line=
WPE_BUILT([AI],      [enable-ai],            [test "x$enable_ai" = xyes])
WPE_BUILT([AI_HOST], [enable-ai-agent-host], [test "x$have_ai_agent_host" = xyes])
WPE_BUILT([AI_TLS],  [enable-ai-tls],        [test "x$have_ai_tls" = xyes])
WPE_BUILT([X11],     [with-x],               [test "x$have_x11" = xyes])
WPE_BUILT([WAYLAND], [with-wayland],         [test "x$have_wayland" = xyes])
WPE_BUILT([GPM],     [with-gpm],             [test "x$have_gpm" = xyes])
WPE_BUILT([XDB],     [with-xdb],             [test "x$with_xdb" = xyes])
WPE_BUILT([TRACE],   [enable-trace],         [test "x$enable_trace" = xyes])
WPE_BUILT([VTERM],   [libvterm],             [test "x$have_vterm" = xyes])
WPE_BUILT([XFT],     [xft],                  [test "x$have_xft" = xyes])
WPE_BUILT([CAIRO],   [cairo],                [test "x$have_cairo" = xyes])
WPE_BUILT([PANGO],   [pango],                [test "x$have_pango" = xyes])

# --- Unconditional defines ---
AC_DEFINE([UNIX], [1], [Building on a Unix system])
AC_DEFINE([NCURSES], [1], [Using ncurses library])
AC_DEFINE([_XOPEN_SOURCE_EXTENDED], [1], [Enable wide character curses support])
AC_DEFINE([NCURSES_WIDECHAR], [1], [Enable ncurses wide character API])
AC_DEFINE([SELECTION], [1], [Use X Selection events])

# --- Print command (configurable) ---
AC_ARG_VAR([PRNTCMD], [Print command (default: lpr)])
test -z "$PRNTCMD" && PRNTCMD="lpr"
AC_DEFINE_UNQUOTED([PRNTCMD], ["$PRNTCMD"], [Command to print files])

# --- Feature macros that the libraries' own flags carry ---
# A library's pkg-config flags may carry feature macros of their own (Debian's
# ncursesw.pc: -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=600; NetBSD's:
# -D_NETBSD_SOURCE; Homebrew's: -D_DARWIN_C_SOURCE).  config.h's macros win
# over them on glibc, macOS and NetBSD.  FreeBSD and OpenBSD have no such
# override, so a strict macro from a library file hides declarations the
# editor uses.  The flags are not stripped (the library's headers may need
# them); configure lists them and tries a declaration they could hide.
xwpe_libmacros=
xwpe_libflags=
for ace_lib in NCURSESW JSONC X XFT CAIRO PANGO WAYLAND VTERM AITLS; do
  eval ace_flags=\$${ace_lib}_CFLAGS
  for ace_f in $ace_flags; do
    case $ace_f in
      -D_*_SOURCE|-D_*_SOURCE=*|-D__EXTENSIONS__|-D__BSD_VISIBLE|-D__BSD_VISIBLE=*)
        xwpe_libmacros="$xwpe_libmacros $ace_f ($ace_lib)"
        xwpe_libflags="$xwpe_libflags $ace_f" ;;
    esac
  done
done
xwpe_say_libmacros=none
AS_IF([test -n "$xwpe_libflags"], [
  ace_save_CPPFLAGS="$CPPFLAGS"
  ace_save_CFLAGS="$CFLAGS"
  CPPFLAGS="$CPPFLAGS $xwpe_libflags"
  CFLAGS="$CFLAGS $XWPE_DECLFLAG"
  AC_MSG_CHECKING([whether the libraries' feature macros hide declarations the editor uses])
  AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[#include <stdlib.h>]],
      [[char t[] = "/tmp/xwpeXXXXXX"; return mkdtemp(t) == 0;]])],
    [AC_MSG_RESULT([no])
     xwpe_say_libmacros="${xwpe_libmacros# } (config.h's macros override them)"],
    [AC_MSG_RESULT([yes])
     xwpe_say_libmacros="${xwpe_libmacros# } WARNING: they hide declarations the editor uses on this system"
     AC_MSG_WARN([a library's feature macros ($xwpe_libflags) hide declarations on this system; the build may fail on an undeclared function])])
  CPPFLAGS="$ace_save_CPPFLAGS"
  CFLAGS="$ace_save_CFLAGS"
])

# --- Output ---
AC_CONFIG_FILES([Makefile docs/Makefile])
AC_OUTPUT

AC_MSG_RESULT([
  xwpe $PACKAGE_VERSION configuration:
    ncursesw:  yes (via pkg-config)
    X11:       $have_x11
    X11 backend: $x11_backend
    Xft:       $have_xft
    Cairo:     $have_cairo
    Pango:     $have_pango
    Wayland:   $have_wayland
    libvterm:  $xwpe_say_vterm
    GPM:       $have_gpm
    AI:        $enable_ai
    AI TLS:    $have_ai_tls
    AI host:   $have_ai_agent_host
    built with:$wpe_built_line
    PRNTCMD:   $PRNTCMD
    pandoc:    $PANDOC

    platform:
      math library:      $xwpe_say_libm
      warning flags:     $xwpe_say_warn
      format checks:     $XWPE_FORMATFLAGS
      declaration check: $xwpe_say_decl
      library feature macros: $xwpe_say_libmacros
      parent-death signal: $xwpe_say_pdeath
      memfd_create:      $xwpe_say_memfd
      close_range:       $xwpe_say_close_range
      closefrom:         $xwpe_say_closefrom
      timerfd:           $xwpe_say_timerfd
      linux console:     $xwpe_say_console
      openpty header:    $xwpe_say_pty
      backtrace:         $xwpe_say_backtrace
      stat times:        $xwpe_say_stat
      regex word escapes: $xwpe_say_regex
      linker --wrap:     $have_ld_wrap
])
