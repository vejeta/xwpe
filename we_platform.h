/* we_platform.h - what differs between the systems xwpe builds on.
 *
 * configure probes each system fact once, writes it into config.h as
 * WPE_HAS_<NAME> (always defined, to 0 or 1) and prints it in its summary
 * with what a "no" turns off.  This header turns those facts, and the few
 * names that move from one system to another (the header that declares
 * openpty, the time fields of struct stat), into one vocabulary.  No other
 * file names an operating system, includes a header that exists on only
 * some systems, or reads a system HAVE_*: it tests a fact with "#if
 * WPE_HAS_X" (never #ifdef) and says in the branch for a missing fact what
 * the user then loses.  A misspelt fact is then a -Wundef warning and
 * never a silent "no".
 *
 * A static inline header, so every program, including the unit tests that
 * link one source file alone, takes the rule without a library.
 */
#ifndef WE_PLATFORM_H
#define WE_PLATFORM_H

#ifndef WPE_HAS_PARENT_DEATH_SIGNAL
# error "we_platform.h reads the facts configure writes into config.h; the build includes config.h before every file"
#endif

#include <limits.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef HAVE_SYS_PRCTL_H
# include <sys/prctl.h>
#endif
#ifdef HAVE_SYS_PROCCTL_H
# include <sys/wait.h>      /* P_PID, the idtype procctl takes */
# include <sys/procctl.h>
#endif
#if WPE_HAS_TIMERFD
# include <sys/timerfd.h>
#endif
#if WPE_HAS_BACKTRACE
# include <execinfo.h>
#endif

/* The header that declares openpty differs: glibc <pty.h>, macOS, NetBSD and
 * OpenBSD <util.h>, FreeBSD <libutil.h>.  It also brings in names of its own
 * (<util.h> pulls <sys/param.h> and its MIN and MAX), so a file that calls
 * openpty asks for it by defining WPE_PLATFORM_WANTS_OPENPTY before it
 * includes this header. */
#ifdef WPE_PLATFORM_WANTS_OPENPTY
# if defined(HAVE_PTY_H)
#  include <pty.h>
# elif defined(HAVE_UTIL_H)
#  include <util.h>
# elif defined(HAVE_LIBUTIL_H)
#  include <libutil.h>
# endif
#endif

/* The Linux virtual console's ioctls (KDGETMODE, VT_GETSTATE, TIOCLINUX).
 * Their kernel headers define short names of their own, so a file that
 * uses them asks by defining WPE_PLATFORM_WANTS_CONSOLE_IOCTLS. */
#if WPE_HAS_LINUX_CONSOLE && defined(WPE_PLATFORM_WANTS_CONSOLE_IOCTLS)
# include <sys/ioctl.h>
# include <linux/kd.h>
# include <linux/vt.h>
#endif

/* The longest path the system takes is WPE_PLATFORM_PATH_MAX, in we_path.h:
   the path owner holds every number of a path's length. */

/**
 * WPE_STAT_MTIM, WPE_STAT_CTIM - A file's modification and status-change
 * time as a struct timespec.
 *
 * POSIX 2008 names the fields st_mtim and st_ctim, macOS st_mtimespec and
 * st_ctimespec.  Where neither exists the time is whole seconds: the built
 * outputs store then cannot tell a build from an edit made in the same
 * second.
 */
#if defined(HAVE_STRUCT_STAT_ST_MTIM) && defined(HAVE_STRUCT_STAT_ST_CTIM)
# define WPE_STAT_MTIM(st) ((st)->st_mtim)
# define WPE_STAT_CTIM(st) ((st)->st_ctim)
#elif defined(HAVE_STRUCT_STAT_ST_MTIMESPEC) && defined(HAVE_STRUCT_STAT_ST_CTIMESPEC)
# define WPE_STAT_MTIM(st) ((st)->st_mtimespec)
# define WPE_STAT_CTIM(st) ((st)->st_ctimespec)
#else
/** wpe_platform_whole_seconds - A timespec for a time known to the second. */
static inline struct timespec wpe_platform_whole_seconds(time_t seconds)
{
 struct timespec ts;

 ts.tv_sec = seconds;
 ts.tv_nsec = 0;
 return ts;
}
# define WPE_STAT_MTIM(st) wpe_platform_whole_seconds((st)->st_mtime)
# define WPE_STAT_CTIM(st) wpe_platform_whole_seconds((st)->st_ctime)
#endif

/**
 * WPE_PLATFORM_STATX_BTIME - Does the system have statx with a birth time?
 * WPE_PLATFORM_STAT_BIRTH - Which member of struct stat carries a birth time.
 *
 * Linux (glibc 2.28 and later) gives a file's birth time through statx, and
 * only the kernel sets it.  FreeBSD and NetBSD name it st_birthtim, macOS
 * st_birthtimespec, and there the file's owner can set it.  OpenBSD and old
 * Linux headers have neither: no birth time, and the owner of "is this the
 * object I saw" (we_seen.c) then answers "not proven" for a touched file.
 * Always defined, to 0 or 1; the member is one of the three
 * WPE_PLATFORM_STAT_BIRTH* values.
 */
#if defined(HAVE_STATX) && defined(HAVE_DECL_STATX_BTIME) && HAVE_DECL_STATX_BTIME
# define WPE_PLATFORM_STATX_BTIME 1
# include <fcntl.h>
# include <sys/sysmacros.h>   /* makedev, to name the device statx reports */
#else
# define WPE_PLATFORM_STATX_BTIME 0
#endif
/**
 * WPE_PLATFORM_STATFS_KIND - How a program reads the name of a file system.
 *
 * Linux gives a magic number in f_type of statfs (<sys/vfs.h>); FreeBSD,
 * NetBSD, OpenBSD and macOS give the name in f_fstypename (<sys/mount.h>).
 * Where neither is there, a test says it cannot tell.  Always defined.
 */
#define WPE_PLATFORM_STATFS_NONE        0
#define WPE_PLATFORM_STATFS_F_TYPE      1
#define WPE_PLATFORM_STATFS_FSTYPENAME  2
#if defined(HAVE_STRUCT_STATFS_F_FSTYPENAME)
# define WPE_PLATFORM_STATFS_KIND WPE_PLATFORM_STATFS_FSTYPENAME
#elif defined(HAVE_SYS_VFS_H)
# define WPE_PLATFORM_STATFS_KIND WPE_PLATFORM_STATFS_F_TYPE
#else
# define WPE_PLATFORM_STATFS_KIND WPE_PLATFORM_STATFS_NONE
#endif

#define WPE_PLATFORM_STAT_BIRTH_NONE         0
#define WPE_PLATFORM_STAT_BIRTHTIM           1
#define WPE_PLATFORM_STAT_BIRTHTIMESPEC      2
#if defined(HAVE_STRUCT_STAT_ST_BIRTHTIM)
# define WPE_PLATFORM_STAT_BIRTH WPE_PLATFORM_STAT_BIRTHTIM
#elif defined(HAVE_STRUCT_STAT_ST_BIRTHTIMESPEC)
# define WPE_PLATFORM_STAT_BIRTH WPE_PLATFORM_STAT_BIRTHTIMESPEC
#else
# define WPE_PLATFORM_STAT_BIRTH WPE_PLATFORM_STAT_BIRTH_NONE
#endif

/**
 * WPE_X11_DEFAULT_ALTMASK - The X modifier the editor takes as Alt when the
 * display's own modifier map says nothing.
 *
 * Linux and Tru64 servers put Alt on Mod1; macOS's X servers put it on Mod4.
 * The better answer is XGetModifierMapping at start, which is the X11
 * backend's to give; until then the system decides here.
 */
#if defined(__linux__) || defined(__osf__)
# define WPE_X11_DEFAULT_ALTMASK Mod1Mask
#else
# define WPE_X11_DEFAULT_ALTMASK Mod4Mask
#endif

/**
 * wpe_platform_die_with_parent - In a forked child: ask the system to send
 * this process @sig when its parent dies.
 * @sig: the signal to receive.
 *
 * When the editor is killed outright, a debugger or server that works for it
 * would otherwise keep running with nobody to stop it.  Linux (prctl) and
 * FreeBSD (procctl) have the call.  Where WPE_HAS_PARENT_DEATH_SIGNAL is 0
 * (macOS, NetBSD, OpenBSD) nothing is set, and such a child ends only when
 * it sees its input close.
 * Return: 1 when the system was asked, 0 when this system has no such call.
 */
static inline int wpe_platform_die_with_parent(int sig)
{
#if WPE_HAS_PARENT_DEATH_SIGNAL && HAVE_DECL_PR_SET_PDEATHSIG
 return prctl(PR_SET_PDEATHSIG, sig) == 0;
#elif WPE_HAS_PARENT_DEATH_SIGNAL && HAVE_DECL_PROC_PDEATHSIG_CTL
 int arg = sig;

 return procctl(P_PID, 0, PROC_PDEATHSIG_CTL, &arg) == 0;
#else
 (void) sig;
 return 0;
#endif
}

#endif /* WE_PLATFORM_H */
