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

# The flag that makes an executable export its own functions to the dynamic
# linker, so a C test's hook of a C library function (tests/interpose.h) can
# be found by dladdr under the name the compiler bound it to.  GNU ld, lld,
# and the BSDs' ld take -Wl,-E or -rdynamic; Mach-O exports globals already
# and takes neither, so it measures to nothing.  Each candidate is linked
# with a program that asks dladdr for its own function's exported name.
AC_MSG_CHECKING([for the flag that exports a program's functions to dladdr])
TEST_EXPORT_DYNAMIC=
ace_save_LDFLAGS="$LDFLAGS"
ace_found_export=no
for ace_try in "" "-Wl,-E" "-rdynamic"; do
  LDFLAGS="$ace_save_LDFLAGS $ace_try"
  AC_RUN_IFELSE(
    [AC_LANG_PROGRAM([[#include <dlfcn.h>
#include <string.h>
int ace_exported_probe(void) { return 0; }]],
       [[Dl_info info;
         if (dladdr((void *)ace_exported_probe, &info) == 0 || info.dli_sname == NULL)
           return 1;
         return strcmp(info.dli_sname, "ace_exported_probe") != 0
                || info.dli_saddr != (void *)ace_exported_probe;]])],
    [ace_found_export=yes; TEST_EXPORT_DYNAMIC="$ace_try"],
    [], [ace_found_export=no])
  test "x$ace_found_export" = xyes && break
done
LDFLAGS="$ace_save_LDFLAGS"
if test "x$ace_found_export" = xyes; then
  AC_MSG_RESULT([${TEST_EXPORT_DYNAMIC:-none needed}])
else
  AC_MSG_RESULT([none works])
fi
AC_SUBST([TEST_EXPORT_DYNAMIC])

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

# What the owner of "is this the object I saw" (we_seen.c) can ask a system:
# the birth time of a file (statx and its STATX_BTIME on Linux, st_birthtim on
# FreeBSD and NetBSD, st_birthtimespec on macOS).  A system with none of them
# still builds: a touched file is then "not proven the same" and the user is
# asked.  STATX_BTIME is declared from glibc 2.28; older headers have no
# struct statx.
AC_CHECK_MEMBERS([struct stat.st_birthtim, struct stat.st_birthtimespec],
                 [], [], [[#include <sys/types.h>
#include <sys/stat.h>]])
AC_CHECK_FUNCS([statx])
AC_CHECK_DECLS([STATX_BTIME], [], [], [[#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>]])
xwpe_say_birth="no birth time: a file touched in place asks before Save"
AS_IF([test "x$ac_cv_func_statx" = xyes && test "x$ac_cv_have_decl_STATX_BTIME" = xyes],
  [xwpe_say_birth="statx (a file touched in place is still told from a new one)"],
  [test "x$ac_cv_member_struct_stat_st_birthtim" = xyes],
  [xwpe_say_birth="st_birthtim (a file touched in place is still told from a new one)"],
  [test "x$ac_cv_member_struct_stat_st_birthtimespec" = xyes],
  [xwpe_say_birth="st_birthtimespec (a file touched in place is still told from a new one)"])

# The name of the file system a test ran on (tests/seen_rows.h): Linux says it
# in f_type of statfs (<sys/vfs.h>), the BSDs and macOS in f_fstypename
# (<sys/mount.h>).
AC_CHECK_HEADERS([sys/vfs.h])
AC_CHECK_MEMBERS([struct statfs.f_fstypename], [], [], [[#include <sys/param.h>
#include <sys/mount.h>]])

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

# The GNU escapes \b \B \w \s are a separate fact from \< \>: FreeBSD's and
# OpenBSD's engines have \< \> and none of these, and the two systems differ
# in what they do with one.  So each escape is run, and the engine's answer
# is one of three: understood, refused by regcomp (Find then says "bad
# expression"), or taken as the letter itself (Find looks for a "b").  The
# fact is 1 only when all four are understood.  A cross build cannot run the
# probe and says 0.
AC_CACHE_CHECK([what regcomp does with the GNU escapes (flag $xwpe_regex_flag)],
  [xwpe_cv_regex_gnu_escapes],
  [AS_IF([test "x$cross_compiling" = xyes],
     [xwpe_cv_regex_gnu_escapes="unknown (cross build)"],
     [AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <stdio.h>
#include <sys/types.h>
#include <regex.h>
static const char *probe(const char *pat, const char *yes, const char *no, const char *lit)
{
 regex_t re;
 regmatch_t m[1];
 const char *answer;
 if (regcomp(&re, pat, REG_EXTENDED | $xwpe_regex_flag)) return "refused";
 if (regexec(&re, yes, 1, m, 0) == 0 && regexec(&re, no, 1, m, 0) != 0) answer = "understood";
 else if (regexec(&re, lit, 1, m, 0) == 0) answer = "literal";
 else answer = "other";
 regfree(&re);
 return answer;
}]], [[printf("b=%s B=%s w=%s s=%s\n",
        probe("\\\\bab", " ab", "xab", "bab"), probe("\\\\Bab", "xab", " ab", "Bab"),
        probe("^\\\\w\$", "a", " ", "w"), probe("^\\\\s\$", " ", "a", "s"));
 return 0;]])],
       [xwpe_cv_regex_gnu_escapes=`./conftest$ac_exeext 2>/dev/null`
        test -n "$xwpe_cv_regex_gnu_escapes" || xwpe_cv_regex_gnu_escapes="unknown (the probe printed nothing)"],
       [xwpe_cv_regex_gnu_escapes="unknown (the probe did not build)"])])])
WPE_FACT([REGEX_GNU_ESCAPES],
  [test "x$xwpe_cv_regex_gnu_escapes" = "xb=understood B=understood w=understood s=understood"],
  [the regular expression engine understands the escapes \b \B \w \s])
AS_IF([test "x$wpe_fact_REGEX_GNU_ESCAPES" = x1],
  [xwpe_say_regex_gnu_escapes="yes (\\b \\B \\w \\s are understood)"],
  [xwpe_say_regex_gnu_escapes="no ($xwpe_cv_regex_gnu_escapes; refused is a bad expression in Find, literal looks for the letter)"])

# Whether the engine reads characters.  A UTF-8 locale existing is not that:
# OpenBSD has C.UTF-8 and its regex is single-byte whatever the locale says.
# So the engine is run: under a UTF-8 locale, does "." take the two bytes of
# an e acute as one character?  A cross build cannot run it and keeps the
# assumption the search owner always made, that it does.
AC_CACHE_CHECK([whether regcomp reads UTF-8 characters],
  [xwpe_cv_regex_multibyte],
  [AS_IF([test "x$cross_compiling" = xyes],
     [xwpe_cv_regex_multibyte="yes (assumed: cross build)"],
     [AC_RUN_IFELSE([AC_LANG_PROGRAM([[#include <locale.h>
#include <sys/types.h>
#include <regex.h>]], [[regex_t re; regmatch_t m[1];
 if (!setlocale(LC_CTYPE, "C.UTF-8") && !setlocale(LC_CTYPE, "en_US.UTF-8")) return 2;
 if (regcomp(&re, ".", REG_EXTENDED)) return 3;
 if (regexec(&re, "\\303\\251", 1, m, 0)) return 3;
 return m[0].rm_eo != 2;]])],
       [xwpe_cv_regex_multibyte=yes],
       [xwpe_cv_regex_multibyte=no])])])
WPE_FACT([REGEX_MULTIBYTE], [test "x$xwpe_cv_regex_multibyte" != xno],
  [the regular expression engine reads UTF-8 characters])
AS_IF([test "x$wpe_fact_REGEX_MULTIBYTE" = x1],
  [xwpe_say_regex_multibyte="$xwpe_cv_regex_multibyte (Find's Case sensitive off folds beyond ASCII)"],
  [xwpe_say_regex_multibyte="no (the engine reads bytes: Case sensitive off folds ASCII only, and a hit is kept to whole characters)"])

# Whether the engine survives a word-boundary escape.  NetBSD 10's regexec
# faults in mbrtowc on `\B(a|b)[ab]` over a row that holds a multibyte
# character (and with REG_ICASE answers a match far outside the row), so the
# class "an escape beside an alternation" is measured by running it, each shape
# in a child of the probe (a crash, a hang or an answer outside the range is
# then a word, not the end of configure).  Two facts come out of one run:
# WPE_HAS_REGEX_SAFE_BOUNDARY_GROUP, 1 when every shape and every control
# answers in range, and WPE_HAS_REGEX_SAFE_BOUNDARY, 0 only when a control (a
# shape outside the class: no alternation, a repeat, a longer group) fails,
# because then the escape alone is the danger.  A cross build, a host with no
# UTF-8 locale, or an answer that is none of ok, refused, range, crash and hang
# is not measured, and the group fact says 0.  A shape is also "wrong" when its
# answer differs from the same call over a row whose multibyte characters are
# plain letters (FreeBSD 14 misjudges \< beside a character, and crashes on
# \> . with no alternation at all, so those are controls).  The row is placed
# before an unmapped page so a read past it crashes.  macOS's regexec answers a
# start inside a character with REG_ILLSEQ; that answer to the one call that
# starts there (gt-dot-1) is readable and safe, because the editor never starts
# inside a character, and gt-dot-2 asks the same expression from the character
# start behind it.  Any other answer is judged as before.  The probe repeats the set under
# the C ctype and reports it; only the UTF-8 part is judged.
AC_CACHE_CHECK([what regexec does with a word-boundary escape beside |],
  [xwpe_cv_regex_boundary],
  [AS_IF([test "x$cross_compiling" = xyes],
     [xwpe_cv_regex_boundary="unknown (cross build)"],
     [AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <regex.h>

/* The rows: a space, U+00C3, "bb", and the 9 bytes of the call that killed
   the editor (tab, space, U+00C3, "bb", space, "ba"). */
#define ROW5 "\\040\\303\\203bb"
#define ROW9 "\\011\\040\\303\\203bb ba"
/* FreeBSD 14's walk() crashes on \\> . over U+00C3, space, "b" and answers \\< b
   wrong (a match at 3..4) over a space, U+00C3, "bb"; no alternation is needed. */
#define ROW4 "\\303\\203 b"

/* @midchar marks the shape whose first call starts inside a character (so is
   not a character start).  The editor never makes such a call, and macOS's
   regexec answers it with REG_ILLSEQ, so that one answer is readable and safe
   there and nowhere else (see one_call). */
struct shape { const char *name; const char *pat; int icase; const char *row; int len; int so; int eflags; int control; int midchar; };

static const struct shape shapes[] = {
 { "B-group", "\\\\B(a|b)[ab]", 0, ROW5, 5, 1, 0, 0 },
 { "B-group-b", "\\\\B(a|b)b", 0, ROW5, 5, 1, 0, 0 },
 { "b-group", "\\\\b(a|b)[ab]", 0, ROW5, 5, 1, 0, 0 },
 { "group-B", "(a|b)\\\\B", 0, ROW5, 5, 1, 0, 0 },
 { "group-b", "(a|b)\\\\b", 0, ROW5, 5, 1, 0, 0 },
 { "top-alt", "\\\\Ba|\\\\Bb", 0, ROW5, 5, 1, 0, 0 },
 { "lt-group", "\\\\<(a|b)\\\\>", 0, ROW5, 5, 1, 0, 0 },
 { "B-icase", "\\\\B(a|b)[ab]", REG_ICASE, ROW5, 5, 1, 0, 0 },
 { "editor-call", "\\\\B(a|b)[ab]", REG_ICASE, ROW9, 9, 1, REG_NOTBOL | REG_NOTEOL, 0 },
 { "ctl-group", "(a|b)[ab]", 0, ROW5, 5, 1, 0, 1 },
 { "ctl-B", "\\\\B(b)b", 0, ROW5, 5, 1, 0, 1 },
 { "ctl-B-opt", "\\\\B(b)?b", 0, ROW5, 5, 1, 0, 1 },
 { "ctl-B-star", "\\\\B(b)*b", 0, ROW5, 5, 1, 0, 1 },
 { "ctl-B-rep", "\\\\Bb*b", 0, ROW5, 5, 1, 0, 1 },
 { "ctl-B-seq", "\\\\B(ab)b", 0, ROW5, 5, 1, 0, 1 },
 { "gt-dot", "\\\\>.", 0, ROW4, 4, 0, 0, 1 },
 { "gt-dot-1", "\\\\>.", 0, ROW4, 4, 1, 0, 1, 1 },
 { "gt-dot-2", "\\\\>.", 0, ROW4, 4, 2, 0, 1, 0 },
 { "lt-b", "\\\\<b", 0, ROW5, 5, 0, 0, 1 },
 { "lt-b-1", "\\\\<b", 0, ROW5, 5, 1, 0, 1 },
 { "gt-br-dot", "[[:>:]].", 0, ROW4, 4, 0, 0, 1 },
 { "lt-br-b", "[[:<:]]b", 0, ROW5, 5, 0, 0, 1 }
};
#define SHAPES ((int) (sizeof shapes / sizeof shapes[0]))

#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif

/* The row put right before an unmapped page, with only the byte after it
   (row[len], a zero) readable, so a read past it is a crash.  With @swap each
   byte of a multibyte character is a letter that is in no pattern, so the
   word and non-word classes and the byte offsets stay the same. */
static const char *place_row(const char *row, int len, int swap)
{
 long page = sysconf(_SC_PAGESIZE);
 char *base;
 int i;
 base = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
 if (base == MAP_FAILED) _exit(3);
 if (mprotect(base + page, page, PROT_NONE) != 0) _exit(3);
 base += page - len - 1;
 for (i = 0; i < len; i++)
 {
  base[i] = row[i];
  if (swap && (unsigned char) row[i] >= 0x80) base[i] = 'x';
 }
 base[len] = 0;
 return base;
}

struct answer { int rc; long so; long eo; };

/* Under the C ctype, and on an engine that reads bytes whatever the locale
   (OpenBSD: WPE_HAS_REGEX_MULTIBYTE is 0), a byte of 0x80 or more is no
   letter, so the swap changes the classes by design and says nothing: there
   the probe does not compare.  The first value is the measured fact. */
static int compare_swapped = $wpe_fact_REGEX_MULTIBYTE;

/* One call, as the editor makes it (REG_STARTEND from @so to the row's end). */
static void ask(const regex_t *re, const struct shape *s, const char *row, int so, int eflags, struct answer *a)
{
 regmatch_t m;
 m.rm_so = so;
 m.rm_eo = s->len;
 a->rc = regexec(re, row, 1, &m, eflags | REG_STARTEND);
 a->so = m.rm_so;
 a->eo = m.rm_eo;
}

/* Whether @rc is the refusal of a start inside a character (macOS answers
   REG_ILLSEQ, 17).  A system that has no such code never answers it. */
static int is_illseq(int rc)
{
#ifdef REG_ILLSEQ
 return rc == REG_ILLSEQ;
#else
 (void) rc;
 return 0;
#endif
}

/* 0 an answer inside the range (or no match), 2 a match outside it, 3 any
   other answer, 4 an answer that differs from the one the same call gives
   when every byte of a multibyte character is a plain letter ("wrong": the
   engine misjudged a boundary beside a character, and a hit inside the range
   is no proof), 5 the refusal of a start inside a character, REG_ILLSEQ,
   given to the call @midchar says starts there.  The editor starts only on a
   character start (char_start_up) and takes any non-zero code as no match,
   so that answer to that call is safe; the same code to any other call, or
   any other code to that one, is judged as before. */
#define ONE_CALL_ILLSEQ 5
static int one_call(const regex_t *re, const struct shape *s, int so, int eflags, int midchar)
{
 struct answer a, b;
 ask(re, s, place_row(s->row, s->len, 0), so, eflags, &a);
 if (a.rc == REG_NOMATCH) a.so = a.eo = -1;
 else if (midchar && is_illseq(a.rc)) return ONE_CALL_ILLSEQ;
 else if (a.rc != 0) return 3;
 else if (a.so < so || a.so > a.eo || a.eo > s->len) return 2;
 if (!compare_swapped) return 0;
 ask(re, s, place_row(s->row, s->len, 1), so, eflags, &b);
 if (b.rc == REG_NOMATCH) b.so = b.eo = -1;
 if (a.rc != b.rc || a.so != b.so || a.eo != b.eo) return 4;
 return 0;
}

/* The child: no core file, five seconds, then the shape's two calls.  Its exit
   status is the answer: 0 ok, 42 refused by regcomp, 2 range, 3 other, 4 wrong,
   5 illseq (the mid-character call of a shape that has one, and nothing worse).
   Refused is 42 because a sanitizer that catches a crash in the child ends it
   with an exit status of its own (AddressSanitizer and UBSan 1, LeakSanitizer
   23), and that must read as a crash, not as a refusal. */
#define EXIT_REFUSED 42
static int child_answer(const struct shape *s)
{
 regex_t re;
 struct rlimit rl;
 int first, second;
 rl.rlim_cur = 0;
 rl.rlim_max = 0;
 setrlimit(RLIMIT_CORE, &rl);
 alarm(5);
 if (regcomp(&re, s->pat, REG_EXTENDED | s->icase | $xwpe_regex_flag)) return EXIT_REFUSED;
 first = one_call(&re, s, s->so, s->eflags, s->midchar);
 second = one_call(&re, s, 0, REG_NOTBOL | REG_NOTEOL, 0);
 if (first == 2 || second == 2) return 2;
 if (first == 3 || second == 3) return 3;
 if (first == 4 || second == 4) return 4;
 if (first == ONE_CALL_ILLSEQ) return ONE_CALL_ILLSEQ;
 return 0;
}

static const char *measure(const struct shape *s)
{
 pid_t pid;
 int st;
 fflush(stdout);
 pid = fork();
 if (pid < 0) return "fork-failed";
 if (pid == 0) _exit(child_answer(s));
 if (waitpid(pid, &st, 0) != pid) return "other";
 if (WIFSIGNALED(st) && WTERMSIG(st) == SIGALRM) return "hang";
 if (WIFSIGNALED(st)) return "crash";
 if (!WIFEXITED(st)) return "other";
 if (WEXITSTATUS(st) == 0) return "ok";
 if (WEXITSTATUS(st) == EXIT_REFUSED) return "refused";
 if (WEXITSTATUS(st) == 2) return "range";
 if (WEXITSTATUS(st) == 3) return "other";
 if (WEXITSTATUS(st) == 4) return "wrong";
 if (WEXITSTATUS(st) == ONE_CALL_ILLSEQ) return "illseq";
 return "crash";
}

/* An answer the probe could not read: another status, a failed wait, or no
   child at all.  It counts like "other" (both facts 0), and the summary names
   which. */
static int unreadable(const char *o)
{
 return strcmp(o, "other") == 0 || strcmp(o, "fork-failed") == 0;
}

static int failed(const char *o)
{
 return strcmp(o, "crash") == 0 || strcmp(o, "range") == 0 || strcmp(o, "hang") == 0 ||
        strcmp(o, "wrong") == 0;
}

static const char *verdict(int bad, int unknown)
{
 if (bad) return "unsafe";
 if (unknown) return "unknown";
 return "safe";
}

/* A shape that already hung is not run again for the report-only pass: a hang
   costs five seconds a call. */
static void measure_all(const char **answer, const char **before)
{
 int i;
 for (i = 0; i < SHAPES; i++)
 {
  if (before != 0 && strcmp(before[i], "hang") == 0) answer[i] = "skipped";
  else answer[i] = measure(&shapes[i]);
 }
}]], [[const char *answer[SHAPES], *again[SHAPES];
 int i, shape_bad = 0, shape_unknown = 0, control_bad = 0, control_unknown = 0;
 if (!setlocale(LC_CTYPE, "C.UTF-8") && !setlocale(LC_CTYPE, "en_US.UTF-8"))
 {
  printf("group=unknown boundary=unknown no-utf8-locale\\n");
  return 0;
 }
 measure_all(answer, 0);
 for (i = 0; i < SHAPES; i++)
 {
  if (shapes[i].control)
  {
   if (failed(answer[i])) control_bad = 1;
   if (unreadable(answer[i])) control_unknown = 1;
  }
  else
  {
   if (failed(answer[i])) shape_bad = 1;
   if (unreadable(answer[i])) shape_unknown = 1;
  }
 }
 printf("group=%s boundary=%s", verdict(shape_bad || control_bad, shape_unknown || control_unknown),
        verdict(control_bad, control_unknown));
 for (i = 0; i < SHAPES; i++) printf(" %s=%s", shapes[i].name, answer[i]);
 setlocale(LC_CTYPE, "C");
 compare_swapped = 0;
 measure_all(again, answer);
 printf(" C:");
 for (i = 0; i < SHAPES; i++) printf(" %s=%s", shapes[i].name, again[i]);
 printf("\\n");
 return 0;]])],
       [xwpe_cv_regex_boundary=`./conftest$ac_exeext 2>/dev/null`
        test -n "$xwpe_cv_regex_boundary" || xwpe_cv_regex_boundary="unknown (the probe printed nothing)"],
       [xwpe_cv_regex_boundary="unknown (the probe did not build)"])])])
AS_CASE([$xwpe_cv_regex_boundary],
  ["group=safe boundary=safe "*],
    [xwpe_boundary_group_ok=yes; xwpe_boundary_ok=yes],
  [*"boundary=unsafe "*],
    [xwpe_boundary_group_ok=no; xwpe_boundary_ok=no],
  ["group=unknown boundary=unknown no-utf8-locale"],
    [xwpe_boundary_group_ok=no; xwpe_boundary_ok=yes],
  [*"boundary=unknown "*],
    [xwpe_boundary_group_ok=no; xwpe_boundary_ok=no],
  ["group=unsafe boundary=safe "*],
    [xwpe_boundary_group_ok=no; xwpe_boundary_ok=yes],
  [xwpe_boundary_group_ok=no; xwpe_boundary_ok=yes])
WPE_FACT([REGEX_SAFE_BOUNDARY_GROUP], [test "x$xwpe_boundary_group_ok" = xyes],
  [the regular expression engine survives a word-boundary escape beside |])
WPE_FACT([REGEX_SAFE_BOUNDARY], [test "x$xwpe_boundary_ok" = xyes],
  [the regular expression engine survives a word-boundary escape alone])
AS_IF([test "x$xwpe_boundary_ok" != xyes],
  [xwpe_say_regex_safe_boundary="no ($xwpe_cv_regex_boundary; Find refuses any word-boundary escape)"],
  [xwpe_say_regex_safe_boundary="yes (\\b \\B \\< \\> alone are not refused)"])
AS_IF([test "x$xwpe_boundary_group_ok" = xyes],
  [xwpe_say_regex_safe_boundary_group="yes (\\b \\B \\< \\> beside | are safe)"],
  [test "x$xwpe_boundary_ok" != xyes],
  [xwpe_say_regex_safe_boundary_group="no (as above)"],
  [xwpe_say_regex_safe_boundary_group="no ($xwpe_cv_regex_boundary; Find refuses a word-boundary escape together with |)"])

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

# libX11 1.7 and later let a program give Xlib an exit handler that returns,
# so the editor can save the user's windows before it ends when the X server
# goes away.  Older libX11 and the libx11-compat shim may lack it: the I/O
# error handler then saves and exits itself.
AS_IF([test "x$have_x11" = xyes], [
  xwpe_save_LIBS=$LIBS
  LIBS="$X_LIBS $LIBS"
  AC_CHECK_FUNCS([XSetIOErrorExitHandler])
  LIBS=$xwpe_save_LIBS
])

AS_IF([test "x$have_x11" != xyes], [
  AC_DEFINE([NO_XWINDOWS], [1], [Define if X11 support is not available])
])
AM_CONDITIONAL([HAVE_X11], [test "x$have_x11" = xyes])

# --- Optional: Xft (antialiased font rendering, requires X11) ---
# Default: auto (use it when found).  --without-xft skips the probe, and the
# X11 side then runs the core font; --with-xft makes a missing library an
# error instead of a quiet "no".  The same two switches follow for Cairo.
AC_ARG_WITH([xft],
  [AS_HELP_STRING([--without-xft],
     [Do not use Xft: the X11 side runs the core font (default: auto)])],
  [], [with_xft=auto])
AC_ARG_WITH([cairo],
  [AS_HELP_STRING([--without-cairo],
     [Do not use Cairo and Pango; this also rules out the native Wayland
      backend (default: auto)])],
  [], [with_cairo=auto])

have_xft=no
AS_IF([test "x$with_xft" = xyes && test "x$have_x11" != xyes],
  [AC_MSG_ERROR([--with-xft needs X11, which this build does not have])])
AS_IF([test "x$have_x11" = xyes && test "x$with_xft" != xno], [
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
    ], [have_xft=no
        AS_IF([test "x$with_xft" = xyes],
          [AC_MSG_ERROR([--with-xft given but the xft and fontconfig development files were not found])])])
  ])
])

# --- Optional: Cairo + Pango (vector rendering, requires X11) ---
# freetype2 is listed explicitly: we_render_cairo.c calls the FreeType API
# directly (FT_Get_Char_Index, cairo_ft_scaled_font_lock_face), so libfreetype
# must be on the link line.  Xft/Cairo pull it transitively, but a strict
# linker (Fedora's -Wl,--no-undefined-ish ld) requires the DSO named directly.
# The same goes for fontconfig (FcDefaultSubstitute): without Xft it is no
# longer on the line through XFT_LIBS.
have_cairo=no
AS_IF([test "x$with_cairo" = xyes && test "x$have_x11" != xyes],
  [AC_MSG_ERROR([--with-cairo needs X11, which this build does not have])])
AS_IF([test "x$with_cairo" = xyes && test "x$with_libx11_compat" != xno],
  [AC_MSG_ERROR([--with-cairo cannot be combined with --with-libx11-compat: the shim build has no Cairo stack])])
AS_IF([test "x$have_x11" = xyes && test "x$with_libx11_compat" = xno && test "x$with_cairo" != xno], [
  PKG_CHECK_MODULES([CAIRO], [cairo cairo-xlib freetype2 fontconfig], [
    have_cairo=yes
    AC_DEFINE([HAVE_CAIRO], [1], [Define if Cairo is available])
  ], [have_cairo=no
      AS_IF([test "x$with_cairo" = xyes],
        [AC_MSG_ERROR([--with-cairo given but the cairo, cairo-xlib, freetype2 and fontconfig development files were not found])])])
])
have_pango=no
AS_IF([test "x$have_cairo" = xyes], [
  PKG_CHECK_MODULES([PANGO], [pangocairo], [
    have_pango=yes
    AC_DEFINE([HAVE_PANGO], [1], [Define if Pango is available])
  ], [have_pango=no
      AS_IF([test "x$with_cairo" = xyes],
        [AC_MSG_ERROR([--with-cairo given but the pangocairo development files were not found])])])
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
        [AS_IF([test "x$with_cairo" = xno],
          [AC_MSG_ERROR([--with-wayland=yes cannot be combined with --without-cairo: the Wayland backend needs Cairo and Pango])],
          [AC_MSG_ERROR([the Wayland backend requires Cairo+Pango, which were not found])])])])
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

# --- Required: pthread_create(), pthread_kill() and pthread_sigmask() ---
# The SIGCHLD handler hands a signal that a library's thread took (Pango's
# font loader) to the editor's own thread, and the child-safety test starts a
# thread of its own.  The library that holds them differs: glibc 2.34+ and
# macOS have all three in libc, older glibc and NetBSD put them in
# libpthread, and FreeBSD's libc carries only stubs of pthread_kill and
# pthread_sigmask while pthread_create lives in libthr (-lpthread).  So the
# search asks for the call a program cannot link without, pthread_create,
# and then requires the other two to resolve with what it found.  A search
# on pthread_kill alone came back "none required" on FreeBSD and left
# PTHREAD_LIBS empty.  Routed through PTHREAD_LIBS like UTIL_LIBS.
ace_save_LIBS="$LIBS"
AC_SEARCH_LIBS([pthread_create], [pthread],
  [AS_IF([test "x$ac_cv_search_pthread_create" = "xnone required"],
     [PTHREAD_LIBS=""], [PTHREAD_LIBS="$ac_cv_search_pthread_create"])],
  [AC_MSG_ERROR([pthread_create() is required (libc, or libpthread/libthr).])])
LIBS="$PTHREAD_LIBS $LIBS"
AC_MSG_CHECKING([for pthread_kill and pthread_sigmask])
AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <pthread.h>
#include <signal.h>]],
    [[sigset_t s; sigemptyset(&s);
      return pthread_sigmask(SIG_BLOCK, &s, 0) + pthread_kill(pthread_self(), 0);]])],
  [AC_MSG_RESULT([yes])],
  [AC_MSG_RESULT([no])
   AC_MSG_ERROR([pthread_kill() and pthread_sigmask() are required (searched with ${PTHREAD_LIBS:-libc}).])])
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
# NetBSD has no uselocale(3) at all, so HAVE_USELOCALE stays undefined there
# and we_rowfind.h exports "no UTF-8 ctype of its own" to the owner and its
# test.
AC_CHECK_FUNCS([newlocale uselocale])

# Whether the engine, started inside a row with REG_STARTEND, still sees the
# text before the start.  glibc does (a \b at the start is judged by the
# character before it); macOS's regexec takes the start for the beginning of
# the string and answers as if no letter were before it.  The search owner
# reads WPE_HAS_REGEX_START_CONTEXT (through the decision in we_rowfind.h) and,
# where it is 0, hands the engine the character before every start itself.
# The probe runs under the owner's UTF-8 ctype and says yes only when all
# hold, each with REG_STARTEND and REG_NOTBOL from offset 1: \ba on "xab" has
# no match, \Ba on "xab" is 1..2, and \ba on " ab" is 1..2 (so an engine that
# never matches does not pass).  Where \b does not compile the same three run
# with [[:<:]]a and, for the second, [[:>:]] and a space on "a b" (the
# bracket forms have no \B).  A cross build, a C library with no REG_STARTEND,
# no UTF-8 locale or an engine with neither spelling says 0, which is the safe
# value: the context path is right on every engine.  This is not the cross
# default of the word-escapes probe above, which says yes when it knows a flag.
AC_CACHE_CHECK([whether regexec started inside a row sees the text before it],
  [xwpe_cv_regex_start_context],
  [AS_IF([test "x$cross_compiling" = xyes],
     [xwpe_cv_regex_start_context="no (cross build)"],
     [test "x$ac_cv_have_decl_REG_STARTEND" != xyes],
     [xwpe_cv_regex_start_context="no (no REG_STARTEND)"],
     [AC_RUN_IFELSE([AC_LANG_PROGRAM([[#include <locale.h>
#include <string.h>
#include <sys/types.h>
#include <regex.h>

/* One call as the editor makes it at a start inside a row.  Return: regexec's
   code, or -1 when the pattern does not compile. */
static int call_from(const char *pat, const char *row, int so, regmatch_t *m)
{
 regex_t re;
 int rc;
 if (regcomp(&re, pat, REG_EXTENDED | $xwpe_regex_flag)) return -1;
 m->rm_so = so;
 m->rm_eo = (regoff_t) strlen(row);
 rc = regexec(&re, row, 1, m, REG_STARTEND | REG_NOTBOL);
 regfree(&re);
 return rc;
}

static int hits(int rc, const regmatch_t *m, int so, int eo)
{
 return rc == 0 && m->rm_so == so && m->rm_eo == eo;
}]], [[regmatch_t m;
 const char *start = "\\\\ba", *edge = "\\\\Ba", *edge_row = "xab";
 if (!setlocale(LC_CTYPE, "C.UTF-8") && !setlocale(LC_CTYPE, "en_US.UTF-8")) return 2;
 if (call_from(start, "xab", 1, &m) == -1)
 {
  start = "[[:<:]]a";
  edge = "[[:>:]] ";
  edge_row = "a b";
  if (call_from(start, "xab", 1, &m) == -1) return 3;
 }
 if (call_from(start, "xab", 1, &m) != REG_NOMATCH) return 1;
 if (!hits(call_from(edge, edge_row, 1, &m), &m, 1, 2)) return 1;
 if (!hits(call_from(start, " ab", 1, &m), &m, 1, 2)) return 1;
 return 0;]])],
       [xwpe_cv_regex_start_context=yes],
       [xwpe_cv_regex_start_context=no],
       [xwpe_cv_regex_start_context="no (cross build)"])])])
WPE_FACT([REGEX_START_CONTEXT], [test "x$xwpe_cv_regex_start_context" = xyes],
  [the regular expression engine, started inside a row, still sees the text before the start])
AS_IF([test "x$xwpe_cv_regex_start_context" = xyes],
  [xwpe_say_regex_start_context="yes (a \\b at a start is judged by the character before it)"],
  [xwpe_say_regex_start_context="no (Find reads one character before each start)"])

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

# --- An assistant child that can never gain privileges ---
# Linux has prctl(PR_SET_NO_NEW_PRIVS) and FreeBSD
# procctl(PROC_NO_NEW_PRIVS_CTL): after it, an exec of a set-user-ID program
# does not raise the child's rights.  The other systems have no such call,
# and the containment report then says privs=none.
AC_CHECK_DECLS([PR_SET_NO_NEW_PRIVS], [], [],
  [[#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif]])
AC_CHECK_DECLS([PROC_NO_NEW_PRIVS_CTL], [], [],
  [[#include <sys/types.h>
#ifdef HAVE_SYS_PROCCTL_H
#include <sys/procctl.h>
#endif]])
AS_IF([test "x$ac_cv_have_decl_PR_SET_NO_NEW_PRIVS" = xyes],
  [xwpe_say_no_new_privs=prctl],
  [test "x$ac_cv_have_decl_PROC_NO_NEW_PRIVS_CTL" = xyes],
  [xwpe_say_no_new_privs=procctl],
  [xwpe_say_no_new_privs="none (an assistant command can gain privileges by running a set-user-ID program)"])
WPE_FACT([NO_NEW_PRIVS],
  [test "x$ac_cv_have_decl_PR_SET_NO_NEW_PRIVS" = xyes || test "x$ac_cv_have_decl_PROC_NO_NEW_PRIVS_CTL" = xyes],
  [the system lets a child give up the right to gain privileges])

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
      no new privileges: $xwpe_say_no_new_privs
      memfd_create:      $xwpe_say_memfd
      close_range:       $xwpe_say_close_range
      closefrom:         $xwpe_say_closefrom
      timerfd:           $xwpe_say_timerfd
      linux console:     $xwpe_say_console
      openpty header:    $xwpe_say_pty
      backtrace:         $xwpe_say_backtrace
      stat times:        $xwpe_say_stat
      birth time:        $xwpe_say_birth
      regex word escapes: $xwpe_say_regex
      regex GNU escapes: $xwpe_say_regex_gnu_escapes
      regex reads UTF-8: $xwpe_say_regex_multibyte
      regex boundary groups: $xwpe_say_regex_safe_boundary_group
      regex boundary escapes: $xwpe_say_regex_safe_boundary
      regex start context: $xwpe_say_regex_start_context
      linker --wrap:     $have_ld_wrap
])
