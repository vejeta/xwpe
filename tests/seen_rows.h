/* seen_rows.h -- what a row needs to test "is this the object I saw" on a
 * real file system: a folder on a disk, a way to make the file system give an
 * inode number back, and the one assertion of "another object" that is safe.
 *
 * An inode number is given back by the file system only after its object is
 * gone, and tmpfs never gives one back (0 in 200 rounds).  A row that needs
 * the reuse therefore runs on a disk file system: the build tree, or the
 * folder XWPE_TEST_REUSE_DIR names.  It prints which file system it ran on, so
 * a log shows whether the rows ran or were skipped.
 *
 * Two outcomes are kept apart on purpose.  A file system that never gives a
 * number back (a one-shot probe in a scratch folder) is SKIP: the table row of
 * test_seen carries the proof there.  A file system that does give numbers
 * back, but where the row could not make the number come again, is a FAIL: the
 * row proved nothing and must not pass for a row that did.
 *
 * Static inline, libc only, like testdir.h.
 */
#ifndef TEST_SEEN_ROWS_H
#define TEST_SEEN_ROWS_H

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "testdir.h"
#include "../we_platform.h"

#if WPE_PLATFORM_STATFS_KIND == WPE_PLATFORM_STATFS_F_TYPE
# include <sys/vfs.h>
#elif WPE_PLATFORM_STATFS_KIND == WPE_PLATFORM_STATFS_FSTYPENAME
# include <sys/param.h>
# include <sys/mount.h>
#endif

#define SEEN_ROWS_DIR_ENV     "XWPE_TEST_REUSE_DIR"
#define SEEN_ROWS_DEFAULT_DIR "."
#define SEEN_ROWS_ATTEMPTS    16      /* whole cases redone when another process takes the freed number */
#define SEEN_ROWS_TRIES       64     /* makes of one object before the number is called not reached */
#define SEEN_ROWS_PROBE_ROUNDS 16     /* unlink and make again, to ask "does it ever give a number back" */
#define SEEN_ROWS_NAME_SIZE   64

/** What a row calls to make the object again under NAME (a file, a link or a folder). */
typedef int (*SeenRowsMake)(const char *path, void *ctx);

/**
 * seen_rows_fs_name - Name the file system a folder is on.
 *
 * Lets every row print where it ran.  The names are the ones of the file
 * systems this project is tested on; any other is its magic number in hex.
 */
static void seen_rows_fs_name(const char *dir, char *out, size_t n)
{
#if WPE_PLATFORM_STATFS_KIND == WPE_PLATFORM_STATFS_FSTYPENAME
 struct statfs sf;

 if (statfs(dir, &sf) != 0)
  snprintf(out, n, "unknown (%s)", strerror(errno));
 else
  snprintf(out, n, "%s", sf.f_fstypename);
#elif WPE_PLATFORM_STATFS_KIND == WPE_PLATFORM_STATFS_F_TYPE
 struct statfs sf;

 if (statfs(dir, &sf) != 0)
 {
  snprintf(out, n, "unknown (%s)", strerror(errno));
  return;
 }
 switch ((unsigned long)sf.f_type)
 {
 case 0xEF53UL:     snprintf(out, n, "ext2/3/4"); break;
 case 0x01021994UL: snprintf(out, n, "tmpfs"); break;
 case 0x58465342UL: snprintf(out, n, "xfs"); break;
 case 0x9123683EUL: snprintf(out, n, "btrfs"); break;
 case 0xF2F52010UL: snprintf(out, n, "f2fs"); break;
 case 0x794C7630UL: snprintf(out, n, "overlayfs"); break;
 case 0x6969UL:     snprintf(out, n, "nfs"); break;
 case 0x65735546UL: snprintf(out, n, "fuse"); break;
 case 0x4D44UL:     snprintf(out, n, "vfat"); break;
 default:           snprintf(out, n, "0x%lx", (unsigned long)sf.f_type); break;
 }
#else
 (void)dir;
 snprintf(out, n, "unknown (this system gives no file system type)");
#endif
}

/**
 * seen_rows_tree_step - nftw callback: remove one entry of the test folder.
 */
static int seen_rows_tree_step(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
 (void)st;
 (void)ftw;
 if (flag == FTW_DP)
  return(rmdir(path) == 0 ? 0 : -1);
 return(unlink(path) == 0 ? 0 : -1);
}

/**
 * seen_rows_remove - Remove the test folder this run made, and what is in it.
 *
 * Only ever called with the exact path test_dir_make_under returned, never a
 * pattern.
 */
static void seen_rows_remove(const char *dir)
{
 if (dir[0] != '\0')
  nftw(dir, seen_rows_tree_step, 16, FTW_DEPTH | FTW_PHYS);
}

/**
 * seen_rows_dir - Make the folder a reuse row works in, on a disk, and say where.
 *
 * The folder is under XWPE_TEST_REUSE_DIR or, when that is not set, the build
 * tree (the folder `make check` runs in), not /tmp, which is tmpfs on many
 * systems and never gives a number back.  The line it prints names the file
 * system.
 * Return: 0, or -1 (DIR empty; the failure is counted by testdir.h).
 */
static int seen_rows_dir(char *dir, size_t n, const char *prefix)
{
 const char *base = getenv(SEEN_ROWS_DIR_ENV);
 char fs[SEEN_ROWS_NAME_SIZE];

 if (base == NULL || base[0] == '\0')
  base = SEEN_ROWS_DEFAULT_DIR;
 int before = test_dir_failures;

 if (test_dir_make_under(base, dir, n, prefix) != 0)
 {
  if (dir[0] == '\0' && test_dir_failures == before)
   test_dir_fail("FAIL: reuse folder: cannot make one under %s\n", base);
  return(-1);
 }
 seen_rows_fs_name(dir, fs, sizeof fs);
 printf("note: the reuse rows ran under %s on %s\n", base, fs);
 return(0);
}

/**
 * seen_rows_make_file - Make an empty file (a SeenRowsMake).
 */
static int seen_rows_make_file(const char *path, void *ctx)
{
 int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);

 (void)ctx;
 if (fd < 0)
  return(-1);
 return(close(fd));
}

/**
 * seen_rows_make_file_text - Make a file holding the text ctx (a SeenRowsMake).
 */
static int seen_rows_make_file_text(const char *path, void *ctx)
{
 const char *text = (const char *)ctx;
 size_t len = strlen(text);
 int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);

 if (fd < 0)
  return(-1);
 if (write(fd, text, len) != (ssize_t)len)
 {
  close(fd);
  return(-1);
 }
 return(close(fd));
}

/**
 * seen_rows_make_folder -Make a folder (a SeenRowsMake).
 */
static int seen_rows_make_folder(const char *path, void *ctx)
{
 (void)ctx;
 return(mkdir(path, 0700));
}

/**
 * seen_rows_make_link - Make a symbolic link whose target text is ctx (a SeenRowsMake).
 */
static int seen_rows_make_link(const char *path, void *ctx)
{
 return(symlink((const char *)ctx, path));
}

/**
 * seen_rows_never_gives_back - Does this folder's file system never reuse a number?
 * @dir: a folder on the file system the row will use.
 *
 * A one-shot probe in a scratch subfolder: make a file, remember its number,
 * remove it, make another, compare, for a few rounds.  A file system that gave
 * the number back even once returns 0.  This is the only thing that may turn
 * a row into a SKIP: "the number was not reached" never does.
 * Return: 1 when no round gave the number back, 0 when one did.
 */
static int seen_rows_never_gives_back(const char *dir)
{
 char scratch[TEST_DIR_PATH_SIZE], path[TEST_DIR_PATH_SIZE];
 struct stat first, second;
 int round, gave_back = 0;

 if (test_path_join(scratch, sizeof scratch, dir, "probe") != 0)
  return(0);
 if (mkdir(scratch, 0700) != 0)
  return(0);
 if (test_path_join(path, sizeof path, scratch, "p") != 0)
 {
  rmdir(scratch);
  return(0);
 }
 for (round = 0; round < SEEN_ROWS_PROBE_ROUNDS && !gave_back; round++)
 {
  if (seen_rows_make_file(path, NULL) != 0 || lstat(path, &first) != 0)
   break;
  unlink(path);
  if (seen_rows_make_file(path, NULL) != 0 || lstat(path, &second) != 0)
   break;
  gave_back = (first.st_ino == second.st_ino);
  unlink(path);
 }
 rmdir(scratch);
 return(!gave_back);
}

/**
 * seen_rows_filler_name - The name a keeper of a lower number is put under.
 */
static void seen_rows_filler_name(const char *dir, int n, char *out, size_t size)
{
 char name[SEEN_ROWS_NAME_SIZE];

 snprintf(name, sizeof name, "filler%d", n);
 test_path_join(out, size, dir, name);
}

/**
 * seen_rows_reuse_once - One try to make the file system give WANT back to a new object.
 * @dir: the folder of the row.  @name: where the new object is made.
 * @make: makes the object (file, link or folder).  @ctx: its argument.
 * @want: the number the old object had, which is gone.
 *
 * Remove-and-make-again does not reach the number when a lower one is free
 * (ext4 gives the lowest free number of the group, and a rename that freed
 * another object's number took the lower one first).  So: make the object;
 * equal to WANT is done; below it, keep it under a filler name and try again,
 * which makes the next one higher; above it, the number cannot be reached.
 * The fillers are removed with the folder.
 * Return: 1 with the object at NAME holding WANT, 0 when it was not reached
 * (a FAIL on a file system that gives numbers back, never a SKIP).
 */
static int seen_rows_reuse_once(const char *dir, const char *name, SeenRowsMake make,
                                void *ctx, ino_t want)
{
 char path[TEST_DIR_PATH_SIZE], filler[TEST_DIR_PATH_SIZE];
 struct stat st;
 int tries;

 if (test_path_join(path, sizeof path, dir, name) != 0)
  return(0);
 for (tries = 0; tries < SEEN_ROWS_TRIES; tries++)
 {
  if (make(path, ctx) != 0 || lstat(path, &st) != 0)
   return(0);
  if (st.st_ino == want)
   return(1);
  if (st.st_ino > want)
   return(0);
  seen_rows_filler_name(dir, tries, filler, sizeof filler);
  if (rename(path, filler) != 0)
   return(0);
 }
 return(0);
}

/**
 * seen_rows_remove_entry - Remove one file, link or folder, quietly.
 */
static void seen_rows_remove_entry(const char *path)
{
 struct stat st;

 if (lstat(path, &st) != 0)
  return;
 if (S_ISDIR(st.st_mode))
  rmdir(path);
 else
  unlink(path);
}

/** What a reuse row needs to say: the old object, the new one, and what to do while the old exists. */
typedef struct {
 SeenRowsMake make_old;                                  /* makes the object that will go */
 void *old_ctx;
 void (*look_old)(const char *path, void *ctx);          /* takes the look while the old one exists; may be NULL */
 void *look_ctx;
 SeenRowsMake make_new;                                  /* makes the object that should take its number */
 void *new_ctx;
} SeenRowsCase;

/**
 * seen_rows_attempt - One whole attempt: a new old object, its number, its removal, the new object.
 * @pc: the case.  @dir, @name: where.
 *
 * The caller's look of the old object is taken afresh each time, so after a
 * lost race the row compares against the object of the attempt that won.
 * Return: 1 when the new object at NAME holds the old one's number.
 */
static int seen_rows_attempt(const char *dir, const char *name, const SeenRowsCase *pc)
{
 char path[TEST_DIR_PATH_SIZE];
 struct stat st;

 if (test_path_join(path, sizeof path, dir, name) != 0)
  return(0);
 if (pc->make_old(path, pc->old_ctx) != 0 || lstat(path, &st) != 0)
  return(0);
 if (pc->look_old != NULL)
  pc->look_old(path, pc->look_ctx);
 seen_rows_remove_entry(path);
 return(seen_rows_reuse_once(dir, name, pc->make_new, pc->new_ctx, st.st_ino));
}

/**
 * seen_rows_clean_attempt - Remove what a lost attempt left: the new object and the fillers.
 */
static void seen_rows_clean_attempt(const char *dir, const char *name)
{
 char path[TEST_DIR_PATH_SIZE];
 int n;

 if (test_path_join(path, sizeof path, dir, name) != 0)
  return;
 seen_rows_remove_entry(path);
 for (n = 0; n < SEEN_ROWS_TRIES; n++)
 {
  seen_rows_filler_name(dir, n, path, sizeof path);
  seen_rows_remove_entry(path);
 }
}

/**
 * seen_rows_reuse_the_number - Make the file system give an old object's number to a new object.
 * @dir: the folder of the row (fresh, so no filler of another row is in it).
 * @name: where the old and then the new object are made.  @pc: the case.
 *
 * Another process can take the freed number between the removal and the make
 * (a parallel run, a build), so one lost race must not fail the row: the
 * whole case is redone with a new old object and a new wanted number, up to
 * SEEN_ROWS_ATTEMPTS times.  On return 1 the old object's look was taken in the
 * attempt that won, and the new object is at NAME.
 * Return: 1 when reached, 0 when every attempt failed: a FAIL on a file system
 * that gives numbers back, never a SKIP.
 */
static int seen_rows_reuse_the_number(const char *dir, const char *name, const SeenRowsCase *pc)
{
 int attempt;

 for (attempt = 0; attempt < SEEN_ROWS_ATTEMPTS; attempt++)
 {
  if (seen_rows_attempt(dir, name, pc))
   return(1);
  seen_rows_clean_attempt(dir, name);
 }
 return(0);
}

/**
 * other_object_while_held - Is NOW a different object from the one HELD_FD holds?
 *
 * The one place a test may assert "another inode".  Comparing two numbers
 * with nothing open proves nothing: the file system may give the first
 * number to the newcomer.  Here the descriptor is the proof that the first
 * object, and so its number, still exists.  A descriptor that does not
 * answer counts as not proven (0).
 * Return: 1 when the numbers differ while the object is held, else 0.
 */
static int other_object_while_held(int held_fd, const struct stat *now)
{
 struct stat held;

 if (held_fd < 0 || fstat(held_fd, &held) != 0)
  return(0);
 return(held.st_ino != now->st_ino || held.st_dev != now->st_dev);
}

#endif
