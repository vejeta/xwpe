/* we_fileop.c -- the one owner of "copy, move, link or remove a user's
   file, and what happens when it fails".
   Copyright (C) 1993 Fred Kruse
   Copyright (C) 2026 Juan Manuel Mendez Rey
   This is free software; you can redistribute it and/or modify it under the
   terms of the GNU General Public License, see the file COPYING.

   The File Manager's Copy, Move, Link and Remove of a file go through here
   and nothing else spells rename, link, unlink or mkdir for the user's
   files (tests/check-write-owner.sh).  The destination is made as a new
   file under a temporary name in its own folder and reaches its name by
   one rename, so a link in its place is replaced and never written
   through, and a copy that stops leaves nothing behind.  A name that is
   not to be replaced is put in place by a call that cannot replace it.
   The bytes are the write owner's (we_write.c).  No drawing, no key reads:
   the questions go through the policy's callback. */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "we_write.h"
#include "we_fileop.h"

/** The longest name a file system takes on every system built on. */
#define FILEOP_NAME_LIMIT 255

/** How many temporary names, and how many wastebasket names, are tried. */
#define FILEOP_TRIES 1000

/** XWPE_TEST_FAIL: the names that make one step fail, for the tests that
    show what the user is told and that nothing is lost. */
#define FILEOP_TEST_FAIL_ENV       "XWPE_TEST_FAIL"
#define FILEOP_HOOK_CLOSE          "fileop-close"
#define FILEOP_HOOK_RENAME         "fileop-rename"
#define FILEOP_HOOK_REMOVE_SOURCE  "fileop-remove-source"
#define FILEOP_HOOK_PLANT          "fileop-plant"
#define FILEOP_HOOK_NO_RENAMEAT2   "fileop-no-renameat2"
#define FILEOP_HOOK_NO_LINK        "fileop-no-link"

/** What the planting hook puts at the destination: a link to nowhere. */
#define FILEOP_PLANTED_TARGET "planted-target"

/* --- the outcome ---------------------------------------------------------- */

/** fileop_test_hook - Is the test hook @name among the comma separated
 *  names in the environment? */
static int fileop_test_hook(const char *name)
{
 const char *v = getenv(FILEOP_TEST_FAIL_ENV);
 size_t len = strlen(name);

 while (v != NULL && *v != '\0')
 {
  if (strncmp(v, name, len) == 0 && (v[len] == '\0' || v[len] == ','))
   return(1);
  v = strchr(v, ',');
  if (v != NULL)
   v++;
 }
 return(0);
}

/** fileop_begin - Start an outcome: nothing done, everything as it was. */
static void fileop_begin(WpeFileOpOutcome *o, WpeFileOpKind kind, const char *src, const char *dst)
{
 memset(o, 0, sizeof *o);
 o->status = WPE_FILEOP_DONE;
 o->kind = kind;
 o->src_kept = 1;
 o->dst_kept = 1;
 snprintf(o->src, sizeof o->src, "%s", src != NULL ? src : "");
 snprintf(o->dst, sizeof o->dst, "%s", dst != NULL ? dst : "");
}

/** fileop_fail - Record that a step failed.  Return: -1. */
static int fileop_fail(WpeFileOpOutcome *o, WpeFileOpStep step, int err)
{
 o->status = WPE_FILEOP_FAILED;
 o->step = step;
 o->err = err;
 return(-1);
}

/** fileop_decline - Record that nothing was done and nothing is wrong.  Return: -1. */
static int fileop_decline(WpeFileOpOutcome *o, WpeFileOpStep step)
{
 o->status = WPE_FILEOP_DECLINED;
 o->step = step;
 return(-1);
}

/** fileop_not_a_file - Record that @path is not a file one may copy over or from. */
static int fileop_not_a_file(WpeFileOpOutcome *o, const char *path)
{
 snprintf(o->bad, sizeof o->bad, "%s", path);
 return(fileop_fail(o, WPE_FILEOP_STEP_NOT_A_FILE, 0));
}

/* --- names ---------------------------------------------------------------- */

/**
 * fileop_split - Cut a path into its folder and its last name.
 * @path: the path; @dir, @name: receive the parts.
 * Return: 0, or the errno (EINVAL for a path with no name, ENAMETOOLONG).
 *
 * "a/b/" is the folder a and the name b; "b" is the folder "." and the
 * name b; "/b" is the folder "/".
 */
static int fileop_split(const char *path, char *dir, size_t dn, char *name, size_t nn)
{
 size_t len = strlen(path), cut;

 while (len > 1 && path[len - 1] == '/')
  len--;
 if (len == 0 || (len == 1 && path[0] == '/'))
  return(EINVAL);
 for (cut = len; cut > 0 && path[cut - 1] != '/'; cut--)
  ;
 if (len - cut >= nn)
  return(ENAMETOOLONG);
 memcpy(name, path + cut, len - cut);
 name[len - cut] = '\0';
 if (cut == 0)
 {
  if (dn < 2)
   return(ENAMETOOLONG);
  memcpy(dir, ".", 2);
  return(0);
 }
 while (cut > 1 && path[cut - 1] == '/')
  cut--;
 if (cut >= dn)
  return(ENAMETOOLONG);
 memcpy(dir, path, cut);
 dir[cut] = '\0';
 return(0);
}

/** fileop_temp_name - A name for the copy inside the destination's folder.
 *  @n: the try; the name is hidden, says what it is, and fits NAME_MAX:
 *  a very long destination name falls back to one without it. */
static int fileop_temp_name(char *out, size_t size, const char *name, int n)
{
 int len = snprintf(out, size, ".%s.xwpe-copy.%ld.%d", name, (long)getpid(), n);

 if (len < 0 || len > FILEOP_NAME_LIMIT)
  len = snprintf(out, size, ".xwpe-copy.%ld.%d", (long)getpid(), n);
 return(len < 0 || (size_t)len >= size ? ENAMETOOLONG : 0);
}

/* --- the way a name is put where none is ---------------------------------- */

/**
 * fileop_rename_noreplace_at - Rename without replacing what is at the target.
 * @fromfd, @from, @tofd, @to: as renameat.
 * Return: 0, or the errno; EEXIST when the target exists, EXDEV across file
 * systems.
 *
 * A name that appears between the check and the rename must not be lost, so
 * the call itself refuses to replace it: renameat2 with RENAME_NOREPLACE
 * (Linux) or RENAME_EXCL (macOS) where the file system takes it, else a
 * hard link and an unlink of the old name, else an lstat followed by a
 * plain rename for the file systems that have neither (vfat, some FUSE).
 */
static int fileop_rename_noreplace_at(int fromfd, const char *from, int tofd, const char *to)
{
 struct stat st;
 int err;

#if defined(HAVE_RENAMEAT2_NOREPLACE)
 if (!fileop_test_hook(FILEOP_HOOK_NO_RENAMEAT2))
 {
  if (renameat2(fromfd, from, tofd, to, RENAME_NOREPLACE) == 0)
   return(0);
  err = errno;
  if (err != EINVAL && err != ENOSYS && err != EOPNOTSUPP && err != ENOTSUP)
   return(err);
 }
#elif defined(HAVE_RENAMEATX_NP_EXCL)
 if (!fileop_test_hook(FILEOP_HOOK_NO_RENAMEAT2))
 {
  if (renameatx_np(fromfd, from, tofd, to, RENAME_EXCL) == 0)
   return(0);
  err = errno;
  if (err != EINVAL && err != ENOSYS && err != EOPNOTSUPP && err != ENOTSUP)
   return(err);
 }
#endif
 if (!fileop_test_hook(FILEOP_HOOK_NO_LINK))
 {
  if (linkat(fromfd, from, tofd, to, 0) == 0)
  {
   /* both names hold the file; the old one staying is not a loss */
   (void)unlinkat(fromfd, from, 0);
   return(0);
  }
  err = errno;
  if (err != EPERM && err != EOPNOTSUPP && err != ENOTSUP && err != EMLINK && err != ENOSYS)
   return(err);
 }
 if (fstatat(tofd, to, &st, AT_SYMLINK_NOFOLLOW) == 0)
  return(EEXIST);
 if (errno != ENOENT)
  return(errno);
 if (renameat(fromfd, from, tofd, to) != 0)
  return(errno);
 return(0);
}

/* --- one operation's state ------------------------------------------------ */

typedef struct {
 WpeFileOpOutcome       *o;
 const WpeFileOpPolicy  *p;
 char                    src[WPE_PATH_SIZE];
 char                    dst[WPE_PATH_SIZE];   /* with a destination folder resolved */
 char                    dir[WPE_PATH_SIZE];   /* the destination's folder */
 char                    name[WPE_NAME_SIZE];  /* and its name in it */
 int                     dirfd;
 struct stat             sst;                  /* the source */
 int                     dst_exists;
 int                     replace;              /* the existing destination is to be replaced */
} FileJob;

/** fileop_job_begin - Start an operation: the outcome, the policy and the source. */
static void fileop_job_begin(FileJob *j, WpeFileOpKind kind, const char *src, const char *dst,
                             const WpeFileOpPolicy *p, WpeFileOpOutcome *o)
{
 memset(j, 0, sizeof *j);
 j->dirfd = -1;
 j->o = o;
 j->p = p;
 snprintf(j->src, sizeof j->src, "%s", src);
 snprintf(j->dst, sizeof j->dst, "%s", dst);
 fileop_begin(o, kind, src, dst);
}

/** fileop_job_end - Close what the operation holds. */
static void fileop_job_end(FileJob *j)
{
 if (j->dirfd >= 0)
  close(j->dirfd);
 j->dirfd = -1;
}

/**
 * fileop_job_ask_replace - Ask once whether the existing destination goes.
 * Return: 0 to go on, -1 when the answer ended the operation (outcome set).
 */
static int fileop_job_ask_replace(FileJob *j)
{
 char question[2 * WPE_PATH_SIZE];
 int answer = WPE_FILEOP_ANSWER_NO;

 snprintf(question, sizeof question, "File %s exists.\nReplace it?", j->dst);
 if (j->p->ask != NULL)
  answer = j->p->ask(j->p->ctx, question);
 if (answer == WPE_FILEOP_ANSWER_STOP)
  return(fileop_fail(j->o, WPE_FILEOP_STEP_CANCELLED, 0));
 if (answer != WPE_FILEOP_ANSWER_YES)
  return(fileop_decline(j->o, WPE_FILEOP_STEP_NONE));
 return(0);
}

/**
 * fileop_job_resolve_dst - A destination that is a folder takes the file inside.
 * Return: 0, or -1 (outcome set).
 *
 * As cp, mv and Midnight Commander do: naming a folder as the place for a
 * file puts the file in it under its own name, and never replaces the
 * folder.  A link to a folder is a name, and is replaced like any other.
 */
static int fileop_job_resolve_dst(FileJob *j)
{
 struct stat st;
 char base[WPE_NAME_SIZE], folder[WPE_PATH_SIZE], joined[WPE_PATH_SIZE];
 int err;

 if (lstat(j->dst, &st) != 0 || !S_ISDIR(st.st_mode))
  return(0);
 err = fileop_split(j->src, folder, sizeof folder, base, sizeof base);
 if (err == 0 && wpe_path_join(joined, sizeof joined, j->dst, base) != 0)
  err = ENAMETOOLONG;
 if (err != 0)
  return(fileop_fail(j->o, WPE_FILEOP_STEP_CREATE, err));
 snprintf(j->dst, sizeof j->dst, "%s", joined);
 snprintf(j->o->dst, sizeof j->o->dst, "%s", joined);
 return(0);
}

/**
 * fileop_job_open_dst - Decide what the destination is and whether it goes.
 * Return: 0 to go on, -1 when the operation ended (outcome set).
 *
 * Opens the destination's folder once and works through it from here on.
 * The same file as the source, a device, a pipe or a socket in the way,
 * "Don't overwrite" and the user's answer end the operation before
 * anything is written.
 */
static int fileop_job_open_dst(FileJob *j)
{
 struct stat st;
 int err;

 if (fileop_job_resolve_dst(j) != 0)
  return(-1);
 err = fileop_split(j->dst, j->dir, sizeof j->dir, j->name, sizeof j->name);
 if (err == 0)
 {
  j->dirfd = open(j->dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (j->dirfd < 0)
   err = errno;
 }
 if (err != 0)
  return(fileop_fail(j->o, WPE_FILEOP_STEP_CREATE, err));
 if (fstatat(j->dirfd, j->name, &st, AT_SYMLINK_NOFOLLOW) != 0)
 {
  if (errno != ENOENT)
   return(fileop_fail(j->o, WPE_FILEOP_STEP_CREATE, errno));
  return(0);
 }
 j->dst_exists = 1;
 if (st.st_dev == j->sst.st_dev && st.st_ino == j->sst.st_ino)
  return(fileop_fail(j->o, WPE_FILEOP_STEP_SAME_FILE, 0));
 if (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode))
  return(fileop_not_a_file(j->o, j->dst));
 if (j->p->replace == WPE_REPLACE_NEVER)
  return(fileop_decline(j->o, WPE_FILEOP_STEP_EXISTS));
 if (j->p->replace == WPE_REPLACE_ASK && fileop_job_ask_replace(j) != 0)
  return(-1);
 j->replace = 1;
 return(0);
}

/* --- links and the wastebasket -------------------------------------------- */

/** fileop_link_text - What the link at @path points to.  Return: 0 or the errno. */
static int fileop_link_text(const char *path, char *out, size_t n)
{
 ssize_t got = readlink(path, out, n - 1);

 if (got < 0)
  return(errno);
 if ((size_t)got >= n - 1)
  return(ENAMETOOLONG);
 out[got] = '\0';
 return(0);
}

/** fileop_copy_link - Make the link @path again as @name in a folder, with the same text. */
static int fileop_copy_link(const char *path, int dirfd, const char *name)
{
 char text[WPE_PATH_SIZE];
 int err = fileop_link_text(path, text, sizeof text);

 if (err != 0)
  return(err);
 if (symlinkat(text, dirfd, name) != 0)
  return(errno);
 return(0);
}

/**
 * fileop_wastebasket_name - The try-th name for @name in the wastebasket:
 * `name`, `name.1`, `name.2`, ...
 */
static int fileop_wastebasket_name(char *out, size_t n, const char *wb, const char *name, int try)
{
 char leaf[WPE_NAME_SIZE + 16];

 if (try == 0)
  snprintf(leaf, sizeof leaf, "%s", name);
 else
  snprintf(leaf, sizeof leaf, "%s.%d", name, try);
 return(wpe_path_join(out, n, wb, leaf));
}

int wpe_fileop_wastebasket(char *out, size_t n, WpeFileOpOutcome *o)
{
 const char *home = getenv("HOME");
 char path[WPE_PATH_SIZE];
 struct stat st;
 int fd;

 fileop_begin(o, WPE_FILEOP_MKDIR, "", "");
 if (home == NULL || home[0] == '\0')
  return(fileop_fail(o, WPE_FILEOP_STEP_WASTEBASKET, ENOENT));
 if (wpe_path_join(path, sizeof path, home, WPE_FILEOP_WASTEBASKET) != 0 || strlen(path) >= n)
  return(fileop_fail(o, WPE_FILEOP_STEP_WASTEBASKET, ENAMETOOLONG));
 snprintf(o->src, sizeof o->src, "%s", path);
 if (mkdir(path, 0700) != 0 && errno != EEXIST)
  return(fileop_fail(o, WPE_FILEOP_STEP_WASTEBASKET, errno));
 fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
 if (fd < 0)
  return(fileop_fail(o, WPE_FILEOP_STEP_WASTEBASKET, errno));
 if (fstat(fd, &st) != 0 || st.st_uid != geteuid())
 {
  close(fd);
  return(fileop_fail(o, WPE_FILEOP_STEP_WASTEBASKET, EPERM));
 }
 if ((st.st_mode & 077) != 0 && fchmod(fd, 0700) != 0)
 {
  int err = errno;

  close(fd);
  return(fileop_fail(o, WPE_FILEOP_STEP_WASTEBASKET, err));
 }
 close(fd);
 if (access(path, R_OK | W_OK | X_OK) != 0)
  return(fileop_fail(o, WPE_FILEOP_STEP_WASTEBASKET, errno));
 snprintf(out, n, "%s", path);
 return(0);
}

int wpe_fileop_wastebasket_slot(const char *path, char *out, size_t n, WpeFileOpOutcome *o)
{
 char wb[WPE_PATH_SIZE], dir[WPE_PATH_SIZE], name[WPE_NAME_SIZE];
 struct stat st;
 int err, i;

 if (wpe_fileop_wastebasket(wb, sizeof wb, o) != 0)
  return(-1);
 err = fileop_split(path, dir, sizeof dir, name, sizeof name);
 if (err != 0)
  return(fileop_fail(o, WPE_FILEOP_STEP_WASTEBASKET, err));
 for (i = 0; i < FILEOP_TRIES; i++)
 {
  if (fileop_wastebasket_name(out, n, wb, name, i) != 0)
   return(fileop_fail(o, WPE_FILEOP_STEP_WASTEBASKET, ENAMETOOLONG));
  if (lstat(out, &st) != 0 && errno == ENOENT)
   return(0);
 }
 return(fileop_fail(o, WPE_FILEOP_STEP_WASTEBASKET, EEXIST));
}

int wpe_fileop_in_wastebasket(const char *path)
{
 char wb[WPE_PATH_SIZE], dir[WPE_PATH_SIZE], name[WPE_NAME_SIZE];
 char real_wb[WPE_PATH_SIZE], real_dir[WPE_PATH_SIZE];
 WpeFileOpOutcome o;
 size_t len;

 if (wpe_fileop_wastebasket(wb, sizeof wb, &o) != 0)
  return(0);
 if (realpath(wb, real_wb) == NULL)
  return(0);
 if (realpath(path, real_dir) != NULL && strcmp(real_dir, real_wb) == 0)
  return(1);
 if (fileop_split(path, dir, sizeof dir, name, sizeof name) != 0)
  return(0);
 if (realpath(dir, real_dir) == NULL)
  return(0);
 len = strlen(real_wb);
 return(strncmp(real_dir, real_wb, len) == 0 && (real_dir[len] == '\0' || real_dir[len] == '/'));
}

/**
 * fileop_job_stash - Keep the destination about to be replaced in the wastebasket.
 * @aside: receives the name it was kept under.
 * Return: 0, or the errno.
 *
 * The old file is given a second name there before the new one takes its
 * name (a link, or a copy when the wastebasket is on another file system),
 * so that a failed replacement can drop that name again and leave
 * everything as it was.  Never over an earlier file of the same name.
 */
static int fileop_job_stash(FileJob *j, char *aside, size_t n)
{
 char wb[WPE_PATH_SIZE];
 WpeFileOpOutcome w, c;
 WpeFileOpPolicy inner;
 int i, err = 0;

 if (wpe_fileop_wastebasket(wb, sizeof wb, &w) != 0)
  return(w.err != 0 ? w.err : EIO);
 for (i = 0; i < FILEOP_TRIES; i++)
 {
  if (fileop_wastebasket_name(aside, n, wb, j->name, i) != 0)
   return(ENAMETOOLONG);
  if (linkat(j->dirfd, j->name, AT_FDCWD, aside, 0) == 0)
   return(0);
  err = errno;
  if (err != EEXIST)
   break;
 }
 if (err == EEXIST)
  return(err);
 if (wpe_fileop_wastebasket_slot(j->dst, aside, n, &w) != 0)
  return(w.err != 0 ? w.err : EIO);
 memset(&inner, 0, sizeof inner);
 inner.replace = WPE_REPLACE_NEVER;
 inner.remove = WPE_REMOVE_NOW;
 {
  struct stat st;

  if (fstatat(j->dirfd, j->name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode))
   return(fileop_copy_link(j->dst, AT_FDCWD, aside));
 }
 if (wpe_fileop_copy(j->dst, aside, &inner, &c) != 0)
  return(c.err != 0 ? c.err : EIO);
 return(0);
}

/**
 * fileop_job_place - Put a finished name at the destination.
 * @fromfd, @from: the copy under its temporary name, or a Move's source;
 * @is_temp: @from is the temporary and is removed when this fails.
 * Return: 0 when placed; 1 when @from is on another file system (nothing
 * changed, the caller copies); -1 when the operation ended (outcome set).
 *
 * An existing destination that is to be replaced is replaced by the rename
 * itself, atomically, after a second name for it has been made in the
 * wastebasket when the Remove option says so; nothing ever removes it
 * first, so a failed rename leaves it where it was.  A destination that is
 * not to be replaced is put in place by a call that cannot replace it.
 */
static int fileop_job_place(FileJob *j, int fromfd, const char *from, int is_temp)
{
 WpeFileOpOutcome *o = j->o;
 char aside[WPE_PATH_SIZE];
 int err, stashed = 0;

 aside[0] = '\0';
 if (j->replace)
 {
  if (j->p->remove == WPE_REMOVE_WASTEBASKET)
  {
   err = fileop_job_stash(j, aside, sizeof aside);
   if (err != 0)
   {
    if (is_temp)
     unlinkat(fromfd, from, 0);
    return(fileop_fail(o, WPE_FILEOP_STEP_REPLACE, err));
   }
   stashed = 1;
  }
  if (fileop_test_hook(FILEOP_HOOK_RENAME))
   err = EIO;
  else
   err = (renameat(fromfd, from, j->dirfd, j->name) == 0 ? 0 : errno);
 }
 else
 {
  if (fileop_test_hook(FILEOP_HOOK_PLANT))
   (void)symlinkat(FILEOP_PLANTED_TARGET, j->dirfd, j->name);
  if (fileop_test_hook(FILEOP_HOOK_RENAME))
   err = EIO;
  else
   err = fileop_rename_noreplace_at(fromfd, from, j->dirfd, j->name);
 }
 if (err == 0)
 {
  if (stashed)
   snprintf(o->aside, sizeof o->aside, "%s", aside);
  o->dst_kept = (j->replace ? 0 : 1);
  return(0);
 }
 if (stashed && unlink(aside) != 0)
  snprintf(o->aside, sizeof o->aside, "%s", aside);
 if (err == EXDEV && !is_temp)
  return(1);
 if (is_temp)
  unlinkat(fromfd, from, 0);
 if (err == EEXIST && !j->replace && j->p->replace == WPE_REPLACE_NEVER)
  return(fileop_decline(o, WPE_FILEOP_STEP_EXISTS));
 return(fileop_fail(o, WPE_FILEOP_STEP_RENAME, err));
}

/* --- the copy ------------------------------------------------------------- */

/**
 * fileop_open_source - Open the file to copy from, whole or not at all.
 * @fd: receives the descriptor.
 * Return: 0, or -1 (outcome set).
 *
 * Opened without blocking so a pipe named as the source is refused at once
 * instead of hanging the editor, then put back to blocking reads.
 */
static int fileop_open_source(FileJob *j, int *fd)
{
 int flags;

 *fd = open(j->src, O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
 if (*fd < 0)
  return(fileop_fail(j->o, WPE_FILEOP_STEP_SOURCE, errno));
 if (fstat(*fd, &j->sst) != 0)
 {
  int err = errno;

  close(*fd);
  return(fileop_fail(j->o, WPE_FILEOP_STEP_SOURCE, err));
 }
 if (!S_ISREG(j->sst.st_mode))
 {
  close(*fd);
  return(fileop_not_a_file(j->o, j->src));
 }
 flags = fcntl(*fd, F_GETFL);
 if (flags >= 0)
  fcntl(*fd, F_SETFL, flags & ~O_NONBLOCK);
 return(0);
}

/**
 * fileop_job_make_temp - Create the copy's file, new and private.
 * @tmp: receives its name in the destination's folder; @fd: its descriptor.
 * Return: 0, or -1 (outcome set).
 */
static int fileop_job_make_temp(FileJob *j, char *tmp, size_t n, int *fd)
{
 static int serial;
 int i, err;

 for (i = 0; i < FILEOP_TRIES; i++)
 {
  err = fileop_temp_name(tmp, n, j->name, serial++);
  if (err == 0)
   err = wpe_write_create_private(j->dirfd, tmp, j->sst.st_mode, fd);
  if (err == 0)
   return(0);
  if (err != EEXIST)
   break;
 }
 return(fileop_fail(j->o, WPE_FILEOP_STEP_CREATE, err));
}

/**
 * fileop_job_fill_temp - Write the source into the copy and finish it.
 * @durable: the data is flushed to disk before the close.
 * Return: 0, or -1 (outcome set; the copy is removed by the caller).
 *
 * Every write, the flush and the close are judged: the copy counts as made
 * only when each returned 0.  The special permission bits go on after the
 * data, because the kernel clears them on the first write.
 */
static int fileop_job_fill_temp(FileJob *j, int sfd, int fd, int durable)
{
 int err, read_failed;

 wpe_write_guard_enter();
 err = wpe_write_copy_fd(sfd, fd, &read_failed);
 if (err != 0)
 {
  close(fd);
  wpe_write_guard_leave();
  return(fileop_fail(j->o, read_failed ? WPE_FILEOP_STEP_SOURCE : WPE_FILEOP_STEP_WRITE, err));
 }
 if (durable && fdatasync(fd) != 0)
 {
  err = errno;
  close(fd);
  wpe_write_guard_leave();
  return(fileop_fail(j->o, WPE_FILEOP_STEP_WRITE, err));
 }
 err = wpe_write_set_special_bits(fd, j->sst.st_mode);
 if (err != 0)
 {
  close(fd);
  wpe_write_guard_leave();
  return(fileop_fail(j->o, WPE_FILEOP_STEP_MODE, err));
 }
 err = (close(fd) == 0 ? 0 : errno);
 if (err == 0 && fileop_test_hook(FILEOP_HOOK_CLOSE))
  err = EIO;
 wpe_write_guard_leave();
 if (err != 0)
  return(fileop_fail(j->o, WPE_FILEOP_STEP_CLOSE, err));
 return(0);
}

/**
 * fileop_job_copy_in - Copy an open source into the destination.
 * Return: 0, or -1 (outcome set, no half copy left).
 */
static int fileop_job_copy_in(FileJob *j, int sfd, int durable)
{
 char tmp[WPE_NAME_SIZE];
 int fd, rc;

 if (fileop_job_make_temp(j, tmp, sizeof tmp, &fd) != 0)
  return(-1);
 if (fileop_job_fill_temp(j, sfd, fd, durable) != 0)
 {
  unlinkat(j->dirfd, tmp, 0);
  return(-1);
 }
 rc = fileop_job_place(j, j->dirfd, tmp, 1);
 return(rc == 0 ? 0 : -1);
}

int wpe_fileop_copy(const char *src, const char *dst, const WpeFileOpPolicy *p, WpeFileOpOutcome *o)
{
 FileJob j;
 int sfd, rc;

 fileop_job_begin(&j, WPE_FILEOP_COPY, src, dst, p, o);
 if (fileop_open_source(&j, &sfd) != 0)
  return(-1);
 if (fileop_job_open_dst(&j) != 0)
 {
  close(sfd);
  fileop_job_end(&j);
  return(-1);
 }
 rc = fileop_job_copy_in(&j, sfd, 0);
 close(sfd);
 fileop_job_end(&j);
 return(rc);
}

/* --- the move ------------------------------------------------------------- */

/**
 * fileop_job_copy_link_in - Make the source link again at the destination.
 * Return: 0, or -1 (outcome set).
 */
static int fileop_job_copy_link_in(FileJob *j)
{
 char tmp[WPE_NAME_SIZE];
 static int serial;
 int i, err = EEXIST;

 for (i = 0; i < FILEOP_TRIES && err == EEXIST; i++)
 {
  err = fileop_temp_name(tmp, sizeof tmp, j->name, serial++);
  if (err == 0)
   err = fileop_copy_link(j->src, j->dirfd, tmp);
 }
 if (err != 0)
  return(fileop_fail(j->o, WPE_FILEOP_STEP_CREATE, err));
 return(fileop_job_place(j, j->dirfd, tmp, 1) == 0 ? 0 : -1);
}

/**
 * fileop_job_finish_move - After a copy across file systems: make the
 * name durable, then remove the source.
 * Return: 0, or -1 (outcome set; the copy stays and the source is in place).
 *
 * The only copy of the data is about to change hands, so the folder that
 * holds the new name is flushed first.
 */
static int fileop_job_finish_move(FileJob *j)
{
 WpeFileOpOutcome *o = j->o;

 o->dst_kept = 0;
 if (fsync(j->dirfd) != 0)
  return(fileop_fail(o, WPE_FILEOP_STEP_CLOSE, errno));
 if (fileop_test_hook(FILEOP_HOOK_REMOVE_SOURCE))
  return(fileop_fail(o, WPE_FILEOP_STEP_REMOVE_SOURCE, EACCES));
 if (unlinkat(AT_FDCWD, j->src, 0) != 0)
  return(fileop_fail(o, WPE_FILEOP_STEP_REMOVE_SOURCE, errno));
 o->src_kept = 0;
 return(0);
}

/**
 * fileop_job_move_across - Move to another file system: copy, then remove.
 * Return: 0 or -1 (outcome set).
 */
static int fileop_job_move_across(FileJob *j)
{
 int sfd, rc;

 if (S_ISLNK(j->sst.st_mode))
  rc = fileop_job_copy_link_in(j);
 else if (fileop_open_source(j, &sfd) != 0)
  return(-1);
 else
 {
  rc = fileop_job_copy_in(j, sfd, 1);
  close(sfd);
 }
 if (rc != 0)
  return(-1);
 return(fileop_job_finish_move(j));
}

int wpe_fileop_move(const char *src, const char *dst, const WpeFileOpPolicy *p, WpeFileOpOutcome *o)
{
 FileJob j;
 int rc;

 fileop_job_begin(&j, WPE_FILEOP_MOVE, src, dst, p, o);
 if (lstat(j.src, &j.sst) != 0)
  return(fileop_fail(o, WPE_FILEOP_STEP_SOURCE, errno));
 if (S_ISDIR(j.sst.st_mode))
  return(fileop_not_a_file(o, j.src));
 if (fileop_job_open_dst(&j) != 0)
 {
  fileop_job_end(&j);
  return(-1);
 }
 rc = fileop_job_place(&j, AT_FDCWD, j.src, 0);
 if (rc == 0)
  o->src_kept = 0;
 else if (rc == 1)
  rc = fileop_job_move_across(&j);
 fileop_job_end(&j);
 return(rc == 0 ? 0 : -1);
}

/* --- the link ------------------------------------------------------------- */

/**
 * fileop_try_link - One try at the link under one temporary name.
 * Return: 0, EEXIST when the name is taken, else the errno of the symbolic
 * link that was the last resort.
 */
static int fileop_try_link(FileJob *j, const char *tmp)
{
 if (j->p->hardlink_first)
 {
  if (linkat(AT_FDCWD, j->src, j->dirfd, tmp, 0) == 0)
   return(0);
  if (errno == EEXIST)
   return(EEXIST);
  j->o->err_link = errno;
 }
 if (symlinkat(j->src, j->dirfd, tmp) != 0)
  return(errno);
 return(0);
}

/**
 * fileop_job_make_link - Make the link under a temporary name in the
 * destination's folder.
 * @tmp: receives the name.
 * Return: 0, or -1 (outcome set).
 *
 * A hard link first when the policy asks for it; when that cannot be made
 * a symbolic link whose text is the source as the user named it.  Both
 * errors are kept, so the sentence can say why the hard link was not made.
 */
static int fileop_job_make_link(FileJob *j, char *tmp, size_t n)
{
 static int serial;
 int i, err = EEXIST;

 for (i = 0; i < FILEOP_TRIES; i++)
 {
  err = fileop_temp_name(tmp, n, j->name, serial++);
  if (err == 0)
   err = fileop_try_link(j, tmp);
  if (err != EEXIST)
   break;
 }
 if (err != 0)
  return(fileop_fail(j->o, WPE_FILEOP_STEP_LINK, err));
 return(0);
}

int wpe_fileop_link(const char *src, const char *dst, const WpeFileOpPolicy *p, WpeFileOpOutcome *o)
{
 FileJob j;
 char tmp[WPE_NAME_SIZE];
 int rc;

 fileop_job_begin(&j, WPE_FILEOP_LINK, src, dst, p, o);
 if (lstat(j.src, &j.sst) != 0)
  return(fileop_fail(o, WPE_FILEOP_STEP_SOURCE, errno));
 if (fileop_job_open_dst(&j) != 0)
 {
  fileop_job_end(&j);
  return(-1);
 }
 rc = fileop_job_make_link(&j, tmp, sizeof tmp);
 if (rc == 0)
  rc = fileop_job_place(&j, j.dirfd, tmp, 1);
 fileop_job_end(&j);
 return(rc == 0 ? 0 : -1);
}

/* --- remove, folders, permissions ----------------------------------------- */

/**
 * fileop_remove_into_wastebasket - Move a file into the wastebasket under a free name.
 * Return: 0, or -1 (outcome set).
 */
static int fileop_remove_into_wastebasket(const char *path, WpeFileOpOutcome *o)
{
 char slot[WPE_PATH_SIZE];
 WpeFileOpOutcome inner;
 WpeFileOpPolicy move;

 if (wpe_fileop_wastebasket_slot(path, slot, sizeof slot, &inner) != 0)
  return(fileop_fail(o, WPE_FILEOP_STEP_WASTEBASKET, inner.err));
 memset(&move, 0, sizeof move);
 move.replace = WPE_REPLACE_NEVER;
 move.remove = WPE_REMOVE_NOW;
 snprintf(o->dst, sizeof o->dst, "%s", slot);
 if (wpe_fileop_move(path, slot, &move, &inner) != 0)
  return(fileop_fail(o, WPE_FILEOP_STEP_WASTEBASKET, inner.err));
 o->src_kept = 0;
 return(0);
}

int wpe_fileop_remove(const char *path, const WpeFileOpPolicy *p, WpeFileOpOutcome *o)
{
 struct stat st;
 char question[WPE_PATH_SIZE + 32];
 int answer;

 fileop_begin(o, WPE_FILEOP_REMOVE, path, "");
 if (lstat(path, &st) != 0)
  return(fileop_fail(o, WPE_FILEOP_STEP_SOURCE, errno));
 if (S_ISDIR(st.st_mode))
  return(fileop_not_a_file(o, path));
 if (p->remove == WPE_REMOVE_WASTEBASKET && !wpe_fileop_in_wastebasket(path))
  return(fileop_remove_into_wastebasket(path, o));
 if (p->remove == WPE_REMOVE_ASK)
 {
  snprintf(question, sizeof question, "Remove File:\n%s", path);
  answer = (p->ask != NULL ? p->ask(p->ctx, question) : WPE_FILEOP_ANSWER_NO);
  if (answer == WPE_FILEOP_ANSWER_STOP)
   return(fileop_fail(o, WPE_FILEOP_STEP_CANCELLED, 0));
  if (answer != WPE_FILEOP_ANSWER_YES)
   return(fileop_decline(o, WPE_FILEOP_STEP_NONE));
 }
 if (unlink(path) != 0)
  return(fileop_fail(o, WPE_FILEOP_STEP_REMOVE, errno));
 o->src_kept = 0;
 return(0);
}

int wpe_fileop_mkdir(const char *path, mode_t mode, WpeFileOpOutcome *o)
{
 fileop_begin(o, WPE_FILEOP_MKDIR, path, "");
 if (mkdir(path, mode) != 0)
  return(fileop_fail(o, WPE_FILEOP_STEP_MKDIR, errno));
 return(0);
}

int wpe_fileop_rmdir(const char *path, WpeFileOpOutcome *o)
{
 fileop_begin(o, WPE_FILEOP_RMDIR, path, "");
 if (rmdir(path) != 0)
  return(fileop_fail(o, WPE_FILEOP_STEP_RMDIR, errno));
 return(0);
}

int wpe_fileop_chmod(const char *path, mode_t perm, WpeFileOpOutcome *o)
{
 struct stat st;

 fileop_begin(o, WPE_FILEOP_CHMOD, path, "");
 if (stat(path, &st) != 0)
  return(fileop_fail(o, WPE_FILEOP_STEP_SOURCE, errno));
 if (chmod(path, (mode_t)((st.st_mode & 07000) | (perm & 0777))) != 0)
  return(fileop_fail(o, WPE_FILEOP_STEP_MODE, errno));
 return(0);
}

int wpe_fileop_rename_noreplace(const char *src, const char *dst, WpeFileOpOutcome *o)
{
 int err;

 fileop_begin(o, WPE_FILEOP_MOVE, src, dst);
 err = fileop_rename_noreplace_at(AT_FDCWD, src, AT_FDCWD, dst);
 if (err != 0)
  return(fileop_fail(o, WPE_FILEOP_STEP_RENAME, err));
 o->src_kept = 0;
 return(0);
}

/* --- the sentence --------------------------------------------------------- */

/** fileop_verb - What the operation was, for "Cannot <verb> ...". */
static const char *fileop_verb(WpeFileOpKind kind)
{
 switch (kind)
 {
 case WPE_FILEOP_COPY:   return("copy");
 case WPE_FILEOP_MOVE:   return("move");
 case WPE_FILEOP_LINK:   return("link");
 case WPE_FILEOP_REMOVE: return("remove");
 case WPE_FILEOP_MKDIR:  return("make the folder");
 case WPE_FILEOP_RMDIR:  return("remove the folder");
 case WPE_FILEOP_CHMOD:  return("change the permissions of");
 }
 return("handle");
}

/** fileop_step_text - The step that stopped, in words; "" when the reason says it all. */
static const char *fileop_step_text(WpeFileOpStep step)
{
 switch (step)
 {
 case WPE_FILEOP_STEP_SOURCE:        return("reading the source");
 case WPE_FILEOP_STEP_CREATE:        return("creating the copy");
 case WPE_FILEOP_STEP_WRITE:         return("writing");
 case WPE_FILEOP_STEP_CLOSE:         return("closing");
 case WPE_FILEOP_STEP_MODE:          return("setting its permissions");
 case WPE_FILEOP_STEP_REPLACE:       return("moving the old file aside");
 case WPE_FILEOP_STEP_RENAME:        return("putting it in place");
 case WPE_FILEOP_STEP_REMOVE_SOURCE: return("removing the source");
 case WPE_FILEOP_STEP_LINK:          return("linking");
 case WPE_FILEOP_STEP_WASTEBASKET:   return("using the wastebasket");
 default:                            return("");
 }
}

/** fileop_reason - The reason half of the sentence, for a FAILED outcome. */
static void fileop_reason(char *out, size_t n, const WpeFileOpOutcome *o)
{
 const char *step = fileop_step_text(o->step);

 if (o->step == WPE_FILEOP_STEP_SAME_FILE)
  snprintf(out, n, "%s and %s are the same file", o->src, o->dst);
 else if (o->step == WPE_FILEOP_STEP_NOT_A_FILE)
  snprintf(out, n, "%s is not a file", o->bad);
 else if (o->kind == WPE_FILEOP_LINK && o->err_link != 0 && o->step == WPE_FILEOP_STEP_LINK)
 {
  char first[128];

  snprintf(first, sizeof first, "%s", strerror(o->err_link));
  snprintf(out, n, "link: %s; symlink: %s", first, strerror(o->err));
 }
 else if (step[0] != '\0')
  snprintf(out, n, "%s: %s", step, strerror(o->err != 0 ? o->err : EIO));
 else
  snprintf(out, n, "%s", strerror(o->err != 0 ? o->err : EIO));
}

/** fileop_left - What is left, for a FAILED outcome; "" when nothing needs saying. */
static void fileop_left(char *out, size_t n, const WpeFileOpOutcome *o)
{
 out[0] = '\0';
 if (o->step == WPE_FILEOP_STEP_REMOVE_SOURCE)
  snprintf(out, n, "  %s holds the copy; %s is still in place.", o->dst, o->src);
 else if (o->aside[0] != '\0')
  snprintf(out, n, "  %s is in the wastebasket as %s.", o->dst, o->aside);
 else if (o->src_kept && o->dst_kept)
  snprintf(out, n, "  Nothing was changed.");
}

int wpe_fileop_sentence(char *out, size_t n, const WpeFileOpOutcome *o)
{
 char reason[2 * WPE_PATH_SIZE + 160], left[2 * WPE_PATH_SIZE + 64];

 out[0] = '\0';
 if (o->status == WPE_FILEOP_DECLINED && o->step == WPE_FILEOP_STEP_EXISTS)
  snprintf(out, n, "%s exists; not overwritten.", o->dst);
 if (o->status != WPE_FILEOP_FAILED || o->step == WPE_FILEOP_STEP_CANCELLED)
  return((int)strlen(out));
 if (o->step == WPE_FILEOP_STEP_WASTEBASKET && o->kind == WPE_FILEOP_MKDIR)
 {
  snprintf(out, n, "Cannot use the wastebasket %s: %s.", o->src, strerror(o->err != 0 ? o->err : EIO));
  return((int)strlen(out));
 }
 fileop_reason(reason, sizeof reason, o);
 fileop_left(left, sizeof left, o);
 if (o->dst[0] != '\0' && o->kind != WPE_FILEOP_REMOVE)
  snprintf(out, n, "Cannot %s %s to %s: %s.%s", fileop_verb(o->kind), o->src, o->dst, reason, left);
 else
  snprintf(out, n, "Cannot %s %s: %s.%s", fileop_verb(o->kind), o->src, reason, left);
 return((int)strlen(out));
}
