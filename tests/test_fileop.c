/* test_fileop.c -- unit test for the owner of "copy, move, link or remove a
 * user's file" (we_fileop.c) over the write owner's bytes (we_write.c).  The
 * editor's side is played by the stubs below.  Always runs.
 *
 * What the user would see go wrong if a rule broke: a copy that is readable
 * by others for a moment, or has lost its setgid bit, or is half a file after
 * a full disk, a link at the destination written through, a name that
 * appeared a moment ago replaced, an old file lost when the rename failed, a
 * file with a long name that cannot be copied, a copy of a file onto its own
 * hard link that empties both.
 */
/* @scenario SC-unsaved-text-47 */
/* @scenario SC-unsaved-text-48 */
/* @scenario SC-unsaved-text-48d */
/* @scenario SC-unsaved-text-48e */
/* @scenario SC-unsaved-text-48f */
/* @scenario SC-unsaved-text-50c */
/* @scenario SC-unsaved-text-51c */
/* @scenario SC-unsaved-text-52c */
/* @scenario SC-unsaved-text-53c */
/* @scenario SC-unsaved-text-54c */
/* @scenario SC-unsaved-text-56 */
/* @scenario SC-unsaved-text-57 */
/* @scenario SC-unsaved-text-58 */
/* @scenario SC-unsaved-text-59 */
/* @scenario SC-unsaved-text-60 */
/* @scenario SC-unsaved-text-61 */
/* @scenario SC-unsaved-text-62 */
/* @scenario SC-unsaved-text-63 */
/* @scenario SC-unsaved-text-64 */
/* @scenario SC-unsaved-text-65 */
/* @scenario SC-unsaved-text-66 */
/* Manual, not a row: the nondeterministic race (a folder renamed for a link
 * while it is walked, RENAME_EXCHANGE, Linux only) is a verifier probe, not
 * a repository test; after the fd-relative walk it leaks and loses nothing
 * in 0 of 40 runs on Copy, Move, a merging Move and Remove. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../edit.h"
#include "../we_write.h"
#include "../we_fileop.h"

static int fails;

static void chk_int(const char *what, long got, long want)
{
 if (got != want) {
  printf("FAIL: %s: got %ld want %ld\n", what, got, want);
  fails++;
 }
}

static void chk_str(const char *what, const char *got, const char *want)
{
 if (strcmp(got, want) != 0) {
  printf("FAIL: %s: \"%s\" want \"%s\"\n", what, got, want);
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

/* ---- the editor's side, as stubs (what we_write.c reaches) ---- */

void (*WpeMouseChangeShape)(WpeMouseShape s) = NULL;
void (*WpeMouseRestoreShape)(void) = NULL;
int e_emergency_save_running(void) { return(0); }
void e_status_say_until_key(FENSTER *f, const char *t) { (void)f; (void)t; }
int e_message(int sw, char *s, FENSTER *f) { (void)sw; (void)s; (void)f; return('Y'); }
void e_eol_nul_sentence(int count, char *buf, size_t size) { (void)count; snprintf(buf, size, "nul"); }
char *e_mkfilename(char *dr, char *fn) { char *s = malloc(strlen(dr) + strlen(fn) + 2); sprintf(s, "%s/%s", dr, fn); return(s); }
char *e_bakfilename(char *s) { return(strdup(s)); }
int e_eol_write(FILE *fp, BUFFER *b, int ya, int xa, int ye, int xe, int keep_final)
{
 (void)fp; (void)b; (void)ya; (void)xa; (void)ye; (void)xe; (void)keep_final;
 return(0);
}

/* ---- helpers ---- */

static char dir[256];
static char other[256];     /* a folder on another file system, or "" */

static int remove_entry(const char *path, const struct stat *sb, int type, struct FTW *ftw)
{
 (void)sb; (void)type; (void)ftw;
 remove(path);
 return(0);
}

static void make_dir(void)
{
 snprintf(dir, sizeof dir, "%s/fo.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
 if (!mkdtemp(dir)) {
  perror("mkdtemp");
  exit(2);
 }
 setenv("HOME", dir, 1);
 unsetenv("XWPE_TEST_FAIL");
}

static void make_other(void)
{
 struct stat a, b;

 other[0] = '\0';
 snprintf(other, sizeof other, "/dev/shm/fo.XXXXXX");
 if (!mkdtemp(other)) {
  other[0] = '\0';
  return;
 }
 if (stat(dir, &a) == 0 && stat(other, &b) == 0 && a.st_dev == b.st_dev) {
  nftw(other, remove_entry, 8, FTW_DEPTH | FTW_PHYS);
  other[0] = '\0';
 }
}

static void clean(void)
{
 nftw(dir, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
 if (other[0])
  nftw(other, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
 other[0] = '\0';
}

static void path_of(char *out, size_t n, const char *name)
{
 snprintf(out, n, "%s/%s", dir, name);
}

static void put(const char *name, const char *text, mode_t mode)
{
 char p[512];
 int fd;

 path_of(p, sizeof p, name);
 fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600);
 if (fd < 0 || write(fd, text, strlen(text)) < 0 || fchmod(fd, mode) != 0) {
  perror("put");
  exit(2);
 }
 close(fd);
}

static int get(const char *path, char *buf, size_t n)
{
 int fd = open(path, O_RDONLY);
 ssize_t got;

 if (fd < 0)
  return(-1);
 got = read(fd, buf, n - 1);
 close(fd);
 if (got < 0)
  return(-1);
 buf[got] = '\0';
 return(0);
}

static void chk_file(const char *what, const char *name, const char *want)
{
 char p[512], buf[256];

 path_of(p, sizeof p, name);
 if (get(p, buf, sizeof buf) != 0) {
  printf("FAIL: %s: %s is not there\n", what, name);
  fails++;
  return;
 }
 chk_str(what, buf, want);
}

static void chk_absent(const char *what, const char *name)
{
 char p[512];
 struct stat st;

 path_of(p, sizeof p, name);
 if (lstat(p, &st) == 0) {
  printf("FAIL: %s: %s is there\n", what, name);
  fails++;
 }
}

/* How many temporary copies the owner left in a folder. */
static int count_temp(const char *folder)
{
 int n = 0;
 DIR *d = opendir(folder);
 struct dirent *e;

 if (!d)
  return(-1);
 while ((e = readdir(d)) != NULL)
  if (strstr(e->d_name, "xwpe-copy"))
   n++;
 closedir(d);
 return(n);
}

/* Work in the test's folder, as the File Manager works in its panel's. */
#define chdir(p) go_to(p)
static void go_to(const char *p)
{
 if ((chdir)(p) != 0) {
  perror("chdir");
  exit(2);
 }
}

static int answer;
static int asked;

static int ask(void *ctx, const char *q)
{
 (void)ctx; (void)q;
 asked++;
 return(answer);
}

static void policy(WpeFileOpPolicy *p, WpeReplaceRule r, WpeRemoveRule m)
{
 memset(p, 0, sizeof *p);
 p->replace = r;
 p->remove = m;
 p->hardlink_first = 1;
 p->ask = ask;
}

/* ---- rows ---- */

/* The copy is private from its first byte and keeps all twelve bits. */
static void test_private_and_special_bits(void)
{
 WpeFileOpPolicy p;
 WpeFileOpOutcome o;
 struct stat st;
 char path[512];

 make_dir();
 put("suid.txt", "setuid\n", 04755);
 put("sgid.txt", "setgid\n", 02755);
 put("priv.txt", "private\n", 0600);
 policy(&p, WPE_REPLACE_NEVER, WPE_REMOVE_NOW);
 chdir(dir);
 chk_int("copy setuid", wpe_fileop_copy("suid.txt", "suid.copy", &p, &o), 0);
 path_of(path, sizeof path, "suid.copy");
 stat(path, &st);
 chk_int("setuid kept after the data", st.st_mode & 07777, 04755);
 chk_int("copy setgid", wpe_fileop_copy("sgid.txt", "sgid.copy", &p, &o), 0);
 path_of(path, sizeof path, "sgid.copy");
 stat(path, &st);
 chk_int("setgid kept after the data", st.st_mode & 07777, 02755);
 chk_int("copy private", wpe_fileop_copy("priv.txt", "priv.copy", &p, &o), 0);
 path_of(path, sizeof path, "priv.copy");
 stat(path, &st);
 chk_int("private stays private", st.st_mode & 07777, 0600);
 chk_int("no temporary left", count_temp(dir), 0);
 clean();
}

/* The write owner's exports: private before the first byte, never through a link. */
static void test_create_private(void)
{
 struct stat st;
 char path[512], buf[16];
 int dfd, fd = -1, err, rd;
 int src;

 make_dir();
 dfd = open(dir, O_RDONLY | O_DIRECTORY);
 umask(077);
 err = wpe_write_create_private(dfd, "made", 04755, &fd);
 chk_int("create_private", err, 0);
 fstat(fd, &st);
 chk_int("nine bits before any byte, special bits not yet", st.st_mode & 07777, 0755);
 chk_int("then the special bits after the data", wpe_write_set_special_bits(fd, 04755), 0);
 fstat(fd, &st);
 chk_int("special bits set", st.st_mode & 07777, 04755);
 close(fd);
 symlinkat("target-not-there", dfd, "planted");
 err = wpe_write_create_private(dfd, "planted", 0644, &fd);
 chk_int("a link of that name is refused", err, EEXIST);
 chk_absent("the link's target was not made", "target-not-there");
 err = wpe_write_create_private(dfd, "made", 0644, &fd);
 chk_int("an existing file is refused", err, EEXIST);
 /* the byte loop: a full device, and a source that cannot be read */
 put("data.txt", "some data\n", 0644);
 path_of(path, sizeof path, "data.txt");
 src = open(path, O_RDONLY);
 fd = open("/dev/full", O_WRONLY);
 if (fd >= 0) {
  err = wpe_write_copy_fd(src, fd, &rd);
  chk_int("copy_fd onto /dev/full", err, ENOSPC);
  chk_int("the write's error, not the read's", rd, 0);
  close(fd);
 }
 close(src);
 src = open("/proc/self/mem", O_RDONLY);
 fd = open("/dev/null", O_WRONLY);
 if (src >= 0 && fd >= 0) {
  err = wpe_write_copy_fd(src, fd, &rd);
  chk_int("copy_fd from an unreadable source", err != 0, 1);
  chk_int("the read's error", rd, 1);
 }
 if (src >= 0)
  close(src);
 if (fd >= 0)
  close(fd);
 (void)buf;
 close(dfd);
 clean();
}

/* Copy onto a hard link of itself empties nothing. */
static void test_same_file(void)
{
 WpeFileOpPolicy p;
 WpeFileOpOutcome o;
 char a[512], b[512];

 make_dir();
 put("one.txt", "bytes\n", 0644);
 path_of(a, sizeof a, "one.txt");
 path_of(b, sizeof b, "two.txt");
 link(a, b);
 policy(&p, WPE_REPLACE_ALWAYS, WPE_REMOVE_NOW);
 chk_int("copy onto its own hard link", wpe_fileop_copy(a, b, &p, &o), -1);
 chk_int("FAILED", o.status, WPE_FILEOP_FAILED);
 chk_int("the same file", o.step, WPE_FILEOP_STEP_SAME_FILE);
 chk_file("first name intact", "one.txt", "bytes\n");
 chk_file("second name intact", "two.txt", "bytes\n");
 chk_int("move onto its own hard link", wpe_fileop_move(a, b, &p, &o), -1);
 chk_int("same file for a move", o.step, WPE_FILEOP_STEP_SAME_FILE);
 clean();
}

/* A device, a pipe or a socket in the way is refused before anything moves. */
static void test_not_a_file(void)
{
 WpeFileOpPolicy p;
 WpeFileOpOutcome o;
 char buf[2048], fifo[512];
 struct stat st;

 make_dir();
 chdir(dir);
 put("src.txt", "data\n", 0644);
 path_of(fifo, sizeof fifo, "pipe");
 mkfifo(fifo, 0600);
 policy(&p, WPE_REPLACE_ALWAYS, WPE_REMOVE_NOW);
 chk_int("copy onto a pipe", wpe_fileop_copy("src.txt", fifo, &p, &o), -1);
 chk_int("the pipe is not a file", o.step, WPE_FILEOP_STEP_NOT_A_FILE);
 chk_int("the pipe is still a pipe", lstat(fifo, &st) == 0 && S_ISFIFO(st.st_mode), 1);
 wpe_fileop_sentence(buf, sizeof buf, &o);
 chk_has("sentence names the pipe", buf, "pipe is not a file");
 chk_has("and that nothing changed", buf, "Nothing was changed");
 chk_int("copy from a pipe", wpe_fileop_copy(fifo, "out.bin", &p, &o), -1);
 chk_int("a pipe as the source is not a file", o.step, WPE_FILEOP_STEP_NOT_A_FILE);
 chk_absent("no out.bin", "out.bin");
 if (access("/dev/full", F_OK) == 0) {
  chk_int("copy onto /dev/full", wpe_fileop_copy("src.txt", "/dev/full", &p, &o), -1);
  chk_int("a device is not a file", o.step, WPE_FILEOP_STEP_NOT_A_FILE);
 }
 clean();
}

/* A destination named as a folder takes the file inside. */
static void test_folder_destination(void)
{
 WpeFileOpPolicy p;
 WpeFileOpOutcome o;
 char folder[512];

 make_dir();
 chdir(dir);
 put("src.txt", "data\n", 0644);
 path_of(folder, sizeof folder, "box");
 mkdir(folder, 0755);
 policy(&p, WPE_REPLACE_NEVER, WPE_REMOVE_NOW);
 chk_int("copy onto a folder", wpe_fileop_copy("src.txt", "box", &p, &o), 0);
 chk_file("inside the folder", "box/src.txt", "data\n");
 chk_int("move onto a folder", wpe_fileop_move("src.txt", "box", &p, &o), -1);
 chk_int("declined: the file inside exists", o.status, WPE_FILEOP_DECLINED);
 chk_file("source kept", "src.txt", "data\n");
 clean();
}

/* "Don't overwrite", the question, and the answers. */
static void test_policy(void)
{
 WpeFileOpPolicy p;
 WpeFileOpOutcome o;
 char buf[2048];

 make_dir();
 chdir(dir);
 put("src.txt", "NEW\n", 0644);
 put("dst.txt", "OLD\n", 0644);
 policy(&p, WPE_REPLACE_NEVER, WPE_REMOVE_NOW);
 chk_int("never", wpe_fileop_copy("src.txt", "dst.txt", &p, &o), -1);
 chk_int("declined", o.status, WPE_FILEOP_DECLINED);
 chk_int("by the rule, said", o.step, WPE_FILEOP_STEP_EXISTS);
 wpe_fileop_sentence(buf, sizeof buf, &o);
 chk_has("one line", buf, "exists; not overwritten");
 chk_file("untouched", "dst.txt", "OLD\n");
 policy(&p, WPE_REPLACE_ASK, WPE_REMOVE_NOW);
 asked = 0;
 answer = WPE_FILEOP_ANSWER_NO;
 chk_int("asked, no", wpe_fileop_copy("src.txt", "dst.txt", &p, &o), -1);
 chk_int("asked once", asked, 1);
 chk_int("no is silent", wpe_fileop_sentence(buf, sizeof buf, &o), 0);
 chk_file("untouched after no", "dst.txt", "OLD\n");
 answer = WPE_FILEOP_ANSWER_STOP;
 chk_int("asked, stop", wpe_fileop_copy("src.txt", "dst.txt", &p, &o), -1);
 chk_int("stopped", o.step, WPE_FILEOP_STEP_CANCELLED);
 chk_int("stop is silent", wpe_fileop_sentence(buf, sizeof buf, &o), 0);
 answer = WPE_FILEOP_ANSWER_YES;
 chk_int("asked, yes", wpe_fileop_copy("src.txt", "dst.txt", &p, &o), 0);
 chk_file("replaced", "dst.txt", "NEW\n");
 chk_absent("no temporary name", ".dst.txt.xwpe-copy.0.0");
 clean();
}

/* A name that appears between the check and the last step is not replaced. */
static void check_planted(const char *hooks, WpeReplaceRule rule, const char *what)
{
 WpeFileOpPolicy p;
 WpeFileOpOutcome o;
 char link[512], buf[256];
 ssize_t n;

 make_dir();
 chdir(dir);
 put("src.txt", "NEW\n", 0644);
 policy(&p, rule, WPE_REMOVE_NOW);
 answer = WPE_FILEOP_ANSWER_YES;
 setenv("XWPE_TEST_FAIL", hooks, 1);
 chk_int(what, wpe_fileop_copy("src.txt", "dst.txt", &p, &o), -1);
 unsetenv("XWPE_TEST_FAIL");
 path_of(link, sizeof link, "dst.txt");
 n = readlink(link, buf, sizeof buf - 1);
 chk_int("the planted name is still the link", n > 0, 1);
 chk_absent("no file was made through the link", "planted-target");
 chk_file("source intact", "src.txt", "NEW\n");
 chk_int("no temporary left", count_temp(dir), 0);
 clean();
}

static void test_noreplace(void)
{
 check_planted("fileop-plant", WPE_REPLACE_NEVER, "NEVER, renameat2 or link chain");
 check_planted("fileop-plant,fileop-no-renameat2", WPE_REPLACE_NEVER, "NEVER, link chain");
 check_planted("fileop-plant,fileop-no-renameat2,fileop-no-link", WPE_REPLACE_NEVER, "NEVER, lstat chain");
 check_planted("fileop-plant", WPE_REPLACE_ASK, "ASK, the name was absent at the check");
}

/* The old destination is never removed before the rename. */
static void test_rename_failure_keeps_everything(void)
{
 WpeFileOpPolicy p;
 WpeFileOpOutcome o;
 char buf[2048], wb[512];
 int n;

 make_dir();
 chdir(dir);
 put("src.txt", "NEW\n", 0644);
 put("dst.txt", "OLD\n", 0644);
 policy(&p, WPE_REPLACE_ALWAYS, WPE_REMOVE_WASTEBASKET);
 setenv("XWPE_TEST_FAIL", "fileop-rename", 1);
 chk_int("rename fails", wpe_fileop_copy("src.txt", "dst.txt", &p, &o), -1);
 unsetenv("XWPE_TEST_FAIL");
 chk_int("at the rename", o.step, WPE_FILEOP_STEP_RENAME);
 chk_int("dst kept", o.dst_kept, 1);
 chk_file("old destination intact", "dst.txt", "OLD\n");
 path_of(wb, sizeof wb, ".wastebasket/dst.txt");
 chk_int("no entry left in the wastebasket", access(wb, F_OK), -1);
 chk_int("no temporary", count_temp(dir), 0);
 wpe_fileop_sentence(buf, sizeof buf, &o);
 chk_has("sentence", buf, "putting it in place: Input/output error");
 chk_has("and nothing changed", buf, "Nothing was changed");
 n = 0;
 chk_int("replace with the wastebasket works", wpe_fileop_copy("src.txt", "dst.txt", &p, &o), 0);
 chk_file("replaced", "dst.txt", "NEW\n");
 chk_file("the old one kept", ".wastebasket/dst.txt", "OLD\n");
 put("dst.txt", "OLD2\n", 0644);
 chk_int("again", wpe_fileop_copy("src.txt", "dst.txt", &p, &o), 0);
 chk_file("the earlier one is not replaced", ".wastebasket/dst.txt", "OLD\n");
 chk_file("the second is name.1", ".wastebasket/dst.txt.1", "OLD2\n");
 (void)n;
 clean();
}

/* A copy that fails leaves no half file; the close is judged. */
static void test_close_and_names(void)
{
 WpeFileOpPolicy p;
 WpeFileOpOutcome o;
 char buf[2048], longsrc[300], longdst[300];
 int i;

 make_dir();
 chdir(dir);
 put("src.txt", "NEW\n", 0644);
 policy(&p, WPE_REPLACE_NEVER, WPE_REMOVE_NOW);
 setenv("XWPE_TEST_FAIL", "fileop-close", 1);
 chk_int("close fails", wpe_fileop_copy("src.txt", "out.bin", &p, &o), -1);
 unsetenv("XWPE_TEST_FAIL");
 chk_int("at the close", o.step, WPE_FILEOP_STEP_CLOSE);
 chk_absent("no half file", "out.bin");
 chk_int("no temporary", count_temp(dir), 0);
 wpe_fileop_sentence(buf, sizeof buf, &o);
 chk_has("sentence", buf, "closing: Input/output error");
 chk_int("no such folder", wpe_fileop_copy("src.txt", "nodir/out.bin", &p, &o), -1);
 wpe_fileop_sentence(buf, sizeof buf, &o);
 chk_has("creating", buf, "creating the copy: No such file or directory");
 /* a name of 250 bytes: its temporary name would be too long */
 for (i = 0; i < 250; i++) {
  longsrc[i] = 'a';
  longdst[i] = 'b';
 }
 longsrc[250] = longdst[250] = '\0';
 put(longsrc, "long\n", 0644);
 chk_int("copy with a 250 byte name", wpe_fileop_copy(longsrc, longdst, &p, &o), 0);
 chk_file("long name copied", longdst, "long\n");
 chk_int("no temporary", count_temp(dir), 0);
 clean();
}

/* Move: same device, across devices, the source and the sentence. */
static void test_move(void)
{
 WpeFileOpPolicy p;
 WpeFileOpOutcome o;
 char buf[2048], dst[512], link[512], text[64];
 ssize_t n;
 int fd;

 make_dir();
 make_other();
 chdir(dir);
 put("src.txt", "MOVED\n", 0644);
 policy(&p, WPE_REPLACE_NEVER, WPE_REMOVE_NOW);
 chk_int("move on one device", wpe_fileop_move("src.txt", "dst.txt", &p, &o), 0);
 chk_absent("source gone", "src.txt");
 chk_file("moved", "dst.txt", "MOVED\n");
 symlink("x", "l");
 chk_int("move a link", wpe_fileop_move("l", "m", &p, &o), 0);
 path_of(link, sizeof link, "m");
 n = readlink(link, text, sizeof text - 1);
 text[n > 0 ? n : 0] = '\0';
 chk_str("the target text is kept", text, "x");
 if (other[0]) {
  snprintf(dst, sizeof dst, "%s/dst.txt", other);
  put("src2.txt", "ACROSS\n", 0640);
  chk_int("move across devices", wpe_fileop_move("src2.txt", dst, &p, &o), 0);
  chk_absent("source gone after the copy", "src2.txt");
  get(dst, buf, sizeof buf);
  chk_str("copied bytes", buf, "ACROSS\n");
  snprintf(link, sizeof link, "%s/l2", other);
  symlink("y", "l2");
  chk_int("move a link across devices", wpe_fileop_move("l2", link, &p, &o), 0);
  n = readlink(link, text, sizeof text - 1);
  text[n > 0 ? n : 0] = '\0';
  chk_str("same text", text, "y");
  put("src3.txt", "KEEP\n", 0644);
  snprintf(dst, sizeof dst, "%s/dst3.txt", other);
  setenv("XWPE_TEST_FAIL", "fileop-remove-source", 1);
  chk_int("the source cannot be removed", wpe_fileop_move("src3.txt", dst, &p, &o), -1);
  unsetenv("XWPE_TEST_FAIL");
  chk_int("at the removal", o.step, WPE_FILEOP_STEP_REMOVE_SOURCE);
  chk_int("source kept", o.src_kept, 1);
  chk_file("source still there", "src3.txt", "KEEP\n");
  get(dst, buf, sizeof buf);
  chk_str("the destination holds the copy", buf, "KEEP\n");
  wpe_fileop_sentence(buf, sizeof buf, &o);
  chk_has("says where the copy is", buf, "holds the copy");
  chk_has("and that the source stays", buf, "is still in place");
  /* a destination that is a link to a device is replaced, not written through */
  snprintf(dst, sizeof dst, "%s/sink", other);
  symlink("/dev/full", dst);
  put("src4.txt", "THROUGH\n", 0644);
  policy(&p, WPE_REPLACE_ALWAYS, WPE_REMOVE_NOW);
  chk_int("move onto a link to /dev/full", wpe_fileop_move("src4.txt", dst, &p, &o), 0);
  fd = open(dst, O_RDONLY);
  chk_int("now a regular file", fd >= 0, 1);
  if (fd >= 0)
   close(fd);
  get(dst, buf, sizeof buf);
  chk_str("with the data", buf, "THROUGH\n");
 }
 clean();
}

/* Link, with both errors kept. */
static void test_link(void)
{
 WpeFileOpPolicy p;
 WpeFileOpOutcome o;
 struct stat st;
 char link[512];

 make_dir();
 make_other();
 chdir(dir);
 put("src.txt", "L\n", 0644);
 policy(&p, WPE_REPLACE_NEVER, WPE_REMOVE_NOW);
 chk_int("hard link", wpe_fileop_link("src.txt", "hard.txt", &p, &o), 0);
 stat("hard.txt", &st);
 chk_int("two names", st.st_nlink, 2);
 p.hardlink_first = 0;
 chk_int("symbolic link", wpe_fileop_link("src.txt", "sym.txt", &p, &o), 0);
 lstat("sym.txt", &st);
 chk_int("a link", S_ISLNK(st.st_mode), 1);
 chk_int("link onto an existing name", wpe_fileop_link("src.txt", "sym.txt", &p, &o), -1);
 chk_int("declined", o.status, WPE_FILEOP_DECLINED);
 p.hardlink_first = 1;
 if (other[0]) {
  snprintf(link, sizeof link, "%s/x", other);
  chk_int("hard link across devices falls back", wpe_fileop_link("src.txt", link, &p, &o), 0);
  lstat(link, &st);
  chk_int("to a symbolic link", S_ISLNK(st.st_mode), 1);
 }
 clean();
}

/* Remove, the wastebasket and the permissions. */
static void test_remove(void)
{
 WpeFileOpPolicy p;
 WpeFileOpOutcome o;
 char wb[512], slot[512], buf[2048];
 struct stat st;

 make_dir();
 chdir(dir);
 put("a.txt", "A1\n", 0644);
 policy(&p, WPE_REPLACE_NEVER, WPE_REMOVE_WASTEBASKET);
 mkdir(".wastebasket", 0755);
 chk_int("remove into the wastebasket", wpe_fileop_remove("a.txt", &p, &o), 0);
 chk_absent("gone", "a.txt");
 chk_file("in the wastebasket", ".wastebasket/a.txt", "A1\n");
 stat(".wastebasket", &st);
 chk_int("a wastebasket open to others is closed", st.st_mode & 0777, 0700);
 put("a.txt", "A2\n", 0644);
 chk_int("again", wpe_fileop_remove("a.txt", &p, &o), 0);
 chk_file("never over the earlier one", ".wastebasket/a.txt", "A1\n");
 chk_file("the second as .1", ".wastebasket/a.txt.1", "A2\n");
 chk_int("a name in the wastebasket", wpe_fileop_wastebasket(wb, sizeof wb, &o), 0);
 path_of(slot, sizeof slot, ".wastebasket/a.txt");
 chk_int("is in the wastebasket", wpe_fileop_in_wastebasket(slot), 1);
 chk_int("the free slot is .2", wpe_fileop_wastebasket_slot("/x/a.txt", slot, sizeof slot, &o), 0);
 chk_has("slot", slot, "a.txt.2");
 chk_int("remove inside the wastebasket is for good", wpe_fileop_remove(".wastebasket/a.txt", &p, &o), 0);
 chk_absent("gone for good", ".wastebasket/a.txt");
 policy(&p, WPE_REPLACE_NEVER, WPE_REMOVE_ASK);
 put("b.txt", "B\n", 0644);
 answer = WPE_FILEOP_ANSWER_NO;
 chk_int("asked, no", wpe_fileop_remove("b.txt", &p, &o), -1);
 chk_file("kept", "b.txt", "B\n");
 answer = WPE_FILEOP_ANSWER_YES;
 chk_int("asked, yes", wpe_fileop_remove("b.txt", &p, &o), 0);
 chk_absent("removed", "b.txt");
 chk_int("a name that is not there", wpe_fileop_remove("b.txt", &p, &o), -1);
 wpe_fileop_sentence(buf, sizeof buf, &o);
 chk_has("says it", buf, "Cannot remove");
 /* permissions keep the special bits the dialog does not show */
 mkdir("g", 02755);
 chmod("g", 02755);
 chk_int("chmod", wpe_fileop_chmod("g", 0750, &o), 0);
 stat("g", &st);
 chk_int("setgid kept", st.st_mode & 07777, 02750);
 clean();
}

/* ---- folders ---- */

static int progress_calls;
static int progress_stop_at;

static int progress(void *ctx, const char *verb, const char *src, const char *dst)
{
 (void)ctx; (void)verb; (void)src; (void)dst;
 progress_calls++;
 return(progress_stop_at > 0 && progress_calls >= progress_stop_at);
}

static void tree_policy(WpeFileOpPolicy *p, WpeReplaceRule r, WpeRemoveRule m)
{
 policy(p, r, m);
 p->recursive = 1;
 p->progress = progress;
 progress_calls = 0;
 progress_stop_at = 0;
}

static int is_link(const char *name)
{
 struct stat st;

 return(lstat(name, &st) == 0 && S_ISLNK(st.st_mode));
}

static int mode_of(const char *name)
{
 struct stat st;

 return(lstat(name, &st) == 0 ? (int)(st.st_mode & 07777) : -1);
}

static void link_text_is(const char *what, const char *name, const char *want)
{
 char buf[256];
 ssize_t n = readlink(name, buf, sizeof buf - 1);

 if (n < 0) {
  printf("FAIL: %s: %s is not a link\n", what, name);
  fails++;
  return;
 }
 buf[n] = '\0';
 chk_str(what, buf, want);
}

/* A folder with a link to a folder outside it: no walk reaches the outside
   (Remove deleted its files before), the link is a name. */
static void test_tree_never_enters_a_link(void)
{
 WpeFileOpPolicy p;
 WpeFileOpOutcome o;
 char dst[512];

 make_dir();
 make_other();
 chdir(dir);
 mkdir("precious", 0755);
 put("precious/keep.txt", "KEEP\n", 0644);
 mkdir("rd", 0755);
 put("rd/a.txt", "A\n", 0644);
 chk_int("link", symlink("../precious", "rd/lnk"), 0);

 tree_policy(&p, WPE_REPLACE_ASK, WPE_REMOVE_NOW);
 chk_int("copy", wpe_fileop_copy_tree("rd", "cp", &p, &o), 0);
 chk_int("the link is a link", is_link("cp/lnk"), 1);
 link_text_is("with its text", "cp/lnk", "../precious");
 chk_file("a file", "cp/a.txt", "A\n");
 chk_file("outside, copy", "precious/keep.txt", "KEEP\n");
 chk_absent("nothing was made outside", "precious/lnk");

 tree_policy(&p, WPE_REPLACE_ASK, WPE_REMOVE_NOW);
 chk_int("link of the folder", wpe_fileop_link_tree("rd", "lk", &p, &o), 0);
 chk_int("link to the link", is_link("lk/lnk"), 1);
 chk_file("outside, link", "precious/keep.txt", "KEEP\n");

 tree_policy(&p, WPE_REPLACE_ASK, WPE_REMOVE_NOW);
 chk_int("move, same file system", wpe_fileop_move_tree("cp", "mv", &p, &o), 0);
 chk_int("moved as a name", is_link("mv/lnk"), 1);
 chk_absent("source gone", "cp");
 chk_file("outside, move", "precious/keep.txt", "KEEP\n");

 if (other[0]) {
  snprintf(dst, sizeof dst, "%s/rd", other);
  tree_policy(&p, WPE_REPLACE_ASK, WPE_REMOVE_NOW);
  chk_int("move across file systems", wpe_fileop_move_tree("mv", dst, &p, &o), 0);
  chk_absent("source gone, across", "mv");
  snprintf(dst, sizeof dst, "%s/rd/lnk", other);
  chk_int("a link across file systems", is_link(dst), 1);
  link_text_is("its text across", dst, "../precious");
  chk_file("outside, move across", "precious/keep.txt", "KEEP\n");
 }

 tree_policy(&p, WPE_REPLACE_ASK, WPE_REMOVE_NOW);
 chk_int("remove", wpe_fileop_remove_tree("rd", &p, &o), 0);
 chk_absent("the folder is gone", "rd");
 chk_file("outside, remove", "precious/keep.txt", "KEEP\n");
 chk_int("remove of a link inside a link-only folder", wpe_fileop_remove_tree("lk", &p, &o), 0);
 chk_file("outside, remove of the folder of links", "precious/keep.txt", "KEEP\n");
 clean();
}

/* A folder is taken whole: hidden entries, permissions, the existing
   destination merged into and left as it was. */
static void test_tree_whole_and_permissions(void)
{
 WpeFileOpPolicy p;
 WpeFileOpOutcome o;

 make_dir();
 chdir(dir);
 mkdir("sd", 0777);
 chmod("sd", 0777);
 mkdir("sg", 02755);
 chmod("sg", 02755);
 put("sg/.hidden", "H\n", 0600);
 put("sg/a.c", "C\n", 0644);
 put("sg/note.txt", "N\n", 0644);
 mkdir("sg/sub", 0711);
 chmod("sg/sub", 0711);          /* a folder made in a setgid folder is setgid */
 put("sg/sub/deep.txt", "D\n", 0644);

 tree_policy(&p, WPE_REPLACE_ASK, WPE_REMOVE_NOW);
 chk_int("copy", wpe_fileop_copy_tree("sd", "sdcopy", &p, &o), 0);
 chk_int("0777 kept", mode_of("sdcopy"), 0777);
 chk_int("copy 2755", wpe_fileop_copy_tree("sg", "sgcopy", &p, &o), 0);
 chk_int("setgid kept", mode_of("sgcopy"), 02755);
 chk_file("hidden entry", "sgcopy/.hidden", "H\n");
 chk_file("an entry any pattern hides", "sgcopy/note.txt", "N\n");
 chk_file("a subfolder's entry", "sgcopy/sub/deep.txt", "D\n");
 chk_int("subfolder mode", mode_of("sgcopy/sub"), 0711);
 chk_int("the file's own mode", mode_of("sgcopy/.hidden"), 0600);

 /* an existing destination: merged into, its mode and its files kept */
 mkdir("ex", 0700);
 put("ex/mine.txt", "MINE\n", 0644);
 put("ex/a.c", "OLD\n", 0644);
 tree_policy(&p, WPE_REPLACE_NEVER, WPE_REMOVE_NOW);
 chk_int("merge under Don't overwrite", wpe_fileop_copy_tree("sg", "ex", &p, &o), -1);
 chk_int("declined", o.status, WPE_FILEOP_DECLINED);
 chk_int("one left alone", o.skipped, 1);
 chk_int("the rest done", o.done, 3);
 chk_int("of four", o.total, 4);
 chk_file("the old file", "ex/a.c", "OLD\n");
 chk_file("the other's file", "ex/mine.txt", "MINE\n");
 chk_file("merged in", "ex/note.txt", "N\n");
 chk_int("an existing folder keeps its mode", mode_of("ex"), 0700);
 chk_int("the folder made inside gets the source's", mode_of("ex/sub"), 0711);

 /* non recursive: the subfolder stays behind */
 tree_policy(&p, WPE_REPLACE_ASK, WPE_REMOVE_NOW);
 p.recursive = 0;
 chk_int("not recursive", wpe_fileop_copy_tree("sg", "flat", &p, &o), 0);
 chk_absent("no subfolder", "flat/sub");
 chk_file("its files", "flat/note.txt", "N\n");
 chk_int("a folder that is a file", wpe_fileop_copy_tree("sg", "flat/note.txt", &p, &o), -1);
 chk_int("says so", o.step, WPE_FILEOP_STEP_NOT_A_FOLDER);
 clean();
}

/* Move of a folder: one rename on one file system, merged into an
   existing folder, the source removed only when everything moved. */
static void test_tree_move(void)
{
 WpeFileOpPolicy p;
 WpeFileOpOutcome o;
 struct stat a, b;

 make_dir();
 chdir(dir);
 mkdir("m", 0755);
 put("m/f.txt", "F\n", 0644);
 mkdir("m/s", 0755);
 put("m/s/g.txt", "G\n", 0644);
 stat("m", &a);
 tree_policy(&p, WPE_REPLACE_ASK, WPE_REMOVE_NOW);
 chk_int("move", wpe_fileop_move_tree("m", "n", &p, &o), 0);
 stat("n", &b);
 chk_int("one rename keeps the folder", a.st_ino == b.st_ino, 1);
 chk_int("no progress for a rename", progress_calls, 0);

 mkdir("e", 0700);
 put("e/old.txt", "OLD\n", 0644);
 chk_int("merge", wpe_fileop_move_tree("n", "e", &p, &o), 0);
 chk_file("merged", "e/f.txt", "F\n");
 chk_file("merged, deep", "e/s/g.txt", "G\n");
 chk_file("kept", "e/old.txt", "OLD\n");
 chk_absent("source removed when it was all moved", "n");

 /* a declined entry keeps its folder */
 mkdir("k", 0755);
 put("k/x.txt", "NEW\n", 0644);
 put("k/y.txt", "Y\n", 0644);
 mkdir("t", 0755);
 put("t/x.txt", "THERE\n", 0644);
 tree_policy(&p, WPE_REPLACE_NEVER, WPE_REMOVE_NOW);
 chk_int("move under Don't overwrite", wpe_fileop_move_tree("k", "t", &p, &o), -1);
 chk_int("declined", o.status, WPE_FILEOP_DECLINED);
 chk_file("not replaced", "t/x.txt", "THERE\n");
 chk_file("the other moved", "t/y.txt", "Y\n");
 chk_file("the declined one stays in the source", "k/x.txt", "NEW\n");

 /* a folder into itself, onto itself */
 mkdir("q", 0755);
 put("q/f.txt", "F\n", 0644);
 chk_int("into itself", wpe_fileop_copy_tree("q", "q/in", &p, &o), -1);
 chk_int("refused", o.step, WPE_FILEOP_STEP_INSIDE);
 chk_absent("nothing made", "q/in");
 chk_int("onto itself", wpe_fileop_move_tree("q", "q", &p, &o), -1);
 chk_int("the same folder", o.step, WPE_FILEOP_STEP_SAME_FILE);
 clean();
}

/* Ctrl-C between entries: the count is kept, nothing half is left. */
static void test_tree_stop_and_depth(void)
{
 WpeFileOpPolicy p;
 WpeFileOpOutcome o;
 char name[64], buf[1024];
 int i;

 make_dir();
 chdir(dir);
 mkdir("fifty", 0755);
 for (i = 0; i < 50; i++) {
  snprintf(name, sizeof name, "fifty/f%02d.txt", i);
  put(name, "x\n", 0644);
 }
 tree_policy(&p, WPE_REPLACE_ASK, WPE_REMOVE_NOW);
 progress_stop_at = 8;
 chk_int("stopped", wpe_fileop_copy_tree("fifty", "out", &p, &o), -1);
 chk_int("status", o.status, WPE_FILEOP_FAILED);
 chk_int("step", o.step, WPE_FILEOP_STEP_STOPPED);
 chk_int("seven done", o.done, 7);
 chk_int("of fifty", o.total, 50);
 wpe_fileop_sentence(buf, sizeof buf, &o);
 chk_str("the sentence", buf, "Stopped: 7 of 50 entries were copied.");
 chk_file("the seventh", "out/f06.txt", "x\n");
 chk_absent("not the eighth", "out/f07.txt");
 chk_int("no temporary", count_temp("out"), 0);
 tree_policy(&p, WPE_REPLACE_ASK, WPE_REMOVE_NOW);
 chk_int("without a key, all", wpe_fileop_copy_tree("fifty", "all", &p, &o), 0);
 chk_int("fifty calls", progress_calls, 50);
 chk_file("the last", "all/f49.txt", "x\n");
 tree_policy(&p, WPE_REPLACE_ASK, WPE_REMOVE_NOW);
 progress_stop_at = 3;
 chk_int("remove stopped", wpe_fileop_remove_tree("all", &p, &o), -1);
 chk_int("two removed", o.done, 2);
 wpe_fileop_sentence(buf, sizeof buf, &o);
 chk_str("said", buf, "Stopped: 2 of 50 entries were removed.");

 /* one folder too many: refused before anything is made or removed */
 mkdir("tall", 0755);
 chdir("tall");
 for (i = 0; i < WPE_FILEOP_MAX_DEPTH + 1; i++) {
  mkdir("d", 0755);
  chdir("d");
 }
 put("leaf.txt", "L\n", 0644);
 chdir(dir);
 tree_policy(&p, WPE_REPLACE_ASK, WPE_REMOVE_NOW);
 chk_int("too deep to copy", wpe_fileop_copy_tree("tall", "tallcopy", &p, &o), -1);
 chk_int("refused", o.step, WPE_FILEOP_STEP_TOO_DEEP);
 chk_absent("nothing made", "tallcopy");
 chk_int("too deep to remove", wpe_fileop_remove_tree("tall", &p, &o), -1);
 chk_int("refused too", o.step, WPE_FILEOP_STEP_TOO_DEEP);
 chk_int("nothing removed", mode_of("tall") >= 0, 1);
 clean();
}

/* Remove of a folder asks per file under the Remove rule, and a no keeps it. */
static void test_tree_remove_asks(void)
{
 WpeFileOpPolicy p;
 WpeFileOpOutcome o;

 make_dir();
 chdir(dir);
 mkdir("rd", 0755);
 put("rd/a.txt", "A\n", 0644);
 put("rd/.b", "B\n", 0644);
 mkdir("rd/s", 0755);
 put("rd/s/c", "C\n", 0644);
 tree_policy(&p, WPE_REPLACE_NEVER, WPE_REMOVE_ASK);
 answer = WPE_FILEOP_ANSWER_NO;
 asked = 0;
 chk_int("asked, no", wpe_fileop_remove_tree("rd", &p, &o), -1);
 chk_int("declined", o.status, WPE_FILEOP_DECLINED);
 chk_int("asked for each file", asked, 3);
 chk_file("kept", "rd/a.txt", "A\n");
 chk_file("kept, deep", "rd/s/c", "C\n");
 answer = WPE_FILEOP_ANSWER_YES;
 chk_int("asked, yes", wpe_fileop_remove_tree("rd", &p, &o), 0);
 chk_absent("whole, hidden files too", "rd");
 clean();
}

/* A Move or a Link of a link onto the file it points to is the same file;
   a file copied onto a link to a folder goes inside the folder; a name
   written with a slash is a folder or nothing. */
static void test_same_file_by_link_and_folder_link(void)
{
 WpeFileOpPolicy p;
 WpeFileOpOutcome o;
 char buf[1024];

 make_dir();
 chdir(dir);
 put("t.txt", "TARGET\n", 0644);
 chk_int("link", symlink("t.txt", "l.lnk"), 0);
 policy(&p, WPE_REPLACE_ALWAYS, WPE_REMOVE_NOW);
 chk_int("move a link onto its target", wpe_fileop_move("l.lnk", "t.txt", &p, &o), -1);
 chk_int("the same file", o.step, WPE_FILEOP_STEP_SAME_FILE);
 chk_int("link a link onto its target", wpe_fileop_link("l.lnk", "t.txt", &p, &o), -1);
 chk_int("the same file, link", o.step, WPE_FILEOP_STEP_SAME_FILE);
 chk_file("the data is there", "t.txt", "TARGET\n");
 chk_int("the target is no link", is_link("t.txt"), 0);
 chk_int("the link is still there", is_link("l.lnk"), 1);

 mkdir("box", 0755);
 chk_int("link to a folder", symlink("box", "boxlink"), 0);
 put("src.txt", "SRC\n", 0644);
 policy(&p, WPE_REPLACE_NEVER, WPE_REMOVE_NOW);
 chk_int("copy onto a link to a folder", wpe_fileop_copy("src.txt", "boxlink", &p, &o), 0);
 chk_file("inside the folder", "box/src.txt", "SRC\n");
 chk_int("the link is still a link", is_link("boxlink"), 1);
 chk_int("move onto it", wpe_fileop_move("t.txt", "boxlink", &p, &o), 0);
 chk_file("moved inside", "box/t.txt", "TARGET\n");

 put("plain.txt", "P\n", 0644);
 chk_int("onto a name with a slash", wpe_fileop_copy("src.txt", "nodir/", &p, &o), -1);
 chk_absent("no file under that name", "nodir");
 wpe_fileop_sentence(buf, sizeof buf, &o);
 chk_has("says why", buf, "No such file or directory");
 chk_int("onto a file with a slash", wpe_fileop_copy("src.txt", "plain.txt/", &p, &o), -1);
 chk_file("the file is as it was", "plain.txt", "P\n");

 /* the fsync of a move across file systems that fails says where things are */
 memset(&o, 0, sizeof o);
 o.status = WPE_FILEOP_FAILED;
 o.kind = WPE_FILEOP_MOVE;
 o.step = WPE_FILEOP_STEP_CLOSE;
 o.err = EIO;
 snprintf(o.src, sizeof o.src, "%s", "/a/src");
 snprintf(o.dst, sizeof o.dst, "%s", "/b/dst");
 o.src_kept = 1;
 o.dst_kept = 0;
 wpe_fileop_sentence(buf, sizeof buf, &o);
 chk_has("where the copy is", buf, "/b/dst holds the copy; /a/src is still in place.");
 clean();
}

/* ---- a link in the destination, a folder that changes under the walk ---- */

static int outside_found;

static int look_for_outside(const char *path, const struct stat *sb, int type, struct FTW *ftw)
{
 char buf[64];

 (void)sb; (void)ftw;
 if (type == FTW_F && get(path, buf, sizeof buf) == 0 && strstr(buf, "OUTSIDE"))
  outside_found++;
 return(0);
}

/* How many files under @name hold the text only the folder outside holds. */
static int count_outside(const char *name)
{
 char p[512];

 path_of(p, sizeof p, name);
 outside_found = 0;
 nftw(p, look_for_outside, 8, FTW_PHYS);
 return(outside_found);
}

static char swap_entry[512], swap_out[512];
static int swap_at;

/* At the swap_at-th call, the entry being walked becomes a link to
   something outside: someone else changed the source meanwhile.  The
   progress callback runs after an entry was judged and before it is used,
   which is the window a walk by path would lose. */
static int swap_progress(void *ctx, const char *verb, const char *src, const char *dst)
{
 char aside[600];

 (void)ctx; (void)verb; (void)src; (void)dst;
 if (++progress_calls == swap_at) {
  snprintf(aside, sizeof aside, "%s.real", swap_entry);
  if (rename(swap_entry, aside) != 0 || symlink(swap_out, swap_entry) != 0)
   perror("swap");
 }
 return(0);
}

static void swap_plan(WpeFileOpPolicy *p, const char *entry, const char *outside, int at)
{
 snprintf(swap_entry, sizeof swap_entry, "%s/%s", dir, entry);
 snprintf(swap_out, sizeof swap_out, "%s/%s", dir, outside);
 swap_at = at;
 p->progress = swap_progress;
 progress_calls = 0;
}

/* src/sub holds f0..f3 (INSIDE), out holds f0..f3 (OUTSIDE). */
static void swap_fixture(void)
{
 char name[64];
 int i;

 mkdir("out", 0755);
 mkdir("src", 0755);
 mkdir("src/sub", 0755);
 for (i = 0; i < 4; i++) {
  snprintf(name, sizeof name, "src/sub/f%d", i);
  put(name, "INSIDE\n", 0644);
  snprintf(name, sizeof name, "out/f%d", i);
  put(name, "OUTSIDE\n", 0644);
 }
}

/* The four files outside are as they were. */
static void chk_outside_whole(const char *what)
{
 char name[64];
 int i;

 for (i = 0; i < 4; i++) {
  snprintf(name, sizeof name, "out/f%d", i);
  chk_file(what, name, "OUTSIDE\n");
 }
}

/* The four files moved or copied from the folder that was held. */
static void chk_inside_arrived(const char *what, const char *folder)
{
 char name[96];
 int i;

 for (i = 0; i < 4; i++) {
  snprintf(name, sizeof name, "%s/f%d", folder, i);
  chk_file(what, name, "INSIDE\n");
 }
}

/* A link in the destination where the source has a folder is refused, as
   GNU cp does: the walk stops at that entry with its counts and nothing
   behind the link is touched.  A real folder there is merged into. */
static void test_tree_refuses_a_destination_link_where_a_folder_goes(void)
{
 WpeFileOpPolicy p;
 WpeFileOpOutcome o;
 char buf[1024];

 make_dir();
 chdir(dir);
 mkdir("out", 0755);
 mkdir("out/inner", 0755);
 put("out/inner/b.txt", "OUTSIDE\n", 0644);
 mkdir("src", 0755);
 mkdir("src/sub", 0755);
 put("src/sub/b.txt", "NEW\n", 0644);
 put("src/a.txt", "NEW\n", 0644);
 mkdir("dst", 0755);
 chk_int("link sub", symlink("../out/inner", "dst/sub"), 0);
 tree_policy(&p, WPE_REPLACE_ALWAYS, WPE_REMOVE_NOW);
 chk_int("refused", wpe_fileop_copy_tree("src", "dst", &p, &o), -1);
 chk_int("failed", o.status, WPE_FILEOP_FAILED);
 chk_int("not a folder", o.step, WPE_FILEOP_STEP_NOT_A_FOLDER);
 chk_str("names the link", o.bad, "dst/sub");
 wpe_fileop_sentence(buf, sizeof buf, &o);
 chk_has("says it", buf, "dst/sub is not a folder");
 chk_has("with the counts", buf, "1 of 2 entries were copied.");
 chk_file("the file before it went", "dst/a.txt", "NEW\n");
 chk_file("outside, untouched", "out/inner/b.txt", "OUTSIDE\n");
 chk_absent("nothing made outside", "out/sub");
 chk_int("the link is still a link", is_link("dst/sub"), 1);

 /* the control: a real folder there is merged into */
 chk_int("remove the link", unlink("dst/sub"), 0);
 mkdir("dst/sub", 0755);
 tree_policy(&p, WPE_REPLACE_ALWAYS, WPE_REMOVE_NOW);
 chk_int("merged", wpe_fileop_copy_tree("src", "dst", &p, &o), 0);
 chk_file("merged in", "dst/sub/b.txt", "NEW\n");
 chk_file("outside, still", "out/inner/b.txt", "OUTSIDE\n");
 clean();
}

/* A file onto a link to a folder inside the destination is a name under the
   replace rule: never written through, never put inside the folder (the
   name the user typed is the only one that means "inside"). */
static void test_tree_file_onto_a_destination_link_is_replaced_as_a_name(void)
{
 WpeFileOpPolicy p;
 WpeFileOpOutcome o;
 struct stat st;

 make_dir();
 chdir(dir);
 mkdir("out", 0755);
 put("out/a.txt", "OUTSIDE\n", 0644);
 mkdir("src", 0755);
 put("src/a.txt", "NEW\n", 0644);
 mkdir("dst", 0755);

 /* ALWAYS: the link goes, a file takes its name */
 chk_int("link", symlink("../out", "dst/a.txt"), 0);
 tree_policy(&p, WPE_REPLACE_ALWAYS, WPE_REMOVE_NOW);
 chk_int("replaced", wpe_fileop_copy_tree("src", "dst", &p, &o), 0);
 chk_int("a file now", lstat("dst/a.txt", &st) == 0 && S_ISREG(st.st_mode), 1);
 chk_file("with the new text", "dst/a.txt", "NEW\n");
 chk_file("outside, untouched", "out/a.txt", "OUTSIDE\n");

 /* NEVER: the entry is left, counted, the link stays */
 chk_int("remove the file", unlink("dst/a.txt"), 0);
 chk_int("link again", symlink("../out", "dst/a.txt"), 0);
 tree_policy(&p, WPE_REPLACE_NEVER, WPE_REMOVE_NOW);
 chk_int("declined", wpe_fileop_copy_tree("src", "dst", &p, &o), -1);
 chk_int("status", o.status, WPE_FILEOP_DECLINED);
 chk_int("one left alone", o.skipped, 1);
 chk_int("the link stays", is_link("dst/a.txt"), 1);
 chk_file("outside, untouched, never", "out/a.txt", "OUTSIDE\n");

 /* ASK, answered no: the link stays */
 tree_policy(&p, WPE_REPLACE_ASK, WPE_REMOVE_NOW);
 answer = WPE_FILEOP_ANSWER_NO;
 asked = 0;
 chk_int("asked, no", wpe_fileop_copy_tree("src", "dst", &p, &o), -1);
 chk_int("it asked", asked, 1);
 chk_int("the link stays, asked", is_link("dst/a.txt"), 1);
 chk_file("outside, untouched, asked", "out/a.txt", "OUTSIDE\n");

 /* with the wastebasket: the link goes there as a link */
 tree_policy(&p, WPE_REPLACE_ALWAYS, WPE_REMOVE_WASTEBASKET);
 chk_int("replaced, wastebasket", wpe_fileop_copy_tree("src", "dst", &p, &o), 0);
 chk_int("the link is in the wastebasket as a link", is_link(".wastebasket/a.txt"), 1);
 chk_int("a file took its name", is_link("dst/a.txt"), 0);
 chk_file("outside, untouched, wastebasket", "out/a.txt", "OUTSIDE\n");

 /* the control: a regular file there is replaced as before */
 chk_int("replace a file", wpe_fileop_copy_tree("src", "dst", &p, &o), 0);
 chk_file("replaced", "dst/a.txt", "NEW\n");
 clean();
}

/* A link to a file inside the destination is a name too: replaced, never
   written through (cp would write through it). */
static void test_tree_file_onto_a_link_to_a_file_is_replaced_as_a_name(void)
{
 WpeFileOpPolicy p;
 WpeFileOpOutcome o;

 make_dir();
 chdir(dir);
 put("ofile.txt", "OUTSIDE\n", 0644);
 mkdir("src", 0755);
 put("src/.hid", "NEW\n", 0644);
 mkdir("dst", 0755);
 chk_int("link", symlink("../ofile.txt", "dst/.hid"), 0);
 tree_policy(&p, WPE_REPLACE_ALWAYS, WPE_REMOVE_NOW);
 chk_int("replaced", wpe_fileop_copy_tree("src", "dst", &p, &o), 0);
 chk_int("the name is a file now", is_link("dst/.hid"), 0);
 chk_file("with the new text", "dst/.hid", "NEW\n");
 chk_file("the file the link named is intact", "ofile.txt", "OUTSIDE\n");
 clean();
}

/* A Copy keeps reading the folder it opened, whatever its name becomes. */
static void test_tree_copy_survives_a_swap_for_a_link(void)
{
 WpeFileOpPolicy p;
 WpeFileOpOutcome o;

 make_dir();
 chdir(dir);
 swap_fixture();
 tree_policy(&p, WPE_REPLACE_ALWAYS, WPE_REMOVE_NOW);
 swap_plan(&p, "src/sub", "out", 2);
 chk_int("copied", wpe_fileop_copy_tree("src", "dst", &p, &o), 0);
 chk_int("done", o.status, WPE_FILEOP_DONE);
 chk_int("the swap happened", is_link("src/sub"), 1);
 chk_inside_arrived("what was held arrived", "dst/sub");
 chk_int("nothing from outside in the copy", count_outside("dst"), 0);
 chk_outside_whole("outside, untouched");
 clean();

 /* the control: no swap, four copied */
 make_dir();
 chdir(dir);
 swap_fixture();
 tree_policy(&p, WPE_REPLACE_ALWAYS, WPE_REMOVE_NOW);
 chk_int("no swap", wpe_fileop_copy_tree("src", "dst", &p, &o), 0);
 chk_int("four copied", o.done, 4);
 clean();
}

/* A Move into a folder that exists walks the subfolder by its descriptor:
   the files move from the folder that was held, and the swapped name is
   neither entered nor removed. */
static void test_tree_move_survives_a_swap_for_a_link(void)
{
 WpeFileOpPolicy p;
 WpeFileOpOutcome o;
 char buf[1024];

 make_dir();
 chdir(dir);
 swap_fixture();
 mkdir("dst", 0755);
 mkdir("dst/sub", 0755);          /* a merge: the subfolder is walked, not renamed */
 tree_policy(&p, WPE_REPLACE_ALWAYS, WPE_REMOVE_NOW);
 swap_plan(&p, "src/sub", "out", 2);
 chk_int("moved", wpe_fileop_move_tree("src", "dst", &p, &o), -1);
 chk_int("failed", o.status, WPE_FILEOP_FAILED);
 chk_int("the folder changed", o.step, WPE_FILEOP_STEP_CHANGED);
 chk_int("four moved first", o.done, 4);
 wpe_fileop_sentence(buf, sizeof buf, &o);
 chk_has("says it", buf, "changed while the folder was being moved");
 chk_outside_whole("outside, untouched");
 chk_inside_arrived("what was held moved", "dst/sub");
 chk_int("the swapped name is still a link", is_link("src/sub"), 1);
 chk_int("nothing from outside in the destination", count_outside("dst"), 0);
 clean();

 /* the control: no swap, moved, the source gone */
 make_dir();
 chdir(dir);
 swap_fixture();
 mkdir("dst", 0755);
 mkdir("dst/sub", 0755);
 tree_policy(&p, WPE_REPLACE_ALWAYS, WPE_REMOVE_NOW);
 chk_int("no swap", wpe_fileop_move_tree("src", "dst", &p, &o), 0);
 chk_absent("source gone", "src");
 clean();
}

/* The same across file systems: copied durably from the descriptor, the
   source entry unlinked through the folder it was read from. */
static void test_tree_move_across_survives_a_swap_for_a_link(void)
{
 WpeFileOpPolicy p;
 WpeFileOpOutcome o;
 char dst[512], sub[512], name[600];
 int i;

 make_dir();
 make_other();
 if (!other[0]) {
  clean();
  return;
 }
 chdir(dir);
 swap_fixture();
 snprintf(dst, sizeof dst, "%s/dst", other);
 snprintf(sub, sizeof sub, "%s/dst/sub", other);
 mkdir(dst, 0755);
 mkdir(sub, 0755);
 tree_policy(&p, WPE_REPLACE_ALWAYS, WPE_REMOVE_NOW);
 swap_plan(&p, "src/sub", "out", 2);
 chk_int("moved across", wpe_fileop_move_tree("src", dst, &p, &o), -1);
 chk_int("the folder changed", o.step, WPE_FILEOP_STEP_CHANGED);
 chk_outside_whole("outside, untouched, across");
 for (i = 0; i < 4; i++) {
  snprintf(name, sizeof name, "%s/f%d", sub, i);
  chk_int("what was held arrived", access(name, F_OK), 0);
 }
 outside_found = 0;
 nftw(dst, look_for_outside, 8, FTW_PHYS);
 chk_int("nothing from outside over there", outside_found, 0);
 clean();
}

enum { OP_COPY, OP_MOVE, OP_MOVE_ACROSS, OP_LINK, OP_REMOVE };

static int run_tree_op(int op, WpeFileOpPolicy *p, WpeFileOpOutcome *o)
{
 char dst[512];

 if (op == OP_COPY)
  return(wpe_fileop_copy_tree("src", "dst", p, o));
 if (op == OP_LINK)
  return(wpe_fileop_link_tree("src", "dst", p, o));
 if (op == OP_REMOVE)
  return(wpe_fileop_remove_tree("src", p, o));
 if (op == OP_MOVE_ACROSS) {
  snprintf(dst, sizeof dst, "%s/dst", other);
  return(wpe_fileop_move_tree("src", dst, p, o));
 }
 return(wpe_fileop_move_tree("src", "dst", p, o));
}

/* The destination of a swap row: an existing folder holding `sub`, so a
   Move merges and walks it, on this file system or on the other. */
static void make_swap_destination(int op)
{
 char sub[512];

 if (op == OP_MOVE_ACROSS) {
  snprintf(sub, sizeof sub, "%s/dst", other);
  mkdir(sub, 0755);
  snprintf(sub, sizeof sub, "%s/dst/sub", other);
  mkdir(sub, 0755);
  return;
 }
 mkdir("dst", 0755);
 mkdir("dst/sub", 0755);
}

/* Nothing of the folder outside reached the destination of a swap row. */
static void chk_nothing_outside_in_destination(int op)
{
 char dst[512];

 snprintf(dst, sizeof dst, "%s/dst", other);
 outside_found = 0;
 if (op == OP_MOVE_ACROSS)
  nftw(dst, look_for_outside, 8, FTW_PHYS);
 chk_int("nothing from outside over there", outside_found, 0);
 chk_int("nothing from outside in the destination", count_outside("dst"), 0);
}

/* The subfolder becomes a link after it was judged to be a folder and
   before it is opened: the open does not follow the link, so the walk says
   the source changed and nothing outside is read, moved, linked or removed. */
static void test_tree_swapped_before_the_folder_is_opened(void)
{
 WpeFileOpPolicy p;
 WpeFileOpOutcome o;
 int op;

 for (op = OP_COPY; op <= OP_REMOVE; op++) {
  make_dir();
  make_other();
  if (op == OP_MOVE_ACROSS && !other[0]) {
   clean();
   continue;
  }
  chdir(dir);
  swap_fixture();
  make_swap_destination(op);
  tree_policy(&p, WPE_REPLACE_ALWAYS, WPE_REMOVE_NOW);
  swap_plan(&p, "src/sub", "out", 1);
  chk_int("stopped", run_tree_op(op, &p, &o), -1);
  chk_int("the folder changed", o.step, WPE_FILEOP_STEP_CHANGED);
  chk_outside_whole("outside, untouched, before the open");
  chk_int("the swapped name is a link", is_link("src/sub"), 1);
  chk_nothing_outside_in_destination(op);
  clean();
 }
}

/* A Link of a tree never makes a link to what a swapped name points to, hard
   or symbolic, and a link inside is linked as the link it is. */
static void link_swap_row(int hard)
{
 WpeFileOpPolicy p;
 WpeFileOpOutcome o;
 struct stat st;

 make_dir();
 chdir(dir);
 swap_fixture();
 chk_int("a link inside", symlink("../../out/f0", "src/sub/l"), 0);
 tree_policy(&p, WPE_REPLACE_ALWAYS, WPE_REMOVE_NOW);
 p.hardlink_first = hard;
 swap_plan(&p, "src/sub", "out", 2);
 chk_int("linked", wpe_fileop_link_tree("src", "lk", &p, &o), 0);
 chk_outside_whole("outside, untouched, link");
 chk_int("outside has no more names", lstat("out/f0", &st) == 0 && st.st_nlink == 1, 1);
 chk_int("the link inside is a link, not a name of what it points to", is_link("lk/sub/l"), 1);
 clean();
}

static void test_tree_link_survives_a_swap_for_a_link(void)
{
 link_swap_row(1);
 link_swap_row(0);
}

/* A Remove keeps removing the folder it opened; the swapped name stays. */
static void test_tree_remove_survives_a_swap_for_a_link(void)
{
 WpeFileOpPolicy p;
 WpeFileOpOutcome o;

 make_dir();
 chdir(dir);
 swap_fixture();
 tree_policy(&p, WPE_REPLACE_ALWAYS, WPE_REMOVE_NOW);
 swap_plan(&p, "src/sub", "out", 2);
 chk_int("removed", wpe_fileop_remove_tree("src", &p, &o), -1);
 chk_int("the folder changed", o.step, WPE_FILEOP_STEP_CHANGED);
 chk_outside_whole("outside, untouched, remove");
 chk_int("the swapped name stays", is_link("src/sub"), 1);
 clean();
}

/* An entry that becomes a link between being judged and being used is not
   read through: the walk stops, says so, and the copy holds nothing from
   outside. */
static void test_tree_copy_says_an_entry_changed(void)
{
 WpeFileOpPolicy p;
 WpeFileOpOutcome o;
 char buf[1024];

 make_dir();
 chdir(dir);
 swap_fixture();
 tree_policy(&p, WPE_REPLACE_ALWAYS, WPE_REMOVE_NOW);
 swap_plan(&p, "src/sub/f1", "out/f1", 3);
 chk_int("stopped", wpe_fileop_copy_tree("src", "dst", &p, &o), -1);
 chk_int("failed", o.status, WPE_FILEOP_FAILED);
 chk_int("the entry changed", o.step, WPE_FILEOP_STEP_CHANGED);
 wpe_fileop_sentence(buf, sizeof buf, &o);
 chk_has("says it", buf, "changed while the folder was being copied");
 chk_has("with the counts", buf, "1 of 4 entries were copied.");
 chk_outside_whole("outside, untouched");
 chk_absent("the swapped entry was not copied", "dst/sub/f1");
 chk_int("nothing from outside in the copy", count_outside("dst"), 0);
 clean();
}

/* A name written with a slash after a link is not followed: a Remove or a
   Move of `lnk/` refuses it as the folder it is not. */
static void test_tree_a_link_with_a_slash_is_not_followed(void)
{
 WpeFileOpPolicy p;
 WpeFileOpOutcome o;

 make_dir();
 make_other();
 chdir(dir);
 mkdir("out", 0755);
 put("out/keep.txt", "OUTSIDE\n", 0644);
 chk_int("link", symlink("out", "lnk"), 0);
 tree_policy(&p, WPE_REPLACE_ALWAYS, WPE_REMOVE_NOW);
 chk_int("remove lnk/", wpe_fileop_remove_tree("lnk/", &p, &o), -1);
 chk_int("not a folder", o.step, WPE_FILEOP_STEP_NOT_A_FOLDER);
 chk_file("outside, untouched", "out/keep.txt", "OUTSIDE\n");
 chk_int("the link stays", is_link("lnk"), 1);
 chk_int("move lnk/", wpe_fileop_move_tree("lnk/", "mv", &p, &o), -1);
 chk_int("not a folder, move", o.step, WPE_FILEOP_STEP_NOT_A_FOLDER);
 chk_file("outside, untouched, move", "out/keep.txt", "OUTSIDE\n");
 chk_int("the link stays, move", is_link("lnk"), 1);
 chk_absent("nothing made", "mv");
 if (other[0]) {
  char dst[512];

  snprintf(dst, sizeof dst, "%s/mv", other);
  chk_int("move lnk/ across", wpe_fileop_move_tree("lnk/", dst, &p, &o), -1);
  chk_file("outside, untouched, across", "out/keep.txt", "OUTSIDE\n");
  chk_int("the link stays, across", is_link("lnk"), 1);
 }

 /* the control: a real folder with a slash goes as before */
 mkdir("rd", 0755);
 put("rd/x", "X\n", 0644);
 chk_int("remove rd/", wpe_fileop_remove_tree("rd/", &p, &o), 0);
 chk_absent("gone", "rd");
 clean();
}

/* The folder the user names is followed once when it is a link and the
   operation is a Copy or a Link (the DirTree shows it as a folder); the
   links inside it are names.  A Move or a Remove of the link is the
   File Manager's own dispatch (the file operation), never the tree's. */
static void test_tree_a_named_link_to_a_folder_is_followed_once(void)
{
 WpeFileOpPolicy p;
 WpeFileOpOutcome o;

 make_dir();
 chdir(dir);
 mkdir("elsewhere", 0755);
 put("elsewhere/e.txt", "ELSE\n", 0644);
 mkdir("target", 0755);
 put("target/t.txt", "T\n", 0644);
 chk_int("inner link", symlink("../elsewhere", "target/inner"), 0);
 chk_int("the named link", symlink("target", "lnk"), 0);
 tree_policy(&p, WPE_REPLACE_ALWAYS, WPE_REMOVE_NOW);
 chk_int("copy", wpe_fileop_copy_tree("lnk", "cp", &p, &o), 0);
 chk_file("the folder's file", "cp/t.txt", "T\n");
 chk_int("the inner link is a link", is_link("cp/inner"), 1);
 chk_file("elsewhere, untouched", "elsewhere/e.txt", "ELSE\n");
 tree_policy(&p, WPE_REPLACE_ALWAYS, WPE_REMOVE_NOW);
 chk_int("link of it", wpe_fileop_link_tree("lnk", "lk", &p, &o), 0);
 chk_int("the inner link is linked as a link", is_link("lk/inner"), 1);
 chk_file("elsewhere, untouched, link", "elsewhere/e.txt", "ELSE\n");
 tree_policy(&p, WPE_REPLACE_ALWAYS, WPE_REMOVE_NOW);
 chk_int("a Move of the link as a tree", wpe_fileop_move_tree("lnk", "mv", &p, &o), -1);
 chk_int("is not a folder", o.step, WPE_FILEOP_STEP_NOT_A_FOLDER);
 chk_int("the link stays", is_link("lnk"), 1);
 clean();
}

int main(void)
{
 test_tree_refuses_a_destination_link_where_a_folder_goes();
 test_tree_file_onto_a_destination_link_is_replaced_as_a_name();
 test_tree_file_onto_a_link_to_a_file_is_replaced_as_a_name();
 test_tree_copy_survives_a_swap_for_a_link();
 test_tree_move_survives_a_swap_for_a_link();
 test_tree_move_across_survives_a_swap_for_a_link();
 test_tree_swapped_before_the_folder_is_opened();
 test_tree_link_survives_a_swap_for_a_link();
 test_tree_remove_survives_a_swap_for_a_link();
 test_tree_copy_says_an_entry_changed();
 test_tree_a_link_with_a_slash_is_not_followed();
 test_tree_a_named_link_to_a_folder_is_followed_once();
 test_tree_never_enters_a_link();
 test_tree_whole_and_permissions();
 test_tree_move();
 test_tree_stop_and_depth();
 test_tree_remove_asks();
 test_same_file_by_link_and_folder_link();
 test_private_and_special_bits();
 test_create_private();
 test_same_file();
 test_not_a_file();
 test_folder_destination();
 test_policy();
 test_noreplace();
 test_rename_failure_keeps_everything();
 test_close_and_names();
 test_move();
 test_link();
 test_remove();
 if (fails) {
  printf("test_fileop: %d failure(s)\n", fails);
  return(1);
 }
 printf("test_fileop: ok\n");
 return(0);
}
