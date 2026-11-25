/* interpose.h -- the one place a C test finds the C library's own function
 * behind a hook it defines.
 *
 * A test makes a failure on purpose by defining, in the test program, a
 * function with the name of a C library function (fstat, open, fsync ...).
 * The program's definition comes first in the dynamic linker's lookup, so
 * the product's calls reach the hook, and the hook passes on to the library's.
 *
 * Finding the library's function by the C spelling is wrong on a system that
 * binds the call to another name: NetBSD binds fstat to __fstat50, glibc with
 * large files to fstat64, with 64-bit time to __fstat64_time64, macOS on x86
 * to fstat$INODE64.  The library's symbol literally named "fstat" is then an
 * old-ABI wrapper that calls the renamed one, which is the hook: an endless
 * loop on NetBSD, a struct of the wrong layout elsewhere.
 *
 * WPE_REAL(hook) asks the binary under which name the hook was exported (the
 * compiler already wrote the bound name there) and looks up that exact name
 * in the objects after the program.  No list of names is kept.  It needs the
 * program linked with the export-dynamic flag configure measured
 * (TEST_EXPORT_DYNAMIC): without it dladdr cannot see the hook.
 *
 * A hook that cannot be resolved ends the program with a message that names
 * the hook and the reason.  It never falls back to the C spelling.
 *
 * WPE_HOOK_GUARD(hook) goes first in the hook.  It counts the entries and the
 * depth; a hook entered over WPE_INTERPOSE_DEPTH_LIMIT deep means "the real"
 * function leads back to the hook, and the program ends saying so, in place
 * of 20000 frames and a stack overflow.
 *
 * This header is the only file of tests/ that calls dlsym or names RTLD_NEXT
 * (tests/check-interpose.sh).
 */
#ifndef WPE_TEST_INTERPOSE_H
#define WPE_TEST_INTERPOSE_H

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "linebuf.h"

/** The lookup scope for "the function after the program's own": the objects
    that come after the executable. */
#define WPE_INTERPOSE_NEXT RTLD_NEXT

/** How many entries of one hook may be open at once before the real function
    is taken to lead back into the hook.  A hook may call itself on purpose a
    few times (a row that plants a socket opens a file under the open hook);
    nothing needs this many. */
#define WPE_INTERPOSE_DEPTH_LIMIT 64

/** How many different hooks one program may guard. */
#define WPE_INTERPOSE_SLOTS 32

typedef struct
{
 const char *name;
 int depth;
 long entries;
} WpeHookSlot;

typedef struct
{
 WpeHookSlot *slot;
} WpeHookGuard;

static WpeHookSlot wpe_hook_slots[WPE_INTERPOSE_SLOTS];
static int wpe_interpose_dying;   /* the message of the end is being written */

/**
 * wpe_interpose_die - End the program because a hook cannot be trusted.
 * @hook: the C name of the hook.
 * @why: the reason, a sentence.
 *
 * A test that went on with a wrong "real" function would report a row of the
 * product as failed.  This names the hook instead.
 */
static inline void wpe_interpose_die(const char *hook, const char *why)
{
 /* The message itself may go through a hooked write: it must not start
    this again (wpe_interpose_enter lets an entry pass while dying). */
 if (!wpe_interpose_dying)
 {
  wpe_interpose_dying = 1;
  setvbuf(stderr, NULL, _IONBF, 0);
  fprintf(stderr, "interpose: %s: %s\n", hook, why);
 }
 abort();
}

/**
 * wpe_interpose_slot - The counters of one hook, made on first use.
 * @hook: the C name of the hook.
 */
static inline WpeHookSlot *wpe_interpose_slot(const char *hook)
{
 int i;

 for (i = 0; i < WPE_INTERPOSE_SLOTS; i++)
 {
  if (wpe_hook_slots[i].name == NULL)
  {
   wpe_hook_slots[i].name = hook;
   return(&wpe_hook_slots[i]);
  }
  if (strcmp(wpe_hook_slots[i].name, hook) == 0)
   return(&wpe_hook_slots[i]);
 }
 wpe_interpose_die(hook, "too many hooks in one program (raise WPE_INTERPOSE_SLOTS)");
 return(NULL);
}

/**
 * wpe_interpose_enter - Count an entry into a hook; stop a loop back into it.
 * @hook: the C name of the hook.
 *
 * Return: the guard, left again by wpe_interpose_leave when the hook returns.
 */
static inline WpeHookGuard wpe_interpose_enter(const char *hook)
{
 WpeHookGuard g;

 g.slot = wpe_interpose_slot(hook);
 g.slot->entries++;
 if (++g.slot->depth > WPE_INTERPOSE_DEPTH_LIMIT && !wpe_interpose_dying)
  wpe_interpose_die(hook, "the real function leads back into its hook");
 return(g);
}

/**
 * wpe_interpose_leave - Leave a hook.  Called by the cleanup of the guard.
 * @g: the guard of the hook that returns.
 */
static inline void wpe_interpose_leave(WpeHookGuard *g)
{
 g->slot->depth--;
}

/** The first line of a hook: guards it for the rest of the function. */
#define WPE_HOOK_GUARD(hook) \
 WpeHookGuard wpe_guard_##hook __attribute__((cleanup(wpe_interpose_leave), unused)) = wpe_interpose_enter(#hook)

/**
 * wpe_interpose_entries - How many times a hook has been entered.
 * @hook: the C name of the hook.
 *
 * A row reads it to prove that a call really went through the hook.
 */
static inline long wpe_interpose_entries(const char *hook)
{
 return(wpe_interpose_slot(hook)->entries);
}

/**
 * wpe_interpose_real - Find the library's function behind a hook.
 * @cname: the C name of the hook, for the messages.
 * @hook: the address of the hook.
 *
 * Return: the function the exported name of the hook has after the program.
 * It ends the program with a message when the answer cannot be trusted.
 */
static inline void *wpe_interpose_real(const char *cname, void *hook)
{
 Dl_info info;
 char why[256];
 void *real;

 if (dladdr(hook, &info) == 0)
  wpe_interpose_die(cname, "dladdr does not know the hook (link with TEST_EXPORT_DYNAMIC)");
 if (info.dli_sname == NULL)
  wpe_interpose_die(cname, "the hook is not exported (link with TEST_EXPORT_DYNAMIC)");
 if (info.dli_saddr != hook)
 {
  snprintf(why, sizeof why, "dladdr named another symbol (%s) for the hook", info.dli_sname);
  wpe_interpose_die(cname, why);
 }
 real = dlsym(WPE_INTERPOSE_NEXT, info.dli_sname);
 if (real == NULL)
 {
  snprintf(why, sizeof why, "the C library has no %s to pass on to", info.dli_sname);
  wpe_interpose_die(cname, why);
 }
 if (real == hook)
 {
  snprintf(why, sizeof why, "the next %s is the hook itself", info.dli_sname);
  wpe_interpose_die(cname, why);
 }
 return(real);
}

/** The function behind the hook of this name, typed as the hook is. */
#define WPE_REAL(hook) ((__typeof__(&(hook)))wpe_interpose_real(#hook, (void *)(hook)))

/*
 * The fortified twins.  Built with _FORTIFY_SOURCE, as the CI's compiler and
 * a distribution's package build do, glibc turns a call whose buffer size it
 * knows into a checked twin (read into __read_chk, readlink into
 * __readlink_chk, ttyname_r into __ttyname_r_chk, open and openat without a
 * mode into __open_2 and __openat_2).  The product then never reaches a hook
 * of the plain name, and a row that depends on the hook passes or fails for
 * the wrong reason ("a grown file" read whole: CI run 45582068519).
 *
 * A test that hooks one of these names places the matching WPE_HOOK_TWIN_*
 * after its hook: the twin checks nothing more and passes to the hook, so the
 * product reaches the hook whichever spelling it was compiled to.  Where the
 * library has no such twin the macro is empty.  tests/check-interpose.sh
 * refuses a hook of these names without its twin.
 */
#if defined(__GLIBC__)
# define WPE_HOOK_TWIN_READ \
 ssize_t __read_chk(int fd, void *buf, size_t n, size_t buflen); \
 ssize_t __read_chk(int fd, void *buf, size_t n, size_t buflen) \
 { (void)buflen; return(read(fd, buf, n)); }
# define WPE_HOOK_TWIN_READLINK \
 ssize_t __readlink_chk(const char *path, char *buf, size_t n, size_t buflen); \
 ssize_t __readlink_chk(const char *path, char *buf, size_t n, size_t buflen) \
 { (void)buflen; return(readlink(path, buf, n)); }
# define WPE_HOOK_TWIN_TTYNAME_R \
 int __ttyname_r_chk(int fd, char *buf, size_t n, size_t buflen); \
 int __ttyname_r_chk(int fd, char *buf, size_t n, size_t buflen) \
 { (void)buflen; return(ttyname_r(fd, buf, n)); }
# define WPE_HOOK_TWIN_OPEN \
 int __open_2(const char *path, int flags); \
 int __open_2(const char *path, int flags) \
 { return(open(path, flags)); }
# define WPE_HOOK_TWIN_OPENAT \
 int __openat_2(int dirfd, const char *path, int flags); \
 int __openat_2(int dirfd, const char *path, int flags) \
 { return(openat(dirfd, path, flags)); }
#else
# define WPE_HOOK_TWIN_READ
# define WPE_HOOK_TWIN_READLINK
# define WPE_HOOK_TWIN_TTYNAME_R
# define WPE_HOOK_TWIN_OPEN
# define WPE_HOOK_TWIN_OPENAT
#endif

#endif /* WPE_TEST_INTERPOSE_H */
