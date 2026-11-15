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

int main(void)
{
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
