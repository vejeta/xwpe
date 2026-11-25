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

/* This file defines the function the type check of we_fileop.h wraps. */
#define WPE_FILEOP_DEFINING_SHOWN 1

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "we_fmt.h"
#include "we_read.h"
#include "we_write.h"
#include "we_places.h"
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
#define FILEOP_HOOK_PLANT_SOURCE   "fileop-plant-source"
#define FILEOP_HOOK_NO_RENAMEAT2   "fileop-no-renameat2"
#define FILEOP_HOOK_NO_LINK        "fileop-no-link"
#define FILEOP_HOOK_REFUSE_SPECIAL "fileop-refuse-special"
#define FILEOP_HOOK_OTHER_OWNER    "fileop-other-owner"
#define FILEOP_HOOK_OWNER_UNREADABLE "fileop-owner-unreadable"

/** The wastebasket is the user's alone. */
#define FILEOP_WASTEBASKET_MODE 0700

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

/* --- the special bits a copy could not keep -------------------------------- */

/**
 * fileop_hook_refuse_special - Let a test see a system that refuses setgid.
 * @k: what the mode owner reported; @wanted: the mode asked for; @err: the
 * errno the refusal is told with.
 *
 * Read after the owner returned, so the editor shows its sentence over a
 * real kernel: the status row of a copy under a BSD's refusal can be driven
 * from a terminal test on a system that has none.
 */
static void fileop_hook_refuse_special(WpeModeKept *k, mode_t wanted, int err)
{
 if (!fileop_test_hook(FILEOP_HOOK_REFUSE_SPECIAL) || (wanted & S_ISGID) == 0)
  return;
 k->lost |= S_ISGID;
 k->err = err;
}

/**
 * fileop_note_bits_lost - Record that an entry did not keep a special bit.
 * @n: the note of the outcome or the folder; @path: the entry; @k: what the
 * mode owner said.
 *
 * Nothing when every bit stayed.  The first entry is named; the others are
 * counted.
 */
static void fileop_note_bits_lost(WpeFileOpBitsNote *n, const char *path, const WpeModeKept *k)
{
 if (k->lost == 0)
  return;
 n->bits_lost_count++;
 if (n->bits_lost_count > 1)
  return;
 n->bits_lost = k->lost;
 n->bits_err = k->err;
 n->bits_gid = k->gid;
 n->bits_in_group = k->in_group;
 n->bits_owners = k->owners;
 snprintf(n->bits_path, sizeof n->bits_path, "%s", path);
}

/**
 * fileop_note_bits_lost_first - Record an entry that did not keep a bit and
 * name it first, before any entry already noted.
 *
 * The user's own copy is what the sentence names first; the wastebasket
 * copy of the file it replaced, noted while the name was put in place, is
 * counted after it.
 */
static void fileop_note_bits_lost_first(WpeFileOpBitsNote *n, const char *path, const WpeModeKept *k)
{
 long later = n->bits_lost_count;

 if (k->lost == 0)
  return;
 n->bits_lost_count = 0;
 fileop_note_bits_lost(n, path, k);
 n->bits_lost_count += later;
}

/**
 * fileop_note_merge - Add the note of one entry to the note of its folder.
 * @to: the folder's; @from: the entry's.  The first entry named stays.
 */
static void fileop_note_merge(WpeFileOpBitsNote *to, const WpeFileOpBitsNote *from)
{
 if (from->bits_lost_count == 0)
  return;
 if (to->bits_lost_count == 0)
 {
  *to = *from;
  return;
 }
 to->bits_lost_count += from->bits_lost_count;
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
 int how = wpe_fmt_whole(out, size, ".%s.xwpe-copy.%ld.%d", name, (long)getpid(), n);

 if (how != WPE_FMT_OK || strlen(out) > FILEOP_NAME_LIMIT)
  how = wpe_fmt_whole(out, size, ".xwpe-copy.%ld.%d", (long)getpid(), n);
 return(how != WPE_FMT_OK ? ENAMETOOLONG : 0);
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

/* One file operation works on two places and never on a path: the source is
   the name @sname in the open folder @spfd, the destination is the name
   @name in the open folder @dirfd.  The paths @src and @dst are words, for
   the questions, the outcome and the text of a symbolic link; no system
   call is given them.  A folder walk lends its descriptors to each entry's
   job (@own_* is 0); a single file operation opens its own and closes them
   in fileop_job_end. */
typedef struct {
 WpeFileOpOutcome       *o;
 const WpeFileOpPolicy  *p;
 char                    src[WPE_PATH_SIZE];   /* words: the source as named */
 char                    dst[WPE_PATH_SIZE];   /* words: with a destination folder resolved */
 char                    sname[WPE_NAME_SIZE]; /* the source's name in spfd */
 char                    name[WPE_NAME_SIZE];  /* the destination's name in dirfd */
 int                     spfd;
 int                     dirfd;
 int                     own_spfd;             /* closed by fileop_job_end */
 int                     own_dirfd;
 int                     follow;               /* a link named as the source is followed (a single Copy) */
 struct stat             sst;                  /* the source */
 int                     dst_exists;
 int                     replace;              /* the existing destination is to be replaced */
 WpeModeKept             kept;                 /* the special bits the new copy did not keep */
 WpeModeKept             stash_kept;           /* the same for the copy of the replaced file in the wastebasket */
} FileJob;

/** fileop_job_begin - Start an operation: the outcome, the policy and the words. */
static void fileop_job_begin(FileJob *j, WpeFileOpKind kind, const char *src, const char *dst,
                             const WpeFileOpPolicy *p, WpeFileOpOutcome *o)
{
 memset(j, 0, sizeof *j);
 j->spfd = -1;
 j->dirfd = -1;
 j->o = o;
 j->p = p;
 snprintf(j->src, sizeof j->src, "%s", src);
 snprintf(j->dst, sizeof j->dst, "%s", dst);
 fileop_begin(o, kind, src, dst);
}

/**
 * fileop_job_aim - Point a job at two names in two open folders it does not own.
 * Used by a folder walk, which keeps its folders open from entry to entry.
 */
static void fileop_job_aim(FileJob *j, int spfd, const char *sname, int dirfd, const char *name)
{
 j->spfd = spfd;
 snprintf(j->sname, sizeof j->sname, "%s", sname);
 j->dirfd = dirfd;
 snprintf(j->name, sizeof j->name, "%s", name);
}

/** fileop_job_end - Close what the operation opened itself. */
static void fileop_job_end(FileJob *j)
{
 if (j->own_dirfd && j->dirfd >= 0)
  close(j->dirfd);
 if (j->own_spfd && j->spfd >= 0)
  close(j->spfd);
 j->dirfd = -1;
 j->spfd = -1;
}

/**
 * fileop_parent_open - Open the folder that holds the last name of a path the
 * user typed, and give the name.
 * @path: what the user typed; @fd: receives the folder; @name, @n: the name.
 * Return: 0, or the errno.
 *
 * The one place a typed path is resolved (links in it are followed, the user
 * named them).  Everything below is reached by descriptor and name.
 */
static int fileop_parent_open(const char *path, int *fd, char *name, size_t n)
{
 char dir[WPE_PATH_SIZE];
 int err = fileop_split(path, dir, sizeof dir, name, n);

 if (err != 0)
  return(err);
 *fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
 return(*fd < 0 ? errno : 0);
}

/**
 * fileop_job_open_src_path - Open the folder of a source the user typed.
 * Return: 0, or -1 (outcome set).
 */
static int fileop_job_open_src_path(FileJob *j)
{
 int err = fileop_parent_open(j->src, &j->spfd, j->sname, sizeof j->sname);

 if (err != 0)
  return(fileop_fail(j->o, WPE_FILEOP_STEP_SOURCE, err));
 j->own_spfd = 1;
 return(0);
}

/**
 * fileop_job_judge_src - Judge a source that is not opened: a name, not a folder.
 * Return: 0, or -1 (outcome set).
 *
 * Move and Link take a link as the name it is, so the source is looked at
 * without following.
 */
static int fileop_job_judge_src(FileJob *j)
{
 if (fstatat(j->spfd, j->sname, &j->sst, AT_SYMLINK_NOFOLLOW) != 0)
  return(fileop_fail(j->o, WPE_FILEOP_STEP_SOURCE, errno));
 if (S_ISDIR(j->sst.st_mode))
  return(fileop_not_a_file(j->o, j->src));
 return(0);
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
 * fileop_job_dst_is_folder - Is the destination the user typed a folder to
 * put the file in?
 * @pfd, @name: the typed destination as a name in its folder.
 * Return: 0 no (a name), 1 yes (j->dirfd is that folder), -1 (outcome set).
 *
 * The typed name is resolved once, here: a link that points to a folder is
 * the folder it points to, so the file goes inside it (a name the user
 * typed, never one a walk found); a link that points to nothing is a name
 * and is replaced like any other.  A name written with a slash at its end
 * can only be a folder: when there is none, nothing is made under that name.
 */
static int fileop_job_dst_is_folder(FileJob *j, int pfd, const char *name)
{
 size_t len = strlen(j->dst);
 int fd = openat(pfd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
 int err = errno;

 if (fd >= 0)
 {
  j->dirfd = fd;
  return(1);
 }
 if (err == ENOENT || err == ENOTDIR || err == ELOOP)
 {
  if (len > 1 && j->dst[len - 1] == '/')
   return(fileop_fail(j->o, WPE_FILEOP_STEP_CREATE, err == ELOOP ? ENOTDIR : err));
  return(0);
 }
 return(fileop_fail(j->o, WPE_FILEOP_STEP_CREATE, err));
}

/**
 * fileop_job_open_dst_path - Open the destination the user typed.
 * Return: 0, or -1 (outcome set).
 *
 * As cp, mv and Midnight Commander do: naming a folder as the place for a
 * file puts the file in it under its own name, and never replaces the
 * folder.  The typed destination's folder is opened once; when the name is
 * a folder, that folder is.
 */
static int fileop_job_open_dst_path(FileJob *j)
{
 char base[WPE_NAME_SIZE], folder[WPE_PATH_SIZE], joined[WPE_PATH_SIZE];
 int pfd, err, rc;

 err = fileop_parent_open(j->dst, &pfd, j->name, sizeof j->name);
 if (err != 0)
  return(fileop_fail(j->o, WPE_FILEOP_STEP_CREATE, err));
 rc = fileop_job_dst_is_folder(j, pfd, j->name);
 if (rc < 0)
 {
  close(pfd);
  return(-1);
 }
 j->own_dirfd = 1;
 if (rc == 0)
 {
  j->dirfd = pfd;
  return(0);
 }
 close(pfd);
 err = fileop_split(j->src, folder, sizeof folder, base, sizeof base);
 if (err == 0 && wpe_path_join(joined, sizeof joined, j->dst, base) != 0)
  err = ENAMETOOLONG;
 if (err == 0)
  snprintf(j->name, sizeof j->name, "%s", base);
 if (err != 0)
  return(fileop_fail(j->o, WPE_FILEOP_STEP_CREATE, err));
 snprintf(j->dst, sizeof j->dst, "%s", joined);
 snprintf(j->o->dst, sizeof j->o->dst, "%s", joined);
 return(0);
}

/**
 * fileop_job_names_target_of_source - Is the destination the file the
 * source link points to?
 * @st: the destination's own status.
 *
 * A Move or a Link of a link onto the file it points to would replace that
 * file with the link, which then points to itself and the data is gone; it
 * is the same file, as GNU mv says.  A Copy of a file opened its source
 * already, so its status is the target's and the plain comparison caught
 * it; only a source judged as a link needs this.  The target is only looked
 * at, never written.
 */
static int fileop_job_names_target_of_source(const FileJob *j, const struct stat *st)
{
 struct stat target;

 if (!S_ISLNK(j->sst.st_mode))
  return(0);
 if (fstatat(j->spfd, j->sname, &target, 0) != 0)
  return(0);
 return(target.st_dev == st->st_dev && target.st_ino == st->st_ino);
}

/**
 * fileop_job_judge_dst - Decide what the destination is and whether it goes.
 * Return: 0 to go on, -1 when the operation ended (outcome set).
 *
 * Looks at the name in the destination's folder without following it.  The
 * same file as the source, a device, a pipe or a socket in the way,
 * "Don't overwrite" and the user's answer end the operation before
 * anything is written.
 */
static int fileop_job_judge_dst(FileJob *j)
{
 struct stat st;

 if (fstatat(j->dirfd, j->name, &st, AT_SYMLINK_NOFOLLOW) != 0)
 {
  if (errno != ENOENT)
   return(fileop_fail(j->o, WPE_FILEOP_STEP_CREATE, errno));
  return(0);
 }
 j->dst_exists = 1;
 if (st.st_dev == j->sst.st_dev && st.st_ino == j->sst.st_ino)
  return(fileop_fail(j->o, WPE_FILEOP_STEP_SAME_FILE, 0));
 if (fileop_job_names_target_of_source(j, &st))
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

/** fileop_link_text - What the link @sname in the open folder @sfd points to.  Return: 0 or the errno. */
static int fileop_link_text(int sfd, const char *sname, char *out, size_t n)
{
 ssize_t got = readlinkat(sfd, sname, out, n - 1);

 if (got < 0)
  return(errno);
 if ((size_t)got >= n - 1)
  return(ENAMETOOLONG);
 out[got] = '\0';
 return(0);
}

/**
 * fileop_copy_link - Make the link (@sfd, @sname) again as (@dirfd, @name),
 * with the same text.
 * Return: 0, or the errno.
 */
static int fileop_copy_link(int sfd, const char *sname, int dirfd, const char *name)
{
 char text[WPE_PATH_SIZE];
 int err = fileop_link_text(sfd, sname, text, sizeof text);

 if (err != 0)
  return(err);
 if (symlinkat(text, dirfd, name) != 0)
  return(errno);
 return(0);
}

/**
 * fileop_wastebasket_leaf - The try-th name for @name in the wastebasket:
 * `name`, `name.1`, `name.2`, ...
 */
static void fileop_wastebasket_leaf(char *leaf, size_t n, const char *name, int try)
{
 if (try == 0)
  snprintf(leaf, n, "%s", name);
 else
  snprintf(leaf, n, "%s.%d", name, try);
}

/** fileop_wastebasket_name - The try-th path for @name in the wastebasket. */
static int fileop_wastebasket_name(char *out, size_t n, const char *wb, const char *name, int try)
{
 char leaf[WPE_NAME_SIZE + 16];

 fileop_wastebasket_leaf(leaf, sizeof leaf, name, try);
 return(wpe_path_join(out, n, wb, leaf));
}

int wpe_fileop_wastebasket(char *out, size_t n, WpeFileOpOutcome *o)
{
 char home[WPE_PATH_SIZE];
 char path[WPE_PATH_SIZE];
 struct stat st;
 int fd;

 fileop_begin(o, WPE_FILEOP_MKDIR, "", "");
 if (wpe_user_home(home, sizeof home) != 0)
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
 if ((st.st_mode & 077) != 0)
 {
  WpeModeKept kept;
  int err = wpe_write_set_mode_fd(fd, FILEOP_WASTEBASKET_MODE, &kept);

  if (err != 0)
  {
   close(fd);
   return(fileop_fail(o, WPE_FILEOP_STEP_WASTEBASKET, err));
  }
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

static int fileop_entry_copy(FileJob *j);

/** The wastebasket's open folder and the name taken in it. */
typedef struct {
 int  fd;
 char leaf[WPE_NAME_SIZE + 16];
} WasteSlot;

/**
 * fileop_wastebasket_open - Open the wastebasket, made and checked as ours,
 * without following a link.
 * Return: 0, or the errno.
 */
static int fileop_wastebasket_open(char *wb, size_t n, int *fd)
{
 WpeFileOpOutcome w;

 if (wpe_fileop_wastebasket(wb, n, &w) != 0)
  return(w.err != 0 ? w.err : EIO);
 *fd = open(wb, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
 return(*fd < 0 ? errno : 0);
}

/**
 * fileop_stash_link - Give the destination a second name in the wastebasket.
 * Return: 0, EEXIST when every name is taken, else the errno that says a
 * link cannot be made there (another file system).
 */
static int fileop_stash_link(FileJob *j, WasteSlot *w)
{
 int i, err = EEXIST;

 for (i = 0; i < FILEOP_TRIES; i++)
 {
  fileop_wastebasket_leaf(w->leaf, sizeof w->leaf, j->name, i);
  if (linkat(j->dirfd, j->name, w->fd, w->leaf, 0) == 0)
   return(0);
  err = errno;
  if (err != EEXIST)
   break;
 }
 return(err);
}

/**
 * fileop_stash_copy - Keep a copy of the destination in the wastebasket when
 * a link cannot be made: the link again, or the file's bytes.
 * Return: 0, or the errno.
 *
 * The destination is read by its name in its folder and without following
 * a link, so a folder swapped for a link meanwhile is not read through.
 */
static int fileop_stash_copy(FileJob *j, WasteSlot *w)
{
 FileJob c;
 WpeFileOpOutcome oc;
 WpeFileOpPolicy inner;
 struct stat st;
 int i, err;

 for (i = 0; i < FILEOP_TRIES; i++)
 {
  fileop_wastebasket_leaf(w->leaf, sizeof w->leaf, j->name, i);
  if (fstatat(w->fd, w->leaf, &st, AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT)
   break;
 }
 if (i == FILEOP_TRIES)
  return(EEXIST);
 if (fstatat(j->dirfd, j->name, &st, AT_SYMLINK_NOFOLLOW) != 0)
  return(errno);
 if (S_ISLNK(st.st_mode))
  return(fileop_copy_link(j->dirfd, j->name, w->fd, w->leaf));
 memset(&inner, 0, sizeof inner);
 inner.replace = WPE_REPLACE_NEVER;
 inner.remove = WPE_REMOVE_NOW;
 fileop_job_begin(&c, WPE_FILEOP_COPY, j->dst, w->leaf, &inner, &oc);
 fileop_job_aim(&c, j->dirfd, j->name, w->fd, w->leaf);
 err = fileop_entry_copy(&c);
 if (err != 0)
  return(oc.err != 0 ? oc.err : EIO);
 j->stash_kept = c.kept;
 return(0);
}

/**
 * fileop_job_stash - Keep the destination about to be replaced in the wastebasket.
 * @aside: receives the name it was kept under.
 * Return: 0, or the errno.
 *
 * The old file is given a second name there before the new one takes its
 * name (a link, or a copy when the wastebasket is on another file system),
 * so that a failed replacement can drop that name again and leave
 * everything as it was.  Never over an earlier file of the same name.  The
 * old file is addressed as its name in its folder; only the wastebasket,
 * which the owner names itself, is a path.
 */
static int fileop_job_stash(FileJob *j, char *aside, size_t n)
{
 char wb[WPE_PATH_SIZE];
 WasteSlot w;
 int err;

 err = fileop_wastebasket_open(wb, sizeof wb, &w.fd);
 if (err != 0)
  return(err);
 err = fileop_stash_link(j, &w);
 if (err != 0 && err != EEXIST)
  err = fileop_stash_copy(j, &w);
 if (err == 0 && wpe_path_join(aside, n, wb, w.leaf) != 0)
 {
  (void)unlinkat(w.fd, w.leaf, 0);
  err = ENAMETOOLONG;
 }
 close(w.fd);
 return(err);
}

/**
 * fileop_job_placed - Record that the name is in place.
 * @aside: where the replaced file was kept; @stashed: it was.
 *
 * The outcome says where the old file is, and, when its copy in the
 * wastebasket lost a special bit, which one: the user who looks for the
 * old file there finds it without the bit and was told so.
 */
static void fileop_job_placed(FileJob *j, const char *aside, int stashed)
{
 WpeFileOpOutcome *o = j->o;

 if (stashed)
 {
  snprintf(o->aside, sizeof o->aside, "%s", aside);
  fileop_note_bits_lost(&o->note, aside, &j->stash_kept);
 }
 o->dst_kept = (j->replace ? 0 : 1);
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
  /* A test plants a name at the last moment; a plant that fails would let
     the rows that need it pass without it, so it fails the step. */
  if (fileop_test_hook(FILEOP_HOOK_PLANT) &&
      symlinkat(FILEOP_PLANTED_TARGET, j->dirfd, j->name) != 0)
  {
   err = errno;
   if (is_temp)
    unlinkat(fromfd, from, 0);
   return(fileop_fail(o, WPE_FILEOP_STEP_CREATE, err));
  }
  if (fileop_test_hook(FILEOP_HOOK_RENAME))
   err = EIO;
  else
   err = fileop_rename_noreplace_at(fromfd, from, j->dirfd, j->name);
 }
 if (err == 0)
 {
  fileop_job_placed(j, aside, stashed);
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

/** The noun of a refusal of a copy's source; the copy tells it its own way. */
#define FILEOP_SOURCE_WHAT "file"

/**
 * fileop_source_refused - Record why the read owner refused a copy's source.
 * @j: the job; @ro: the owner's outcome.
 * Return: -1 (outcome set).
 *
 * Something that is not a file (a pipe, a device, a folder) is told as that;
 * a link where the entry was judged a file means the name changed after it
 * was looked at, so the user reads that the source changed, never a system
 * error that names no cause; every other refusal is told as the error the
 * system gave.
 */
static int fileop_source_refused(FileJob *j, const WpeReadOutcome *ro)
{
 if (ro->why == WPE_READ_NOT_A_FILE)
  return(fileop_not_a_file(j->o, j->src));
 if (ro->why == WPE_READ_IS_A_LINK)
  return(fileop_fail(j->o, WPE_FILEOP_STEP_CHANGED, 0));
 if (ro->why == WPE_READ_REFUSED_BY_SYSTEM)
  return(fileop_fail(j->o, WPE_FILEOP_STEP_SOURCE, ro->err));
 return(fileop_fail(j->o, WPE_FILEOP_STEP_SOURCE, ENOMEM));
}

/**
 * fileop_source_ask - What the copy asks of the read owner for its source.
 * @j: the job.
 *
 * The name is relative to the folder the walk holds open, so a folder
 * swapped for a link meanwhile is not followed.  A link is followed only when
 * the copy was asked to follow it.
 */
static WpeReadAsk fileop_source_ask(const FileJob *j)
{
 if (j->follow)
  return(WPE_READ_ASK_AT(WPE_READ_ASK_SCAN(FILEOP_SOURCE_WHAT), j->spfd));
 return(WPE_READ_ASK_AT(WPE_READ_ASK_SCAN_NOFOLLOW(FILEOP_SOURCE_WHAT), j->spfd));
}

/**
 * fileop_open_source - Open the file to copy from, whole or not at all.
 * @fd: receives the descriptor.
 * Return: 0, or -1 (outcome set).
 *
 * Opened by the read owner below the folder the walk holds, without waiting,
 * so a pipe named as the source is refused at once instead of hanging the
 * editor, then put back to blocking reads.
 */
static int fileop_open_source(FileJob *j, int *fd)
{
 WpeReadAsk ask = fileop_source_ask(j);
 WpeReadOutcome ro;

 *fd = wpe_read_open(j->sname, &ask, &ro);
 if (*fd < 0)
  return(fileop_source_refused(j, &ro));
 if (fstat(*fd, &j->sst) != 0)
 {
  int err = errno;

  close(*fd);
  return(fileop_fail(j->o, WPE_FILEOP_STEP_SOURCE, err));
 }
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

/** The bits that make a program run as its owner or its group. */
#define FILEOP_SETID_BITS (S_ISUID | S_ISGID)

/**
 * wpe_fileop_copy_mode - The mode a copy may be given, from who owns it and
 * who owned the original.
 * @wanted: the original's mode; @src_uid, @src_gid: the original's owner and
 * group; @copy_uid, @copy_gid: the copy's; @dropped: receives the set-ID
 * bits left off and why.
 * Return: @wanted less the set-ID bits the copy may not have.
 *
 * Setuid stays only when the copy has the original's owner.  Setgid stays
 * only when it has the original's owner and group.  The sticky bit says
 * nothing about who owns a file and stays.  So a program of another user
 * never runs as the user who copied it, which matters most when root copies
 * a user's setuid program.  Pure: no file is read, so a test can give it any
 * pair of owners.
 */
mode_t wpe_fileop_copy_mode(mode_t wanted, uid_t src_uid, gid_t src_gid, uid_t copy_uid, gid_t copy_gid, WpeModeKept *dropped)
{
 int owner_differs = (copy_uid != src_uid);
 int group_differs = (copy_gid != src_gid);

 memset(dropped, 0, sizeof *dropped);
 if ((wanted & FILEOP_SETID_BITS) == 0)
  return(wanted);
 if ((wanted & S_ISUID) && owner_differs)
  dropped->lost |= S_ISUID;
 if ((wanted & S_ISGID) && (owner_differs || group_differs))
  dropped->lost |= S_ISGID;
 if (dropped->lost == 0)
  return(wanted);
 dropped->owners.dropped = dropped->lost;
 dropped->owners.other_owner = owner_differs;
 dropped->owners.other_group = !owner_differs;
 dropped->owners.uid_copy = copy_uid;
 dropped->owners.uid_src = src_uid;
 dropped->owners.gid_copy = copy_gid;
 dropped->owners.gid_src = src_gid;
 dropped->gid = copy_gid;
 dropped->in_group = wpe_write_caller_in_group(copy_gid);
 return(wanted & ~dropped->lost);
}

/**
 * fileop_read_copy_owner - Read who owns the copy this job made.
 * @fd: the copy; @copy: receives its status.
 * Return: 0, or -1 with errno.
 *
 * Two test hooks play the cases a user's machine cannot make: another owner
 * (root copying another user's file) and an owner that cannot be read.
 */
static int fileop_read_copy_owner(const FileJob *j, int fd, struct stat *copy)
{
 if (fileop_test_hook(FILEOP_HOOK_OWNER_UNREADABLE))
 {
  errno = EIO;
  return(-1);
 }
 if (fstat(fd, copy) != 0)
  return(-1);
 if (fileop_test_hook(FILEOP_HOOK_OTHER_OWNER))
  copy->st_uid = j->sst.st_uid + 1;
 return(0);
}

/**
 * fileop_bits_for_copy - The mode a finished copy may be given.
 * @fd: the copy, made by this job and not yet closed; @dropped: receives the
 * set-ID bits that were left off and why.
 * Return: the original's mode less the set-ID bits the copy may not have.
 *
 * Decided from the owner and group the system gave the copy (read from its
 * descriptor), never from a name.  When root copies another user's setuid
 * program, the copy belongs to root and would be a program that runs as
 * root with text another user wrote; it is given no setuid bit, and the
 * status row names the user and group involved.  A plain file costs no
 * call.  A copy whose owner cannot be read gets no set-ID bit either, and
 * the status row says why.
 */
static mode_t fileop_bits_for_copy(const FileJob *j, int fd, WpeModeKept *dropped)
{
 struct stat copy;
 mode_t wanted = j->sst.st_mode;

 memset(dropped, 0, sizeof *dropped);
 if ((wanted & FILEOP_SETID_BITS) == 0)
  return(wanted);
 if (fileop_read_copy_owner(j, fd, &copy) != 0)
 {
  dropped->lost = (wanted & FILEOP_SETID_BITS);
  dropped->err = errno;
  dropped->in_group = 1;    /* the reason is the failed read, never a group */
  return(wanted & ~dropped->lost);
 }
 return(wpe_fileop_copy_mode(wanted, j->sst.st_uid, j->sst.st_gid, copy.st_uid, copy.st_gid, dropped));
}

/**
 * fileop_job_fill_temp - Write the source into the copy and finish it.
 * @durable: the data is flushed to disk before the close.
 * Return: 0, or -1 (outcome set; the copy is removed by the caller).
 *
 * Every write, the flush and the close are judged: the copy counts as made
 * only when each returned 0.  The special permission bits go on after the
 * data, because the kernel clears them on the first write, and after the
 * data is on disk.
 */
static int fileop_job_fill_temp(FileJob *j, int sfd, int fd, int durable)
{
 int err, read_failed;
 mode_t keep;
 WpeModeKept dropped;

 wpe_write_guard_enter();
 err = wpe_write_copy_fd(sfd, fd, &read_failed);
 if (err != 0)
 {
  close(fd);
  wpe_write_guard_leave();
  return(fileop_fail(j->o, read_failed ? WPE_FILEOP_STEP_SOURCE : WPE_FILEOP_STEP_WRITE, err));
 }
 keep = fileop_bits_for_copy(j, fd, &dropped);
 err = wpe_write_sync_before_bits(fd, keep, durable);
 if (err != 0)
 {
  close(fd);
  wpe_write_guard_leave();
  return(fileop_fail(j->o, WPE_FILEOP_STEP_WRITE, err));
 }
 err = wpe_write_set_special_bits(fd, keep, &j->kept);
 if (err != 0)
 {
  close(fd);
  wpe_write_guard_leave();
  return(fileop_fail(j->o, WPE_FILEOP_STEP_MODE, err));
 }
 wpe_write_kept_merge(&j->kept, &dropped);
 fileop_hook_refuse_special(&j->kept, keep, EPERM);
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
 if (rc != 0)
  return(-1);
 fileop_note_bits_lost_first(&j->o->note, j->dst, &j->kept);
 return(0);
}

/**
 * fileop_entry_copy_from - Copy an opened source over what is at the
 * destination name, under the policy.
 * @sfd: the open source; it is closed here.
 * Return: 0, or -1 (outcome set).
 */
static int fileop_entry_copy_from(FileJob *j, int sfd)
{
 int rc = fileop_job_judge_dst(j);

 if (rc == 0)
  rc = fileop_job_copy_in(j, sfd, 0);
 close(sfd);
 return(rc == 0 ? 0 : -1);
}

/**
 * fileop_entry_copy - Copy the file (j->spfd, j->sname) to (j->dirfd, j->name).
 * Return: 0, or -1 (outcome set).
 *
 * The source is opened by its name in its folder without following a link
 * (a single Copy of a link the user named follows it, once).
 */
static int fileop_entry_copy(FileJob *j)
{
 int sfd;

 if (fileop_open_source(j, &sfd) != 0)
  return(-1);
 return(fileop_entry_copy_from(j, sfd));
}

int wpe_fileop_copy(const char *src, const char *dst, const WpeFileOpPolicy *p, WpeFileOpOutcome *o)
{
 FileJob j;
 int sfd, rc;

 fileop_job_begin(&j, WPE_FILEOP_COPY, src, dst, p, o);
 j.follow = 1;
 if (fileop_job_open_src_path(&j) != 0)
  return(-1);
 if (fileop_open_source(&j, &sfd) != 0)
 {
  fileop_job_end(&j);
  return(-1);
 }
 if (fileop_job_open_dst_path(&j) != 0)
 {
  close(sfd);
  fileop_job_end(&j);
  return(-1);
 }
 rc = fileop_entry_copy_from(&j, sfd);
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
   err = fileop_copy_link(j->spfd, j->sname, j->dirfd, tmp);
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
 if (unlinkat(j->spfd, j->sname, 0) != 0)
  return(fileop_fail(o, WPE_FILEOP_STEP_REMOVE_SOURCE, errno));
 o->src_kept = 0;
 return(0);
}

/**
 * fileop_job_plant_source - Test seam: a link takes the source's name.
 * @j: the job.
 *
 * Under the test hook the source is replaced by a link between the moment
 * it was judged a file and the moment it is opened, the race the user can
 * only meet by chance.  Does nothing otherwise, and nothing more when the
 * link cannot be made: the row then sees a missing source, not a link, and
 * fails on its own assertion.
 */
static void fileop_job_plant_source(FileJob *j)
{
 if (!fileop_test_hook(FILEOP_HOOK_PLANT_SOURCE))
  return;
 if (unlinkat(j->spfd, j->sname, 0) != 0)
  return;
 if (symlinkat(FILEOP_PLANTED_TARGET, j->spfd, j->sname) != 0)
  return;
}

/**
 * fileop_job_move_across - Move to another file system: copy, then remove.
 * Return: 0 or -1 (outcome set).
 */
static int fileop_job_move_across(FileJob *j)
{
 int sfd, rc;

 fileop_job_plant_source(j);
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

/**
 * fileop_entry_move - Move the entry (j->spfd, j->sname) to (j->dirfd, j->name).
 * Return: 0, or -1 (outcome set).
 *
 * @j->sst is what the entry was judged to be.  On one file system the name
 * is renamed from its folder to the destination's folder; across file
 * systems it is copied and then removed, by the same folder and name.
 */
static int fileop_entry_move(FileJob *j)
{
 int rc = fileop_job_judge_dst(j);

 if (rc != 0)
  return(-1);
 rc = fileop_job_place(j, j->spfd, j->sname, 0);
 if (rc == 0)
  j->o->src_kept = 0;
 else if (rc == 1)
  rc = fileop_job_move_across(j);
 return(rc == 0 ? 0 : -1);
}

int wpe_fileop_move(const char *src, const char *dst, const WpeFileOpPolicy *p, WpeFileOpOutcome *o)
{
 FileJob j;
 int rc;

 fileop_job_begin(&j, WPE_FILEOP_MOVE, src, dst, p, o);
 if (fileop_job_open_src_path(&j) != 0)
  return(-1);
 if (fileop_job_judge_src(&j) != 0 || fileop_job_open_dst_path(&j) != 0)
 {
  fileop_job_end(&j);
  return(-1);
 }
 rc = fileop_entry_move(&j);
 fileop_job_end(&j);
 return(rc);
}

/* --- the link ------------------------------------------------------------- */

/**
 * fileop_try_link - One try at the link under one temporary name.
 * Return: 0, EEXIST when the name is taken, else the errno of the symbolic
 * link that was the last resort.
 */
static int fileop_try_link(FileJob *j, const char *tmp)
{
#ifdef NOSYMLINKS
 /* a system without symbolic links: a hard link, or the reason it was not made */
 if (linkat(j->spfd, j->sname, j->dirfd, tmp, 0) != 0)
  return(errno);
 return(0);
#else
 if (j->p->hardlink_first)
 {
  /* no AT_SYMLINK_FOLLOW: a link gives a hard link to the link, never to its target */
  if (linkat(j->spfd, j->sname, j->dirfd, tmp, 0) == 0)
   return(0);
  if (errno == EEXIST)
   return(EEXIST);
  j->o->err_link = errno;
 }
 if (symlinkat(j->src, j->dirfd, tmp) != 0)
  return(errno);
 return(0);
#endif
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

/**
 * fileop_entry_link - Link the entry (j->spfd, j->sname) at (j->dirfd, j->name).
 * Return: 0, or -1 (outcome set).
 *
 * @j->sst is what the entry was judged to be.  A hard link to a link is a
 * link to the link itself, never to what it points to.
 */
static int fileop_entry_link(FileJob *j)
{
 char tmp[WPE_NAME_SIZE];
 int rc = fileop_job_judge_dst(j);

 if (rc == 0)
  rc = fileop_job_make_link(j, tmp, sizeof tmp);
 if (rc == 0)
  rc = fileop_job_place(j, j->dirfd, tmp, 1);
 return(rc == 0 ? 0 : -1);
}

int wpe_fileop_link(const char *src, const char *dst, const WpeFileOpPolicy *p, WpeFileOpOutcome *o)
{
 FileJob j;
 int rc;

 fileop_job_begin(&j, WPE_FILEOP_LINK, src, dst, p, o);
 if (fileop_job_open_src_path(&j) != 0)
  return(-1);
 if (fileop_job_judge_src(&j) != 0 || fileop_job_open_dst_path(&j) != 0)
 {
  fileop_job_end(&j);
  return(-1);
 }
 rc = fileop_entry_link(&j);
 fileop_job_end(&j);
 return(rc);
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
 o->note = inner.note;
 return(0);
}

/**
 * fileop_remove_answer - May this file go?  What the Remove rule and the
 * user say.
 * Return: a WPE_FILEOP_ANSWER_*; YES without a question unless the rule asks.
 */
static int fileop_remove_answer(const WpeFileOpPolicy *p, const char *path)
{
 char question[WPE_PATH_SIZE + 32];

 if (p->remove != WPE_REMOVE_ASK)
  return(WPE_FILEOP_ANSWER_YES);
 snprintf(question, sizeof question, "Remove File:\n%s", path);
 if (p->ask == NULL)
  return(WPE_FILEOP_ANSWER_NO);
 return(p->ask(p->ctx, question));
}

int wpe_fileop_remove(const char *path, const WpeFileOpPolicy *p, WpeFileOpOutcome *o)
{
 struct stat st;
 char name[WPE_NAME_SIZE];
 int pfd, err, answer;

 fileop_begin(o, WPE_FILEOP_REMOVE, path, "");
 err = fileop_parent_open(path, &pfd, name, sizeof name);
 if (err != 0)
  return(fileop_fail(o, WPE_FILEOP_STEP_SOURCE, err));
 if (fstatat(pfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
 {
  err = errno;
  close(pfd);
  return(fileop_fail(o, WPE_FILEOP_STEP_SOURCE, err));
 }
 if (S_ISDIR(st.st_mode))
 {
  close(pfd);
  return(fileop_not_a_file(o, path));
 }
 if (p->remove == WPE_REMOVE_WASTEBASKET && !wpe_fileop_in_wastebasket(path))
 {
  close(pfd);
  return(fileop_remove_into_wastebasket(path, o));
 }
 answer = fileop_remove_answer(p, path);
 if (answer == WPE_FILEOP_ANSWER_STOP)
  err = fileop_fail(o, WPE_FILEOP_STEP_CANCELLED, 0);
 else if (answer != WPE_FILEOP_ANSWER_YES)
  err = fileop_decline(o, WPE_FILEOP_STEP_NONE);
 else if (unlinkat(pfd, name, 0) != 0)
  err = fileop_fail(o, WPE_FILEOP_STEP_REMOVE, errno);
 else
 {
  o->src_kept = 0;
  err = 0;
 }
 close(pfd);
 return(err);
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

/**
 * fileop_chmod_failed - Record why the permissions of a name were not changed.
 * @o: the outcome; @k: what the mode owner said; @err: its errno.
 * Return: -1.
 *
 * The user reads where it stopped: a name that leads nowhere, another file
 * or a link that took the name while the dialog was open (nothing of the
 * user's file was touched), or the permissions refused.  A failure after a
 * call that took the nine bits is not "nothing was changed", so the link
 * sentence that says so is chosen only when no call changed anything.
 */
static int fileop_chmod_failed(WpeFileOpOutcome *o, const WpeModeKept *k, int err)
{
 if (k->changed)
  o->src_kept = 0;
 if (k->unreached)
  return(fileop_fail(o, WPE_FILEOP_STEP_SOURCE, err));
 if (k->replaced)
 {
  o->replaced_by_link = S_ISLNK(k->replaced_kind);
  o->replaced_unsure = k->replaced_unsure;
  return(fileop_fail(o, WPE_FILEOP_STEP_CHANGED, err));
 }
 return(fileop_fail(o, WPE_FILEOP_STEP_MODE, err));
}

int wpe_fileop_chmod(const char *path, mode_t perm, WpeFileOpOutcome *o)
{
 return(wpe_fileop_chmod_shown(path, NULL, NULL, perm, o));
}

/**
 * wpe_fileop_chmod_shown - Change the permissions of the file the user was shown.
 * @path: the name; @shown: the record the Attributes dialog took of it; @seen:
 * what the name itself was then (wpe_write_name_seen), or NULL when not
 * known; @perm: the nine bits; @o: the outcome.
 *
 * The dialog can stay open for as long as the user likes.  The file it
 * showed is the only one changed: a name that now leads to another file
 * (a link, a socket, another file) is refused as replaced and nothing is
 * changed.  @seen keeps the sentence true when the dialog was opened on a
 * link: if only the file the link leads to was replaced, the user reads
 * "another file", not "a link".  Not known never says "a link".
 */
int wpe_fileop_chmod_shown(const char *path, const WpeSeen *shown, const WpeNameSeen *seen,
                           mode_t perm, WpeFileOpOutcome *o)
{
 WpeModeKept kept;
 int err;

 fileop_begin(o, WPE_FILEOP_CHMOD, path, "");
 err = wpe_write_set_perm_seen(path, shown, seen, perm, &kept);
 if (err != 0)
  return(fileop_chmod_failed(o, &kept, err));
 fileop_hook_refuse_special(&kept, kept.asked, EPERM);
 fileop_note_bits_lost(&o->note, path, &kept);
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

/* --- folders -------------------------------------------------------------- */

/* A folder operation takes the folder whole.  One walker (fileop_walk)
   enters every folder, for Copy, Move, Link, Remove and for the count that
   comes first.  The two folders the user named are opened once, from the
   paths typed; below them every folder is opened by its name in its parent
   with O_DIRECTORY | O_NOFOLLOW, and every entry is judged and used by that
   folder's descriptor and its name, never by a path.  So a link, wherever
   it sits (source or destination) and whenever it appears, is a name:
   copied, moved, linked or removed as a name, never entered and never
   written through.  The paths of a walk are words, for the progress line
   and the sentence (tests/check-write-owner.sh, rule 11). */

typedef struct {
 const WpeFileOpPolicy *p;
 WpeFileOpOutcome      *o;
 WpeFileOpKind          kind;
 int                    counting;   /* the walk only counts, and changes nothing */
 long                   done, skipped, total;
 WpeFileOpBitsNote      note;       /* the entries that did not keep a special bit */
} FileTree;

/** One folder, or one entry, of a walk: where it is and where it goes. */
typedef struct {
 int  spfd;                   /* the open folder that holds the source */
 int  dpfd;                   /* the open folder that holds the destination, or -1 */
 int  sfd;                    /* the source folder itself, once opened */
 int  dfd;                    /* the destination folder itself, once opened (-1: not yet) */
 int  made;                   /* this walk made the destination folder */
 int  follow;                 /* the top level source is a link to follow, once */
 int  depth;                  /* folders above this one */
 char sname[WPE_NAME_SIZE];   /* the source's name in spfd */
 char dname[WPE_NAME_SIZE];   /* the destination's name in dpfd */
 char spath[WPE_PATH_SIZE];   /* words: the source's path */
 char dpath[WPE_PATH_SIZE];   /* words: the destination's path, "" for a Remove */
} WalkLevel;

/** The names of one folder, sorted, without "." and "..". */
typedef struct {
 char **name;
 int    n;
} NameList;

/** fileop_tree_verb - The word the progress callback is given. */
static const char *fileop_tree_verb(WpeFileOpKind kind)
{
 switch (kind)
 {
 case WPE_FILEOP_MOVE:   return("Move");
 case WPE_FILEOP_LINK:   return("Link");
 case WPE_FILEOP_REMOVE: return("Remove");
 default:                return("Copy");
 }
}

/** fileop_names_free - Give back a list of names. */
static void fileop_names_free(NameList *l)
{
 int i;

 for (i = 0; i < l->n; i++)
  free(l->name[i]);
 free(l->name);
 l->name = NULL;
 l->n = 0;
}

static int fileop_names_cmp(const void *a, const void *b)
{
 return(strcmp(*(char *const *)a, *(char *const *)b));
}

/**
 * fileop_names_read - Read all the names of an open folder, before any is
 * changed, so that removing entries does not disturb the reading.
 * Return: 0, or the errno.
 */
static int fileop_names_read(DIR *d, NameList *l)
{
 struct dirent *e;
 char **grown;
 int cap = 0;

 l->name = NULL;
 l->n = 0;
 for (;;)
 {
  errno = 0;
  e = readdir(d);
  if (e == NULL)
   break;
  if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
   continue;
  if (l->n == cap)
  {
   cap = (cap != 0 ? 2 * cap : 32);
   grown = realloc(l->name, (size_t)cap * sizeof *grown);
   if (grown == NULL)
   {
    fileop_names_free(l);
    return(ENOMEM);
   }
   l->name = grown;
  }
  l->name[l->n] = strdup(e->d_name);
  if (l->name[l->n] == NULL)
  {
   fileop_names_free(l);
   return(ENOMEM);
  }
  l->n++;
 }
 if (errno != 0)
 {
  int err = errno;

  fileop_names_free(l);
  return(err);
 }
 if (l->n > 1)
  qsort(l->name, (size_t)l->n, sizeof *l->name, fileop_names_cmp);
 return(0);
}

/**
 * fileop_tree_fail - A step of a folder operation failed on an entry.
 * Return: -1.
 */
static int fileop_tree_fail(FileTree *t, WpeFileOpStep step, int err, const char *src, const char *dst)
{
 snprintf(t->o->src, sizeof t->o->src, "%s", src);
 snprintf(t->o->dst, sizeof t->o->dst, "%s", dst);
 t->o->src_kept = 0;
 t->o->dst_kept = 0;
 return(fileop_fail(t->o, step, err));
}

/** fileop_tree_stop - The user ended the folder operation.  Return: -1. */
static int fileop_tree_stop(FileTree *t, const char *src, const char *dst)
{
 return(fileop_tree_fail(t, WPE_FILEOP_STEP_STOPPED, 0, src, dst));
}

/** fileop_tree_not_a_folder - @path is not the folder it should be.  Return: -1. */
static int fileop_tree_not_a_folder(WpeFileOpOutcome *o, const char *path)
{
 snprintf(o->bad, sizeof o->bad, "%s", path);
 return(fileop_fail(o, WPE_FILEOP_STEP_NOT_A_FOLDER, 0));
}

/**
 * fileop_names_read_fd - Read all the names of the open folder @fd.
 * Return: 0, or the errno.
 *
 * The folder is read through a copy of the descriptor, which the reading
 * closes; @fd stays open for the calls that name entries relative to it.
 */
static int fileop_names_read_fd(int fd, NameList *l)
{
 DIR *d;
 int err, copy = dup(fd);

 if (copy < 0)
  return(errno);
 d = fdopendir(copy);
 if (d == NULL)
 {
  err = errno;
  close(copy);
  return(err);
 }
 err = fileop_names_read(d, l);
 closedir(d);
 return(err);
}

/**
 * fileop_tree_inside - Is @dst the folder @src or a place inside it?
 * Return: 0 no, 1 the same folder, 2 inside it.
 *
 * Judged on the real paths, so a link in the way does not hide it.  A
 * destination that is not there yet is its folder's real path and its name.
 */
static int fileop_tree_inside(const char *src, const char *dst)
{
 char rs[WPE_PATH_SIZE], rd[WPE_PATH_SIZE], dir[WPE_PATH_SIZE], parent[WPE_PATH_SIZE];
 char name[WPE_NAME_SIZE];
 size_t len;

 if (realpath(src, rs) == NULL)
  return(0);
 if (realpath(dst, rd) == NULL)
 {
  if (fileop_split(dst, dir, sizeof dir, name, sizeof name) != 0 || realpath(dir, parent) == NULL ||
      wpe_path_join(rd, sizeof rd, parent, name) != 0)
   return(0);
 }
 len = strlen(rs);
 if (strcmp(rs, rd) == 0)
  return(1);
 if (strncmp(rd, rs, len) == 0 && (len == 1 || rd[len] == '/'))
  return(2);
 return(0);
}

/**
 * fileop_walk_fail - A step of the walk failed on the folder or entry @lv.
 * Return: -1.
 *
 * The count that comes first says why it failed and leaves the paths of the
 * outcome as the user typed them.
 */
static int fileop_walk_fail(FileTree *t, WpeFileOpStep step, int err, const WalkLevel *lv)
{
 if (t->counting)
  return(fileop_fail(t->o, step, err));
 return(fileop_tree_fail(t, step, err, lv->spath, lv->dpath));
}

/** fileop_level_init - A level that holds nothing yet. */
static void fileop_level_init(WalkLevel *lv, int depth)
{
 lv->spfd = -1;
 lv->dpfd = -1;
 lv->sfd = -1;
 lv->dfd = -1;
 lv->made = 0;
 lv->follow = 0;
 lv->depth = depth;
 lv->sname[0] = '\0';
 lv->dname[0] = '\0';
 lv->spath[0] = '\0';
 lv->dpath[0] = '\0';
}

/** fileop_level_close - Close the folders this level opened itself. */
static void fileop_level_close(WalkLevel *lv)
{
 if (lv->sfd >= 0)
  close(lv->sfd);
 if (lv->dfd >= 0)
  close(lv->dfd);
 lv->sfd = -1;
 lv->dfd = -1;
}

/** fileop_level_release - Close the folders the top level was handed. */
static void fileop_level_release(WalkLevel *lv)
{
 if (lv->spfd >= 0)
  close(lv->spfd);
 if (lv->dpfd >= 0)
  close(lv->dpfd);
 lv->spfd = -1;
 lv->dpfd = -1;
}

/**
 * fileop_level_child - Make the level of the entry @name of the open folder @lv.
 * Return: 0, or -1 when a path would be too long.
 */
static int fileop_level_child(WalkLevel *c, const WalkLevel *lv, const char *name)
{
 int bad;

 fileop_level_init(c, lv->depth + 1);
 c->spfd = lv->sfd;
 c->dpfd = lv->dfd;
 snprintf(c->sname, sizeof c->sname, "%s", name);
 snprintf(c->dname, sizeof c->dname, "%s", name);
 bad = wpe_path_join(c->spath, sizeof c->spath, lv->spath, name);
 if (lv->dpath[0] != '\0' && wpe_path_join(c->dpath, sizeof c->dpath, lv->dpath, name) != 0)
  bad = 1;
 return(bad != 0 ? -1 : 0);
}

/**
 * fileop_entry_changed - Is the entry another kind than it was judged to be, or gone?
 * @judged: its mode when it was judged.
 */
static int fileop_entry_changed(int pfd, const char *name, mode_t judged)
{
 struct stat now;

 if (fstatat(pfd, name, &now, AT_SYMLINK_NOFOLLOW) != 0)
  return(errno == ENOENT);
 return((now.st_mode & S_IFMT) != (judged & S_IFMT));
}

/**
 * fileop_walk_source_failed - The source entry could not be opened after it
 * was judged.
 * @judged: its mode when it was judged; @err: why the open failed.
 * Return: -1 (outcome set).
 *
 * When it is now another kind of thing, or gone, someone changed the folder
 * while it was being walked: the walk stops, says so, and has touched
 * nothing it was not given.  Otherwise it is the plain error.
 */
static int fileop_walk_source_failed(FileTree *t, const WalkLevel *lv, mode_t judged, int err)
{
 if (!lv->follow && fileop_entry_changed(lv->spfd, lv->sname, judged))
  return(fileop_walk_fail(t, WPE_FILEOP_STEP_CHANGED, err, lv));
 return(fileop_walk_fail(t, WPE_FILEOP_STEP_SOURCE, err, lv));
}

/**
 * fileop_folder_open - Open the source folder by its name in its parent
 * without following a link.
 * Return: 0, or -1 (outcome set).
 */
static int fileop_folder_open(FileTree *t, WalkLevel *lv)
{
 int nofollow = (lv->follow ? 0 : O_NOFOLLOW);

 lv->sfd = openat(lv->spfd, lv->sname, O_RDONLY | O_DIRECTORY | O_CLOEXEC | nofollow);
 if (lv->sfd < 0)
  return(fileop_walk_source_failed(t, lv, S_IFDIR, errno));
 return(0);
}

/**
 * fileop_folder_dst_failed - The destination folder could not be opened.
 * @err: why.
 * Return: -1 (outcome set).
 *
 * Only a name that is not a folder, a link included, says "is not a
 * folder"; a folder that cannot be searched or a full table of open files
 * says what it is.
 */
static int fileop_folder_dst_failed(FileTree *t, const WalkLevel *lv, int err)
{
 struct stat st;

 if (fstatat(lv->dpfd, lv->dname, &st, AT_SYMLINK_NOFOLLOW) == 0 && !S_ISDIR(st.st_mode))
  return(fileop_tree_not_a_folder(t->o, lv->dpath));
 return(fileop_walk_fail(t, WPE_FILEOP_STEP_MKDIR, err, lv));
}

/**
 * fileop_folder_enter_dst - Make the folder an entry's contents go into, or
 * merge into the one that is there, and open it.
 * Return: 0, or -1 (outcome set).
 *
 * The kernel judges the name: it is opened without following a link, so a
 * link in the destination is not a folder to merge into and nothing is
 * written through it.  A new folder is made private; it gets the source's
 * permissions when it is filled.
 */
static int fileop_folder_enter_dst(FileTree *t, WalkLevel *lv)
{
 if (mkdirat(lv->dpfd, lv->dname, 0700) == 0)
  lv->made = 1;
 else if (errno != EEXIST)
  return(fileop_walk_fail(t, WPE_FILEOP_STEP_MKDIR, errno, lv));
 lv->dfd = openat(lv->dpfd, lv->dname, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
 if (lv->dfd < 0)
  return(fileop_folder_dst_failed(t, lv, errno));
 return(0);
}

/**
 * fileop_root_dst_follow - The destination the user typed is a link: it is
 * the folder it points to, once.
 * Return: 0, or -1 (outcome set).
 *
 * A link to a file or to nothing is not a folder to merge into.
 */
static int fileop_root_dst_follow(FileTree *t, WalkLevel *lv)
{
 int err;

 lv->dfd = openat(lv->dpfd, lv->dname, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
 if (lv->dfd >= 0)
  return(0);
 err = errno;
 if (err == ENOTDIR || err == ENOENT || err == ELOOP)
  return(fileop_tree_not_a_folder(t->o, lv->dpath));
 return(fileop_walk_fail(t, WPE_FILEOP_STEP_MKDIR, err, lv));
}

/**
 * fileop_root_dst_open - Open the destination folder the user named: made
 * when it is not there, merged into when it is, followed once when it is a
 * link to a folder.
 * Return: 0, or -1 (outcome set).
 *
 * The one name where a link means "the folder it points to" for a whole
 * folder operation; below it a link is only a name.
 */
static int fileop_root_dst_open(FileTree *t, WalkLevel *lv)
{
 struct stat st;
 int is_link = (fstatat(lv->dpfd, lv->dname, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode));

 if (is_link)
  return(fileop_root_dst_follow(t, lv));
 return(fileop_folder_enter_dst(t, lv));
}

/**
 * fileop_folder_finish_dst - Give a folder this operation made the source's
 * permissions, setgid included, once it is filled (a read only folder can
 * be filled first).
 * @quiet: the operation already ended; try, and say nothing more.
 * Return: 0, or -1 (outcome set).
 *
 * A folder that was there is left as the user had it.
 */
static int fileop_folder_finish_dst(FileTree *t, const WalkLevel *lv, int quiet)
{
 struct stat st;
 WpeModeKept kept;
 int err;

 if (!lv->made)
  return(0);
 if (fstat(lv->sfd, &st) != 0)
  return(quiet ? 0 : fileop_walk_fail(t, WPE_FILEOP_STEP_SOURCE, errno, lv));
 err = wpe_write_set_mode_fd(lv->dfd, st.st_mode, &kept);
 if (err != 0)
  return(quiet ? 0 : fileop_walk_fail(t, WPE_FILEOP_STEP_MODE, err, lv));
 fileop_hook_refuse_special(&kept, st.st_mode, EPERM);
 fileop_note_bits_lost(&t->note, lv->dpath, &kept);
 return(0);
}

/**
 * fileop_folder_remove_source - Remove an emptied source folder, by its name
 * in its parent.
 * Return: 0, or -1 (outcome set).
 *
 * A name that is no longer a folder (swapped for a link meanwhile) is not
 * removed; the walk says it changed.
 */
static int fileop_folder_remove_source(FileTree *t, const WalkLevel *lv)
{
 int err;

 if (unlinkat(lv->spfd, lv->sname, AT_REMOVEDIR) == 0)
  return(0);
 err = errno;
 if (fileop_entry_changed(lv->spfd, lv->sname, S_IFDIR))
  return(fileop_walk_fail(t, WPE_FILEOP_STEP_CHANGED, err, lv));
 return(fileop_walk_fail(t, WPE_FILEOP_STEP_RMDIR, err, lv));
}

/**
 * fileop_folder_rename_whole - Move a folder to a name that is free with one rename.
 * @renamed: receives 1 when it was done.
 * Return: 0 (renamed, or the folder has to be walked: another file system,
 * or a destination that is there), -1 (outcome set).
 *
 * The rename cannot replace a folder that appeared meanwhile, which
 * rename(2) would do silently for an empty one.  A folder moved on its own
 * file system to a free name is one rename, whatever its depth.
 */
static int fileop_folder_rename_whole(FileTree *t, const WalkLevel *lv, int *renamed)
{
 struct stat st;
 int err;

 *renamed = 0;
 if (fstatat(lv->dpfd, lv->dname, &st, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT)
  return(0);
 err = fileop_rename_noreplace_at(lv->spfd, lv->sname, lv->dpfd, lv->dname);
 if (err == 0)
 {
  *renamed = 1;
  return(0);
 }
 if (err == EXDEV || err == EEXIST)
  return(0);
 return(fileop_walk_fail(t, WPE_FILEOP_STEP_RENAME, err, lv));
}

/** fileop_entry_copy_link - Make the link (j->spfd, j->sname) again at the destination. */
static int fileop_entry_copy_link(FileJob *j)
{
 int rc = fileop_job_judge_dst(j);

 if (rc == 0)
  rc = fileop_job_copy_link_in(j);
 return(rc == 0 ? 0 : -1);
}

/**
 * fileop_entry_transfer - One entry of a folder copied, moved or linked.
 * @j: the job, aimed at the entry and its destination, @j->sst judged;
 * @kind: what the operation is; @st: what the entry is, not following links.
 * Return: 0, or -1 (outcome set).
 *
 * A link inside a copied folder is copied as a link, never as what it points to.
 */
static int fileop_entry_transfer(FileJob *j, WpeFileOpKind kind, const struct stat *st)
{
 if (kind == WPE_FILEOP_MOVE)
  return(fileop_entry_move(j));
 if (kind == WPE_FILEOP_LINK)
  return(fileop_entry_link(j));
 if (S_ISLNK(st->st_mode))
  return(fileop_entry_copy_link(j));
 return(fileop_entry_copy(j));
}

/**
 * fileop_walk_progress - Tell the progress callback what comes next, and
 * stop when the user said to.
 * Return: 0 to go on, -1 when the operation was stopped (outcome set).
 *
 * Asked before each entry and before entering each subfolder, so a tree of
 * many empty folders can be stopped too; never for the top folder and
 * never while only counting.
 */
static int fileop_walk_progress(FileTree *t, const WalkLevel *lv)
{
 if (t->counting || t->p->progress == NULL)
  return(0);
 if (!t->p->progress(t->p->ctx, fileop_tree_verb(t->kind), lv->spath, lv->dpath))
  return(0);
 return(fileop_tree_stop(t, lv->spath, lv->dpath));
}

/**
 * fileop_walk_transfer - Copy, move or link one entry that is not a folder.
 * @st: what the entry was judged to be.
 * Return: 0 done, 1 left alone (the policy or the user said no), -1 the
 * operation ended (outcome set).
 */
static int fileop_walk_transfer(FileTree *t, const WalkLevel *ent, const struct stat *st)
{
 FileJob j;
 WpeFileOpOutcome e;
 int rc;

 fileop_job_begin(&j, t->kind, ent->spath, ent->dpath, t->p, &e);
 fileop_job_aim(&j, ent->spfd, ent->sname, ent->dpfd, ent->dname);
 j.sst = *st;
 rc = fileop_entry_transfer(&j, t->kind, st);
 if (rc == 0)
 {
  t->done++;
  fileop_note_merge(&t->note, &e.note);
  return(0);
 }
 if (e.status == WPE_FILEOP_DECLINED)
 {
  t->skipped++;
  return(1);
 }
 if (e.step == WPE_FILEOP_STEP_CANCELLED)
  return(fileop_tree_stop(t, ent->spath, ent->dpath));
 if (e.step == WPE_FILEOP_STEP_SOURCE)
  return(fileop_walk_source_failed(t, ent, st->st_mode, e.err));
 fileop_note_merge(&t->note, &e.note);
 *t->o = e;
 return(-1);
}

/**
 * fileop_walk_remove - Remove one entry that is not a folder, by its name in
 * its folder.
 * Return: 0 done, 1 left alone (the answer was no), -1 ended (outcome set).
 *
 * A link, a pipe, a socket and a device are names: they are unlinked, never
 * opened or followed.
 */
static int fileop_walk_remove(FileTree *t, const WalkLevel *ent)
{
 int answer = fileop_remove_answer(t->p, ent->spath);

 if (answer == WPE_FILEOP_ANSWER_STOP)
  return(fileop_tree_stop(t, ent->spath, ""));
 if (answer != WPE_FILEOP_ANSWER_YES)
 {
  t->skipped++;
  return(1);
 }
 if (unlinkat(ent->spfd, ent->sname, 0) != 0)
  return(fileop_tree_fail(t, WPE_FILEOP_STEP_REMOVE, errno, ent->spath, ""));
 t->done++;
 return(0);
}

/**
 * fileop_walk_entry - One entry that is not a folder: counted, or done.
 * Return: 0 done, 1 left alone, -1 the operation ended (outcome set).
 */
static int fileop_walk_entry(FileTree *t, const WalkLevel *ent, const struct stat *st)
{
 if (t->counting)
 {
  t->total++;
  return(0);
 }
 if (fileop_walk_progress(t, ent) != 0)
  return(-1);
 if (t->kind == WPE_FILEOP_REMOVE)
  return(fileop_walk_remove(t, ent));
 return(fileop_walk_transfer(t, ent, st));
}

static int fileop_walk(FileTree *t, WalkLevel *lv, int *kept);

/** fileop_walk_descends - Does this operation go into subfolders?  Remove always does. */
static int fileop_walk_descends(const FileTree *t)
{
 return(t->kind == WPE_FILEOP_REMOVE || t->p->recursive);
}

/**
 * fileop_walk_subfolder - Enter one subfolder of the folder being walked.
 * @ent: the subfolder's level; @kept: set when something stays where it was.
 * Return: 0, or -1 (outcome set).
 */
static int fileop_walk_subfolder(FileTree *t, WalkLevel *ent, int *kept)
{
 int k = 0;

 if (!fileop_walk_descends(t))
 {
  if (!t->counting)
   *kept = 1;
  return(0);
 }
 if (fileop_walk_progress(t, ent) != 0)
  return(-1);
 if (fileop_walk(t, ent, &k) != 0)
  return(-1);
 if (k)
  *kept = 1;
 return(0);
}

/**
 * fileop_walk_names - The entries of an open source folder, in order.
 * @l: its names; @kept: set when something stays where it was.
 * Return: 0, or -1 (outcome set).
 *
 * Each entry is judged by what it is, without following it, through the
 * folder's descriptor; the judgement is used as it stands.
 */
static int fileop_walk_names(FileTree *t, const WalkLevel *lv, const NameList *l, int *kept)
{
 WalkLevel ent;
 struct stat st;
 int i, rc;

 for (i = 0; i < l->n; i++)
 {
  if (fileop_level_child(&ent, lv, l->name[i]) != 0)
   return(fileop_walk_fail(t, WPE_FILEOP_STEP_CREATE, ENAMETOOLONG, lv));
  if (fstatat(lv->sfd, l->name[i], &st, AT_SYMLINK_NOFOLLOW) != 0)
   return(fileop_walk_fail(t, WPE_FILEOP_STEP_SOURCE, errno, &ent));
  if (S_ISDIR(st.st_mode))
   rc = fileop_walk_subfolder(t, &ent, kept);
  else
   rc = fileop_walk_entry(t, &ent, &st);
  if (rc < 0)
   return(-1);
  if (rc > 0)
   *kept = 1;
 }
 return(0);
}

/**
 * fileop_walk_count_renamed - Count what a whole rename moved, through the
 * descriptor of the folder, which still names it after its name is gone.
 */
static void fileop_walk_count_renamed(FileTree *t, WalkLevel *lv)
{
 FileTree c = *t;
 WpeFileOpOutcome scratch;
 WpeFileOpPolicy deep = *t->p;
 NameList l;
 int kept = 0;

 deep.recursive = 1;
 c.p = &deep;
 c.o = &scratch;
 c.counting = 1;
 c.total = 0;
 if (fileop_names_read_fd(lv->sfd, &l) != 0)
  return;
 if (fileop_walk_names(&c, lv, &l, &kept) == 0)
  t->done += c.total;
 fileop_names_free(&l);
}

/**
 * fileop_walk_start - What comes before a folder's entries.
 * @renamed: receives 1 when a Move did the whole folder with one rename.
 * Return: 0, or -1 (outcome set).
 *
 * A subfolder that is moved to a free name on its file system is one
 * rename; otherwise the destination is made or merged into.
 */
static int fileop_walk_start(FileTree *t, WalkLevel *lv, int *renamed)
{
 *renamed = 0;
 if (t->counting || t->kind == WPE_FILEOP_REMOVE)
  return(0);
 if (t->kind == WPE_FILEOP_MOVE && lv->depth > 0)
 {
  if (fileop_folder_rename_whole(t, lv, renamed) != 0)
   return(-1);
  if (*renamed)
  {
   fileop_walk_count_renamed(t, lv);
   return(0);
  }
 }
 if (lv->dfd >= 0)
  return(0);
 return(fileop_folder_enter_dst(t, lv));
}

/**
 * fileop_walk_entries - Do every entry of the open source folder.
 * Return: 0, or -1 (outcome set; a folder this walk made is still given
 * its permissions, quietly).
 */
static int fileop_walk_entries(FileTree *t, WalkLevel *lv, int *kept)
{
 NameList l;
 int rc, err = fileop_names_read_fd(lv->sfd, &l);

 if (err != 0)
  rc = fileop_walk_fail(t, WPE_FILEOP_STEP_SOURCE, err, lv);
 else
 {
  rc = fileop_walk_names(t, lv, &l, kept);
  fileop_names_free(&l);
 }
 if (rc != 0 && !t->counting)
  (void)fileop_folder_finish_dst(t, lv, 1);
 return(rc);
}

/**
 * fileop_walk_finish - What comes after a folder's entries: its permissions
 * on a folder this walk made, and a moved or removed source folder taken
 * away once it is empty.
 * @here_kept: something stays in it.
 * Return: 0, or -1 (outcome set).
 */
static int fileop_walk_finish(FileTree *t, WalkLevel *lv, int here_kept)
{
 if (t->counting)
  return(0);
 if (fileop_folder_finish_dst(t, lv, 0) != 0)
  return(-1);
 if ((t->kind == WPE_FILEOP_MOVE || t->kind == WPE_FILEOP_REMOVE) && !here_kept)
  return(fileop_folder_remove_source(t, lv));
 return(0);
}

/**
 * fileop_walk - Copy, move, link, remove or count one folder and what is in it.
 * @lv: the folder, as a name in its parent, and where it goes.
 * @kept: set when an entry stays where it was (left alone, or not walked
 * because the options are not recursive).
 * Return: 0, or -1 (outcome set).
 *
 * The only function that enters a folder.  It opens the folder by its name
 * with O_NOFOLLOW and keeps it open, so what is used afterwards is the
 * folder that was judged, whatever happens to its name meanwhile.
 */
static int fileop_walk(FileTree *t, WalkLevel *lv, int *kept)
{
 int rc, here_kept = 0, renamed = 0;

 if (lv->depth > WPE_FILEOP_MAX_DEPTH)
  return(fileop_walk_fail(t, WPE_FILEOP_STEP_TOO_DEEP, ELOOP, lv));
 if (fileop_folder_open(t, lv) != 0)
 {
  fileop_level_close(lv);
  return(-1);
 }
 rc = fileop_walk_start(t, lv, &renamed);
 if (rc == 0 && !renamed)
  rc = fileop_walk_entries(t, lv, &here_kept);
 if (rc == 0 && !renamed)
  rc = fileop_walk_finish(t, lv, here_kept);
 fileop_level_close(lv);
 if (rc == 0 && here_kept)
  *kept = 1;
 return(rc);
}

/**
 * fileop_tree_conclude - Set the counts and the status of a folder operation.
 * @rc: what the walk returned; @kept: something stayed where it was.
 * Return: 0 for DONE, -1 otherwise.
 */
static int fileop_tree_conclude(FileTree *t, int rc, int kept)
{
 WpeFileOpOutcome *o = t->o;

 o->done = t->done;
 o->skipped = t->skipped;
 o->total = t->total;
 o->note = t->note;
 if (rc != 0)
  return(-1);
 if (t->skipped > 0)
 {
  o->src_kept = 1;
  o->dst_kept = 1;
  return(fileop_decline(o, WPE_FILEOP_STEP_EXISTS));
 }
 o->src_kept = 1;
 if ((t->kind == WPE_FILEOP_MOVE || t->kind == WPE_FILEOP_REMOVE) && !kept)
  o->src_kept = 0;
 o->dst_kept = 0;
 return(0);
}

/**
 * fileop_root_source_open - Open the folder the user named as the source,
 * and check that it is one.
 * @src: what the user typed.
 * Return: 0, or -1 (outcome set).
 *
 * The folder's own folder is opened by the typed path once.  A Copy or a
 * Link takes a link to a folder as the folder it points to, once (the
 * DirTree shows it as a folder); a Move or a Remove takes a link as a name,
 * as mv and rm do, so `lnk/` with a link `lnk` is refused, not followed.
 */
static int fileop_root_source_open(FileTree *t, WalkLevel *lv, const char *src)
{
 struct stat st;
 int err = fileop_parent_open(src, &lv->spfd, lv->sname, sizeof lv->sname);
 int follows = (t->kind == WPE_FILEOP_COPY || t->kind == WPE_FILEOP_LINK);

 if (err != 0)
  return(fileop_fail(t->o, WPE_FILEOP_STEP_SOURCE, err));
 snprintf(lv->spath, sizeof lv->spath, "%s", src);
 lv->follow = follows;
 if (fstatat(lv->spfd, lv->sname, &st, follows ? 0 : AT_SYMLINK_NOFOLLOW) != 0)
  return(fileop_fail(t->o, WPE_FILEOP_STEP_SOURCE, errno));
 if (!S_ISDIR(st.st_mode))
  return(fileop_tree_not_a_folder(t->o, src));
 return(0);
}

/**
 * fileop_root_dst_parent - Open the folder that holds the destination the
 * user named.
 * Return: 0, or -1 (outcome set).
 */
static int fileop_root_dst_parent(FileTree *t, WalkLevel *lv, const char *dst)
{
 int err = fileop_parent_open(dst, &lv->dpfd, lv->dname, sizeof lv->dname);

 if (err != 0)
  return(fileop_fail(t->o, WPE_FILEOP_STEP_MKDIR, err));
 snprintf(lv->dpath, sizeof lv->dpath, "%s", dst);
 return(0);
}

/**
 * fileop_tree_roots - Open the two folders the user named, and refuse a
 * folder put inside itself.
 * Return: 0, or -1 (outcome set).
 *
 * The only paths a folder operation resolves.
 */
static int fileop_tree_roots(FileTree *t, WalkLevel *lv, const char *src, const char *dst)
{
 int inside;

 if (fileop_root_source_open(t, lv, src) != 0)
  return(-1);
 if (t->kind == WPE_FILEOP_REMOVE)
  return(0);
 inside = fileop_tree_inside(src, dst);
 if (inside != 0)
  return(fileop_fail(t->o, inside == 1 ? WPE_FILEOP_STEP_SAME_FILE : WPE_FILEOP_STEP_INSIDE, 0));
 return(fileop_root_dst_parent(t, lv, dst));
}

/**
 * fileop_tree_count - How many entries that are not folders the tree holds.
 * Return: 0, or -1 (outcome set: TOO_DEEP for a tree deeper than the limit).
 *
 * Asked before anything is changed, so a tree that is too deep or cannot
 * be read is refused whole and the progress can say "n of total".
 */
static int fileop_tree_count(FileTree *t, const WalkLevel *root)
{
 FileTree c = *t;
 WalkLevel lv = *root;
 int kept = 0;

 c.counting = 1;
 c.total = 0;
 if (fileop_walk(&c, &lv, &kept) != 0)
  return(-1);
 t->total = c.total;
 return(0);
}

/**
 * fileop_tree_link_text - Make the source of a Link the folder's real path.
 *
 * The links of a folder of links sit in another folder: a relative source
 * would point somewhere else from there.
 */
static void fileop_tree_link_text(WalkLevel *lv)
{
 char whole[WPE_PATH_SIZE];

 if (realpath(lv->spath, whole) != NULL)
  snprintf(lv->spath, sizeof lv->spath, "%s", whole);
}

/**
 * fileop_tree_work - What a folder operation does once its two folders are open.
 * @kept: set when something stays where it was.
 * Return: 0 done, 1 done by one rename (the outcome is final), -1 failed.
 */
static int fileop_tree_work(FileTree *t, WalkLevel *lv, int *kept)
{
 int renamed = 0;

 if (t->kind == WPE_FILEOP_MOVE)
 {
  if (fileop_folder_rename_whole(t, lv, &renamed) != 0)
   return(-1);
  if (renamed)
   return(1);
 }
 if (fileop_tree_count(t, lv) != 0)
 {
  t->total = 0;
  return(-1);
 }
 if (t->kind != WPE_FILEOP_REMOVE && fileop_root_dst_open(t, lv) != 0)
  return(-1);
 if (t->kind == WPE_FILEOP_LINK)
  fileop_tree_link_text(lv);
 return(fileop_walk(t, lv, kept));
}

/**
 * fileop_tree_run - Copy, move, link or remove a folder: the checks, the
 * count, the walk.
 * @dst: "" for a Remove.
 * Return: 0 for DONE, -1 otherwise.
 */
static int fileop_tree_run(WpeFileOpKind kind, const char *src, const char *dst,
                           const WpeFileOpPolicy *p, WpeFileOpOutcome *o)
{
 FileTree t;
 WalkLevel lv;
 int rc, kept = 0;

 fileop_begin(o, kind, src, dst);
 memset(&t, 0, sizeof t);
 t.p = p;
 t.o = o;
 t.kind = kind;
 fileop_level_init(&lv, 0);
 if (fileop_tree_roots(&t, &lv, src, dst) != 0)
 {
  fileop_level_release(&lv);
  return(-1);
 }
 rc = fileop_tree_work(&t, &lv, &kept);
 fileop_level_release(&lv);
 if (rc > 0)
 {
  o->src_kept = 0;
  o->dst_kept = 0;
  return(0);
 }
 return(fileop_tree_conclude(&t, rc, kept));
}

int wpe_fileop_copy_tree(const char *src, const char *dst, const WpeFileOpPolicy *p, WpeFileOpOutcome *o)
{
 return(fileop_tree_run(WPE_FILEOP_COPY, src, dst, p, o));
}

int wpe_fileop_move_tree(const char *src, const char *dst, const WpeFileOpPolicy *p, WpeFileOpOutcome *o)
{
 return(fileop_tree_run(WPE_FILEOP_MOVE, src, dst, p, o));
}

int wpe_fileop_link_tree(const char *src, const char *dst, const WpeFileOpPolicy *p, WpeFileOpOutcome *o)
{
 return(fileop_tree_run(WPE_FILEOP_LINK, src, dst, p, o));
}

int wpe_fileop_remove_tree(const char *path, const WpeFileOpPolicy *p, WpeFileOpOutcome *o)
{
 return(fileop_tree_run(WPE_FILEOP_REMOVE, path, "", p, o));
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

static const char *fileop_past(WpeFileOpKind kind);

/**
 * fileop_replaced_words - Say what took the name of a file whose permissions
 * were being changed, and whether anything was changed.
 * @out, @n: receive the words; @o: the outcome.
 *
 * A link that took the name while no call changed anything: the call never
 * follows a link, so the file it leads to is as it was and the sentence
 * says so.  When a call did succeed (the BSDs and macOS give a link its own
 * permissions), what held the name may have been changed and the sentence
 * says that, whatever holds the name now.
 */
static void fileop_replaced_words(char *out, size_t n, const WpeFileOpOutcome *o)
{
 if (o->replaced_unsure && o->src_kept)
  snprintf(out, n, "%s was changed or replaced", o->src);
 else if (o->replaced_unsure)
  snprintf(out, n, "%s was changed or replaced while its permissions were being changed; "
           "the file now under that name may have been changed", o->src);
 else if (o->replaced_by_link && o->src_kept)
  snprintf(out, n, "%s was replaced by a link; the file it leads to was not changed", o->src);
 else if (o->replaced_by_link)
  snprintf(out, n, "%s was replaced by a link while its permissions were being changed; "
           "what held the name may have been changed", o->src);
 else if (o->src_kept)
  snprintf(out, n, "%s was replaced by another file", o->src);
 else
  snprintf(out, n, "%s was replaced by another file; the file now under that name may have been changed",
           o->src);
}

/** fileop_reason - The reason half of the sentence, for a FAILED outcome. */
static void fileop_reason(char *out, size_t n, const WpeFileOpOutcome *o)
{
 const char *step = fileop_step_text(o->step);

 if (o->step == WPE_FILEOP_STEP_SAME_FILE)
  snprintf(out, n, "%s and %s are the same file", o->src, o->dst);
 else if (o->step == WPE_FILEOP_STEP_NOT_A_FILE)
  snprintf(out, n, "%s is not a file", o->bad);
 else if (o->step == WPE_FILEOP_STEP_NOT_A_FOLDER)
  snprintf(out, n, "%s is not a folder", o->bad);
 else if (o->step == WPE_FILEOP_STEP_INSIDE)
  snprintf(out, n, "%s is inside %s", o->dst, o->src);
 else if (o->step == WPE_FILEOP_STEP_TOO_DEEP)
  snprintf(out, n, "%s is deeper than %d folders", o->src, WPE_FILEOP_MAX_DEPTH);
 else if (o->step == WPE_FILEOP_STEP_CHANGED && o->kind == WPE_FILEOP_CHMOD)
  fileop_replaced_words(out, n, o);
 else if (o->step == WPE_FILEOP_STEP_CHANGED)
  snprintf(out, n, "%s changed while the folder was being %s", o->src, fileop_past(o->kind));
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

/** fileop_past - What was done to the entries of a folder, for the counts. */
static const char *fileop_past(WpeFileOpKind kind)
{
 switch (kind)
 {
 case WPE_FILEOP_MOVE:   return("moved");
 case WPE_FILEOP_LINK:   return("linked");
 case WPE_FILEOP_REMOVE: return("removed");
 default:                return("copied");
 }
}

/** fileop_left - What is left, for a FAILED outcome; "" when nothing needs saying. */
static void fileop_left(char *out, size_t n, const WpeFileOpOutcome *o)
{
 out[0] = '\0';
 if (o->total > 0)
  snprintf(out, n, "  %ld of %ld entries were %s.", o->done, o->total, fileop_past(o->kind));
 else if (o->step == WPE_FILEOP_STEP_REMOVE_SOURCE ||
     (o->step == WPE_FILEOP_STEP_CLOSE && o->kind == WPE_FILEOP_MOVE && !o->dst_kept))
  snprintf(out, n, "  %s holds the copy; %s is still in place.", o->dst, o->src);
 else if (o->aside[0] != '\0')
  snprintf(out, n, "  %s is in the wastebasket as %s.", o->dst, o->aside);
 else if (o->src_kept && o->dst_kept)
  snprintf(out, n, "  Nothing was changed.");
}

/** The room for the words about the bits: the entry's path, its name and the reason. */
#define FILEOP_BITS_CLAUSE_MAX (WPE_PATH_SIZE + 512)

/** fileop_entries_words - "1 entry" or "N entries". */
static void fileop_entries_words(char *out, size_t n, long count)
{
 if (count == 1)
  snprintf(out, n, "1 entry");
 else
  snprintf(out, n, "%ld entries", count);
}

/** fileop_bits_are - "was" for one bit that was not kept, "were" for several. */
static const char *fileop_bits_are(mode_t lost)
{
 mode_t one_less = lost - 1;

 return((lost & one_less) != 0 ? "were" : "was");
}

/**
 * fileop_bits_entry_target - What the note names when it concerns one entry:
 * its path; several entries: their count and the first.
 */
static void fileop_bits_entry_target(char *out, size_t n, const WpeFileOpBitsNote *b)
{
 char entries[48];

 if (b->bits_lost_count == 1)
 {
  snprintf(out, n, "%s", b->bits_path);
  return;
 }
 fileop_entries_words(entries, sizeof entries, b->bits_lost_count);
 snprintf(out, n, "%s (first: %s)", entries, b->bits_path);
}

/** fileop_bits_count_target - "N entries (first: path)", also for one entry. */
static void fileop_bits_count_target(char *out, size_t n, const WpeFileOpBitsNote *b)
{
 char entries[48];

 fileop_entries_words(entries, sizeof entries, b->bits_lost_count);
 snprintf(out, n, "%s (first: %s)", entries, b->bits_path);
}

/**
 * fileop_bits_clause - "setgid bit was not kept on <target>: <reason>".
 * @target: where, or "" for the permissions of one named file.
 *
 * The words are the write owner's, so the sentence of a copy and the one of
 * a save say the same thing in the same way.
 */
static void fileop_bits_clause(char *out, size_t n, const WpeFileOpBitsNote *b, const char *target)
{
 WpeModeKept k;
 char words[64], reason[192];

 memset(&k, 0, sizeof k);
 k.lost = b->bits_lost;
 k.err = b->bits_err;
 k.gid = b->bits_gid;
 k.in_group = b->bits_in_group;
 k.owners = b->bits_owners;
 wpe_write_bits_words(words, sizeof words, b->bits_lost);
 wpe_write_bits_reason(reason, sizeof reason, &k);
 if (target[0] != '\0')
  snprintf(out, n, "%s %s not kept on %s: %s", words, fileop_bits_are(b->bits_lost), target, reason);
 else
  snprintf(out, n, "%s %s not kept: %s", words, fileop_bits_are(b->bits_lost), reason);
}

/**
 * fileop_sentence_done - What a finished operation says when the system did
 * not let this user keep a special bit.
 *
 * "Copied, but the setgid bit was not kept on sg/a.c: Operation not
 * permitted."  A copy that is complete and a bit that is missing must not be
 * missing in silence.  Nothing when every bit stayed.
 */
static void fileop_sentence_done(char *out, size_t n, const WpeFileOpOutcome *o)
{
 const WpeFileOpBitsNote *b = &o->note;
 char target[WPE_PATH_SIZE + 64], clause[FILEOP_BITS_CLAUSE_MAX], lead[48];

 if (b->bits_lost_count == 0)
  return;
 if (o->kind == WPE_FILEOP_CHMOD)
 {
  fileop_bits_clause(clause, sizeof clause, b, "");
  snprintf(out, n, "Permissions of %s changed, but the %s.", o->src, clause);
  return;
 }
 if (o->total > 0)
 {
  fileop_bits_count_target(target, sizeof target, b);
  fileop_bits_clause(clause, sizeof clause, b, target);
  fileop_entries_words(lead, sizeof lead, o->done);
  snprintf(out, n, "%s %s, but the %s.", lead, fileop_past(o->kind), clause);
  return;
 }
 fileop_bits_entry_target(target, sizeof target, b);
 fileop_bits_clause(clause, sizeof clause, b, target);
 snprintf(lead, sizeof lead, "%s", fileop_past(o->kind));
 lead[0] = (char)toupper((unsigned char)lead[0]);
 snprintf(out, n, "%s, but the %s.", lead, clause);
}

/**
 * fileop_note_append - Add the bits note to the sentence of an operation
 * that did not finish whole.
 *
 * A folder that was stopped or failed after some of its entries lost a
 * bit says so too: the entries that were made are on the disk.
 */
static void fileop_note_append(char *out, size_t n, const WpeFileOpOutcome *o)
{
 char target[WPE_PATH_SIZE + 64], clause[FILEOP_BITS_CLAUSE_MAX];
 size_t used = strlen(out);

 if (o->note.bits_lost_count == 0 || used + 2 >= n)
  return;
 fileop_bits_count_target(target, sizeof target, &o->note);
 fileop_bits_clause(clause, sizeof clause, &o->note, target);
 if (used > 0)
  (void)wpe_fmt_shown(out + used, n - used, " The %s.", clause);
 else
  (void)wpe_fmt_shown(out, n, "The %s.", clause);
}

/**
 * fileop_sentence_text - The sentence of a DECLINED or FAILED outcome, without
 * the note of the special bits; empty for DONE, a question answered no and Esc.
 */
static int fileop_sentence_text(char *out, size_t n, const WpeFileOpOutcome *o)
{
 char reason[2 * WPE_PATH_SIZE + 160], left[2 * WPE_PATH_SIZE + 64];

 out[0] = '\0';
 if (o->status == WPE_FILEOP_DECLINED && o->step == WPE_FILEOP_STEP_EXISTS && o->total > 0)
  snprintf(out, n, "%ld of %ld entries were %s; %ld left as they were.", o->done, o->total,
           fileop_past(o->kind), o->skipped);
 else if (o->status == WPE_FILEOP_DECLINED && o->step == WPE_FILEOP_STEP_EXISTS)
  snprintf(out, n, "%s exists; not overwritten.", o->dst);
 if (o->status == WPE_FILEOP_FAILED && o->step == WPE_FILEOP_STEP_STOPPED)
 {
  snprintf(out, n, "Stopped: %ld of %ld entries were %s.", o->done, o->total, fileop_past(o->kind));
  return((int)strlen(out));
 }
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

int wpe_fileop_sentence(char *out, size_t n, const WpeFileOpOutcome *o)
{
 (void)fileop_sentence_text(out, n, o);
 if (o->status == WPE_FILEOP_DONE)
  fileop_sentence_done(out, n, o);
 else if (o->step != WPE_FILEOP_STEP_CANCELLED)
  fileop_note_append(out, n, o);
 return((int)strlen(out));
}
