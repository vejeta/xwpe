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
 * Two outcomes are kept apart on purpose, and one function decides them
 * (seen_rows_reuse_or_skip, which asks seen_rows_missed after a walk that
 * missed).  A file system that never gives a number back for the row's own
 * kind of object (a probe in a scratch folder, made the way the row makes it)
 * is SKIP, with what was measured and on which system.  A file system that
 * does give numbers back, but where the row could not make the number come
 * again, is a FAIL: the row proved nothing and must not pass for a row that
 * did.  Each system answers for itself: the probe is never a list of systems.
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
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
# include <sys/attr.h>
#endif

#include "testdir.h"
#include "../we_platform.h"
#include "../we_seen.h"

/* NetBSD has no statfs (its struct statvfs carries the file system's name in
   f_fstypename and the mount flags in f_flag), so the build's statfs fact says
   "none" there; the name and flags are read from statvfs. */
#if defined(__NetBSD__) && WPE_PLATFORM_STATFS_KIND == WPE_PLATFORM_STATFS_NONE
# define SEEN_ROWS_STATVFS 1
# include <sys/statvfs.h>
#else
# define SEEN_ROWS_STATVFS 0
#endif

#if WPE_PLATFORM_STATFS_KIND == WPE_PLATFORM_STATFS_F_TYPE
# include <sys/vfs.h>
#elif WPE_PLATFORM_STATFS_KIND == WPE_PLATFORM_STATFS_FSTYPENAME
# include <sys/param.h>
# include <sys/mount.h>
#endif

#define SEEN_ROWS_DIR_ENV     "XWPE_TEST_REUSE_DIR"
#define SEEN_ROWS_DEFAULT_DIR "."
#define SEEN_ROWS_PROBE_ENV   "XWPE_TEST_REUSE_PROBE"   /* test hook: "never:<kind>,gives:<kind>" plays the probe's answer */
#define SEEN_ROWS_ATTEMPTS    16      /* whole cases redone when another process takes the freed number */
#define SEEN_ROWS_TRIES       64     /* makes of one object before the number is called not reached */
#define SEEN_ROWS_PROBE_ROUNDS 16     /* unlink and make again, to ask "does it ever give a number back" */
#define SEEN_ROWS_NAME_SIZE   64
#define SEEN_ROWS_FORGE_BACK_S  1000  /* seconds a probe sets a file's times back to see whether its birth follows */
#define SEEN_ROWS_FORGE_SLACK_S 2     /* seconds of slack when reading the birth back */
#define SEEN_ROWS_FORGE_FORWARD_S 1000 /* seconds a probe sets a file's times later, to see whether its birth follows */

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
#elif SEEN_ROWS_STATVFS
 struct statvfs sv;

 if (statvfs(dir, &sv) != 0)
  snprintf(out, n, "unknown (%s)", strerror(errno));
 else
  snprintf(out, n, "%s", sv.f_fstypename);
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
 * seen_rows_remove_entry - Remove one file, link, folder or FIFO, quietly.
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

/**
 * seen_rows_make_fifo - Make a FIFO (a SeenRowsMake), the object of the swap row.
 */
static int seen_rows_make_fifo(const char *path, void *ctx)
{
 (void)ctx;
 return(mkfifo(path, 0644));
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

/** How a reuse row ended: it ran, it was skipped by name, or it proved nothing. */
typedef enum
{
 SEEN_ROWS_RAN,
 SEEN_ROWS_SKIP,
 SEEN_ROWS_FAIL
} SeenRowsOutcome;

/**
 * What a probe saw.  kind is the S_IFMT of the first object it made, so the
 * words of a log name the kind the row really walks with.
 */
typedef struct
{
 ino_t first, second;       /* the two numbers of the last round that ran (0 when none did) */
 mode_t kind;               /* S_IFMT of the first object made, 0 when none was */
 int gave_back;             /* a round gave the number back, or the probe could not be set up */
 int hooked;                /* the answer came from the test hook, not from the file system */
} SeenRowsProbe;

/** seen_rows_kind_word - The kind of an object in words, for the lines a row prints. */
static const char *seen_rows_kind_word(mode_t kind)
{
 if (kind == S_IFREG)
  return("file");
 if (kind == S_IFLNK)
  return("link");
 if (kind == S_IFDIR)
  return("folder");
 if (kind == S_IFIFO)
  return("FIFO");
 return("object");
}

/** seen_rows_hook_has - Does the comma-separated list of the test hook hold exactly TOKEN? */
static int seen_rows_hook_has(const char *list, const char *token)
{
 size_t len = strlen(token);

 while (*list != '\0')
 {
  const char *end = strchr(list, ',');
  size_t item = (end != NULL) ? (size_t)(end - list) : strlen(list);

  if (item == len && strncmp(list, token, len) == 0)
   return(1);
  if (end == NULL)
   break;
  list = end + 1;
 }
 return(0);
}

/**
 * seen_rows_hook_answer - What the test hook says about one kind of object.
 * @kind: S_IFMT of the object the probe made.
 *
 * XWPE_TEST_REUSE_PROBE is a comma list of "never:<kind>" and "gives:<kind>"
 * (kind: file, link, folder, fifo, in lower case).  It plays the file system's
 * answer for that kind alone, so a canary can show the probe asks about the
 * kind the row walks with: with "never:link" the link rows are skipped and the
 * file rows are not.  It acts only where a walk already failed, because the
 * probe is asked only then.
 * Return: 0 never gives back, 1 gives back, -1 the hook says nothing.
 */
static int seen_rows_hook_answer(mode_t kind)
{
 const char *list = getenv(SEEN_ROWS_PROBE_ENV);
 char never[SEEN_ROWS_NAME_SIZE], gives[SEEN_ROWS_NAME_SIZE], word[SEEN_ROWS_NAME_SIZE];
 size_t i;

 if (list == NULL)
  return(-1);
 snprintf(word, sizeof word, "%s", seen_rows_kind_word(kind));
 for (i = 0; word[i] != '\0'; i++)
  if (word[i] >= 'A' && word[i] <= 'Z')
   word[i] = (char)(word[i] - 'A' + 'a');
 snprintf(never, sizeof never, "never:%s", word);
 snprintf(gives, sizeof gives, "gives:%s", word);
 if (seen_rows_hook_has(list, never))
  return(0);
 if (seen_rows_hook_has(list, gives))
  return(1);
 return(-1);
}

/**
 * seen_rows_probe_round - One round of the probe: the row's own make, look, removal and make.
 * @path: the name in the scratch folder.  @pc: the case.  @pr: receives the numbers.
 * Return: 1 when the number came back, 0 when it did not, -1 when a step failed.
 */
static int seen_rows_probe_round(const char *path, const SeenRowsCase *pc, SeenRowsProbe *pr)
{
 struct stat first, second;

 if (pc->make_old(path, pc->old_ctx) != 0 || lstat(path, &first) != 0)
  return(-1);
 pr->kind = first.st_mode & S_IFMT;
 seen_rows_remove_entry(path);
 if (pc->make_new(path, pc->new_ctx) != 0 || lstat(path, &second) != 0)
  return(-1);
 pr->first = first.st_ino;
 pr->second = second.st_ino;
 seen_rows_remove_entry(path);
 return(first.st_ino == second.st_ino);
}

/**
 * seen_rows_probe - Ask the file system whether it gives back a freed number, in the row's own shape.
 * @dir: a folder on the file system the row will use.
 * @pc: the case the row walks with.  @pr: receives what the probe saw.
 *
 * Makes the case's old object in a scratch subfolder, removes it the way the
 * walk does, makes the new object, and compares the two numbers, for a few
 * rounds.  The shape is the row's own and nothing more: the same two makers,
 * no flush between make and removal (the walk does none), and a measured fact
 * behind it: on FreeBSD soft updates a file removed with no flush gave its
 * number back 20 times in 20 and a link or a folder 0 in 20, so a probe that
 * always made a file would say "gives back" for a link row that cannot.  The
 * one difference from the walk is look_old, which is not run: it has side
 * effects in the row's window (row 6 makes and renames files in it).  If the
 * probe and the row then disagree the row FAILs with the REUSE-GIVEUP lines.
 *
 * A probe that cannot be set up says "gives back": the row then fails by name
 * instead of skipping on a probe that never ran.
 */
static void seen_rows_probe(const char *dir, const SeenRowsCase *pc, SeenRowsProbe *pr)
{
 char scratch[TEST_DIR_PATH_SIZE], path[TEST_DIR_PATH_SIZE];
 int round, said = 0;

 memset(pr, 0, sizeof *pr);
 pr->gave_back = 1;
 if (test_path_join(scratch, sizeof scratch, dir, "probe") != 0 || mkdir(scratch, 0700) != 0)
  return;
 if (test_path_join(path, sizeof path, scratch, "p") != 0)
 {
  rmdir(scratch);
  return;
 }
 pr->gave_back = 0;
 for (round = 0; round < SEEN_ROWS_PROBE_ROUNDS && !pr->gave_back; round++)
 {
  said = seen_rows_probe_round(path, pc, pr);
  if (said < 0)
  {
   pr->gave_back = 1;
   break;
  }
  pr->gave_back = said;
  if (round == 0 && seen_rows_hook_answer(pr->kind) >= 0)
  {
   pr->gave_back = seen_rows_hook_answer(pr->kind);
   pr->hooked = 1;
   break;
  }
 }
 seen_rows_remove_entry(path);
 rmdir(scratch);
}

/**
 * seen_rows_never_gives_back - Does this folder's file system never reuse a number for the row's kind?
 * @dir: a folder on the file system the row will use.  @pc: the case.
 *
 * The probe of seen_rows_probe.  The one thing that may turn a missed walk
 * into a SKIP.
 * Return: 1 when no round gave the number back, 0 when one did.
 */
static int seen_rows_never_gives_back(const char *dir, const SeenRowsCase *pc)
{
 SeenRowsProbe pr;

 seen_rows_probe(dir, pc, &pr);
 return(!pr.gave_back);
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

/** How a walk that did not reach the wanted number ended. */
typedef enum
{
 SEEN_REUSE_MAKE_FAILED,     /* the object could not be made or looked at */
 SEEN_REUSE_ABOVE_WANTED,    /* the new object took a number above the wanted one */
 SEEN_REUSE_RENAME_FAILED,   /* a lower number could not be kept under a filler name */
 SEEN_REUSE_TRIES_RAN_OUT    /* SEEN_ROWS_TRIES objects and none held the number */
} SeenRowsGiveUpExit;

/**
 * SeenRowsGiveUp - What the last walk that gave up saw, kept for the row that
 * reports it (seen_rows_say_giveup).  One walk at a time, as the rows are.
 */
typedef struct
{
 const char *dir, *name;
 SeenRowsMake make;
 void *ctx;
 ino_t want;
 SeenRowsGiveUpExit how;
 int err;                              /* errno at the exit, 0 when the exit has none */
 int tries;                            /* numbers noted */
 ino_t got[SEEN_ROWS_TRIES];           /* the number of every try, in order */
} SeenRowsGiveUp;

static SeenRowsGiveUp seen_rows_last_giveup;

/** seen_rows_giveup_begin - Start the record of a walk. */
static void seen_rows_giveup_begin(SeenRowsGiveUp *g, const char *dir, const char *name,
                                   SeenRowsMake make, void *ctx, ino_t want)
{
 memset(g, 0, sizeof *g);
 g->dir = dir;
 g->name = name;
 g->make = make;
 g->ctx = ctx;
 g->want = want;
}

/** seen_rows_giveup_note - Keep the number one try gave. */
static void seen_rows_giveup_note(SeenRowsGiveUp *g, ino_t got)
{
 if (g->tries < SEEN_ROWS_TRIES)
  g->got[g->tries++] = got;
}

/** seen_rows_giveup_end - Keep how the walk ended; always 0, the walk's answer. */
static int seen_rows_giveup_end(SeenRowsGiveUp *g, SeenRowsGiveUpExit how, int err)
{
 g->how = how;
 g->err = err;
 return(0);
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
 SeenRowsGiveUp *g = &seen_rows_last_giveup;
 struct stat st;
 int tries;

 if (test_path_join(path, sizeof path, dir, name) != 0)
  return(0);
 seen_rows_giveup_begin(g, dir, name, make, ctx, want);
 for (tries = 0; tries < SEEN_ROWS_TRIES; tries++)
 {
  if (make(path, ctx) != 0 || lstat(path, &st) != 0)
   return(seen_rows_giveup_end(g, SEEN_REUSE_MAKE_FAILED, errno));
  seen_rows_giveup_note(g, st.st_ino);
  if (st.st_ino == want)
   return(1);
  if (st.st_ino > want)
   return(seen_rows_giveup_end(g, SEEN_REUSE_ABOVE_WANTED, 0));
  seen_rows_filler_name(dir, tries, filler, sizeof filler);
  if (rename(path, filler) != 0)
   return(seen_rows_giveup_end(g, SEEN_REUSE_RENAME_FAILED, errno));
 }
 return(seen_rows_giveup_end(g, SEEN_REUSE_TRIES_RAN_OUT, 0));
}

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

/** seen_rows_giveup_exit_name - The exit of a walk in words. */
static const char *seen_rows_giveup_exit_name(SeenRowsGiveUpExit how)
{
 if (how == SEEN_REUSE_MAKE_FAILED)
  return("the object could not be made or looked at");
 if (how == SEEN_REUSE_ABOVE_WANTED)
  return("the new object took a number above the wanted one");
 if (how == SEEN_REUSE_RENAME_FAILED)
  return("a lower number could not be kept under a filler name");
 return("every try gave a number below the wanted one");
}

/** seen_rows_fs_flags - The mount flags of the file system DIR is on, or 0 where the system gives none. */
static unsigned long seen_rows_fs_flags(const char *dir)
{
#if SEEN_ROWS_STATVFS
 struct statvfs sv;

 if (statvfs(dir, &sv) != 0)
  return(0);
 return((unsigned long)sv.f_flag);
#elif WPE_PLATFORM_STATFS_KIND == WPE_PLATFORM_STATFS_NONE
 (void)dir;
 return(0);
#else
 struct statfs sf;

 if (statfs(dir, &sf) != 0)
  return(0);
 return((unsigned long)sf.f_flags);
#endif
}

/** seen_rows_say_giveup_walk - What the walk saw: the exit, the wanted number, every try's number, errno. */
static void seen_rows_say_giveup_walk(const SeenRowsGiveUp *g)
{
 int i;

 fprintf(stderr, "REUSE-GIVEUP exit: %s; want=%lu; errno=%d (%s); tries=%d; got=",
         seen_rows_giveup_exit_name(g->how), (unsigned long)g->want, g->err,
         g->err != 0 ? strerror(g->err) : "none", g->tries);
 for (i = 0; i < g->tries; i++)
  fprintf(stderr, "%s%lu", i > 0 ? "," : "", (unsigned long)g->got[i]);
 fprintf(stderr, "\n");
}

/** seen_rows_say_giveup_system - What the probe found, the file system and its flags, and the system release. */
static void seen_rows_say_giveup_system(const char *dir, const SeenRowsCase *pc)
{
 char fs[SEEN_ROWS_NAME_SIZE];
 struct utsname u;
 SeenRowsProbe pr;

 seen_rows_probe(dir, pc, &pr);
 seen_rows_fs_name(dir, fs, sizeof fs);
 fprintf(stderr, "REUSE-GIVEUP probe: kind=%s first=%lu second=%lu gave_back=%d\n",
         seen_rows_kind_word(pr.kind), (unsigned long)pr.first, (unsigned long)pr.second, pr.gave_back);
 fprintf(stderr, "REUSE-GIVEUP file system: %s, mount flags 0x%lx\n", fs, seen_rows_fs_flags(dir));
 if (uname(&u) == 0)
  fprintf(stderr, "REUSE-GIVEUP system: %s %s\n", u.sysname, u.release);
}

/**
 * seen_rows_extra_try_after_sync - One more try after the folder and the disk are flushed.
 * @g: the walk that gave up.
 *
 * Separates two causes that look alike in a log: the file system defers
 * giving a freed number back (soft updates; the fix is a sync before the new
 * object is made), or it allocates above the wanted number whatever is done
 * (the fix is another method).  Removes the object the walk left under the
 * name, flushes the folder with fsync and everything with sync, makes the
 * object again and says whether it holds the wanted number.
 */
static void seen_rows_extra_try_after_sync(const SeenRowsGiveUp *g)
{
 char path[TEST_DIR_PATH_SIZE];
 struct stat st;
 int fd;

 if (test_path_join(path, sizeof path, g->dir, g->name) != 0)
  return;
 seen_rows_remove_entry(path);
 fd = open(g->dir, O_RDONLY);
 if (fd >= 0)
 {
  if (fsync(fd) != 0)
   fprintf(stderr, "REUSE-GIVEUP fsync of the folder: %s\n", strerror(errno));
  close(fd);
 }
 sync();
 if (g->make(path, g->ctx) != 0 || lstat(path, &st) != 0)
 {
  fprintf(stderr, "REUSE-GIVEUP after fsync of the folder and sync: the object could not be made: %s\n",
          strerror(errno));
  return;
 }
 fprintf(stderr, "REUSE-GIVEUP after fsync of the folder and sync: got=%lu want=%lu reached=%d\n",
         (unsigned long)st.st_ino, (unsigned long)g->want, st.st_ino == g->want);
 seen_rows_remove_entry(path);
}

/**
 * seen_rows_say_giveup - Say, on stderr, everything a log needs to settle why the number was not reached.
 * @g: the record of the walk that gave up.
 *
 * When a row fails on a file system that gives numbers back, the log of the
 * failed run is the only evidence there is: it names the exit the walk took,
 * every number it saw, the error, what the one-shot probe found, the file
 * system and its flags, the release of the system, and whether one more try
 * after a flush reached the number.
 */
static void seen_rows_say_giveup(const SeenRowsGiveUp *g, const SeenRowsCase *pc)
{
 seen_rows_say_giveup_walk(g);
 seen_rows_say_giveup_system(g->dir, pc);
 seen_rows_extra_try_after_sync(g);
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

 seen_rows_giveup_begin(&seen_rows_last_giveup, dir, name, pc->make_new, pc->new_ctx, 0);
 for (attempt = 0; attempt < SEEN_ROWS_ATTEMPTS; attempt++)
 {
  if (seen_rows_attempt(dir, name, pc))
   return(1);
  seen_rows_clean_attempt(dir, name);
 }
 seen_rows_say_giveup(&seen_rows_last_giveup, pc);
 return(0);
}

/** seen_rows_say_ran - Say where a reuse row ran, so a log shows the row was not skipped. */
static void seen_rows_say_ran(const char *row, const char *dir)
{
 char fs[SEEN_ROWS_NAME_SIZE];

 seen_rows_fs_name(dir, fs, sizeof fs);
 printf("RAN %s: fs=%s\n", row, fs);
}

/**
 * seen_rows_say_skip - Say what the probe measured, as the reason a row is skipped.
 * @row: the row.  @dir: its folder.  @pr: what the probe saw.
 *
 * Words only what was measured: the kind of object, the file system, the
 * system and the mount flags.  (Inferred, not measured here: FreeBSD soft
 * updates defer releasing a freed inode until its removal has been written,
 * and a freed link or folder then does not come back within a row.)
 */
static void seen_rows_say_skip(const char *row, const char *dir, const SeenRowsProbe *pr)
{
 char fs[SEEN_ROWS_NAME_SIZE];
 struct utsname u;

 seen_rows_fs_name(dir, fs, sizeof fs);
 printf("SKIP: %s: a freed %s number did not come back on %s", row, seen_rows_kind_word(pr->kind), fs);
 if (uname(&u) == 0)
  printf(" (%s %s, mount flags 0x%lx)", u.sysname, u.release, seen_rows_fs_flags(dir));
 printf(" in the probe%s\n", pr->hooked ? " (played by the test hook)" : "");
}

/**
 * seen_rows_decide_miss - What a walk that missed the number is: a SKIP or a FAIL.
 * @dir: the row's folder.  @pc: the case it walks with.  @pr: receives what the probe saw.
 *
 * The decision itself, with nothing printed.  The probe asks the file system,
 * in the row's own shape and kind, whether a freed number ever comes back.
 * When it does not, the row cannot be run on this system: SKIP.  When it does,
 * the row proved nothing and must not pass for one that did: FAIL.
 * Return: SEEN_ROWS_SKIP or SEEN_ROWS_FAIL.
 */
static SeenRowsOutcome seen_rows_decide_miss(const char *dir, const SeenRowsCase *pc, SeenRowsProbe *pr)
{
 seen_rows_probe(dir, pc, pr);
 if (!pr->gave_back)
  return(SEEN_ROWS_SKIP);
 return(SEEN_ROWS_FAIL);
}

/**
 * seen_rows_missed - A row's walk did not reach the number: SKIP by name or FAIL.
 * @dir: the row's folder.  @row: its name.  @pc: the case it walks with.
 *
 * The one place that decides and says it, for every row (the reuse rows of
 * test_seen, test_write_owner and the FIFO swap).  Asked only after the walk
 * missed, so a walk that reached the number is always RAN, whatever the probe
 * would have said.  A SKIP names what was measured; a FAIL names the kind the
 * probe got back.
 * Return: SEEN_ROWS_SKIP or SEEN_ROWS_FAIL; the caller counts it.
 */
static SeenRowsOutcome seen_rows_missed(const char *dir, const char *row, const SeenRowsCase *pc)
{
 SeenRowsProbe pr;
 SeenRowsOutcome out = seen_rows_decide_miss(dir, pc, &pr);

 if (out == SEEN_ROWS_SKIP)
 {
  seen_rows_say_skip(row, dir, &pr);
  return(out);
 }
 printf("FAIL: %s: the number was not reached, and a freed %s number came back in the probe\n",
        row, seen_rows_kind_word(pr.kind));
 return(out);
}

/**
 * seen_rows_reuse_or_skip - Walk to the old object's number; on a miss, SKIP by name or FAIL.
 * @dir: the row's folder (fresh).  @row: its name.  @name: where the objects are made.  @pc: the case.
 *
 * The call every reuse row makes.  The walk first (seen_rows_reuse_the_number);
 * the probe only after the walk failed (seen_rows_missed).
 * Return: SEEN_ROWS_RAN with the new object at NAME holding the old number,
 * else SEEN_ROWS_SKIP or SEEN_ROWS_FAIL.
 */
static SeenRowsOutcome seen_rows_reuse_or_skip(const char *dir, const char *row, const char *name,
                                               const SeenRowsCase *pc)
{
 if (seen_rows_reuse_the_number(dir, name, pc))
 {
  seen_rows_say_ran(row, dir);
  return(SEEN_ROWS_RAN);
 }
 return(seen_rows_missed(dir, row, pc));
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

/** Who can say when a file was made on this system: the kernel alone, the file's owner too, or nobody. */
typedef enum
{
 SEEN_BIRTH_TRUSTED,           /* statx (Linux): only the kernel sets the birth time */
 SEEN_BIRTH_OWNER_SETTABLE,    /* st_birthtim, st_birthtimespec (FreeBSD, NetBSD, macOS): the owner can set it */
 SEEN_BIRTH_ABSENT             /* no birth time on this system or on this file system */
} SeenBirthFact;

/** What a row is about: another file took the number (the births differ), or the same file was touched in place. */
typedef enum
{
 SEEN_BIRTHS_DIFFER,
 SEEN_SAME_BIRTH_TOUCHED
} SeenScenario;

/**
 * seen_rows_birth_route - How this build reads a birth time, from the build's own facts.
 *
 * The design input of the judge (we_seen.c): a differing birth is another
 * object where only the kernel sets it, and is not proof where the owner can
 * set it.  It is read from the platform macros, not from the judge, so the
 * expectation is independent of the code it judges.
 */
static SeenBirthFact seen_rows_birth_route(void)
{
#if WPE_PLATFORM_STATX_BTIME
 return(SEEN_BIRTH_TRUSTED);
#elif WPE_PLATFORM_STAT_BIRTH != WPE_PLATFORM_STAT_BIRTH_NONE
 return(SEEN_BIRTH_OWNER_SETTABLE);
#else
 return(SEEN_BIRTH_ABSENT);
#endif
}

/**
 * seen_rows_system_gives_birth - Does the system itself report a birth time for this file?
 * @path: a file just made in the row's folder.
 *
 * Asked of the system, never of the product (we_seen.c answers "no birth" under
 * the test hook that plays a system without one, and a fact read from the
 * judge cannot check the judge).  Where the build reads the birth through
 * statx: STATX_BTIME must come back in the mask with a non-zero time.  Where it
 * reads it from struct stat: st_birthtim or st_birthtimespec must be neither
 * 0 nor -1.
 * Return: 1 when the system gives one.
 */
static int seen_rows_system_gives_birth(const char *path)
{
#if WPE_PLATFORM_STATX_BTIME
 struct statx sx;

 if (statx(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW, STATX_BTIME, &sx) != 0)
  return(0);
 if ((sx.stx_mask & STATX_BTIME) == 0)
  return(0);
 return(sx.stx_btime.tv_sec != 0 || sx.stx_btime.tv_nsec != 0);
#elif WPE_PLATFORM_STAT_BIRTH != WPE_PLATFORM_STAT_BIRTH_NONE
 struct stat st;
 time_t birth;

 if (lstat(path, &st) != 0)
  return(0);
# if WPE_PLATFORM_STAT_BIRTH == WPE_PLATFORM_STAT_BIRTHTIM
 birth = st.st_birthtim.tv_sec;
# else
 birth = st.st_birthtimespec.tv_sec;
# endif
 return(birth != 0 && birth != (time_t)-1);
#else
 (void)path;
 return(0);
#endif
}

/**
 * seen_rows_birth_fact - Which of the three a file made in DIR has.
 * @dir: the row's folder.
 *
 * The route says what the build can read; whether this file system gives a
 * birth time at all (msdosfs, some tmpfs) is measured on a fresh file in the
 * row's own folder, by asking the system (seen_rows_system_gives_birth).
 */
static SeenBirthFact seen_rows_birth_fact(const char *dir)
{
 SeenBirthFact route = seen_rows_birth_route();
 char path[TEST_DIR_PATH_SIZE];
 int has;

 if (route == SEEN_BIRTH_ABSENT)
  return(SEEN_BIRTH_ABSENT);
 if (test_path_join(path, sizeof path, dir, "probe_birth") != 0)
  return(SEEN_BIRTH_ABSENT);
 if (seen_rows_make_file(path, NULL) != 0)
  return(SEEN_BIRTH_ABSENT);
 has = seen_rows_system_gives_birth(path);
 unlink(path);
 if (has)
  return(route);
 return(SEEN_BIRTH_ABSENT);
}

/**
 * seen_rows_expected_verdict - What the judge must answer for a scenario on a system with this birth fact.
 *
 * Another file took the number and the births differ: OTHER where only the
 * kernel sets the birth, UNSURE where the owner can set it (a twin made by the
 * owner can copy it) and where there is none.  The same file touched in place:
 * SAME where both records have a birth that agrees, UNSURE where there is none
 * (nothing tells a touched file from a new one, so the user is asked, never
 * written over in silence).
 */
static WpeSeenIs seen_rows_expected_verdict(SeenScenario scenario, SeenBirthFact fact)
{
 if (scenario == SEEN_BIRTHS_DIFFER)
 {
  if (fact == SEEN_BIRTH_TRUSTED)
   return(WPE_SEEN_OTHER);
  return(WPE_SEEN_UNSURE);
 }
 if (fact == SEEN_BIRTH_ABSENT)
  return(WPE_SEEN_UNSURE);
 return(WPE_SEEN_SAME);
}

/** seen_rows_birth_fact_name - The fact in words, for the line a row prints. */
static const char *seen_rows_birth_fact_name(SeenBirthFact fact)
{
 if (fact == SEEN_BIRTH_TRUSTED)
  return("a birth time only the kernel sets");
 if (fact == SEEN_BIRTH_OWNER_SETTABLE)
  return("a birth time its owner can set");
 return("no birth time");
}

/** seen_rows_verdict_name - The judge's answer in words. */
static const char *seen_rows_verdict_name(WpeSeenIs verdict)
{
 if (verdict == WPE_SEEN_SAME)
  return("SAME");
 if (verdict == WPE_SEEN_OTHER)
  return("OTHER");
 return("UNSURE");
}

/**
 * seen_rows_utimes_moves_birth - Does setting a file's times back move its birth time back?
 * @dir: the row's folder.
 *
 * A measurement, not an input: it shows in the log that the design's premise
 * (the owner can forge a birth time) holds on this system.  FreeBSD and NetBSD
 * pull the birth back to the time given.
 * Return: 1 when it moved, 0 when it did not, -1 where the build has no birth
 * member to read (statx and no-birth systems).
 */
static int seen_rows_utimes_moves_birth(const char *dir)
{
#if WPE_PLATFORM_STAT_BIRTH == WPE_PLATFORM_STAT_BIRTH_NONE
 (void)dir;
 return(-1);
#else
 char path[TEST_DIR_PATH_SIZE];
 struct timeval back[2];
 struct stat st;
 time_t past = time(NULL) - SEEN_ROWS_FORGE_BACK_S;
 time_t birth;

 if (test_path_join(path, sizeof path, dir, "probe_forge") != 0 || seen_rows_make_file(path, NULL) != 0)
  return(-1);
 back[0].tv_sec = back[1].tv_sec = past;
 back[0].tv_usec = back[1].tv_usec = 0;
 if (utimes(path, back) != 0 || lstat(path, &st) != 0)
 {
  unlink(path);
  return(-1);
 }
 unlink(path);
# if WPE_PLATFORM_STAT_BIRTH == WPE_PLATFORM_STAT_BIRTHTIM
 birth = st.st_birthtim.tv_sec;
# else
 birth = st.st_birthtimespec.tv_sec;
# endif
 return(birth <= past + SEEN_ROWS_FORGE_SLACK_S);
#endif
}

/**
 * seen_rows_birth_back_only_route - Can a row play, or run on, a system where a birth only goes back?
 *
 * Read from the build's own macros, never from the judge: 1 where the birth
 * comes from statx (the test hook plays FreeBSD there) or where the build says
 * the system is back-only, 0 elsewhere (macOS, a build with no birth member).
 * A row that needs it skips by name when it is 0.
 */
static int seen_rows_birth_back_only_route(void)
{
#if WPE_PLATFORM_STATX_BTIME
 return(1);
#elif WPE_PLATFORM_BIRTH_BACK_ONLY
 return(1);
#else
 return(0);
#endif
}

#if WPE_PLATFORM_STAT_BIRTH != WPE_PLATFORM_STAT_BIRTH_NONE

/** The seconds of the birth time a stat carries. */
static time_t seen_rows_birth_seconds(const struct stat *st)
{
# if WPE_PLATFORM_STAT_BIRTH == WPE_PLATFORM_STAT_BIRTHTIM
 return(st->st_birthtim.tv_sec);
# else
 return(st->st_birthtimespec.tv_sec);
# endif
}

/** Set a file's access and modification times with utimes. */
static int seen_rows_set_by_utimes(const char *path, time_t when)
{
 struct timeval tv[2];

 tv[0].tv_sec = tv[1].tv_sec = when;
 tv[0].tv_usec = tv[1].tv_usec = 0;
 return(utimes(path, tv));
}

/** Set a file's access and modification times with utimensat. */
static int seen_rows_set_by_utimensat(const char *path, time_t when)
{
 struct timespec ts[2];

 ts[0].tv_sec = ts[1].tv_sec = when;
 ts[0].tv_nsec = ts[1].tv_nsec = 0;
 return(utimensat(AT_FDCWD, path, ts, 0));
}

/** Set a file's access and modification times with futimens on a descriptor. */
static int seen_rows_set_by_futimens(const char *path, time_t when)
{
 struct timespec ts[2];
 int fd = open(path, O_RDONLY);
 int rc;

 if (fd < 0)
  return(-1);
 ts[0].tv_sec = ts[1].tv_sec = when;
 ts[0].tv_nsec = ts[1].tv_nsec = 0;
 rc = futimens(fd, ts);
 close(fd);
 return(rc);
}

# if defined(__APPLE__)
/** Set a file's birth time itself, the one call that does on macOS (ATTR_CMN_CRTIME). */
static int seen_rows_set_by_setattrlist(const char *path, time_t when)
{
 struct attrlist al;
 struct timespec ts;

 memset(&al, 0, sizeof al);
 al.bitmapcount = ATTR_BIT_MAP_COUNT;
 al.commonattr = ATTR_CMN_CRTIME;
 ts.tv_sec = when;
 ts.tv_nsec = 0;
 return(setattrlist(path, &al, &ts, sizeof ts, 0));
}
# endif

typedef int (*SeenRowsBirthSetter)(const char *path, time_t when);

/**
 * seen_rows_setter_moves_birth_forward - Does one system call move this file's birth later?
 * @path: a file just made.  @setter: the call.  @base: the birth seconds read before.
 *
 * A setter that fails counts as not moving it: a system that refuses the call
 * has not shown a forward move.
 */
static int seen_rows_setter_moves_birth_forward(const char *path, SeenRowsBirthSetter setter, time_t base)
{
 struct stat st;

 if (setter(path, base + SEEN_ROWS_FORGE_FORWARD_S) != 0)
  return(0);
 if (lstat(path, &st) != 0)
  return(0);
 return(seen_rows_birth_seconds(&st) > base + SEEN_ROWS_FORGE_SLACK_S);
}

/** Does any documented setter of this system move the birth of the file at PATH later? */
static int seen_rows_any_setter_moves_birth_forward(const char *path, time_t base)
{
 if (seen_rows_setter_moves_birth_forward(path, seen_rows_set_by_utimes, base))
  return(1);
 if (seen_rows_setter_moves_birth_forward(path, seen_rows_set_by_utimensat, base))
  return(1);
 if (seen_rows_setter_moves_birth_forward(path, seen_rows_set_by_futimens, base))
  return(1);
# if defined(__APPLE__)
 if (seen_rows_setter_moves_birth_forward(path, seen_rows_set_by_setattrlist, base))
  return(1);
# endif
 return(0);
}

#endif

/**
 * seen_rows_birth_moves_forward - Does any call this system documents move a file's birth later?
 * @dir: the row's folder.
 *
 * A measurement, never an input of the judge: it shows in the log whether the
 * rule the build relies on (no system call moves a birth forward) holds here.
 * It tries utimes, utimensat and futimens, and on macOS setattrlist with
 * ATTR_CMN_CRTIME, which is the call that sets a birth there (a probe on
 * utimes alone would read "no" on a system that is ANY_WAY).
 * Return: 1 when one moved it forward, 0 when none did, -1 where the build has
 * no birth member to read (statx and no-birth systems) or the probe could not
 * make its file.
 */
static int seen_rows_birth_moves_forward(const char *dir)
{
#if WPE_PLATFORM_STAT_BIRTH == WPE_PLATFORM_STAT_BIRTH_NONE
 (void)dir;
 return(-1);
#else
 char path[TEST_DIR_PATH_SIZE];
 struct stat st;
 int moved;

 if (test_path_join(path, sizeof path, dir, "probe_forward") != 0 || seen_rows_make_file(path, NULL) != 0)
  return(-1);
 if (lstat(path, &st) != 0)
 {
  unlink(path);
  return(-1);
 }
 moved = seen_rows_any_setter_moves_birth_forward(path, seen_rows_birth_seconds(&st));
 unlink(path);
 return(moved);
#endif
}

/**
 * seen_rows_print_forward_note - Say in the log whether a birth moves forward here, and on which file system.
 * @dir: the row's folder.
 *
 * What the maintainer gets: a lane log carries the evidence that promotes or
 * demotes a system's trust class (whether any call moved a birth forward), and
 * names the file system the probe ran on, because a class is a fact of the
 * system calls and a file system such as ZFS, NFS or msdosfs can differ.
 */
static void seen_rows_print_forward_note(const char *dir)
{
 char fs[SEEN_ROWS_NAME_SIZE];
 int forward = seen_rows_birth_moves_forward(dir);

 seen_rows_fs_name(dir, fs, sizeof fs);
 if (forward == 1)
  printf(" (a call moved the birth time forward here, on %s)", fs);
 else if (forward == 0)
  printf(" (no call moved the birth time forward here, on %s)", fs);
}

/**
 * seen_rows_expect - The verdict a row must see here, and the facts that make it so, in the log.
 * @row: the row's name.  @dir: its folder.  @scenario: what it is about.
 *
 * One call per row, so no row repeats the birth rule: the verdict comes from
 * the scenario and the birth fact measured in the row's own folder.  The line
 * it prints names the fact, whether utimes forges the birth on this system,
 * and the verdict, so a log says which branch of a row ran and why.
 * Return: the verdict the judge must answer.
 */
static WpeSeenIs seen_rows_expect(const char *row, const char *dir, SeenScenario scenario)
{
 SeenBirthFact fact = seen_rows_birth_fact(dir);
 WpeSeenIs verdict = seen_rows_expected_verdict(scenario, fact);
 int forged = seen_rows_utimes_moves_birth(dir);

 printf("note: %s: %s; the judge must answer %s", row, seen_rows_birth_fact_name(fact),
        seen_rows_verdict_name(verdict));
 if (forged == 1)
  printf(" (utimes moved the birth time back here)");
 else if (forged == 0)
  printf(" (utimes did not move the birth time here)");
 seen_rows_print_forward_note(dir);
 printf("\n");
 return(verdict);
}

#endif
