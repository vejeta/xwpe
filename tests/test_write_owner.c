/* test_write_owner.c -- unit test for the owner of "write the user's text"
 * (we_write.c).  The editor's side (the rows, the boxes, the file names) is
 * played by the stubs below.  Always runs.
 *
 * What the user would see go wrong if a rule broke: a failed save with no
 * word about the path or the reason, a save that reports success on a full
 * disk, the file cut short with nothing to put it back, a symlinked file
 * replaced by a copy, a backup that is readable by others for a moment or
 * that stops half way and is trusted, the editor killed by a file size
 * limit with the text in it, a program started later that inherits an
 * ignored signal.
 */
/* @scenario SC-unsaved-text-19 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE             /* RTLD_NEXT, for the two watched calls below */
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600
#endif
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../edit.h"
#include "../we_write.h"

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

/* ---- the editor's side, as stubs ---- */

void (*WpeMouseChangeShape)(WpeMouseShape s) = NULL;
void (*WpeMouseRestoreShape)(void) = NULL;
int e_emergency_save_running(void) { return(0); }
void e_status_say_until_key(FENSTER *f, const char *t) { (void)f; (void)t; }
int e_message(int sw, char *s, FENSTER *f) { (void)sw; (void)s; (void)f; return('Y'); }
void e_eol_nul_sentence(int count, char *buf, size_t size) { (void)count; snprintf(buf, size, "nul"); }

char *e_mkfilename(char *dr, char *fn)
{
 char *s = malloc(strlen(dr) + strlen(fn) + 2);

 sprintf(s, "%s/%s", dr, fn);
 return(s);
}

/* The turbo-C style name: the extension replaced by .bak. */
char *e_bakfilename(char *s)
{
 char *r = malloc(strlen(s) + 8), *dot;

 strcpy(r, s);
 dot = strrchr(r, '.');
 if (dot && strchr(dot, '/') == NULL)
  *dot = '\0';
 strcat(r, ".bak");
 return(r);
}

/* The text the stub writes in place of the buffer's rows. */
static char *rows_text;
static size_t rows_repeat = 1;

static void set_rows(const char *text)
{
 free(rows_text);
 rows_text = strdup(text);
}

int e_eol_write(FILE *fp, BUFFER *b, int ya, int xa, int ye, int xe, int keep_final)
{
 size_t i;

 (void)b; (void)ya; (void)xa; (void)ye; (void)xe; (void)keep_final;
 for (i = 0; i < rows_repeat; i++)
  if (fputs(rows_text, fp) == EOF)
   return(-1);
 return(0);
}

/* ---- helpers ---- */

static char dir[256];
static char made[32][256];
static int made_n;

static void make_dir(void)
{
 snprintf(dir, sizeof dir, "%s/wo.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
 if (!mkdtemp(dir)) {
  perror("mkdtemp");
  exit(2);
 }
 if (made_n < 32)
  snprintf(made[made_n++], sizeof made[0], "%s", dir);
}

static int remove_entry(const char *path, const struct stat *sb, int type, struct FTW *ftw)
{
 (void)sb; (void)type; (void)ftw;
 remove(path);
 return(0);
}

/* Each folder this test made, and nothing else, is removed at the end. */
static void remove_made_dirs(void)
{
 while (made_n > 0)
  nftw(made[--made_n], remove_entry, 8, FTW_DEPTH | FTW_PHYS);
}

static void path_of(char *out, size_t n, const char *name)
{
 snprintf(out, n, "%s/%s", dir, name);
}

static void put_file(const char *name, const char *text, mode_t mode)
{
 char p[512];
 FILE *fp;

 path_of(p, sizeof p, name);
 unlink(p);
 fp = fopen(p, "w");
 fputs(text, fp);
 fclose(fp);
 chmod(p, mode);
}

static char *get_file(const char *name, char *buf, size_t n)
{
 char p[512];
 FILE *fp;
 size_t got;

 path_of(p, sizeof p, name);
 buf[0] = '\0';
 fp = fopen(p, "r");
 if (!fp)
  return(buf);
 got = fread(buf, 1, n - 1, fp);
 buf[got] = '\0';
 fclose(fp);
 return(buf);
}

static int mode_of(const char *name)
{
 char p[512];
 struct stat st;

 path_of(p, sizeof p, name);
 return(lstat(p, &st) == 0 ? (int)(st.st_mode & 07777) : -1);
}

/* What a test allocates is freed once, at the end. */
static void *garbage[256];
static int garbage_n;

static void *keep(void *p)
{
 if (garbage_n < (int)(sizeof garbage / sizeof garbage[0]))
  garbage[garbage_n++] = p;
 return(p);
}

static void free_garbage(void)
{
 while (garbage_n > 0)
  free(garbage[--garbage_n]);
 free(rows_text);
 rows_text = NULL;
}

static FENSTER *make_window(void)
{
 FENSTER *f = keep(calloc(1, sizeof *f));

 f->ed = keep(calloc(1, sizeof *f->ed));
 f->b = keep(calloc(1, sizeof *f->b));
 f->filemode = -1;
 return(f);
}

static void limit_file_size(rlim_t bytes)
{
 struct rlimit rl;

 rl.rlim_cur = bytes;
 rl.rlim_max = RLIM_INFINITY;
 setrlimit(RLIMIT_FSIZE, &rl);
}

static void no_file_size_limit(void)
{
 limit_file_size(RLIM_INFINITY);
}

static char *big(size_t n, char c)
{
 char *s = keep(malloc(n + 1));

 memset(s, c, n);
 s[n] = '\0';
 return(s);
}

/* ---- the sentence ---- */

static void test_sentence(void)
{
 WpeWriteOutcome o;
 char s[WPE_WRITE_SENTENCE_MAX];

 memset(&o, 0, sizeof o);
 o.status = WPE_WRITE_FAILED;
 o.err = EACCES;
 snprintf(o.path, sizeof o.path, "/w/t.c");
 wpe_write_sentence(s, sizeof s, &o, WPE_WRITE_THEN_KEPT);
 chk_has("failed, kept", s, "Cannot write /w/t.c: Permission denied.");
 chk_has("failed, kept: what stays", s, "  Your text is still in the window.");
 wpe_write_sentence(s, sizeof s, &o, WPE_WRITE_THEN_NO_BUILD);
 chk_has("failed, no build", s, "  The build was not started.");
 chk_lacks("failed, no build: not the window", s, "still in the window");
 wpe_write_sentence(s, sizeof s, &o, WPE_WRITE_THEN_NO_PRINT);
 chk_has("failed, no print", s, "  Nothing was printed.");
 snprintf(o.keep_name, sizeof o.keep_name, "/w/t.c");
 wpe_write_sentence(s, sizeof s, &o, WPE_WRITE_THEN_NAME_KEPT);
 chk_has("failed, name kept", s, "  The window keeps its name t.c.");

 o.read_only = 1;
 wpe_write_sentence(s, sizeof s, &o, WPE_WRITE_THEN_KEPT);
 chk_has("read-only window", s, "t.c is read-only.");
 chk_lacks("read-only window: no reason", s, "Permission");
 o.read_only = 0;

 o.err = EFBIG;
 o.backup_stopped = 1;
 wpe_write_sentence(s, sizeof s, &o, WPE_WRITE_THEN_KEPT);
 chk_has("backup stopped", s, "the backup copy stopped at File too large; the file was not touched.");
 o.backup_stopped = 0;

 o.restore_err = EIO;
 snprintf(o.bak_path, sizeof o.bak_path, "/w/t.bak");
 wpe_write_sentence(s, sizeof s, &o, WPE_WRITE_THEN_KEPT);
 chk_has("restore failed", s, "may be cut short; its previous content is in /w/t.bak.");

 memset(&o, 0, sizeof o);
 o.status = WPE_WRITE_WRITTEN;
 chk_int("written says nothing", wpe_write_sentence(s, sizeof s, &o, WPE_WRITE_THEN_KEPT), 0);
 chk_int("nothing to write says nothing", (long)strlen(s), 0);
 snprintf(o.path, sizeof o.path, "/w/t.c");
 o.backup_err = EACCES;
 wpe_write_sentence(s, sizeof s, &o, WPE_WRITE_THEN_KEPT);
 chk_has("written, no backup", s, "t.c written; no backup was made: Permission denied.");
 o.backup_err = 0;
 o.kept = 1;
 o.err = ENOSPC;
 snprintf(o.kept_path, sizeof o.kept_path, "/r/x.ESV");
 wpe_write_sentence(s, sizeof s, &o, WPE_WRITE_THEN_KEPT);
 chk_has("written, kept elsewhere", s, "could not go beside the file (No space left on device); it is in /r/x.ESV.");

 memset(&o, 0, sizeof o);
 o.status = WPE_WRITE_DECLINED;
 chk_int("declined says nothing", wpe_write_sentence(s, sizeof s, &o, WPE_WRITE_THEN_KEPT), 0);

 memset(&o, 0, sizeof o);
 o.status = WPE_WRITE_FAILED;
 o.err = ENOSPC;
 wpe_write_sentence(s, sizeof s, &o, WPE_WRITE_THEN_KEPT);
 chk_has("no path", s, "Cannot write the file: No space left on device.");
}

/* ---- text ---- */

static void test_text(void)
{
 WpeWriteOutcome o;
 char buf[256], p[512];
 struct sigaction before, after;

 make_dir();
 path_of(p, sizeof p, "plain.txt");
 chk_int("text: written", wpe_write_text(p, "hello\n", &o), 0);
 chk_int("text: status", o.status, WPE_WRITE_WRITTEN);
 get_file("plain.txt", buf, sizeof buf);
 chk_has("text: content", buf, "hello\n");

 chk_int("text onto /dev/full fails", wpe_write_text("/dev/full", "x", &o), -1);
 chk_int("text onto /dev/full: status", o.status, WPE_WRITE_FAILED);
 chk_int("text onto /dev/full: ENOSPC", o.err, ENOSPC);

 path_of(p, sizeof p, "no/such/dir/x");
 chk_int("text in a missing folder fails", wpe_write_text(p, "x", &o), -1);
 chk_int("text in a missing folder: ENOENT", o.err, ENOENT);
 chk_has("text in a missing folder: path", o.path, "no/such/dir/x");

 /* the size limit is an outcome, not the end of the process, and the
    action for SIGXFSZ is as it was afterwards */
 sigaction(SIGXFSZ, NULL, &before);
 limit_file_size(1024);
 {
  char *text = big(4096, 'q');

  path_of(p, sizeof p, "limited.txt");
  chk_int("text past the limit fails", wpe_write_text(p, text, &o), -1);
  chk_int("text past the limit: EFBIG", o.err, EFBIG);
 }
 no_file_size_limit();
 sigaction(SIGXFSZ, NULL, &after);
 chk_int("SIGXFSZ put back", after.sa_handler == before.sa_handler, 1);
 chk_int("SIGXFSZ not ignored afterwards", after.sa_handler == SIG_IGN, 0);
}

/* ---- the user's file and its backup ---- */

static void test_backup_made_private_then_file_mode(void)
{
 WpeWriteOutcome o;
 FENSTER *f = make_window();
 char p[512], buf[256];

 make_dir();
 put_file("t.c", "old\n", 0640);
 set_rows("new\n");
 path_of(p, sizeof p, "t.c");
 chk_int("write", wpe_write_range(f, p, 0, 0, 1, 1, 1, &o), 0);
 chk_has("file written", get_file("t.c", buf, sizeof buf), "new\n");
 chk_has("backup holds the old text", get_file("t.bak", buf, sizeof buf), "old\n");
 chk_int("backup has the file's mode", mode_of("t.bak"), 0640);
 chk_int("no note when the backup was made", o.backup_err, 0);
}

static void test_backup_not_made_in_a_folder_that_refuses(void)
{
 WpeWriteOutcome o;
 FENSTER *f = make_window();
 char p[512], buf[256], s[WPE_WRITE_SENTENCE_MAX];

 make_dir();
 put_file("t.c", "old\n", 0644);
 chmod(dir, 0500);
 set_rows("new\n");
 path_of(p, sizeof p, "t.c");
 chk_int("write goes on without a backup", wpe_write_range(f, p, 0, 0, 1, 1, 1, &o), 0);
 chmod(dir, 0700);
 chk_has("file written", get_file("t.c", buf, sizeof buf), "new\n");
 chk_int("backup_err is the reason", o.backup_err, EACCES);
 wpe_write_sentence(s, sizeof s, &o, WPE_WRITE_THEN_KEPT);
 chk_has("the sentence", s, "no backup was made: Permission denied.");
}

static void test_old_bak_that_is_a_link_is_left_alone(void)
{
 WpeWriteOutcome o;
 FENSTER *f = make_window();
 char p[512], buf[256], q[512];

 make_dir();
 put_file("t.c", "old\n", 0644);
 put_file("victim", "keep me\n", 0644);
 path_of(q, sizeof q, "victim");
 {
  char l[512];

  path_of(l, sizeof l, "t.bak");
  if (symlink(q, l) != 0)
   perror("symlink");
 }
 set_rows("new\n");
 path_of(p, sizeof p, "t.c");
 chk_int("write goes on", wpe_write_range(f, p, 0, 0, 1, 1, 1, &o), 0);
 chk_int("backup refused: EEXIST", o.backup_err, EEXIST);
 chk_has("the link's target is untouched", get_file("victim", buf, sizeof buf), "keep me\n");
}

/* a write that fails after the file was opened: the file as it was */
static void write_past_limit(FENSTER *f, const char *name, WpeWriteOutcome *o, size_t limit,
                             size_t write_bytes)
{
 char p[512], *text = big(write_bytes, 'n');

 set_rows(text);
 path_of(p, sizeof p, name);
 limit_file_size(limit);
 chk_int("the write fails", wpe_write_range(f, p, 0, 0, 1, 1, 1, o), -1);
 no_file_size_limit();
}

static void test_restore_by_rename(void)
{
 WpeWriteOutcome o;
 FENSTER *f = make_window();
 char *old = big(1024, 'o'), buf[8192];

 make_dir();
 put_file("t.c", old, 0644);
 write_past_limit(f, "t.c", &o, 2048, 4096);
 chk_int("status", o.status, WPE_WRITE_FAILED);
 chk_int("err is EFBIG", o.err, EFBIG);
 chk_int("restored", o.restore_err, 0);
 chk_int("the file is as it was", strcmp(get_file("t.c", buf, sizeof buf), old), 0);
 chk_int("the file keeps its mode", mode_of("t.c"), 0644);
}

static void test_restore_through_a_symlink(void)
{
 WpeWriteOutcome o;
 FENSTER *f = make_window();
 char *old = big(1024, 'o'), buf[8192], real[512], link[512];

 make_dir();
 put_file("real.c", old, 0644);
 path_of(real, sizeof real, "real.c");
 path_of(link, sizeof link, "t.c");
 if (symlink(real, link) != 0)
  perror("symlink");
 write_past_limit(f, "t.c", &o, 2048, 4096);
 chk_int("restored", o.restore_err, 0);
 {
  struct stat st;

  chk_int("lstat", lstat(link, &st), 0);
  chk_int("t.c is still a symlink", S_ISLNK(st.st_mode) != 0, 1);
 }
 chk_int("the target is as it was", strcmp(get_file("real.c", buf, sizeof buf), old), 0);
}

static void test_restore_keeps_a_hard_link(void)
{
 WpeWriteOutcome o;
 FENSTER *f = make_window();
 char *old = big(1024, 'o'), buf[8192], a[512], b[512];

 make_dir();
 put_file("t.c", old, 0644);
 path_of(a, sizeof a, "t.c");
 path_of(b, sizeof b, "other");
 if (link(a, b) != 0)
  perror("link");
 write_past_limit(f, "t.c", &o, 2048, 4096);
 chk_int("restored", o.restore_err, 0);
 chk_int("t.c as it was", strcmp(get_file("t.c", buf, sizeof buf), old), 0);
 chk_int("the other name as it was", strcmp(get_file("other", buf, sizeof buf), old), 0);
 {
  struct stat st;

  chk_int("still two names on one inode", stat(a, &st) == 0 && st.st_nlink == 2, 1);
 }
}

static void test_a_restore_that_fails_is_said(void)
{
 WpeWriteOutcome o;
 FENSTER *f = make_window();
 char *old = big(1024, 'o'), buf[8192], s[WPE_WRITE_SENTENCE_MAX];

 make_dir();
 put_file("t.c", old, 0644);
 setenv("XWPE_TEST_FAIL", "restore", 1);
 write_past_limit(f, "t.c", &o, 2048, 4096);
 unsetenv("XWPE_TEST_FAIL");
 chk_int("restore_err is EIO", o.restore_err, EIO);
 chk_int("the .bak holds the old bytes", strcmp(get_file("t.bak", buf, sizeof buf), old), 0);
 wpe_write_sentence(s, sizeof s, &o, WPE_WRITE_THEN_KEPT);
 chk_has("the sentence", s, "may be cut short");
 chk_has("the sentence names the .bak", s, "t.bak");
}

static void test_a_backup_that_stops_refuses_the_save(void)
{
 WpeWriteOutcome o;
 FENSTER *f = make_window();
 char *old = big(4096, 'o'), buf[8192], s[WPE_WRITE_SENTENCE_MAX], p[512];

 make_dir();
 put_file("t.c", old, 0644);
 set_rows("X");
 path_of(p, sizeof p, "t.c");
 limit_file_size(1024);
 chk_int("the save is refused", wpe_write_range(f, p, 0, 0, 1, 1, 1, &o), -1);
 no_file_size_limit();
 chk_int("backup_stopped", o.backup_stopped, 1);
 chk_int("err is EFBIG", o.err, EFBIG);
 chk_int("the file was not touched", strcmp(get_file("t.c", buf, sizeof buf), old), 0);
 chk_int("no short .bak is left", mode_of("t.bak"), -1);
 wpe_write_sentence(s, sizeof s, &o, WPE_WRITE_THEN_KEPT);
 chk_has("the sentence", s, "the backup copy stopped at File too large; the file was not touched.");
}

static void test_a_read_only_window_is_failed_and_untouched(void)
{
 WpeWriteOutcome o;
 FENSTER *f = make_window();
 char p[512], buf[64];

 make_dir();
 put_file("t.c", "old\n", 0644);
 f->ins = 8;
 set_rows("new\n");
 path_of(p, sizeof p, "t.c");
 chk_int("refused", wpe_write_range(f, p, 0, 0, 1, 1, 1, &o), -1);
 chk_int("read_only", o.read_only, 1);
 chk_has("untouched", get_file("t.c", buf, sizeof buf), "old\n");
}

/* ---- two calls of the owner watched: write(2) and unlink(2) ----
 * The owner calls write(2) only for the backup copy, the restore and the
 * text writers, and unlink(2) for the old .bak.  Defined here, they pass
 * through to the C library and let a row see the .bak's mode at the moment
 * it receives the text, or put something where the old .bak was. */

typedef ssize_t (*write_fn)(int, const void *, size_t);
typedef int (*unlink_fn)(const char *);

static int watch_writes;          /* look at the mode of each file written */
static int write_shared_bits;     /* group/other bits seen on such a file */
static char plant_at[512];        /* after this name is unlinked ... */
static char plant_from[512];      /* ... a hard link to this file takes it */

static write_fn real_write(void)
{
 static write_fn fn;

 if (fn == NULL)
  *(void **)&fn = dlsym(RTLD_NEXT, "write");
 return(fn);
}

static unlink_fn real_unlink(void)
{
 static unlink_fn fn;

 if (fn == NULL)
  *(void **)&fn = dlsym(RTLD_NEXT, "unlink");
 return(fn);
}

ssize_t write(int fd, const void *buf, size_t n)
{
 struct stat st;

 if (watch_writes && fstat(fd, &st) == 0 && S_ISREG(st.st_mode))
  write_shared_bits |= (int)(st.st_mode & 077);
 return(real_write()(fd, buf, n));
}

int unlink(const char *path)
{
 int r = real_unlink()(path);

 if (r == 0 && plant_at[0] != '\0' && strcmp(path, plant_at) == 0)
 {
  plant_at[0] = '\0';
  if (link(plant_from, path) != 0)
   perror("link");
 }
 return(r);
}

/* A 0600 file's backup never holds its text while others may read it, under
   any umask: the mode is narrowed before the first byte, not after the last. */
static void test_the_backup_is_private_while_it_holds_text(void)
{
 static const mode_t masks[] = { 022, 002, 0 };
 WpeWriteOutcome o;
 FENSTER *f = make_window();
 char p[512];
 mode_t was;
 size_t i;

 for (i = 0; i < sizeof masks / sizeof masks[0]; i++)
 {
  make_dir();
  put_file("t.c", "secret\n", 0600);
  set_rows("new\n");
  path_of(p, sizeof p, "t.c");
  was = umask(masks[i]);
  write_shared_bits = 0;
  watch_writes = 1;
  chk_int("write", wpe_write_range(f, p, 0, 0, 1, 1, 1, &o), 0);
  watch_writes = 0;
  umask(was);
  chk_int("the .bak was private while it held the text", write_shared_bits, 0);
  chk_int("the .bak ends with the file's mode", mode_of("t.bak"), 0600);
 }
}

/* A name that takes the old .bak's place between its removal and the new
   one's creation (here a second name of another file) is not written into:
   the .bak is a new file of the owner's or none, and the save goes on. */
static void test_a_name_planted_where_the_old_backup_was_is_not_written(void)
{
 WpeWriteOutcome o;
 FENSTER *f = make_window();
 char p[512], buf[256];

 make_dir();
 put_file("t.c", "old\n", 0644);
 put_file("t.bak", "stale\n", 0644);
 put_file("victim", "keep me\n", 0644);
 path_of(plant_at, sizeof plant_at, "t.bak");
 path_of(plant_from, sizeof plant_from, "victim");
 set_rows("new\n");
 path_of(p, sizeof p, "t.c");
 chk_int("write goes on", wpe_write_range(f, p, 0, 0, 1, 1, 1, &o), 0);
 plant_at[0] = '\0';
 chk_int("backup refused: EEXIST", o.backup_err, EEXIST);
 chk_has("the planted name's file is untouched", get_file("victim", buf, sizeof buf),
         "keep me\n");
 chk_has("file written", get_file("t.c", buf, sizeof buf), "new\n");
}

/* A write that fails after the file was opened, with no .bak to put back
   (the file can be written but not read): the sentence must not let the user
   think the file on disk is as it was. */
static void test_a_failure_with_no_backup_says_the_file_may_be_cut_short(void)
{
 WpeWriteOutcome o;
 FENSTER *f = make_window();
 char s[WPE_WRITE_SENTENCE_MAX];

 make_dir();
 put_file("t.c", "old\n", 0200);
 write_past_limit(f, "t.c", &o, 2048, 4096);
 chk_int("no backup was made", o.bak_path[0], '\0');
 wpe_write_sentence(s, sizeof s, &o, WPE_WRITE_THEN_KEPT);
 chk_has("the sentence", s, "may be cut short");
 chk_lacks("no backup is named", s, ".bak");
}

/* The restore by rename leaves the file's group as it was: a group that could
   read the file still can.  Needs a second group of the user's; skipped
   without one. */
static void test_a_restore_keeps_the_files_group(void)
{
 WpeWriteOutcome o;
 FENSTER *f = make_window();
 char *old = big(1024, 'o'), p[512];
 gid_t groups[64], other = (gid_t)-1;
 struct stat st;
 int n, i;

 n = getgroups((int)(sizeof groups / sizeof groups[0]), groups);
 for (i = 0; i < n && other == (gid_t)-1; i++)
  if (groups[i] != getegid())
   other = groups[i];
 if (other == (gid_t)-1)
 {
  printf("SKIP: a restore keeps the file's group: no second group\n");
  return;
 }
 make_dir();
 put_file("t.c", old, 0640);
 path_of(p, sizeof p, "t.c");
 if (chown(p, (uid_t)-1, other) != 0)
 {
  printf("SKIP: a restore keeps the file's group: chown refused\n");
  return;
 }
 write_past_limit(f, "t.c", &o, 2048, 4096);
 chk_int("restored", o.restore_err, 0);
 chk_int("stat", stat(p, &st), 0);
 chk_int("the file keeps its group", (long)st.st_gid, (long)other);
 chk_int("the file keeps its mode", mode_of("t.c"), 0640);
}

int main(void)
{
 test_sentence();
 test_text();
 test_backup_made_private_then_file_mode();
 test_backup_not_made_in_a_folder_that_refuses();
 test_old_bak_that_is_a_link_is_left_alone();
 test_restore_by_rename();
 test_restore_through_a_symlink();
 test_restore_keeps_a_hard_link();
 test_a_restore_that_fails_is_said();
 test_a_backup_that_stops_refuses_the_save();
 test_a_read_only_window_is_failed_and_untouched();
 test_the_backup_is_private_while_it_holds_text();
 test_a_name_planted_where_the_old_backup_was_is_not_written();
 test_a_failure_with_no_backup_says_the_file_may_be_cut_short();
 test_a_restore_keeps_the_files_group();
 free_garbage();
 remove_made_dirs();
 if (fails) {
  printf("test_write_owner: %d failure(s)\n", fails);
  return(1);
 }
 printf("test_write_owner: ok\n");
 return(0);
}
