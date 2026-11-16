/* testdir.h -- the one place a C test makes the folder it works in and states
 * what it assumes about the user who runs it.
 *
 * A test that sets a special bit or a mode that keeps a reader out assumes
 * things about the invoking user: that the folder's group is one the user is
 * in (on the BSDs a new file takes its folder's group, and /tmp is not the
 * user's), that the user may set the bit at all, that file modes are honoured
 * (root and a capability that overrides them read a 0200 file).  When such an
 * assumption fails the row must say so by name: SKIP for a user who cannot
 * be expected to meet it, FAIL when the run says it is unprivileged
 * (WPE_TEST_UNPRIVILEGED=1, set by the BSD CI) and the premise must hold.
 * It must never end the program with a bare perror, which hides every row
 * after it.
 *
 * Every helper prints its own one-line FAIL (the step, the mode in octal and
 * strerror), counts it in test_dir_failures and returns -1 or 0; none of
 * them exits.  A test adds test_dir_failures to its own count in main, and a
 * row whose setup failed returns early.  FAIL lines go to stdout, or to
 * test_dir_out when a test collects them.
 *
 * Static inline, libc only, like test_watchdog.h.  tests/check-mode-premise.sh
 * fails a test that sets a special-bit or deny mode without it.
 */
#ifndef TEST_TESTDIR_H
#define TEST_TESTDIR_H

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define TEST_DIR_UNPRIVILEGED_ENV "WPE_TEST_UNPRIVILEGED"
#define TEST_DIR_PATH_SIZE   1024
#define TEST_DIR_WHY_SIZE    160
#define TEST_DIR_GROUPS_MAX  64
#define TEST_DIR_MODE_BITS   07777
#define TEST_DIR_NO_GROUP    ((gid_t)-1)

/* Failures the helpers printed; the test adds this to its own count. */
static int test_dir_failures;

/* Where the helpers print; NULL is stdout. */
static FILE *test_dir_out;

#ifdef __GNUC__
static void test_dir_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void test_dir_fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#endif

static void test_dir_say(const char *fmt, ...)
{
 va_list ap;

 va_start(ap, fmt);
 vfprintf(test_dir_out != NULL ? test_dir_out : stdout, fmt, ap);
 va_end(ap);
}

/**
 * test_dir_fail - Print one FAIL line and count it.
 *
 * The count is what makes an unchecked helper result still end the run with a
 * failure, so a premise that did not hold cannot pass unseen.
 */
static void test_dir_fail(const char *fmt, ...)
{
 va_list ap;

 va_start(ap, fmt);
 vfprintf(test_dir_out != NULL ? test_dir_out : stdout, fmt, ap);
 va_end(ap);
 test_dir_failures++;
}

/**
 * test_dir_make_under - Make a test folder under BASE, in a group of this user.
 *
 * Where BASE does not exist (ENOENT, ENOTDIR) the result is -1 with DIR
 * empty and nothing printed: a test that wants a second file system under a
 * path that may not be there (/dev/shm) decides for itself.  Any other
 * failure to make the folder (a full disk, no permission) is a counted FAIL
 * with DIR empty.  A folder that
 * was made but cannot be given this user's group is a FAIL, because every
 * special-bit row in it would then fail for the system's group rule and not
 * for the editor's.  Any user may give a folder it owns a group it is in.
 *
 * Return: 0, or -1.
 */
static int test_dir_make_under(const char *base, char *dir, size_t n, const char *prefix)
{
 struct stat st;
 gid_t me = getegid();

 snprintf(dir, n, "%s/%s.XXXXXX", base, prefix);
 if (mkdtemp(dir) == NULL)
 {
  int why = errno;

  if (why != ENOENT && why != ENOTDIR)
   test_dir_fail("FAIL: test folder under %s: cannot make it: %s\n", base, strerror(why));
  dir[0] = '\0';
  errno = why;
  return(-1);
 }
 if (chown(dir, (uid_t)-1, me) != 0)
 {
  test_dir_fail("FAIL: test folder %s: cannot give it this user's group %ld: %s\n",
                dir, (long)me, strerror(errno));
  return(-1);
 }
 if (stat(dir, &st) != 0)
 {
  test_dir_fail("FAIL: test folder %s: cannot read its group: %s\n", dir, strerror(errno));
  return(-1);
 }
 if (st.st_gid != me)
 {
  test_dir_fail("FAIL: test folder %s: its group is %ld, not this user's %ld\n",
                dir, (long)st.st_gid, (long)me);
  return(-1);
 }
 return(0);
}

/**
 * test_dir_make - Make the folder a test works in, under $TMPDIR or /tmp.
 *
 * On the BSDs a new file takes its folder's group and /tmp is root's, so a
 * test that put a setgid file there as an ordinary user was refused for a
 * reason that had nothing to do with the editor.  The folder made here is in
 * a group the user is in, and the helper says so by name when it cannot be.
 *
 * Return: 0, or -1 (DIR is empty when no folder could be made at all).
 */
static int test_dir_make(char *dir, size_t n, const char *prefix)
{
 const char *base = getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp";
 int before = test_dir_failures;
 int rc = test_dir_make_under(base, dir, n, prefix);

 if (rc != 0 && dir[0] == '\0' && test_dir_failures == before)
  test_dir_fail("FAIL: test folder: cannot make one under %s: %s\n", base, strerror(errno));
 return(rc);
}

static void test_dir_why(char *why, size_t n, const char *step)
{
 snprintf(why, n, "%s: %s", step, strerror(errno));
}

/**
 * test_dir_put_step - Make a file with this text and exactly this mode, quietly.
 *
 * The name is removed first and made again with O_EXCL, so the file is a new
 * inode and never the target of a link someone left under that name.  The
 * mode is set after the text (writing clears a setuid bit) and read back: a
 * system that accepted the call and kept less is as much a refusal as one
 * that said no.
 *
 * Return: 0, or -1 with WHY naming the step that failed.
 */
static int test_dir_put_step(const char *dir, const char *name, const char *text,
                             mode_t mode, char *why, size_t n)
{
 char path[TEST_DIR_PATH_SIZE];
 struct stat st;
 size_t len = strlen(text), done = 0;
 ssize_t got;
 int fd;

 snprintf(path, sizeof path, "%s/%s", dir, name);
 if (unlink(path) != 0 && errno != ENOENT)
 {
  test_dir_why(why, n, "unlink");
  return(-1);
 }
 fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
 if (fd < 0)
 {
  test_dir_why(why, n, "open");
  return(-1);
 }
 while (done < len)
 {
  got = write(fd, text + done, len - done);
  if (got < 0)
  {
   test_dir_why(why, n, "write");
   close(fd);
   return(-1);
  }
  done += (size_t)got;
 }
 if (fchmod(fd, mode) != 0)
 {
  test_dir_why(why, n, "fchmod");
  close(fd);
  return(-1);
 }
 if (fstat(fd, &st) != 0)
 {
  test_dir_why(why, n, "fstat");
  close(fd);
  return(-1);
 }
 close(fd);
 if ((st.st_mode & TEST_DIR_MODE_BITS) != mode)
 {
  snprintf(why, n, "mode read back 0%o", (unsigned)(st.st_mode & TEST_DIR_MODE_BITS));
  return(-1);
 }
 return(0);
}

/**
 * test_put_mode - Put a file in DIR with this text and mode, or say which step failed.
 *
 * A row's setup that cannot be made as written reads "FAIL: put sgid.txt
 * 02755: fchmod: Operation not permitted" and the rows after it still run.
 *
 * Return: 0, or -1 (printed and counted).
 */
static int test_put_mode(const char *dir, const char *name, const char *text, mode_t mode)
{
 char why[TEST_DIR_WHY_SIZE];

 if (test_dir_put_step(dir, name, text, mode, why, sizeof why) == 0)
  return(0);
 test_dir_fail("FAIL: put %s 0%o: %s\n", name, (unsigned)mode, why);
 return(-1);
}

static int test_dir_mode_check(const char *path, mode_t mode, char *why, size_t n)
{
 struct stat st;

 if (chmod(path, mode) != 0)
 {
  test_dir_why(why, n, "chmod");
  return(-1);
 }
 if (stat(path, &st) != 0)
 {
  test_dir_why(why, n, "stat");
  return(-1);
 }
 if ((st.st_mode & TEST_DIR_MODE_BITS) != mode)
 {
  snprintf(why, n, "mode read back 0%o", (unsigned)(st.st_mode & TEST_DIR_MODE_BITS));
  return(-1);
 }
 return(0);
}

/**
 * test_mkdir_mode - Make a folder in DIR with exactly this mode, or say which step failed.
 *
 * mkdir applies the umask and the folder's own setgid rule, so the mode is
 * set again by chmod and read back, which is what a row that asserts a
 * folder's bits needs as its starting point.
 *
 * Return: 0, or -1 (printed and counted).
 */
static int test_mkdir_mode(const char *dir, const char *name, mode_t mode)
{
 char path[TEST_DIR_PATH_SIZE], why[TEST_DIR_WHY_SIZE];

 snprintf(path, sizeof path, "%s/%s", dir, name);
 if (mkdir(path, 0700) != 0)
  test_dir_why(why, sizeof why, "mkdir");
 else if (test_dir_mode_check(path, mode, why, sizeof why) == 0)
  return(0);
 test_dir_fail("FAIL: mkdir %s 0%o: %s\n", name, (unsigned)mode, why);
 return(-1);
}

/**
 * test_chmod_mode - Give an existing entry of DIR exactly this mode, or say why not.
 *
 * Return: 0, or -1 (printed and counted).
 */
static int test_chmod_mode(const char *dir, const char *name, mode_t mode)
{
 char path[TEST_DIR_PATH_SIZE], why[TEST_DIR_WHY_SIZE];

 snprintf(path, sizeof path, "%s/%s", dir, name);
 if (test_dir_mode_check(path, mode, why, sizeof why) == 0)
  return(0);
 test_dir_fail("FAIL: chmod %s 0%o: %s\n", name, (unsigned)mode, why);
 return(-1);
}

/**
 * test_premise - Say by name why a row is skipped when what it needs is missing.
 *
 * A skipped row shows in the log with its reason, so a run that cannot test a
 * permission row says so instead of failing or passing silently.  A run that
 * says it is unprivileged (WPE_TEST_UNPRIVILEGED=1) has no such excuse: the
 * premise must hold there, so the same line is a FAIL and is counted.
 *
 * Return: HOLDS, so the caller can write "if (!test_premise(...)) return;".
 */
static int test_premise(int holds, const char *row, const char *missing)
{
 const char *flag = getenv(TEST_DIR_UNPRIVILEGED_ENV);

 if (holds)
  return(1);
 if (flag != NULL && strcmp(flag, "1") == 0)
  test_dir_fail("FAIL: %s: the run is unprivileged, so this must hold: %s\n", row, missing);
 else
  test_dir_say("SKIP: %s: %s\n", row, missing);
 return(0);
}

/**
 * test_premise_folder_refuses_new_files - Ask the system whether a 0500 folder keeps files out.
 *
 * Rows that need a folder that says no to a new file would fail for a reason
 * that has nothing to do with the editor when the user is root, or holds the
 * capability that overrides file modes.  This tries it in DIR, so such rows
 * skip by name on that user and run for everybody else.  DIR is put back to
 * 0700.
 *
 * Return: 1 when the create was refused, 0 when it succeeded or the probe
 * could not be set up (printed and counted).
 */
static int test_premise_folder_refuses_new_files(const char *dir)
{
 char p[TEST_DIR_PATH_SIZE];
 int fd, refused;

 snprintf(p, sizeof p, "%s/probe_new", dir);
 if (chmod(dir, 0500) != 0)
 {
  test_dir_fail("FAIL: probe: chmod %s 0500: %s\n", dir, strerror(errno));
  return(0);
 }
 fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0600);
 refused = (fd < 0);
 if (chmod(dir, 0700) != 0)
  test_dir_fail("FAIL: probe: chmod %s 0700: %s\n", dir, strerror(errno));
 if (fd >= 0)
 {
  close(fd);
  unlink(p);
 }
 return(refused);
}

/**
 * test_premise_file_0200_is_unreadable - Ask the system whether a 0200 file keeps its reader out.
 *
 * The row that writes a file the user can write but not read needs the read
 * to fail.  A user whose file modes are overridden reads it, and the row
 * would fail for the wrong reason.  This tries it in DIR.
 *
 * Return: 1 when the read was refused, 0 when it succeeded or the probe
 * could not be set up (printed and counted).
 */
static int test_premise_file_0200_is_unreadable(const char *dir)
{
 char p[TEST_DIR_PATH_SIZE];
 int fd, refused;

 if (test_put_mode(dir, "probe_read", "x\n", 0200) != 0)
  return(0);
 snprintf(p, sizeof p, "%s/probe_read", dir);
 fd = open(p, O_RDONLY);
 refused = (fd < 0);
 if (fd >= 0)
  close(fd);
 unlink(p);
 return(refused);
}

/**
 * test_premise_special_bits_settable - Ask the system whether this user keeps these special bits here.
 *
 * Makes a file in DIR, asks for 0644 with BITS, reads the mode back and
 * removes it.  A refusal (the BSDs, for setgid outside the file's group) and
 * a silent drop (Linux) both read as "no", so the row that needs a setgid
 * file skips by name instead of dying inside its setup.  Sticky on a regular
 * file is refused to a user by FreeBSD and NetBSD; the probe says so for
 * whoever asks.
 *
 * Return: 1 when every bit of BITS is on afterwards, else 0.
 */
static int test_premise_special_bits_settable(const char *dir, mode_t bits)
{
 char p[TEST_DIR_PATH_SIZE], why[TEST_DIR_WHY_SIZE];
 int holds;

 holds = (test_dir_put_step(dir, "probe_bits", "x\n", 0644 | bits, why, sizeof why) == 0);
 snprintf(p, sizeof p, "%s/probe_bits", dir);
 unlink(p);
 return(holds);
}

/**
 * test_other_group - Find a group of this user that is not the effective one.
 *
 * A row that needs a file in another group of the user's uses this, and
 * skips by name when there is none.
 *
 * Return: the group, or TEST_DIR_NO_GROUP.
 */
static gid_t test_other_group(void)
{
 gid_t groups[TEST_DIR_GROUPS_MAX];
 int n, i;

 n = getgroups((int)(sizeof groups / sizeof groups[0]), groups);
 for (i = 0; i < n; i++)
  if (groups[i] != getegid())
   return(groups[i]);
 return(TEST_DIR_NO_GROUP);
}

#endif
