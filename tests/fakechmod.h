/* fakechmod.h -- a test's way to make this user unable to keep a setgid bit.
 *
 * The BSDs refuse a chmod that sets S_ISGID for a user who is not in the
 * file's group (EPERM, the whole call); Linux accepts the call and clears the
 * bit.  A Linux run cannot reach either case without a group the user is not
 * in, so this header defines fchmod and chmod in the test program, where they
 * take the place of the C library's for every caller linked into it (the
 * editor's files compiled into the test included), and reach the library's
 * through the one resolver of tests/interpose.h.  Both faces are the system's own:
 *
 *	refuse-sgid	a call that asks for S_ISGID fails with EPERM, nothing changes
 *	drop-sgid	the call is made without S_ISGID and returns 0
 *	FAKECHMOD_LCHMOD	a fchmodat that does not follow links, on a name
 *			that is a link, returns 0, as the BSDs and macOS
 *			do (lchmod's meaning), and skips the change.  Those
 *			systems do change the link's own bits; Linux refuses
 *			the call.  Any other call is transparent.  chmod is
 *			never faked this way: it follows links on every
 *			system.
 *
 * XWPE_TEST_FAKE_FCHMOD names the face for a whole run; a test that needs a
 * face for one row sets it with fakechmod_set and puts it back to
 * FAKECHMOD_OFF.  Off, both functions are transparent.
 *
 * fchmod, chmod and fchmodat are all faked, so a change of mode reaches the
 * face whether the editor makes it through a descriptor (fchmod) or through
 * a name (fchmodat, the owner's face for a file it cannot open, and chmod).
 * The calls are counted by kind (fakechmod_by_fd, fakechmod_by_name) so a row
 * can say which way a change went.
 *
 * It defines functions with external linkage: include it from exactly one
 * file of a program.  An unknown face, or a C library that has no fchmod to
 * pass on to, ends the program with a message: a run that silently did
 * nothing would show a green row that tested nothing.
 */
#ifndef TEST_FAKECHMOD_H
#define TEST_FAKECHMOD_H

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../we_platform.h"
#include "interpose.h"

#define FAKECHMOD_ENV "XWPE_TEST_FAKE_FCHMOD"

/* FAKECHMOD_VANISH: a chmod by name succeeds, and the name is gone right
   after, so the read-back of the mode that follows fails; a chmod by
   descriptor succeeds and the descriptor is closed right after, so the
   read-back through it fails the same way. */
/* FAKECHMOD_REFUSE_FILES: a change that asks for the given bits on a regular
   file fails with EFTYPE (EPERM where the system has no EFTYPE), and the same
   change on a folder goes through: what FreeBSD, NetBSD and OpenBSD say to a
   user who asks for the sticky bit on a file. */
typedef enum { FAKECHMOD_OFF, FAKECHMOD_REFUSE, FAKECHMOD_DROP, FAKECHMOD_VANISH,
               FAKECHMOD_LCHMOD, FAKECHMOD_REFUSE_FILES } FakeChmodFace;

typedef int (*fakechmod_fchmod_fn)(int, mode_t);
typedef int (*fakechmod_chmod_fn)(const char *, mode_t);
typedef int (*fakechmod_fchmodat_fn)(int, const char *, mode_t, int);

static int fakechmod_by_fd;             /* changes of mode through a descriptor since fakechmod_count_reset */
static int fakechmod_by_name;           /* changes of mode through a name since fakechmod_count_reset */
static char fakechmod_swap_name[512];   /* the name a link takes just before the next change through it */
static char fakechmod_swap_target[512]; /* ... a link to this */
static int fakechmod_swap_hard;         /* the swap is a hard link, not a symbolic one */
static int fakechmod_last_rc;           /* what the last fchmodat returned: the real call's value,
                                           or 0 where a face made the call succeed */

/** fakechmod_count_reset - Start counting the changes of mode again. */
static void fakechmod_count_reset(void)
{
 fakechmod_by_fd = 0;
 fakechmod_by_name = 0;
 fakechmod_last_rc = -1;
}

/**
 * fakechmod_swap_at - Put a link under a name just before the next change of
 * mode through that name.
 * @name: the name the editor will change; @target: where the link leads.
 *
 * The moment between the editor looking at a file and changing it, which
 * another user of the folder can use.  Armed once; it fires for the next
 * fchmodat or chmod of exactly that name.
 */
static void fakechmod_swap_at(const char *name, const char *target)
{
 snprintf(fakechmod_swap_name, sizeof fakechmod_swap_name, "%s", name);
 snprintf(fakechmod_swap_target, sizeof fakechmod_swap_target, "%s", target);
 fakechmod_swap_hard = 0;
}

/**
 * fakechmod_swap_hard_at - Put a hard link under a name just before the next
 * change of mode through that name.
 * @name: the name the editor will change; @target: the file it is linked to.
 *
 * The same moment as fakechmod_swap_at, with a file that is another name of
 * an existing file: a change of mode through the name reaches that file.
 */
static void fakechmod_swap_hard_at(const char *name, const char *target)
{
 fakechmod_swap_at(name, target);
 fakechmod_swap_hard = 1;
}

/* The swap of a FIFO for another FIFO that holds the old one's inode number:
   the name another user of the folder can use, on a file system that gives
   a number back.  A FIFO is changed by name, never opened. */
#define FAKECHMOD_FIFO_TRIES 16
/* The wanted number of a swap that takes any newcomer: no inode is 0, so the
   first FIFO made stays under the name. */
#define FAKECHMOD_ANY_NUMBER ((ino_t)0)
static char fakechmod_fifo_name[512];   /* the name whose FIFO is swapped just before the next change through it */
static ino_t fakechmod_fifo_want;       /* the inode number the new FIFO is to hold */
static int fakechmod_fifo_fired;        /* the swap was made */
static int fakechmod_fifo_reused;       /* the new FIFO holds the wanted number */
static int fakechmod_fifo_twin_mtime_was_equal; /* the new FIFO's modification time equalled the old one's before it was set apart */
/** Seconds between the old FIFO's modification time and the twin's. */
#define FAKECHMOD_TWIN_APART_S 1

/**
 * fakechmod_swap_fifo_reusing_at - Put a new FIFO with a given inode number
 * under a name just before the next change of mode through that name.
 * @name: the name the editor will change; @want: the inode number of the
 * FIFO that is there now (the new one is made to hold it again).
 *
 * The moment between the editor's last look and its change.  After it
 * fakechmod_fifo_fired says the swap happened and fakechmod_fifo_reused says
 * the file system gave the number back (it never does on tmpfs).
 */
static void fakechmod_swap_fifo_reusing_at(const char *name, ino_t want)
{
 snprintf(fakechmod_fifo_name, sizeof fakechmod_fifo_name, "%s", name);
 fakechmod_fifo_want = want;
 fakechmod_fifo_fired = 0;
 fakechmod_fifo_reused = 0;
}

/**
 * fakechmod_fifo_make_reusing - Make FIFOs at a name until one holds the wanted number.
 *
 * A FIFO that lands on a lower number is kept under a filler name, which makes
 * the next one higher (the way the reuse rows of seen_rows.h do it).
 */
static void fakechmod_fifo_make_reusing(const char *path)
{
 char filler[640];
 struct stat st;
 int i;

 for (i = 0; i < FAKECHMOD_FIFO_TRIES; i++)
 {
  if (mkfifo(path, 0644) != 0 || lstat(path, &st) != 0)
   return;
  if (st.st_ino == fakechmod_fifo_want)
  {
   fakechmod_fifo_reused = 1;
   return;
  }
  if (st.st_ino > fakechmod_fifo_want)
   return;
  snprintf(filler, sizeof filler, "%s.filler%d", path, i);
  if (rename(path, filler) != 0)
   return;
 }
}

/**
 * fakechmod_fifo_set_twin_apart - Set the new FIFO's modification time one second away from the old one's.
 * @path: the name the new FIFO holds.
 * @old: what the old FIFO's stat said before it was removed.
 *
 * A twin made in the same tick of the file system's clock carries the same
 * modification time, size and owner as the FIFO it replaces, and a system with
 * no birth time then cannot tell them apart: that is the documented limit of
 * such a system, not what the row is about.  The row is about "another FIFO
 * with the old number took the name", so the twin differs in a member on every
 * system.  fakechmod_fifo_twin_mtime_was_equal records whether it already
 * differed, for the log.
 */
static void fakechmod_fifo_set_twin_apart(const char *path, const struct stat *old)
{
 struct stat twin;
 struct timespec twin_mtim, old_mtim, apart[2];

 if (lstat(path, &twin) != 0)
  return;
 twin_mtim = WPE_STAT_MTIM(&twin);
 old_mtim = WPE_STAT_MTIM(old);
 fakechmod_fifo_twin_mtime_was_equal = (twin_mtim.tv_sec == old_mtim.tv_sec && twin_mtim.tv_nsec == old_mtim.tv_nsec);
 apart[0] = old_mtim;
 apart[0].tv_sec += FAKECHMOD_TWIN_APART_S;
 apart[1] = apart[0];
 if (utimensat(AT_FDCWD, path, apart, AT_SYMLINK_NOFOLLOW) != 0)
  perror("fakechmod twin mtime");
}

/** fakechmod_fifo_fire - Make the armed FIFO swap, when this is its name. */
static void fakechmod_fifo_fire(const char *path)
{
 struct stat old;

 if (fakechmod_fifo_name[0] == '\0' || strcmp(path, fakechmod_fifo_name) != 0)
  return;
 fakechmod_fifo_name[0] = '\0';
 fakechmod_fifo_fired = 1;
 if (lstat(path, &old) != 0 || unlink(path) != 0)
  return;
 fakechmod_fifo_make_reusing(path);
 fakechmod_fifo_set_twin_apart(path, &old);
}

/** fakechmod_swap_fire - Make the armed swap, when this is its name. */
static void fakechmod_swap_fire(const char *path)
{
 int rc;

 fakechmod_fifo_fire(path);
 if (fakechmod_swap_name[0] == '\0' || strcmp(path, fakechmod_swap_name) != 0)
  return;
 fakechmod_swap_name[0] = '\0';
 if (unlink(path) != 0)
  rc = -1;
 else if (fakechmod_swap_hard)
  rc = link(fakechmod_swap_target, path);
 else
  rc = symlink(fakechmod_swap_target, path);
 if (rc != 0)
  perror("fakechmod swap");
}

/* The swap that comes after the look that proved the file: armed for a name,
   it waits for the next change of mode through the name, then for the first
   look at the name (statx, the one call every look of we_seen.c makes) after
   that change, lets that look answer, and only then replaces the file by a
   FIFO holding the old number.  A look made after it, by name, sees the
   newcomer. */
typedef enum { FAKECHMOD_AFTER_OFF, FAKECHMOD_AFTER_WAIT_CHMOD, FAKECHMOD_AFTER_WAIT_LOOK } FakeChmodAfter;
static char fakechmod_after_name[512];
static FakeChmodAfter fakechmod_after = FAKECHMOD_AFTER_OFF;

/**
 * fakechmod_swap_after_look_at - Replace the file just after the look that follows the next change of mode.
 * @name: the name the editor will change; @want: the inode number of the file there now.
 *
 * The moment between the editor's judgement that the name still leads to its
 * file and any later use of the name.  fakechmod_fifo_fired says it happened.
 */
static void fakechmod_swap_after_look_at(const char *name, ino_t want)
{
 snprintf(fakechmod_after_name, sizeof fakechmod_after_name, "%s", name);
 fakechmod_fifo_want = want;
 fakechmod_fifo_fired = 0;
 fakechmod_fifo_reused = 0;
 fakechmod_after = FAKECHMOD_AFTER_WAIT_CHMOD;
}

/** fakechmod_after_saw_change - A change of mode through this name was made: the next look is the one. */
static void fakechmod_after_saw_change(const char *path)
{
 if (fakechmod_after == FAKECHMOD_AFTER_WAIT_CHMOD && strcmp(path, fakechmod_after_name) == 0)
  fakechmod_after = FAKECHMOD_AFTER_WAIT_LOOK;
}

/** fakechmod_after_saw_look - A look at this name was answered: make the swap, once. */
static void fakechmod_after_saw_look(const char *path)
{
 if (fakechmod_after != FAKECHMOD_AFTER_WAIT_LOOK || path == NULL || strcmp(path, fakechmod_after_name) != 0)
  return;
 fakechmod_after = FAKECHMOD_AFTER_OFF;
 fakechmod_fifo_fired = 1;
 if (unlink(path) == 0)
  fakechmod_fifo_make_reusing(path);
}

/**
 * fakechmod_name_is_link - Does lstat say this name is a link?
 * @path: the name.  Return: 1 for a link, 0 for anything else.
 */
static int fakechmod_name_is_link(const char *path)
{
 struct stat st;

 return(lstat(path, &st) == 0 && S_ISLNK(st.st_mode));
}

static FakeChmodFace fakechmod_face = FAKECHMOD_OFF;
static mode_t fakechmod_bits = 0;
static int fakechmod_ready;
static int fakechmod_errno = EPERM;     /* what the first refused call fails with */
static int fakechmod_errno_after;       /* what every later refused call fails with, 0: the same */
static int fakechmod_refusals;          /* refused calls since fakechmod_set */

/**
 * fakechmod_first_errno - The errno a face's first refused call fails with.
 * @face: the face being set.
 *
 * EPERM for the faces that refuse a bit to a user outside the file's group;
 * EFTYPE, where the system has it, for the face that refuses a bit to every
 * regular file.
 */
static int fakechmod_first_errno(FakeChmodFace face)
{
#ifdef EFTYPE
 if (face == FAKECHMOD_REFUSE_FILES)
  return(EFTYPE);
#endif
 (void)face;
 return(EPERM);
}

/**
 * fakechmod_refuse - Make the calling chmod or fchmod fail as a refused one.
 *
 * The first refusal is EPERM (the BSDs' answer for a setgid bit of a group the
 * user is not in); fakechmod_set_errors can give it another errno and the
 * refusals after it a different one, for a row that needs a real failure
 * after the nine bits were taken.
 */
static int fakechmod_refuse(void)
{
 int err = fakechmod_errno;

 if (fakechmod_refusals++ > 0 && fakechmod_errno_after != 0)
  err = fakechmod_errno_after;
 errno = err;
 return(-1);
}

/**
 * fakechmod_set_errors - Choose the errno of the refused calls.
 * @first: the first refused call's; @after: every later one's, or 0 for the
 * same.  Call it after fakechmod_set, which puts EPERM back.
 */
static void fakechmod_set_errors(int first, int after)
{
 fakechmod_errno = first;
 fakechmod_errno_after = after;
}

/**
 * fakechmod_set - Make this user unable to keep the given special bits.
 *
 * A row that proves what the editor does when the system refuses or drops a
 * bit calls this after its setup and puts FAKECHMOD_OFF back at its end.
 */
static void fakechmod_set(FakeChmodFace face, mode_t bits)
{
 fakechmod_face = face;
 fakechmod_bits = bits;
 fakechmod_ready = 1;
 fakechmod_errno = fakechmod_first_errno(face);
 fakechmod_errno_after = 0;
 fakechmod_refusals = 0;
}

/**
 * fakechmod_asks_bits_of_a_file - Is this change the one FAKECHMOD_REFUSE_FILES refuses?
 * @mode: the mode asked for.
 * @st: what the system says of the target, or NULL when it cannot be read.
 *
 * Return: 1 when the face is that one, @mode holds a bit it names, and the
 * target is a regular file; a folder, a link and an unreadable target are
 * let through.
 */
static int fakechmod_asks_bits_of_a_file(mode_t mode, const struct stat *st)
{
 if (fakechmod_face != FAKECHMOD_REFUSE_FILES || (mode & fakechmod_bits) == 0)
  return(0);
 return(st != NULL && S_ISREG(st->st_mode));
}

/** fakechmod_file_by_descriptor - The state of the file FD names, or NULL. */
static const struct stat *fakechmod_file_by_descriptor(int fd, struct stat *st)
{
 if (fstat(fd, st) != 0)
  return(NULL);
 return(st);
}

/** fakechmod_file_by_name - The state of the entry DIRFD and PATH name, links not followed when FLAGS say so, or NULL. */
static const struct stat *fakechmod_file_by_name(int dirfd, const char *path, int flags, struct stat *st)
{
 if (fstatat(dirfd, path, st, flags & AT_SYMLINK_NOFOLLOW) != 0)
  return(NULL);
 return(st);
}

/**
 * fakechmod_arm_from_env - Take the face a whole run asks for, once.
 *
 * Lets one command line run a test as the user of a BSD would meet it:
 * XWPE_TEST_FAKE_FCHMOD=refuse-sgid ./test_fileop.
 */
static void fakechmod_arm_from_env(void)
{
 const char *v;

 if (fakechmod_ready)
  return;
 fakechmod_ready = 1;
 v = getenv(FAKECHMOD_ENV);
 if (v == NULL || v[0] == '\0')
  return;
 if (strcmp(v, "refuse-sgid") == 0)
  fakechmod_set(FAKECHMOD_REFUSE, S_ISGID);
 else if (strcmp(v, "drop-sgid") == 0)
  fakechmod_set(FAKECHMOD_DROP, S_ISGID);
 else if (strcmp(v, "refuse-sticky-files") == 0)
  fakechmod_set(FAKECHMOD_REFUSE_FILES, S_ISVTX);
 else
 {
  fprintf(stderr, "%s=%s: not a face (refuse-sgid, drop-sgid or refuse-sticky-files)\n", FAKECHMOD_ENV, v);
  abort();
 }
}

int fchmod(int fd, mode_t mode)
{
 static fakechmod_fchmod_fn real;
 struct stat st;
 WPE_HOOK_GUARD(fchmod);

 fakechmod_arm_from_env();
 if (real == NULL)
  real = WPE_REAL(fchmod);
 fakechmod_by_fd++;
 if (fakechmod_face == FAKECHMOD_REFUSE && (mode & fakechmod_bits))
  return(fakechmod_refuse());
 if (fakechmod_asks_bits_of_a_file(mode, fakechmod_file_by_descriptor(fd, &st)))
  return(fakechmod_refuse());
 if (fakechmod_face == FAKECHMOD_DROP)
  mode &= ~fakechmod_bits;
 if (fakechmod_face == FAKECHMOD_VANISH)
 {
  int rc = real(fd, mode);

  if (rc == 0)
   (void)close(fd);
  return(rc);
 }
 return(real(fd, mode));
}

int chmod(const char *path, mode_t mode)
{
 static fakechmod_chmod_fn real;
 struct stat st;
 WPE_HOOK_GUARD(chmod);

 fakechmod_arm_from_env();
 if (real == NULL)
  real = WPE_REAL(chmod);
 fakechmod_by_name++;
 fakechmod_swap_fire(path);
 if (fakechmod_face == FAKECHMOD_REFUSE && (mode & fakechmod_bits))
  return(fakechmod_refuse());
 if (fakechmod_asks_bits_of_a_file(mode, fakechmod_file_by_name(AT_FDCWD, path, 0, &st)))
  return(fakechmod_refuse());
 if (fakechmod_face == FAKECHMOD_DROP)
  mode &= ~fakechmod_bits;
 if (fakechmod_face == FAKECHMOD_VANISH)
 {
  int rc = real(path, mode);

  if (rc == 0)
   (void)remove(path);
  return(rc);
 }
 return(real(path, mode));
}

int fchmodat(int dirfd, const char *path, mode_t mode, int flags)
{
 static fakechmod_fchmodat_fn real;
 struct stat st;
 WPE_HOOK_GUARD(fchmodat);

 fakechmod_arm_from_env();
 if (real == NULL)
  real = WPE_REAL(fchmodat);
 fakechmod_by_name++;
 fakechmod_last_rc = -1;
 fakechmod_swap_fire(path);
 if (fakechmod_face == FAKECHMOD_LCHMOD && (flags & AT_SYMLINK_NOFOLLOW) && fakechmod_name_is_link(path))
 {
  fakechmod_last_rc = 0;
  return(0);
 }
 if (fakechmod_face == FAKECHMOD_REFUSE && (mode & fakechmod_bits))
  return(fakechmod_last_rc = fakechmod_refuse());
 if (fakechmod_asks_bits_of_a_file(mode, fakechmod_file_by_name(dirfd, path, flags, &st)))
  return(fakechmod_last_rc = fakechmod_refuse());
 if (fakechmod_face == FAKECHMOD_DROP)
  mode &= ~fakechmod_bits;
 fakechmod_last_rc = real(dirfd, path, mode, flags);
 if (fakechmod_face == FAKECHMOD_VANISH && fakechmod_last_rc == 0)
  (void)remove(path);
 if (fakechmod_last_rc == 0)
  fakechmod_after_saw_change(path);
 return(fakechmod_last_rc);
}

/* The played inode number: the look of a name that does not follow a link is
   told a number of the row's choosing.  It lets a row reach the judge's
   "same number" case on every system, with no help from a file system that
   gives a freed number back (FreeBSD soft updates do not, for a link). */
static char fakechmod_ino_name[512];    /* the name whose own look is told the number; "" when none */
static ino_t fakechmod_ino_played;      /* the number told */
static int fakechmod_ino_hits;          /* looks that were told it since fakechmod_play_number_at */

/**
 * fakechmod_play_number_at - Tell every look of a name, link not followed, a given inode number.
 * @name: the name as the editor spells it (the whole path); "" to stop.
 * @ino: the number to tell.
 *
 * What a row uses to put a new link under the old link's number without the
 * file system giving it back.  The look still reports the real device, kind,
 * times and size; only the number is played.  fakechmod_ino_hits says how many
 * looks it changed, so a row proves the editor's look went through it.
 */
static void fakechmod_play_number_at(const char *name, ino_t ino)
{
 snprintf(fakechmod_ino_name, sizeof fakechmod_ino_name, "%s", name);
 fakechmod_ino_played = ino;
 fakechmod_ino_hits = 0;
}

/** fakechmod_plays_number - Is this look the one whose number is played (this name, link not followed)? */
static int fakechmod_plays_number(int dirfd, const char *path, int nofollow)
{
 if (fakechmod_ino_name[0] == '\0' || !nofollow || dirfd != AT_FDCWD || path == NULL)
  return(0);
 return(strcmp(path, fakechmod_ino_name) == 0);
}

/* Where the looks of we_seen.c are made with statx (Linux), statx carries the
   swap and the played number; elsewhere they are made with fstatat, which
   carries the played number, and the row that arms the swap does not run. */
#if WPE_PLATFORM_STATX_BTIME
typedef int (*fakechmod_statx_fn)(int, const char *, int, unsigned int, struct statx *);

/** statx: transparent, except that it makes the swap of fakechmod_swap_after_look_at and plays a number. */
int statx(int dirfd, const char *path, int flags, unsigned int mask, struct statx *buf)
{
 static fakechmod_statx_fn real;
 int rc;
 WPE_HOOK_GUARD(statx);

 if (real == NULL)
  real = WPE_REAL(statx);
 rc = real(dirfd, path, flags, mask, buf);
 if (rc == 0 && fakechmod_plays_number(dirfd, path, (flags & AT_SYMLINK_NOFOLLOW) != 0))
 {
  buf->stx_ino = (uint64_t)fakechmod_ino_played;
  fakechmod_ino_hits++;
 }
 if (fakechmod_after == FAKECHMOD_AFTER_WAIT_LOOK && dirfd == AT_FDCWD)
  fakechmod_after_saw_look(path);
 return(rc);
}
#else
typedef int (*fakechmod_fstatat_fn)(int, const char *, struct stat *, int);

/** fstatat: transparent, except that it plays a number (fakechmod_play_number_at). */
int fstatat(int dirfd, const char *path, struct stat *st, int flag)
{
 static fakechmod_fstatat_fn real;
 int rc;
 WPE_HOOK_GUARD(fstatat);

 if (real == NULL)
  real = WPE_REAL(fstatat);
 rc = real(dirfd, path, st, flag);
 if (rc == 0 && fakechmod_plays_number(dirfd, path, (flag & AT_SYMLINK_NOFOLLOW) != 0))
 {
  st->st_ino = fakechmod_ino_played;
  fakechmod_ino_hits++;
 }
 return(rc);
}
#endif

#endif
