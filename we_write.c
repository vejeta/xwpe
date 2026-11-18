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
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "messages.h"
#include "edit.h"
#include "we_buffer.h"
#include "we_lineend.h"
#include "we_path.h"
#include "we_places.h"
#include "we_write.h"

/** The bytes the backup copy and the restore move at a time. */
#define WPE_WRITE_COPY_CHUNK 16384

/** A copy the owner makes is private to the user until it is complete. */
#define WPE_WRITE_COPY_MODE 0600

/** The window flag of a read-only window (what the write owner refuses). */
#define WPE_WRITE_WINDOW_READ_ONLY 8

/** The permission bits a restore by rename would change for someone else. */
#define WPE_WRITE_SHARED_BITS (S_IWGRP | S_IWOTH)

/** XWPE_TEST_FAIL: the name that makes the restore fail, for the test that
    shows what the user is told when the old content cannot be put back. */
#define WPE_WRITE_TEST_FAIL_ENV     "XWPE_TEST_FAIL"
#define WPE_WRITE_TEST_FAIL_RESTORE "restore"

/** XWPE_TEST_FAIL=swap-bak: before the restore looks at the .bak's name, a
    file called <bak>.swap takes that name, as another user of the folder
    could do between the backup and the restore. */
#define WPE_WRITE_TEST_FAIL_SWAP_BAK "swap-bak"
#define WPE_WRITE_TEST_SWAP_SUFFIX   ".swap"

/** The errno a refused save carries when its name leads to another file by
    the time the backup reads it.  Callers that test err see a failure; the
    user is told the editor's own sentence (write_failed_head), never this. */
#define WPE_WRITE_REPLACED_ERR EBUSY

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

/** Room for "user <name>" or "group <name>" in a sentence. */
#define WRITE_WHO_WORDS 96

/** How a mode reaches its file: through a descriptor that was opened, or,
    for a file that cannot be opened, through its name. */
typedef enum { WRITE_MODE_BY_FD, WRITE_MODE_BY_NAME } WriteModeHow;

/** The place a mode is applied to.  By descriptor it is an open file or
    folder.  By name it is a file the user named, with what lstat said of it
    before anything was opened, so that the name can be checked afterwards. */
typedef struct {
 WriteModeHow how;
 int          fd;        /* the descriptor, for BY_FD; -1 when none was opened */
 const char  *name;      /* the name, for BY_NAME and for the open */
 struct stat  first;     /* what lstat said of the name before it was opened */
 char         real[WPE_PATH_SIZE];  /* where a link the user named leads */
 int          replaced;  /* the name stopped leading to the file `first` describes */
 mode_t       replaced_kind;  /* S_IFMT of what holds the name then; 0 when none, or gone */
} WriteModeTarget;

/** What the name leads to now, compared with what lstat saw of it first. */
typedef enum {
 WRITE_NAME_GONE,    /* lstat fails: nothing is at the name */
 WRITE_NAME_SAME,    /* the file lstat saw first, and not a link */
 WRITE_NAME_CHANGED  /* another file, a link included, holds the name */
} WriteNameNow;

/**
 * write_mode_set - The one chmod, fchmod or fchmodat of the editor's files.
 * @t: the file or the name; @mode: the bits to ask for.
 * Return: 0, or the errno.
 *
 * Every change of a file's permissions the write and the file owners make
 * goes through here, so a bit the system refuses is met in one place.  A
 * name is changed without following a link (AT_SYMLINK_NOFOLLOW): Linux and
 * glibc refuse such a call on a link with EOPNOTSUPP, nothing changed; the
 * BSDs and macOS give lchmod's meaning and change the link itself.  Either
 * way a link that took the name is never followed, and the caller checks
 * afterwards that the name still led to the file it read.
 */
static int write_mode_set(const WriteModeTarget *t, mode_t mode)
{
 int rc;

 if (t->how == WRITE_MODE_BY_NAME)
  rc = fchmodat(AT_FDCWD, t->name, mode, AT_SYMLINK_NOFOLLOW);
 else
  rc = fchmod(t->fd, mode);
 return(rc == 0 ? 0 : errno);
}

/** write_mode_read - The mode the file has now.  Return: 0, or the errno. */
static int write_mode_read(const WriteModeTarget *t, struct stat *st)
{
 int rc;

 if (t->how == WRITE_MODE_BY_NAME)
  rc = lstat(t->name, st);
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
 k->asked = mode & WRITE_SPECIAL_BITS;
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

 t.how = WRITE_MODE_BY_FD;
 t.fd = fd;
 t.name = NULL;
 return(write_mode_apply(&t, mode, k));
}

/** The flags every descriptor taken for a change of mode shares: a link is
    never followed, a FIFO is never waited on, and nothing becomes the
    terminal of the editor. */
#define WRITE_MODE_OPEN_FLAGS (O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC)

/**
 * write_mode_same_file - Is this the file the name led to the first time?
 * @first: what lstat said then; @now: what the file in hand says.
 * Return: 1 for the same device, inode and kind of file, and not a link.
 */
static int write_mode_same_file(const struct stat *first, const struct stat *now)
{
 if (S_ISLNK(now->st_mode))
  return(0);
 if (first->st_dev != now->st_dev || first->st_ino != now->st_ino)
  return(0);
 return((first->st_mode & S_IFMT) == (now->st_mode & S_IFMT));
}

/**
 * write_name_look - Look at what holds a name right now, without following a link.
 * @path: the name; @st: receives what lstat says.
 * Return: 1 when something holds the name, 0 when lstat fails.
 *
 * The one look every decision of "another file took the name" is made
 * from, in the mode owner and in the write owner (write_name_kind,
 * write_mode_name_changed and write_mode_shown_kind all call it).  What a
 * system's open or chmod answered for such a file (ELOOP, EMLINK, EFTYPE,
 * ENXIO, EOPNOTSUPP) is one system's word for it and is never asked.
 */
static int write_name_look(const char *path, struct stat *st)
{
 return(lstat(path, st) == 0);
}

/**
 * write_name_kind - What kind of thing holds this name right now.
 * @path: the name.
 * Return: S_IFMT of what write_name_look sees (S_IFLNK for a link), or 0
 * when nothing holds the name.
 */
static mode_t write_name_kind(const char *path)
{
 struct stat now;

 if (!write_name_look(path, &now))
  return(0);
 return(now.st_mode & S_IFMT);
}

/**
 * wpe_write_name_seen - Remember what a name was when the user was shown it.
 * @path: the name; @seen: receives its kind, device and inode, looked at
 * without following a link.
 *
 * For the Attributes dialog, which shows the file a link leads to.  When the
 * user presses Ok the owner can then tell a link that was there all along
 * (only its target changed) from a link that took the name since, and the
 * sentence the user reads is true of what happened to the name.  known stays
 * 0 when the name cannot be looked at.
 */
void wpe_write_name_seen(const char *path, WpeNameSeen *seen)
{
 struct stat st;

 memset(seen, 0, sizeof *seen);
 if (!write_name_look(path, &st))
  return;
 seen->known = 1;
 seen->kind = st.st_mode & S_IFMT;
 seen->dev = st.st_dev;
 seen->ino = st.st_ino;
}

/**
 * write_mode_shown_kind - What kind of thing took the name after the dialog.
 * @path: the name the user was shown; @seen: what it was then, or NULL.
 * Return: S_IFMT of what holds the name now, but S_IFLNK only when that is
 * true of the name: the name was not a link when it was shown, or it is
 * another link than the one shown.  The same link with another target is 0
 * ("another file", not "a link").  Without @seen a link is never claimed (0):
 * the name may have been a link all along, and the sentence must not say
 * otherwise.
 */
static mode_t write_mode_shown_kind(const char *path, const WpeNameSeen *seen)
{
 struct stat now;

 if (!write_name_look(path, &now))
  return(0);
 if (!S_ISLNK(now.st_mode))
  return(now.st_mode & S_IFMT);
 if (seen == NULL || !seen->known)
  return(0);
 if (seen->kind == S_IFLNK && seen->dev == now.st_dev && seen->ino == now.st_ino)
  return(0);
 return(S_IFLNK);
}

/**
 * write_mode_name_changed - Did the name change hands since the user named it?
 * @t: the target; t->replaced_kind receives what holds the name now when
 * another file does.
 * Return: WRITE_NAME_CHANGED for a link, a socket, a hard link or any other
 * file put under the name, whatever the system said when it refused to
 * open it; WRITE_NAME_SAME for the file the first lstat saw;
 * WRITE_NAME_GONE when nothing is at the name (the open's own errno then
 * tells why).
 */
static WriteNameNow write_mode_name_changed(WriteModeTarget *t)
{
 struct stat now;

 if (!write_name_look(t->name, &now))
  return(WRITE_NAME_GONE);
 if (write_mode_same_file(&t->first, &now))
  return(WRITE_NAME_SAME);
 t->replaced_kind = now.st_mode & S_IFMT;
 return(WRITE_NAME_CHANGED);
}

/**
 * write_mode_name_first - Look at the name the user gave, once.
 * @path: the name; @t: receives what lstat says and the name to open.
 * Return: 0, or the errno.
 *
 * A link the user named is followed here, once, as the Attributes dialog
 * and a single Copy always followed one: it leads to the file the user
 * means.  Everything after this treats a link as a file that was swapped in.
 */
static int write_mode_name_first(const char *path, WriteModeTarget *t)
{
 t->name = path;
 if (lstat(path, &t->first) != 0)
  return(errno);
 if (!S_ISLNK(t->first.st_mode))
  return(0);
 if (realpath(path, t->real) == NULL)
  return(errno);
 t->name = t->real;
 return(lstat(t->name, &t->first) == 0 ? 0 : errno);
}

/**
 * write_mode_open_descriptor - Open the file whose mode is to change.
 * @t: the target, with its first lstat.
 * Return: 0 with t->fd open, or the errno of the last open that failed.
 *
 * Read only first, which every file of ours allows.  A regular file the
 * user may write but not read (mode 0200) is opened write only after
 * EACCES: a descriptor of either kind takes fchmod.  A folder cannot be
 * opened for writing, so it has no second try.
 */
static int write_mode_open_descriptor(WriteModeTarget *t)
{
 int err;

 t->fd = open(t->name, O_RDONLY | WRITE_MODE_OPEN_FLAGS);
 if (t->fd >= 0)
  return(0);
 err = errno;
 if (err != EACCES || !S_ISREG(t->first.st_mode))
  return(err);
 t->fd = open(t->name, O_WRONLY | WRITE_MODE_OPEN_FLAGS);
 if (t->fd >= 0)
  return(0);
 return(errno);
}

/**
 * write_mode_check_descriptor - Is the open file the one lstat saw?
 * @t: the target with t->fd open.
 * Return: 0, or ESTALE (and t->replaced, the descriptor closed) when the
 * name had been given to another file between the lstat and the open.
 */
static int write_mode_check_descriptor(WriteModeTarget *t)
{
 struct stat now;

 if (fstat(t->fd, &now) == 0 && write_mode_same_file(&t->first, &now))
  return(0);
 close(t->fd);
 t->fd = -1;
 t->replaced = 1;
 (void)write_mode_name_changed(t);
 return(ESTALE);
}

/**
 * write_mode_open_failed - What a failed open says of the name.
 * @t: the target; @err: the errno of the open.
 * Return: 0 when the file can still be changed through its name, else the
 * errno to report (ESTALE with t->replaced when another file holds the
 * name, the open's own errno otherwise).
 *
 * The name is looked at again, never the errno.  A link, a socket or any
 * other file put where the file was is another file under the name:
 * refused with ESTALE and never worked around, the same on every system,
 * whatever each one calls the refusal.  Only when it is still the file
 * the user named does a system saying "not for you" (EACCES, EPERM) send it
 * to the name.  When nothing is at the name the open's errno stands: a
 * closed parent folder is not "another file".
 */
static int write_mode_open_failed(WriteModeTarget *t, int err)
{
 WriteNameNow now = write_mode_name_changed(t);

 if (now == WRITE_NAME_CHANGED)
 {
  t->replaced = 1;
  return(ESTALE);
 }
 if (now == WRITE_NAME_SAME && (err == EACCES || err == EPERM))
  return(0);
 return(err);
}

/**
 * write_mode_target_open - Reach the file the user named, for a change of mode.
 * @path: the name; @t: receives the target.
 * Return: 0, or the errno.
 *
 * @shown: what the user was shown when naming the file (the Attributes
 * dialog's stat), or NULL when nothing was shown.  When given, the file the
 * name leads to must be that file (device and inode): a name that changed
 * hands while the dialog was open is refused before anything is opened.
 * @seen: what the name itself was when it was shown (wpe_write_name_seen),
 * or NULL when not known; it decides only whether the sentence may say a
 * link took the name.
 *
 * A regular file or a folder is opened (never through a link that took the
 * name since lstat) and compared by device and inode with what lstat saw:
 * the mode that is read and the mode that is given are the same file's.  A
 * FIFO, a socket or a device is changed by name, because opening one has
 * effects on others: a tape rewinds when closed, and a FIFO opened for
 * reading, even O_NONBLOCK, counts as a reader, so a writer blocked in its
 * open is released and then killed by SIGPIPE when the descriptor closes.
 * A change of mode must not release or kill another process.  A file this
 * user may neither read nor write, or a folder with no read permission,
 * cannot be opened but its owner may still change it, also by name.  No
 * O_PATH is used: it is Linux's, and fchmod on such a descriptor is
 * refused there.
 */
static int write_mode_target_open(const char *path, const struct stat *shown, const WpeNameSeen *seen,
                                  WriteModeTarget *t)
{
 int err;

 t->how = WRITE_MODE_BY_FD;
 t->fd = -1;
 t->replaced = 0;
 t->replaced_kind = 0;
 err = write_mode_name_first(path, t);
 if (err != 0)
  return(err);
 if (shown != NULL && !write_mode_same_file(shown, &t->first))
 {
  t->replaced = 1;
  t->replaced_kind = write_mode_shown_kind(path, seen);
  return(ESTALE);
 }
 if (!S_ISREG(t->first.st_mode) && !S_ISDIR(t->first.st_mode))
 {
  t->how = WRITE_MODE_BY_NAME;
  return(0);
 }
 err = write_mode_open_descriptor(t);
 if (err == 0)
  return(write_mode_check_descriptor(t));
 err = write_mode_open_failed(t, err);
 if (err != 0)
  return(err);
 t->how = WRITE_MODE_BY_NAME;
 return(0);
}

/** write_mode_target_close - Let go of the descriptor a target holds. */
static void write_mode_target_close(WriteModeTarget *t)
{
 if (t->fd >= 0)
  close(t->fd);
 t->fd = -1;
}

/**
 * write_mode_name_still_leads - After a change by name, is it the same file?
 * @t: the target; @err: what the change returned.
 * Return: @err, or ESTALE (with t->replaced) when the name leads to another
 * file than the one lstat saw, a link included, or to nothing.
 *
 * Nothing is trusted that was done through a name: a file swapped in
 * after the first lstat is told, not reported as changed.  On a system
 * whose fchmodat acts on a link the swapped file is a link; here too the
 * name no longer leads to what was read.  What the call did to a file
 * that was swapped in cannot be undone; it can only be said.
 */
static int write_mode_name_still_leads(WriteModeTarget *t, int err)
{
 if (t->how != WRITE_MODE_BY_NAME)
  return(err);
 if (write_mode_name_changed(t) == WRITE_NAME_SAME)
  return(err);
 t->replaced = 1;
 return(ESTALE);
}

/** How a name's mode is to be made from the mode the file has now. */
typedef enum {
 WRITE_CHANGE_NINE,  /* the nine permission bits are these; the special bits stay */
 WRITE_CHANGE_ADD    /* these bits are added to the twelve */
} WriteModeChange;

/** write_mode_wanted - The twelve bits a change asks for.  Return: the mode. */
static mode_t write_mode_wanted(WriteModeChange how, mode_t current, mode_t bits)
{
 if (how == WRITE_CHANGE_NINE)
  return((current & WRITE_SPECIAL_BITS) | (bits & WRITE_NINE_BITS));
 return((current & WRITE_TWELVE_BITS) | bits);
}

/**
 * write_mode_change - Change the mode of a file the user named.
 * @path: the name; @shown: the file the user was shown, or NULL;
 * @seen: what the name was then, or NULL (not known);
 * @how and @bits: what to ask for; @k: what was not kept.
 * Return: 0, or the errno.
 *
 * The file is opened once; its special bits are read from that open file
 * and the new mode is given to it.  Another file that has taken the name
 * in between never gets the bits or the change: a link put there is not
 * followed, and the caller reads k->replaced.  k->unreached says no call
 * was made at all (the name leads nowhere).
 */
static int write_mode_change(const char *path, const struct stat *shown, const WpeNameSeen *seen,
                             WriteModeChange how, mode_t bits, WpeModeKept *k)
{
 WriteModeTarget t;
 struct stat now;
 int err, after;

 memset(k, 0, sizeof *k);
 err = write_mode_target_open(path, shown, seen, &t);
 k->replaced = t.replaced;
 k->replaced_kind = t.replaced_kind;
 if (err != 0)
 {
  k->unreached = !t.replaced;
  return(err);
 }
 err = write_mode_name_still_leads(&t, 0);
 if (err == 0)
  err = write_mode_read(&t, &now);
 if (err == 0)
  err = write_mode_apply(&t, write_mode_wanted(how, now.st_mode, bits), k);
 after = write_mode_name_still_leads(&t, err);
 if (err == 0 && after != 0)
  k->changed = 1;
 k->replaced = t.replaced;
 k->replaced_kind = t.replaced_kind;
 write_mode_target_close(&t);
 return(after);
}

/**
 * wpe_write_set_perm - Give a file the user named these permissions.
 * @path: the file or folder; @shown: the stat the dialog showed the user
 * (a link already followed), or NULL; @nine: the nine permission bits; @k:
 * receives what was not kept.
 * Return: 0, or the errno when even the nine bits were refused.
 *
 * For the Attributes dialog.  A file the user may write but not read, or a
 * folder with no read permission, still gets its permissions changed, and
 * its setuid, setgid and sticky bits are kept when the system allows it
 * and named when it does not.  The permissions are changed only on the file
 * the dialog showed, however long the dialog stayed open: a file that took
 * its name meanwhile is refused with k->replaced and nothing changed.
 */
int wpe_write_set_perm(const char *path, const struct stat *shown, mode_t nine, WpeModeKept *k)
{
 return(wpe_write_set_perm_seen(path, shown, NULL, nine, k));
}

/**
 * wpe_write_set_perm_seen - wpe_write_set_perm for a dialog that also knows
 * what the name was.
 * @seen: wpe_write_name_seen of the name when the dialog opened; NULL, or
 * not known, when there was no dialog.
 *
 * A name the dialog opened on a link shows the file the link leads to.  If
 * that file is replaced while the dialog is open the name is still the same
 * link, and the sentence says another file took it, not that a link did.
 * Not known is the safe default: a link at the name is then never claimed to
 * have taken it, because it may have been a link all along.
 */
int wpe_write_set_perm_seen(const char *path, const struct stat *shown, const WpeNameSeen *seen,
                            mode_t nine, WpeModeKept *k)
{
 return(write_mode_change(path, shown, seen, WRITE_CHANGE_NINE, nine, k));
}

/**
 * wpe_write_add_perm - Add permission bits to a file the user named.
 * @path: the file; @add: the bits; @k: receives what was not kept.
 * Return: 0, or the errno.
 *
 * For Ctrl-F9 on a script: the script becomes executable on disk with the
 * special bits it had, as a save of it keeps them, and never through a link
 * that took its name after the call began.  @own is the file the window read
 * (NULL, or not known, when it read none): when another file has since taken
 * the name, the script is not touched and k->replaced says so, exactly as a
 * Save refuses to write into it.
 */
int wpe_write_add_perm(const char *path, const WpeFileIdentity *own, mode_t add, WpeModeKept *k)
{
 struct stat shown;

 if (own == NULL || !own->known)
  return(write_mode_change(path, NULL, NULL, WRITE_CHANGE_ADD, add, k));
 memset(&shown, 0, sizeof shown);
 shown.st_dev = own->dev;
 shown.st_ino = own->ino;
 shown.st_mode = S_IFREG;
 return(write_mode_change(path, &shown, NULL, WRITE_CHANGE_ADD, add, k));
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
 * write_user_words - "user alice", or "user 1001" when the system has no name for it.
 * @out, @n: receive the words; @uid: the user.
 */
static void write_user_words(char *out, size_t n, uid_t uid)
{
 const struct passwd *pw = getpwuid(uid);

 if (pw != NULL && pw->pw_name[0] != '\0')
  snprintf(out, n, "user %s", pw->pw_name);
 else
  snprintf(out, n, "user %lu", (unsigned long)uid);
}

/**
 * write_group_words - "group devs", or "group 50" when the system has no name for it.
 * @out, @n: receive the words; @gid: the group.
 */
static void write_group_words(char *out, size_t n, gid_t gid)
{
 const struct group *gr = getgrgid(gid);

 if (gr != NULL && gr->gr_name[0] != '\0')
  snprintf(out, n, "group %s", gr->gr_name);
 else
  snprintf(out, n, "group %lu", (unsigned long)gid);
}

/**
 * write_owners_reason - Why a copy was not given a set-ID bit of its original.
 * @out, @n: receive the words; @o: who owned the copy and who the original.
 *
 * Tells the user who to ask: "the copy belongs to user root, the original
 * to user alice" when a program of another user would otherwise have become
 * the copier's, "the copy belongs to group users, the original to group
 * devs" when only the group differs.
 */
static void write_owners_reason(char *out, size_t n, const WpeModeOwners *o)
{
 char copy[WRITE_WHO_WORDS], src[WRITE_WHO_WORDS];

 if (o->other_owner)
 {
  write_user_words(copy, sizeof copy, o->uid_copy);
  write_user_words(src, sizeof src, o->uid_src);
 }
 else
 {
  write_group_words(copy, sizeof copy, o->gid_copy);
  write_group_words(src, sizeof src, o->gid_src);
 }
 snprintf(out, n, "the copy belongs to %s, the original to %s", copy, src);
}

/**
 * wpe_write_kept_merge - Add what one change of mode did not keep to another's.
 * @to: the record that goes on; @from: the one to add.
 *
 * A copy's set-ID bit dropped on purpose (another owner) and a bit the
 * system refused are two records of one copy; the sentence the user reads
 * names both bits and the first reason.
 */
void wpe_write_kept_merge(WpeModeKept *to, const WpeModeKept *from)
{
 if (from->lost == 0)
  return;
 if (to->lost == 0)
 {
  to->gid = from->gid;
  to->in_group = from->in_group;
 }
 to->lost |= from->lost;
 if (to->err == 0)
  to->err = from->err;
 to->changed |= from->changed;
 if (from->owners.dropped != 0)
  to->owners = from->owners;
}

/**
 * wpe_write_bits_reason - Why a special bit was not kept, in words.
 * @out, @n: receive the words; @k: what the mode owner reported.
 *
 * The same words on every system: a bit left off because the copy has
 * another owner or group than its original says whose they are, and only
 * for the bits it explains; a bit another way lost is explained after it.
 * A setgid bit the user could not have kept because the file's group is not
 * theirs says so (the BSDs refuse it with EPERM, Linux drops it in silence,
 * and both mean this); else the system's own reason; else that the system
 * cleared the bit.
 */
void wpe_write_bits_reason(char *out, size_t n, const WpeModeKept *k)
{
 WpeModeKept rest;
 char first[WPE_PATH_SIZE], more[WPE_PATH_SIZE];

 if (k->owners.dropped != 0)
 {
  rest = *k;
  memset(&rest.owners, 0, sizeof rest.owners);
  rest.lost &= ~k->owners.dropped;
  write_owners_reason(first, sizeof first, &k->owners);
  if (rest.lost == 0)
   snprintf(out, n, "%s", first);
  else
  {
   wpe_write_bits_reason(more, sizeof more, &rest);
   snprintf(out, n, "%s; the rest: %s", first, more);
  }
 }
 else if ((k->lost & S_ISGID) && !k->in_group)
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
 target.how = WRITE_MODE_BY_FD;
 target.fd = made;
 target.name = NULL;
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

/**
 * wpe_write_sync_before_bits - Make the text of a file reach the disk before
 * its setuid, setgid or sticky bit goes back on.
 * @fd: the file, whose text is complete; @mode: the mode about to be given;
 * @durable: the caller owes the text on disk even with no special bit (a
 * copy that is the only one).
 * Return: 0 (also when nothing is owed, which costs no call), or the errno
 * of the sync that failed.
 *
 * The kernel clears these bits on a write so that a half-written program is
 * not a program.  Putting them back while the text may still be in memory
 * would let a power cut leave a setuid or setgid file with the bits on and
 * the text lost.  A file with no special bit keeps the cost it always had.
 * One call covers both reasons: a file with a bit gets one fsync, which
 * also satisfies @durable.  A caller whose sync fails does not set the
 * bits, and says the write failed.
 */
int wpe_write_sync_before_bits(int fd, mode_t mode, int durable)
{
 if ((mode & WRITE_SPECIAL_BITS) != 0)
 {
  if (fsync(fd) != 0)
   return(errno);
  return(0);
 }
 if (durable && fdatasync(fd) != 0)
  return(errno);
 return(0);
}

/**
 * write_bits_mark_lost - Record that special bits could not be put back.
 * @k: receives the record; @bits: the bits; @err: the errno that stopped them.
 *
 * Shared by the save and the restore, so a bit lost on either way is named
 * in the same words (wpe_write_bits_reason): the errno is the system's own
 * reason, never a group, because no read-back was possible.
 */
static void write_bits_mark_lost(WpeModeKept *k, mode_t bits, int err)
{
 memset(k, 0, sizeof *k);
 k->lost = bits & WRITE_SPECIAL_BITS;
 k->err = err;
 k->in_group = 1;
}

/**
 * write_note_unsure - Record special bits a write may have cleared when the
 * file could not be read to find out.
 * @k: receives the record; @bits: the bits; @err: why the file could not be
 * read (no descriptor, or the descriptor would not say).
 *
 * The status row then says the bits "may not have been kept", not that they
 * were lost.
 */
static void write_note_unsure(WpeModeKept *k, mode_t bits, int err)
{
 write_bits_mark_lost(k, bits, err);
 k->unsure = 1;
}

/**
 * write_bits_read_back - Record which special bits a file does not have after
 * a call to put them on failed.
 * @fd: a descriptor of the file; @had: the bits it was to have; @err: the
 * errno of the call that failed; @k: receives the record.
 *
 * The one place a failed give-back is turned into words, for a save and for
 * a restore.  A bit that is on the file is not lost, whatever the call said.
 * A descriptor that will not say what the file has leaves the bits "may not
 * have been kept", the same words as a file that could not be opened at all
 * (a descriptor of -1 fails the same read): never silence, never a flat
 * "lost".
 */
static void write_bits_read_back(int fd, mode_t had, int err, WpeModeKept *k)
{
 struct stat now;
 mode_t special = had & WRITE_SPECIAL_BITS;

 memset(k, 0, sizeof *k);
 if (special == 0)
  return;
 if (fstat(fd, &now) != 0)
 {
  write_note_unsure(k, special, err);
  return;
 }
 if ((special & ~now.st_mode) != 0)
  write_bits_mark_lost(k, special & ~now.st_mode, err);
}

/* --- the backup copy ------------------------------------------------------ */

typedef enum {
 BACKUP_NONE,      /* nothing to back up: no such file, or not a regular file */
 BACKUP_MADE,      /* the .bak holds the file as it is, every byte on disk */
 BACKUP_SKIPPED,   /* it could not be made; the write goes on (backup_err) */
 BACKUP_REPLACED,  /* the name leads to another file than the one the write opened:
                      the save is refused before anything is written */
 BACKUP_STOPPED    /* it was begun and not finished: the save is refused */
} BackupResult;

/**
 * write_chosen_clear_old - Make room for a new file under a name the editor
 * chose (a .bak, an .ASV, an .ESV).
 * @name: the name.
 * Return: 0 when the name is free, EEXIST when something there stays (a
 * link, a pipe, a folder, another user's file), else the errno of the
 * unlink.
 *
 * An old copy that is a regular file of this user is replaced, as the
 * original did; anything else is left alone, so the new file is never made
 * through a link planted at the name and another user's file is never
 * removed by a copy of ours.
 */
static int write_chosen_clear_old(const char *name)
{
 struct stat st;

 if (lstat(name, &st) != 0)
  return(0);
 if (!S_ISREG(st.st_mode) || st.st_uid != geteuid())
  return(EEXIST);
 if (unlink(name) != 0)
  return(errno);
 return(0);
}

/**
 * write_backup_clear_old - Make room for a new .bak.
 * @bak: the .bak's name; @o: receives backup_err.
 * Return: 0 when the name is free, -1 when something there stays.
 */
static int write_backup_clear_old(const char *bak, WpeWriteOutcome *o)
{
 int err = write_chosen_clear_old(bak);

 if (err != 0)
 {
  o->backup_err = err;
  return(-1);
 }
 return(0);
}

/**
 * write_backup_source - Open the file to copy, and only if it is the file
 * the write opened.
 * @path: the file's name; @target: fstat of the descriptor the write opened;
 * @src: receives the source; @o: receives backup_err.
 * Return: 0 when @src is open on that very inode; BACKUP_REPLACED when the
 * name is gone or leads to another file; BACKUP_SKIPPED for any other
 * failure (backup_err says why).  Nothing is open unless 0.
 *
 * The name was looked up a moment after the write opened the file, and
 * another user of the folder may have put something else under it by then.
 * The copy that would follow would hold that other file's bytes, and a
 * restore would write them into the user's file.  And the text would go into
 * a file no longer at that name while the status said it was saved, so the
 * save is refused: nothing has been cut yet.  The open does not wait on a
 * pipe somebody put there, and never makes it the terminal.
 */
static int write_backup_source(const char *path, const struct stat *target, int *src,
                               WpeWriteOutcome *o)
{
 struct stat st;
 int result = 0;

 *src = open(path, O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
 if (*src < 0)
 {
  if (errno == ENOENT || errno == ENOTDIR)
   return(BACKUP_REPLACED);
  o->backup_err = errno;
  return(BACKUP_SKIPPED);
 }
 if (fstat(*src, &st) != 0)
 {
  o->backup_err = errno;
  result = BACKUP_SKIPPED;
 }
 else if (!S_ISREG(st.st_mode) || st.st_dev != target->st_dev || st.st_ino != target->st_ino)
  result = BACKUP_REPLACED;
 if (result != 0)
  close(*src);
 return(result);
}

/**
 * write_backup_open - Open the source and create the .bak, private.
 * @path: the file; @bak: the .bak's name; @target: fstat of the descriptor
 * the write opened; @src, @dst: receive the descriptors; @o: receives backup_err.
 * Return: BACKUP_REPLACED (the name leads to another file than the one being
 * written), BACKUP_SKIPPED (the source cannot be read, or the .bak cannot be
 * created) or BACKUP_MADE (both are open).
 *
 * The .bak is created with O_EXCL and O_NOFOLLOW, mode 0600, open for reading
 * and writing so the restore can read it through the same descriptor, and
 * takes the file's nine permission bits before it holds any of its text, so a
 * private file's backup is never readable by others, not even for a moment.
 * It is never given the setuid, setgid or sticky bit: a second copy of a
 * program with the old text must not be a program that runs with another
 * user's rights.
 */
static BackupResult write_backup_open(const char *path, const char *bak, const struct stat *target,
                                      int *src, int *dst, WpeWriteOutcome *o)
{
 WpeModeKept kept;
 int refused = write_backup_source(path, target, src, o);

 if (refused != 0)
  return((BackupResult)refused);
 if (write_backup_clear_old(bak, o) != 0)
 {
  close(*src);
  return(BACKUP_SKIPPED);
 }
 *dst = open(bak, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, WPE_WRITE_COPY_MODE);
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
 if (fchown(*dst, (uid_t)-1, target->st_gid) != 0)
  errno = 0;
 (void)wpe_write_set_mode_fd(*dst, target->st_mode & WRITE_NINE_BITS, &kept);
 return(BACKUP_MADE);
}

/**
 * write_backup - Copy the file to its .bak before the write truncates it.
 * @path: the file; @target: fstat of the descriptor the write opened, so the
 * copy is of that inode; @bak_fd: receives a descriptor of the .bak, open
 * for reading, or -1 unless the result is BACKUP_MADE; @o: receives
 * bak_path, backup_err and backup_stopped.
 * Return: the BackupResult.
 *
 * Shows no box and asks no question.  The copy counts as made only when
 * every write and the close returned 0: a short .bak put back over the
 * user's file would be the loss the .bak exists to prevent.  A .bak that
 * cannot be created lets the write go on (the status row says so); one
 * that was begun and stopped (a full disk, a size limit, no descriptor left
 * to keep it) refuses the save, because the same disk would refuse the new
 * text and nothing could be put back.  The descriptor is kept so a restore
 * reads the very file made here, never whatever the name leads to later; the
 * caller closes it.
 */
static BackupResult write_backup(const char *path, const struct stat *target, int *bak_fd,
                                 WpeWriteOutcome *o)
{
 char *bak = e_bakfilename((char *)path);
 int src = -1, dst = -1, kept = -1, err;
 BackupResult r;

 *bak_fd = -1;
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
 r = write_backup_open(path, bak, target, &src, &dst, o);
 if (r != BACKUP_MADE)
 {
  FREE(bak);
  return(r);
 }
 err = wpe_write_copy_fd(src, dst, NULL);
 if (err == 0)
 {
  kept = fcntl(dst, F_DUPFD_CLOEXEC, 0);
  if (kept < 0)
   err = errno;
 }
 close(src);
 if (close(dst) != 0 && err == 0)
  err = errno;
 if (err != 0)
 {
  if (kept >= 0)
   close(kept);
  unlink(bak);
  FREE(bak);
  o->err = err;
  o->backup_stopped = 1;
  return(BACKUP_STOPPED);
 }
 snprintf(o->bak_path, sizeof o->bak_path, "%s", bak);
 FREE(bak);
 *bak_fd = kept;
 return(BACKUP_MADE);
}

/* --- putting the old content back ----------------------------------------- */

/**
 * write_bits_found_off - Which special bits a file had and no longer has,
 * read from the file itself.
 * @fd: a descriptor of the file, or -1; @had: the bits it had (the nine and
 * the special ones, or 0 when it had no special bit).
 * Return: the special bits of @had that the file does not have now; 0 when
 * it had none, when it still has them all, or when it cannot be read.
 *
 * What a failed write, or a refused call for a bit, tells the user about a
 * program's bits.  It is read back, never inferred: a write that failed
 * before it touched the mode (a refused open, a cut that did not happen)
 * left the bit on, and saying otherwise would be false.
 */
static mode_t write_bits_found_off(int fd, mode_t had)
{
 struct stat now;
 mode_t special = had & WRITE_SPECIAL_BITS;

 if (fd < 0 || special == 0 || fstat(fd, &now) != 0)
  return(0);
 return(special & ~now.st_mode);
}

/**
 * write_back_give_bits - Put the special bits on a file that holds its old
 * text again, and record the ones that could not go.
 * @to: the file; @keep: the mode to give back (the nine bits and the special
 * ones), or 0; @k: receives the bits not put back.
 *
 * The text is synced first, as in a save.  A sync that fails, or a bit the
 * system refuses, leaves the old text on disk and the bit off: the restore
 * itself worked, and the failure box names the bit.
 */
static void write_back_give_bits(int to, mode_t keep, WpeModeKept *k)
{
 int err = wpe_write_sync_before_bits(to, keep, 0);

 if (err == 0)
  err = wpe_write_set_special_bits(to, keep, k);
 if (err == 0)
  return;
 write_bits_read_back(to, keep, err, k);
}

/**
 * write_back_through_fd - Write the .bak's bytes into the very file the
 * failed write went to.
 * @to: a second descriptor of that file, closed here; @from: the descriptor
 * of the .bak the backup made, closed here; @keep: the mode to give back with
 * the text when it has special bits (the nine bits and those), else 0;
 * @restore_kept: receives the special bits that could not be put back.
 * Return: 0, or the errno of what failed.
 *
 * The inode, the links, the owner and the group stay as they are, which a
 * rename could not promise; and no name is looked up again, neither the
 * file's nor the .bak's, so a link or a file put in either place since the
 * write cannot send other content into the user's file.  The failed write
 * cleared the file's setuid and setgid bits; they go back with the text, so
 * a program whose save failed is still the program it was.  A bit the system
 * will not give back is not a reason to fail the restore: the old text is on
 * disk again, and the user is told which bit is not.
 */
static int write_back_through_fd(int to, int from, mode_t keep, WpeModeKept *restore_kept)
{
 int err = 0;

 if (lseek(from, 0, SEEK_SET) < 0 || ftruncate(to, 0) != 0 || lseek(to, 0, SEEK_SET) < 0)
  err = errno;
 if (err == 0)
  err = wpe_write_copy_fd(from, to, NULL);
 if (err == 0)
  write_back_give_bits(to, keep, restore_kept);
 close(from);
 if (close(to) != 0 && err == 0)
  err = errno;
 return(err);
}

/**
 * write_bak_is_ours - Is the name still the .bak this write made?
 * @bak: the .bak's name; @bak_fd: the descriptor the backup kept; @named:
 * receives lstat of the name.
 * Return: 1 when the name and the descriptor are one regular file, owned by
 * this user, with one link; 0 for anything else, a failed lstat or fstat
 * included.
 *
 * Decides whether a rename may put the .bak over the user's file.  A "no" is
 * never a refusal to restore: the restore then writes the old text back
 * through the file, which needs no name.  Where the answer is unreliable
 * (a mount that reports another owner, a file system with no stable inode
 * numbers) the cost is only that the slower way is taken.
 */
static int write_bak_is_ours(const char *bak, int bak_fd, struct stat *named)
{
 struct stat held;

 if (lstat(bak, named) != 0 || fstat(bak_fd, &held) != 0)
  return(0);
 if (!S_ISREG(named->st_mode) || named->st_dev != held.st_dev || named->st_ino != held.st_ino)
  return(0);
 return(named->st_uid == geteuid() && named->st_nlink == 1);
}

/**
 * write_test_swap_bak - Let a second file take the .bak's name.
 * @bak: the .bak's name.
 *
 * Only for XWPE_TEST_FAIL=swap-bak: what a user who may write the folder
 * could do between the backup and the restore, so the test can show the
 * restore does not trust the name.  Nothing happens when no such file waits.
 */
static void write_test_swap_bak(const char *bak)
{
 char swap[WPE_PATH_SIZE];

 if (snprintf(swap, sizeof swap, "%s%s", bak, WPE_WRITE_TEST_SWAP_SUFFIX) < (int)sizeof swap)
  (void)rename(swap, bak);
}

/** write_close_both - Close two descriptors of a restore, either of which may be -1. */
static void write_close_both(int a, int b)
{
 if (a >= 0)
  close(a);
 if (b >= 0)
  close(b);
}

/**
 * write_restore_by_rename_ok - May the .bak replace the file by rename?
 * @path: the file as the user named it; @written: what fstat said of the
 * descriptor the write used.
 * @bak: the .bak's name; @bak_fd: the descriptor the backup kept; @keep: the
 * mode with special bits the restore gives back, or 0.
 * Return: 1 when the name is that very regular file, with one link, ours,
 * not writable by others, has no special bit to give back, and the .bak is
 * still the file the backup made (write_bak_is_ours) with the file's group
 * and mode.
 *
 * A symlinked file, a hard link, another user's file and a shared one would
 * each be broken by a rename: the link replaced, the other names left on the
 * short inode, the owner or the group changed (a group that could read the
 * file would lose it).  A file that had a special bit is not renamed over
 * either: the .bak never has the bits, and the failed write has already
 * cleared them on the file, so only a write back through the file can give
 * them back.
 */
static int write_restore_by_rename_ok(const char *path, const char *bak, int bak_fd,
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
 if (!write_bak_is_ours(bak, bak_fd, &copy))
  return(0);
 return(copy.st_gid == now.st_gid && (copy.st_mode & 07777) == (now.st_mode & 07777));
}

/**
 * write_restore - Put the file back as it was after a write failed half way.
 * @path: the file; @bak: the .bak made before the write; @bak_fd: the
 * descriptor the backup kept for it, closed here; @written: fstat of the
 * descriptor the write used, or NULL when it could not be taken;
 * @again: a second descriptor of the written file, closed here, or -1;
 * @keep: the mode with special bits to give back with the text, or 0 (the
 * .bak never has them, so a file with one is written back, not renamed over);
 * @restore_kept: receives the special bits a write-back could not put back.
 * Return: 0 when the file holds its old content again, else an errno.
 *
 * The .bak is renamed over the file when that changes nothing but the
 * content and the name still leads to the .bak this write made; otherwise
 * its bytes are written back into the file the write went to, from the
 * descriptor kept, with the special bits the file had.  Both descriptors are
 * closed on every way out.  XWPE_TEST_FAIL=restore makes this fail with EIO;
 * XWPE_TEST_FAIL=swap-bak puts another file under the .bak's name first.
 */
static int write_restore(const char *path, const char *bak, int bak_fd, const struct stat *written,
                         int again, mode_t keep, WpeModeKept *restore_kept)
{
 const char *hook = getenv(WPE_WRITE_TEST_FAIL_ENV);

 if (hook && strcmp(hook, WPE_WRITE_TEST_FAIL_RESTORE) == 0)
 {
  write_close_both(again, bak_fd);
  return(EIO);
 }
 if (hook && strcmp(hook, WPE_WRITE_TEST_FAIL_SWAP_BAK) == 0)
  write_test_swap_bak(bak);
 if (written && write_restore_by_rename_ok(path, bak, bak_fd, written, keep) && rename(bak, path) == 0)
 {
  write_close_both(again, bak_fd);
  return(0);
 }
 if (again < 0)
 {
  write_close_both(-1, bak_fd);
  return(EBADF);
 }
 return(write_back_through_fd(again, bak_fd, keep, restore_kept));
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

/** The part of a window's text a write takes: from (xa, ya) to (xe, ye). */
typedef struct {
 int xa, ya, xe, ye;
} WriteRange;

/** What a write puts into the file: a window's rows (@f and @r, @text NULL),
    or a text (@text; @f is then NULL, a text has no window).  The one shape
    both the editor's saves and the assistant's writes take, so a file is
    written one way whoever asked. */
typedef struct {
 FENSTER    *f;
 WriteRange  r;
 const char *text;
 size_t      len;     /* the bytes of @text; a before image may hold NUL bytes */
} WriteSource;

/** How the file's name is opened. */
typedef enum {
 WRITE_OPEN_NAMED,          /* the name the user typed: a link at its end is followed,
                               and one that leads nowhere is made */
 WRITE_OPEN_NAMED_NOFOLLOW, /* a name the caller has already resolved and judged: a
                               link at its end is refused (ELOOP and
                               replaced_by_link, on every system), never followed */
 WRITE_OPEN_NEW             /* a name the editor chose itself (the autosave, the emergency
                               copy, the post-edit copy): an old copy of ours is removed,
                               anything else there is refused (planted), and the file is
                               made new with O_EXCL and O_NOFOLLOW, never opened existing */
} WriteOpenHow;

/** What the write is for. */
typedef enum {
 WRITE_AIM_SAVE,     /* the window's own file: the identity it read must be the file
                        opened, when it is known */
 WRITE_AIM_REPLACE,  /* the file at the name is the file (the assistant's write with no
                        window open on it, a revert): its own special bits are kept */
 WRITE_AIM_COPY      /* another file: it never gets a special bit */
} WriteAimKind;

/** What a write does when its name leads to another file than the window read. */
typedef enum {
 WRITE_REPLACED_AS_COPY,   /* Save As, Block Write, the autosave: a second file, no question */
 WRITE_REPLACED_ASK,       /* a plain Save, first pass: stop, nothing touched, and say the
                              user is to be asked (o->ask_overwrite) */
 WRITE_REPLACED_ACCEPTED,  /* a plain Save, after the user said Yes: write it as a copy */
 WRITE_REPLACED_REFUSE     /* a write with no one to ask: refuse, nothing written */
} WriteReplaced;

/** The policy of one write for a name that leads to another file.  After a
    Yes, @asked is the file the user was asked about: only that one may be
    written, never another that took the name while the box was up. */
typedef struct {
 WriteReplaced   how;
 WpeFileIdentity asked;
} WriteReplacedPolicy;

/** What the write is for, and everything about the file it needs beyond the
    text: @own is the identity of the file the window read (NULL: none known),
    @replaced the policy for another file under the name, @open how the name is
    opened, and, for a revert, @recorded the twelve bits the text is to wear
    again (@has_recorded; the nine go on before the text, the special ones
    after it is on disk). */
typedef struct {
 WriteAimKind          kind;
 const WpeFileIdentity *own;
 WriteReplacedPolicy   replaced;
 WriteOpenHow          open;
 int                   has_recorded;
 mode_t                recorded;
 const WpeWriteExpect *expect;   /* what the caller saw at the name, or NULL: the file
                                    opened must be that file (write_target_expected) */
} WriteAim;

/**
 * write_text_put - Put a text into an open stream.
 * @text: the bytes; @len: how many; @fp: the stream.
 * Return: 1 when it did not all go (errno says why, or is 0), else 0.
 *
 * The length is the caller's, so a before image that holds NUL bytes goes
 * back whole.
 */
static int write_text_put(const char *text, size_t len, FILE *fp)
{
 return(len > 0 && fwrite(text, 1, len, fp) != len);
}

/**
 * write_source_fill - Put the source into an open stream, whichever it is.
 * @s: the source; @fp: the stream.
 * Return: 1 when the write failed, else 0.
 *
 * A window's rows go through the line-ending owner, in the window's own line
 * style; a text goes as it is.
 */
static int write_source_fill(const WriteSource *s, FILE *fp)
{
 FENSTER *f = s->f;
 BUFFER *b;
 int xa = s->r.xa, ya = s->r.ya, xe = s->r.xe, ye = s->r.ye;

 if (s->text != NULL)
  return(write_text_put(s->text, s->len, fp));
 b = f->b;
 return(e_eol_write(fp, b, ya, xa, ye, xe, write_keeps_file_ending(f, b, xa, ya, xe, ye)) != 0);
}

/**
 * write_source_put - Put the source into an open file, judged, and close it.
 * @s: the source; @fp: the file; @written: receives fstat of the file when it
 * could be taken; @have_written: receives whether it was.
 * Return: 0, or the errno of the first thing that failed (the bytes, the
 * flush, the stream's error flag, the close).
 */
static int write_source_put(const WriteSource *s, FILE *fp, struct stat *written, int *have_written)
{
 int failed, err;

 errno = 0;
 failed = write_source_fill(s, fp);
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
 * @o: the outcome; @path: the file; @bk: what the backup did; @bak_fd: the
 * descriptor it kept (closed here), or -1; @written: what the write's
 * descriptor showed; @again: a second descriptor of the file (closed here), or
 * -1; @keep: the mode with special bits the restore gives back, or 0.
 *
 * With a .bak made in this call the file is put back; when that fails too
 * the outcome says so and names the .bak that holds the old content.  With
 * no .bak the file was opened and truncated with nothing to put back, so
 * the outcome says the file may be cut short (restore_err ENOENT, no
 * bak_path).
 */
static void write_after_failure(WpeWriteOutcome *o, const char *path, BackupResult bk, int bak_fd,
                                const struct stat *written, int have_written, int again,
                                mode_t keep)
{
 if (bk != BACKUP_MADE)
 {
  o->file_off = write_bits_found_off(again, keep);
  write_close_both(again, bak_fd);
  o->restore_err = ENOENT;
  return;
 }
 o->restore_err = write_restore(path, o->bak_path, bak_fd, have_written ? written : NULL, again,
                                keep, &o->restore_kept);
 if (o->restore_err != 0)
  memset(&o->restore_kept, 0, sizeof o->restore_kept);
}

/* --- the file a write goes to --------------------------------------------- */

/** What a write is for.  A Save overwrites the user's file and gives it back
    the special bits it had; a copy (Block Write, the autosave, the emergency
    save, the print copy, Save As onto another file) is a second file and
    never has them.  REPLACED is a regular file under the window's name
    that is not the one the window read: the write must ask or refuse before
    it touches it. */
typedef enum { WRITE_PURPOSE_SAVE, WRITE_PURPOSE_COPY, WRITE_PURPOSE_REPLACED } WritePurpose;

/**
 * write_purpose_for_target - Is the file this write opened the window's own?
 * @own: the file the window read (FENSTER.file_id), or NULL.
 * @opened: fstat of the file the write opened.
 * @created: this write made the file, so nothing was there to be replaced.
 * Return: Save when it is the same regular file (same device and inode),
 * Replaced when the window knows its file and this is another regular file
 * that was already there, else copy.
 *
 * The one place that decides it, by identity and never by name: the same
 * folder and name, a hard link to it and a path through a symbolic link all
 * reach one inode and one program, which keeps its bits; the same name in
 * another folder is another file, which gets none.  Callers pass what they
 * know and never the purpose; no knowledge means a copy, which fails safe.
 * A file this write created is never a replacement: the one the window read
 * is gone, and the save makes it again.  Whatever else holds the name, of
 * any type (a link to /dev/null, a device), is a replacement too, so the
 * window's text is never poured into it unasked.  A FIFO under the name
 * still blocks in open(O_WRONLY) before this is reached, as it always has.
 */
static WritePurpose write_purpose_for_target(const WpeFileIdentity *own, const struct stat *opened,
                                             int created)
{
 if (own == NULL || !own->known)
  return(WRITE_PURPOSE_COPY);
 if (S_ISREG(opened->st_mode) && own->dev == opened->st_dev && own->ino == opened->st_ino)
  return(WRITE_PURPOSE_SAVE);
 return(created ? WRITE_PURPOSE_COPY : WRITE_PURPOSE_REPLACED);
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
 WritePurpose purpose; /* what write_purpose_for_target said of this file */
 int         created; /* the name did not exist: this open made the file, so there is
                         nothing to back up */
 int         nine_err; /* errno of the call that gave the file its nine bits before the
                         text (write_mode_before_data), 0 when it worked or none was
                         made: what a copy says when it could not drop a special bit */
} WriteTarget;

/** What the backup left a write holding: what it did, and the descriptor of the
    .bak when it was made (else -1).  Whoever ends the write closes it. */
typedef struct {
 BackupResult result;
 int          fd;
} WriteBackup;

/** write_backup_release - Close the .bak's descriptor once nothing can need it. */
static void write_backup_release(WriteBackup *b)
{
 if (b->fd >= 0)
  close(b->fd);
 b->fd = -1;
}

/**
 * write_backup_refuses_save - Did the backup leave a reason not to write?
 * @bk: what the backup did; @o: receives err and replaced.
 * Return: 1 when the save is refused (the backup stopped, or the name leads
 * to another file), 0 when the write goes on.
 *
 * Nothing is cut when this says 1, so the file is as the user had it and the
 * window keeps the text.
 */
static int write_backup_refuses_save(const WriteBackup *bk, WpeWriteOutcome *o)
{
 if (bk->result == BACKUP_REPLACED)
 {
  o->replaced = 1;
  o->err = WPE_WRITE_REPLACED_ERR;
  return(1);
 }
 return(bk->result == BACKUP_STOPPED);
}

/**
 * write_target_open_fd - Open the file for writing, and say whether this made it.
 * @path: the file; @created: receives 1 when the name did not exist.
 * Return: the descriptor, or -1 with errno.
 *
 * Looking for the file first, and making it only when it is not there, is
 * what tells a file the user had from one that is new: a new file has no old
 * text to back up, and the save must not leave a .bak of nothing.  The file
 * is made with O_EXCL, so one that appeared in between is opened as the
 * user's file and backed up, not taken for ours.  A name that leads nowhere
 * (a link to a file to be made) cannot be made with O_EXCL and is made
 * without, as before.
 *
 * @how says what a link at the end of the name means.  A name the user typed
 * is followed (NAMED).  A name the caller already resolved and judged
 * (NAMED_NOFOLLOW) is opened with O_NOFOLLOW both times and never takes the
 * dangling-link way: a link found there was put there since, the open fails
 * with the system's own errno, which write_open_failure turns into the one
 * the owner reports for a link, and nothing is created through it.  Such an
 * open also never waits: a pipe with no reader would otherwise hold the
 * editor in open() before the file's type could be looked at, so it is opened
 * O_NONBLOCK, write_target_hold looks at the type from the descriptor, and
 * the flag is cleared on a regular file before any byte is written.
 *
 * @may_create is 0 when the caller looked at a file there: a file removed
 * since is not made again empty, the open fails and the owner says it was
 * removed.
 */
static int write_target_open_fd(const char *path, WriteOpenHow how, int may_create, int *created)
{
 int follow = (how == WRITE_OPEN_NAMED_NOFOLLOW ? (O_NOFOLLOW | O_NONBLOCK) : 0);
 int flags = O_NOCTTY | O_CLOEXEC | follow;
 int fd = open(path, O_WRONLY | flags);

 *created = 0;
 if (fd >= 0 || errno != ENOENT || !may_create)
  return(fd);
 fd = open(path, O_WRONLY | O_CREAT | O_EXCL | flags, WRITE_NEW_FILE_MODE);
 if (fd >= 0)
 {
  *created = 1;
  return(fd);
 }
 if (errno != EEXIST)
  return(-1);
 fd = open(path, O_WRONLY | flags);
 if (fd >= 0 || errno != ENOENT)
  return(fd);
 if (how == WRITE_OPEN_NAMED_NOFOLLOW)
  return(-1);
 *created = 1;
 return(open(path, O_WRONLY | O_CREAT | O_NOCTTY | O_CLOEXEC, WRITE_NEW_FILE_MODE));
}

/**
 * write_aim_purpose - What a write is for, from what it aimed at and what it opened.
 * @aim: what the caller wants; @opened: fstat of the file opened; @created:
 * this write made the file.
 * Return: the purpose.
 *
 * A copy is a copy and a replace is a save of the file at the name, but only
 * a regular file: anything else already there (a device, a pty) is a
 * replacement the write must refuse, as a window's own Save refuses it.  A
 * save of a window's file is decided by the identity the window read
 * (write_purpose_for_target).
 */
static WritePurpose write_aim_purpose(const WriteAim *aim, const struct stat *opened, int created)
{
 if (aim->kind == WRITE_AIM_COPY)
  return(WRITE_PURPOSE_COPY);
 if (aim->kind == WRITE_AIM_REPLACE && !created && !S_ISREG(opened->st_mode))
  return(WRITE_PURPOSE_REPLACED);
 if (aim->kind == WRITE_AIM_REPLACE)
  return(WRITE_PURPOSE_SAVE);
 return(write_purpose_for_target(aim->own, opened, created));
}

/**
 * write_aim_keep - The special bits a write gives back after the text.
 * @aim: what the caller wants; @t: the target, opened.
 * Return: the bits a revert recorded when it has some, else those the file
 * had for a save, else none (a copy never gets one).
 */
static mode_t write_aim_keep(const WriteAim *aim, const WriteTarget *t)
{
 if (aim->has_recorded)
  return(aim->recorded & WRITE_SPECIAL_BITS);
 if (t->purpose == WRITE_PURPOSE_SAVE)
  return(t->before & WRITE_SPECIAL_BITS);
 return(0);
}

/**
 * write_clear_nonblock - Make an opened file wait for the disk again.
 * @fd: the descriptor.
 * Return: 0, or the errno of what failed.
 */
static int write_clear_nonblock(int fd)
{
 int flags = fcntl(fd, F_GETFL);

 if (flags < 0)
  return(errno);
 if (fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) != 0)
  return(errno);
 return(0);
}

/**
 * write_note_not_regular - Record that the file opened is not a regular file.
 * @o: receives not_regular, its kind and whether it is a terminal; @t: the
 * target, opened and fstat'ed.
 *
 * What the user reads is "a pipe", "a socket", "a device" or "a terminal",
 * from the file's own mode and from isatty on the descriptor, never from an
 * errno.
 */
static void write_note_not_regular(WpeWriteOutcome *o, const WriteTarget *t)
{
 o->not_regular = 1;
 o->not_regular_kind = t->st.st_mode & S_IFMT;
 o->not_regular_tty = (o->not_regular_kind == S_IFCHR && isatty(t->fd));
}

/**
 * write_target_expected - Is the file opened the one the caller looked at?
 * @t: the target, opened and not cut; @expect: what the caller saw at the
 * name, or NULL; @o: receives swapped or gained_name.
 * Return: 0 when it is, else the errno the refusal carries.
 *
 * Decided on the opened descriptor, never on the name: the file must be the
 * one with the device and inode the caller saw, with no more names than it
 * had, and a name that was free must have been made by this very open.  A
 * file swapped in under the name while the user was being asked, or a hard
 * link made to it, is refused with nothing written; the sentence says so.
 */
static int write_target_expected(const WriteTarget *t, const WpeWriteExpect *expect,
                                 WpeWriteOutcome *o)
{
 if (expect == NULL || !expect->judged)
  return(0);
 if (!expect->exists && t->created)
  return(0);
 if (!expect->exists || t->created || t->st.st_dev != expect->dev || t->st.st_ino != expect->ino)
 {
  o->swapped = 1;
  return(WPE_WRITE_REPLACED_ERR);
 }
 if (t->st.st_nlink > expect->nlink)
 {
  o->gained_name = 1;
  return(WPE_WRITE_REPLACED_ERR);
 }
 return(0);
}

/**
 * write_target_hold - Judge the file just opened, before anything touches it.
 * @t: the target, opened; @aim: what the write is for; @o: receives the
 * refusal.
 * Return: 0 when the write may go on, else the errno the refusal carries.
 *
 * A name the caller resolved and judged (NOFOLLOW) is written only when it is
 * a regular file, decided from the file's own mode: a pipe, a socket, a
 * device or a terminal is refused and named by its kind, never written to.
 * Such a file was opened without waiting; a regular file waits for the disk
 * again from here on.  Then the file must be the one the caller looked at.
 */
static int write_target_hold(const WriteTarget *t, const WriteAim *aim, WpeWriteOutcome *o)
{
 int err;

 if (aim->open == WRITE_OPEN_NAMED_NOFOLLOW)
 {
  if (!S_ISREG(t->st.st_mode))
  {
   write_note_not_regular(o, t);
   return(WPE_WRITE_REPLACED_ERR);
  }
  err = write_clear_nonblock(t->fd);
  if (err != 0)
   return(err);
 }
 return(write_target_expected(t, aim->expect, o));
}

/**
 * write_target_make_new - Make the file under a name the editor chose.
 * @path: the name; @created: receives 1 when it was made; @o: receives
 * planted when something that is not ours holds the name.
 * Return: the descriptor, or -1 with errno.
 *
 * The file is private (0600) when it is made; the window's own nine bits
 * are given to it before its first byte (write_mode_before_data), so the
 * copy of a private file is never readable by others, not even for a moment.
 * What the user gets: the autosave, the emergency copy and the hook's copy
 * never write through a link or into a file somebody else put at their name.
 * An old copy of ours is removed first (write_chosen_clear_old), anything
 * else there is left alone and said, and the new file is made with O_EXCL
 * and O_NOFOLLOW (wpe_write_create_private), so the inode written is one
 * this call created, whatever is put at the name in between.
 */
static int write_target_make_new(const char *path, int *created, WpeWriteOutcome *o)
{
 int fd = -1, err = write_chosen_clear_old(path);

 *created = 0;
 if (err == 0)
  err = wpe_write_create_private(AT_FDCWD, path, WPE_WRITE_COPY_MODE, &fd);
 if (err == EEXIST)
  o->planted = 1;
 if (err != 0)
 {
  errno = err;
  return(-1);
 }
 *created = 1;
 return(fd);
}

/**
 * write_target_open - Open the file a write goes to, without cutting it.
 * @path: the file; @aim: what the write is for; @t: receives the target;
 * @o: receives the reason the file was refused, when it is.
 * Return: 0, or the errno of what failed (nothing was truncated).
 *
 * The write clears a file's setuid and setgid bits in the open itself when
 * it truncates, so the bits of the file being saved can only be read before
 * that.  Reading them from this descriptor, and not from a name looked up
 * before or a mode recorded when the window loaded the file, means a bit the
 * user took off since stays off and a file that is new has none to get.
 */
static int write_target_open(const char *path, const WriteAim *aim, WriteTarget *t,
                             WpeWriteOutcome *o)
{
 int err;

 memset(t, 0, sizeof *t);
 if (aim->open == WRITE_OPEN_NEW)
  t->fd = write_target_make_new(path, &t->created, o);
 else
  t->fd = write_target_open_fd(path, aim->open, !(aim->expect != NULL && aim->expect->judged &&
                                                     aim->expect->exists), &t->created);
 if (t->fd < 0)
  return(errno);
 err = (fstat(t->fd, &t->st) == 0 ? 0 : errno);
 if (err == 0)
  err = write_target_hold(t, aim, o);
 if (err == 0 && (t->fp = fdopen(t->fd, "w")) == NULL)
  err = errno;
 if (err != 0)
 {
  close(t->fd);
  return(err);
 }
 if (S_ISREG(t->st.st_mode))
  t->before = t->st.st_mode & WRITE_TWELVE_BITS;
 t->purpose = write_aim_purpose(aim, &t->st, t->created);
 t->keep = write_aim_keep(aim, t);
 return(0);
}

/**
 * write_open_failure - The one errno a failed open of the file is reported with.
 * @path: the file; @aim: what the write is for; @err: the errno of the open;
 * @o: receives replaced_by_link.
 * Return: ELOOP when the open was one that never follows a link and a link
 * holds the name now (the one value the owner reports for it: set here,
 * never read), the refusal's own value when the file was opened and refused
 * (write_target_hold), else @err (EIO when it left none).
 *
 * The name is looked at again, never the errno: FreeBSD says EMLINK for a
 * link refused by O_NOFOLLOW, NetBSD EFTYPE, Linux ELOOP, and a pipe with no
 * reader or a socket is ENXIO here and something else elsewhere.  The caller
 * reads o->replaced_by_link and o->not_regular, the user sees the same
 * sentence on every system.
 */
static int write_open_failure(const char *path, const WriteAim *aim, int err, WpeWriteOutcome *o)
{
 mode_t kind;

 if (o->not_regular || o->swapped || o->gained_name || o->planted)
  return(err);
 kind = write_name_kind(path);
 if (kind == 0 && aim->expect != NULL && aim->expect->judged && aim->expect->exists)
 {
  o->vanished = 1;
  return(WPE_WRITE_REPLACED_ERR);
 }
 if (aim->open != WRITE_OPEN_NAMED_NOFOLLOW)
  return(write_error_number(err));
 if (kind == S_IFLNK)
 {
  o->replaced_by_link = 1;
  return(ELOOP);
 }
 if (kind != 0 && kind != S_IFREG && kind != S_IFDIR)
 {
  o->not_regular = 1;
  o->not_regular_kind = kind;
  return(WPE_WRITE_REPLACED_ERR);
 }
 return(write_error_number(err));
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
 *
 * POSIX makes open with O_TRUNC always mark the file as changed, but
 * ftruncate only when the size changes, and some systems (OpenBSD) keep to
 * that.  A file that was already empty is therefore marked explicitly, so a
 * save moves the file's time as the original's did and the build's
 * up-to-date checks see the save.  A time that cannot be set is not a
 * failed save.
 */
static int write_target_truncate(const WriteTarget *t)
{
 if (!S_ISREG(t->st.st_mode))
  return(0);
 if (ftruncate(t->fd, 0) != 0)
  return(errno);
 if (t->st.st_size == 0)
  (void)futimens(t->fd, NULL);
 return(0);
}

/**
 * write_target_has_special_bits - Did the file have a setuid, setgid or
 * sticky bit when it was opened?
 * @t: the target.
 * Return: 1 when it did.
 */
static int write_target_has_special_bits(const WriteTarget *t)
{
 return((t->before & WRITE_SPECIAL_BITS) != 0);
}

/**
 * write_mode_before_data - Give the file its nine permission bits before it
 * holds any text.
 * @t: the target; @s: the source; @aim: what the write is for.
 *
 * With a window these are the nine bits the window recorded when it loaded
 * the file (the only place that mode is consulted, and only for the nine
 * bits), so an autosave of a private file is private from its first byte.  A
 * revert gives the nine bits it recorded.  A text with no window and no
 * recorded mode changes nothing, except on a file that has a special bit
 * right now: it is given its own nine bits at once, which takes the bit off,
 * so no byte of the text goes into a file that is a program (for root the
 * system would not take it off by itself).  Special bits never come from
 * here: the save puts them back after the text is on disk.  A mode that did
 * not change is not a lost save: the text is written.  The errno of the
 * call is kept in t->nine_err, so a copy that could not drop a special bit
 * can say why.
 */
static void write_mode_before_data(WriteTarget *t, const WriteSource *s, const WriteAim *aim)
{
 WpeModeKept kept;

 if (aim->has_recorded)
  t->nine_err = wpe_write_set_mode_fd(t->fd, aim->recorded & WRITE_NINE_BITS, &kept);
 else if (s->f != NULL && s->f->filemode >= 0)
  t->nine_err = wpe_write_set_mode_fd(t->fd, (mode_t)s->f->filemode & WRITE_NINE_BITS, &kept);
 else if (s->f == NULL && write_target_has_special_bits(t))
  t->nine_err = wpe_write_set_mode_fd(t->fd, t->before & WRITE_NINE_BITS, &kept);
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
 * write_mode_after_text - The mode the file has now, read from the file.
 * @fd: a descriptor of it, or -1; @written: what fstat showed right after the
 * text, when @have_written; @mode: receives the mode.
 * Return: 1 when the mode could be read, else 0.
 *
 * The one read-back of what a write left on the file: the descriptor when
 * there is one, else the fstat taken after the text.  A report about special
 * bits is made from this and from nothing else.
 */
static int write_mode_after_text(int fd, const struct stat *written, int have_written, mode_t *mode)
{
 struct stat now;

 if (fd >= 0 && fstat(fd, &now) == 0)
 {
  *mode = now.st_mode;
  return(1);
 }
 if (!have_written)
  return(0);
 *mode = written->st_mode;
 return(1);
}

/**
 * write_keep_special_bits - Put the saved file's setuid, setgid and sticky
 * bits back after the text, and record the ones it could not keep.
 * @fd: a descriptor of the saved file, or -1 when none could be made;
 * @fd_err: why there is none; @t: the target, whose @keep are the bits to
 * give back; @written, @have_written: the file as fstat showed it after the
 * text; @k: receives the bits not kept.
 *
 * The write clears these bits, so a setgid program saved by its owner would
 * stop being setgid without a word.  Called after the last byte, on the
 * descriptor of the file written, never on its name: a link put in the
 * name's place cannot be given the bits.  A bit the system refuses, drops
 * or cannot be asked for is recorded for the status row; the text is on
 * disk either way, so this never fails the save.  Whatever failed, the file
 * is read back before anything is said (the descriptor, or without one the
 * fstat taken after the text): a bit that is on the file is not lost, so the
 * user is never told a program stopped being one when it did not.  With
 * nothing to read, the bits are said to be possibly not kept.
 */
static void write_keep_special_bits(int fd, int fd_err, const WriteTarget *t,
                                    const struct stat *written, int have_written, WpeModeKept *k)
{
 int err;
 mode_t mode;

 memset(k, 0, sizeof *k);
 if (t->keep == 0)
  return;
 if (fd < 0)
 {
  err = (fd_err != 0 ? fd_err : EBADF);
  if (!write_mode_after_text(-1, written, have_written, &mode))
   write_note_unsure(k, t->keep, err);
  else if ((t->keep & ~mode) != 0)
   write_bits_mark_lost(k, t->keep & ~mode, err);
  return;
 }
 err = write_give_back_bits(fd, t->keep, k);
 if (err == 0)
  return;
 write_bits_read_back(fd, t->keep, err, k);
}

/**
 * write_read_back_stays - Record the special bits a copy was to drop and
 * still has.
 * @fd: a descriptor of the file written, or -1; @t: the target;
 * @written, @have_written: the file as fstat showed it after the text; @k:
 * receives the bits that stayed and the errno of the call that was to drop
 * them.
 *
 * A copy is never a program: the nine bits it is given before the text take
 * the setuid, setgid and sticky bits of the file it overwrote off.  When that
 * call is refused (a file the user does not own) a bit can stay, and the
 * user is told so.  Said only of bits read from the file after the text.
 */
static void write_read_back_stays(int fd, const WriteTarget *t, const struct stat *written,
                                  int have_written, WpeModeKept *k)
{
 mode_t mode;

 memset(k, 0, sizeof *k);
 if (t->keep != 0 || (t->before & WRITE_SPECIAL_BITS) == 0)
  return;
 if (!write_mode_after_text(fd, written, have_written, &mode))
  return;
 k->lost = (t->before & WRITE_SPECIAL_BITS) & mode;
 if (k->lost == 0)
  return;
 k->err = t->nine_err;
 k->in_group = 1;
}

/** XWPE_TEST_FAIL=write-other-owner: the replaced file looks owned by another
    user, the case a test run by one user cannot make with a real file. */
#define WPE_WRITE_TEST_OTHER_OWNER "write-other-owner"

/**
 * write_replaced_is_ours - May the editor ask about overwriting this file?
 * @st: fstat of the file that took the window's name.
 * Return: 1 for a regular file with no setuid, setgid or sticky bit that
 * belongs to the user running the editor, else 0.
 *
 * A file another user owns, one that is a program, and anything that is not
 * a regular file (a device, a link to /dev/null) is never overwritten on a
 * yes typed at a box: the user may not know what it is.
 */
static int write_replaced_is_ours(const struct stat *st)
{
 const char *hook = getenv(WPE_WRITE_TEST_FAIL_ENV);
 uid_t owner = st->st_uid;

 if (!S_ISREG(st->st_mode))
  return(0);
 if (hook != NULL && strstr(hook, WPE_WRITE_TEST_OTHER_OWNER) != NULL)
  owner = (uid_t)(owner + 1);
 if (st->st_mode & WRITE_SPECIAL_BITS)
  return(0);
 return(owner == geteuid());
}

/** XWPE_TEST_FAIL=write-replacement-check-no: wpe_write_replacement_may_be_asked
    says no for a file the open then finds askable, the disagreement a file
    changing between the two looks would make. */
#define WPE_WRITE_TEST_REPLACEMENT_NO "write-replacement-check-no"

/** The policy of every write that never asks: a copy is a second file. */
static const WriteReplacedPolicy write_replaced_as_copy = { WRITE_REPLACED_AS_COPY, { 0, 0, 0 } };
/** The policy of a plain Save's first pass: stop and report the file to ask about. */
static const WriteReplacedPolicy write_replaced_ask = { WRITE_REPLACED_ASK, { 0, 0, 0 } };
/** The policy of a write with no one to ask: another file under the name is
    refused, nothing written. */
static const WriteReplacedPolicy write_replaced_refuse = { WRITE_REPLACED_REFUSE, { 0, 0, 0 } };

/**
 * write_replaced_may_go_on - Decide a write whose name leads to another file
 * than the one the window read.
 * @t: the target, open and not cut; @policy: what the caller does about it;
 * @o: receives the refusal or the request to ask (and, with it, the file
 * asked about).
 * Return: 1 when the write goes on (as a copy: no special bit, and the window
 * takes the new file as its own), 0 when it stops (@o says why).
 *
 * After a Yes only the file the user was asked about is written.  Another
 * file that took the name while the box was up is refused, with Save As as
 * the way out, not asked about again: the user never answers a question
 * about one file with a write into another; the outcome says the name
 * changed while the user was being asked.  A write that cannot ask anybody
 * (the REFUSE policy) refuses at once, nothing written.
 *
 * An editor that saves by renaming (vim, sed -i, git checkout, rsync) gives
 * the name a new file under the window's feet, and the user then sees one
 * question instead of a refusal.  A replacement that is a program, belongs
 * to another user or is not a regular file is refused with Save As as the
 * way out, because the box cannot tell the user what it is.  The question
 * itself is asked by the caller, outside the write, so no signal stays
 * ignored and no range is computed while the box is up.
 */
static int write_replaced_may_go_on(WriteTarget *t, const WriteReplacedPolicy *policy,
                                    WpeWriteOutcome *o)
{
 int not_asked = (policy->how == WRITE_REPLACED_ACCEPTED &&
                  (!policy->asked.known || policy->asked.dev != t->st.st_dev ||
                   policy->asked.ino != t->st.st_ino));

 if (policy->how == WRITE_REPLACED_REFUSE && !S_ISREG(t->st.st_mode))
 {
  write_note_not_regular(o, t);
  o->err = WPE_WRITE_REPLACED_ERR;
  return(0);
 }
 if (not_asked || policy->how == WRITE_REPLACED_REFUSE || !write_replaced_is_ours(&t->st))
 {
  o->other_file = 1;
  o->changed_while_asked = not_asked;
  o->err = WPE_WRITE_REPLACED_ERR;
  return(0);
 }
 if (policy->how == WRITE_REPLACED_ASK)
 {
  wpe_write_identity_take(&o->asked, &t->st);
  o->ask_overwrite = 1;
  return(0);
 }
 t->purpose = WRITE_PURPOSE_COPY;
 return(1);
}

/** XWPE_TEST_FAIL=emergency-save-hangs: TEST ONLY.  The emergency save blocks
    after its copy is made and before its first byte, as a save on a dead
    disk would, so a test can prove that a second SIGTERM ends the editor.
    Without the variable it does nothing. */
#define WPE_WRITE_TEST_EMERGENCY_HANGS "emergency-save-hangs"

/**
 * write_test_emergency_hang - Test hook: block the emergency save here.
 * @aim: what the write is for.
 *
 * Only the copy made while the editor is ending is held, never an autosave
 * or a Save.  A signal that ends the editor still ends it.
 */
static void write_test_emergency_hang(const WriteAim *aim)
{
 const char *hook = getenv(WPE_WRITE_TEST_FAIL_ENV);

 if (aim->open != WRITE_OPEN_NEW || hook == NULL ||
     strstr(hook, WPE_WRITE_TEST_EMERGENCY_HANGS) == NULL || !e_emergency_save_running())
  return;
 for (;;)
  pause();
}

/**
 * write_run_open - Open the file a write goes to and make it ready for text.
 * @s: the source; @path: the file; @backup: 1 to copy an existing file to its
 * .bak; @aim: what the write is for, which file it is allowed to be and what
 * to do when the name leads to another; @t: receives the target; @bk:
 * receives what the backup did and holds the .bak's descriptor; @o: receives
 * err when it fails.
 * Return: 0 when the file is open, cut to nothing and has its nine
 * permission bits; -1 when not (nothing was written, the old file is as it
 * was, and nothing stays open).
 *
 * The order is the point.  The file is opened first, its bits are read from
 * that descriptor, and only a regular file that was already there is backed
 * up, from the same inode; so a file that cannot be opened leaves no new
 * .bak, a pipe is never opened by the backup, and the .bak is never of a
 * file the save will not overwrite.  The cut comes last, when the old text is
 * safe.
 */
static int write_run_open(const WriteSource *s, const char *path, int backup, const WriteAim *aim,
                          WriteTarget *t, WriteBackup *bk, WpeWriteOutcome *o)
{
 int err = write_target_open(path, aim, t, o);

 bk->result = BACKUP_NONE;
 bk->fd = -1;
 if (err != 0)
 {
  o->err = write_open_failure(path, aim, err, o);
  return(-1);
 }
 if (t->purpose == WRITE_PURPOSE_REPLACED && aim->replaced.how != WRITE_REPLACED_AS_COPY &&
     !write_replaced_may_go_on(t, &aim->replaced, o))
 {
  fclose(t->fp);
  return(-1);
 }
 if (backup && !t->created && S_ISREG(t->st.st_mode))
  bk->result = write_backup(path, &t->st, &bk->fd, o);
 if (write_backup_refuses_save(bk, o))
 {
  fclose(t->fp);
  return(-1);
 }
 err = write_target_truncate(t);
 if (err != 0)
 {
  o->err = write_error_number(err);
  write_backup_release(bk);
  fclose(t->fp);
  return(-1);
 }
 write_mode_before_data(t, s, aim);
 write_test_emergency_hang(aim);
 return(0);
}

/**
 * write_run_finish - Put the source into the open target, then give a Save's
 * file its special bits back, or put the old content back when it failed.
 * @s: the source; @path: the file; @t: the open target, closed here; @bk:
 * what the backup did and the .bak's descriptor, closed here; @o: receives
 * the outcome.
 *
 * The restore, and the special bits after the text, go through a second
 * descriptor of this same file, never through whatever the name points at by
 * then.  A file that is to get a special bit back is synced first; a sync
 * that fails is a failed save, like a write that fails: the window stays
 * modified and the old content goes back, because bytes that may not have
 * reached the disk must not wear the bits of a program.
 */
static void write_run_finish(const WriteSource *s, const char *path, WriteTarget *t,
                             WriteBackup *bk, WpeWriteOutcome *o)
{
 struct stat written;
 int have_written = 0, err, again, again_err;

 again = fcntl(t->fd, F_DUPFD_CLOEXEC, 0);
 again_err = errno;
 err = write_source_put(s, t->fp, &written, &have_written);
 if (err == 0 && again >= 0)
  err = wpe_write_sync_before_bits(again, t->keep, 0);
 if (err != 0)
 {
  o->err = write_error_number(err);
  write_after_failure(o, path, bk->result, bk->fd, &written, have_written, again,
                      write_restore_mode(t));
  bk->fd = -1;
  return;
 }
 write_keep_special_bits(again, again_err, t, &written, have_written, &o->file_kept);
 write_read_back_stays(again, t, &written, have_written, &o->file_stays);
 if (again >= 0)
  close(again);
 write_backup_release(bk);
 wpe_write_identity_take(&o->written, &t->st);
 o->status = WPE_WRITE_WRITTEN;
}

/**
 * write_run - Write a source (a window's rows or a text) to a path, judged.
 * @s: the source; @path: the file; @backup: 1 to copy an existing file to its
 * .bak first; @aim: what the write is for (a save of the window's file, which
 * gets its special bits back; a replace of the file at the name, which
 * does too; or a copy, which gets none), how the name is opened and what to do
 * when it now leads to another file; @o: receives the outcome.
 *
 * The one place the editor's text reaches a user's file, whoever asked: the
 * window's Save, a block, and the assistant's writes.  A write that fails
 * after the file was opened puts the old content back (write_restore) when a
 * .bak was made, and the outcome says what could not be done.
 */
static void write_run(const WriteSource *s, const char *path, int backup, const WriteAim *aim,
                      WpeWriteOutcome *o)
{
 WriteBackup bk;
 WriteTarget t;

 memset(o, 0, sizeof *o);
 o->status = WPE_WRITE_FAILED;
 write_set_path(o, path);
 if (s->f != NULL && s->f->ins == WPE_WRITE_WINDOW_READ_ONLY)
 {
  o->err = EACCES;
  o->read_only = 1;
  return;
 }
 wpe_write_guard_enter();
 if (write_run_open(s, path, backup, aim, &t, &bk, o) == 0)
  write_run_finish(s, path, &t, &bk, o);
 wpe_write_guard_leave();
}

/**
 * write_aim_make - Say what a write is for.
 * @aim: receives it; @kind: save, replace or copy; @own: the identity of the
 * file the window read, or NULL; @replaced: what to do when the name leads to
 * another file; @open: how the name is opened.
 *
 * A revert adds its recorded mode afterwards (write_aim_record).
 */
static void write_aim_make(WriteAim *aim, WriteAimKind kind, const WpeFileIdentity *own,
                           const WriteReplacedPolicy *replaced, WriteOpenHow open)
{
 memset(aim, 0, sizeof *aim);
 aim->kind = kind;
 aim->own = own;
 aim->replaced = *replaced;
 aim->open = open;
}

/** write_aim_record - A revert's aim wears again the twelve bits it recorded. */
static void write_aim_record(WriteAim *aim, mode_t recorded)
{
 aim->has_recorded = 1;
 aim->recorded = recorded;
}

/** write_source_window - A window's rows from (xa, ya) to (xe, ye) as a source. */
static void write_source_window(WriteSource *s, FENSTER *f, int xa, int ya, int xe, int ye)
{
 memset(s, 0, sizeof *s);
 s->f = f;
 s->r.xa = xa;
 s->r.ya = ya;
 s->r.xe = xe;
 s->r.ye = ye;
}

/** write_source_text - A text of @len bytes, which has no window, as a source. */
static void write_source_text(WriteSource *s, const char *text, size_t len)
{
 memset(s, 0, sizeof *s);
 s->text = text;
 s->len = len;
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
 WriteSource s;
 WriteAim aim;

 write_source_window(&s, f, xa, ya, xe, ye);
 write_aim_make(&aim, WRITE_AIM_COPY, NULL, &write_replaced_as_copy, WRITE_OPEN_NAMED);
 write_run(&s, path, backup, &aim, o);
 return(o->status == WPE_WRITE_WRITTEN ? 0 : -1);
}

/**
 * wpe_write_copy - Write a window's whole text to a copy the editor named.
 * @f: the window; @path: the copy's name (the autosave's .ASV, the emergency
 * save's .ESV, the hook's post-edit copy); @o: receives the outcome, with
 * @path in o->path.
 * Return: 0 when the copy holds the text, -1 when not (the outcome says why).
 *
 * What the user gets: the copy the editor keeps beside a file is never
 * written through a link or into a file that somebody else put at its name.
 * An old copy of ours (a regular file of this user) is replaced by a new
 * file; anything else there is left alone and said (o->planted), with
 * nothing written.  The new file has the window's nine permission bits
 * before its first byte, never a special bit, and no .bak is made: the copy
 * is not the user's file.
 */
int wpe_write_copy(FENSTER *f, const char *path, WpeWriteOutcome *o)
{
 WriteSource s;
 WriteAim aim;

 write_source_window(&s, f, 0, 0, E_BUFFER_ROW_END(f->b), f->b->mxlines - 1);
 write_aim_make(&aim, WRITE_AIM_COPY, NULL, &write_replaced_as_copy, WRITE_OPEN_NEW);
 write_run(&s, path, 0, &aim, o);
 return(o->status == WPE_WRITE_WRITTEN ? 0 : -1);
}

/**
 * wpe_write_copy_remove - Remove a copy the editor named when its window closes.
 * @path: the copy's name; @own: the identity of the copy this window wrote
 * or read in this session.
 *
 * What the user gets: closing a window leaves no .ASV or .ESV that it made
 * itself, while a copy from an earlier session (the one "wpe -r" brings
 * back) or one the user made is never deleted by a plain close.  Only the
 * very file the window wrote, still a regular file of this user, is removed.
 */
void wpe_write_copy_remove(const char *path, const WpeFileIdentity *own)
{
 struct stat st;

 if (own == NULL || !own->known)
  return;
 if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid())
  return;
 if (!wpe_write_identity_is(own, st.st_dev, st.st_ino))
  return;
 (void)unlink(path);
}

/**
 * wpe_write_copies_forget - A new window has made and read no copy.
 * @c: the window's copies.
 */
void wpe_write_copies_forget(WpeWindowCopies *c)
{
 memset(c, 0, sizeof *c);
}

/**
 * write_open_how_of - How a name is opened, from the caller's flags.
 * @flags: WPE_WRITE_NOFOLLOW or 0.
 */
static WriteOpenHow write_open_how_of(int flags)
{
 if (flags & WPE_WRITE_NOFOLLOW)
  return(WRITE_OPEN_NAMED_NOFOLLOW);
 return(WRITE_OPEN_NAMED);
}

/**
 * write_text_kind - What a text written into a user's file is for.
 * @own: the identity of the window open on the file, or NULL.
 * Return: a save of that window's file, or, with no window, a replace of
 * the file at the name.
 */
static WriteAimKind write_text_kind(const WpeFileIdentity *own)
{
 if (own != NULL && own->known)
  return(WRITE_AIM_SAVE);
 return(WRITE_AIM_REPLACE);
}

/**
 * write_text_run - Write a whole text into a user's file under a policy for
 * a name that leads to another file than the window read.
 * @path, @text, @own, @flags, @expect, @o: as for wpe_write_save_text;
 * @policy: refuse, ask, or write what the user said Yes about.
 * Return: 0 when the file holds the text, -1 when not.
 */
static int write_text_run(const char *path, const char *text, const WpeFileIdentity *own, int flags,
                          const WriteReplacedPolicy *policy, const WpeWriteExpect *expect,
                          WpeWriteOutcome *o)
{
 WriteSource s;
 WriteAim aim;

 write_source_text(&s, text, strlen(text));
 write_aim_make(&aim, write_text_kind(own), own, policy, write_open_how_of(flags));
 aim.expect = expect;
 write_run(&s, path, 1, &aim, o);
 return(o->status == WPE_WRITE_WRITTEN ? 0 : -1);
}

/**
 * wpe_write_save_text - Write a whole text into a file of the user's, as a
 * Save does.
 * @path: the file; @text: the bytes; @own: the identity of the window open on
 * that file, or NULL; @flags: WPE_WRITE_NOFOLLOW to refuse a link at the end
 * of @path; @expect: what the caller saw at the name, or NULL; @o: receives
 * the outcome.
 * Return: 0 when the file holds the text, -1 when not (the outcome says why).
 *
 * What the user gets when the assistant changes a file: the old text kept in
 * its .bak, a program's setuid, setgid or sticky bit still on it after the
 * text is on disk (or named, when the system would not keep it), and, when
 * a window is open on the file, a refusal instead of a write into a different
 * file that took the name since the window read it.  A file that is not the
 * one the caller looked at, or has gained a name since, is refused too, and
 * so is a pipe or a device: the assistant never writes where the user was not
 * shown.  This face refuses: it has no one to ask.  A caller that can show
 * the question asks first, as Save does, and writes only on a Yes.
 */
int wpe_write_save_text(const char *path, const char *text, const WpeFileIdentity *own, int flags,
                        const WpeWriteExpect *expect, WpeWriteOutcome *o)
{
 return(write_text_run(path, text, own, flags, &write_replaced_refuse, expect, o));
}

/**
 * wpe_write_save_text_ask - The same Save of a text, for a caller that can
 * ask the user when the window's file was replaced on disk.
 * @path, @text, @own, @flags, @expect, @o: as for wpe_write_save_text;
 * @asked: NULL on the first pass, else the file the user said Yes about.
 * Return: 0 when the file holds the text, -1 when not.
 *
 * What the user gets: when the name leads to another file than the one the
 * window read, a plain file of the user's own with no setuid, setgid or
 * sticky bit, the assistant does not refuse and does not write behind the
 * user's back; it asks, as Save does.  The first pass (@asked NULL) writes
 * nothing and sets o->ask_overwrite with the file in o->asked; the caller
 * shows the question and, on a Yes, calls again with that identity in
 * @asked.  That pass writes the text as a copy: the file gets no special
 * bit and the window takes it as its own.  Only the file the user was asked
 * about is written; one that took the name while the box was up is refused.
 * A program, another user's file or one that is not a regular file is
 * refused on the first pass, as it is for Save.
 */
int wpe_write_save_text_ask(const char *path, const char *text, const WpeFileIdentity *own,
                            int flags, const WpeFileIdentity *asked, const WpeWriteExpect *expect,
                            WpeWriteOutcome *o)
{
 WriteReplacedPolicy policy;

 if (asked == NULL)
  policy = write_replaced_ask;
 else
 {
  policy.how = WRITE_REPLACED_ACCEPTED;
  policy.asked = *asked;
 }
 return(write_text_run(path, text, own, flags, &policy, expect, o));
}

/**
 * wpe_write_restore_text - Put a before image's text back into a file, with
 * the permission bits recorded with it.
 * @path: the file, already resolved by the caller; @text, @len: the bytes
 * (which may hold NUL bytes); @recorded: the twelve bits to give back, or
 * WPE_WRITE_NO_RECORDED_MODE; @own: the identity of the window open on the
 * file, or NULL; @expect: what the caller saw at the name, or NULL; @o:
 * receives the outcome.
 * Return: 0 when the file holds the text, -1 when not (the outcome says why).
 *
 * The name is opened without following a link at its end
 * (WRITE_OPEN_NAMED_NOFOLLOW): the caller resolved it once, and a link put
 * there since is refused.  A file that is not there is made with
 * WRITE_NEW_FILE_MODE (0666 less the umask) and then given the recorded nine
 * bits.  What the user gets: after a revert the file is what it was before
 * the assistant's run, a program still a program, and a bit the system would
 * not give back is named.  A file that no mode was recorded for (only a
 * command changed it) keeps the bits it has: its text goes back and nothing
 * else about it changes.  No .bak is made: the before image the text comes
 * from is the copy, and a revert leaves no new file beside the user's.
 */
int wpe_write_restore_text(const char *path, const char *text, size_t len, mode_t recorded,
                           const WpeFileIdentity *own, const WpeWriteExpect *expect,
                           WpeWriteOutcome *o)
{
 WriteSource s;
 WriteAim aim;

 write_source_text(&s, text, len);
 write_aim_make(&aim, write_text_kind(own), own, &write_replaced_refuse, WRITE_OPEN_NAMED_NOFOLLOW);
 if (recorded != WPE_WRITE_NO_RECORDED_MODE)
  write_aim_record(&aim, recorded);
 aim.expect = expect;
 write_run(&s, path, 0, &aim, o);
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
 * wpe_write_identity_take - Remember the file a window just read.
 * @id: the window's FENSTER.file_id; @st: fstat of the descriptor that read it.
 *
 * Known when it is a regular file; anything else (a device, a pipe) leaves
 * the window with no file to write into by identity.  Taken from the open
 * descriptor, never from a second look at the name, so it is the file whose
 * text the window holds.
 */
void wpe_write_identity_take(WpeFileIdentity *id, const struct stat *st)
{
 id->known = S_ISREG(st->st_mode) ? 1 : 0;
 id->dev = id->known ? st->st_dev : 0;
 id->ino = id->known ? st->st_ino : 0;
}

/**
 * wpe_write_identity_forget - The window has read no file.
 * @id: the window's FENSTER.file_id.
 *
 * A new window, the clipboard and a window of a file that did not exist have
 * nothing to compare a later Save with: the save writes as a copy and the
 * window then takes the file it made.
 */
void wpe_write_identity_forget(WpeFileIdentity *id)
{
 memset(id, 0, sizeof *id);
}

/**
 * wpe_write_identity_set - A window now has the file another write made or kept.
 * @id: the window's FENSTER.file_id; @from: the identity the write reported
 * (WpeWriteOutcome.written).
 *
 * After the assistant's revert puts text back into a file, the window that
 * shows it keeps its identity, even when the revert had to make the file
 * again.
 */
void wpe_write_identity_set(WpeFileIdentity *id, const WpeFileIdentity *from)
{
 *id = *from;
}

/**
 * wpe_write_identity_is - Is this the file with that device and inode?
 * @id: a window's FENSTER.file_id; @dev, @ino: a file's.
 * Return: 1 when the window read that very file.
 *
 * What finds the window that is open on a file the assistant is about to
 * write, whatever name or link the window was opened by.
 */
int wpe_write_identity_is(const WpeFileIdentity *id, dev_t dev, ino_t ino)
{
 return(id->known && id->dev == dev && id->ino == ino);
}

/**
 * wpe_write_identity_differs - Did the window read a file other than this one?
 * @id: a window's FENSTER.file_id; @dev, @ino: the file now at its name.
 * Return: 1 when the window knows the file it read and it is not that one,
 * 0 when it is, or when the window knows no file.
 *
 * What lets the editor say, before a write, that the file under the window's
 * name was replaced.
 */
int wpe_write_identity_differs(const WpeFileIdentity *id, dev_t dev, ino_t ino)
{
 return(id->known && (id->dev != dev || id->ino != ino));
}

/**
 * wpe_write_replacement_may_be_asked - May a replaced file be written on a Yes?
 * @path: the file now at the window's name.
 * Return: 1 when it is a plain file of the user's own with no setuid, setgid
 * or sticky bit (the owner's own rule, write_replaced_is_ours), else 0.
 *
 * What lets a caller that asks before it writes (the assistant's box under
 * ask) skip a question the owner would refuse after the Yes: a program,
 * another user's file or anything that is not a regular file is refused at
 * once, with the sentence, as when there is no one to ask.
 */
int wpe_write_replacement_may_be_asked(const char *path)
{
 const char *hook = getenv(WPE_WRITE_TEST_FAIL_ENV);
 struct stat st;

 if (hook != NULL && strstr(hook, WPE_WRITE_TEST_REPLACEMENT_NO) != NULL)
  return(0);                       /* the file changed after this look: the open decides otherwise */
 if (lstat(path, &st) != 0)
  return(0);
 return(write_replaced_is_ours(&st));
}

/**
 * wpe_write_identity_of_name - Take the identity of the file at a folder and name.
 * @dir, @name: where it is; @id: receives it.
 * Return: 1 when it is a regular file and @id is known, else 0 (@id forgotten).
 *
 * The one take by name: for a window that read a copy of its file (the
 * recovery reads the .ESV), whose own file is the one under the original
 * name.  A swap in that moment is the recovery's own and not seen.
 */
int wpe_write_identity_of_name(const char *dir, const char *name, WpeFileIdentity *id)
{
 struct stat st;
 char *path;

 wpe_write_identity_forget(id);
 if (dir == NULL || name == NULL)
  return(0);
 path = e_mkfilename((char *)dir, (char *)name);
 if (path == NULL)
  return(0);
 if (stat(path, &st) == 0)
  wpe_write_identity_take(id, &st);
 FREE(path);
 return(id->known);
}

/**
 * wpe_write_overwrite_bits - What a write onto an existing file would do to
 * its special bits.
 * @path: the file the user picked; @own: the file the window read (its
 * FENSTER.file_id), or NULL for a block (always a copy); @drops, @stays:
 * receive the setuid, setgid and sticky bits the copy will not keep, and
 * those it will keep (only the sticky bit, of a file the user does not own).
 *
 * Asked before Save As or Write Block replaces a file, so the user is told
 * that the copy will not be a program, or that it will stay one, before
 * answering, not after.  Both are 0 for a file that is not regular, has no
 * such bit, or is the window's own file (which keeps them).  It uses the
 * identity rule of the write itself (write_purpose_for_target), so the
 * question and the write cannot disagree about which file is the window's.
 * A link is followed, as the write follows it.  The split is a prediction.
 * The owner or root may change a mode, so every bit drops.  For a file the
 * user does not own the system itself clears the setuid and setgid bits on
 * a write (Linux and the BSDs), so those drop too, and only the sticky bit
 * stays, because the call that would take it off is refused.  Where an ACL
 * or a capability makes this wrong, the status row after the write tells
 * the truth.
 */
void wpe_write_overwrite_bits(const char *path, const WpeFileIdentity *own, mode_t *drops,
                              mode_t *stays)
{
 struct stat st;
 mode_t bits;
 int can_drop;

 *drops = 0;
 *stays = 0;
 if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
  return;
 if (write_purpose_for_target(own, &st, 0) == WRITE_PURPOSE_SAVE)
  return;
 bits = st.st_mode & WRITE_SPECIAL_BITS;
 can_drop = (st.st_uid == geteuid() || geteuid() == 0);
 if (can_drop)
  *drops = bits;
 else
 {
  *drops = bits & (S_ISUID | S_ISGID);
  *stays = bits & S_ISVTX;
 }
}

/**
 * write_window_pass - One try to write the whole window to its file.
 * @f: the window; @path: the file; @replaced: what to do when the name leads
 * to another file than the window read; @o: receives the outcome.
 *
 * The range is taken from the window's text now, so a pass that follows a
 * question writes what the window holds after the question, not before.
 */
static void write_window_pass(FENSTER *f, const char *path, const WriteReplacedPolicy *replaced,
                              WpeWriteOutcome *o)
{
 WriteSource s;
 WriteAim aim;

 write_source_window(&s, f, 0, 0, E_BUFFER_ROW_END(f->b), f->b->mxlines - 1);
 write_aim_make(&aim, WRITE_AIM_SAVE, &f->file_id, replaced, WRITE_OPEN_NAMED);
 write_run(&s, path, 1, &aim, o);
}

/**
 * write_window_after_question - A plain Save found another file under the
 * name: ask, and write it as a copy on Yes.
 * @f: the window; @path: the file; @o: the first pass's outcome, replaced by
 * the outcome of the question.
 *
 * Asked while no write is open, so nothing is held and no signal is
 * ignored while the box is up.  No and Esc write nothing.
 */
static void write_window_after_question(FENSTER *f, const char *path, WpeWriteOutcome *o)
{
 char question[WPE_PATH_SIZE + 128];
 WriteReplacedPolicy accepted;

 accepted.how = WRITE_REPLACED_ACCEPTED;
 accepted.asked = o->asked;
 snprintf(question, sizeof question, "%s was replaced on disk since this window read it. "
          "Overwrite it?", write_base_name(path));
 WpeMouseRestoreShape();
 if (e_message(1, question, f) != 'Y')
 {
  WpeMouseChangeShape(WpeWorkingShape);
  o->ask_overwrite = 0;
  o->status = WPE_WRITE_DECLINED;
  return;
 }
 WpeMouseChangeShape(WpeWorkingShape);
 write_window_pass(f, path, &accepted, o);
}

/**
 * write_window_file - Write the whole window to the file its name says.
 * @f: the window; @plain_save: a Save onto the window's own name, which asks
 * or refuses when that name leads to another file (a Save As is a copy and
 * never asks); @o: receives the outcome.
 * Return: 0 when the file holds the text (or there is nothing to write),
 * -1 when it does not (declined or failed; the outcome says which).
 *
 * Asks first when the file had NUL bytes the window cannot hold.  On success
 * the window no longer reads as modified and takes the file it wrote as its
 * own; on any other outcome it stays as it is, so the user still has the text
 * and the next Save tries again.  The file the write opens is recognised as
 * the window's by the identity the window recorded when it read it
 * (write_purpose_for_target), never by looking at the name again.
 */
static int write_window_file(FENSTER *f, int plain_save, WpeWriteOutcome *o)
{
 char *path;

 memset(o, 0, sizeof *o);
 o->status = WPE_WRITE_WRITTEN;           /* nothing to write is written */
 if (!write_window_listed(f))
  return(0);
 if (!write_nul_confirmed(f, f->b))
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
 write_window_pass(f, path, plain_save ? &write_replaced_ask : &write_replaced_as_copy, o);
 if (o->ask_overwrite)
  write_window_after_question(f, path, o);
 FREE(path);
 if (o->status == WPE_WRITE_WRITTEN)
 {
  f->file_id = o->written;
  f->save = 0;
  write_final_break_after(f, f->b);
  f->b->nul_dropped = 0;                 /* the user knew, and the file is written */
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
 return(write_window_file(f, 1, o));
}

/* --- text that is not a window's rows ------------------------------------- */

/**
 * write_fd_into - Write @len bytes to a descriptor and close it, judged.
 * @fd: an open descriptor, closed here; @text, @len: the bytes; @o: receives
 * the outcome (path and the rest are the caller's).
 */
static void write_fd_into(int fd, const char *text, size_t len, WpeWriteOutcome *o)
{
 size_t done = 0;
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
 write_fd_into(fd, text, strlen(text), o);
 return(o->status == WPE_WRITE_WRITTEN ? 0 : -1);
}

/* --- The editor's own folders and files -------------------------------------- */

/** The mode of a folder the editor makes, and the bits that make one not
    private to its user. */
#define WRITE_OWN_DIR_MODE   0700
#define WRITE_OWN_OTHERS     (S_IRWXG | S_IRWXO)

/** How many folders a name below a base may have. */
#define WRITE_OWN_SUB_MAX    2

/**
 * write_own_base - The base folder of a kind, resolved by name.
 * @kind: which of the editor's folders; @buf/@n: receive its path.
 * Return: 0, or -1 when there is none (no home folder, no run folder).
 */
static int write_own_base(WpeOwnKind kind, char *buf, size_t n)
{
 switch (kind)
 {
 case WPE_OWN_STATE:
  return(wpe_state_dir(buf, n));
 case WPE_OWN_RUN:
  return(wpe_run_dir(buf, n));
 case WPE_OWN_CONFIG:
  return(wpe_config_dir(buf, n));
 }
 return(-1);
}

/**
 * write_own_name_ok - May this be one component of a folder or file name?
 * @name: the text; @len: how much of it is the component.
 * Return: 1 when it is a name the editor may make: not empty, not "." or "..".
 */
static int write_own_name_ok(const char *name, size_t len)
{
 if (len == 0 || len >= WPE_NAME_SIZE)
  return(0);
 if (len == 1 && name[0] == '.')
  return(0);
 return(!(len == 2 && name[0] == '.' && name[1] == '.'));
}

/**
 * write_own_sub_ok - May this be the folder below a base?
 * @sub: NULL, or one or two names joined by '/'.
 * Return: 1 when every component is a name and there are at most two.
 */
static int write_own_sub_ok(const char *sub)
{
 size_t parts = 0, len;

 if (sub == NULL)
  return(1);
 for (;;)
 {
  len = strcspn(sub, "/");
  if (!write_own_name_ok(sub, len))
   return(0);
  parts++;
  if (sub[len] == '\0')
   break;
  sub += len + 1;
 }
 return(parts <= WRITE_OWN_SUB_MAX);
}

/**
 * write_own_leaf_ok - May this be the name of a file in an own folder?
 * @leaf: the text.
 * Return: 1 when it is one component and not "." or "..".
 */
static int write_own_leaf_ok(const char *leaf)
{
 size_t len;

 if (leaf == NULL)
  return(0);
 len = strlen(leaf);
 return(strchr(leaf, '/') == NULL && write_own_name_ok(leaf, len));
}

/**
 * write_own_failed - Record why an own folder or file was refused.
 * @o: the outcome; @err: the errno; @planted: 1 when something that is not
 * the user's own folder holds the name.
 * Return: -1.
 */
static int write_own_failed(WpeWriteOutcome *o, int err, int planted)
{
 o->status = WPE_WRITE_FAILED;
 o->err = err;
 o->planted = planted;
 return(-1);
}

/**
 * write_own_make_above - Make the folders above a base that are missing.
 * @base: the base folder's path.
 *
 * Each is made 0700 and never judged: a link at ~/.local/state is the
 * user's own choice.  A failure is met when the base is opened.
 */
static void write_own_make_above(const char *base)
{
 char path[WPE_PATH_SIZE];
 size_t i, len = strlen(base);

 if (len == 0 || len >= sizeof path)
  return;
 memcpy(path, base, len + 1);
 for (i = 1; i < len; i++)
 {
  if (path[i] != '/')
   continue;
  path[i] = '\0';
  (void)mkdir(path, WRITE_OWN_DIR_MODE);
  path[i] = '/';
 }
}

/**
 * write_own_judge - Judge an open folder: the user's own, and the user's alone.
 * @fd: the folder, open.
 * Return: 0 when it may be used, else the errno that says why not (ENOTDIR
 * for something that is not a folder, EEXIST for another user's folder).
 *
 * A folder that others can read or write is made 0700, through the mode
 * owner: the conversations and before images in it are the user's alone.
 */
static int write_own_judge(int fd)
{
 struct stat st;
 WpeModeKept kept;

 if (fstat(fd, &st) != 0)
  return(errno);
 if (!S_ISDIR(st.st_mode))
  return(ENOTDIR);
 if (st.st_uid != geteuid())
  return(EEXIST);
 if ((st.st_mode & WRITE_OWN_OTHERS) == 0)
  return(0);
 return(wpe_write_set_mode_fd(fd, WRITE_OWN_DIR_MODE, &kept));
}

/**
 * write_own_planted - Is this errno the word for "not the user's own folder"?
 * @err: what write_own_judge or an open said.
 */
static int write_own_planted(int err)
{
 return(err == ENOTDIR || err == EEXIST);
}

/**
 * write_own_open_base - Open and judge the base folder of an own kind.
 * @base: its path; @how: make it or only look; @o: receives the refusal.
 * Return: the open folder, or -1.
 *
 * The base is resolved by name and a link at it is followed (a state folder
 * kept in a dotfiles tree is the user's choice); what is judged is the folder
 * it leads to.
 */
static int write_own_open_base(const char *base, WpeOwnHow how, WpeWriteOutcome *o)
{
 int fd, err;

 if (how == WPE_OWN_MAKE)
 {
  write_own_make_above(base);
  (void)mkdir(base, WRITE_OWN_DIR_MODE);
 }
 fd = open(base, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
 if (fd < 0)
 {
  err = errno;
  return(write_own_failed(o, err, write_own_planted(err)));
 }
 err = write_own_judge(fd);
 if (err != 0)
 {
  close(fd);
  return(write_own_failed(o, err, write_own_planted(err)));
 }
 return(fd);
}

/**
 * write_own_step - Go one folder down below the base, judged.
 * @fd: the folder we are in; @name: the next component (NUL-terminated);
 * @how: make it or only look; @o: receives the refusal.
 * Return: the open folder below, or -1.
 *
 * Nothing here follows a link: what is found at the name is looked at first,
 * and a link, another user's folder or a file is refused.
 */
static int write_own_step(int fd, const char *name, WpeOwnHow how, WpeWriteOutcome *o)
{
 struct stat st;
 int below, err;

 if (how == WPE_OWN_MAKE)
  (void)mkdirat(fd, name, WRITE_OWN_DIR_MODE);
 if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
  return(write_own_failed(o, errno, 0));
 if (S_ISLNK(st.st_mode))
  return(write_own_failed(o, EEXIST, 1));
 if (!S_ISDIR(st.st_mode))
  return(write_own_failed(o, ENOTDIR, 1));
 below = openat(fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
 if (below < 0)
  return(write_own_failed(o, errno, 0));
 err = write_own_judge(below);
 if (err != 0)
 {
  close(below);
  return(write_own_failed(o, err, write_own_planted(err)));
 }
 return(below);
}

/**
 * write_own_walk - Walk the components of @sub below an open base, judging each.
 * @fd: the base, open (closed here); @sub: NULL or one or two names;
 * @how: make or look; @o: receives the refusal.
 * Return: 0 when every component is the user's own folder, else -1.
 */
static int write_own_walk(int fd, const char *sub, WpeOwnHow how, WpeWriteOutcome *o)
{
 char name[WPE_NAME_SIZE];
 size_t len;
 int below;

 while (sub != NULL && *sub != '\0')
 {
  len = strcspn(sub, "/");
  memcpy(name, sub, len);
  name[len] = '\0';
  below = write_own_step(fd, name, how, o);
  close(fd);
  if (below < 0)
   return(-1);
  fd = below;
  sub += len + (sub[len] == '/' ? 1 : 0);
 }
 close(fd);
 return(0);
}

/**
 * write_own_path - The path of an own folder: the base, then @sub.
 * @path/@n: receive it; @base: the base's path; @sub: NULL or below it.
 * Return: 0, or -1 when it does not fit.
 */
static int write_own_path(char *path, size_t n, const char *base, const char *sub)
{
 if (sub == NULL)
  return(wpe_path_copy(path, n, base));
 return(wpe_path_join(path, n, base, sub));
}

/**
 * wpe_own_dir - Resolve and judge one of the editor's own folders.
 * @kind: the state, run or configuration folder; @sub: NULL, or the folder
 * below it (one or two names joined by '/'); @how: WPE_OWN_MAKE or
 * WPE_OWN_LOOK; @d: receives the folder's path; @o: receives the refusal.
 * Return: 0 when the folder is the user's own and private, else -1.
 *
 * What the user gets: what the editor keeps for itself (the conversation, the
 * before images of a run, the lists it hands to git) is in a folder that is
 * the user's alone, made when it is first needed, and never in a folder a
 * cloned repository could have planted, and with no home folder known it is
 * said so instead of written under the folder the editor was started in.  A
 * reader that only looks never leaves a folder behind.
 */
int wpe_own_dir(WpeOwnKind kind, const char *sub, WpeOwnHow how, WpeOwnDir *d, WpeWriteOutcome *o)
{
 char base[WPE_PATH_SIZE];
 int fd;

 memset(o, 0, sizeof *o);
 d->path[0] = '\0';
 o->status = WPE_WRITE_FAILED;
 if (!write_own_sub_ok(sub))
  return(write_own_failed(o, EINVAL, 0));
 if (write_own_base(kind, base, sizeof base) != 0)
 {
  o->no_place = 1;
  return(write_own_failed(o, ENOENT, 0));
 }
 if (write_own_path(d->path, sizeof d->path, base, sub) != 0)
  return(write_own_failed(o, ENAMETOOLONG, 0));
 write_set_path(o, d->path);
 fd = write_own_open_base(base, how, o);
 if (fd < 0 || write_own_walk(fd, sub, how, o) != 0)
  return(-1);
 o->status = WPE_WRITE_WRITTEN;
 return(0);
}

/**
 * wpe_write_own_bytes - Write a file of the editor's own folder, new and private.
 * @kind, @sub: the folder (see wpe_own_dir); @leaf: the file's name, one
 * component; @bytes, @len: the content, which may hold NUL bytes; @o: receives
 * the outcome, with the path.
 * Return: 0 when the file holds the bytes, else -1.
 *
 * What the user gets: the conversation, a before image or a recorded mode is
 * never written through a link or into a file that somebody else put at its
 * name, and is readable by the user alone from its first byte.  An old file
 * of ours is replaced by a new one; anything else at the name is left alone
 * and said.
 *
 * How: the folders are judged on open descriptors (wpe_own_dir).  The final
 * leaf is then reached by name, inside a folder just proven to be the user's
 * alone at 0700, so nobody else can put anything there between the judgement
 * and the write; the file itself is made with O_EXCL and O_NOFOLLOW.
 */
int wpe_write_own_bytes(WpeOwnKind kind, const char *sub, const char *leaf, const char *bytes,
                        size_t len, WpeWriteOutcome *o)
{
 char path[WPE_PATH_SIZE];
 WpeOwnDir d;
 WriteSource s;
 WriteAim aim;

 if (!write_own_leaf_ok(leaf))
 {
  memset(o, 0, sizeof *o);
  (void)write_own_failed(o, EINVAL, 0);
  return(-1);
 }
 if (wpe_own_dir(kind, sub, WPE_OWN_MAKE, &d, o) != 0)
  return(-1);
 if (wpe_path_join(path, sizeof path, d.path, leaf) != 0)
  return(write_own_failed(o, ENAMETOOLONG, 0));
 write_source_text(&s, bytes, len);
 write_aim_make(&aim, WRITE_AIM_COPY, NULL, &write_replaced_as_copy, WRITE_OPEN_NEW);
 write_run(&s, path, 0, &aim, o);
 return(o->status == WPE_WRITE_WRITTEN ? 0 : -1);
}

/**
 * wpe_write_own_remove - Remove a file the editor keeps in its own folder.
 * @kind, @sub, @leaf: as for wpe_write_own_bytes.
 *
 * Only a regular file of this user is removed, so closing a conversation or
 * finishing a run never deletes something that is not ours.  A folder that
 * is not there is nothing to remove, and none is made.
 *
 * How: the folders are judged on open descriptors (wpe_own_dir).  The leaf
 * is then looked at and removed by name, inside a folder just proven to be the
 * user's alone at 0700.
 */
void wpe_write_own_remove(WpeOwnKind kind, const char *sub, const char *leaf)
{
 char path[WPE_PATH_SIZE];
 WpeWriteOutcome o;
 struct stat st;
 WpeOwnDir d;

 if (!write_own_leaf_ok(leaf))
  return;
 if (wpe_own_dir(kind, sub, WPE_OWN_LOOK, &d, &o) != 0)
  return;
 if (wpe_path_join(path, sizeof path, d.path, leaf) != 0)
  return;
 if (lstat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid())
  (void)unlink(path);
}

/**
 * write_named_copy_aim - What a text or a stream the user named is for.
 * @aim: receives it.
 *
 * A file the user named for a list or a text (an options file, a project, the
 * AI transcript) is a copy: the name is followed as Save follows it, the
 * file is cut only after its bits are read, and a setuid, setgid or sticky
 * bit it had is taken off, never given back, because the new content is not
 * the program that was there.
 */
static void write_named_copy_aim(WriteAim *aim)
{
 write_aim_make(aim, WRITE_AIM_COPY, NULL, &write_replaced_as_copy, WRITE_OPEN_NAMED);
}

/**
 * wpe_write_text - Create or replace a file with a string, judged.
 * @path: the file; @text: the bytes; @o: receives the outcome, with the path.
 * Return: 0 when written, else -1.
 *
 * What the user gets: the file the user named is written the way a Save
 * writes it (opened without cutting it, bits read from the file itself, cut
 * only then, errors named by their reason), but as a copy: over a setgid or
 * sticky file of the user's the text drops the bit, and when the system
 * refuses to drop it the outcome says the file keeps it (file_stays).
 */
int wpe_write_text(const char *path, const char *text, WpeWriteOutcome *o)
{
 WriteSource s;
 WriteAim aim;

 write_source_text(&s, text, strlen(text));
 write_named_copy_aim(&aim);
 write_run(&s, path, 0, &aim, o);
 return(o->status == WPE_WRITE_WRITTEN ? 0 : -1);
}

/**
 * write_stream_read_back - Record the special bits a stream's file still has.
 * @t: the target of the stream, open; @o: receives file_stays.
 *
 * Read from the open file after the nine-bit call that was to take its
 * special bits off.  This is only the list of bits to check: a system may
 * clear a bit when the text is written (FreeBSD, NetBSD and macOS for a user
 * who is not root), and the user may change the file's mode while the stream
 * is open.  write_stream_confirm_stays reads the file again at the close and
 * keeps only the bits it still has; the sentence names those and no others.
 */
static void write_stream_read_back(const WriteTarget *t, WpeWriteOutcome *o)
{
 struct stat unused;

 write_read_back_stays(t->fd, t, &unused, 0, &o->file_stays);
}

/**
 * write_stream_confirm_stays - Keep, of the bits a stream's file was found to
 * keep at the open, only those it has when the stream ends.
 * @fp: the stream, still open; @o: holds file_stays from the open.
 *
 * What the user gets: "written, but it keeps the sticky bit" is said only of
 * a bit that is on the file when the last byte is in, never of one the system
 * or the user took off in between.  When the file cannot be read, nothing is
 * said: a bit is never claimed to stay on a guess.
 */
static void write_stream_confirm_stays(FILE *fp, WpeWriteOutcome *o)
{
 struct stat now;

 if (o->file_stays.lost == 0)
  return;
 if (fstat(fileno(fp), &now) != 0)
 {
  memset(&o->file_stays, 0, sizeof o->file_stays);
  return;
 }
 o->file_stays.lost &= now.st_mode & WRITE_SPECIAL_BITS;
 if (o->file_stays.lost == 0)
  memset(&o->file_stays, 0, sizeof o->file_stays);
}

/**
 * wpe_write_stream_open - Create or replace a file the caller prints into.
 * @path: the file; @o: receives the outcome, with the path.
 * Return: the stream, or NULL when the file could not be opened (the outcome
 * is FAILED and says why).
 *
 * For a list of unknown length (the project's lines, the options): the user
 * who names a folder that refuses the file hears the system's reason instead
 * of a guess.  The file is opened and cut the way a Save does it
 * (write_run_open), as a copy of what was there, so a setgid or sticky bit on
 * the old file is taken off before the first byte, and a link at the name is
 * followed as Save follows it.  The caller must end with
 * wpe_write_stream_close and read its result; the file size limit is ignored
 * until then, so a full limit is a failed outcome and not the end of the
 * editor.
 */
FILE *wpe_write_stream_open(const char *path, WpeWriteOutcome *o)
{
 WriteSource s;
 WriteAim aim;
 WriteTarget t;
 WriteBackup bk;

 memset(o, 0, sizeof *o);
 o->status = WPE_WRITE_FAILED;
 write_set_path(o, path);
 write_source_text(&s, "", 0);
 write_named_copy_aim(&aim);
 wpe_write_guard_enter();
 if (write_run_open(&s, path, 0, &aim, &t, &bk, o) != 0)
 {
  wpe_write_guard_leave();
  return(NULL);
 }
 write_stream_read_back(&t, o);
 o->status = WPE_WRITE_WRITTEN;
 return(t.fp);
}

/**
 * wpe_write_stream_close - End a stream of wpe_write_stream_open, judged.
 * @fp: the stream, closed here; @o: the outcome of the open, updated.
 * Return: 0 when every byte reached the file, else -1.
 *
 * The file was emptied when it was opened, so a failure here leaves it cut
 * short: the outcome says so (no backup copy exists for a stream), and the
 * sentence the user reads says the file may be cut short.  When the stream
 * worked, the special bits the open found the file keeping are read again
 * from the file before it is closed (write_stream_confirm_stays).
 */
int wpe_write_stream_close(FILE *fp, WpeWriteOutcome *o)
{
 int err = 0;

 if (fflush(fp) != 0)
  err = write_error_number(errno);
 if (err == 0 && ferror(fp))
  err = EIO;
 if (err == 0)
  write_stream_confirm_stays(fp, o);
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
 case WPE_WRITE_THEN_NOTHING_MORE:
  return("");
 case WPE_WRITE_THEN_KEPT:
 default:
  return("Your text is still in the window.");
 }
}

/** Room for "the setuid, setgid and sticky bits were not kept: <reason>". */
#define WRITE_BITS_CLAUSE_MAX (WPE_PATH_SIZE + 128)

/**
 * write_bits_clause - Name the special bits a file did not keep, and why.
 * @out, @n: the room; @k: what was not kept.
 *
 * "the setgid bit was not kept: this user is not in group 12", with no
 * capital and no full stop, so the one sentence of a save and the one of a
 * failed save's restore can each carry it in their own place.
 */
static void write_bits_clause(char *out, size_t n, const WpeModeKept *k)
{
 char words[64], reason[WPE_PATH_SIZE];
 int several = ((k->lost & (k->lost - 1)) != 0);

 wpe_write_bits_words(words, sizeof words, k->lost);
 wpe_write_bits_reason(reason, sizeof reason, k);
 if (k->unsure)
  snprintf(out, n, "the %s may not have been kept: %s", words, reason);
 else
  snprintf(out, n, "the %s %s not kept: %s", words, several ? "were" : "was", reason);
}

/**
 * write_stays_clause - Name the special bits a copy still has, and why.
 * @out, @n: the room; @k: what stayed (lost) and the errno that refused to
 * drop it (err).
 *
 * "it keeps the sticky bit: Operation not permitted", with no capital and no
 * full stop, so it sits where the lost clause sits.  A bit that stayed with
 * no refused call says the system would not take it off.
 */
static void write_stays_clause(char *out, size_t n, const WpeModeKept *k)
{
 char words[64];
 int several = ((k->lost & (k->lost - 1)) != 0);
 const char *none = (several ? "the system did not take them off" : "the system did not take it off");

 wpe_write_bits_words(words, sizeof words, k->lost);
 snprintf(out, n, "it keeps the %s: %s", words, k->err != 0 ? strerror(k->err) : none);
}

/**
 * write_written_bits_clause - What a save that worked says about the special
 * bits of its file, or "" when there is nothing to say.
 * @out, @n: the room; @o: the outcome of a write that was WRITTEN.
 *
 * A Save says which bit it could not give back; a copy says which bit it
 * could not take off.  A write is one or the other, never both.
 */
static void write_written_bits_clause(char *out, size_t n, const WpeWriteOutcome *o)
{
 out[0] = '\0';
 if (o->file_kept.lost != 0)
  write_bits_clause(out, n, &o->file_kept);
 else if (o->file_stays.lost != 0)
  write_stays_clause(out, n, &o->file_stays);
}

/** Room for ", and the setuid and setgid bits it had are off". */
#define WRITE_OFF_CLAUSE_MAX 96

/**
 * write_bits_off_clause - Name the special bits a failed write left off a
 * file that had them.
 * @out, @n: the room; @off: the bits, as write_bits_found_off read them.
 *
 * ", and the setgid bit it had is off", or "" when there is none to name, so
 * the sentence of a failed write with no backup can end with it.  Said only
 * of bits read back from the file.
 */
static void write_bits_off_clause(char *out, size_t n, mode_t off)
{
 char words[64];
 int several = ((off & (off - 1)) != 0);

 out[0] = '\0';
 if (off == 0)
  return;
 wpe_write_bits_words(words, sizeof words, off);
 snprintf(out, n, ", and the %s it had %s off", words, several ? "are" : "is");
}

/**
 * write_kind_words - What a file that is not regular is called to the user.
 * @o: the outcome with not_regular set.
 * Return: "a terminal", "a pipe", "a socket", "a device", or the generic
 * list when the kind is not known.
 */
static const char *write_kind_words(const WpeWriteOutcome *o)
{
 if (o->not_regular_tty)
  return("a terminal");
 switch (o->not_regular_kind)
 {
 case S_IFIFO:
  return("a pipe");
 case S_IFSOCK:
  return("a socket");
 case S_IFCHR:
 case S_IFBLK:
  return("a device");
 default:
  return("a device, a terminal or a pipe");
 }
}

/**
 * write_failed_head - The first sentence of a failed write.
 * @out, @n: the room; @o: the outcome.
 *
 * A read-only window, a backup copy that stopped and a restore that failed
 * each say what the user must know about the file on disk; a restore that
 * put the old content back but not a special bit says which bit, so the
 * user knows the program is no longer one; every other failure says the
 * path and the reason.
 */
static void write_failed_head(char *out, size_t n, const WpeWriteOutcome *o)
{
 const char *path = (o->path[0] != '\0' ? o->path : "the file");
 char bits[WRITE_BITS_CLAUSE_MAX], off[WRITE_OFF_CLAUSE_MAX];

 if (o->read_only)
  snprintf(out, n, "%s is read-only.", write_base_name(path));
 else if (o->no_place)
  snprintf(out, n, "Cannot write %s: the editor has no folder of its own for it (no home "
           "folder is known).  Nothing was written.", write_base_name(path));
 else if (o->planted)
  snprintf(out, n, "Cannot write %s: something that is not a copy of ours is in its place.  "
           "Nothing was written.", write_base_name(path));
 else if (o->not_regular)
  snprintf(out, n, "Cannot write %s: it is not a regular file (%s).  Nothing was written.", path,
           write_kind_words(o));
 else if (o->vanished)
  snprintf(out, n, "Cannot write %s: the file was removed after the check.  Nothing was "
           "written.", path);
 else if (o->replaced_by_link)
  snprintf(out, n, "Cannot write %s: a link has taken the name.  Nothing was written.", path);
 else if (o->swapped)
  snprintf(out, n, "Cannot write %s: it is not the file that was checked; another file has taken "
           "the name since.  Nothing was written.", path);
 else if (o->gained_name)
  snprintf(out, n, "Cannot write %s: the file gained another name after the check.  Nothing was "
           "written.", path);
 else if (o->other_file && o->changed_while_asked)
  snprintf(out, n, "Cannot write %s: the name changed again while you were being asked, and now "
           "leads to yet another file.  Nothing was written.  Use Save As to write your text "
           "over that file.", path);
 else if (o->other_file)
  snprintf(out, n, "Cannot write %s: the name leads to a different file than the one this window "
           "read.  Nothing was written.  Use Save As to write your text over that file.",
           path);
 else if (o->replaced)
  snprintf(out, n, "Cannot write %s: it was replaced by another file while it was being saved.  "
           "Nothing was written.", path);
 else if (o->backup_stopped)
  snprintf(out, n, "Cannot write %s: the backup copy stopped at %s; the file was not touched.",
           path, strerror(o->err));
 else if (o->restore_err != 0 && o->bak_path[0] != '\0')
  snprintf(out, n, "Cannot write %s: %s.  The file on disk may be cut short; its previous "
           "content is in %s.", path, strerror(o->err), o->bak_path);
 else if (o->restore_err != 0)
 {
  write_bits_off_clause(off, sizeof off, o->file_off);
  snprintf(out, n, "Cannot write %s: %s.  The file on disk may be cut short; no backup copy "
           "was made%s.", path, strerror(o->err), off);
 }
 else if (o->restore_kept.lost != 0)
 {
  write_bits_clause(bits, sizeof bits, &o->restore_kept);
  snprintf(out, n, "Cannot write %s: %s.  The file holds its previous content, but %s.", path,
           strerror(o->err), bits);
 }
 else
  snprintf(out, n, "Cannot write %s: %s.", path, strerror(o->err));
}

/**
 * write_other_file_is_not_a_save - Is the caller one that has no text of the
 * user's to write over another file?
 * @then: what the caller keeps.
 * Return: 1 for the assistant's write and for a revert, 0 for Save and the rest.
 * Such a caller also adds no "your text is still in the window" to a failure.
 *
 * "Use Save As to write your text over that file" is advice to a user who
 * has the text in a window.  The assistant's write and the revert have no
 * such text, so they say what happened and that nothing was changed.
 */
static int write_other_file_is_not_a_save(WpeWriteThen then)
{
 return(then == WPE_WRITE_THEN_NOTHING_MORE || then == WPE_WRITE_THEN_PUT_BACK);
}

/**
 * write_other_file_sentence - The sentence for a name that leads to another
 * file, for a caller that has no text to keep.
 * @out, @n: the room; @o: the outcome (other_file set); @then: the caller.
 *
 * A revert says "Cannot put back t.c: the name now leads to a different file
 * than the one the window shows.  Nothing was changed."; the assistant's write
 * says the same about writing, and, when the user had said Yes about one file
 * and the name changed again, says so.
 */
static void write_other_file_sentence(char *out, size_t n, const WpeWriteOutcome *o,
                                      WpeWriteThen then)
{
 const char *path = (o->path[0] != '\0' ? o->path : "the file");

 if (then == WPE_WRITE_THEN_PUT_BACK)
  snprintf(out, n, "Cannot put back %s: the name now leads to a different file than the one the "
           "window shows.  Nothing was changed.", path);
 else if (o->changed_while_asked)
  snprintf(out, n, "Cannot write %s: the name changed again while you were being asked, and now "
           "leads to yet another file.  Nothing was written.", path);
 else
  snprintf(out, n, "Cannot write %s: the name now leads to a different file than the one the "
           "window shows, and it is not a plain file of yours without a special bit.  Nothing "
           "was written.", path);
}

/**
 * write_written_sentence - Say how a save that worked left the file, when
 * there is something to say.
 * @out, @n: the room; @o: the outcome.
 *
 * Two things can need saying, and one sentence says both so that neither
 * hides the other: a special bit the file did not keep (a setgid program
 * saved from the editor learns at once that it no longer is one, instead of
 * when it stops working) or, for a copy, one it could not take off (it keeps
 * the sticky bit of a file the user does not own), and the backup that could
 * not be made.  A save with neither says nothing, and a copy that went
 * elsewhere says where.
 */
static void write_written_sentence(char *out, size_t n, const WpeWriteOutcome *o)
{
 const char *base = write_base_name(o->path);
 char bits[WRITE_BITS_CLAUSE_MAX];

 write_written_bits_clause(bits, sizeof bits, o);
 if (o->backup_err == 0 && bits[0] == '\0')
 {
  if (o->kept)
   snprintf(out, n, "%s's copy could not go beside the file (%s); it is in %s.", base,
            strerror(o->err), o->kept_path);
  return;
 }
 if (bits[0] == '\0')
 {
  snprintf(out, n, "%s written; no backup was made: %s.", base, strerror(o->backup_err));
  return;
 }
 if (o->backup_err == 0)
  snprintf(out, n, "%s written, but %s.", base, bits);
 else
  snprintf(out, n, "%s written, but %s; no backup was made: %s.", base, bits,
           strerror(o->backup_err));
}

/**
 * wpe_write_written_clause - What a save that worked could not keep, as a clause.
 * @out, @n: the room; @o: the outcome of a write that was WRITTEN.
 * Return: its length; 0 (and an empty @out) when the file kept everything.
 *
 * "the setgid bit was not kept: Operation not permitted", and, joined by a
 * semicolon, "no backup was made: Permission denied": the words a caller puts
 * after "written, but" when it says the news in its own sentence (the
 * assistant's result to the model) instead of the owner's.
 */
int wpe_write_written_clause(char *out, size_t n, const WpeWriteOutcome *o)
{
 size_t used = 0;

 if (n == 0)
  return(0);
 out[0] = '\0';
 if (o->status != WPE_WRITE_WRITTEN)
  return(0);
 write_written_bits_clause(out, n, o);
 used = strlen(out);
 if (o->backup_err != 0)
  snprintf(out + used, n - used, "%sno backup was made: %s", used > 0 ? "; " : "",
           strerror(o->backup_err));
 return((int)strlen(out));
}

/**
 * wpe_write_sentence - The one sentence that tells the user how a write ended.
 * @out, @n: the room; @o: the outcome; @then: what the caller keeps.
 * Return: its length; 0 (and an empty @out) when there is nothing to say.
 *
 * A failure names the path and the reason and what is kept, so the user
 * knows whether the text is on disk and where it still is.  A save that
 * worked says something only when it made no backup or the file did not
 * keep a special bit it had, and then says both in one sentence.
 */
int wpe_write_sentence(char *out, size_t n, const WpeWriteOutcome *o, WpeWriteThen then)
{
 char head[WPE_WRITE_SENTENCE_MAX], name[WPE_PATH_SIZE + 64];

 if (n == 0)
  return(0);
 out[0] = '\0';
 if (o->status == WPE_WRITE_DECLINED)
  return(0);
 if (o->status == WPE_WRITE_WRITTEN)
 {
  write_written_sentence(out, n, o);
  return((int)strlen(out));
 }
 if (o->other_file && write_other_file_is_not_a_save(then))
 {
  write_other_file_sentence(out, n, o, then);
  return((int)strlen(out));
 }
 write_failed_head(head, sizeof head, o);
 if (write_other_file_is_not_a_save(then))
  snprintf(out, n, "%s", head);
 else
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
 * Save As keeps); @keep_name: the name the window had before a Save As, or
 * NULL for a plain Save.
 * Return: the WpeWriteStatus: the caller keeps the window, the name or the
 * build running only on WPE_WRITE_WRITTEN.
 *
 * A Save As that writes back onto the file the window read (the same name,
 * or a hard link to it) is a Save, and the setgid program stays one; onto
 * any other file it is a copy and gives that file no special bit.  A plain
 * Save onto a name that now leads to another file asks (a plain file of the
 * same owner) or refuses, so it never writes into a file the window did not
 * read without the user knowing.
 */
int wpe_write_save(FENSTER *f, WpeWriteThen then, const char *keep_name)
{
 WpeWriteOutcome o;

 if (write_window_file(f, keep_name == NULL, &o) != 0 && keep_name != NULL)
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
 return(wpe_write_save(f, WPE_WRITE_THEN_KEPT, NULL));
}

