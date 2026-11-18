/* we_seen.c -- the one owner of "is this the object I saw" (see we_seen.h). */
#include "we_seen.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "we_platform.h"

/* Whether this system has statx with a birth time, and which member of a
   struct stat carries one, are facts of we_platform.h. */
#define WPE_SEEN_USE_STATX WPE_PLATFORM_STATX_BTIME

/** The twelve bits of a mode: permissions, setuid, setgid and sticky. */
#define WPE_SEEN_MODE_BITS 07777

/* ---- recording a look ---------------------------------------------- */

/**
 * wpe_seen_forget - Make a record say "nothing was seen".
 *
 * A window that has read no file, or whose file is gone, holds a forgotten
 * record, and no later object is ever taken for it.
 */
void wpe_seen_forget(WpeSeen *s)
{
 memset(s, 0, sizeof *s);
}

/**
 * wpe_seen_known - Does this record describe an object?
 *
 * Lets a caller ask whether the window read a file at all, without reading
 * the record's members.
 */
int wpe_seen_known(const WpeSeen *s)
{
 return(s->known != 0);
}

/**
 * wpe_seen_kind - The kind of object a record describes (S_IFMT bits).
 *
 * Lets a caller tell "a link took the name" from "a file took the name"
 * for the sentence it shows, without reading the record's members.
 */
mode_t wpe_seen_kind(const WpeSeen *s)
{
 return(s->seen_kind);
}

/**
 * wpe_seen_into_stat - Give a record as the struct stat the mode owner still takes.
 * @s: the record; @st: receives the device, inode and kind of object.
 *
 * A bridge for Ctrl-F9 (wpe_write_add_perm), which hands the window's record
 * to the mode owner as the "shown" file.  It carries only the number, as
 * before.  The pass that moves the mode owner onto records deletes it.
 */
void wpe_seen_into_stat(const WpeSeen *s, struct stat *st)
{
 memset(st, 0, sizeof *st);
 st->st_dev = s->seen_dev;
 st->st_ino = s->seen_ino;
 st->st_mode = s->seen_kind;
}

/**
 * seen_test_drops_birth - Does the test hook ask every look to drop the birth?
 *
 * XWPE_TEST_FAIL=seen-no-birth lets a row on Linux play a system without a
 * birth time.  It is a test hook only: without births an answer moves from
 * SAME or OTHER to UNSURE, and a pair that differed only in its births
 * (UNSURE with them) is SAME without them.  Every look asks, so two records
 * never differ in birth because one route ignored it.
 */
static int seen_test_drops_birth(void)
{
 const char *hook = getenv(WPE_SEEN_TEST_FAIL_ENV);

 return(hook != NULL && strstr(hook, WPE_SEEN_TEST_FAIL_NO_BIRTH) != NULL);
}

/**
 * seen_fill_common - Copy the members every system reports from a stat.
 *
 * The birth time is left out: it is added by the look that has one.
 */
static void seen_fill_common(WpeSeen *s, const struct stat *st)
{
 wpe_seen_forget(s);
 s->known = 1;
 s->seen_kind = st->st_mode & S_IFMT;
 s->seen_dev = st->st_dev;
 s->seen_ino = st->st_ino;
 s->seen_uid = st->st_uid;
 s->seen_gid = st->st_gid;
 s->seen_mode = st->st_mode & WPE_SEEN_MODE_BITS;
 s->seen_size = st->st_size;
 s->seen_ctime = WPE_STAT_CTIM(st);
 s->seen_mtime = WPE_STAT_MTIM(st);
}

/**
 * seen_set_birth - Keep a birth time the system reported, if it is a real one.
 * @forgeable: 1 when a user can set that time on this system.
 *
 * A birth of zero seconds and zero nanoseconds, or of -1, counts as none:
 * some systems print that for a file system that has no birth time.
 */
static void seen_set_birth(WpeSeen *s, const struct timespec *birth, int forgeable)
{
 if (seen_test_drops_birth())
  return;
 if (birth->tv_sec == 0 && birth->tv_nsec == 0)
  return;
 if (birth->tv_sec == -1)
  return;
 s->seen_has_birth = 1;
 s->seen_birth_forgeable = forgeable;
 s->seen_birth = *birth;
}

/**
 * seen_take_stat_birth - Add the birth time a struct stat carries, if it has one.
 *
 * FreeBSD, NetBSD and macOS give it in the stat itself, and there its owner
 * can set it, so a differing birth is not proof (see wpe_seen_judge).  On
 * Linux the members do not exist: the birth comes from statx.
 */
static void seen_take_stat_birth(WpeSeen *s, const struct stat *st)
{
#if WPE_PLATFORM_STAT_BIRTH == WPE_PLATFORM_STAT_BIRTHTIM
 seen_set_birth(s, &st->st_birthtim, 1);
#elif WPE_PLATFORM_STAT_BIRTH == WPE_PLATFORM_STAT_BIRTHTIMESPEC
 seen_set_birth(s, &st->st_birthtimespec, 1);
#else
 (void)s;
 (void)st;
#endif
}

/**
 * seen_fill_stat - Record a stat the system gave, with its birth if the stat has one.
 */
static void seen_fill_stat(WpeSeen *s, const struct stat *st)
{
 seen_fill_common(s, st);
 seen_take_stat_birth(s, st);
}

/**
 * wpe_seen_from_stat - Record a stat a test already has.
 *
 * FOR TESTS ONLY.  The record has no birth time, so on Linux a touched file
 * falls to "unsure" by the judge's last step.  A caller that remembers an
 * object takes it with wpe_seen_look_fd, _name or _at, which ask for the
 * birth time.
 */
void wpe_seen_from_stat(WpeSeen *s, const struct stat *st)
{
 seen_fill_common(s, st);
}

#if WPE_SEEN_USE_STATX

/**
 * seen_fill_statx - Record what statx reported, with the birth if its mask says so.
 *
 * statx names a birth time only for the objects whose file system keeps one,
 * so the mask of each object decides and no list of file systems is needed.
 * On the local file systems (ext4, xfs, btrfs, f2fs, tmpfs) only the kernel
 * sets that time, so a differing birth is proof of another object.  On cifs
 * and FUSE a client or server can set it, which this does not tell apart.
 */
static void seen_fill_statx(WpeSeen *s, const struct statx *sx)
{
 struct timespec birth;

 wpe_seen_forget(s);
 s->known = 1;
 s->seen_kind = sx->stx_mode & S_IFMT;
 s->seen_dev = makedev(sx->stx_dev_major, sx->stx_dev_minor);
 s->seen_ino = (ino_t)sx->stx_ino;
 s->seen_uid = sx->stx_uid;
 s->seen_gid = sx->stx_gid;
 s->seen_mode = sx->stx_mode & WPE_SEEN_MODE_BITS;
 s->seen_size = (off_t)sx->stx_size;
 s->seen_ctime.tv_sec = (time_t)sx->stx_ctime.tv_sec;
 s->seen_ctime.tv_nsec = (long)sx->stx_ctime.tv_nsec;
 s->seen_mtime.tv_sec = (time_t)sx->stx_mtime.tv_sec;
 s->seen_mtime.tv_nsec = (long)sx->stx_mtime.tv_nsec;
 if ((sx->stx_mask & STATX_BTIME) == 0)
  return;
 birth.tv_sec = (time_t)sx->stx_btime.tv_sec;
 birth.tv_nsec = (long)sx->stx_btime.tv_nsec;
 seen_set_birth(s, &birth, 0);
}

/**
 * seen_statx_unavailable - Is this statx error about the call, not the object?
 *
 * ENOSYS is a kernel without statx, EPERM a container's seccomp filter that
 * refuses it.  Neither says anything about the file, so the look goes on with
 * fstat and has no birth time.  ENOENT, ENOTDIR, ELOOP, EACCES and EBADF are
 * the object's own and stand.
 */
static int seen_statx_unavailable(int err)
{
 return(err == ENOSYS || err == EPERM);
}

/**
 * seen_statx_at - Look at a directory entry with statx.
 * @flags: AT_SYMLINK_NOFOLLOW or AT_EMPTY_PATH, as the caller needs.
 *
 * Return: 0 with @s recorded, -1 with errno set, or 1 when statx is not
 * available here and the caller must use stat.
 */
static int seen_statx_at(int dirfd, const char *leaf, int flags, WpeSeen *s)
{
 struct statx sx;

 if (statx(dirfd, leaf, flags, STATX_BASIC_STATS | STATX_BTIME, &sx) == 0)
 {
  seen_fill_statx(s, &sx);
  return(0);
 }
 if (seen_statx_unavailable(errno))
  return(1);
 wpe_seen_forget(s);
 return(-1);
}

#endif /* WPE_SEEN_USE_STATX */

/**
 * seen_stat_fd - Look at a descriptor with fstat and record it.
 *
 * Return: 0, or -1 with errno set and @s forgotten.
 */
static int seen_stat_fd(int fd, WpeSeen *s)
{
 struct stat st;

 if (fstat(fd, &st) != 0)
 {
  wpe_seen_forget(s);
  return(-1);
 }
 seen_fill_stat(s, &st);
 return(0);
}

/**
 * seen_stat_at - Look at a directory entry with fstatat and record it.
 *
 * Return: 0, or -1 with errno set and @s forgotten.
 */
static int seen_stat_at(int dirfd, const char *leaf, int follow, WpeSeen *s)
{
 struct stat st;
 int flags = follow ? 0 : AT_SYMLINK_NOFOLLOW;

 if (fstatat(dirfd, leaf, &st, flags) != 0)
 {
  wpe_seen_forget(s);
  return(-1);
 }
 seen_fill_stat(s, &st);
 return(0);
}

/**
 * wpe_seen_look_fd - Record the object a descriptor holds.
 *
 * Used right after a file is opened or written, so the record is of the
 * object the editor itself holds and a later Save can tell it from a file
 * that took the name.  Return: 0, or -1 with errno set and @s forgotten.
 */
int wpe_seen_look_fd(int fd, WpeSeen *s)
{
#if WPE_SEEN_USE_STATX
 int rc = seen_statx_at(fd, "", AT_EMPTY_PATH, s);

 if (rc <= 0)
  return(rc);
#endif
 return(seen_stat_fd(fd, s));
}

/**
 * wpe_seen_look_at - Record a directory entry, relative to a folder held open.
 * @follow: 1 to look at what a link leads to, 0 at the name itself.
 *
 * The path-walk owner looks at a leaf through its parent folder with this, so
 * no component is looked up by name a second time.
 * Return: 0, or -1 with errno set and @s forgotten.
 */
int wpe_seen_look_at(int dirfd, const char *leaf, int follow, WpeSeen *s)
{
#if WPE_SEEN_USE_STATX
 int rc = seen_statx_at(dirfd, leaf, follow ? 0 : AT_SYMLINK_NOFOLLOW, s);

 if (rc <= 0)
  return(rc);
#endif
 return(seen_stat_at(dirfd, leaf, follow, s));
}

/**
 * wpe_seen_look_name - Record what a path names.
 * @follow: 1 to look at what a final link leads to, 0 at the name itself.
 *
 * Return: 0, or -1 with errno set and @s forgotten.
 */
int wpe_seen_look_name(const char *path, int follow, WpeSeen *s)
{
 return(wpe_seen_look_at(AT_FDCWD, path, follow, s));
}

/**
 * seen_read_link_text - Keep a link's whole target text in the record.
 *
 * The text counts only when it fitted and its length is the size the look
 * reported: a link swapped for another between the two calls leaves
 * has_text at 0 and the judge falls to the stamps.
 */
static void seen_read_link_text(const char *path, WpeSeenName *n)
{
 ssize_t got = readlink(path, n->text, sizeof n->text - 1);

 if (got < 0 || (size_t)got >= sizeof n->text - 1)
  return;
 if ((off_t)got != n->seen.seen_size)
  return;
 n->text[got] = '\0';
 n->len = (size_t)got;
 n->has_text = 1;
}

/**
 * wpe_seen_name_look - Record a name itself, with the text of a link.
 *
 * Used where the sentence must say "a link took the name" and the same link
 * made again must not look like another one.  A failed look leaves the
 * record unknown.
 */
void wpe_seen_name_look(const char *path, WpeSeenName *n)
{
 n->has_text = 0;
 n->len = 0;
 n->text[0] = '\0';
 if (wpe_seen_look_name(path, 0, &n->seen) != 0)
  return;
 if (n->seen.seen_kind == S_IFLNK)
  seen_read_link_text(path, n);
}

/* ---- the judge ------------------------------------------------------ */

/** Do two times fall on the same tick? */
static int seen_time_equal(const struct timespec *a, const struct timespec *b)
{
 return(a->tv_sec == b->tv_sec && a->tv_nsec == b->tv_nsec);
}

/** Do the records name the same device, inode and kind of object? */
static int seen_same_number(const WpeSeen *a, const WpeSeen *b)
{
 return(a->seen_dev == b->seen_dev && a->seen_ino == b->seen_ino && a->seen_kind == b->seen_kind);
}

/** Do both records have a birth time, and is it one? */
static int seen_births_comparable(const WpeSeen *a, const WpeSeen *b)
{
 return(a->seen_has_birth && b->seen_has_birth);
}

/** Do the owner, group, mode, size and modification time agree? */
static int seen_fields_agree(const WpeSeen *a, const WpeSeen *b)
{
 return(a->seen_uid == b->seen_uid && a->seen_gid == b->seen_gid &&
        a->seen_mode == b->seen_mode && a->seen_size == b->seen_size &&
        seen_time_equal(&a->seen_mtime, &b->seen_mtime));
}

/**
 * seen_judge_same_stamp - The answer when the status-change times are equal.
 *
 * Nothing touched the object, or something made another one inside the same
 * tick of the file system's clock.  A newcomer in that tick must also have
 * the same owner, group, mode, size and modification time, and only that
 * owner or root can make it: a file of another user fails on the owner.  When
 * both records have a birth time the births must agree too.
 */
static WpeSeenIs seen_judge_same_stamp(const WpeSeen *then, const WpeSeen *now)
{
 if (!seen_fields_agree(then, now))
  return(WPE_SEEN_UNSURE);
 if (seen_births_comparable(then, now) && !seen_time_equal(&then->seen_birth, &now->seen_birth))
  return(WPE_SEEN_UNSURE);
 return(WPE_SEEN_SAME);
}

/**
 * seen_judge_touched - The answer when the object was changed after the look.
 *
 * With a birth time on both sides the same birth and the same owner is the
 * same object touched since.  A different birth is another object where only
 * the kernel sets it (statx) and not proven where its owner can set it
 * (FreeBSD, NetBSD, macOS).  Without a usable birth nothing can tell a
 * touched file from a new one, which is UNSURE: the user gets a question,
 * never a silent write.
 */
static WpeSeenIs seen_judge_touched(const WpeSeen *then, const WpeSeen *now)
{
 if (!seen_births_comparable(then, now))
  return(WPE_SEEN_UNSURE);
 if (seen_time_equal(&then->seen_birth, &now->seen_birth))
 {
  if (then->seen_uid == now->seen_uid)
   return(WPE_SEEN_SAME);
  return(WPE_SEEN_UNSURE);
 }
 if (then->seen_birth_forgeable || now->seen_birth_forgeable)
  return(WPE_SEEN_UNSURE);
 return(WPE_SEEN_OTHER);
}

/**
 * wpe_seen_judge - Is the object now the one that was seen?
 * @then: what the editor remembered.  @now: a look made just now.
 *
 * Lets Save write into the file the window read, and ask the user instead
 * when another file, or one that cannot be told from it, took the name.  A
 * record that is not known on either side is UNSURE, never SAME.
 */
WpeSeenIs wpe_seen_judge(const WpeSeen *then, const WpeSeen *now)
{
 if (!then->known || !now->known)
  return(WPE_SEEN_UNSURE);
 if (!seen_same_number(then, now))
  return(WPE_SEEN_OTHER);
 if (seen_time_equal(&then->seen_ctime, &now->seen_ctime))
  return(seen_judge_same_stamp(then, now));
 return(seen_judge_touched(then, now));
}

/**
 * wpe_seen_name_judge - Is the name now the link or file that was seen there?
 *
 * ONLY WORDS A SENTENCE.  The same target text at the same name can lead
 * elsewhere after a folder above it was swapped, so this never allows an
 * action.  It tells "a link took the name" from "another file took it" in the
 * message: a link whose whole target text is the same is the same link, and
 * another text is another link.  Anything else goes to wpe_seen_judge.
 */
WpeSeenIs wpe_seen_name_judge(const WpeSeenName *then, const WpeSeenName *now)
{
 if (!then->seen.known || !now->seen.known)
  return(WPE_SEEN_UNSURE);
 if (!seen_same_number(&then->seen, &now->seen))
  return(WPE_SEEN_OTHER);
 if (then->has_text && now->has_text)
 {
  if (then->len == now->len && memcmp(then->text, now->text, then->len) == 0)
   return(WPE_SEEN_SAME);
  return(WPE_SEEN_OTHER);
 }
 return(wpe_seen_judge(&then->seen, &now->seen));
}

/* ---- two looks of one moment, or a held descriptor ------------------ */

/**
 * wpe_seen_same_now - Are two stats made now one object?
 *
 * For two looks made in one call, or a name against a descriptor still open:
 * the number cannot be given back in between, so device, inode and kind are
 * enough.  Not for a record kept from an earlier moment: that is
 * wpe_seen_judge.
 */
int wpe_seen_same_now(const struct stat *a, const struct stat *b)
{
 return(a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
        (a->st_mode & S_IFMT) == (b->st_mode & S_IFMT));
}

/**
 * wpe_seen_names_same_now - Do two paths lead to one object right now?
 *
 * Lets the File Manager refuse to copy a file onto itself, whatever the two
 * spellings of its name.  Links are followed.  A path that cannot be looked
 * at is not the same as anything.
 */
int wpe_seen_names_same_now(const char *a, const char *b)
{
 struct stat sa, sb;

 if (stat(a, &sa) != 0 || stat(b, &sb) != 0)
  return(0);
 return(wpe_seen_same_now(&sa, &sb));
}

/* ---- a folder held open ---------------------------------------------- */

/**
 * wpe_seen_hold_dir - Keep a folder open so no other folder can take its number.
 *
 * The assistant's checkpoint holds its workspace this way: the revert then
 * writes only into the folder the checkpoint saw, even if it was removed and
 * made again.  The descriptor is closed on exec, so a child never gets it.
 * It keeps a removable or network workspace busy until it is released.
 * Return: 0, or -1 with errno set and nothing held.
 */
int wpe_seen_hold_dir(const char *path, WpeSeenHeld *h)
{
 h->fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
 return(h->fd < 0 ? -1 : 0);
}

/**
 * wpe_seen_held_is - Is this path the folder that is held?
 *
 * Complete on every system: while the descriptor is open the number cannot
 * go to another folder.  A descriptor that no longer answers (a stale network
 * handle, a forced unmount) proves nothing, so the answer is 0.
 */
int wpe_seen_held_is(const WpeSeenHeld *h, const char *path)
{
 struct stat held, named;

 if (h->fd < 0)
  return(0);
 if (fstat(h->fd, &held) != 0 || stat(path, &named) != 0)
  return(0);
 return(wpe_seen_same_now(&held, &named));
}

/**
 * wpe_seen_release - Let go of a held folder.
 *
 * Safe on a hold that holds nothing.  Called when a checkpoint ends, so the
 * workspace can be unmounted again.
 */
void wpe_seen_release(WpeSeenHeld *h)
{
 if (h->fd >= 0)
  close(h->fd);
 h->fd = -1;
}
