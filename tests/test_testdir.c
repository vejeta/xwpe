/* test_testdir.c -- unit test for tests/testdir.h, the one place a C test
 * makes its folder and states what it assumes about the user running it.
 *
 * What goes wrong if a helper breaks: a test folder in a group the user is
 * not in (every setgid row of a BSD run then dies in its setup with a bare
 * "Operation not permitted" and hides the rows after it), a file made with a
 * mode other than the one asked for and trusted, a premise that does not
 * hold passing silently, or a failure the helper printed that the test's
 * exit status does not count.
 *
 * The chown of a folder and the fchmod of a file are played by the test (the
 * chown here, tests/fakechmod.h for the rest), so the refusals a BSD gives
 * are made on any system.  Always runs.
 */
/* @scenario SC-unsaved-text-19 */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "testdir.h"
#include "fakechmod.h"

static int fails;

static void chk_int(const char *what, long got, long want)
{
 if (got != want) {
  printf("FAIL: %s: got %ld want %ld\n", what, got, want);
  fails++;
 }
}

static void chk_has(const char *what, const char *got, const char *want)
{
 if (!strstr(got, want)) {
  printf("FAIL: %s: \"%s\" lacks \"%s\"\n", what, got, want);
  fails++;
 }
}

static void chk_lacks(const char *what, const char *got, const char *not_want)
{
 if (strstr(got, not_want)) {
  printf("FAIL: %s: \"%s\" has \"%s\"\n", what, got, not_want);
  fails++;
 }
}

/* ---- the chown of a folder, as the test's own ---- */

typedef int (*chown_fn)(const char *, uid_t, gid_t);

static int chown_refuse;          /* 1: the call fails with EPERM */
static int chown_calls;
static uid_t chown_last_uid;
static gid_t chown_last_gid;

int chown(const char *path, uid_t uid, gid_t gid)
{
 static chown_fn real;

 chown_calls++;
 chown_last_uid = uid;
 chown_last_gid = gid;
 if (chown_refuse)
 {
  errno = EPERM;
  return(-1);
 }
 if (real == NULL)
  *(void **)&real = dlsym(RTLD_NEXT, "chown");
 if (real == NULL)
 {
  fprintf(stderr, "test_testdir: the C library has no chown to pass on to\n");
  abort();
 }
 return(real(path, uid, gid));
}

/* ---- the fchdir of a socket plant, as the test's own ---- */

static int fchdir_fail_at;        /* the Nth call from now fails; 0: none */
static int fchdir_calls;

int fchdir(int fd)
{
 static int (*real)(int);

 fchdir_calls++;
 if (fchdir_fail_at != 0 && fchdir_calls == fchdir_fail_at)
 {
  errno = EACCES;
  return(-1);
 }
 if (real == NULL)
  *(void **)&real = dlsym(RTLD_NEXT, "fchdir");
 if (real == NULL)
  abort();
 return(real(fd));
}

/* ---- helpers of the test ---- */

static char out_text[4096];

/* Start collecting what the helpers print. */
static void collect_begin(void)
{
 test_dir_out = tmpfile();
 test_dir_failures = 0;
}

/* Stop collecting; what was printed is in out_text. */
static void collect_end(void)
{
 size_t n = 0;

 if (test_dir_out != NULL)
 {
  rewind(test_dir_out);
  n = fread(out_text, 1, sizeof out_text - 1, test_dir_out);
  fclose(test_dir_out);
 }
 out_text[n] = '\0';
 test_dir_out = NULL;
}

static int remove_entry(const char *path, const struct stat *sb, int type, struct FTW *ftw)
{
 (void)sb; (void)type; (void)ftw;
 remove(path);
 return(0);
}

static void remove_tree(const char *path)
{
 nftw(path, remove_entry, 8, FTW_DEPTH | FTW_PHYS);
}

static long mode_of(const char *dir, const char *name)
{
 char p[512];
 struct stat st;

 test_path_join(p, sizeof p, dir, name);
 return(lstat(p, &st) == 0 ? (long)(st.st_mode & 07777) : -1L);
}

static char folder[TEST_DIR_PATH_SIZE];

static void make_folder(void)
{
 if (test_dir_make(folder, sizeof folder, "td") != 0)
 {
  printf("FAIL: the test cannot make its own folder (%s)\n", folder);
  exit(2);
 }
}

static void drop_folder(void)
{
 remove_tree(folder);
 folder[0] = '\0';
}

/* ---- rows ---- */

/* The folder is made and is in a group the user is in: the chown asks for
   this user's group and nobody else's, and the group is read back. */
static void test_dir_make_gives_the_users_group(void)
{
 struct stat st;

 chown_calls = 0;
 make_folder();
 chk_int("the folder asks for its group once", chown_calls, 1);
 chk_int("the owner is left alone", (long)chown_last_uid, (long)(uid_t)-1);
 chk_int("the group asked for is this user's", (long)chown_last_gid, (long)getegid());
 chk_int("the folder is there", stat(folder, &st), 0);
 chk_int("its group is this user's", (long)st.st_gid, (long)getegid());
 chk_int("nothing was counted as failed", test_dir_failures, 0);
 drop_folder();
}

/* A folder that cannot be given the user's group is a failure by name, and
   the failure is counted so the test's exit status shows it. */
static void test_dir_make_says_when_the_group_is_refused(void)
{
 char dir[TEST_DIR_PATH_SIZE];
 int rc;

 collect_begin();
 chown_refuse = 1;
 rc = test_dir_make(dir, sizeof dir, "td");
 chown_refuse = 0;
 collect_end();
 chk_int("make fails", rc, -1);
 chk_has("the line names the folder's group", out_text, "cannot give it this user's group");
 chk_has("the line says why", out_text, "Operation not permitted");
 chk_int("the failure is counted", test_dir_failures, 1);
 test_dir_failures = 0;
 if (dir[0] != '\0')
  remove_tree(dir);
}

/* A base that is not there is not a failure of the helper: the caller (a
   second file system under /dev/shm) decides. */
static void test_dir_make_under_a_missing_base_is_quiet(void)
{
 char dir[TEST_DIR_PATH_SIZE];
 int rc;

 collect_begin();
 rc = test_dir_make_under("/nonexistent-base-for-test-testdir", dir, sizeof dir, "td");
 collect_end();
 chk_int("make_under fails", rc, -1);
 chk_int("the folder name is empty", dir[0], '\0');
 chk_int("nothing is printed", (long)strlen(out_text), 0);
 chk_int("nothing is counted", test_dir_failures, 0);
}

/* Any other reason the folder cannot be made (here a base this user may not
   write to) is a counted failure by name, not a quiet "no such base". */
static void test_dir_make_under_an_unwritable_base_is_a_failure(void)
{
 char dir[TEST_DIR_PATH_SIZE];
 int rc;

 if (access("/", W_OK) == 0)
  return;                       /* a user who may write to / has no such base */
 collect_begin();
 rc = test_dir_make_under("/", dir, sizeof dir, "td");
 collect_end();
 chk_int("make_under fails", rc, -1);
 chk_has("the line names the base", out_text, "FAIL: test folder under /: cannot make it: ");
 chk_int("the failure is counted", test_dir_failures, 1);
 test_dir_failures = 0;
}

/* A file is made with the text and mode asked for, as a new inode: a hard
   link someone left under the name keeps its bytes. */
static void test_put_mode_makes_a_new_file_with_its_mode(void)
{
 char p[512], q[512], buf[64];
 struct stat a, b;
 FILE *fp;

 make_folder();
 chk_int("put 0644", test_put_mode(folder, "plain.txt", "plain\n", 0644), 0);
 chk_int("put 04755", test_put_mode(folder, "suid.txt", "setuid\n", 04755), 0);
 chk_int("put 02755", test_put_mode(folder, "sgid.txt", "setgid\n", 02755), 0);
 chk_int("put 0200", test_put_mode(folder, "wo.txt", "x\n", 0200), 0);
 chk_int("the mode 0644", mode_of(folder, "plain.txt"), 0644);
 chk_int("the mode 04755", mode_of(folder, "suid.txt"), 04755);
 chk_int("the mode 02755", mode_of(folder, "sgid.txt"), 02755);
 chk_int("the mode 0200", mode_of(folder, "wo.txt"), 0200);
 snprintf(p, sizeof p, "%s/plain.txt", folder);
 snprintf(q, sizeof q, "%s/linked.txt", folder);
 chk_int("a second name", link(p, q), 0);
 stat(p, &a);
 chk_int("put again", test_put_mode(folder, "plain.txt", "again\n", 0600), 0);
 stat(p, &b);
 chk_int("a new inode", a.st_ino != b.st_ino, 1);
 fp = fopen(q, "r");
 buf[0] = '\0';
 if (fp != NULL)
 {
  if (fgets(buf, sizeof buf, fp) == NULL)
   buf[0] = '\0';
  fclose(fp);
 }
 chk_int("the link keeps the old bytes", strcmp(buf, "plain\n"), 0);
 chk_int("nothing was counted as failed", test_dir_failures, 0);
 drop_folder();
}

/* A system that refuses the setgid bit (the BSDs, outside the file's group)
   is said by step and mode, and the run goes on. */
static void test_put_mode_says_the_step_that_was_refused(void)
{
 int rc;

 make_folder();
 fakechmod_set(FAKECHMOD_REFUSE, S_ISGID);
 collect_begin();
 rc = test_put_mode(folder, "sgid.txt", "x\n", 02755);
 collect_end();
 fakechmod_set(FAKECHMOD_OFF, 0);
 chk_int("put fails", rc, -1);
 chk_has("the whole line", out_text, "FAIL: put sgid.txt 02755: fchmod: Operation not permitted\n");
 chk_int("the failure is counted", test_dir_failures, 1);
 test_dir_failures = 0;
 drop_folder();
}

/* A system that takes the call and keeps less (Linux, outside the group) is
   a refusal too: the mode is read back. */
static void test_put_mode_says_when_a_bit_was_dropped(void)
{
 int rc;

 make_folder();
 fakechmod_set(FAKECHMOD_DROP, S_ISGID);
 collect_begin();
 rc = test_put_mode(folder, "sgid.txt", "x\n", 02755);
 collect_end();
 fakechmod_set(FAKECHMOD_OFF, 0);
 chk_int("put fails", rc, -1);
 chk_has("the line says what was read back", out_text, "FAIL: put sgid.txt 02755: mode read back 0755\n");
 chk_int("the failure is counted", test_dir_failures, 1);
 test_dir_failures = 0;
 drop_folder();
}

/* The folder and chmod helpers read the mode back, and say the step. */
static void test_mkdir_and_chmod_mode_read_back(void)
{
 int rc;

 make_folder();
 chk_int("mkdir 02755", test_mkdir_mode(folder, "sg", 02755), 0);
 chk_int("the folder's mode", mode_of(folder, "sg"), 02755);
 chk_int("chmod 0711", test_chmod_mode(folder, "sg", 0711), 0);
 chk_int("the folder's new mode", mode_of(folder, "sg"), 0711);
 fakechmod_set(FAKECHMOD_REFUSE, S_ISGID);
 collect_begin();
 rc = test_mkdir_mode(folder, "sg2", 02755);
 collect_end();
 fakechmod_set(FAKECHMOD_OFF, 0);
 chk_int("a refused mkdir mode fails", rc, -1);
 chk_has("the line names mkdir and chmod", out_text, "FAIL: mkdir sg2 02755: chmod: Operation not permitted\n");
 chk_int("the failure is counted", test_dir_failures, 1);
 collect_begin();
 rc = test_chmod_mode(folder, "missing", 0700);
 collect_end();
 chk_int("chmod of a name that is not there fails", rc, -1);
 chk_has("the line names chmod", out_text, "FAIL: chmod missing 0700: chmod: ");
 test_dir_failures = 0;
 /* a system that takes the call and keeps less is a refusal too */
 fakechmod_set(FAKECHMOD_DROP, S_ISGID);
 collect_begin();
 rc = test_mkdir_mode(folder, "sg3", 02755);
 collect_end();
 chk_int("a dropped bit on mkdir fails", rc, -1);
 chk_has("mkdir reads the mode back", out_text, "FAIL: mkdir sg3 02755: mode read back 0755\n");
 chk_int("the dropped mkdir is counted", test_dir_failures, 1);
 test_dir_failures = 0;
 collect_begin();
 rc = test_chmod_mode(folder, "sg", 02755);
 collect_end();
 fakechmod_set(FAKECHMOD_OFF, 0);
 chk_int("a dropped bit on chmod fails", rc, -1);
 chk_has("chmod reads the mode back", out_text, "FAIL: chmod sg 02755: mode read back 0755\n");
 chk_int("the dropped chmod is counted", test_dir_failures, 1);
 test_dir_failures = 0;
 drop_folder();
}

/* The probe for the setgid premise says yes where the bit is kept, and no
   where the system refuses it or drops it. */
static void test_special_bits_probe_follows_the_system(void)
{
 make_folder();
 chk_int("setuid and setgid are kept here", test_premise_special_bits_settable(folder, S_ISUID | S_ISGID), 1);
 fakechmod_set(FAKECHMOD_REFUSE, S_ISGID);
 chk_int("refused: no", test_premise_special_bits_settable(folder, S_ISGID), 0);
 chk_int("refused for setgid, setuid alone still yes", test_premise_special_bits_settable(folder, S_ISUID), 1);
 fakechmod_set(FAKECHMOD_DROP, S_ISGID);
 chk_int("dropped: no", test_premise_special_bits_settable(folder, S_ISGID), 0);
 fakechmod_set(FAKECHMOD_OFF, 0);
 chk_int("the probe leaves no file", mode_of(folder, "probe_bits"), -1);
 chk_int("a probe is not a failure", test_dir_failures, 0);
 drop_folder();
}

/* fakechmod_last_rc is the value of the call just made, on every path: a
   call that succeeded before, then one the face refuses, leaves the refusal,
   and a count reset leaves no earlier value to be inherited.  Red under the
   mutation "a refused call does not set it". */
static void test_fakechmod_last_rc_is_the_last_calls(void)
{
 char path[512];
 mode_t refused_mode = S_ISGID | 0755;

 make_folder();
 snprintf(path, sizeof path, "%s/%s", folder, "plain.txt");
 chk_int("put", test_put_mode(folder, "plain.txt", "x\n", 0644), 0);
 chk_int("a call that succeeds", fchmodat(AT_FDCWD, path, 0600, 0), 0);
 chk_int("its value", fakechmod_last_rc, 0);
 fakechmod_set(FAKECHMOD_REFUSE, S_ISGID);
 chk_int("a call the face refuses", fchmodat(AT_FDCWD, path, refused_mode, 0), -1);
 fakechmod_set(FAKECHMOD_OFF, 0);
 chk_int("its value, not the earlier one", fakechmod_last_rc, -1);
 chk_int("a call that succeeds again", fchmodat(AT_FDCWD, path, 0600, 0), 0);
 fakechmod_count_reset();
 chk_int("a count reset leaves none to inherit", fakechmod_last_rc, -1);
 drop_folder();
}

/* A premise that does not hold is a SKIP by name for a user who cannot be
   expected to meet it, and a counted FAIL when the run says it is
   unprivileged.  The SKIP line is the one the BSD CI's list greps. */
static void test_premise_skips_or_fails_by_the_run(void)
{
 int rc;

 unsetenv(TEST_DIR_UNPRIVILEGED_ENV);
 collect_begin();
 rc = test_premise(1, "a row", "what it needs");
 collect_end();
 chk_int("a premise that holds returns 1", rc, 1);
 chk_int("and prints nothing", (long)strlen(out_text), 0);

 collect_begin();
 rc = test_premise(0, "a row", "what it needs");
 collect_end();
 chk_int("a premise that does not hold returns 0", rc, 0);
 chk_has("the SKIP line", out_text, "SKIP: a row: what it needs\n");
 chk_lacks("it is not a FAIL", out_text, "FAIL");
 chk_int("and not counted", test_dir_failures, 0);

 setenv(TEST_DIR_UNPRIVILEGED_ENV, "1", 1);
 collect_begin();
 rc = test_premise(0, "a row", "what it needs");
 collect_end();
 unsetenv(TEST_DIR_UNPRIVILEGED_ENV);
 chk_int("unprivileged: returns 0", rc, 0);
 chk_has("the FAIL line", out_text, "FAIL: a row: the run is unprivileged, so this must hold: what it needs\n");
 chk_lacks("it is not a SKIP", out_text, "SKIP");
 chk_int("and counted", test_dir_failures, 1);
 test_dir_failures = 0;
}

/* An observation the system does not let a row make is a SKIP by name even
   when the run says it is unprivileged, and never a counted FAIL.  Red under
   the mutation "test_observation calls test_premise". */
static void test_an_observation_skips_under_the_unprivileged_flag(void)
{
 int rc;

 setenv(TEST_DIR_UNPRIVILEGED_ENV, "1", 1);
 collect_begin();
 rc = test_observation(1, "a row", "what it needs");
 collect_end();
 chk_int("an observation that is there returns 1", rc, 1);
 chk_int("and prints nothing", (long)strlen(out_text), 0);

 collect_begin();
 rc = test_observation(0, "a row", "what it needs");
 collect_end();
 unsetenv(TEST_DIR_UNPRIVILEGED_ENV);
 chk_int("unprivileged: returns 0", rc, 0);
 chk_has("the SKIP line", out_text, "SKIP: a row: cannot be observed here: what it needs\n");
 chk_lacks("it is not a FAIL", out_text, "FAIL");
 chk_int("and not counted", test_dir_failures, 0);
}

/* The two probes of a mode that keeps a user out leave the folder as it was. */
static void test_deny_mode_probes_leave_no_trace(void)
{
 struct stat st;
 int refuses, unreadable;

 make_folder();
 refuses = test_premise_folder_refuses_new_files(folder);
 unreadable = test_premise_file_0200_is_unreadable(folder);
 chk_int("the folder is there", stat(folder, &st), 0);
 chk_int("the folder is 0700 again", (long)(st.st_mode & 07777), 0700);
 chk_int("no create probe is left", mode_of(folder, "probe_new"), -1);
 chk_int("no read probe is left", mode_of(folder, "probe_read"), -1);
 if (geteuid() == 0)
 {
  chk_int("root creates in a 0500 folder", refuses, 0);
  chk_int("root reads a 0200 file", unreadable, 0);
 }
 else
 {
  chk_int("a user is refused in a 0500 folder", refuses, 1);
  chk_int("a user cannot read a 0200 file", unreadable, 1);
 }
 chk_int("nothing was counted as failed", test_dir_failures, 0);
 drop_folder();
}

/* The other group is one of the user's own, and not the effective one. */
static void test_other_group_is_a_group_of_ours(void)
{
 gid_t g = test_other_group();
 gid_t groups[64];
 int n, i, found = 0;

 if (g == TEST_DIR_NO_GROUP)
  return;
 n = getgroups((int)(sizeof groups / sizeof groups[0]), groups);
 for (i = 0; i < n; i++)
  if (groups[i] == g)
   found = 1;
 chk_int("a group of ours", found, 1);
 chk_int("not the effective one", g != getegid(), 1);
}

/* A template that does not fit the caller's buffer is a counted FAIL that
   names both lengths, and no folder is made under a cut name. */
static void test_dir_make_under_refuses_a_cut_template(void)
{
 char dir[16];
 int rc;

 collect_begin();
 rc = test_dir_make_under("/var/tmp/a-base-longer-than-the-buffer", dir, sizeof dir, "td");
 collect_end();
 chk_int("make_under fails", rc, -1);
 chk_int("the folder name is empty", dir[0], '\0');
 chk_has("the line says the path is too long", out_text, "the path needs ");
 chk_has("the line says what the buffer holds", out_text, "the buffer holds 16");
 chk_int("the failure is counted", test_dir_failures, 1);
 test_dir_failures = 0;
}

/* A join that does not fit is a counted FAIL and an empty name, never a
   shorter path in another folder. */
static void test_path_join_refuses_a_cut_path(void)
{
 char out[8];
 int rc;

 chk_int("a path that fits", test_path_join(out, sizeof out, "ab", "cd"), 0);
 chk_has("is joined with a slash", out, "ab/cd");
 collect_begin();
 rc = test_path_join(out, sizeof out, "abcd", "efgh");
 collect_end();
 chk_int("a path that does not fit fails", rc, -1);
 chk_int("the name is empty", out[0], '\0');
 chk_has("the line names the path", out_text, "path abcd/efgh");
 chk_int("the failure is counted", test_dir_failures, 1);
 test_dir_failures = 0;
}

/* A socket is planted in a folder whose path is longer than a socket name
   holds, is a socket, and nothing is left under a cut name outside the
   folder. */
static void test_plant_socket_in_a_folder_longer_than_a_socket_name(void)
{
 char deep[TEST_DIR_PATH_SIZE], path[TEST_DIR_PATH_SIZE], cwd_before[TEST_DIR_PATH_SIZE];
 char cwd_after[TEST_DIR_PATH_SIZE];
 struct stat st;
 size_t used;

 make_folder();
 snprintf(deep, sizeof deep, "%s", folder);
 for (used = strlen(deep); used < test_plant_socket_name_max() + 40; used = strlen(deep))
 {
  strcat(deep, "/dddddddddddddddddddddddddddddddddddd");
  chk_int("a deeper folder", mkdir(deep, 0700), 0);
 }
 test_path_join(path, sizeof path, deep, "s");
 if (getcwd(cwd_before, sizeof cwd_before) == NULL)
  cwd_before[0] = '\0';
 chk_int("the socket is planted", test_plant_socket(path), 0);
 chk_int("it is there", lstat(path, &st), 0);
 chk_int("it is a socket", S_ISSOCK(st.st_mode) != 0, 1);
 if (getcwd(cwd_after, sizeof cwd_after) == NULL)
  cwd_after[0] = '\0';
 chk_int("the working folder is as it was", strcmp(cwd_before, cwd_after), 0);
 chk_int("no failure was counted", test_dir_failures, 0);
 drop_folder();
}

/* A last name longer than a socket name holds is refused by name, and the
   working folder is put back. */
static void test_plant_socket_refuses_a_name_longer_than_a_socket_name(void)
{
 char path[TEST_DIR_PATH_SIZE], name[200];
 int rc;

 make_folder();
 memset(name, 'n', sizeof name - 1);
 name[sizeof name - 1] = '\0';
 test_path_join(path, sizeof path, folder, name);
 collect_begin();
 rc = test_plant_socket(path);
 collect_end();
 chk_int("the plant fails", rc, -1);
 chk_has("the line says the name is too long", out_text, "is longer than a socket name holds");
 chk_int("the failure is counted", test_dir_failures, 1);
 test_dir_failures = 0;
 drop_folder();
}

/* A plant that cannot return to the working folder ends the run at once with
   a line naming the reason: the child's second fchdir fails, and it must exit
   2 and say so, never return to the next row in the wrong folder. */
static void test_plant_socket_that_cannot_return_ends_the_run(void)
{
 char path[TEST_DIR_PATH_SIZE], line[512];
 int pipefd[2], status = 0;
 ssize_t got;
 pid_t pid;

 make_folder();
 test_path_join(path, sizeof path, folder, "s");
 if (pipe(pipefd) != 0)
 {
  chk_int("a pipe", -1, 0);
  drop_folder();
  return;
 }
 fflush(stdout);
 pid = fork();
 if (pid == 0)
 {
  close(pipefd[0]);
  dup2(pipefd[1], STDOUT_FILENO);
  fchdir_calls = 0;
  fchdir_fail_at = 2;
  test_plant_socket(path);
  _exit(0);
 }
 close(pipefd[1]);
 got = read(pipefd[0], line, sizeof line - 1);
 line[got > 0 ? got : 0] = '\0';
 close(pipefd[0]);
 if (pid < 0 || waitpid(pid, &status, 0) != pid)
  status = -1;
 chk_int("the run ended with exit 2", WIFEXITED(status) ? WEXITSTATUS(status) : -1, 2);
 chk_has("the line says it could not return", line, "cannot return to the working folder: Permission denied");
 drop_folder();
}

int main(void)
{
 fakechmod_set(FAKECHMOD_OFF, 0);       /* the rows arm the fake themselves */
 test_plant_socket_that_cannot_return_ends_the_run();
 test_dir_make_under_refuses_a_cut_template();
 test_path_join_refuses_a_cut_path();
 test_plant_socket_in_a_folder_longer_than_a_socket_name();
 test_plant_socket_refuses_a_name_longer_than_a_socket_name();
 test_dir_make_gives_the_users_group();
 test_dir_make_says_when_the_group_is_refused();
 test_dir_make_under_a_missing_base_is_quiet();
 test_dir_make_under_an_unwritable_base_is_a_failure();
 test_put_mode_makes_a_new_file_with_its_mode();
 test_put_mode_says_the_step_that_was_refused();
 test_put_mode_says_when_a_bit_was_dropped();
 test_mkdir_and_chmod_mode_read_back();
 test_special_bits_probe_follows_the_system();
 test_fakechmod_last_rc_is_the_last_calls();
 test_premise_skips_or_fails_by_the_run();
 test_an_observation_skips_under_the_unprivileged_flag();
 test_deny_mode_probes_leave_no_trace();
 test_other_group_is_a_group_of_ours();
 if (fails) {
  printf("test_testdir: %d failure(s)\n", fails);
  return(1);
 }
 printf("test_testdir: ok\n");
 return(0);
}
