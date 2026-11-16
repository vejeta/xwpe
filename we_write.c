/* we_write.c -- the one owner of "write the user's text, and what happens
   when the write fails".
   Copyright (C) 1993 Fred Kruse
   Copyright (C) 2026 Juan Manuel Mendez Rey
   This is free software; you can redistribute it and/or modify it under the
   terms of the GNU General Public License, see the file COPYING.

   Everything that puts a window's text, a block of it or a string on disk
   for the user goes through here, and the outcome is one of three values a
   caller reads: written, declined or failed (we_write.h). */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "messages.h"
#include "edit.h"
#include "we_buffer.h"
#include "we_lineend.h"
#include "we_write.h"

/** The bytes the backup copy and the restore move at a time. */
#define WPE_WRITE_COPY_CHUNK 16384

/** A copy the owner makes is private to the user until it is complete. */
#define WPE_WRITE_COPY_MODE 0600

/** The window flag of a read-only window (what e_write refused). */
#define WPE_WRITE_WINDOW_READ_ONLY 8

/** The permission bits a restore by rename would change for someone else. */
#define WPE_WRITE_SHARED_BITS (S_IWGRP | S_IWOTH)

/** XWPE_TEST_FAIL: the name that makes the restore fail, for the test that
    shows what the user is told when the old content cannot be put back. */
#define WPE_WRITE_TEST_FAIL_ENV     "XWPE_TEST_FAIL"
#define WPE_WRITE_TEST_FAIL_RESTORE "restore"

/* --- SIGXFSZ -------------------------------------------------------------- */

static int write_fsize_depth;
static struct sigaction write_fsize_old;

/**
 * wpe_write_guard_enter - Ignore SIGXFSZ for the length of the owner's write.
 *
 * A write past the file size limit would kill the editor with the text in
 * it; ignored, it is EFBIG, a failed outcome the user is told about.  Only
 * around the owner's own writes and put back after, so a program the editor
 * starts later does not inherit an ignored signal.  The File Manager's copy
 * (we_fileop.c) wears the same guard around its bytes.
 */
void wpe_write_guard_enter(void)
{
 struct sigaction ign;

 if (write_fsize_depth++ > 0)
  return;
 memset(&ign, 0, sizeof ign);
 ign.sa_handler = SIG_IGN;
 sigemptyset(&ign.sa_mask);
 sigaction(SIGXFSZ, &ign, &write_fsize_old);
}

/** wpe_write_guard_leave - Put SIGXFSZ back as wpe_write_guard_enter found it. */
void wpe_write_guard_leave(void)
{
 if (--write_fsize_depth > 0)
  return;
 sigaction(SIGXFSZ, &write_fsize_old, NULL);
}

/* --- small helpers -------------------------------------------------------- */

/** write_base_name - The last part of a path, for a sentence about one file. */
static const char *write_base_name(const char *path)
{
 const char *slash = strrchr(path, '/');

 return(slash ? slash + 1 : path);
}

/** write_set_path - Record in the outcome where the text was going. */
static void write_set_path(WpeWriteOutcome *o, const char *path)
{
 snprintf(o->path, sizeof o->path, "%s", path);
}

/** write_error_number - The errno of a failure that left none: EIO. */
static int write_error_number(int err)
{
 return(err != 0 ? err : EIO);
}

/**
 * wpe_write_copy_fd - Copy what is left of one descriptor to another.
 * @from, @to: open descriptors; @read_failed: receives 1 when the error
 * was the read's, 0 when it was the write's, or NULL when the caller does
 * not care.
 * Return: 0, or the errno of the read or the write that failed.
 *
 * The one byte loop of the editor's copies: the backup, the restore and
 * the File Manager's Copy and Move all go through it, so a read error, a
 * full disk and a size limit end the loop at once and say which.
 */
int wpe_write_copy_fd(int from, int to, int *read_failed)
{
 char buf[WPE_WRITE_COPY_CHUNK];
 ssize_t got, put;
 size_t done;

 if (read_failed != NULL)
  *read_failed = 0;
 for (;;)
 {
  got = read(from, buf, sizeof buf);
  if (got < 0 && errno == EINTR)
   continue;
  if (got < 0)
  {
   if (read_failed != NULL)
    *read_failed = 1;
   return(errno);
  }
  if (got == 0)
   return(0);
  for (done = 0; done < (size_t)got; done += (size_t)put)
  {
   put = write(to, buf + done, (size_t)got - done);
   if (put < 0 && errno == EINTR)
   {
    put = 0;
    continue;
   }
   if (put < 0)
    return(errno);
   if (put == 0)
    return(EIO);
  }
 }
}

/* --- the mode of a file: the one owner of chmod ----------------------------- */

/** The special permission bits: setuid, setgid and sticky. */
#define WRITE_SPECIAL_BITS (S_ISUID | S_ISGID | S_ISVTX)

/** The nine permission bits a user can always ask for. */
#define WRITE_NINE_BITS 0777

/** The twelve bits of a mode. */
#define WRITE_TWELVE_BITS 07777

/** What a new file is asked to be before the umask takes its share. */
#define WRITE_NEW_FILE_MODE 0666

/** getgroups with no list says how many groups the caller is in. */
#define WRITE_NO_GROUPS 0

/** The place a mode is applied to: an open file, or a name (a file that
    cannot be opened for reading and writing, a folder the user names). */
typedef struct {
 int         fd;      /* the descriptor, used when path is NULL */
 const char *path;    /* the name, or NULL */
} WriteModeTarget;

/**
 * write_mode_set - The one chmod or fchmod of the editor's files.
 * @t: the file or the name; @mode: the bits to ask for.
 * Return: 0, or the errno.
 *
 * Every change of a file's permissions the write and the file owners make
 * goes through here, so a bit the system refuses is met in one place.
 */
static int write_mode_set(const WriteModeTarget *t, mode_t mode)
{
 int rc;

 if (t->path != NULL)
  rc = chmod(t->path, mode);
 else
  rc = fchmod(t->fd, mode);
 return(rc == 0 ? 0 : errno);
}

/** write_mode_read - The mode the file has now.  Return: 0, or the errno. */
static int write_mode_read(const WriteModeTarget *t, struct stat *st)
{
 int rc;

 if (t->path != NULL)
  rc = stat(t->path, st);
 else
  rc = fstat(t->fd, st);
 return(rc == 0 ? 0 : errno);
}

/**
 * write_mode_refusal - Is this errno a system saying "not this special bit"?
 * @err: what a chmod returned.
 * Return: 1 for EPERM, and for EFTYPE where the system has it (the BSDs
 * refuse a sticky bit on a regular file with it); 0 for every other errno,
 * which is a real failure (EIO, EROFS, ENOENT).
 */
static int write_mode_refusal(int err)
{
#ifdef EFTYPE
 if (err == EFTYPE)
  return(1);
#endif
 return(err == EPERM);
}

/**
 * wpe_write_caller_in_group - Is the user in this group?
 * @gid: a group number.
 * Return: 1 when it is the effective group or one of the supplementary
 * groups, else 0.
 *
 * Lets the sentence say "this user is not in group 12" when a setgid bit
 * cannot be kept, the real reason on every system, without reading
 * /etc/group.
 */
int wpe_write_caller_in_group(gid_t gid)
{
 gid_t *list;
 int count, i, found = 0;

 if (getegid() == gid)
  return(1);
 count = getgroups(WRITE_NO_GROUPS, NULL);
 if (count <= 0)
  return(0);
 list = malloc((size_t)count * sizeof *list);
 if (list == NULL)
  return(0);
 count = getgroups(count, list);
 for (i = 0; i < count; i++)
  if (list[i] == gid)
   found = 1;
 free(list);
 return(found);
}

/**
 * write_mode_read_back - Say which special bits the file does not have.
 * @t: the file; @mode: what was asked for; @k: receives lost, gid, in_group.
 * Return: 0, or the errno of the read.
 *
 * A system that returns 0 and drops the bit (Linux, for a user outside the
 * file's group) and one that refuses it with EPERM end alike: the bit is not
 * on the file, and the caller can say so.
 */
static int write_mode_read_back(const WriteModeTarget *t, mode_t mode, WpeModeKept *k)
{
 struct stat st;
 int err = write_mode_read(t, &st);

 if (err != 0)
  return(err);
 k->lost |= (mode & WRITE_SPECIAL_BITS) & ~(st.st_mode & WRITE_SPECIAL_BITS);
 k->gid = st.st_gid;
 k->in_group = wpe_write_caller_in_group(st.st_gid);
 return(0);
}

/**
 * write_mode_apart - Set the nine bits, then each special bit by itself.
 * @t: the file; @mode: the twelve bits wanted; @k: receives lost, err, changed.
 * Return: 0, or the errno of a call that is a real failure.
 *
 * A system that refuses one special bit refuses the whole call; asking for
 * the bits one at a time keeps the ones it does take (setuid stays when
 * setgid is refused).  The nine bits go first and alone: their refusal is the
 * user's request refused, and nothing was changed.
 */
static int write_mode_apart(const WriteModeTarget *t, mode_t mode, WpeModeKept *k)
{
 static const mode_t bits[] = { S_ISUID, S_ISGID, S_ISVTX };
 mode_t accepted = 0;
 size_t i;
 int err = write_mode_set(t, mode & WRITE_NINE_BITS);

 if (err != 0)
  return(err);
 k->changed = 1;
 for (i = 0; i < sizeof bits / sizeof bits[0]; i++)
 {
  if ((mode & bits[i]) == 0)
   continue;
  err = write_mode_set(t, (mode & WRITE_NINE_BITS) | accepted | bits[i]);
  if (err == 0)
   accepted |= bits[i];
  else if (write_mode_refusal(err))
  {
   k->lost |= bits[i];
   k->err = err;
  }
  else
   return(err);
 }
 return(0);
}

/**
 * write_mode_apply - Give a file the mode asked for, keeping what the system
 * lets this user keep.
 * @t: the file; @mode: the twelve bits wanted; @k: what was not kept.
 * Return: 0 when the nine bits are on, whatever happened to the special bits
 * (k says); else the errno of a real failure.
 *
 * With no special bit asked for this is one call and no read-back, as the
 * editor always did.  With one, the whole mode is asked for first, which is
 * what a system that takes it needs and leaves the file untouched when it
 * refuses; only a refusal that names a special bit sends the call apart.
 * The mode is read back after, so a bit dropped in silence is found too.
 */
static int write_mode_apply(const WriteModeTarget *t, mode_t mode, WpeModeKept *k)
{
 int err;

 memset(k, 0, sizeof *k);
 if ((mode & WRITE_SPECIAL_BITS) == 0)
  return(write_mode_set(t, mode & WRITE_NINE_BITS));
 err = write_mode_set(t, mode & WRITE_TWELVE_BITS);
 if (err == 0)
  k->changed = 1;
 else if (write_mode_refusal(err))
  err = write_mode_apart(t, mode, k);
 if (err != 0)
  return(err);
 return(write_mode_read_back(t, mode, k));
}

/**
 * wpe_write_set_mode_fd - Give an open file or folder its mode and say which
 * special bits it could not keep.
 * @fd: the file; @mode: the twelve bits; @k: receives what was not kept.
 * Return: 0, or the errno when even the nine bits were refused.
 *
 * For the folder a copy made, the wastebasket and the saved text: the user
 * gets every bit the system takes, and the caller can name the one it did
 * not, instead of the whole operation failing on it.
 */
int wpe_write_set_mode_fd(int fd, mode_t mode, WpeModeKept *k)
{
 WriteModeTarget t;

 t.fd = fd;
 t.path = NULL;
 return(write_mode_apply(&t, mode, k));
}

/**
 * wpe_write_set_mode_path - The same by name, for the Attributes dialog.
 * @path: the file or folder; @mode: the twelve bits; @k: receives what was
 * not kept.
 * Return: 0, or the errno when even the nine bits were refused.
 *
 * A file the user cannot open (write only, a folder) still gets its
 * permissions changed, and the special bits it had are kept when the
 * system allows it and named when it does not.
 */
int wpe_write_set_mode_path(const char *path, mode_t mode, WpeModeKept *k)
{
 WriteModeTarget t;

 t.fd = -1;
 t.path = path;
 return(write_mode_apply(&t, mode, k));
}

/**
 * wpe_write_bits_words - Name special bits, for a sentence.
 * @out, @n: receive the words; @bits: the special bits.
 *
 * "setgid bit", "setuid and setgid bits", "setuid, setgid and sticky bits".
 */
void wpe_write_bits_words(char *out, size_t n, mode_t bits)
{
 static const struct { mode_t bit; const char *name; } names[] = {
  { S_ISUID, "setuid" }, { S_ISGID, "setgid" }, { S_ISVTX, "sticky" }
 };
 const char *found[3];
 size_t i, count = 0;

 for (i = 0; i < sizeof names / sizeof names[0]; i++)
  if (bits & names[i].bit)
   found[count++] = names[i].name;
 if (count == 0)
  snprintf(out, n, "special bits");
 else if (count == 1)
  snprintf(out, n, "%s bit", found[0]);
 else if (count == 2)
  snprintf(out, n, "%s and %s bits", found[0], found[1]);
 else
  snprintf(out, n, "%s, %s and %s bits", found[0], found[1], found[2]);
}

/**
 * wpe_write_bits_reason - Why a special bit was not kept, in words.
 * @out, @n: receive the words; @k: what the mode owner reported.
 *
 * The same words on every system: a setgid bit the user could not have kept
 * because the file's group is not theirs says so (the BSDs refuse it with
 * EPERM, Linux drops it in silence, and both mean this); else the system's
 * own reason; else that the system cleared the bit.
 */
void wpe_write_bits_reason(char *out, size_t n, const WpeModeKept *k)
{
 if ((k->lost & S_ISGID) && !k->in_group)
  snprintf(out, n, "this user is not in group %lu", (unsigned long)k->gid);
 else if (k->err != 0)
  snprintf(out, n, "%s", strerror(k->err));
 else
  snprintf(out, n, "the system cleared it");
}

/**
 * wpe_write_create_private - Create a new file in a folder, private to the user.
 * @dirfd: the folder's descriptor, or AT_FDCWD; @name: the new file's name in it;
 * @mode: the permission bits the file is to have when it is complete;
 * @fd: receives the descriptor.
 * Return: 0, or the errno.
 *
 * The file is made with O_EXCL and O_NOFOLLOW, so it is new and a link of
 * that name is never written through, and it has the nine permission bits
 * of @mode before it holds a byte: a private file's copy is never readable
 * by others, not even for a moment.  The setuid, setgid and sticky bits
 * are not set here, because the kernel clears them on the first write;
 * wpe_write_set_special_bits puts them on after the data.  A file that
 * turns out not to be a regular file of ours is closed, removed and
 * refused with EEXIST.
 */
int wpe_write_create_private(int dirfd, const char *name, mode_t mode, int *fd)
{
 struct stat st;
 WriteModeTarget target;
 int made, err;

 made = openat(dirfd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
               WPE_WRITE_COPY_MODE);
 if (made < 0)
  return(errno);
 if (fstat(made, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid())
 {
  close(made);
  unlinkat(dirfd, name, 0);
  return(EEXIST);
 }
 target.fd = made;
 target.path = NULL;
 err = write_mode_set(&target, mode & WRITE_NINE_BITS);
 if (err != 0)
 {
  close(made);
  unlinkat(dirfd, name, 0);
  return(err);
 }
 *fd = made;
 return(0);
}

/**
 * wpe_write_set_special_bits - Put the setuid, setgid and sticky bits on a
 * finished copy.
 * @fd: the copy, whole and not yet closed; @mode: the original's mode;
 * @k: receives the special bits the system did not keep.
 * Return: 0, or the errno of a real failure (a refused special bit is not
 * one: it is in @k).
 *
 * Called after the last write: the kernel clears these bits when a file is
 * written, so setting them earlier would lose them.  A copy of a setgid
 * file is setgid when the system lets this user keep the bit, as 1.5.30a's
 * chmod after the data made it; when it does not, the copy is made without
 * it and the caller says which bit and why.  A mode with no special bit
 * costs no call: the nine bits are already on.
 */
int wpe_write_set_special_bits(int fd, mode_t mode, WpeModeKept *k)
{
 memset(k, 0, sizeof *k);
 if ((mode & WRITE_SPECIAL_BITS) == 0)
  return(0);
 return(wpe_write_set_mode_fd(fd, mode, k));
}

/* --- the backup copy ------------------------------------------------------ */

typedef enum {
 BACKUP_NONE,      /* nothing to back up: no such file, or not a regular file */
 BACKUP_MADE,      /* the .bak holds the file as it is, every byte on disk */
 BACKUP_SKIPPED,   /* it could not be made; the write goes on (backup_err) */
 BACKUP_STOPPED    /* it was begun and not finished: the save is refused */
} BackupResult;

/**
 * write_backup_clear_old - Make room for a new .bak.
 * @bak: the .bak's name; @o: receives backup_err.
 * Return: 0 when the name is free, -1 when something there stays.
 *
 * An old .bak that is a regular file of the user's is replaced, as the
 * original did; a link, a pipe or another user's file is left alone.
 */
static int write_backup_clear_old(const char *bak, WpeWriteOutcome *o)
{
 struct stat st;

 if (lstat(bak, &st) != 0)
  return(0);
 if (!S_ISREG(st.st_mode) || st.st_uid != getuid())
 {
  o->backup_err = EEXIST;
  return(-1);
 }
 if (unlink(bak) != 0)
 {
  o->backup_err = errno;
  return(-1);
 }
 return(0);
}

/**
 * write_backup_open - Open the source and create the .bak, private.
 * @path: the file; @bak: the .bak's name; @src, @dst: receive the
 * descriptors; @o: receives backup_err.
 * Return: BACKUP_NONE (no regular file to copy), BACKUP_SKIPPED (the .bak
 * cannot be created) or BACKUP_MADE (both are open).
 *
 * The .bak is created with O_EXCL and O_NOFOLLOW, mode 0600, and takes the
 * file's nine permission bits before it holds any of its text, so a private
 * file's backup is never readable by others, not even for a moment.  It is
 * never given the setuid, setgid or sticky bit: a second copy of a program
 * with the old text must not be a program that runs with another user's
 * rights.
 */
static BackupResult write_backup_open(const char *path, const char *bak, int *src, int *dst,
                                      WpeWriteOutcome *o)
{
 struct stat st;
 WpeModeKept kept;

 *src = open(path, O_RDONLY | O_CLOEXEC);
 if (*src < 0)
 {
  if (errno == ENOENT || errno == ENOTDIR)
   return(BACKUP_NONE);
  o->backup_err = errno;
  return(BACKUP_SKIPPED);
 }
 if (fstat(*src, &st) != 0 || !S_ISREG(st.st_mode))
 {
  close(*src);
  return(BACKUP_NONE);
 }
 if (write_backup_clear_old(bak, o) != 0)
 {
  close(*src);
  return(BACKUP_SKIPPED);
 }
 *dst = open(bak, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, WPE_WRITE_COPY_MODE);
 if (*dst < 0)
 {
  o->backup_err = errno;
  close(*src);
  return(BACKUP_SKIPPED);
 }
 /* The file's group, then its nine bits, before any data.  When either
    fails the .bak stays narrower than the file (0600, our group) and the
    restore writes it back through the file instead of renaming it into
    place. */
 if (fchown(*dst, (uid_t)-1, st.st_gid) != 0)
  errno = 0;
 (void)wpe_write_set_mode_fd(*dst, st.st_mode & WRITE_NINE_BITS, &kept);
 return(BACKUP_MADE);
}

/**
 * write_backup - Copy the file to its .bak before the write truncates it.
 * @path: the file; @o: receives bak_path, backup_err and backup_stopped.
 * Return: the BackupResult.
 *
 * Shows no box and asks no question.  The copy counts as made only when
 * every write and the close returned 0: a short .bak put back over the
 * user's file would be the loss the .bak exists to prevent.  A .bak that
 * cannot be created lets the write go on (the status row says so); one
 * that was begun and stopped (a full disk, a size limit) refuses the save,
 * because the same disk would refuse the new text and nothing could be put
 * back.
 */
static BackupResult write_backup(const char *path, WpeWriteOutcome *o)
{
 char *bak = e_bakfilename((char *)path);
 int src = -1, dst = -1, err;
 BackupResult r;

 if (!bak)
 {
  o->backup_err = ENOMEM;
  return(BACKUP_SKIPPED);
 }
 if (strcmp(bak, path) == 0)
 {
  FREE(bak);
  return(BACKUP_NONE);
 }
 if (strlen(bak) >= sizeof o->bak_path)
 {
  FREE(bak);
  o->backup_err = ENAMETOOLONG;
  return(BACKUP_SKIPPED);
 }
 r = write_backup_open(path, bak, &src, &dst, o);
 if (r != BACKUP_MADE)
 {
  FREE(bak);
  return(r);
 }
 err = wpe_write_copy_fd(src, dst, NULL);
 close(src);
 if (close(dst) != 0 && err == 0)
  err = errno;
 if (err != 0)
 {
  unlink(bak);
  FREE(bak);
  o->err = err;
  o->backup_stopped = 1;
  return(BACKUP_STOPPED);
 }
 snprintf(o->bak_path, sizeof o->bak_path, "%s", bak);
 FREE(bak);
 return(BACKUP_MADE);
}

/* --- putting the old content back ----------------------------------------- */

/**
 * write_back_through_fd - Write the .bak's bytes into the very file the
 * failed write went to.
 * @to: a second descriptor of that file, closed here; @bak: the .bak;
 * @keep: the mode to give back with the text when it has special bits (the
 * nine bits and those), else 0.
 * Return: 0, or the errno of what failed.
 *
 * The inode, the links, the owner and the group stay as they are, which a
 * rename could not promise; and the name is not looked up again, so a link
 * put in its place since the write cannot send the old content elsewhere.
 * The failed write cleared the file's setuid and setgid bits; they go back
 * with the text, so a program whose save failed is still the program it was.
 * A bit the system will not give back is not a reason to fail the restore:
 * the old text is on disk again.
 */
static int write_back_through_fd(int to, const char *bak, mode_t keep)
{
 WpeModeKept kept;
 int from, err;

 from = open(bak, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
 if (from < 0)
 {
  err = errno;
  close(to);
  return(err);
 }
 err = 0;
 if (ftruncate(to, 0) != 0 || lseek(to, 0, SEEK_SET) < 0)
  err = errno;
 if (err == 0)
  err = wpe_write_copy_fd(from, to, NULL);
 if (err == 0)
  (void)wpe_write_set_special_bits(to, keep, &kept);
 close(from);
 if (close(to) != 0 && err == 0)
  err = errno;
 return(err);
}

/**
 * write_restore_by_rename_ok - May the .bak replace the file by rename?
 * @path: the file as the user named it; @written: what fstat said of the
 * descriptor the write used.
 * @bak: the .bak; @keep: the mode with special bits the restore gives
 * back, or 0.
 * Return: 1 when the name is that very regular file, with one link, ours,
 * not writable by others, has no special bit to give back, and the .bak has
 * its group and its mode.
 *
 * A symlinked file, a hard link, another user's file and a shared one would
 * each be broken by a rename: the link replaced, the other names left on the
 * short inode, the owner or the group changed (a group that could read the
 * file would lose it).  A file that had a special bit is not renamed over
 * either: the .bak never has the bits, and the failed write has already
 * cleared them on the file, so only a write back through the file can give
 * them back.
 */
static int write_restore_by_rename_ok(const char *path, const char *bak,
                                      const struct stat *written, mode_t keep)
{
 struct stat now, copy;

 if ((keep & WRITE_SPECIAL_BITS) != 0)
  return(0);
 if (lstat(path, &now) != 0 || !S_ISREG(now.st_mode))
  return(0);
 if (now.st_ino != written->st_ino || now.st_dev != written->st_dev)
  return(0);
 if (now.st_nlink != 1 || now.st_uid != getuid())
  return(0);
 if ((now.st_mode & WPE_WRITE_SHARED_BITS) != 0)
  return(0);
 if (lstat(bak, &copy) != 0 || !S_ISREG(copy.st_mode))
  return(0);
 return(copy.st_gid == now.st_gid && (copy.st_mode & 07777) == (now.st_mode & 07777));
}

/**
 * write_restore - Put the file back as it was after a write failed half way.
 * @path: the file; @bak: the .bak made before the write; @written: fstat of
 * the descriptor the write used, or NULL when it could not be taken;
 * @again: a second descriptor of the written file, closed here, or -1;
 * @keep: the mode with special bits to give back with the text, or 0 (the
 * .bak never has them, so a file with one is written back, not renamed over).
 * Return: 0 when the file holds its old content again, else an errno.
 *
 * The .bak is renamed over the file when that changes nothing but the
 * content; otherwise its bytes are written back into the file the write
 * went to, with the special bits the file had.  XWPE_TEST_FAIL=restore
 * makes this fail with EIO.
 */
static int write_restore(const char *path, const char *bak, const struct stat *written,
                         int again, mode_t keep)
{
 const char *hook = getenv(WPE_WRITE_TEST_FAIL_ENV);

 if (hook && strcmp(hook, WPE_WRITE_TEST_FAIL_RESTORE) == 0)
 {
  if (again >= 0)
   close(again);
  return(EIO);
 }
 if (written && write_restore_by_rename_ok(path, bak, written, keep) && rename(bak, path) == 0)
 {
  if (again >= 0)
   close(again);
  return(0);
 }
 if (again < 0)
  return(EBADF);
 return(write_back_through_fd(again, bak, keep));
}

/* --- the write ------------------------------------------------------------ */

/**
 * write_keeps_file_ending - Does a write of this range leave out the final
 * line break the file did not have?
 * @f: the window written; @b: its buffer; the range as the write takes it.
 *
 * Only when the option "Keep Final newline" is on and the write is the
 * whole text: a block written to a file, or the part of a file below a
 * mark, is not the file's own end.  So a file read without a final line
 * break is saved without one, and so is a new or an empty file, and
 * `git diff` shows no change to a last line the user did not touch.
 * Return: 1 to leave the final break out when the buffer recorded none.
 */
static int write_keeps_file_ending(FENSTER *f, BUFFER *b, int xa, int ya, int xe, int ye)
{
 int whole = (xa == 0 && ya == 0 && xe >= E_BUFFER_ROW_END(b) && ye >= b->mxlines - 1);

 return(whole && (f->ed->edopt & ED_KEEP_FILE_ENDING) != 0);
}

/**
 * write_rows - Put a range of the buffer into an open file, judged.
 * @fp: the file; @f: the window; @b: its buffer; the range.
 * @written: receives fstat of the file when it could be taken.
 * Return: 0, or the errno of the first thing that failed (the rows, the
 * flush, the stream's error flag, the close).
 */
static int write_rows(FILE *fp, FENSTER *f, BUFFER *b, int xa, int ya, int xe, int ye,
                      struct stat *written, int *have_written)
{
 int failed, err;

 errno = 0;
 failed = (e_eol_write(fp, b, ya, xa, ye, xe, write_keeps_file_ending(f, b, xa, ya, xe, ye)) != 0);
 if (!failed && fflush(fp) != 0)
  failed = 1;
 if (!failed && ferror(fp))
  failed = 1;
 err = failed ? write_error_number(errno) : 0;
 *have_written = (fstat(fileno(fp), written) == 0);
 if (fclose(fp) != 0 && !failed)
  err = write_error_number(errno);
 return(err);
}

/**
 * write_after_failure - Say how a failed write left the file.
 * @o: the outcome; @path: the file; @bk: what the backup did; @written:
 * what the write's descriptor showed; @keep: the mode with special bits the
 * restore gives back, or 0.
 *
 * With a .bak made in this call the file is put back; when that fails too
 * the outcome says so and names the .bak that holds the old content.  With
 * no .bak the file was opened and truncated with nothing to put back, so
 * the outcome says the file may be cut short (restore_err ENOENT, no
 * bak_path).
 */
static void write_after_failure(WpeWriteOutcome *o, const char *path, BackupResult bk,
                                const struct stat *written, int have_written, int again,
                                mode_t keep)
{
 if (bk != BACKUP_MADE)
 {
  if (again >= 0)
   close(again);
  o->restore_err = ENOENT;
  return;
 }
 o->restore_err = write_restore(path, o->bak_path, have_written ? written : NULL, again,
                                keep);
}

/* --- the file a write goes to --------------------------------------------- */

/** What a write is for.  A Save overwrites the user's file and gives it back
    the special bits it had; a copy (Block Write, the autosave, the emergency
    save, the print copy, Save As onto another file) is a second file and
    never has them. */
typedef enum { WRITE_PURPOSE_SAVE, WRITE_PURPOSE_COPY } WritePurpose;

/** The file the window holds, as a caller knows it before the write: what
    stat said of its folder and name.  A caller with no such file passes NULL
    where a WriteOwnFile is asked for. */
typedef struct {
 struct stat st;
} WriteOwnFile;

/**
 * write_purpose_for_target - Is the file this write opened the window's own?
 * @own: what the caller knows of the window's own file, or NULL.
 * @opened: fstat of the file the write opened.
 * Return: Save when it is the same regular file (same device and inode),
 * else copy.
 *
 * The one place that decides it, by identity and never by name: the same
 * folder and name, a hard link to it and a path through a symbolic link all
 * reach one inode and one program, which keeps its bits; the same name in
 * another folder is another file, which gets none.  Callers pass what they
 * know and never the purpose; no knowledge means a copy, which fails safe.
 */
static WritePurpose write_purpose_for_target(const WriteOwnFile *own, const struct stat *opened)
{
 if (own == NULL || !S_ISREG(opened->st_mode))
  return(WRITE_PURPOSE_COPY);
 if (own->st.st_dev != opened->st_dev || own->st.st_ino != opened->st_ino)
  return(WRITE_PURPOSE_COPY);
 return(WRITE_PURPOSE_SAVE);
}

/** The file a write goes to, as the owner opened it.  The bits it had are
    read from this descriptor before anything is truncated, so they are the
    bits of the very inode that is overwritten and no other. */
typedef struct {
 int         fd;      /* open O_WRONLY|O_CREAT, never O_TRUNC */
 FILE       *fp;      /* fdopen(fd, "w"): the rows go through it, fclose closes fd */
 struct stat st;      /* fstat right after the open */
 mode_t      before;  /* the twelve bits the inode had, 0 when it is not a regular file
                         or was just made */
 mode_t      keep;    /* the special bits this write gives back: those of @before for a
                         Save, none for a copy */
} WriteTarget;

/**
 * write_target_open - Open the file a write goes to, without cutting it.
 * @path: the file; @own: the window's own file, or NULL; @t: receives the
 * target.
 * Return: 0, or the errno of what failed (nothing was truncated).
 *
 * The write clears a file's setuid and setgid bits in the open itself when
 * it truncates, so the bits of the file being saved can only be read before
 * that.  Reading them from this descriptor, and not from a name looked up
 * before or a mode recorded when the window loaded the file, means a bit the
 * user took off since stays off and a file that is new has none to get.
 */
static int write_target_open(const char *path, const WriteOwnFile *own, WriteTarget *t)
{
 int err;

 memset(t, 0, sizeof *t);
 t->fd = open(path, O_WRONLY | O_CREAT | O_NOCTTY | O_CLOEXEC, WRITE_NEW_FILE_MODE);
 if (t->fd < 0)
  return(errno);
 if (fstat(t->fd, &t->st) == 0)
  t->fp = fdopen(t->fd, "w");
 if (t->fp == NULL)
 {
  err = errno;
  close(t->fd);
  return(err);
 }
 if (S_ISREG(t->st.st_mode))
  t->before = t->st.st_mode & WRITE_TWELVE_BITS;
 if (write_purpose_for_target(own, &t->st) == WRITE_PURPOSE_SAVE)
  t->keep = t->before & WRITE_SPECIAL_BITS;
 return(0);
}

/**
 * write_target_truncate - Cut a regular file to nothing, now that its bits
 * have been read.
 * @t: the target.
 * Return: 0, or the errno of what failed (nothing was written; the stream
 * is still open).
 *
 * Every regular file is cut, whatever its size: a file that grew since it
 * was read would otherwise keep a tail of the old text after the new one.
 * A device or a pipe is not cut, as the open with O_TRUNC never cut it.
 */
static int write_target_truncate(const WriteTarget *t)
{
 if (!S_ISREG(t->st.st_mode))
  return(0);
 if (ftruncate(t->fd, 0) != 0)
  return(errno);
 return(0);
}

/**
 * write_mode_before_data - Give the file the window's nine permission bits
 * before it holds any text.
 * @t: the target; @f: the window.
 *
 * The only place the editor's recorded mode of a file (what the window read
 * when it loaded it) is consulted, and only for the nine bits, so an
 * autosave of a private file is private from its first byte.  Special bits
 * never come from here.  A mode that did not change is not a lost save: the
 * text is written.
 */
static void write_mode_before_data(const WriteTarget *t, FENSTER *f)
{
 WpeModeKept kept;

 if (f->filemode >= 0)
  (void)wpe_write_set_mode_fd(t->fd, (mode_t)f->filemode & WRITE_NINE_BITS, &kept);
}

/**
 * write_give_back_bits - Put special bits on a file that holds its text.
 * @fd: the file; @keep: the special bits; @k: receives the bits not kept.
 * Return: 0, or the errno of a real failure (a refused bit is in @k).
 *
 * The file keeps the nine permission bits it has now, which are the ones
 * the user's window gave it before the text.
 */
static int write_give_back_bits(int fd, mode_t keep, WpeModeKept *k)
{
 struct stat now;

 if (fstat(fd, &now) != 0)
  return(errno);
 return(wpe_write_set_special_bits(fd, (now.st_mode & WRITE_NINE_BITS) | keep, k));
}

/**
 * write_restore_mode - The mode a restore gives back with the old text.
 * @t: the target.
 * Return: the nine bits the file had and its special bits, or 0 when it had
 * none.
 *
 * A restore undoes a write that failed, for a copy as for a Save: the file
 * is the one the user had, with the bits it had, whatever the write was for.
 */
static mode_t write_restore_mode(const WriteTarget *t)
{
 mode_t special = t->before & WRITE_SPECIAL_BITS;

 if (special == 0)
  return(0);
 return((t->before & WRITE_NINE_BITS) | special);
}

/**
 * write_keep_special_bits - Put the saved file's setuid, setgid and sticky
 * bits back after the text, and record the ones it could not keep.
 * @fd: a descriptor of the saved file, or -1 when none could be made;
 * @fd_err: why there is none; @t: the target, whose @keep are the bits to
 * give back; @k: receives the bits not kept.
 *
 * The write clears these bits, so a setgid program saved by its owner would
 * stop being setgid without a word.  Called after the last byte, on the
 * descriptor of the file written, never on its name: a link put in the
 * name's place cannot be given the bits.  A bit the system refuses, drops
 * or cannot be asked for is recorded for the status row; the text is on
 * disk either way, so this never fails the save.
 */
static void write_keep_special_bits(int fd, int fd_err, const WriteTarget *t, WpeModeKept *k)
{
 int err;

 memset(k, 0, sizeof *k);
 if (t->keep == 0)
  return;
 if (fd >= 0)
  err = write_give_back_bits(fd, t->keep, k);
 else
  err = (fd_err != 0 ? fd_err : EBADF);
 if (err == 0)
  return;
 memset(k, 0, sizeof *k);
 k->lost = t->keep;
 k->err = err;
 k->in_group = 1;
}

/** The part of a window's text a write takes: from (xa, ya) to (xe, ye). */
typedef struct {
 int xa, ya, xe, ye;
} WriteRange;

/**
 * write_run_open - Open the file a write goes to and make it ready for text.
 * @f: the window; @path: the file; @own: the window's own file, or NULL; @t:
 * receives the target; @o: receives err when it fails.
 * Return: 0 when the file is open, cut to nothing and has the window's nine
 * permission bits; -1 when not (nothing was written, the old file is as it was).
 *
 * The bits of the file being overwritten are read here, before the cut.
 */
static int write_run_open(FENSTER *f, const char *path, const WriteOwnFile *own, WriteTarget *t,
                          WpeWriteOutcome *o)
{
 int err = write_target_open(path, own, t);

 if (err == 0)
 {
  err = write_target_truncate(t);
  if (err != 0)
   fclose(t->fp);
 }
 if (err != 0)
 {
  o->err = write_error_number(err);
  return(-1);
 }
 write_mode_before_data(t, f);
 return(0);
}

/**
 * write_run_finish - Put the rows into the open target, then give a Save's
 * file its special bits back, or put the old content back when it failed.
 * @f: the window; @path: the file; @r: the range; @t: the open target,
 * closed here; @bk: what the backup did; @o: receives the outcome.
 *
 * The restore, and the special bits after the text, go through a second
 * descriptor of this same file, never through whatever the name points at by
 * then.
 */
static void write_run_finish(FENSTER *f, const char *path, const WriteRange *r, WriteTarget *t,
                             BackupResult bk, WpeWriteOutcome *o)
{
 struct stat written;
 int have_written = 0, err, again, again_err;

 again = fcntl(t->fd, F_DUPFD_CLOEXEC, 0);
 again_err = errno;
 err = write_rows(t->fp, f, f->b, r->xa, r->ya, r->xe, r->ye, &written, &have_written);
 if (err != 0)
 {
  o->err = err;
  write_after_failure(o, path, bk, &written, have_written, again, write_restore_mode(t));
  return;
 }
 write_keep_special_bits(again, again_err, t, &o->file_kept);
 if (again >= 0)
  close(again);
 o->status = WPE_WRITE_WRITTEN;
}

/**
 * write_run - Write a range of a window's text to a path, judged.
 * @f: the window; @path: the file; @r: the range; @backup: 1 to copy an
 * existing file to its .bak first; @own: the window's own file, which gets
 * its special bits back, or NULL (every other file is a copy and gets none);
 * @o: receives the outcome.
 *
 * The one place a window's rows reach a file.  A write that fails after
 * the file was opened puts the old content back (write_restore) when a
 * .bak was made, and the outcome says what could not be done.
 */
static void write_run(FENSTER *f, const char *path, const WriteRange *r, int backup,
                      const WriteOwnFile *own, WpeWriteOutcome *o)
{
 BackupResult bk = BACKUP_NONE;
 WriteTarget t;

 memset(o, 0, sizeof *o);
 o->status = WPE_WRITE_FAILED;
 write_set_path(o, path);
 if (f->ins == WPE_WRITE_WINDOW_READ_ONLY)
 {
  o->err = EACCES;
  o->read_only = 1;
  return;
 }
 wpe_write_guard_enter();
 if (backup)
  bk = write_backup(path, o);
 if (bk != BACKUP_STOPPED && write_run_open(f, path, own, &t, o) == 0)
  write_run_finish(f, path, r, &t, bk, o);
 wpe_write_guard_leave();
}

/**
 * wpe_write_range - Write a range of a window's text to a file the user named.
 * @f: the window; @path: the file; @xa, @ya, @xe, @ye: the range; @backup: 1
 * to keep an existing file's old content in its .bak; @o: receives the outcome.
 * Return: 0 when written, -1 when not (the outcome says why).
 *
 * For Block Write and the print copy: the user finds the file holding the
 * block as the window wrote it, in the window's own line style, and a write
 * that fails says so instead of leaving a missing file.  The file is a copy:
 * it is never given a setuid, setgid or sticky bit, not even over a file that
 * had one, so a block of a setgid program never leaves a second program.
 */
int wpe_write_range(FENSTER *f, const char *path, int xa, int ya, int xe, int ye, int backup,
                    WpeWriteOutcome *o)
{
 WriteRange r;

 r.xa = xa;
 r.ya = ya;
 r.xe = xe;
 r.ye = ye;
 write_run(f, path, &r, backup, NULL, o);
 return(o->status == WPE_WRITE_WRITTEN ? 0 : -1);
}

/**
 * write_nul_confirmed - Before the first Save of a file that lost NUL bytes
 * when it was read, ask whether to write it without them.
 * @f: the window; @b: its buffer.
 *
 * A row cannot hold a NUL byte, so the file on disk is longer than what the
 * window holds.  Saving over it without a word would shorten a binary file
 * silently; answering No (or Cancel) leaves the disk as it is.
 * Return: 1 when the Save may go on (nothing was lost, or the user said Yes).
 */
static int write_nul_confirmed(FENSTER *f, BUFFER *b)
{
 char sentence[E_NUL_SAY_MAX], question[E_NUL_ASK_MAX];

 if (b->nul_dropped <= 0)
  return(1);
 e_eol_nul_sentence(b->nul_dropped, sentence, sizeof sentence);
 snprintf(question, sizeof question, "%s.\nSaving writes the file without them.\nSave anyway?",
          sentence);
 return(e_message(1, question, f) == 'Y');
}

/**
 * write_final_break_after - What the buffer records of its file's final
 * line break once the text was saved.
 * @f: the window saved; @b: its buffer.
 *
 * A save that wrote the break leaves the file ending on one.  A save that
 * kept the file's ending leaves the record as it was: a file that had no
 * final break still has none, and turning the option off later makes the
 * next save add it.
 */
static void write_final_break_after(FENSTER *f, BUFFER *b)
{
 if (f->ed->edopt & ED_KEEP_FILE_ENDING)
  return;
 b->final_break = E_FINAL_BREAK_HAD;
}

/**
 * write_window_listed - Is this window one of the desktop's text windows?
 * @f: the window.
 * Return: 1 for a text window the desktop holds, 0 for a file manager, a
 * list or a window that is gone (nothing to write).
 */
static int write_window_listed(FENSTER *f)
{
 int i;

 if (!DTMD_ISTEXT(f->dtmd))
  return(0);
 for (i = f->ed->mxedt; i > 0 && f->ed->f[i] != f; i--)
  ;
 return(i > 0);
}

/**
 * write_own_file - Take what stat says of the window's own file, before the
 * write touches anything.
 * @dir, @name: the folder and the name the window holds, or NULL for none;
 * @own: receives it.
 * Return: @own, or NULL when there is no such regular file or it cannot be
 * stat'ed (which makes the write a copy for the bits: it fails safe).
 */
static const WriteOwnFile *write_own_file(const char *dir, const char *name, WriteOwnFile *own)
{
 char *path;
 int ok;

 if (dir == NULL || name == NULL)
  return(NULL);
 path = e_mkfilename((char *)dir, (char *)name);
 if (path == NULL)
  return(NULL);
 ok = (stat(path, &own->st) == 0 && S_ISREG(own->st.st_mode));
 FREE(path);
 return(ok ? own : NULL);
}

/**
 * write_window_file - Write the whole window to the file its name says.
 * @f: the window; @own_dir, @own_name: the folder and name of the file the
 * window holds (they are its name too, except in a Save As); @o: receives the
 * outcome.
 * Return: 0 when the file holds the text (or there is nothing to write),
 * -1 when it does not (declined or failed; the outcome says which).
 *
 * Asks first when the file had NUL bytes the window cannot hold.  On success
 * the window no longer reads as modified; on any other outcome it stays as it
 * is, so the user still has the text and the next Save tries again.  The
 * window's own file is looked at once, before the write, so that the file the
 * write opens can be recognised as it (write_purpose_for_target).
 */
static int write_window_file(FENSTER *f, const char *own_dir, const char *own_name,
                             WpeWriteOutcome *o)
{
 WriteOwnFile own_file;
 BUFFER *b;
 WriteRange r;
 char *path;

 memset(o, 0, sizeof *o);
 o->status = WPE_WRITE_WRITTEN;           /* nothing to write is written */
 if (!write_window_listed(f))
  return(0);
 b = f->b;
 if (!write_nul_confirmed(f, b))
 {
  o->status = WPE_WRITE_DECLINED;
  return(-1);
 }
 path = e_mkfilename(f->dirct, f->datnam);
 if (path == NULL)
 {
  o->status = WPE_WRITE_FAILED;
  o->err = ENOMEM;
  return(-1);
 }
 WpeMouseChangeShape(WpeWorkingShape);
 r.xa = 0;
 r.ya = 0;
 r.xe = E_BUFFER_ROW_END(b);
 r.ye = b->mxlines - 1;
 write_run(f, path, &r, 1, write_own_file(own_dir, own_name, &own_file), o);
 FREE(path);
 if (o->status == WPE_WRITE_WRITTEN)
 {
  f->save = 0;
  write_final_break_after(f, b);
  b->nul_dropped = 0;                   /* the user knew, and the file is written */
 }
 WpeMouseRestoreShape();
 return(o->status == WPE_WRITE_WRITTEN ? 0 : -1);
}

/**
 * wpe_write_file - Write the whole window to its own file: what Save does.
 * @f: the window; @o: receives the outcome.
 * Return: 0 when the file holds the text (or there is nothing to write),
 * -1 when it does not (declined or failed; the outcome says which).
 *
 * The file is the one the window holds, overwritten in place: the setuid,
 * setgid and sticky bits it has on disk when the save opens it are put back
 * after the text, so a setgid program saved from the editor is still one.
 */
int wpe_write_file(FENSTER *f, WpeWriteOutcome *o)
{
 return(write_window_file(f, f->dirct, f->datnam, o));
}

/* --- text that is not a window's rows ------------------------------------- */

/**
 * write_fd_into - Write all of a string to a descriptor and close it, judged.
 * @fd: an open descriptor, closed here; @text: the bytes; @o: receives the
 * outcome (path and the rest are the caller's).
 */
static void write_fd_into(int fd, const char *text, WpeWriteOutcome *o)
{
 size_t len = strlen(text), done = 0;
 ssize_t put;
 int err = 0;

 wpe_write_guard_enter();
 while (done < len)
 {
  put = write(fd, text + done, len - done);
  if (put < 0 && errno == EINTR)
   continue;
  if (put <= 0)
  {
   err = write_error_number(put < 0 ? errno : 0);
   break;
  }
  done += (size_t)put;
 }
 if (close(fd) != 0 && err == 0)
  err = errno;
 wpe_write_guard_leave();
 o->err = err;
 o->status = (err == 0 ? WPE_WRITE_WRITTEN : WPE_WRITE_FAILED);
}

/**
 * wpe_write_fd - Write a string to an open descriptor and close it, judged.
 * @fd: the descriptor, closed here; @text: the bytes; @o: receives the outcome.
 * Return: 0 when every byte and the close succeeded, else -1.
 *
 * For text that is not a window's rows: a full disk or a size limit is a
 * failed outcome the caller reports, not a half file nobody notices.
 */
int wpe_write_fd(int fd, const char *text, WpeWriteOutcome *o)
{
 memset(o, 0, sizeof *o);
 write_fd_into(fd, text, o);
 return(o->status == WPE_WRITE_WRITTEN ? 0 : -1);
}

/**
 * wpe_write_text - Create or replace a file with a string, judged.
 * @path: the file; @text: the bytes; @o: receives the outcome, with the path.
 * Return: 0 when written, else -1.
 */
int wpe_write_text(const char *path, const char *text, WpeWriteOutcome *o)
{
 int fd;

 memset(o, 0, sizeof *o);
 write_set_path(o, path);
 fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
 if (fd < 0)
 {
  o->status = WPE_WRITE_FAILED;
  o->err = errno;
  return(-1);
 }
 write_fd_into(fd, text, o);
 return(o->status == WPE_WRITE_WRITTEN ? 0 : -1);
}

/**
 * wpe_write_stream_open - Create or replace a file the caller prints into.
 * @path: the file; @o: receives the outcome, with the path.
 * Return: the stream, or NULL when the file could not be opened (the outcome
 * is FAILED and says why).
 *
 * For a list of unknown length (the project's lines, the options): the user
 * who names a folder that refuses the file hears the system's reason instead
 * of a guess.  The caller must end with wpe_write_stream_close and read its
 * result; the file size limit is ignored until then, so a full limit is a
 * failed outcome and not the end of the editor.
 */
FILE *wpe_write_stream_open(const char *path, WpeWriteOutcome *o)
{
 FILE *fp;
 int fd;

 memset(o, 0, sizeof *o);
 write_set_path(o, path);
 fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
 if (fd < 0)
 {
  o->status = WPE_WRITE_FAILED;
  o->err = errno;
  return(NULL);
 }
 fp = fdopen(fd, "w");
 if (fp == NULL)
 {
  o->status = WPE_WRITE_FAILED;
  o->err = write_error_number(errno);
  close(fd);
  return(NULL);
 }
 wpe_write_guard_enter();
 o->status = WPE_WRITE_WRITTEN;
 return(fp);
}

/**
 * wpe_write_stream_close - End a stream of wpe_write_stream_open, judged.
 * @fp: the stream, closed here; @o: the outcome of the open, updated.
 * Return: 0 when every byte reached the file, else -1.
 *
 * The file was emptied when it was opened, so a failure here leaves it cut
 * short: the outcome says so (no backup copy exists for a stream), and the
 * sentence the user reads says the file may be cut short.
 */
int wpe_write_stream_close(FILE *fp, WpeWriteOutcome *o)
{
 int err = 0;

 if (fflush(fp) != 0)
  err = write_error_number(errno);
 if (err == 0 && ferror(fp))
  err = EIO;
 if (fclose(fp) != 0 && err == 0)
  err = write_error_number(errno);
 wpe_write_guard_leave();
 if (err == 0)
  return(0);
 o->status = WPE_WRITE_FAILED;
 o->err = err;
 o->restore_err = ENOENT;
 o->bak_path[0] = '\0';
 return(-1);
}

/* --- the one message ------------------------------------------------------ */

/**
 * write_then_text - The sentence that says what is kept after a failure.
 * @o: the outcome; @then: what the caller keeps.
 * Return: a constant sentence, or "" when @then has none to say.
 */
static const char *write_then_text(const WpeWriteOutcome *o, WpeWriteThen then, char *buf,
                                   size_t n)
{
 switch (then)
 {
 case WPE_WRITE_THEN_NAME_KEPT:
  if (o->keep_name[0] != '\0')
  {
   snprintf(buf, n, "The window keeps its name %s.", write_base_name(o->keep_name));
   return(buf);
  }
  return("Your text is still in the window.");
 case WPE_WRITE_THEN_NO_BUILD:
  return("The build was not started.");
 case WPE_WRITE_THEN_NO_PRINT:
  return("Nothing was printed.");
 case WPE_WRITE_THEN_NO_PROJECT:
  return("No project was created.");
 case WPE_WRITE_THEN_PROJECT_KEPT:
  return("The project list is still in its window.");
 case WPE_WRITE_THEN_NO_OPTIONS:
  return("The options were not saved.");
 case WPE_WRITE_THEN_USER_KEYS_STAY:
  return("Your own settings (hook, engine command, endpoints, dial) are still in it; "
         "take them out before you share the file.");
 case WPE_WRITE_THEN_KEPT:
 default:
  return("Your text is still in the window.");
 }
}

/**
 * write_failed_head - The first sentence of a failed write.
 * @out, @n: the room; @o: the outcome.
 *
 * A read-only window, a backup copy that stopped and a restore that failed
 * each say what the user must know about the file on disk; every other
 * failure says the path and the reason.
 */
static void write_failed_head(char *out, size_t n, const WpeWriteOutcome *o)
{
 const char *path = (o->path[0] != '\0' ? o->path : "the file");

 if (o->read_only)
  snprintf(out, n, "%s is read-only.", write_base_name(path));
 else if (o->backup_stopped)
  snprintf(out, n, "Cannot write %s: the backup copy stopped at %s; the file was not touched.",
           path, strerror(o->err));
 else if (o->restore_err != 0 && o->bak_path[0] != '\0')
  snprintf(out, n, "Cannot write %s: %s.  The file on disk may be cut short; its previous "
           "content is in %s.", path, strerror(o->err), o->bak_path);
 else if (o->restore_err != 0)
  snprintf(out, n, "Cannot write %s: %s.  The file on disk may be cut short; no backup copy "
           "was made.", path, strerror(o->err));
 else
  snprintf(out, n, "Cannot write %s: %s.", path, strerror(o->err));
}

/**
 * write_bits_lost_sentence - Say that a saved file lost a special bit, and why.
 * @out, @n: the room; @base: the file's name; @k: what was not kept.
 *
 * Shown on the status row after a save that worked, so the user who saves a
 * setgid program learns at once that it no longer is one, instead of when
 * it stops working.
 */
static void write_bits_lost_sentence(char *out, size_t n, const char *base, const WpeModeKept *k)
{
 char words[64], reason[WPE_PATH_SIZE];
 int several = ((k->lost & (k->lost - 1)) != 0);

 wpe_write_bits_words(words, sizeof words, k->lost);
 wpe_write_bits_reason(reason, sizeof reason, k);
 snprintf(out, n, "%s written, but the %s %s not kept: %s.", base, words,
          several ? "were" : "was", reason);
}

/**
 * wpe_write_sentence - The one sentence that tells the user how a write ended.
 * @out, @n: the room; @o: the outcome; @then: what the caller keeps.
 * Return: its length; 0 (and an empty @out) when there is nothing to say.
 *
 * A failure names the path and the reason and what is kept, so the user
 * knows whether the text is on disk and where it still is.  A save that
 * worked says something only when it made no backup.
 */
int wpe_write_sentence(char *out, size_t n, const WpeWriteOutcome *o, WpeWriteThen then)
{
 char head[WPE_WRITE_SENTENCE_MAX], name[WPE_PATH_SIZE + 64];
 const char *base = write_base_name(o->path);

 if (n == 0)
  return(0);
 out[0] = '\0';
 if (o->status == WPE_WRITE_DECLINED)
  return(0);
 if (o->status == WPE_WRITE_WRITTEN)
 {
  if (o->backup_err != 0)
   snprintf(out, n, "%s written; no backup was made: %s.", base, strerror(o->backup_err));
  else if (o->kept)
   snprintf(out, n, "%s's copy could not go beside the file (%s); it is in %s.", base,
            strerror(o->err), o->kept_path);
  else if (o->file_kept.lost != 0)
   write_bits_lost_sentence(out, n, base, &o->file_kept);
  return((int)strlen(out));
 }
 write_failed_head(head, sizeof head, o);
 snprintf(out, n, "%s  %s", head, write_then_text(o, then, name, sizeof name));
 return((int)strlen(out));
}

/**
 * wpe_write_say - Tell the user how a write ended: a box for a failure the
 * user asked for, the status row for a note on a save that worked.
 * @f: the window the user is in; @o: the outcome; @then: what the caller keeps.
 *
 * Says nothing while the editor is ending and saving its copies: nobody
 * can answer a box then.
 */
void wpe_write_say(FENSTER *f, const WpeWriteOutcome *o, WpeWriteThen then)
{
 char text[WPE_WRITE_SENTENCE_MAX];

#ifdef UNIX
 /* The editor is ending and writing its copies: nobody can answer a box,
    and the terminal may be gone.  The exit's report says what was kept. */
 if (e_emergency_save_running())
  return;
#endif
 if (wpe_write_sentence(text, sizeof text, o, then) == 0)
  return;
 if (o->status == WPE_WRITE_FAILED)
  e_message(0, text, f);
 else
  e_status_say_until_key(f, text);
}

/* --- what F2 and the commands call ---------------------------------------- */

/**
 * wpe_write_save - Save the window and tell the user how it went.
 * @f: the window; @then: what a failed save leaves (the text, or the name
 * Save As keeps); @keep_dir, @keep_name: the folder and name the window had
 * before a Save As, or NULL and NULL for a plain Save.
 * Return: the WpeWriteStatus: the caller keeps the window, the name or the
 * build running only on WPE_WRITE_WRITTEN.
 *
 * A Save As that writes back onto the file the window holds (the same name,
 * or a hard link to it) is a Save, and the setgid program stays one; onto
 * any other file it is a copy and gives that file no special bit.
 */
int wpe_write_save(FENSTER *f, WpeWriteThen then, const char *keep_dir, const char *keep_name)
{
 WpeWriteOutcome o;
 const char *own_dir = (keep_name != NULL ? keep_dir : f->dirct);
 const char *own_name = (keep_name != NULL ? keep_name : f->datnam);

 if (write_window_file(f, own_dir, own_name, &o) != 0 && keep_name != NULL)
  snprintf(o.keep_name, sizeof o.keep_name, "%s", keep_name);
 wpe_write_say(f, &o, then);
 return(o.status);
}

/**
 * e_save - Save a window: what F2, File > Save, Close and the builds call.
 * @f: the window.
 * Return: the WpeWriteStatus; WPE_WRITE_WRITTEN also for a window with
 * nothing to write.  Callers test the status, never a number.
 */
int e_save(FENSTER *f)
{
 return(wpe_write_save(f, WPE_WRITE_THEN_KEPT, NULL, NULL));
}

/**
 * e_write - Write a range of a window to the file its dirct and datnam name.
 * @xa, @ya, @xe, @ye: the range; @f: the window; @backup: WPE_BACKUP to keep
 * the file's old content in its .bak.
 * Return: 0 when written, WPE_ESC when not (the user has been told).
 *
 * For the writers of a copy that borrow the window's name (the autosave
 * and the emergency save): they move to wpe_write_copy with the pass that
 * makes the copies.
 */
int e_write(int xa, int ya, int xe, int ye, FENSTER *f, int backup)
{
 WpeWriteOutcome o;
 char *path = e_mkfilename(f->dirct, f->datnam);
 int rc;

 if (path == NULL)
  return(WPE_ESC);
 rc = wpe_write_range(f, path, xa, ya, xe, ye, backup == WPE_BACKUP, &o);
 FREE(path);
 if (rc == 0)
  return(0);
 wpe_write_say(f, &o, WPE_WRITE_THEN_KEPT);
 return(WPE_ESC);
}
