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
#include <sys/time.h>
#include <unistd.h>

#include "../edit.h"
#include "../we_write.h"
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
 if (test_dir_make(dir, sizeof dir, "wo") != 0 && dir[0] == '\0')
  exit(2);
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

/* A file of the test folder with this text and exactly this mode; a setup
   that cannot be made says which step failed and counts as a failure. */
static void put_file(const char *name, const char *text, mode_t mode)
{
 test_put_mode(dir, name, text, mode);
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

/* Writing onto a device that is always full ends as a failure with ENOSPC.
   Only Linux and the systems that copy it have /dev/full; where it is not a
   character device (OpenBSD) the row is skipped, the assertion is the same. */
static void test_text_onto_full_device(WpeWriteOutcome *o)
{
 struct stat st;

 if (stat("/dev/full", &st) != 0 || !S_ISCHR(st.st_mode))
 {
  printf("SKIP: text onto /dev/full: this system has no /dev/full device\n");
  return;
 }
 chk_int("text onto /dev/full fails", wpe_write_text("/dev/full", "x", o), -1);
 chk_int("text onto /dev/full: status", o->status, WPE_WRITE_FAILED);
 chk_int("text onto /dev/full: ENOSPC", o->err, ENOSPC);
}

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

 test_text_onto_full_device(&o);

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

/* ---- the project's and the options' follow-up sentences ---- */

/* A name longer than the system holds is refused by the system on the write;
   the sentence the user reads is the system's reason plus what the caller
   keeps.  On Linux the name is over NAME_MAX, the same reason macOS gives for
   a path over its limit. */
static void test_follow_ups_after_a_name_the_system_refuses(void)
{
 WpeWriteOutcome o;
 char long_path[512], s[WPE_WRITE_SENTENCE_MAX];

 memset(long_path, 'q', sizeof long_path);
 memcpy(long_path, "/tmp/", 5);
 long_path[5 + 300] = '\0';
 chk_int("long name: refused", wpe_write_text(long_path, "x", &o), -1);
 chk_int("long name: ENAMETOOLONG", o.err, ENAMETOOLONG);
 wpe_write_sentence(s, sizeof s, &o, WPE_WRITE_THEN_NO_PROJECT);
 chk_has("no project: the reason", s, "File name too long");
 chk_has("no project: what stays", s, "  No project was created.");
 wpe_write_sentence(s, sizeof s, &o, WPE_WRITE_THEN_PROJECT_KEPT);
 chk_has("project kept", s, "  The project list is still in its window.");
 wpe_write_sentence(s, sizeof s, &o, WPE_WRITE_THEN_NO_OPTIONS);
 chk_has("no options", s, "  The options were not saved.");
 wpe_write_sentence(s, sizeof s, &o, WPE_WRITE_THEN_USER_KEYS_STAY);
 chk_has("user keys stay", s, "Your own settings (hook, engine command, endpoints, dial) are still in it;");
 chk_has("user keys stay: what to do", s, "take them out before you share the file.");
 chk_lacks("user keys stay: not the window", s, "still in the window");
}

/* ---- the stream pair ---- */

static void test_stream_in_a_folder_that_refuses(void)
{
 WpeWriteOutcome o;
 char p[512];
 FILE *fp;

 make_dir();
 if (!test_premise(test_premise_folder_refuses_new_files(dir), "stream in a closed folder",
                    "this user can create files in a 0500 folder (root, or a capability that overrides file modes)"))
  return;
 path_of(p, sizeof p, "new.prj");
 chmod(dir, 0500);
 fp = wpe_write_stream_open(p, &o);
 chmod(dir, 0700);
 chk_int("stream open refused", fp == NULL, 1);
 chk_int("stream open: FAILED", o.status, WPE_WRITE_FAILED);
 chk_int("stream open: EACCES", o.err, EACCES);
 chk_has("stream open: path", o.path, "new.prj");
}

static void test_stream_onto_full_device(void)
{
 WpeWriteOutcome o;
 struct stat st;
 FILE *fp;
 char s[WPE_WRITE_SENTENCE_MAX];

 if (stat("/dev/full", &st) != 0 || !S_ISCHR(st.st_mode))
 {
  printf("SKIP: stream onto /dev/full: this system has no /dev/full device\n");
  return;
 }
 fp = wpe_write_stream_open("/dev/full", &o);
 chk_int("stream /dev/full opens", fp != NULL, 1);
 if (fp == NULL)
  return;
 fputs("x\n", fp);
 chk_int("stream /dev/full: close fails", wpe_write_stream_close(fp, &o), -1);
 chk_int("stream /dev/full: FAILED", o.status, WPE_WRITE_FAILED);
 chk_int("stream /dev/full: ENOSPC", o.err, ENOSPC);
 /* the open emptied the file: the sentence must not say the text is safe */
 wpe_write_sentence(s, sizeof s, &o, WPE_WRITE_THEN_NO_OPTIONS);
 chk_has("stream close: may be cut short", s, "may be cut short; no backup copy was made.");
}

static void test_stream_written_and_empty(void)
{
 WpeWriteOutcome o;
 char buf[64], p[512];
 FILE *fp;

 make_dir();
 path_of(p, sizeof p, "list.prj");
 fp = wpe_write_stream_open(p, &o);
 chk_int("stream opens", fp != NULL, 1);
 if (fp == NULL)
  return;
 fprintf(fp, "a\n%s\n", "b");
 chk_int("stream close ok", wpe_write_stream_close(fp, &o), 0);
 chk_int("stream: WRITTEN", o.status, WPE_WRITE_WRITTEN);
 get_file("list.prj", buf, sizeof buf);
 chk_has("stream: content", buf, "a\nb\n");
 path_of(p, sizeof p, "empty.prj");
 fp = wpe_write_stream_open(p, &o);
 chk_int("empty stream opens", fp != NULL, 1);
 if (fp == NULL)
  return;
 chk_int("empty stream close ok", wpe_write_stream_close(fp, &o), 0);
 chk_int("empty stream: WRITTEN", o.status, WPE_WRITE_WRITTEN);
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
 if (!test_premise(test_premise_folder_refuses_new_files(dir), "a backup is not made in a folder that refuses",
                   "this user can create files in a 0500 folder (root, or a capability that overrides file modes)"))
  return;
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

/* Which special bits each file had when text was written into it, by inode. */
#define WATCHED_FILES 8
static ino_t watched_ino[WATCHED_FILES];
static mode_t watched_special[WATCHED_FILES];
static int watched_n;

static void watch_special_bits(const struct stat *st)
{
 int i;

 for (i = 0; i < watched_n; i++)
  if (watched_ino[i] == st->st_ino)
  {
   watched_special[i] |= st->st_mode & 07000;
   return;
  }
 if (watched_n < WATCHED_FILES)
 {
  watched_ino[watched_n] = st->st_ino;
  watched_special[watched_n++] = st->st_mode & 07000;
 }
}

/* The special bits the file of this inode had at any write; -1: never written. */
static int special_bits_seen(ino_t ino)
{
 int i;

 for (i = 0; i < watched_n; i++)
  if (watched_ino[i] == ino)
   return((int)watched_special[i]);
 return(-1);
}

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
 {
  write_shared_bits |= (int)(st.st_mode & 077);
  watch_special_bits(&st);
 }
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

/* fsync, watched: the calls on regular files are counted, the special bits
   the first one found on its file are remembered, and an armed call fails
   with EIO.  Off, it passes every call on. */
typedef int (*fsync_fn)(int);

static int fsync_calls;           /* calls on regular files since the last reset */
static ino_t fsync_last_ino;      /* the inode of the last such call */
static int fsync_first_special;   /* special bits the first such file had, -1: none yet */
static int fsync_fails;           /* 1: a call on a regular file fails with EIO */
static int fdatasync_calls;       /* fdatasync calls on regular files since the last reset */

static void fsync_reset(void)
{
 fsync_calls = 0;
 fsync_last_ino = 0;
 fsync_first_special = -1;
 fsync_fails = 0;
 fdatasync_calls = 0;
}

/* fdatasync, watched the same way: the calls on regular files are counted. */
int fdatasync(int fd)
{
 static fsync_fn real;
 struct stat st;

 if (real == NULL)
  *(void **)&real = dlsym(RTLD_NEXT, "fdatasync");
 if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode))
  fdatasync_calls++;
 return(real(fd));
}

int fsync(int fd)
{
 static fsync_fn real;
 struct stat st;

 if (real == NULL)
  *(void **)&real = dlsym(RTLD_NEXT, "fsync");
 if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode))
 {
  if (fsync_calls++ == 0)
   fsync_first_special = (int)(st.st_mode & 07000);
  fsync_last_ino = st.st_ino;
  if (fsync_fails)
  {
   errno = EIO;
   return(-1);
  }
 }
 return(real(fd));
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
 if (!test_premise(test_premise_file_0200_is_unreadable(dir), "a failure with no backup says the file may be cut short",
                   "this user can read a 0200 file (root, or a capability that overrides file modes)"))
  return;
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
 gid_t other = test_other_group();
 struct stat st;

 if (other == TEST_DIR_NO_GROUP)
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

/* ---- a save keeps the file's special bits; the .bak never has them ---- */

/* The window of a file read with this whole mode, as Save sees it. */
static FENSTER *window_of_file_with_mode(mode_t mode)
{
 FENSTER *f = make_window();

 f->filemode = (int)(S_IFREG | mode);
 return(f);
}

static void save_sentence(const WpeWriteOutcome *o, char *out, size_t n)
{
 (void)wpe_write_sentence(out, n, o, WPE_WRITE_THEN_KEPT);
}

/* Save the window onto the file NAME of the test folder, as File > Save does
   (the one writer that gives a file back its special bits). */
static int save_window(FENSTER *f, const char *name, WpeWriteOutcome *o)
{
 f->dirct = dir;
 f->datnam = (char *)name;
 f->dtmd = DTMD_NORMAL;
 f->ed->mxedt = 1;
 f->ed->f[1] = f;
 return(wpe_write_file(f, o));
}

/* Save As: the window, which read the file OLD_DIR/OLD_NAME, is written under
   NEW_DIR/NEW_NAME, as the file manager's Save As does. */
static int save_window_as(FENSTER *f, char *new_dir, const char *new_name, char *old_dir,
                          const char *old_name)
{
 f->dirct = new_dir;
 f->datnam = (char *)new_name;
 f->dtmd = DTMD_NORMAL;
 f->ed->mxedt = 1;
 f->ed->f[1] = f;
 return(wpe_write_save(f, WPE_WRITE_THEN_NAME_KEPT, old_dir, old_name));
}

/* A Save that fails half way: the limit is below what it writes. */
static void save_past_limit(FENSTER *f, const char *name, WpeWriteOutcome *o, size_t limit,
                            size_t write_bytes)
{
 char *text = big(write_bytes, 'n');

 set_rows(text);
 limit_file_size(limit);
 chk_int("the save fails", save_window(f, name, o), -1);
 no_file_size_limit();
}

/* A setgid program saved by its owner is still setgid: the write clears the
   bit and the save puts it back. */
static void test_a_saved_setgid_file_keeps_its_bit(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char buf[256], say[1024];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "a saved setgid file keeps its bit",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("t.c", "old\n", 02755);
 f = window_of_file_with_mode(02755);
 set_rows("new\n");
 chk_int("write", save_window(f, "t.c", &o), 0);
 chk_has("file written", get_file("t.c", buf, sizeof buf), "new\n");
 chk_int("the saved file keeps setgid", mode_of("t.c"), 02755);
 save_sentence(&o, say, sizeof say);
 chk_int("nothing to say when the bit was kept", (long)strlen(say), 0);
}

/* A system that refuses or drops the bit: the text is saved, and the status
   row names the bit and why. */
static void test_a_save_names_the_bit_the_system_refuses(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char buf[256], say[1024];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "a save names a refused bit",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("t.c", "old\n", 02755);
 f = window_of_file_with_mode(02755);
 set_rows("new\n");
 fakechmod_set(FAKECHMOD_REFUSE, S_ISGID);
 chk_int("write", save_window(f, "t.c", &o), 0);
 fakechmod_set(FAKECHMOD_OFF, 0);
 chk_int("written", o.status, WPE_WRITE_WRITTEN);
 chk_has("file written", get_file("t.c", buf, sizeof buf), "new\n");
 chk_int("the nine bits are on", mode_of("t.c"), 0755);
 save_sentence(&o, say, sizeof say);
 chk_has("the bit is named", say, "t.c written, but the setgid bit was not kept: ");
 chk_has("the reason is the system's", say, "Operation not permitted");

 put_file("u.c", "old\n", 02755);
 f = window_of_file_with_mode(02755);
 fakechmod_set(FAKECHMOD_DROP, S_ISGID);
 chk_int("write, bit dropped", save_window(f, "u.c", &o), 0);
 fakechmod_set(FAKECHMOD_OFF, 0);
 save_sentence(&o, say, sizeof say);
 chk_has("a dropped bit is named", say, "u.c written, but the setgid bit was not kept: ");
 chk_has("a dropped bit has its reason", say, "the system cleared it");
}

/* The nine permission bits refused after the text is on disk is a mode that
   did not change, not a lost save. */
static void test_a_save_whose_modes_are_all_refused_is_still_written(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char buf[256], say[1024];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "a refused mode is not a lost save",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("t.c", "old\n", 02755);
 f = window_of_file_with_mode(02755);
 set_rows("new\n");
 fakechmod_set(FAKECHMOD_REFUSE, 07777);
 chk_int("write", save_window(f, "t.c", &o), 0);
 fakechmod_set(FAKECHMOD_OFF, 0);
 chk_int("written", o.status, WPE_WRITE_WRITTEN);
 chk_has("file written", get_file("t.c", buf, sizeof buf), "new\n");
 save_sentence(&o, say, sizeof say);
 chk_has("the bit is named", say, "setgid bit was not kept");
}

/* The backup is a copy of old text: no setuid, setgid or sticky bit at any
   moment, whatever the file had. */
static void test_the_backup_never_has_the_special_bits(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char bakp[512];
 struct stat st;

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "the backup has no special bit",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("t.c", "old\n", 02755);
 f = window_of_file_with_mode(02755);
 set_rows("new\n");
 path_of(bakp, sizeof bakp, "t.bak");
 watched_n = 0;
 watch_writes = 1;
 chk_int("write", save_window(f, "t.c", &o), 0);
 watch_writes = 0;
 chk_int("stat the backup", stat(bakp, &st), 0);
 chk_int("the backup held no special bit while it was written", special_bits_seen(st.st_ino), 0);
 chk_int("the backup ends with the nine bits", mode_of("t.bak"), 0755);
}

/* A file with no special bit is saved and backed up as before. */
static void test_the_nine_bits_of_a_save_are_unchanged(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char buf[256], say[1024];

 make_dir();
 put_file("t.c", "old\n", 0640);
 f = window_of_file_with_mode(0640);
 set_rows("new\n");
 chk_int("write", save_window(f, "t.c", &o), 0);
 chk_has("file written", get_file("t.c", buf, sizeof buf), "new\n");
 chk_int("the file keeps its nine bits", mode_of("t.c"), 0640);
 chk_int("the backup has the nine bits", mode_of("t.bak"), 0640);
 save_sentence(&o, say, sizeof say);
 chk_int("nothing to say", (long)strlen(say), 0);
}

/* A save that fails half way puts the old text back with the file's special
   bits, which the failed write had cleared. */
static void test_a_restore_keeps_the_special_bits(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char *old = big(1024, 'o'), buf[8192];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "a restore keeps setgid",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("t.c", old, 02755);
 f = window_of_file_with_mode(02755);
 save_past_limit(f, "t.c", &o, 2048, 4096);
 chk_int("restored", o.restore_err, 0);
 chk_int("the file is as it was", strcmp(get_file("t.c", buf, sizeof buf), old), 0);
 chk_int("the restored file keeps setgid", mode_of("t.c"), 02755);
}

/* ---- where the bits come from: the file the write overwrites ---- */

/* The bits a write puts back are the ones the inode had when the write opened
   it, never the window's mode and never for a copy.  One row for the four
   copies that go through wpe_write_range (Block Write, the autosave, the
   emergency save and the print copy): a new name has no bit to give back.  The
   window says 02755 on purpose, as a window of a setgid program does. */
static void test_a_block_write_to_a_new_name_gets_no_bit(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char p[512], say[1024];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "a copy to a new name gets no bit",
                   "this user cannot set setgid in the test folder"))
  return;
 f = window_of_file_with_mode(02755);
 set_rows("block\n");
 path_of(p, sizeof p, "blk.txt");
 chk_int("block write with a backup", wpe_write_range(f, p, 0, 0, 1, 1, 1, &o), 0);
 chk_int("the block's file has no special bit", mode_of("blk.txt"), 0755);
 chk_int("nothing was kept or lost", (long)o.file_kept.lost, 0);
 save_sentence(&o, say, sizeof say);
 chk_int("nothing to say", (long)strlen(say), 0);
 path_of(p, sizeof p, "blk2.txt");
 chk_int("copy with no backup", wpe_write_range(f, p, 0, 0, 1, 1, 0, &o), 0);
 chk_int("that file has no special bit", mode_of("blk2.txt"), 0755);
}

/* Over an existing setgid file a copy is still a copy: the file it overwrites
   loses the bit and the write does not give it back, a Block Write and the
   autosave of the file alike. */
static void test_a_copy_over_an_existing_setgid_file_gets_no_bit(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char p[512], say[1024];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "a copy over a setgid file gets no bit",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("blk.txt", "old\n", 02755);
 put_file("t.c.ASV", "old\n", 02755);
 f = window_of_file_with_mode(02755);
 set_rows("block\n");
 path_of(p, sizeof p, "blk.txt");
 chk_int("block write onto it", wpe_write_range(f, p, 0, 0, 1, 1, 1, &o), 0);
 chk_int("the block's file ends with the nine bits", mode_of("blk.txt"), 0755);
 save_sentence(&o, say, sizeof say);
 chk_int("nothing to say for the block", (long)strlen(say), 0);
 path_of(p, sizeof p, "t.c.ASV");
 chk_int("autosave onto it", wpe_write_range(f, p, 0, 0, 1, 1, 0, &o), 0);
 chk_int("the autosave ends with the nine bits", mode_of("t.c.ASV"), 0755);
 save_sentence(&o, say, sizeof say);
 chk_int("nothing to say for the autosave", (long)strlen(say), 0);
}

/* A user who took setuid off a program to make it safe does not get it back
   at the next Save, whatever the window read when it loaded the file. */
static void test_a_bit_removed_on_disk_is_not_restored(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char buf[256], say[1024];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISUID), "a removed bit stays removed",
                   "this user cannot set setuid in the test folder"))
  return;
 put_file("t.c", "old\n", 0755);
 f = window_of_file_with_mode(04755);
 set_rows("new\n");
 chk_int("save", save_window(f, "t.c", &o), 0);
 chk_has("file written", get_file("t.c", buf, sizeof buf), "new\n");
 chk_int("the bit removed on disk stays removed", mode_of("t.c"), 0755);
 save_sentence(&o, say, sizeof say);
 chk_int("nothing to say", (long)strlen(say), 0);
}

/* The other direction: a bit put on the file since the window loaded it is on
   the file that is overwritten, so it is kept. */
static void test_a_bit_added_on_disk_since_loading_is_kept(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char buf[256];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "a bit added on disk is kept",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("t.c", "old\n", 02755);
 f = window_of_file_with_mode(0755);
 set_rows("new\n");
 chk_int("save", save_window(f, "t.c", &o), 0);
 chk_has("file written", get_file("t.c", buf, sizeof buf), "new\n");
 chk_int("the bit on disk is kept", mode_of("t.c"), 02755);
}

/* The truncation has no size condition: an empty file saved with no text is
   still cut, so its modification time moves (the build's up-to-date checks
   read it).  A truncation skipped for an empty file leaves the time alone. */
static void test_saving_an_empty_file_over_an_empty_file_marks_it_changed(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char p[512];
 struct timeval old[2];
 struct stat st;

 make_dir();
 put_file("t.c", "", 0644);
 path_of(p, sizeof p, "t.c");
 old[0].tv_sec = old[1].tv_sec = 946684800;
 old[0].tv_usec = old[1].tv_usec = 0;
 chk_int("set the old time", utimes(p, old), 0);
 f = window_of_file_with_mode(0644);
 set_rows("");
 chk_int("save", save_window(f, "t.c", &o), 0);
 chk_int("stat", stat(p, &st), 0);
 chk_int("the empty file was cut, so its time moved", st.st_mtime > 946684800, 1);
}

/* Save As writes a different file.  Over an existing setgid program it is a
   copy for the bits: the program keeps its mode of nine bits, never a setgid
   text nobody can run.  A Save of the same window onto its own file keeps the
   bit (test_a_saved_setgid_file_keeps_its_bit). */
static void test_save_as_over_an_existing_setgid_file_gets_no_bit(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char buf[256];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "Save As over a setgid file gets no bit",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("u.c", "program\n", 02755);
 f = window_of_file_with_mode(0644);
 set_rows("text\n");
 put_file("t.c", "window's file\n", 0644);
 chk_int("Save As", save_window_as(f, dir, "u.c", dir, "t.c"), WPE_WRITE_WRITTEN);
 chk_has("file written", get_file("u.c", buf, sizeof buf), "text\n");
 chk_int("the file Save As overwrote has no special bit", mode_of("u.c"), 0644);
}

/* Save As onto the window's own file, by the same folder and name, is a Save
   for the bits: a setgid program saved as itself is still one. */
static void test_save_as_onto_the_windows_own_file_keeps_its_bit(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char buf[256];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "Save As onto its own file keeps the bit",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("t.c", "old\n", 02755);
 f = window_of_file_with_mode(02755);
 set_rows("new\n");
 chk_int("Save As", save_window_as(f, dir, "t.c", dir, "t.c"), WPE_WRITE_WRITTEN);
 chk_has("file written", get_file("t.c", buf, sizeof buf), "new\n");
 chk_int("the window's own file keeps setgid", mode_of("t.c"), 02755);
}

/* The same name in another folder is another file: a copy for the bits, so
   a name match alone does not give the bit. */
static void test_save_as_to_the_same_name_in_another_folder_gets_no_bit(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char sub[512], buf[256];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "the same name elsewhere gets no bit",
                   "this user cannot set setgid in the test folder"))
  return;
 path_of(sub, sizeof sub, "sub");
 chk_int("make the other folder", mkdir(sub, 0755), 0);
 put_file("t.c", "window's file\n", 0755);
 put_file("sub/t.c", "other\n", 02755);
 f = window_of_file_with_mode(0755);
 set_rows("new\n");
 chk_int("Save As", save_window_as(f, sub, "t.c", dir, "t.c"), WPE_WRITE_WRITTEN);
 chk_has("file written", get_file("sub/t.c", buf, sizeof buf), "new\n");
 chk_int("the other folder's file has no special bit", mode_of("sub/t.c"), 0755);
 chk_int("the window's own file is untouched", mode_of("t.c"), 0755);
}

/* A hard link to the window's own file is the same inode, so it is the same
   file for the bits: the bit stays on the program whichever name saved it. */
static void test_save_as_through_a_hard_link_to_its_own_file_keeps_the_bit(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char own[512], link_to[512], buf[256];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "a hard link to its own file keeps the bit",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("t.c", "old\n", 02755);
 path_of(own, sizeof own, "t.c");
 path_of(link_to, sizeof link_to, "l.c");
 chk_int("hard link", link(own, link_to), 0);
 f = window_of_file_with_mode(02755);
 set_rows("new\n");
 chk_int("Save As", save_window_as(f, dir, "l.c", dir, "t.c"), WPE_WRITE_WRITTEN);
 chk_has("file written", get_file("t.c", buf, sizeof buf), "new\n");
 chk_int("the one inode keeps setgid", mode_of("t.c"), 02755);
}

/* A write that fails half way and is undone gives back what the overwritten
   file had, a copy's target included: the restore undoes the write, it does
   not give the copy a bit. */
static void test_a_failed_block_write_over_a_setgid_file_restores_its_bit(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char *old = big(1024, 'o'), buf[8192];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "a failed copy restores the bit",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("blk.txt", old, 02755);
 f = window_of_file_with_mode(0755);
 write_past_limit(f, "blk.txt", &o, 2048, 4096);
 chk_int("restored", o.restore_err, 0);
 chk_int("the file is as it was", strcmp(get_file("blk.txt", buf, sizeof buf), old), 0);
 chk_int("the restored file has its setgid bit again", mode_of("blk.txt"), 02755);
}

/* The file is no longer truncated by the open, so the truncation is its own
   step: a longer file saved short leaves no tail of the old text. */
static void test_a_longer_file_saved_short_leaves_no_tail(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char *old = big(4096, 'o'), buf[8192];

 make_dir();
 put_file("t.c", old, 0644);
 f = window_of_file_with_mode(0644);
 set_rows("new\n");
 chk_int("save", save_window(f, "t.c", &o), 0);
 chk_int("the file is the new text and nothing else", strcmp(get_file("t.c", buf, sizeof buf), "new\n"), 0);
}

/* ---- what the user is told ---- */

/* A save that lost a bit and made no backup says both in one sentence: the
   backup note must not hide the bit. */
static void test_the_bit_loss_and_the_backup_note_share_one_sentence(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char say[1024], target[512], link[512];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "one sentence for the bit and the backup",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("t.c", "old\n", 02755);
 put_file("victim", "keep me\n", 0644);
 path_of(target, sizeof target, "victim");
 path_of(link, sizeof link, "t.bak");
 if (symlink(target, link) != 0)
  perror("symlink");
 f = window_of_file_with_mode(02755);
 set_rows("new\n");
 fakechmod_set(FAKECHMOD_REFUSE, S_ISGID);
 chk_int("write", save_window(f, "t.c", &o), 0);
 fakechmod_set(FAKECHMOD_OFF, 0);
 save_sentence(&o, say, sizeof say);
 chk_int("both clauses, one sentence",
         strcmp(say, "t.c written, but the setgid bit was not kept: Operation not permitted; "
                "no backup was made: File exists."), 0);
}

/* A save that fails half way and is undone names a bit the undo could not put
   back, in the same box as the failure.  Only the restore's call carries a
   special bit, so refusing setgid refuses only it. */
static void test_a_restore_that_cannot_put_the_bits_back_names_them(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char *old = big(1024, 'o'), buf[8192], say[2048];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "a restore names a bit it could not put back",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("t.c", old, 02755);
 f = window_of_file_with_mode(0755);
 fakechmod_set(FAKECHMOD_REFUSE, S_ISGID);
 write_past_limit(f, "t.c", &o, 2048, 4096);
 fakechmod_set(FAKECHMOD_OFF, 0);
 chk_int("restored", o.restore_err, 0);
 chk_int("the file is as it was", strcmp(get_file("t.c", buf, sizeof buf), old), 0);
 chk_int("without the bit", mode_of("t.c"), 0755);
 chk_int("the bit is recorded", o.restore_kept.lost, S_ISGID);
 chk_int("the save itself lost nothing", o.file_kept.lost, 0);
 wpe_write_sentence(say, sizeof say, &o, WPE_WRITE_THEN_KEPT);
 chk_has("the failure", say, "File too large.");
 chk_has("the bit is named", say, "The file holds its previous content, but the setgid bit was not kept: "
         "Operation not permitted.");
 chk_has("and what stays", say, "Your text is still in the window.");
}

/* The kernel clears the bits on a write so a half-written program is not one.
   They go back only after the text is on disk: one sync on the saved file for
   a setgid save, none for a plain one, and a sync that fails is a failed save
   (the window stays modified, the old text and no bit come back). */
static void test_the_bits_go_back_only_after_the_bytes_are_on_disk(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char *old = big(1024, 'o'), buf[8192], say[2048], p[512];
 struct stat st;

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "the bits go back after the bytes",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("t.c", old, 02755);
 path_of(p, sizeof p, "t.c");
 f = window_of_file_with_mode(02755);
 set_rows("new\n");
 fsync_reset();
 chk_int("a setgid save", save_window(f, "t.c", &o), 0);
 stat(p, &st);
 chk_int("one sync", fsync_calls, 1);
 chk_int("on the saved file", (long)(fsync_last_ino == st.st_ino), 1);
 chk_int("before the bit was on it", fsync_first_special, 0);
 chk_int("the bit is on after", mode_of("t.c"), 02755);

 put_file("u.c", "old\n", 0644);
 f = window_of_file_with_mode(0644);
 fsync_reset();
 chk_int("a plain save", save_window(f, "u.c", &o), 0);
 chk_int("costs no sync", fsync_calls, 0);
 chk_int("and no data sync", fdatasync_calls, 0);

 put_file("v.c", old, 02755);
 f = window_of_file_with_mode(02755);
 f->save = 1;
 fsync_reset();
 fsync_fails = 1;
 chk_int("a save whose sync fails", save_window(f, "v.c", &o), -1);
 fsync_reset();
 chk_int("is FAILED", o.status, WPE_WRITE_FAILED);
 chk_int("with the sync's errno", o.err, EIO);
 wpe_write_sentence(say, sizeof say, &o, WPE_WRITE_THEN_KEPT);
 chk_has("and says so", say, "Input/output error");
 chk_has("with the path", say, "Cannot write ");
 chk_int("the window is still modified", f->save, 1);
 chk_int("the old text is back", strcmp(get_file("v.c", buf, sizeof buf), old), 0);
 chk_int("the bit did not go back", mode_of("v.c"), 0755);
}

/* What the sync owner calls for each of the four cases, asked directly: a
   plain file that is not owed costs nothing, a plain durable one costs one
   data sync, a file about to get a special bit costs one full sync, and a
   durable one with a bit costs that one sync and not a data sync as well. */
static void test_the_sync_owner_syncs_once_and_only_when_owed(void)
{
 char p[512];
 int fd;

 make_dir();
 put_file("s.c", "text\n", 0644);
 path_of(p, sizeof p, "s.c");
 fd = open(p, O_WRONLY);
 if (fd < 0)
 {
  printf("FAIL: the sync owner: cannot open its file\n");
  fails++;
  return;
 }
 fsync_reset();
 chk_int("plain, not durable: ok", wpe_write_sync_before_bits(fd, 0644, 0), 0);
 chk_int("plain, not durable: no fsync", fsync_calls, 0);
 chk_int("plain, not durable: no fdatasync", fdatasync_calls, 0);
 fsync_reset();
 chk_int("plain, durable: ok", wpe_write_sync_before_bits(fd, 0644, 1), 0);
 chk_int("plain, durable: no fsync", fsync_calls, 0);
 chk_int("plain, durable: one fdatasync", fdatasync_calls, 1);
 fsync_reset();
 chk_int("special, not durable: ok", wpe_write_sync_before_bits(fd, 02755, 0), 0);
 chk_int("special, not durable: one fsync", fsync_calls, 1);
 chk_int("special, not durable: no fdatasync", fdatasync_calls, 0);
 fsync_reset();
 chk_int("special, durable: ok", wpe_write_sync_before_bits(fd, 02755, 1), 0);
 chk_int("special, durable: one fsync", fsync_calls, 1);
 chk_int("special, durable: no fdatasync", fdatasync_calls, 0);
 close(fd);
}

/* The sentence for an outcome built by hand: the reasons that need a system
   this one is not (a group the user is not in, no descriptor to ask on). */
static void test_the_sentence_of_a_save_that_lost_a_bit(void)
{
 WpeWriteOutcome o;
 char say[1024];

 memset(&o, 0, sizeof o);
 o.status = WPE_WRITE_WRITTEN;
 snprintf(o.path, sizeof o.path, "%s/t.c", "/x");
 o.file_kept.lost = S_ISGID;
 o.file_kept.err = EPERM;
 o.file_kept.gid = 12;
 o.file_kept.in_group = 0;
 save_sentence(&o, say, sizeof say);
 chk_has("the group is the reason", say, "t.c written, but the setgid bit was not kept: "
         "this user is not in group 12.");
 o.file_kept.lost = S_ISUID | S_ISGID;
 o.file_kept.in_group = 1;
 save_sentence(&o, say, sizeof say);
 chk_has("two bits", say, "the setuid and setgid bits were not kept: Operation not permitted.");
 o.file_kept.err = EMFILE;
 save_sentence(&o, say, sizeof say);
 chk_has("no descriptor to ask on", say, "Too many open files");
 o.file_kept.lost = 0;
 save_sentence(&o, say, sizeof say);
 chk_int("nothing lost, nothing said", (long)strlen(say), 0);

 /* a failed write, undone, that could not put the bit back */
 o.status = WPE_WRITE_FAILED;
 o.err = EFBIG;
 o.restore_kept.lost = S_ISGID;
 o.restore_kept.err = EPERM;
 o.restore_kept.gid = 12;
 o.restore_kept.in_group = 0;
 save_sentence(&o, say, sizeof say);
 chk_int("the restore names the bit and the group",
         strcmp(say, "Cannot write /x/t.c: File too large.  The file holds its previous content, "
                "but the setgid bit was not kept: this user is not in group 12.  "
                "Your text is still in the window."), 0);
 o.restore_err = EIO;
 save_sentence(&o, say, sizeof say);
 chk_lacks("a restore that failed says no bit", say, "was not kept");
}

static void stub_mouse_shape(WpeMouseShape s) { (void)s; }
static void stub_mouse_restore(void) { }

int main(void)
{
 WpeMouseChangeShape = stub_mouse_shape;
 WpeMouseRestoreShape = stub_mouse_restore;
 test_a_block_write_to_a_new_name_gets_no_bit();
 test_a_copy_over_an_existing_setgid_file_gets_no_bit();
 test_a_bit_removed_on_disk_is_not_restored();
 test_a_bit_added_on_disk_since_loading_is_kept();
 test_a_longer_file_saved_short_leaves_no_tail();
 test_saving_an_empty_file_over_an_empty_file_marks_it_changed();
 test_save_as_over_an_existing_setgid_file_gets_no_bit();
 test_save_as_onto_the_windows_own_file_keeps_its_bit();
 test_save_as_to_the_same_name_in_another_folder_gets_no_bit();
 test_save_as_through_a_hard_link_to_its_own_file_keeps_the_bit();
 test_a_failed_block_write_over_a_setgid_file_restores_its_bit();
 test_the_sentence_of_a_save_that_lost_a_bit();
 test_the_bit_loss_and_the_backup_note_share_one_sentence();
 test_a_restore_that_cannot_put_the_bits_back_names_them();
 test_the_bits_go_back_only_after_the_bytes_are_on_disk();
 test_the_sync_owner_syncs_once_and_only_when_owed();
 test_a_saved_setgid_file_keeps_its_bit();
 test_a_save_names_the_bit_the_system_refuses();
 test_a_save_whose_modes_are_all_refused_is_still_written();
 test_the_backup_never_has_the_special_bits();
 test_the_nine_bits_of_a_save_are_unchanged();
 test_a_restore_keeps_the_special_bits();
 test_sentence();
 test_text();
 test_follow_ups_after_a_name_the_system_refuses();
 test_stream_in_a_folder_that_refuses();
 test_stream_onto_full_device();
 test_stream_written_and_empty();
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
 fails += test_dir_failures;
 if (fails) {
  printf("test_write_owner: %d failure(s)\n", fails);
  return(1);
 }
 printf("test_write_owner: ok\n");
 return(0);
}
