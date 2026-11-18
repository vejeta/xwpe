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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>

#include "../edit.h"
#include "../we_write.h"
#include "testdir.h"
#include "seen_rows.h"
#include "fakechmod.h"
#include "test_watchdog.h"

/** How long the FIFO row's writer may live, whatever happens to the row. */
#define WRITER_LIFETIME_S 30
/** How long the row waits for the writer to be inside its open. */
#define WRITER_BLOCK_WAIT_MS 5000
/** How long the row watches the writer after the change of mode: a writer
    released by an open for reading leaves its open within this. */
#define WRITER_WATCH_MS 300
/** How long the row gives the ended writer to be reaped. */
#define WRITER_END_MS 5000

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
static int question_answer = 'Y';
static int question_count;
static char question_text[512];
/* What the box can see while it is up: whether SIGXFSZ is ignored (the
   owner's guard is held), and the text a program changes in the window. */
static int question_saw_xfsz_ignored;
static const char *question_changes_rows;
/* What another program does to the folder while the box is up. */
static void (*question_during)(void);
static void set_rows(const char *text);
int e_message(int sw, char *s, FENSTER *f)
{
 struct sigaction sa;

 (void)sw; (void)f;
 if (sigaction(SIGXFSZ, NULL, &sa) == 0)
  question_saw_xfsz_ignored = (sa.sa_handler == SIG_IGN);
 if (question_changes_rows != NULL)
  set_rows(question_changes_rows);
 if (question_during != NULL)
  question_during();
 question_count++;
 snprintf(question_text, sizeof question_text, "%s", s);
 return(question_answer);
}
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
/* Every folder this test made, in a list that grows with the rows, so that
   none is left behind whatever the number of rows. */
static char (*made)[256];
static int made_n;
static int made_room;

static void remove_made_dirs(void);

/* Remember the folder just made; a list that cannot grow is a failure that
   removes what is known and stops, not a folder left in /tmp. */
static void remember_made_dir(void)
{
 char (*grown)[256];

 if (made_n == made_room)
 {
  grown = realloc(made, (size_t)(made_room + 64) * sizeof *made);
  if (grown == NULL)
  {
   printf("FAIL: no room to remember the test folders\n");
   remove_made_dirs();
   exit(2);
  }
  made = grown;
  made_room += 64;
 }
 snprintf(made[made_n++], sizeof made[0], "%s", dir);
}

static void make_dir(void)
{
 if (test_dir_make(dir, sizeof dir, "wo") != 0 && dir[0] == '\0')
 {
  remove_made_dirs();
  exit(2);
 }
 remember_made_dir();
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
 free(made);
 made = NULL;
 made_room = 0;
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
#define GARBAGE_STEP 256            /* room added each time the list is full */
static void **garbage;
static int garbage_n;
static int garbage_room;

static void *keep(void *p)
{
 void **grown;

 if (garbage_n == garbage_room)
 {
  grown = realloc(garbage, (size_t)(garbage_room + GARBAGE_STEP) * sizeof *garbage);
  if (grown == NULL)
  {
   printf("FAIL: no room to remember what a row allocated\n");
   exit(2);
  }
  garbage = grown;
  garbage_room += GARBAGE_STEP;
 }
 garbage[garbage_n++] = p;
 return(p);
}

static void free_garbage(void)
{
 while (garbage_n > 0)
  free(garbage[--garbage_n]);
 free(garbage);
 garbage = NULL;
 garbage_room = 0;
 free(rows_text);
 rows_text = NULL;
}

/* Set by a row whose window read no file: its saves have no identity. */
static int window_reads_nothing;

static FENSTER *make_window(void)
{
 FENSTER *f = keep(calloc(1, sizeof *f));

 window_reads_nothing = 0;
 question_saw_xfsz_ignored = 0;
 question_changes_rows = NULL;
 question_during = NULL;
 question_answer = 'Y';
 question_count = 0;
 question_text[0] = '\0';
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

/* Write @text over the file @name through the stream pair, the way a list is
   written; the outcome is left in @o. */
static int stream_write_text(const char *name, const char *text, WpeWriteOutcome *o)
{
 char p[512];
 FILE *fp;

 path_of(p, sizeof p, name);
 fp = wpe_write_stream_open(p, o);
 chk_int("the stream opens", fp != NULL, 1);
 if (fp == NULL)
  return(-1);
 fputs(text, fp);
 return(wpe_write_stream_close(fp, o));
}

/* @scenario SC-write-owner-E-11: a text written over a setgid or sticky file
   of ours drops the bit, as a copy does, through both faces (the text and
   the stream pair).  Open with O_TRUNC left the bit on (the kernel clears
   setuid, and setgid only with group execute), so this row is red when
   either face opens with O_TRUNC again. */
static void test_a_text_written_over_a_setgid_file_of_ours_drops_the_bit(void)
{
 static const mode_t modes[] = { 02644, 01644 };
 WpeWriteOutcome o;
 char buf[64], p[512];
 size_t i;

 for (i = 0; i < sizeof modes / sizeof modes[0]; i++)
 {
  make_dir();
  if (!test_premise(test_premise_special_bits_settable(dir, modes[i] & 07000),
                    "a text over a file with a special bit drops it",
                    "this user cannot set the bit in the test folder"))
   continue;
  put_file("list.txt", "old\n", modes[i]);
  path_of(p, sizeof p, "list.txt");
  chk_int("text over it", wpe_write_text(p, "new\n", &o), 0);
  chk_int("text: the bit is gone", mode_of("list.txt"), 0644);
  chk_has("text: the new text", get_file("list.txt", buf, sizeof buf), "new\n");
  chk_int("text: nothing stays", (long)o.file_stays.lost, 0);
  put_file("list.txt", "old\n", modes[i]);
  chk_int("stream over it", stream_write_text("list.txt", "streamed\n", &o), 0);
  chk_int("stream: the bit is gone", mode_of("list.txt"), 0644);
  chk_has("stream: the new text", get_file("list.txt", buf, sizeof buf), "streamed\n");
  chk_int("stream: nothing stays", (long)o.file_stays.lost, 0);
 }
}

/* @scenario SC-write-owner-E-12: a text or a stream over a sticky file whose
   mode the user cannot change keeps the bit, and the outcome says so, with
   the system's reason.  Red when the faces never read the bits that stayed. */
static void test_a_text_over_a_file_whose_bit_cannot_be_dropped_says_it_stays(void)
{
 WpeWriteOutcome o;
 char p[512], say[1024];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISVTX), "a text says a bit stays",
                   "this user cannot set the sticky bit in the test folder"))
  return;
 put_file("list.txt", "old\n", 01664);
 path_of(p, sizeof p, "list.txt");
 fakechmod_set(FAKECHMOD_REFUSE, 07777);
 chk_int("text over it", wpe_write_text(p, "new\n", &o), 0);
 fakechmod_set(FAKECHMOD_OFF, 0);
 chk_int("text: written", o.status, WPE_WRITE_WRITTEN);
 chk_int("text: the bit stays", mode_of("list.txt"), 01664);
 chk_int("text: the outcome names it", (long)o.file_stays.lost, (long)S_ISVTX);
 (void)wpe_write_sentence(say, sizeof say, &o, WPE_WRITE_THEN_KEPT);
 chk_int("text: it says so",
         strcmp(say, "list.txt written, but it keeps the sticky bit: Operation not permitted."), 0);
 fakechmod_set(FAKECHMOD_REFUSE, 07777);
 chk_int("stream over it", stream_write_text("list.txt", "streamed\n", &o), 0);
 fakechmod_set(FAKECHMOD_OFF, 0);
 chk_int("stream: the bit stays", mode_of("list.txt"), 01664);
 chk_int("stream: the outcome names it", (long)o.file_stays.lost, (long)S_ISVTX);
}

/* @scenario SC-write-owner-E-13: a text written to a name that is a link goes
   to the file the link leads to, as a Save writes it, and the link stays. */
static void test_a_text_written_to_a_link_goes_to_its_target(void)
{
 WpeWriteOutcome o;
 char buf[64], p[512], l[512];

 make_dir();
 put_file("real.txt", "old\n", 0644);
 path_of(p, sizeof p, "real.txt");
 path_of(l, sizeof l, "link.txt");
 if (symlink(p, l) != 0)
 {
  perror("symlink");
  return;
 }
 chk_int("text to the link", wpe_write_text(l, "new\n", &o), 0);
 chk_has("the target holds it", get_file("real.txt", buf, sizeof buf), "new\n");
 chk_int("stream to the link", stream_write_text("link.txt", "streamed\n", &o), 0);
 chk_has("the target holds the stream", get_file("real.txt", buf, sizeof buf), "streamed\n");
 chk_int("the link is still a link", mode_of("link.txt") >= 0 && readlink(l, buf, sizeof buf) > 0, 1);
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

/* lstat, with an owner fake: for the one armed path the file looks owned by
   another user, the case a test run by one user cannot make with a real
   file.  Everything else is as the system answers.  The real call is the
   system's fstatat with AT_SYMLINK_NOFOLLOW, which every system has, so the
   fake never needs dlsym.  A row proves the fake took (owner_fake_takes)
   and skips by name when the C library did not route the call through it. */
static char owner_fake_path[512];

static void owner_fake_arm(const char *name)
{
 path_of(owner_fake_path, sizeof owner_fake_path, name);
}

static void owner_fake_disarm(void)
{
 owner_fake_path[0] = '\0';
}

int lstat(const char *path, struct stat *st)
{
 int r = fstatat(AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW);

 if (r == 0 && owner_fake_path[0] != '\0' && strcmp(path, owner_fake_path) == 0)
  st->st_uid = (uid_t)(geteuid() + 1);
 return(r);
}

/* Did the armed fake change what lstat says about the file NAME? */
static int owner_fake_takes(const char *name)
{
 struct stat st;
 char p[512];

 path_of(p, sizeof p, name);
 return(lstat(p, &st) == 0 && st.st_uid != geteuid());
}

/* stat, with its own owner fake, for the question that asks about a name the
   user picked: for the one armed path the file looks owned by another user.
   It is separate from the lstat fake so a row arms only what it needs.  The
   real call is fstatat, which every system has.  A row proves the fake took
   (owner_stat_fake_takes) and skips by name when it did not. */
static char owner_stat_fake_path[512];

static void owner_stat_fake_arm(const char *name)
{
 path_of(owner_stat_fake_path, sizeof owner_stat_fake_path, name);
}

static void owner_stat_fake_disarm(void)
{
 owner_stat_fake_path[0] = '\0';
}

int stat(const char *path, struct stat *st)
{
 int r = fstatat(AT_FDCWD, path, st, 0);

 if (r == 0 && owner_stat_fake_path[0] != '\0' && strcmp(path, owner_stat_fake_path) == 0)
  st->st_uid = (uid_t)(geteuid() + 1);
 return(r);
}

/* Did the armed fake change what stat says about the file NAME? */
static int owner_stat_fake_takes(const char *name)
{
 struct stat st;
 char p[512];

 path_of(p, sizeof p, name);
 return(stat(p, &st) == 0 && st.st_uid != geteuid());
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

/* plant_socket - A socket file at this name (nothing listens on it).  Returns
   0, or -1 after saying why. */
static int plant_socket(const char *path)
{
 struct sockaddr_un addr;
 int fd = socket(AF_UNIX, SOCK_STREAM, 0);

 if (fd < 0)
  return(-1);
 memset(&addr, 0, sizeof addr);
 addr.sun_family = AF_UNIX;
 snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
 if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
  perror("bind");
  close(fd);
  return(-1);
 }
 close(fd);
 return(0);
}

/* open, watched: a read-only open of the armed name, the first one only, is
   preceded by a second file taking that name.  It is the moment between the
   owner opening the file it will overwrite and opening the same name to copy
   it, which another user of the folder can use. */
typedef int (*open_fn)(const char *, int, ...);

static char open_swap_at[512];    /* the name that changes hands ... */
static char open_swap_from[512];  /* ... for this file, or is removed when "", at its next
                                     read-only open */
static char open_plant_at[512];   /* a file appears under this name just before the next
                                     exclusive create of it */
static char open_link_at[512];    /* a link to open_link_to takes this name just before
                                     its next open, whatever the flags */
static char open_link_to[512];
static char open_socket_at[512];  /* a socket takes this name just before its next open */
static char open_fail_at[512];    /* the next open of this name, after any swap above, fails with
                                     open_fail_with and opens nothing: what another system calls
                                     the refusal of a link or a socket (EMLINK, EFTYPE,
                                     EOPNOTSUPP) */
static int  open_fail_with;

/**
 * open_hooked - The one open of this program, with every row's swap and failure.
 * @path, @flags, @mode: as open.
 *
 * Both open and, under _FORTIFY_SOURCE, __open_2 end here: the C library
 * routes an open with flags it cannot see and no mode to __open_2, which a
 * hook of open alone never meets.
 */
static int open_hooked(const char *path, int flags, mode_t mode)
{
 static open_fn real;

 if (real == NULL)
  *(void **)&real = dlsym(RTLD_NEXT, "open");
 if (open_swap_at[0] != '\0' && (flags & O_ACCMODE) == O_RDONLY && strcmp(path, open_swap_at) == 0)
 {
  open_swap_at[0] = '\0';
  if (open_swap_from[0] == '\0')
   unlink(path);
  else if (rename(open_swap_from, path) != 0)
   perror("rename");
 }
 if (open_link_at[0] != '\0' && strcmp(path, open_link_at) == 0)
 {
  open_link_at[0] = '\0';
  if (unlink(path) != 0 || symlink(open_link_to, path) != 0)
   perror("link swap");
 }
 if (open_socket_at[0] != '\0' && strcmp(path, open_socket_at) == 0)
 {
  open_socket_at[0] = '\0';
  if (unlink(path) != 0 || plant_socket(path) != 0)
   perror("socket swap");
 }
 if (open_plant_at[0] != '\0' && (flags & O_EXCL) && strcmp(path, open_plant_at) == 0)
 {
  FILE *planted;

  open_plant_at[0] = '\0';
  planted = fopen(path, "w");
  if (planted != NULL)
  {
   fputs("planted\n", planted);
   fclose(planted);
  }
 }
 if (open_fail_at[0] != '\0' && strcmp(path, open_fail_at) == 0)
 {
  open_fail_at[0] = '\0';
  errno = open_fail_with;
  return(-1);
 }
 return(real(path, flags, mode));
}

int open(const char *path, int flags, ...)
{
 mode_t mode = 0;
 va_list ap;

 if (flags & O_CREAT)
 {
  va_start(ap, flags);
  mode = (mode_t)va_arg(ap, int);
  va_end(ap);
 }
 return(open_hooked(path, flags, mode));
}

#ifdef __USE_FORTIFY_LEVEL
#if __USE_FORTIFY_LEVEL > 0
/** The fortified open with no mode: the same hook as open. */
int __open_2(const char *path, int flags)
{
 return(open_hooked(path, flags, 0));
}
#endif
#endif

/* ftruncate and fcntl, watched: an armed ftruncate fails with EPERM, and the
   Nth duplication of a descriptor (F_DUPFD_CLOEXEC) fails with EMFILE, so a
   row can reach what a full descriptor table or a file that refuses to be cut
   would do. */
typedef int (*ftruncate_fn)(int, off_t);
typedef int (*fcntl_fn)(int, int, ...);

static int ftruncate_fails;       /* 1: ftruncate fails with EPERM */
static int ftruncate_spares_empty_time; /* 1: cutting an empty file to nothing does
                                     nothing at all, its time included, as POSIX allows */
static int dup_fails_at;          /* N > 0: the Nth F_DUPFD_CLOEXEC from now fails */
static void (*dup_fail_action)(void); /* what happens to the folder as that duplication fails */

int ftruncate(int fd, off_t length)
{
 static ftruncate_fn real;

 if (real == NULL)
  *(void **)&real = dlsym(RTLD_NEXT, "ftruncate");
 if (ftruncate_fails)
 {
  errno = EPERM;
  return(-1);
 }
 if (ftruncate_spares_empty_time && length == 0)
 {
  struct stat st;

  /* A system that marks the file changed only when the size changes. */
  if (fstat(fd, &st) == 0 && st.st_size == 0)
   return(0);
 }
 return(real(fd, length));
}

int fcntl(int fd, int cmd, ...)
{
 static fcntl_fn real;
 va_list ap;
 void *arg;

 if (real == NULL)
  *(void **)&real = dlsym(RTLD_NEXT, "fcntl");
 va_start(ap, cmd);
 arg = va_arg(ap, void *);
 va_end(ap);
 if (cmd == F_DUPFD_CLOEXEC && dup_fails_at > 0 && --dup_fails_at == 0)
 {
  if (dup_fail_action != NULL)
   dup_fail_action();
  errno = EMFILE;
  return(-1);
 }
 return(real(fd, cmd, arg));
}

/* fstat, with a fake for a file that cannot be read back: once a change of
   mode has been refused (fakechmod_refusals), every fstat fails with EIO until
   the row switches the fake off.  Nothing before the first refusal is touched,
   so the open and the backup of a write see the file as it is, and only the
   read-back after the text finds nothing to read.  fstat_fake_hits counts the
   calls it failed, so a row proves the fake took (a C library that does not
   route the call through here leaves it at 0) and skips by name when it did
   not. */
typedef int (*fstat_fn)(int, struct stat *);

static int fstat_fails_after_refusal;
static int fstat_fake_hits;

int fstat(int fd, struct stat *st)
{
 static fstat_fn real;

 if (real == NULL)
  *(void **)&real = dlsym(RTLD_NEXT, "fstat");
 if (fstat_fails_after_refusal && fakechmod_refusals > 0)
 {
  fstat_fake_hits++;
  errno = EIO;
  return(-1);
 }
 return(real(fd, st));
}

/* Switch the fstat fake off; return whether it failed a call. */
static int fstat_fake_off(void)
{
 int took = (fstat_fake_hits > 0);

 fstat_fails_after_refusal = 0;
 fstat_fake_hits = 0;
 return(took);
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

/* The window has read the file DIR/NAME as it is now: what e_edit records
   from the descriptor that read it. */
static void window_reads_in(FENSTER *f, const char *folder, const char *name)
{
 chk_int("the window reads its file", wpe_write_identity_of_name(folder, name, &f->file_id), 1);
}

static void window_reads(FENSTER *f, const char *name)
{
 window_reads_in(f, dir, name);
}

/* A row that did not say what its window read reads the file it saves, as it
   was when the row began, so a save is the window's own save; a row that
   made the window read nothing keeps that. */
static void window_reads_unless_said(FENSTER *f, const char *folder, const char *name)
{
 if (f->file_id.known || window_reads_nothing)
  return;
 (void)wpe_write_identity_of_name(folder, name, &f->file_id);
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
 window_reads_unless_said(f, dir, name);
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
 window_reads_unless_said(f, old_dir, old_name);
 return(wpe_write_save(f, WPE_WRITE_THEN_NAME_KEPT, old_name));
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

/* Does this system clear a setgid bit when a file of this mode is written,
   as the kernel does for a user without the privilege to keep it?  Linux
   clears it only when the file has group execute; the BSDs and macOS clear
   it whatever the group execute bit says.  Asks the system with a plain
   write, no fake in the way, and answers for the mode given. */
static int the_system_clears_setgid_on_write_of(mode_t before, mode_t after)
{
 char p[512];
 FILE *fp;

 put_file("probe_clear", "x\n", before);
 path_of(p, sizeof p, "probe_clear");
 fp = fopen(p, "w");
 if (fp != NULL)
 {
  fputs("y\n", fp);
  fclose(fp);
 }
 return(mode_of("probe_clear") == (int)after);
}

/* Does this system clear setgid when a file with group execute is written? */
static int the_system_clears_setgid_on_write(void)
{
 return(the_system_clears_setgid_on_write_of(02775, 0775));
}

/* Does this system clear setgid when a file without group execute is
   written?  Linux does not; macOS and the BSDs do. */
static int the_system_clears_setgid_without_group_exec_on_write(void)
{
 return(the_system_clears_setgid_on_write_of(02664, 0664));
}

/* Save a file of this mode with every mode call refused, and read back the
   mode and the sentence.  The one body of the E-01 halves. */
static void save_with_every_chmod_refused(mode_t mode, WpeWriteOutcome *o, char *say, size_t say_size)
{
 FENSTER *f;

 put_file("t.c", "old\n", mode);
 f = window_of_file_with_mode(mode);
 set_rows("new\n");
 fakechmod_set(FAKECHMOD_REFUSE, 07777);
 chk_int("write", save_window(f, "t.c", o), 0);
 fakechmod_set(FAKECHMOD_OFF, 0);
 chk_int("written", o->status, WPE_WRITE_WRITTEN);
 save_sentence(o, say, say_size);
}

/* E-01, sticky half.  Every system leaves a sticky bit alone on a write, so
   the bit is on after the save and the sentence is empty. */
static void a_save_on_a_sticky_file_says_nothing_false(void)
{
 WpeWriteOutcome o;
 char say[1024];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISVTX),
                   "a save on a file that keeps its bit says nothing false",
                   "this user cannot set the bit in the test folder"))
  return;
 save_with_every_chmod_refused(01664, &o, say, sizeof say);
 chk_int("the mode is as it was", mode_of("t.c"), 01664);
 chk_int("it says nothing", (long)strlen(say), 0);
}

/* E-01, setgid half, on a system that keeps setgid without group execute
   through a write (Linux): the bit is on after the save and the sentence is
   empty, whatever the refused call said. */
static void a_save_on_a_setgid_file_the_system_keeps_says_nothing_false(void)
{
 WpeWriteOutcome o;
 char say[1024];

 save_with_every_chmod_refused(02664, &o, say, sizeof say);
 chk_int("the mode is as it was", mode_of("t.c"), 02664);
 chk_int("it says nothing", (long)strlen(say), 0);
}

/* E-01, setgid half, on a system that clears setgid on any write (macOS and
   the BSDs): the bit is off after the save and the sentence names it, so the
   editor still tells the truth where the premise of the other branch fails. */
static void a_save_on_a_setgid_file_the_system_cleared_names_it(void)
{
 WpeWriteOutcome o;
 char say[1024];

 save_with_every_chmod_refused(02664, &o, say, sizeof say);
 chk_int("the system cleared the bit", mode_of("t.c"), 0664);
 chk_has("the bit is named", say, "setgid bit was not kept");
}

/* @scenario SC-write-owner-E-01: a save never says a bit was lost when the
   file still has it, and says so when the system took it.  The user cannot
   change the file's mode (refuse all).  A sticky file is not touched by the
   write.  A setgid file without group execute is kept by Linux and cleared by
   macOS and the BSDs, so the row asks the system first and checks the half
   that holds there: kept, the sentence is empty; cleared, it names setgid.
   Red when the owner marks the bit lost from the refusal and not from the
   file. */
static void test_a_save_on_a_file_that_keeps_its_bit_says_nothing_false(void)
{
 a_save_on_a_sticky_file_says_nothing_false();
 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID),
                   "a save on a file that keeps its bit says nothing false",
                   "this user cannot set setgid in the test folder"))
  return;
 if (the_system_clears_setgid_without_group_exec_on_write())
  a_save_on_a_setgid_file_the_system_cleared_names_it();
 else
  a_save_on_a_setgid_file_the_system_keeps_says_nothing_false();
}

/* @scenario SC-write-owner-E-02: a bit the kernel cleared is named, with the
   system's reason.  Guard for row E-01: reading back must not hide a real
   loss.  The row needs a system that clears setgid on write for this user,
   and skips by name where it does not (a user with CAP_FSETID). */
static void test_a_save_on_a_file_whose_bit_the_kernel_cleared_names_it(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char say[1024];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID),
                   "a bit the kernel cleared is named", "this user cannot set setgid in the test folder"))
  return;
 if (!test_observation(the_system_clears_setgid_on_write(), "a bit the kernel cleared is named",
                       "this user keeps setgid through a write (CAP_FSETID)"))
  return;
 put_file("t.c", "old\n", 02775);
 f = window_of_file_with_mode(02775);
 set_rows("new\n");
 fakechmod_set(FAKECHMOD_REFUSE, 07777);
 chk_int("write", save_window(f, "t.c", &o), 0);
 fakechmod_set(FAKECHMOD_OFF, 0);
 chk_int("the bit is off", mode_of("t.c"), 0775);
 save_sentence(&o, say, sizeof say);
 chk_has("the bit is named", say, "the setgid bit was not kept: Operation not permitted");
}

/* @scenario SC-write-owner-E-03: a block written over a sticky file whose
   mode the user cannot change keeps the bit, and says so.  Red when the
   owner never reads the bits that stayed. */
static void test_a_block_write_onto_a_sticky_file_that_cannot_be_chmoded_says_it_stays(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char p[512], say[1024];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISVTX), "a copy says a bit stays",
                   "this user cannot set the sticky bit in the test folder"))
  return;
 put_file("blk.txt", "old\n", 01664);
 f = window_of_file_with_mode(0664);
 set_rows("block\n");
 path_of(p, sizeof p, "blk.txt");
 fakechmod_set(FAKECHMOD_REFUSE, 07777);
 chk_int("block write with a backup", wpe_write_range(f, p, 0, 0, 1, 1, 1, &o), 0);
 fakechmod_set(FAKECHMOD_OFF, 0);
 chk_int("written", o.status, WPE_WRITE_WRITTEN);
 chk_int("the bit stays", mode_of("blk.txt"), 01664);
 save_sentence(&o, say, sizeof say);
 chk_int("it says so",
         strcmp(say, "blk.txt written, but it keeps the sticky bit: Operation not permitted."), 0);
}

/* @scenario SC-write-owner-E-04: a copy over a file of ours that has a
   setgid or a sticky bit gets no bit and says nothing.  Guard: the nine bits
   of the file already match the window's, and the call that gives them is the
   one that takes the special bits off, so skipping it as "nothing to do" turns
   this row red. */
static void test_a_copy_over_02644_and_over_01644_gets_no_bit(void)
{
 static const mode_t modes[] = { 02644, 01644 };
 WpeWriteOutcome o;
 FENSTER *f;
 char p[512], say[1024];
 size_t i;

 for (i = 0; i < sizeof modes / sizeof modes[0]; i++)
 {
  make_dir();
  if (!test_premise(test_premise_special_bits_settable(dir, modes[i] & 07000),
                    "a copy over 02644 and 01644 gets no bit",
                    "this user cannot set the bit in the test folder"))
   continue;
  put_file("blk.txt", "old\n", modes[i]);
  f = window_of_file_with_mode(0644);
  set_rows("block\n");
  path_of(p, sizeof p, "blk.txt");
  chk_int("block write", wpe_write_range(f, p, 0, 0, 1, 1, 1, &o), 0);
  chk_int("written", o.status, WPE_WRITE_WRITTEN);
  chk_int("the copy has no bit", mode_of("blk.txt"), 0644);
  save_sentence(&o, say, sizeof say);
  chk_int("it says nothing", (long)strlen(say), 0);
 }
}

/* @scenario SC-write-owner-E-05: the overwrite question says a bit stays on a
   file the user does not own.  Played by the stat fake (the file looks owned
   by another user), which a row proves took and skips by name otherwise;
   root may change any mode, so there is no such file for it. */
static void test_the_question_says_a_bit_stays_on_a_file_we_do_not_own(void)
{
 mode_t drops = 0, stays = 0;
 char p[512];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISVTX), "the question says a bit stays",
                   "this user cannot set the sticky bit in the test folder"))
  return;
 put_file("s.c", "x\n", 01644);
 path_of(p, sizeof p, "s.c");
 wpe_write_overwrite_bits(p, NULL, &drops, &stays);
 chk_int("a file of ours: the bit drops", (long)drops, (long)S_ISVTX);
 chk_int("a file of ours: nothing stays", (long)stays, 0);
 if (!test_observation(geteuid() != 0, "the question says a bit stays",
                       "root may change any mode: no file it does not own"))
  return;
 owner_stat_fake_arm("s.c");
 if (!test_observation(owner_stat_fake_takes("s.c"), "the question says a bit stays",
                       "stat is not interposable in this C library"))
 {
  owner_stat_fake_disarm();
  return;
 }
 wpe_write_overwrite_bits(p, NULL, &drops, &stays);
 owner_stat_fake_disarm();
 chk_int("another user's file: nothing drops", (long)drops, 0);
 chk_int("another user's file: the bit stays", (long)stays, (long)S_ISVTX);
 owner_stat_fake_disarm();
}

/* @scenario SC-write-owner-E-06: of a file the user does not own, the system
   clears setuid and setgid on a write and only the sticky bit stays, so the
   question says the first two drop and only the third stays.  Red when it
   predicts every bit stays (or every bit drops) by the owner alone. */
static void test_the_question_for_a_file_we_do_not_own_says_only_sticky_stays(void)
{
 static const mode_t modes[] = { 06644, 05644, 07644 };
 mode_t drops = 0, stays = 0;
 char p[512];
 size_t i;

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, 07000), "only the sticky bit stays",
                   "this user cannot set the three special bits in the test folder"))
  return;
 if (!test_observation(geteuid() != 0, "only the sticky bit stays",
                       "root may change any mode: no file it does not own"))
  return;
 path_of(p, sizeof p, "o.c");
 for (i = 0; i < sizeof modes / sizeof modes[0]; i++)
 {
  put_file("o.c", "x\n", modes[i]);
  owner_stat_fake_arm("o.c");
  if (!test_observation(owner_stat_fake_takes("o.c"), "only the sticky bit stays",
                        "stat is not interposable in this C library"))
  {
   owner_stat_fake_disarm();
   return;
  }
  wpe_write_overwrite_bits(p, NULL, &drops, &stays);
  owner_stat_fake_disarm();
  chk_int("setuid and setgid drop", (long)drops, (long)(modes[i] & (S_ISUID | S_ISGID)));
  chk_int("only sticky stays", (long)stays, (long)(modes[i] & S_ISVTX));
 }
}

/* @scenario SC-write-owner-E-07: a copy over a setgid file whose mode the user
   cannot change gets no bit from the kernel, and says nothing.  The twin of
   E-02 for a copy: the bit that stays is read from the file, not taken from the
   refusal.  Skips by name where the kernel keeps setgid through a write. */
static void test_a_copy_over_a_setgid_file_the_kernel_cleared_says_nothing(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char p[512], say[1024];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "a cleared bit on a copy is not said to stay",
                   "this user cannot set setgid in the test folder"))
  return;
 if (!test_observation(the_system_clears_setgid_on_write(), "a cleared bit on a copy is not said to stay",
                       "this user keeps setgid through a write (CAP_FSETID)"))
  return;
 put_file("blk.txt", "old\n", 02775);
 f = window_of_file_with_mode(0775);
 set_rows("block\n");
 path_of(p, sizeof p, "blk.txt");
 fakechmod_set(FAKECHMOD_REFUSE, 07777);
 chk_int("block write", wpe_write_range(f, p, 0, 0, 1, 1, 1, &o), 0);
 fakechmod_set(FAKECHMOD_OFF, 0);
 chk_int("the kernel cleared the bit", mode_of("blk.txt"), 0775);
 chk_int("nothing stays", (long)o.file_stays.lost, 0);
 save_sentence(&o, say, sizeof say);
 chk_int("it says nothing", (long)strlen(say), 0);
}

/* @scenario SC-write-owner-E-08: a save that lost one bit and kept another
   names only the lost one.  03775 with every call refused: the kernel clears
   setgid, sticky stays; the sentence is about setgid and does not mention
   sticky.  Red when the record names every bit that was to be given back. */
static void test_a_save_that_lost_one_bit_and_kept_another_names_only_the_lost(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char say[1024];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID | S_ISVTX), "a partial loss names one bit",
                   "this user cannot set setgid and sticky in the test folder"))
  return;
 if (!test_observation(the_system_clears_setgid_on_write(), "a partial loss names one bit",
                       "this user keeps setgid through a write (CAP_FSETID)"))
  return;
 put_file("t.c", "old\n", 03775);
 f = window_of_file_with_mode(03775);
 set_rows("new\n");
 fakechmod_set(FAKECHMOD_REFUSE, 07777);
 chk_int("write", save_window(f, "t.c", &o), 0);
 fakechmod_set(FAKECHMOD_OFF, 0);
 chk_int("sticky is still on", mode_of("t.c"), 01775);
 chk_int("only setgid is recorded", (long)o.file_kept.lost, (long)S_ISGID);
 save_sentence(&o, say, sizeof say);
 chk_int("the sentence names setgid",
         strcmp(say, "t.c written, but the setgid bit was not kept: Operation not permitted."), 0);
}

/* @scenario SC-write-owner-E-09: with no descriptor to give the bits back on,
   the file is still read: a bit the text left on the file is not said lost.
   The duplication of the descriptor for the bits is made to fail.  A sticky file is
   not touched by the write, so the save says nothing. */
static void test_a_save_with_no_descriptor_does_not_call_a_standing_bit_lost(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char say[1024];
 int took;

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISVTX), "no descriptor, a bit that stands",
                   "this user cannot set the sticky bit in the test folder"))
  return;
 put_file("t.c", "old\n", 01664);
 f = window_of_file_with_mode(01664);
 set_rows("new\n");
 fakechmod_set(FAKECHMOD_REFUSE, 07777); /* the nine-bit call before the text is refused too */
 dup_fails_at = 2; /* the first duplication is the backup's, the second is the save's */
 chk_int("write", save_window(f, "t.c", &o), 0);
 took = (dup_fails_at == 0); /* the second duplication was reached and failed */
 dup_fails_at = 0;
 fakechmod_set(FAKECHMOD_OFF, 0);
 if (!test_observation(took, "no descriptor, a bit that stands",
                       "fcntl is not interposable for F_DUPFD_CLOEXEC in this C library"))
  return;
 chk_int("written", o.status, WPE_WRITE_WRITTEN);
 chk_int("sticky is on", mode_of("t.c"), 01664);
 save_sentence(&o, say, sizeof say);
 chk_int("it says nothing", (long)strlen(say), 0);
}

/* @scenario SC-write-owner-E-10: a restore after a failed write says nothing
   of a bit the failed write left on the file.  A sticky file is not touched by
   the write; the restore's call for the bits is refused; the file still has
   the bit, so the box does not say it was lost. */
static void test_a_restore_does_not_call_a_standing_bit_lost(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char *old = big(1024, 'o'), say[2048];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISVTX), "a restore leaves a standing bit unsaid",
                   "this user cannot set the sticky bit in the test folder"))
  return;
 put_file("t.c", old, 01664);
 f = window_of_file_with_mode(0664);
 fakechmod_set(FAKECHMOD_REFUSE, 07777);
 write_past_limit(f, "t.c", &o, 2048, 4096);
 fakechmod_set(FAKECHMOD_OFF, 0);
 chk_int("restored", o.restore_err, 0);
 chk_int("sticky is on", mode_of("t.c"), 01664);
 chk_int("no bit is recorded lost", (long)o.restore_kept.lost, 0);
 wpe_write_sentence(say, sizeof say, &o, WPE_WRITE_THEN_KEPT);
 chk_int("the box does not name sticky", strstr(say, "sticky") == NULL, 1);
}

/* @scenario SC-write-owner-E-14: a stream names a bit the file keeps only when
   the file still has it when the stream ends.  The nine-bit call is refused at
   the open, so the bit is on the file then; the bit is taken off by name
   before the close (as a system that clears it at write time would), and the
   sentence must say nothing.  Red when the open-time read is what is said. */
static void test_a_stream_does_not_name_a_bit_the_file_lost_before_the_close(void)
{
 WpeWriteOutcome o;
 char p[512], say[1024];
 FILE *fp;

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISVTX), "a stream names only a bit that stands",
                   "this user cannot set the sticky bit in the test folder"))
  return;
 put_file("s.txt", "old\n", 01664);
 path_of(p, sizeof p, "s.txt");
 fakechmod_set(FAKECHMOD_REFUSE, 07777);
 fp = wpe_write_stream_open(p, &o);
 fakechmod_set(FAKECHMOD_OFF, 0);
 chk_int("the stream opens", fp != NULL, 1);
 if (fp == NULL)
  return;
 chk_int("the open found the bit staying", (long)o.file_stays.lost, (long)S_ISVTX);
 chk_int("the bit is taken off by name", chmod(p, 0664), 0);
 fputs("streamed\n", fp);
 chk_int("the stream closes", wpe_write_stream_close(fp, &o), 0);
 chk_int("the file has no bit", mode_of("s.txt"), 0664);
 chk_int("nothing stays", (long)o.file_stays.lost, 0);
 (void)wpe_write_sentence(say, sizeof say, &o, WPE_WRITE_THEN_KEPT);
 chk_int("it says nothing", (long)strlen(say), 0);
}

/* @scenario SC-write-owner-E-15: a save whose file cannot be read back after
   the text says the bit "may not have been kept": not silence, and not a flat
   "was not kept".  The call for the bit is refused and the fstat that would
   say what the file has fails (the fstat fake).  Red when the read-back says
   nothing on an fstat that fails. */
static void test_a_save_whose_file_cannot_be_read_back_says_it_may_not_have_kept_the_bit(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char say[1024];
 int took;

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISVTX), "an unreadable file: the bit may not be kept",
                   "this user cannot set the sticky bit in the test folder"))
  return;
 put_file("t.c", "old\n", 01664);
 f = window_of_file_with_mode(01664);
 set_rows("new\n");
 fakechmod_set(FAKECHMOD_REFUSE, 07777);
 fstat_fails_after_refusal = 1;
 chk_int("write", save_window(f, "t.c", &o), 0);
 took = fstat_fake_off();
 fakechmod_set(FAKECHMOD_OFF, 0);
 if (!test_observation(took, "an unreadable file: the bit may not be kept",
                       "fstat is not interposable in this C library"))
  return;
 chk_int("written", o.status, WPE_WRITE_WRITTEN);
 chk_int("the record is unsure", o.file_kept.unsure, 1);
 save_sentence(&o, say, sizeof say);
 chk_has("the sentence says it may not have been kept", say, "sticky bit may not have been kept");
 chk_lacks("the sentence does not say it was lost", say, "was not kept");
}

/* @scenario SC-write-owner-E-16: with no descriptor to give the bits back on
   and no way to read the file either, the same words.  The duplication of the
   descriptor fails (as in E-09) and so does every fstat.  Red when the no-
   descriptor path says nothing when the file cannot be read. */
static void test_a_save_with_no_descriptor_and_no_read_says_it_may_not_have_kept_the_bit(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char say[1024];
 int took, dup_took;

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISVTX), "no descriptor and no read: the bit may not be kept",
                   "this user cannot set the sticky bit in the test folder"))
  return;
 put_file("t.c", "old\n", 01664);
 f = window_of_file_with_mode(01664);
 set_rows("new\n");
 fakechmod_set(FAKECHMOD_REFUSE, 07777);
 fstat_fails_after_refusal = 1;
 dup_fails_at = 2; /* the first duplication is the backup's, the second is the save's */
 chk_int("write", save_window(f, "t.c", &o), 0);
 dup_took = (dup_fails_at == 0);
 dup_fails_at = 0;
 took = fstat_fake_off();
 fakechmod_set(FAKECHMOD_OFF, 0);
 if (!test_observation(took && dup_took, "no descriptor and no read: the bit may not be kept",
                       "fcntl or fstat is not interposable in this C library"))
  return;
 chk_int("written", o.status, WPE_WRITE_WRITTEN);
 save_sentence(&o, say, sizeof say);
 chk_has("the sentence says it may not have been kept", say, "sticky bit may not have been kept");
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

/* ---- the copies the editor names itself: .ASV, .ESV, the post-edit copy ---- */

/* Is this name a link, itself and not what it leads to? */
static int name_is_link(const char *path)
{
 struct stat st;

 return(lstat(path, &st) == 0 && S_ISLNK(st.st_mode));
}

/* The window of t.c whose copy t.c.ASV is written, with this mode. */
static FENSTER *window_for_a_copy(mode_t mode)
{
 FENSTER *f = window_of_file_with_mode(mode);

 f->dirct = dir;
 f->datnam = "t.c";
 set_rows("copy\n");
 return(f);
}

/* @scenario SC-write-owner-C-01: a name the editor chose that is a link is
   never written through.  The link leads to a program with a setgid bit:
   writing through it would strip the bit and replace the program's text, in
   silence. */
static void test_a_chosen_name_that_is_a_link_is_left_alone_and_said(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char p[512], q[512], buf[256], say[1024];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "a link at a chosen name is left alone",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("prog", "program\n", 02755);
 path_of(p, sizeof p, "t.c.ASV");
 path_of(q, sizeof q, "prog");
 if (symlink(q, p) != 0)
  perror("symlink");
 f = window_for_a_copy(0644);
 chk_int("the copy is refused", wpe_write_copy(f, p, &o), -1);
 chk_int("it says something else is in the name", o.planted, 1);
 chk_int("nothing was written", o.status, WPE_WRITE_FAILED);
 wpe_write_sentence(say, sizeof say, &o, WPE_WRITE_THEN_NOTHING_MORE);
 chk_has("the sentence", say, "something that is not a copy of ours is in its place.  Nothing was written.");
 chk_has("the program keeps its text", get_file("prog", buf, sizeof buf), "program\n");
 chk_int("the program keeps its setgid bit", mode_of("prog"), 02755);
 chk_int("the link is still a link", name_is_link(p), 1);
}

/* @scenario SC-write-owner-C-02: a link that leads nowhere is not followed
   to make the file it names. */
static void test_a_dangling_chosen_name_creates_nothing_elsewhere(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char p[512], target[512];

 make_dir();
 path_of(target, sizeof target, "elsewhere.txt");
 path_of(p, sizeof p, "t.c.ASV");
 if (symlink(target, p) != 0)
  perror("symlink");
 f = window_for_a_copy(0644);
 chk_int("the copy is refused", wpe_write_copy(f, p, &o), -1);
 chk_int("it says something else is in the name", o.planted, 1);
 chk_int("the file the link names was not made", mode_of("elsewhere.txt"), -1);
}

/* @scenario SC-write-owner-C-03: an old copy of ours is replaced by a new
   file, not rewritten in place, so a hard link made to it earlier keeps the
   old text. */
static void test_an_old_copy_of_ours_is_replaced_by_a_new_inode(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 struct stat before, after, held;
 char p[512], keep[512], buf[256];

 make_dir();
 put_file("t.c.ASV", "old\n", 0644);
 path_of(p, sizeof p, "t.c.ASV");
 path_of(keep, sizeof keep, "old.keep");
 chk_int("the old copy is there", stat(p, &before), 0);
 /* A second name keeps the old inode alive, so its number cannot be given
    to the new file by a file system that reuses numbers. */
 chk_int("a second name for the old copy", link(p, keep), 0);
 f = window_for_a_copy(0640);
 chk_int("the copy is written", wpe_write_copy(f, p, &o), 0);
 chk_int("written", o.status, WPE_WRITE_WRITTEN);
 chk_has("the new text", get_file("t.c.ASV", buf, sizeof buf), "copy\n");
 chk_has("the second name keeps the old text", get_file("old.keep", buf, sizeof buf), "old\n");
 chk_int("the copy has the window's nine bits", mode_of("t.c.ASV"), 0640);
 chk_int("stat after", stat(p, &after), 0);
 chk_int("stat of the second name", stat(keep, &held), 0);
 chk_int("the second name is still the old file",
         held.st_ino == before.st_ino && held.st_dev == before.st_dev, 1);
 chk_int("it is a new inode", after.st_ino != held.st_ino, 1);
 chk_has("the path written is named", o.path, "t.c.ASV");
}

/* @scenario SC-write-owner-C-04: another user's file at the name is left
   alone and said.  Played by the owner fake. */
static void test_another_users_copy_is_left_alone(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char p[512], buf[256];

 make_dir();
 put_file("t.c.ASV", "theirs\n", 0644);
 owner_fake_arm("t.c.ASV");
 if (!test_observation(owner_fake_takes("t.c.ASV"), "another user's copy is left alone",
                       "lstat is not interposable in this C library"))
 {
  owner_fake_disarm();
  return;
 }
 path_of(p, sizeof p, "t.c.ASV");
 f = window_for_a_copy(0644);
 chk_int("the copy is refused", wpe_write_copy(f, p, &o), -1);
 owner_fake_disarm();
 chk_int("it says something else is in the name", o.planted, 1);
 chk_has("their text is untouched", get_file("t.c.ASV", buf, sizeof buf), "theirs\n");
}

/* @scenario SC-write-owner-C-05: the copy of a private file is private from
   its first byte (guard: it holds with and without the fix). */
static void test_a_copy_of_a_private_file_is_private_from_its_first_byte(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char p[512];

 make_dir();
 path_of(p, sizeof p, "t.c.ASV");
 f = window_for_a_copy(0600);
 write_shared_bits = 0;
 watch_writes = 1;
 chk_int("the copy is written", wpe_write_copy(f, p, &o), 0);
 watch_writes = 0;
 chk_int("no byte went in while others could read the file", write_shared_bits, 0);
 chk_int("the copy is private", mode_of("t.c.ASV"), 0600);
}

/* @scenario SC-write-owner-C-06: a copy removed at close: a link at the name
   and another user's file stay, a regular file of ours goes. */
static void test_copy_remove_leaves_a_link_and_another_users_file_alone(void)
{
 char p[512], q[512];
 WpeFileIdentity own, nobody;

 make_dir();
 memset(&nobody, 0, sizeof nobody);
 put_file("victim", "keep me\n", 0644);
 chk_int("the victim's identity", wpe_write_identity_of_name(dir, "victim", &own), 1);
 path_of(p, sizeof p, "t.c.ASV");
 path_of(q, sizeof q, "victim");
 if (symlink(q, p) != 0)
  perror("symlink");
 wpe_write_copy_remove(p, &own);
 chk_int("the link stays", name_is_link(p), 1);
 chk_int("what it leads to stays", mode_of("victim"), 0644);
 unlink(p);
 put_file("t.c.ASV", "theirs\n", 0644);
 owner_fake_arm("t.c.ASV");
 chk_int("its identity", wpe_write_identity_of_name(dir, "t.c.ASV", &own), 1);
 if (test_observation(owner_fake_takes("t.c.ASV"), "another user's copy stays at close",
                      "lstat is not interposable in this C library"))
 {
  wpe_write_copy_remove(p, &own);
  chk_int("another user's copy stays", mode_of("t.c.ASV"), 0644);
 }
 owner_fake_disarm();
 wpe_write_copy_remove(p, &nobody);
 chk_int("a copy this window did not write stays", mode_of("t.c.ASV"), 0644);
 wpe_write_copy_remove(p, NULL);
 chk_int("no identity, nothing removed", mode_of("t.c.ASV"), 0644);
 put_file("other", "x\n", 0644);
 chk_int("another file's identity", wpe_write_identity_of_name(dir, "other", &nobody), 1);
 wpe_write_copy_remove(p, &nobody);
 chk_int("a copy of another identity stays", mode_of("t.c.ASV"), 0644);
 wpe_write_copy_remove(p, &own);
 chk_int("the copy this window wrote goes", mode_of("t.c.ASV"), -1);
}

/* @scenario SC-write-owner-C-07: the window between clearing an old copy and
   making the new one.  Another program puts a hard link to a file of its own
   at the name just after the old copy is unlinked; the new file is made with
   O_EXCL and O_NOFOLLOW, so it is refused and the file keeps its text. */
static void test_a_name_planted_between_the_clear_and_the_create_is_refused(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char p[512], buf[256];

 make_dir();
 put_file("t.c.ASV", "old\n", 0644);
 put_file("victim", "victim text\n", 0644);
 path_of(p, sizeof p, "t.c.ASV");
 path_of(plant_at, sizeof plant_at, "t.c.ASV");
 path_of(plant_from, sizeof plant_from, "victim");
 f = window_for_a_copy(0644);
 chk_int("the copy is refused", wpe_write_copy(f, p, &o), -1);
 plant_at[0] = '\0';
 chk_int("it says something else is in the name", o.planted, 1);
 chk_has("the victim keeps its text", get_file("victim", buf, sizeof buf), "victim text\n");
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

/* ---- class F: the file the window read, not the name it holds ---- */

/* Another file takes the name, as vim (backupcopy=no), sed -i, git checkout
   and rsync do: a new inode renamed over the old one. */
static void another_file_takes_the_name(const char *name, const char *text, mode_t mode)
{
 char from[512], to[512];

 put_file("incoming", text, mode);
 path_of(from, sizeof from, "incoming");
 path_of(to, sizeof to, name);
 chk_int("rename the other file over the name", rename(from, to), 0);
}

/* Does the window hold exactly the file the name leads to now? */
static int window_holds_the_file_at(const FENSTER *f, const char *name)
{
 char p[512];
 WpeSeen now;

 path_of(p, sizeof p, name);
 if (!f->file_id.known || wpe_seen_look_name(p, 1, &now) != 0)
  return(0);
 return(wpe_write_identity_is(&f->file_id, &now));
}

static int descriptors_open(void)
{
 int fd = dup(0), n;

 n = fd;
 close(fd);
 return(n);
}

/* @scenario SC-write-owner-F-01: a Save writes into the inode the window
   read, and keeps the bits it has. */
static void test_a_save_into_the_inode_the_window_read_keeps_the_bits(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char buf[256];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "the same inode keeps the bits",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("t.c", "old\n", 02755);
 f = window_of_file_with_mode(02755);
 window_reads(f, "t.c");
 set_rows("new\n");
 chk_int("write", save_window(f, "t.c", &o), 0);
 chk_has("file written", get_file("t.c", buf, sizeof buf), "new\n");
 chk_int("the saved file keeps setgid", mode_of("t.c"), 02755);
 chk_int("nothing was asked", question_count, 0);
 chk_int("the window still holds it", window_holds_the_file_at(f, "t.c"), 1);
}

/* @scenario SC-write-owner-F-02: a hard link to the window's file is its own
   file, in the question and in the write. */
static void test_a_hard_link_to_the_windows_file_is_its_own_file(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char own[512], link_to[512];
 mode_t drops, stays;

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "a hard link is its own file",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("t.c", "old\n", 02755);
 path_of(own, sizeof own, "t.c");
 path_of(link_to, sizeof link_to, "h.c");
 chk_int("hard link", link(own, link_to), 0);
 f = window_of_file_with_mode(02755);
 window_reads(f, "t.c");
 set_rows("new\n");
 wpe_write_overwrite_bits(link_to, &f->file_id, &drops, &stays);
 chk_int("the question names no bit that drops", (long)drops, 0);
 chk_int("the question names no bit that stays", (long)stays, 0);
 chk_int("Save As onto the link", save_window_as(f, dir, "h.c", dir, "t.c"), WPE_WRITE_WRITTEN);
 chk_int("the link keeps setgid", mode_of("h.c"), 02755);
 chk_int("nothing was asked", question_count, 0);
}

/* @scenario SC-write-owner-F-03: a plain file of the same owner took the name:
   the Save asks, and Yes writes it as a copy and takes the new file. */
static void test_a_replaced_plain_file_is_asked_about_and_yes_writes_a_copy(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char buf[256];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "a replaced plain file is asked about",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("t.c", "old\n", 02755);
 f = window_of_file_with_mode(02755);
 window_reads(f, "t.c");
 another_file_takes_the_name("t.c", "theirs\n", 0644);
 set_rows("mine\n");
 question_answer = 'Y';
 chk_int("write", save_window(f, "t.c", &o), 0);
 chk_int("one question", question_count, 1);
 chk_has("the question", question_text, "t.c was replaced on disk since this window read it. Overwrite it?");
 chk_has("the text is on disk", get_file("t.c", buf, sizeof buf), "mine\n");
 chk_int("a copy has no special bit", mode_of("t.c") & 07000, 0);
 chk_int("the window takes the new file", window_holds_the_file_at(f, "t.c"), 1);
 set_rows("again\n");
 question_count = 0;
 chk_int("the next Save asks nothing", save_window(f, "t.c", &o), 0);
 chk_int("no second question", question_count, 0);
}

/* @scenario SC-write-owner-F-04: No and Escape write nothing. */
static void test_no_and_escape_on_the_question_write_nothing(void)
{
 static const int answers[] = { 'N', WPE_ESC };
 WpeWriteOutcome o;
 FENSTER *f;
 char buf[256], say[1024], bakp[512];
 struct stat st;
 size_t i;

 make_dir();
 for (i = 0; i < sizeof answers / sizeof answers[0]; i++)
 {
  put_file("t.c", "old\n", 0644);
  f = window_of_file_with_mode(0644);
  window_reads(f, "t.c");
  another_file_takes_the_name("t.c", "theirs\n", 0644);
  set_rows("mine\n");
  question_answer = answers[i];
  chk_int("declined", save_window(f, "t.c", &o), -1);
  chk_int("status", o.status, WPE_WRITE_DECLINED);
  chk_int("asked", question_count, 1);
  chk_has("their text stays", get_file("t.c", buf, sizeof buf), "theirs\n");
  path_of(bakp, sizeof bakp, "t.bak");
  chk_int("no backup", stat(bakp, &st), -1);
  save_sentence(&o, say, sizeof say);
  chk_int("nothing to say", (long)strlen(say), 0);
  chk_int("the window keeps its old file", window_holds_the_file_at(f, "t.c"), 0);
 }
}

/* @scenario SC-write-owner-F-05: a replacement with a special bit is refused,
   untouched, and Save As is the way out. */
static void test_a_replaced_file_with_a_special_bit_is_refused(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char buf[256], say[1024];
 int open_before;

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISUID), "a setuid replacement is refused",
                   "this user cannot set setuid in the test folder"))
  return;
 put_file("t.c", "old\n", 0644);
 f = window_of_file_with_mode(0644);
 window_reads(f, "t.c");
 another_file_takes_the_name("t.c", "theirs\n", 04755);
 set_rows("mine\n");
 open_before = descriptors_open();
 chk_int("refused", save_window(f, "t.c", &o), -1);
 chk_int("failed", o.status, WPE_WRITE_FAILED);
 chk_int("says it is another file", o.other_file, 1);
 chk_int("nothing was asked", question_count, 0);
 chk_int("no descriptor left open", descriptors_open(), open_before);
 save_sentence(&o, say, sizeof say);
 chk_has("the sentence", say, "the name leads to a different file than the one this window read");
 chk_has("nothing written", say, "Nothing was written.");
 chk_has("the way out", say, "Save As");
 chk_has("their text is untouched", get_file("t.c", buf, sizeof buf), "theirs\n");
 chk_int("their setuid is untouched", mode_of("t.c"), 04755);
 chk_int("no backup", mode_of("t.bak"), -1);
 chk_int("Save As over the same name works", save_window_as(f, dir, "t.c", dir, "t.c"), WPE_WRITE_WRITTEN);
 chk_has("the text is on disk", get_file("t.c", buf, sizeof buf), "mine\n");
 chk_int("a copy has no special bit", mode_of("t.c") & 07000, 0);
 chk_int("the window takes the file", window_holds_the_file_at(f, "t.c"), 1);
}

/* @scenario SC-write-owner-F-06: a replacement owned by another user is
   refused (the owner is played by a hook: a test cannot give a file away). */
static void test_a_replaced_file_of_another_owner_is_refused(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char buf[256];

 make_dir();
 put_file("t.c", "old\n", 0644);
 f = window_of_file_with_mode(0644);
 window_reads(f, "t.c");
 another_file_takes_the_name("t.c", "theirs\n", 0644);
 set_rows("mine\n");
 setenv("XWPE_TEST_FAIL", "write-other-owner", 1);
 chk_int("refused", save_window(f, "t.c", &o), -1);
 unsetenv("XWPE_TEST_FAIL");
 chk_int("says it is another file", o.other_file, 1);
 chk_int("nothing was asked", question_count, 0);
 chk_has("their text is untouched", get_file("t.c", buf, sizeof buf), "theirs\n");
}

/* @scenario SC-write-owner-F-07: a window that read no file saves as a copy. */
static void test_a_window_that_read_no_file_saves_as_a_copy(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char say[1024];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "a window with no file saves as a copy",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("t.c", "program\n", 02755);
 f = window_of_file_with_mode(0755);
 window_reads_nothing = 1;
 set_rows("text\n");
 chk_int("write", save_window(f, "t.c", &o), 0);
 chk_int("a copy has no special bit", mode_of("t.c"), 0755);
 chk_int("nothing was asked", question_count, 0);
 save_sentence(&o, say, sizeof say);
 chk_int("nothing to say", (long)strlen(say), 0);
}

/* @scenario SC-write-owner-F-08: a file that was removed is made again and
   the window takes the new file. */
static void test_a_removed_file_is_made_again_and_the_window_takes_it(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char p[512];
 WpeSeen now;

 make_dir();
 put_file("t.c", "old\n", 0644);
 f = window_of_file_with_mode(0644);
 window_reads(f, "t.c");
 path_of(p, sizeof p, "t.c");
 chk_int("remove", unlink(p), 0);
 set_rows("new\n");
 chk_int("write", save_window(f, "t.c", &o), 0);
 chk_int("nothing was asked", question_count, 0);
 chk_int("the outcome names the new file", o.written.known &&
         wpe_seen_look_name(p, 1, &now) == 0 && wpe_write_identity_is(&o.written, &now), 1);
 chk_int("the window takes it", window_holds_the_file_at(f, "t.c"), 1);
}

/* @scenario SC-write-owner-F-09: Save As onto the name of a replaced file is a
   copy and asks nothing (the Save As box asked already). */
static void test_save_as_onto_a_replaced_name_is_a_copy(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char buf[256];

 make_dir();
 put_file("t.c", "old\n", 0644);
 f = window_of_file_with_mode(0644);
 window_reads(f, "t.c");
 another_file_takes_the_name("t.c", "theirs\n", 0644);
 set_rows("mine\n");
 chk_int("Save As", save_window_as(f, dir, "t.c", dir, "t.c"), WPE_WRITE_WRITTEN);
 chk_int("nothing was asked", question_count, 0);
 chk_has("the text is on disk", get_file("t.c", buf, sizeof buf), "mine\n");
 chk_int("the window takes the file", window_holds_the_file_at(f, "t.c"), 1);
}

/* @scenario SC-write-owner-F-10: Ctrl-F9 does not make a file executable that
   took the script's name after the window read it. */
static void test_ctrl_f9_on_a_replaced_script_is_refused(void)
{
 WpeModeKept k;
 FENSTER *f;
 char p[512];

 make_dir();
 put_file("s.sh", "echo\n", 0644);
 f = window_of_file_with_mode(0644);
 window_reads(f, "s.sh");
 path_of(p, sizeof p, "s.sh");
 chk_int("the script itself is made runnable", wpe_write_add_perm(p, &f->file_id, S_IXUSR, &k), 0);
 chk_int("x added", mode_of("s.sh"), 0744);
 another_file_takes_the_name("s.sh", "theirs\n", 0644);
 chk_int("refused", wpe_write_add_perm(p, &f->file_id, S_IXUSR, &k) != 0, 1);
 chk_int("says it was replaced", k.replaced, 1);
 chk_int("their file is not made executable", mode_of("s.sh"), 0644);
}

/* @scenario SC-write-owner-F-14: a link to /dev/null took the name: the Save is
   refused, the text stays in the window, and nothing is written into it. */
static void test_a_device_under_the_name_is_refused(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char p[512];
 WpeFileIdentity was;
 int open_before;

 make_dir();
 put_file("t.c", "old\n", 0644);
 f = window_of_file_with_mode(0644);
 window_reads(f, "t.c");
 was = f->file_id;
 path_of(p, sizeof p, "t.c");
 chk_int("remove", unlink(p), 0);
 chk_int("link to the null device", symlink("/dev/null", p), 0);
 f->save = 1;
 set_rows("mine\n");
 open_before = descriptors_open();
 chk_int("refused", save_window(f, "t.c", &o), -1);
 chk_int("failed", o.status, WPE_WRITE_FAILED);
 chk_int("says it is another file", o.other_file, 1);
 chk_int("nothing was asked", question_count, 0);
 chk_int("no descriptor left open", descriptors_open(), open_before);
 chk_int("the window still reads as modified", f->save, 1);
 chk_int("the window still holds the file it read",
         f->file_id.known && wpe_seen_judge(&was, &f->file_id) == WPE_SEEN_SAME, 1);
}

/* @scenario SC-write-owner-F-15: the question is asked with no write open:
   SIGXFSZ is not ignored while the box is up, and what the window holds after
   the question is what is written. */
static void test_the_question_is_asked_outside_the_write(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char buf[256];

 make_dir();
 put_file("t.c", "old\n", 0644);
 f = window_of_file_with_mode(0644);
 window_reads(f, "t.c");
 another_file_takes_the_name("t.c", "theirs\n", 0644);
 set_rows("before\n");
 question_changes_rows = "changed\n";
 chk_int("write", save_window(f, "t.c", &o), 0);
 chk_int("asked once", question_count, 1);
 chk_int("the guard was not held during the question", question_saw_xfsz_ignored, 0);
 chk_has("the window's text after the question is on disk", get_file("t.c", buf, sizeof buf), "changed\n");
}

/* A second name for a file keeps its inode alive, so that a file made later
   cannot be given the same inode number and look like it. */
static void keep_alive(const char *name, const char *second_name)
{
 char from[512], to[512];

 path_of(from, sizeof from, name);
 path_of(to, sizeof to, second_name);
 chk_int("keep the inode alive", link(from, to), 0);
}

/* A third program puts yet another file under the name while the box is up. */
static void a_third_file_takes_the_name(void)
{
 another_file_takes_the_name("t.c", "third\n", 0644);
}

/* @scenario SC-write-owner-F-18: Yes reaches the file the user was asked
   about and no other: a file that took the name while the box was up is
   refused, untouched. */
static void test_yes_does_not_reach_a_file_that_took_the_name_during_the_question(void)
{
 WpeWriteOutcome o;
 WpeFileIdentity original;
 FENSTER *f;
 char buf[256], say[1024];

 make_dir();
 put_file("t.c", "old\n", 0644);
 f = window_of_file_with_mode(0644);
 window_reads(f, "t.c");
 original = f->file_id;
 keep_alive("t.c", "old.keep");
 another_file_takes_the_name("t.c", "theirs\n", 0644);
 keep_alive("t.c", "theirs.keep");
 set_rows("mine\n");
 question_during = a_third_file_takes_the_name;
 chk_int("refused", save_window(f, "t.c", &o), -1);
 chk_int("failed", o.status, WPE_WRITE_FAILED);
 chk_int("says it is another file", o.other_file, 1);
 chk_int("asked once", question_count, 1);
 chk_has("the third file is untouched", get_file("t.c", buf, sizeof buf), "third\n");
 chk_int("the window keeps the file it read", window_holds_the_file_at(f, "t.c"), 0);
 chk_int("and still holds the original inode, not the second or the third",
         f->file_id.known && wpe_seen_judge(&original, &f->file_id) == WPE_SEEN_SAME, 1);
 chk_int("the refusal says the name changed during the question", o.changed_while_asked, 1);
 save_sentence(&o, say, sizeof say);
 chk_has("the sentence says the name changed again while the user was asked", say,
         "the name changed again while you were being asked");
 chk_has("nothing was written", say, "Nothing was written.");
 chk_has("the way out", say, "Use Save As");
}

int posix_openpt(int flags);
int grantpt(int fd);
int unlockpt(int fd);
char *ptsname(int fd);

/* @scenario SC-write-owner-F-19: an object the user owns that is not a regular
   file (the slave of a pty) under the name is refused by its type alone. */
static void test_a_user_owned_non_regular_file_under_the_name_is_refused(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char p[512], *slave_name;
 int master, slave;

 make_dir();
 master = posix_openpt(O_RDWR | O_NOCTTY);
 if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0 || (slave_name = ptsname(master)) == NULL)
 {
  printf("SKIP: a user-owned non-regular file is refused: no pty here\n");
  if (master >= 0)
   close(master);
  return;
 }
 slave = open(slave_name, O_RDWR | O_NOCTTY);
 put_file("t.c", "old\n", 0644);
 f = window_of_file_with_mode(0644);
 window_reads(f, "t.c");
 path_of(p, sizeof p, "t.c");
 chk_int("remove", unlink(p), 0);
 chk_int("link to the pty slave", symlink(slave_name, p), 0);
 set_rows("mine\n");
 chk_int("refused", save_window(f, "t.c", &o), -1);
 chk_int("says it is another file", o.other_file, 1);
 chk_int("nothing was asked", question_count, 0);
 if (slave >= 0)
  close(slave);
 close(master);
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

/* ---- the .bak's identity and the descriptors ---- */

/* @scenario SC-unsaved-text-19 */
/* A .bak that someone put in the name's place between the write's making and
   its undoing is not what the undo copies from: the undo reads the .bak the
   owner made, through the descriptor it kept.  The planted file would
   otherwise become the user's text, and with the special bits back on it. */
static void test_a_swapped_backup_is_not_written_back(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char *old = big(1024, 'o'), buf[8192];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "a swapped backup is not written back",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("t.c", old, 02755);
 put_file("t.bak.swap", "evil\n", 0644);
 f = window_of_file_with_mode(02755);
 setenv("XWPE_TEST_FAIL", "swap-bak", 1);
 write_past_limit(f, "t.c", &o, 2048, 4096);
 unsetenv("XWPE_TEST_FAIL");
 chk_int("restored", o.restore_err, 0);
 chk_int("the file is as it was", strcmp(get_file("t.c", buf, sizeof buf), old), 0);
 chk_lacks("the planted text is not in the file", get_file("t.c", buf, sizeof buf), "evil");
 chk_int("with its bit", mode_of("t.c"), 02755);
 chk_int("the planted file was put in the .bak's place", mode_of("t.bak.swap"), -1);
}

/* @scenario SC-unsaved-text-19 */
/* The same for a file with no special bit, which the undo would rename into
   place: a name that is no longer the .bak the owner made is left alone, and
   the undo writes back through the file. */
static void test_a_swapped_backup_is_not_renamed_into_place(void)
{
 WpeWriteOutcome o;
 FENSTER *f = make_window();
 char *old = big(1024, 'o'), buf[8192];

 make_dir();
 put_file("t.c", old, 0644);
 put_file("t.bak.swap", "evil\n", 0644);
 setenv("XWPE_TEST_FAIL", "swap-bak", 1);
 write_past_limit(f, "t.c", &o, 2048, 4096);
 unsetenv("XWPE_TEST_FAIL");
 chk_int("restored", o.restore_err, 0);
 chk_int("the file is as it was", strcmp(get_file("t.c", buf, sizeof buf), old), 0);
 chk_int("with its mode", mode_of("t.c"), 0644);
 chk_has("the planted file is still in the .bak's place", get_file("t.bak", buf, sizeof buf), "evil\n");
 chk_int("the planted file was put there", mode_of("t.bak.swap"), -1);
}

static int open_fd_count(void);
static void same_descriptors(const char *what, int before);

/* The save of the window's file t.c, the name having changed hands (to the
   file SWAP_FROM, or removed when that is NULL) between the write opening the
   file and the backup reading it; says what the user is told. */
static void save_with_the_name_changing_hands(const char *swap_from, WpeWriteOutcome *o,
                                              FENSTER *f, char *say, size_t n)
{
 char p[512];

 path_of(p, sizeof p, "t.c");
 snprintf(open_swap_at, sizeof open_swap_at, "%s", p);
 if (swap_from != NULL)
  path_of(open_swap_from, sizeof open_swap_from, swap_from);
 else
  open_swap_from[0] = '\0';
 set_rows("new\n");
 f->save = 1;
 chk_int("the save is refused", save_window(f, "t.c", o), -1);
 open_swap_at[0] = '\0';
 save_sentence(o, say, n);
}

/* @scenario SC-unsaved-text-19 */
/* The .bak is copied from the file the write opened, and from no other.  A
   name that changed hands before the backup read it refuses the save: the text
   would go into a file no longer at that name while the window said saved. */
static void test_a_name_that_changed_hands_refuses_the_save(void)
{
 WpeWriteOutcome o;
 FENSTER *f = make_window();
 char buf[256], say[1024];

 make_dir();
 put_file("t.c", "old\n", 0644);
 put_file("swap.c", "evil\n", 0644);
 save_with_the_name_changing_hands("swap.c", &o, f, say, sizeof say);
 chk_int("FAILED", o.status, WPE_WRITE_FAILED);
 chk_int("replaced is said", o.replaced, 1);
 chk_int("no backup was made", o.bak_path[0], '\0');
 chk_int("no .bak holds the other file's text", mode_of("t.bak"), -1);
 chk_has("the other file is untouched", get_file("t.c", buf, sizeof buf), "evil\n");
 chk_int("the window is still modified", f->save, 1);
 chk_has("the editor's own sentence", say,
         ": it was replaced by another file while it was being saved.  Nothing was written.  "
         "Your text is still in the window.");
 chk_has("it begins as a failure does", say, "Cannot write ");
 chk_lacks("and gives no errno text", say, "Stale");
}

/* @scenario SC-unsaved-text-19 */
/* The same when the name was removed: nothing is written, the text stays in
   the window. */
static void test_a_name_that_was_removed_refuses_the_save(void)
{
 WpeWriteOutcome o;
 FENSTER *f = make_window();
 char say[1024];

 make_dir();
 put_file("t.c", "old\n", 0644);
 save_with_the_name_changing_hands(NULL, &o, f, say, sizeof say);
 chk_int("FAILED", o.status, WPE_WRITE_FAILED);
 chk_int("replaced is said", o.replaced, 1);
 chk_int("no .bak", mode_of("t.bak"), -1);
 chk_int("nothing was made under the name", mode_of("t.c"), -1);
 chk_int("the window is still modified", f->save, 1);
 chk_has("the sentence", say, "it was replaced by another file while it was being saved.");
}

/* @scenario SC-unsaved-text-19 */
/* A name that is new gets a file and no .bak: there is no old text to keep,
   and a save must not leave an empty .bak beside every new file. */
static void test_a_new_name_leaves_no_backup(void)
{
 WpeWriteOutcome o;
 FENSTER *f = make_window();
 char p[512], buf[64];

 make_dir();
 path_of(p, sizeof p, "fresh.c");
 set_rows("new\n");
 chk_int("written", wpe_write_range(f, p, 0, 0, 1, 1, 1, &o), 0);
 chk_has("the text", get_file("fresh.c", buf, sizeof buf), "new\n");
 chk_int("no .bak", mode_of("fresh.bak"), -1);
 chk_int("none named", o.bak_path[0], '\0');
 chk_int("and no reason given", o.backup_err, 0);
}

/* @scenario SC-unsaved-text-19 */
/* A name that is a link to a file not yet made is saved as it always was: the
   file the link names is made and holds the text, the link stays a link, and
   no .bak is made for a file that did not exist. */
static void test_a_dangling_link_is_saved_into_the_file_it_names(void)
{
 WpeWriteOutcome o;
 FENSTER *f = make_window();
 char p[512], made_path[512], buf[64];
 struct stat st;

 make_dir();
 path_of(p, sizeof p, "link.c");
 path_of(made_path, sizeof made_path, "made.c");
 if (symlink(made_path, p) != 0)
  perror("symlink");
 set_rows("new\n");
 chk_int("written", wpe_write_range(f, p, 0, 0, 1, 1, 1, &o), 0);
 chk_has("the file the link names holds the text", get_file("made.c", buf, sizeof buf), "new\n");
 chk_int("lstat", lstat(p, &st), 0);
 chk_int("the name is still a link", S_ISLNK(st.st_mode) != 0, 1);
 chk_int("no .bak", mode_of("link.bak"), -1);
 chk_int("none named", o.bak_path[0], '\0');
}

/* @scenario SC-unsaved-text-19 */
/* A file that appears between the owner finding the name free and making it is
   the user's file and not the owner's: it is backed up, not taken for new. */
static void test_a_file_that_appears_as_the_name_is_made_is_backed_up(void)
{
 WpeWriteOutcome o;
 FENSTER *f = make_window();
 char p[512], buf[64];

 make_dir();
 path_of(p, sizeof p, "t.c");
 snprintf(open_plant_at, sizeof open_plant_at, "%s", p);
 set_rows("new\n");
 chk_int("written", wpe_write_range(f, p, 0, 0, 1, 1, 1, &o), 0);
 open_plant_at[0] = '\0';
 chk_has("the text", get_file("t.c", buf, sizeof buf), "new\n");
 chk_has("the planted file's text is in the .bak", get_file("t.bak", buf, sizeof buf), "planted\n");
 chk_int("the .bak is named", o.bak_path[0] != '\0', 1);
}

/* @scenario SC-unsaved-text-19 */
/* A file that was backed up and then cannot be cut is left whole, with its
   .bak, and nothing stays open. */
static void test_a_file_that_cannot_be_cut_keeps_its_text_and_its_backup(void)
{
 WpeWriteOutcome o;
 FENSTER *f = make_window();
 char p[512], buf[64], say[1024];
 int before;

 make_dir();
 put_file("t.c", "old\n", 0644);
 path_of(p, sizeof p, "t.c");
 set_rows("new\n");
 before = open_fd_count();
 ftruncate_fails = 1;
 chk_int("refused", wpe_write_range(f, p, 0, 0, 1, 1, 1, &o), -1);
 ftruncate_fails = 0;
 chk_int("FAILED", o.status, WPE_WRITE_FAILED);
 chk_int("with the system's reason", o.err, EPERM);
 chk_has("the old text is in the file", get_file("t.c", buf, sizeof buf), "old\n");
 chk_has("and in the .bak", get_file("t.bak", buf, sizeof buf), "old\n");
 save_sentence(&o, say, sizeof say);
 chk_has("the reason is said", say, "Operation not permitted");
 same_descriptors("a file that cannot be cut", before);
}

/* @scenario SC-unsaved-text-19 */
/* A failed write that finds no descriptor for the undo says the file may be
   cut short and names the .bak that holds the old text; nothing stays open. */
static void test_an_undo_with_no_descriptor_names_the_backup(void)
{
 WpeWriteOutcome o;
 FENSTER *f = make_window();
 char *old = big(1024, 'o'), a[512], b[512], say[2048], buf[8192];
 int before;

 make_dir();
 put_file("t.c", old, 0644);
 path_of(a, sizeof a, "t.c");
 path_of(b, sizeof b, "other");
 if (link(a, b) != 0)
  perror("link");
 before = open_fd_count();
 dup_fails_at = 2;
 write_past_limit(f, "t.c", &o, 2048, 4096);
 dup_fails_at = 0;
 chk_int("the undo has no descriptor", o.restore_err, EBADF);
 chk_has("the .bak is named", o.bak_path, "t.bak");
 chk_int("the .bak holds the old text", strcmp(get_file("t.bak", buf, sizeof buf), old), 0);
 save_sentence(&o, say, sizeof say);
 chk_has("the sentence", say, "may be cut short");
 same_descriptors("an undo with no descriptor", before);
}

/* @scenario SC-unsaved-text-19 */
/* A file that cannot be opened for writing leaves no new .bak behind: the
   backup is made only once the write has the file. */
static void test_a_file_that_refuses_the_write_leaves_no_new_backup(void)
{
 WpeWriteOutcome o;
 FENSTER *f = make_window();
 char p[512];

 make_dir();
 put_file("t.c", "old\n", 0444);
 path_of(p, sizeof p, "t.c");
 if (access(p, W_OK) == 0)
 {
  printf("SKIP: a file that refuses the write leaves no new backup: this user can write a 0444 file\n");
  return;
 }
 set_rows("new\n");
 chk_int("the write fails", wpe_write_range(f, p, 0, 0, 1, 1, 1, &o), -1);
 chk_int("with the open's error", o.err, EACCES);
 chk_int("no .bak was made", mode_of("t.bak"), -1);
}

/* The lowest descriptor number the process would get next, and the nth such
   number counting from the lowest (1 is the lowest). */
static int nth_free_fd(int n)
{
 int fd, found = 0;

 for (fd = 0; fd < 1024; fd++)
 {
  struct stat st;

  if (fstat(fd, &st) != 0 && errno == EBADF && ++found == n)
   return(fd);
 }
 return(-1);
}

/* How many descriptors the process holds.  A descriptor left open raises it
   even when it is above the lowest free number, which the next open would
   not show. */
static int open_fd_count(void)
{
 int fd, held = 0;

 for (fd = 0; fd < 1024; fd++)
 {
  struct stat st;

  if (fstat(fd, &st) == 0)
   held++;
 }
 return(held);
}

static void same_descriptors(const char *what, int before)
{
 chk_int(what, open_fd_count(), before);
}

/* @scenario SC-unsaved-text-19 */
/* A write, whatever happens to it, ends with the descriptors it started
   with: not a copy of the file, nor of its .bak, stays open.  Every outcome
   that has no special bit, and so needs no premise. */
static void test_every_plain_outcome_closes_what_it_opened(void)
{
 WpeWriteOutcome o;
 FENSTER *f = make_window();
 char *old = big(1024, 'o'), p[512];
 int before;

 make_dir();
 put_file("t.c", old, 0644);
 path_of(p, sizeof p, "t.c");
 set_rows("new\n");
 before = open_fd_count();
 chk_int("written, with a backup", wpe_write_range(f, p, 0, 0, 1, 1, 1, &o), 0);
 same_descriptors("written, with a backup", before);
 chk_int("written, no backup", wpe_write_range(f, p, 0, 0, 1, 1, 0, &o), 0);
 same_descriptors("written, no backup", before);
 f->ins = 8;
 chk_int("read-only", wpe_write_range(f, p, 0, 0, 1, 1, 1, &o), -1);
 same_descriptors("read-only", before);
 f->ins = 0;
 {
  char d[512];

  path_of(d, sizeof d, "dir");
  if (mkdir(d, 0755) != 0)
   perror("mkdir");
  chk_int("a folder as the target", wpe_write_range(f, d, 0, 0, 1, 1, 1, &o), -1);
  same_descriptors("target does not open", before);
 }
 put_file("t.c", old, 0644);
 write_past_limit(f, "t.c", &o, 2048, 4096);
 chk_int("undone by rename", o.restore_err, 0);
 same_descriptors("restore by rename", before);
 put_file("t.c", big(4096, 'o'), 0644);
 set_rows("X");
 limit_file_size(1024);
 chk_int("backup stopped", wpe_write_range(f, p, 0, 0, 1, 1, 1, &o), -1);
 no_file_size_limit();
 chk_int("by the size limit", o.backup_stopped, 1);
 same_descriptors("backup stopped", before);
 put_file("t.c", old, 0644);
 setenv("XWPE_TEST_FAIL", "restore", 1);
 write_past_limit(f, "t.c", &o, 2048, 4096);
 unsetenv("XWPE_TEST_FAIL");
 chk_int("restore refused by the hook", o.restore_err, EIO);
 same_descriptors("restore hook", before);
 put_file("t.bak.swap", "evil\n", 0644);
 setenv("XWPE_TEST_FAIL", "swap-bak", 1);
 write_past_limit(f, "t.c", &o, 2048, 4096);
 unsetenv("XWPE_TEST_FAIL");
 chk_int("undone by the file", o.restore_err, 0);
 same_descriptors("swapped backup", before);
}

/* @scenario SC-unsaved-text-19 */
/* The same for the outcomes of a file with a special bit, whose undo writes
   back through the file and whose save syncs it. */
static void test_every_special_bit_outcome_closes_what_it_opened(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char *old = big(1024, 'o');
 int before;

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "special-bit outcomes close their descriptors",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("t.c", old, 02755);
 f = window_of_file_with_mode(02755);
 set_rows("new\n");
 before = open_fd_count();
 chk_int("saved with its bit", save_window(f, "t.c", &o), 0);
 same_descriptors("saved with its bit", before);
 put_file("t.c", old, 02755);
 write_past_limit(f, "t.c", &o, 2048, 4096);
 chk_int("written back", o.restore_err, 0);
 same_descriptors("written back through the file", before);
 setenv("XWPE_TEST_FAIL", "restore", 1);
 write_past_limit(f, "t.c", &o, 2048, 4096);
 unsetenv("XWPE_TEST_FAIL");
 same_descriptors("restore hook, with a bit", before);
}

/* @scenario SC-unsaved-text-19 */
/* The .bak is made, and the descriptor kept, even when the system has no
   descriptor left for the keeping: the backup is then stopped and the file
   is not touched, as for a full disk. */
static void test_a_backup_with_no_descriptor_to_keep_stops_the_save(void)
{
 WpeWriteOutcome o;
 FENSTER *f = make_window();
 char *old = big(1024, 'o'), buf[8192], p[512];
 struct rlimit was, tight;
 int before = open_fd_count();

 make_dir();
 put_file("t.c", old, 0644);
 path_of(p, sizeof p, "t.c");
 set_rows("new\n");
 getrlimit(RLIMIT_NOFILE, &was);
 tight = was;
 tight.rlim_cur = (rlim_t)nth_free_fd(4);
 setrlimit(RLIMIT_NOFILE, &tight);
 chk_int("the save is refused", wpe_write_range(f, p, 0, 0, 1, 1, 1, &o), -1);
 setrlimit(RLIMIT_NOFILE, &was);
 chk_int("the backup stopped", o.backup_stopped, 1);
 chk_int("for want of a descriptor", o.err, EMFILE);
 chk_int("the file was not touched", strcmp(get_file("t.c", buf, sizeof buf), old), 0);
 chk_int("no .bak is left", mode_of("t.bak"), -1);
 same_descriptors("no descriptor to keep", before);
}

/* ---- a mode changed by name: Attributes and Ctrl-F9 ---- */

/* The Attributes dialog changes the mode through the descriptor it opened,
   not through the name.  Red on a tree that calls chmod by name. */
static void test_set_perm_changes_the_mode_by_descriptor(void)
{
 WpeModeKept k;
 char p[512];

 make_dir();
 put_file("f", "x\n", 0644);
 path_of(p, sizeof p, "f");
 fakechmod_count_reset();
 chk_int("changed", wpe_write_set_perm(p, NULL, 0600, &k), 0);
 chk_int("the mode", mode_of("f"), 0600);
 chk_int("by descriptor", fakechmod_by_fd, 1);
 chk_int("not by name", fakechmod_by_name, 0);
}

/* A file the user may write but not read is opened write only: the
   descriptor takes the change.  Mutation that turns it red: drop the retry
   with O_WRONLY (the change then goes by name). */
static void test_set_perm_on_a_write_only_file_uses_the_write_descriptor(void)
{
 WpeModeKept k;
 char p[512];

 make_dir();
 if (!test_premise(test_premise_file_0200_is_unreadable(dir), "a write-only file is changed by descriptor",
                   "this user reads a 0200 file"))
  return;
 put_file("w", "x\n", 0200);
 path_of(p, sizeof p, "w");
 fakechmod_count_reset();
 chk_int("changed", wpe_write_set_perm(p, NULL, 0600, &k), 0);
 chk_int("the mode", mode_of("w"), 0600);
 chk_int("by descriptor", fakechmod_by_fd, 1);
 chk_int("not by name", fakechmod_by_name, 0);
}

/* A file that can be neither read nor written is changed by its name, and
   keeps the special bits it had.  Mutation that turns it red: no fallback. */
static void test_set_perm_on_a_file_that_cannot_be_opened_goes_by_name(void)
{
 WpeModeKept k;
 char p[512];

 make_dir();
 if (!test_premise(test_premise_file_0200_is_unreadable(dir), "an unopenable file is changed by name",
                   "this user opens a file with no permission") ||
     !test_premise(test_premise_special_bits_settable(dir, S_ISGID), "an unopenable file is changed by name",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("z", "x\n", 02000);
 path_of(p, sizeof p, "z");
 fakechmod_count_reset();
 chk_int("changed", wpe_write_set_perm(p, NULL, 0600, &k), 0);
 chk_int("the mode, with its bit", mode_of("z"), 02600);
 chk_int("by name", fakechmod_by_name, 1);
 chk_int("the name was not unreached", k.unreached, 0);
}

/* A link put under the name after the dialog looked at the file is not
   followed: the file it leads to keeps its mode and the change is reported
   as replaced.  Red on a tree that changes the mode by name. */
static void test_set_perm_does_not_follow_a_link_put_in_place(void)
{
 WpeModeKept k;
 char p[512], to[512];

 make_dir();
 put_file("f", "x\n", 0644);
 put_file("f.target", "y\n", 0644);
 path_of(p, sizeof p, "f");
 path_of(to, sizeof to, "f.target");
 snprintf(open_link_at, sizeof open_link_at, "%s", p);
 snprintf(open_link_to, sizeof open_link_to, "%s", to);
 chk_int("refused as a link", wpe_write_set_perm(p, NULL, 0600, &k), ESTALE);
 open_link_at[0] = '\0';
 chk_int("said to be replaced", k.replaced, 1);
 chk_int("by a link", k.replaced_kind, S_IFLNK);
 chk_int("the file the link leads to is as it was", mode_of("f.target"), 0644);
}

/* Another regular file renamed into place between the look and the open is
   caught by the inode compare: it keeps its mode too. */
static void test_set_perm_does_not_change_a_file_that_took_the_name(void)
{
 WpeModeKept k;
 char p[512], from[512];

 make_dir();
 put_file("f", "x\n", 0644);
 put_file("g", "y\n", 0644);
 path_of(p, sizeof p, "f");
 path_of(from, sizeof from, "g");
 snprintf(open_swap_at, sizeof open_swap_at, "%s", p);
 snprintf(open_swap_from, sizeof open_swap_from, "%s", from);
 chk_int("refused as replaced", wpe_write_set_perm(p, NULL, 0600, &k), ESTALE);
 open_swap_at[0] = '\0';
 chk_int("said to be replaced", k.replaced, 1);
 chk_int("by a regular file", k.replaced_kind, S_IFREG);
 chk_int("the file now under the name is as it was", mode_of("f"), 0644);
}

/* A link swapped in just before the change by name is not followed either,
   and the name that no longer leads to the file is reported.  Needs a file
   this user cannot open, so the change goes by name. */
static void test_a_link_swapped_in_before_a_change_by_name_is_reported(void)
{
 WpeModeKept k;
 char p[512];

 make_dir();
 if (!test_premise(test_premise_file_0200_is_unreadable(dir), "a swap before a change by name",
                   "this user opens a file with no permission"))
  return;
 put_file("z", "x\n", 0000);
 put_file("z.target", "y\n", 0644);
 path_of(p, sizeof p, "z");
 fakechmod_swap_at(p, "z.target");
 chk_int("refused as replaced", wpe_write_set_perm(p, NULL, 0600, &k), ESTALE);
 fakechmod_swap_at("", "");
 chk_int("said to be replaced", k.replaced, 1);
 chk_int("by a link", k.replaced_kind, S_IFLNK);
 chk_int("the file the link leads to is as it was", mode_of("z.target"), 0644);
}

/* The dialog showed one file; the name was given to another while the dialog
   was open.  The owner changes only the file it was shown.  Each row swaps
   after the dialog's stat and before the owner looks.  Mutation that turns
   them red: ignore the shown stat. */

/* A regular file replaced by a link to a victim: not followed, victim
   untouched, reported as replaced. */
static void test_a_link_swapped_in_after_the_dialog_is_not_followed(void)
{
 WpeModeKept k;
 WpeNameSeen seen;
 struct stat shown;
 char p[512], v[512];

 make_dir();
 put_file("f", "x\n", 0644);
 put_file("victim", "y\n", 0600);
 path_of(p, sizeof p, "f");
 path_of(v, sizeof v, "victim");
 stat(p, &shown);
 wpe_write_name_seen(p, &seen);
 unlink(p);
 if (symlink(v, p) != 0)
  perror("symlink");
 chk_int("refused as replaced", wpe_write_set_perm_seen(p, &shown, &seen, 0666, &k), ESTALE);
 chk_int("said to be replaced", k.replaced, 1);
 chk_int("by a link: the name was a file when shown", k.replaced_kind, S_IFLNK);
 chk_int("nothing was changed", k.changed, 0);
 chk_int("the victim is as it was", mode_of("victim"), 0600);
}

/* A link the user named is followed once, and the dialog's stat followed it
   too: the shown file is the target, which changes. */
static void test_a_named_link_shown_by_the_dialog_is_changed(void)
{
 WpeModeKept k;
 struct stat shown;
 char p[512], to[512];

 make_dir();
 put_file("real", "x\n", 0644);
 path_of(p, sizeof p, "ln");
 path_of(to, sizeof to, "real");
 if (symlink(to, p) != 0)
  perror("symlink");
 stat(p, &shown);
 chk_int("changed", wpe_write_set_perm(p, &shown, 0600, &k), 0);
 chk_int("the target changed", mode_of("real"), 0600);
}

/* A name that goes through a folder, swapped for a link to another folder
   after the name was resolved by the dialog: the file at the end of it is
   another file. */
static void test_a_folder_swapped_after_the_dialog_is_refused(void)
{
 WpeModeKept k;
 WpeNameSeen seen;
 struct stat shown;
 char p[512], d[512], d_away[512], e[512];

 make_dir();
 path_of(d, sizeof d, "d");
 path_of(d_away, sizeof d_away, "d.away");
 path_of(e, sizeof e, "e");
 mkdir(d, 0700);
 mkdir(e, 0700);
 put_file("d/T", "x\n", 0644);
 put_file("e/T", "y\n", 0644);
 path_of(p, sizeof p, "L");
 if (symlink("d/T", p) != 0)
  perror("symlink");
 stat(p, &shown);
 wpe_write_name_seen(p, &seen);
 rename(d, d_away);
 if (symlink(e, d) != 0)
  perror("symlink");
 chk_int("refused as replaced", wpe_write_set_perm_seen(p, &shown, &seen, 0600, &k), ESTALE);
 chk_int("said to be replaced", k.replaced, 1);
 chk_int("L was a link all along: not said to be one that took the name", k.replaced_kind, 0);
 chk_int("the other file is as it was", mode_of("e/T"), 0644);
}

/* The dialog was opened on a link and showed the file it leads to; that file
   was then replaced by an atomic save.  The name is the same link it always
   was: the owner refuses, and does not say a link took the name.  A link
   that is another link than the one shown is a link that took the name; with
   no knowledge of the name a link is never claimed.  Red on a tree that
   records S_IFLNK for any name that is a link now. */
static void test_a_link_the_dialog_named_is_not_a_link_that_took_the_name(void)
{
 WpeModeKept k;
 WpeNameSeen seen;
 struct stat shown;
 char p[512], t[512], t_new[512], u[512];

 make_dir();
 put_file("t", "x\n", 0644);
 put_file("t.new", "y\n", 0644);
 put_file("u", "z\n", 0644);
 path_of(p, sizeof p, "z");
 path_of(t, sizeof t, "t");
 path_of(t_new, sizeof t_new, "t.new");
 path_of(u, sizeof u, "u");
 if (symlink(t, p) != 0)
  perror("symlink");
 stat(p, &shown);
 wpe_write_name_seen(p, &seen);
 chk_int("the name was a link when shown", seen.kind, S_IFLNK);
 chk_int("known", seen.known, 1);
 chk_int("the target is replaced", rename(t_new, t), 0);
 chk_int("refused", wpe_write_set_perm_seen(p, &shown, &seen, 0600, &k), ESTALE);
 chk_int("said to be replaced", k.replaced, 1);
 chk_int("not by a link: the same link as before", k.replaced_kind, 0);
 chk_int("refused with no knowledge of the name", wpe_write_set_perm(p, &shown, 0600, &k), ESTALE);
 chk_int("a link is never claimed without it", k.replaced_kind, 0);
 unlink(p);
 if (symlink(u, p) != 0)
  perror("symlink");
 chk_int("refused for another link", wpe_write_set_perm_seen(p, &shown, &seen, 0600, &k), ESTALE);
 chk_int("by a link: it is another one", k.replaced_kind, S_IFLNK);
 chk_int("the file it leads to is as it was", mode_of("u"), 0644);
}

/* A socket planted at the name before the dialog, swapped for a hard link to
   a victim: the name used to go by name and change the victim.  It is
   refused before any call. */
static void test_a_socket_swapped_for_a_hard_link_changes_nothing(void)
{
 WpeModeKept k;
 struct stat shown;
 char p[512], v[512];

 make_dir();
 put_file("victim", "y\n", 0600);
 path_of(p, sizeof p, "s");
 path_of(v, sizeof v, "victim");
 if (plant_socket(p) != 0) {
  printf("FAIL: a socket could not be planted\n");
  fails++;
  return;
 }
 stat(p, &shown);
 unlink(p);
 if (link(v, p) != 0)
  perror("link");
 fakechmod_count_reset();
 chk_int("refused as replaced", wpe_write_set_perm(p, &shown, 0666, &k), ESTALE);
 chk_int("said to be replaced", k.replaced, 1);
 chk_int("by a hard link, which is a regular file", k.replaced_kind, S_IFREG);
 chk_int("no call was made", fakechmod_by_name + fakechmod_by_fd, 0);
 chk_int("the victim is as it was", mode_of("victim"), 0600);
}

/* A socket that takes the name between the owner's lstat and its open is
   another file, never worked around by name, whatever the system calls the
   refusal of the open. */
static void test_a_socket_swapped_in_before_the_open_is_replaced(void)
{
 WpeModeKept k;
 char p[512];

 make_dir();
 put_file("f", "x\n", 0644);
 path_of(p, sizeof p, "f");
 snprintf(open_socket_at, sizeof open_socket_at, "%s", p);
 fakechmod_count_reset();
 chk_int("refused", wpe_write_set_perm(p, NULL, 0600, &k), ESTALE);
 open_socket_at[0] = '\0';
 chk_int("said to be replaced", k.replaced, 1);
 chk_int("by a socket", k.replaced_kind, S_IFSOCK);
 chk_int("no change by name", fakechmod_by_name, 0);
}

/* What this system calls the refusal of an open of a file that took the name
   is no matter: the owner looks at the name.  A link takes the name, and the
   open fails with the word FreeBSD (EMLINK) or NetBSD (EFTYPE) uses.  Red
   on a tree that decides from the errno: EMLINK goes through with nothing
   said to be replaced. */
static void test_a_link_put_in_place_is_replaced_whatever_the_system_says(void)
{
 static const int words[] = {
#ifdef EMLINK
  EMLINK,
#endif
#ifdef EFTYPE
  EFTYPE,
#endif
  EOPNOTSUPP
 };
 WpeModeKept k;
 char p[512], to[512];
 size_t i;

 make_dir();
 put_file("f", "x\n", 0644);
 put_file("f.target", "y\n", 0644);
 path_of(p, sizeof p, "f");
 path_of(to, sizeof to, "f.target");
 for (i = 0; i < sizeof words / sizeof words[0]; i++)
 {
  snprintf(open_link_at, sizeof open_link_at, "%s", p);
  snprintf(open_link_to, sizeof open_link_to, "%s", to);
  snprintf(open_fail_at, sizeof open_fail_at, "%s", p);
  open_fail_with = words[i];
  chk_int("refused", wpe_write_set_perm(p, NULL, 0600, &k), ESTALE);
  open_link_at[0] = '\0';
  open_fail_at[0] = '\0';
  chk_int("said to be replaced", k.replaced, 1);
  chk_int("by a link", k.replaced_kind, S_IFLNK);
  chk_int("no call was made", k.unreached, 0);
  chk_int("the file it leads to is as it was", mode_of("f.target"), 0644);
  unlink(p);
  put_file("f", "x\n", 0644);
 }
}

/* The same for a socket: the BSDs say EOPNOTSUPP for the open of one, Linux
   ENXIO.  Another file under the name, by either word. */
static void test_a_socket_put_in_place_is_replaced_whatever_the_system_says(void)
{
 static const int words[] = { EOPNOTSUPP, ENXIO };
 WpeModeKept k;
 char p[512];
 size_t i;

 make_dir();
 path_of(p, sizeof p, "f");
 for (i = 0; i < sizeof words / sizeof words[0]; i++)
 {
  put_file("f", "x\n", 0644);
  snprintf(open_socket_at, sizeof open_socket_at, "%s", p);
  snprintf(open_fail_at, sizeof open_fail_at, "%s", p);
  open_fail_with = words[i];
  fakechmod_count_reset();
  chk_int("refused", wpe_write_set_perm(p, NULL, 0600, &k), ESTALE);
  open_socket_at[0] = '\0';
  open_fail_at[0] = '\0';
  chk_int("said to be replaced", k.replaced, 1);
  chk_int("by a socket", k.replaced_kind, S_IFSOCK);
  chk_int("no change by name", fakechmod_by_name, 0);
  unlink(p);
 }
}

/* A name that is not there is not "another file": a failed open with nothing
   at the name keeps the open's own errno and is not said to be replaced. */
static void test_an_open_that_fails_with_nothing_at_the_name_keeps_its_errno(void)
{
 WpeModeKept k;
 char p[512];

 make_dir();
 put_file("f", "x\n", 0644);
 path_of(p, sizeof p, "f");
 snprintf(open_swap_at, sizeof open_swap_at, "%s", p);
 open_swap_from[0] = '\0';
 snprintf(open_fail_at, sizeof open_fail_at, "%s", p);
 open_fail_with = EMFILE;
 chk_int("the open's errno", wpe_write_set_perm(p, NULL, 0600, &k), EMFILE);
 open_swap_at[0] = '\0';
 open_fail_at[0] = '\0';
 chk_int("not said to be replaced", k.replaced, 0);
 chk_int("the name was unreached", k.unreached, 1);
}

/* blocked_in_open - Is this process waiting inside openat?  Reads
   /proc/<pid>/syscall (Linux).  Return: 1 yes, 0 no, -1 cannot be read. */
static int blocked_in_open(pid_t pid)
{
 char path[64], text[128];
 FILE *fp;
 long number = -1;
 int got;

 snprintf(path, sizeof path, "/proc/%ld/syscall", (long)pid);
 fp = fopen(path, "r");
 if (fp == NULL)
  return(-1);
 got = fscanf(fp, "%127s", text);
 fclose(fp);
 if (got != 1)
  return(-1);
 if (sscanf(text, "%ld", &number) != 1)
  return(0);
 return(number == (long)SYS_openat);
}

/* The writer's side of the FIFO row: it opens the FIFO for writing, which
   blocks until a reader opens it, then writes one byte.  Lives at most the
   watchdog's lifetime. */
static void blocked_writer_child(const char *fifo)
{
 int fd;

 test_lifetime_arm(WRITER_LIFETIME_S);
 fd = open(fifo, O_WRONLY);
 if (fd >= 0)
  (void)write(fd, "x", 1);
 _exit(0);
}

/* A FIFO is changed by its name and never opened.  A writer waiting in its
   open must still be waiting, and alive, after the change of mode: an open
   for reading, even O_NONBLOCK, counts as a reader, releases the writer and
   kills it with SIGPIPE when the descriptor closes.  The writer is a child
   of the row, found waiting in openat through /proc (a bounded poll of that
   state, not a sleep), observed for a fixed window after the change and
   ended by its own PID.  Red on a tree that sends FIFOs through a descriptor. */
static void test_a_writer_waiting_on_a_fifo_is_not_released_by_a_change_of_mode(void)
{
 WpeModeKept k;
 char p[512];
 pid_t pid;
 long waited;

 make_dir();
 path_of(p, sizeof p, "pipe");
 mkfifo(p, 0644);
 pid = fork();
 if (pid == 0)
  blocked_writer_child(p);
 for (waited = 0; waited < WRITER_BLOCK_WAIT_MS && blocked_in_open(pid) == 0; waited += TEST_WAIT_STEP_MS)
  test_sleep_ms(TEST_WAIT_STEP_MS);
 if (!test_observation(blocked_in_open(pid) == 1, "a blocked writer is not released",
                       "/proc/<pid>/syscall does not show the writer waiting in openat")) {
  kill(pid, SIGKILL);
  test_wait_child(pid, WRITER_END_MS);
  return;
 }
 chk_int("changed", wpe_write_set_perm(p, NULL, 0600, &k), 0);
 for (waited = 0; waited < WRITER_WATCH_MS && blocked_in_open(pid) == 1; waited += TEST_WAIT_STEP_MS)
  test_sleep_ms(TEST_WAIT_STEP_MS);
 chk_int("the writer is still waiting in its open", blocked_in_open(pid), 1);
 chk_int("the writer is alive", kill(pid, 0), 0);
 kill(pid, SIGKILL);
 test_wait_child(pid, WRITER_END_MS);
}

/* A FIFO is changed by its name: opening it would have effects on others.
   Red on a tree that sends FIFOs through a descriptor. */
static void test_a_fifo_is_changed_by_name(void)
{
 WpeModeKept k;
 char p[512];

 make_dir();
 path_of(p, sizeof p, "pipe");
 mkfifo(p, 0644);
 fakechmod_count_reset();
 chk_int("changed", wpe_write_set_perm(p, NULL, 0600, &k), 0);
 chk_int("the mode", mode_of("pipe"), 0600);
 chk_int("by name", fakechmod_by_name, 1);
 chk_int("not by descriptor", fakechmod_by_fd, 0);
}

/* Ctrl-F9 adds the executable bit and keeps the special bits the script had.
   The dialog's own change keeps them as well. */
static void test_add_perm_adds_x_and_keeps_the_special_bits(void)
{
 WpeModeKept k;
 char p[512];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "Ctrl-F9 keeps the special bits",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("s.sh", "echo\n", 02644);
 path_of(p, sizeof p, "s.sh");
 chk_int("added", wpe_write_add_perm(p, NULL, S_IXUSR, &k), 0);
 chk_int("x added, setgid kept", mode_of("s.sh"), 02744);
 chk_int("nothing lost", (long)k.lost, 0);
 chk_int("replaced the nine bits", wpe_write_set_perm(p, NULL, 0600, &k), 0);
 chk_int("setgid kept", mode_of("s.sh"), 02600);
}

/* A link the user named is followed once: the file it leads to changes. */
static void test_add_perm_on_a_link_the_user_named_follows_it_once(void)
{
 WpeModeKept k;
 char p[512];

 make_dir();
 put_file("real.sh", "echo\n", 0644);
 path_of(p, sizeof p, "l.sh");
 {
  char to[512];

  path_of(to, sizeof to, "real.sh");
  if (symlink(to, p) != 0)
   perror("symlink");
 }
 chk_int("added", wpe_write_add_perm(p, NULL, S_IXUSR, &k), 0);
 chk_int("the file changed", mode_of("real.sh"), 0744);
}

/* A name that leads nowhere is reported as not reached, nothing changed. */
static void test_a_name_that_leads_nowhere_is_not_reached(void)
{
 WpeModeKept k;
 char p[512];

 make_dir();
 path_of(p, sizeof p, "none");
 chk_int("refused", wpe_write_set_perm(p, NULL, 0600, &k), ENOENT);
 chk_int("not reached", k.unreached, 1);
}

/* ---- class A: the assistant's writes and a revert go through the owner ---- */

/* The identity of the file NAME of the test folder, as a window would hold it. */
static WpeFileIdentity identity_of(const char *name)
{
 WpeFileIdentity id;

 memset(&id, 0, sizeof id);
 chk_int("take the identity", wpe_write_identity_of_name(dir, name, &id), 1);
 return(id);
}

/* @scenario SC-write-owner-A-01: the assistant's write is a Save: the old
   text in the .bak (which has no special bit), the new text on disk, the
   file's own bit back after one sync of the saved file. */
static void test_the_assistants_write_is_a_save(void)
{
 WpeWriteOutcome o;
 WpeFileIdentity id;
 char p[512], buf[256];
 struct stat st;

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "the assistant's write is a save",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("t.c", "old\n", 02755);
 id = identity_of("t.c");
 path_of(p, sizeof p, "t.c");
 fsync_reset();
 chk_int("written", wpe_write_save_text(p, "new\n", &id, 0, NULL, &o), 0);
 chk_int("status", o.status, WPE_WRITE_WRITTEN);
 chk_int("the new text is on disk", strcmp(get_file("t.c", buf, sizeof buf), "new\n"), 0);
 chk_int("the file keeps its setgid bit", mode_of("t.c"), 02755);
 chk_int("the old text is in the .bak", strcmp(get_file("t.bak", buf, sizeof buf), "old\n"), 0);
 chk_int("the .bak has no special bit", mode_of("t.bak"), 0755);
 stat(p, &st);
 chk_int("one sync", fsync_calls, 1);
 chk_int("on the saved file", (long)(fsync_last_ino == st.st_ino), 1);
 chk_int("the outcome names no lost bit", (long)o.file_kept.lost, 0);
}

/* @scenario SC-write-owner-A-02: a name the caller resolved is not followed
   when a link has taken its place: nothing is written through it. */
static void test_the_assistants_write_refuses_a_link_at_the_end(void)
{
 WpeWriteOutcome o;
 char p[512], q[512], buf[256];

 make_dir();
 put_file("other", "keep me\n", 0644);
 path_of(p, sizeof p, "t.c");
 path_of(q, sizeof q, "other");
 chk_int("the link", symlink(q, p), 0);
 chk_int("refused", wpe_write_save_text(p, "new\n", NULL, WPE_WRITE_NOFOLLOW, NULL, &o), -1);
 chk_int("FAILED", o.status, WPE_WRITE_FAILED);
 chk_int("a link took the name", o.replaced_by_link, 1);
 chk_int("the reason", o.err, ELOOP);
 chk_int("the file it leads to is untouched", strcmp(get_file("other", buf, sizeof buf), "keep me\n"), 0);
 chk_int("no backup of it", mode_of("t.bak"), -1);
 chk_int("followed without the flag, as the user's own names are",
         wpe_write_save_text(p, "new\n", NULL, 0, NULL, &o), 0);
 chk_int("then the file it leads to holds the text", strcmp(get_file("other", buf, sizeof buf), "new\n"), 0);
}

/* @scenario SC-write-owner-A-03: with no window open on the file, the file at
   the name is the file, and its own bits are kept. */
static void test_the_assistants_write_with_no_window_keeps_the_files_own_bits(void)
{
 WpeWriteOutcome o;
 char p[512];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "a write with no window keeps the bit",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("t.c", "old\n", 02755);
 path_of(p, sizeof p, "t.c");
 chk_int("written", wpe_write_save_text(p, "new\n", NULL, WPE_WRITE_NOFOLLOW, NULL, &o), 0);
 chk_int("the file keeps its setgid bit", mode_of("t.c"), 02755);
}

/* A window open on the file that is not the file at the name: refused, as a
   Save is, with nothing written (the assistant cannot be asked). */
static void test_the_assistants_write_over_a_different_file_than_the_windows_is_refused(void)
{
 WpeWriteOutcome o;
 WpeFileIdentity id;
 char p[512], buf[256], say[1024];

 make_dir();
 put_file("t.c", "old\n", 0644);
 id = identity_of("t.c");
 another_file_takes_the_name("t.c", "theirs\n", 0644);
 path_of(p, sizeof p, "t.c");
 chk_int("refused", wpe_write_save_text(p, "new\n", &id, 0, NULL, &o), -1);
 chk_int("says it is another file", o.other_file, 1);
 chk_int("nothing was asked", question_count, 0);
 chk_int("their text is untouched", strcmp(get_file("t.c", buf, sizeof buf), "theirs\n"), 0);
 save_sentence(&o, say, sizeof say);
 chk_has("the sentence", say, "the name leads to a different file than the one this window read");
}

/* @scenario SC-write-owner-H2b-01: the assistant's write into a file replaced
   under its window asks, as Save does.  The first pass writes nothing and says
   whom to ask about; a Yes pass writes the text as a copy (no special bit); a
   file that took the name while the box was up is refused, not written. */
static void test_the_assistants_write_asks_about_a_replaced_file(void)
{
 WpeWriteOutcome o;
 WpeFileIdentity id, asked;
 WpeSeen now;
 char p[512], buf[256], say[1024];

 make_dir();
 put_file("t.c", "old\n", 0644);
 id = identity_of("t.c");
 another_file_takes_the_name("t.c", "theirs\n", 0644);
 path_of(p, sizeof p, "t.c");
 chk_int("the first pass writes nothing", wpe_write_save_text_ask(p, "new\n", &id, 0, NULL, NULL, &o), -1);
 chk_int("it says to ask", o.ask_overwrite, 1);
 chk_int("about a file it names", o.asked.known, 1);
 chk_int("their text is untouched", strcmp(get_file("t.c", buf, sizeof buf), "theirs\n"), 0);
 chk_int("no backup was made", mode_of("t.bak"), -1);
 asked = o.asked;
 chk_int("after a Yes it writes", wpe_write_save_text_ask(p, "new\n", &id, 0, &asked, NULL, &o), 0);
 chk_int("the text is on disk", strcmp(get_file("t.c", buf, sizeof buf), "new\n"), 0);
 chk_int("a copy has no special bit", mode_of("t.c") & 07000, 0);
 chk_int("the file now there is looked at", wpe_seen_look_name(p, 1, &now), 0);
 chk_int("the outcome names the file now there", wpe_write_identity_is(&o.written, &now), 1);
 another_file_takes_the_name("t.c", "theirs again\n", 0644);
 chk_int("a third file is refused", wpe_write_save_text_ask(p, "newer\n", &id, 0, &asked, NULL, &o), -1);
 chk_int("it says the name changed", o.changed_while_asked, 1);
 chk_int("their text is untouched too", strcmp(get_file("t.c", buf, sizeof buf), "theirs again\n"), 0);
 (void)wpe_write_sentence(say, sizeof say, &o, WPE_WRITE_THEN_NOTHING_MORE);
 chk_has("the assistant's sentence", say, "the name changed again while you were being asked");
 chk_int("no advice about Save As", strstr(say, "Save As") == NULL, 1);
}

/* @scenario SC-write-owner-H2b-02: a program, another owner's file or a file
   that is not a plain one is refused on the first pass, nothing asked. */
static void test_the_assistants_write_refuses_a_replaced_program(void)
{
 WpeWriteOutcome o;
 WpeFileIdentity id;
 char p[512], buf[256], say[1024];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "a replaced program is refused",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("t.c", "old\n", 0644);
 id = identity_of("t.c");
 another_file_takes_the_name("t.c", "theirs\n", 02755);
 path_of(p, sizeof p, "t.c");
 chk_int("refused", wpe_write_save_text_ask(p, "new\n", &id, 0, NULL, NULL, &o), -1);
 chk_int("not asked", o.ask_overwrite, 0);
 chk_int("another file", o.other_file, 1);
 chk_int("their text is untouched", strcmp(get_file("t.c", buf, sizeof buf), "theirs\n"), 0);
 (void)wpe_write_sentence(say, sizeof say, &o, WPE_WRITE_THEN_NOTHING_MORE);
 chk_has("the assistant's sentence", say, "Cannot write");
 chk_int("no advice about Save As", strstr(say, "Save As") == NULL, 1);
}

/* @scenario SC-write-owner-H2b-03: a revert that finds the name leading to a
   different file says it put nothing back, and gives no Save As advice. */
static void test_a_revert_that_finds_another_file_says_nothing_was_changed(void)
{
 WpeWriteOutcome o;
 WpeFileIdentity id;
 char p[512], say[1024];

 make_dir();
 put_file("t.c", "old\n", 0644);
 id = identity_of("t.c");
 another_file_takes_the_name("t.c", "theirs\n", 0644);
 path_of(p, sizeof p, "t.c");
 chk_int("refused", wpe_write_restore_text(p, "old\n", 4, WPE_WRITE_NO_RECORDED_MODE, &id, NULL, &o), -1);
 (void)wpe_write_sentence(say, sizeof say, &o, WPE_WRITE_THEN_PUT_BACK);
 chk_has("the revert's sentence", say, "Cannot put back");
 chk_has("what the name leads to", say, "the name now leads to a different file than the one the window shows");
 chk_has("what is kept", say, "Nothing was changed.");
 chk_int("no advice about Save As", strstr(say, "Save As") == NULL, 1);
}

/* A name that is not there is made, with no backup of nothing. */
static void test_the_assistants_write_makes_a_new_file(void)
{
 WpeWriteOutcome o;
 char p[512], buf[256];

 make_dir();
 path_of(p, sizeof p, "fresh.c");
 chk_int("written", wpe_write_save_text(p, "new\n", NULL, WPE_WRITE_NOFOLLOW, NULL, &o), 0);
 chk_int("the text is on disk", strcmp(get_file("fresh.c", buf, sizeof buf), "new\n"), 0);
 chk_int("no backup", mode_of("fresh.bak"), -1);
}

/* @scenario SC-write-owner-A-04: a revert gives back the bits recorded with
   the before image, not the ones the file has now. */
static void test_a_revert_puts_back_the_recorded_bits(void)
{
 WpeWriteOutcome o;
 char p[512], buf[256];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "a revert puts the bit back",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("t.c", "agent\n", 0755);
 path_of(p, sizeof p, "t.c");
 fsync_reset();
 chk_int("restored", wpe_write_restore_text(p, "old\n", 4, 02755, NULL, NULL, &o), 0);
 chk_int("the old text is back", strcmp(get_file("t.c", buf, sizeof buf), "old\n"), 0);
 chk_int("with the recorded bits", mode_of("t.c"), 02755);
 chk_int("one sync before the bit", fsync_calls, 1);
 chk_int("no backup is made beside the file", mode_of("t.bak"), -1);
}

/* The nine bits recorded are given too, and a revert refuses a link as the
   assistant's write does. */
static void test_a_revert_gives_the_recorded_nine_bits_and_refuses_a_link(void)
{
 WpeWriteOutcome o;
 char p[512], q[512], target[512], buf[256];

 make_dir();
 put_file("t.c", "agent\n", 0600);
 path_of(p, sizeof p, "t.c");
 chk_int("restored", wpe_write_restore_text(p, "old\n", 4, 0644, NULL, NULL, &o), 0);
 chk_int("the nine bits are the recorded ones", mode_of("t.c"), 0644);
 put_file("other", "keep me\n", 0644);
 path_of(q, sizeof q, "link.c");
 path_of(target, sizeof target, "other");
 chk_int("the link", symlink(target, q), 0);
 chk_int("refused", wpe_write_restore_text(q, "old\n", 4, 0644, NULL, NULL, &o), -1);
 chk_int("a link took the name", o.replaced_by_link, 1);
 chk_int("the reason", o.err, ELOOP);
 chk_int("the target is untouched", strcmp(get_file("other", buf, sizeof buf), "keep me\n"), 0);
}

/* What this system calls the refusal of an open of a link is no matter: the
   write owner looks at the name.  FreeBSD says EMLINK, NetBSD EFTYPE.  Red on
   a tree that reports the open's errno as it came: EMLINK goes through and
   no link is said to have taken the name.  Also a name that is a plain file
   is not taken for a link whatever the open said. */
static void test_the_assistants_write_says_a_link_took_the_name_whatever_the_system_says(void)
{
 static const int words[] = {
#ifdef EMLINK
  EMLINK,
#endif
#ifdef EFTYPE
  EFTYPE,
#endif
  ELOOP
 };
 WpeWriteOutcome o;
 char p[512], q[512], buf[256];
 size_t i;

 make_dir();
 put_file("other", "keep me\n", 0644);
 path_of(p, sizeof p, "t.c");
 path_of(q, sizeof q, "other");
 chk_int("the link", symlink(q, p), 0);
 for (i = 0; i < sizeof words / sizeof words[0]; i++)
 {
  snprintf(open_fail_at, sizeof open_fail_at, "%s", p);
  open_fail_with = words[i];
  chk_int("refused", wpe_write_save_text(p, "new\n", NULL, WPE_WRITE_NOFOLLOW, NULL, &o), -1);
  open_fail_at[0] = '\0';
  chk_int("a link took the name", o.replaced_by_link, 1);
  chk_int("one reason on every system", o.err, ELOOP);
 }
 chk_int("the file it leads to is untouched", strcmp(get_file("other", buf, sizeof buf), "keep me\n"), 0);
 unlink(p);
 put_file("t.c", "plain\n", 0644);
 snprintf(open_fail_at, sizeof open_fail_at, "%s", p);
 open_fail_with = EACCES;
 chk_int("a plain file refused", wpe_write_save_text(p, "new\n", NULL, WPE_WRITE_NOFOLLOW, NULL, &o), -1);
 open_fail_at[0] = '\0';
 chk_int("no link took the name", o.replaced_by_link, 0);
 chk_int("the system's own reason", o.err, EACCES);
}

/* @scenario SC-write-owner-A-05: a failed save that left no backup names the
   special bit the file had and no longer has, read back from the file. */
static void test_a_failed_save_with_no_backup_names_the_lost_bit(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char p[512], say[1024];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "a failed copy names the lost bit",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("t.c", "old\n", 02755);
 f = window_of_file_with_mode(0755);
 set_rows(big(4096, 'n'));
 path_of(p, sizeof p, "t.c");
 limit_file_size(2048);
 chk_int("the write fails", wpe_write_range(f, p, 0, 0, 1, 1, 0, &o), -1);
 no_file_size_limit();
 chk_int("no backup was made", o.bak_path[0], '\0');
 chk_int("the file lost its bit", mode_of("t.c") & 07000, 0);
 wpe_write_sentence(say, sizeof say, &o, WPE_WRITE_THEN_KEPT);
 chk_has("the sentence", say, "no backup copy was made, and the setgid bit it had is off.");
}

/* The same failure on a file whose bit the failed write did not take off
   (no mode of the window was applied, and the system keeps a setgid bit of a
   file with no group execute bit): the sentence says nothing of a bit. */
static void test_a_failed_save_that_left_the_bit_on_does_not_say_it_is_off(void)
{
 WpeWriteOutcome o;
 FENSTER *f = make_window();
 char p[512], say[1024];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "a failed copy that kept the bit",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("t.c", "old\n", 02644);
 set_rows(big(4096, 'n'));
 path_of(p, sizeof p, "t.c");
 limit_file_size(0);   /* the first byte fails: no write ever touched the file's mode */
 chk_int("the write fails", wpe_write_range(f, p, 0, 0, 1, 1, 0, &o), -1);
 no_file_size_limit();
 chk_int("no backup was made", o.bak_path[0], '\0');
 chk_int("the bit is still on", mode_of("t.c") & 07000, 02000);
 wpe_write_sentence(say, sizeof say, &o, WPE_WRITE_THEN_KEPT);
 chk_has("the file may be cut short", say, "may be cut short");
 chk_lacks("no word of a bit", say, "it had");
 chk_lacks("no word of a bit being off", say, "is off");
}

/* A failure before anything was written (the cut is refused) leaves the file
   and its bit as they were, and the sentence says no more than the reason. */
static void test_a_save_refused_before_the_bits_are_dropped_names_none(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char p[512], say[1024];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "a refused cut names no bit",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("t.c", "old\n", 02755);
 f = window_of_file_with_mode(0755);
 set_rows("new\n");
 path_of(p, sizeof p, "t.c");
 ftruncate_fails = 1;
 chk_int("refused", wpe_write_range(f, p, 0, 0, 1, 1, 0, &o), -1);
 ftruncate_fails = 0;
 chk_int("the file keeps its bit", mode_of("t.c"), 02755);
 wpe_write_sentence(say, sizeof say, &o, WPE_WRITE_THEN_KEPT);
 chk_has("the reason", say, "Operation not permitted");
 chk_lacks("no word of a bit", say, "it had");
}

/* @scenario SC-write-owner-A-06: no byte of a text goes into a file while a
   special bit is on it: the nine bits are given first, which takes the bit
   off, and the bit goes back after the text.  02644 has no group execute
   bit, so the system itself would keep the bit through the write. */
static void test_a_text_with_no_window_is_written_only_after_the_bit_is_off(void)
{
 WpeWriteOutcome o;
 char p[512];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "no byte while a bit is on",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("t.c", "old\n", 02644);
 path_of(p, sizeof p, "t.c");
 fsync_reset();
 chk_int("written", wpe_write_save_text(p, "new\n", NULL, WPE_WRITE_NOFOLLOW, NULL, &o), 0);
 chk_int("the first sync saw no special bit on the file", fsync_first_special, 0);
 chk_int("the bit is back after the text", mode_of("t.c"), 02644);
}

/* @scenario SC-write-owner-A-07: with no window, a thing that is not a
   regular file (the slave of a pty the user owns) is refused, with a
   sentence that says what it is and speaks of no window; nothing is written
   into it. */
static void test_the_assistants_write_refuses_a_file_that_is_not_regular(void)
{
 WpeWriteOutcome o;
 char *slave_name, say[1024];
 int master, slave;

 make_dir();
 master = posix_openpt(O_RDWR | O_NOCTTY);
 if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0 || (slave_name = ptsname(master)) == NULL)
 {
  printf("SKIP: the assistant's write refuses a non-regular file: no pty here\n");
  if (master >= 0)
   close(master);
  return;
 }
 slave = open(slave_name, O_RDWR | O_NOCTTY);
 chk_int("refused", wpe_write_save_text(slave_name, "new\n", NULL, 0, NULL, &o), -1);
 chk_int("FAILED", o.status, WPE_WRITE_FAILED);
 chk_int("says it is not a regular file", o.not_regular, 1);
 chk_int("nothing was asked", question_count, 0);
 save_sentence(&o, say, sizeof say);
 chk_has("the sentence", say, "it is not a regular file (a terminal).  Nothing was written.");
 chk_lacks("no word of a window", say, "this window read");
 if (slave >= 0)
  close(slave);
 close(master);
}

/* ---- the assistant's write held to the file the judge looked at ---- */

/** How long a row that could wait on a pipe may take before it is a failure. */
#define PIPE_ROW_LIFETIME_S 10

/* What the judge's look would keep of DIR/NAME: the file there, by device and
   inode, its names and whether it is regular; or that the name was free. */
static WpeWriteExpect look_at(const char *name)
{
 WpeWriteExpect x;
 struct stat st;
 char p[512];

 memset(&x, 0, sizeof x);
 x.judged = 1;
 path_of(p, sizeof p, name);
 if (lstat(p, &st) != 0)
  return(x);
 x.exists = 1;
 (void)wpe_seen_look_name(p, 0, &x.seen);
 x.nlink = st.st_nlink;
 x.regular = S_ISREG(st.st_mode) ? 1 : 0;
 return(x);
}

/* S_IFMT of what holds DIR/NAME, itself and not what it leads to; -1 when nothing does. */
static long kind_of(const char *name)
{
 char p[512];
 struct stat st;

 path_of(p, sizeof p, name);
 return(lstat(p, &st) == 0 ? (long)(st.st_mode & S_IFMT) : -1L);
}

/* The bytes of DIR/NAME, NUL bytes included; the count, or -1 when it cannot be read. */
static long get_bytes(const char *name, char *buf, size_t n)
{
 char p[512];
 FILE *fp;
 size_t got;

 path_of(p, sizeof p, name);
 fp = fopen(p, "r");
 if (!fp)
  return(-1);
 got = fread(buf, 1, n, fp);
 fclose(fp);
 return((long)got);
}

/* @scenario SC-write-owner-H2a-01: a pipe under the name is never waited for
   and never written: the open does not block, the type is read from the
   file's own mode, and the outcome's fields (not the system's errno) say what
   it was.  Bounded by the watchdog: on a tree that opens with plain
   O_WRONLY the open of a pipe with no reader hangs here and the row fails by
   the alarm. */
static void test_the_assistants_write_refuses_a_pipe_and_does_not_wait(void)
{
 WpeWriteOutcome o;
 WpeWriteExpect look;
 char p[512], say[1024];
 int reader;

 make_dir();
 path_of(p, sizeof p, "t.c");
 chk_int("the pipe", mkfifo(p, 0600), 0);
 look = look_at("t.c");
 test_watchdog_case("test_the_assistants_write_refuses_a_pipe_and_does_not_wait", PIPE_ROW_LIFETIME_S);
 chk_int("refused with no reader", wpe_write_save_text(p, "new\n", NULL, WPE_WRITE_NOFOLLOW, &look, &o), -1);
 chk_int("FAILED", o.status, WPE_WRITE_FAILED);
 chk_int("says it is not a regular file", o.not_regular, 1);
 chk_int("and that it is a pipe", (long)o.not_regular_kind, (long)S_IFIFO);
 chk_int("not taken for a link", o.replaced_by_link, 0);
 save_sentence(&o, say, sizeof say);
 chk_has("the sentence names a pipe", say, "it is not a regular file (a pipe).  Nothing was written.");
 reader = open(p, O_RDONLY | O_NONBLOCK);
 chk_int("a reader is there", reader >= 0, 1);
 chk_int("refused with a reader", wpe_write_save_text(p, "new\n", NULL, WPE_WRITE_NOFOLLOW, &look, &o), -1);
 chk_int("says it is not a regular file again", o.not_regular, 1);
 chk_int("and that it is a pipe again", (long)o.not_regular_kind, (long)S_IFIFO);
 if (reader >= 0)
  close(reader);
 chk_int("a revert refuses it the same way", wpe_write_restore_text(p, "old\n", 4, 0644, NULL, &look, &o), -1);
 chk_int("revert: not a regular file", o.not_regular, 1);
 chk_int("the pipe is still a pipe", kind_of("t.c"), (long)S_IFIFO);
 chk_int("no backup of a pipe", mode_of("t.bak"), -1);
 test_watchdog_disarm();
}

/* @scenario SC-write-owner-H2a-02: a file swapped in under the name after the
   look is not written: the file opened must be the file looked at. */
static void test_the_assistants_write_refuses_a_file_swapped_in_after_the_look(void)
{
 WpeWriteOutcome o;
 WpeWriteExpect look;
 char p[512], buf[256], say[1024];

 make_dir();
 put_file("t.c", "old\n", 0644);
 look = look_at("t.c");
 another_file_takes_the_name("t.c", "theirs\n", 0644);
 path_of(p, sizeof p, "t.c");
 chk_int("refused", wpe_write_save_text(p, "new\n", NULL, WPE_WRITE_NOFOLLOW, &look, &o), -1);
 chk_int("FAILED", o.status, WPE_WRITE_FAILED);
 chk_int("says it is not the file that was checked", o.swapped, 1);
 chk_int("their text is untouched", strcmp(get_file("t.c", buf, sizeof buf), "theirs\n"), 0);
 chk_int("no backup of a file that was not written", mode_of("t.bak"), -1);
 save_sentence(&o, say, sizeof say);
 chk_has("the sentence", say, "it is not the file that was checked");
 chk_int("a revert is held to its look too",
         wpe_write_restore_text(p, "old\n", 4, 0644, NULL, &look, &o), -1);
 chk_int("revert: not the file that was checked", o.swapped, 1);
 chk_int("their text is still untouched", strcmp(get_file("t.c", buf, sizeof buf), "theirs\n"), 0);
}

/* @scenario SC-write-owner-H2a-03: a name the file gained after the look is a
   refusal: a hard link made while the question was up leads somewhere the
   user was not shown. */
static void test_the_assistants_write_refuses_a_name_gained_after_the_look(void)
{
 WpeWriteOutcome o;
 WpeWriteExpect look;
 char p[512], q[512], buf[256], say[1024];

 make_dir();
 put_file("t.c", "old\n", 0644);
 look = look_at("t.c");
 path_of(p, sizeof p, "t.c");
 path_of(q, sizeof q, "h");
 chk_int("the other name", link(p, q), 0);
 chk_int("refused", wpe_write_save_text(p, "new\n", NULL, WPE_WRITE_NOFOLLOW, &look, &o), -1);
 chk_int("says the file gained a name", o.gained_name, 1);
 chk_int("not another file", o.swapped, 0);
 chk_int("the file is untouched", strcmp(get_file("t.c", buf, sizeof buf), "old\n"), 0);
 chk_int("through its other name too", strcmp(get_file("h", buf, sizeof buf), "old\n"), 0);
 save_sentence(&o, say, sizeof say);
 chk_has("the sentence", say, "gained another name after the check");
}

/* @scenario SC-write-owner-H2a-04: a name the look found free must be made by
   the write: a file that appeared there since is refused and never written
   (a hard link planted at a new name is taken by the exclusive create's
   re-open otherwise); a name still free is made, and a file that is the one
   looked at is written. */
static void test_the_assistants_write_of_a_free_name_must_make_the_file(void)
{
 WpeWriteOutcome o;
 WpeWriteExpect look;
 char p[512], buf[256];

 make_dir();
 look = look_at("fresh.c");
 chk_int("the look found nothing", look.exists, 0);
 put_file("fresh.c", "theirs\n", 0644);
 path_of(p, sizeof p, "fresh.c");
 chk_int("refused", wpe_write_save_text(p, "new\n", NULL, WPE_WRITE_NOFOLLOW, &look, &o), -1);
 chk_int("says it is not the file that was checked", o.swapped, 1);
 chk_int("their text is untouched", strcmp(get_file("fresh.c", buf, sizeof buf), "theirs\n"), 0);
 unlink(p);
 chk_int("a name still free is made", wpe_write_save_text(p, "new\n", NULL, WPE_WRITE_NOFOLLOW, &look, &o), 0);
 chk_int("the text is on disk", strcmp(get_file("fresh.c", buf, sizeof buf), "new\n"), 0);
 look = look_at("fresh.c");
 chk_int("a file that is the one looked at is written",
         wpe_write_save_text(p, "newer\n", NULL, WPE_WRITE_NOFOLLOW, &look, &o), 0);
 chk_int("the new text", strcmp(get_file("fresh.c", buf, sizeof buf), "newer\n"), 0);
 chk_int("the old text is in the backup", strcmp(get_file("fresh.bak", buf, sizeof buf), "new\n"), 0);
}

/* @scenario SC-write-owner-H2a-14: a file the look saw and that was removed
   before the open is not made again as an empty file: the write is refused
   with its own sentence and the name is still absent, for a write and for a
   revert.  Red on a tree that takes the create path for it: the name holds an
   empty file afterwards and the sentence says another file took the name. */
static void test_a_file_removed_after_the_look_is_not_made_again(void)
{
 WpeWriteOutcome o;
 WpeWriteExpect look;
 char p[512], say[1024];

 make_dir();
 put_file("gone.c", "old\n", 0644);
 look = look_at("gone.c");
 path_of(p, sizeof p, "gone.c");
 chk_int("removed", unlink(p), 0);
 chk_int("refused", wpe_write_save_text(p, "new\n", NULL, WPE_WRITE_NOFOLLOW, &look, &o), -1);
 chk_int("says it was removed", o.vanished, 1);
 chk_int("not another file", o.swapped, 0);
 chk_int("the name is still absent", kind_of("gone.c"), -1);
 chk_int("no backup", mode_of("gone.bak"), -1);
 save_sentence(&o, say, sizeof say);
 chk_has("the sentence", say, "the file was removed after the check.  Nothing was written.");
 chk_int("a revert is refused too", wpe_write_restore_text(p, "old\n", 4, 0644, NULL, &look, &o), -1);
 chk_int("revert: says it was removed", o.vanished, 1);
 chk_int("revert: the name is still absent", kind_of("gone.c"), -1);
}

/* @scenario SC-write-owner-H2a-15: a link at the name is refused with its own
   sentence, which a revert shows. */
static void test_a_link_at_the_name_is_refused_and_named(void)
{
 WpeWriteOutcome o;
 char p[512], q[512], buf[256], say[1024];

 make_dir();
 put_file("victim", "USER DATA\n", 0644);
 path_of(p, sizeof p, "t.c");
 path_of(q, sizeof q, "victim");
 chk_int("the link", symlink(q, p), 0);
 chk_int("refused", wpe_write_restore_text(p, "old\n", 4, WPE_WRITE_NO_RECORDED_MODE, NULL, NULL, &o), -1);
 chk_int("a link took the name", o.replaced_by_link, 1);
 save_sentence(&o, say, sizeof say);
 chk_has("the sentence", say, "a link has taken the name.  Nothing was written.");
 chk_int("the file it leads to is untouched", strcmp(get_file("victim", buf, sizeof buf), "USER DATA\n"), 0);
}

/* @scenario SC-write-owner-H2a-05: the assistant's write with the look of the
   file is a Save in every respect: the bit back, the .bak made. */
static void test_the_assistants_write_with_the_look_keeps_the_bit_and_makes_the_backup(void)
{
 WpeWriteOutcome o;
 WpeWriteExpect look;
 char p[512], buf[256];

 make_dir();
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "the assistant's write with a look",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("t.c", "old\n", 02755);
 look = look_at("t.c");
 path_of(p, sizeof p, "t.c");
 chk_int("written", wpe_write_save_text(p, "new\n", NULL, WPE_WRITE_NOFOLLOW, &look, &o), 0);
 chk_int("the new text", strcmp(get_file("t.c", buf, sizeof buf), "new\n"), 0);
 chk_int("the file keeps its setgid bit", mode_of("t.c"), 02755);
 chk_int("the old text is in the backup", strcmp(get_file("t.bak", buf, sizeof buf), "old\n"), 0);
 chk_int("the backup is reported", strcmp(o.bak_path + strlen(o.bak_path) - strlen("t.bak"), "t.bak"), 0);
}

/* @scenario SC-write-owner-H2a-06: a before image that holds a NUL byte goes
   back whole, the length travelling with the text. */
static void test_a_revert_puts_back_a_before_image_that_holds_a_nul_byte(void)
{
 static const char image[] = { 'a', '\0', 'b', '\n' };
 WpeWriteOutcome o;
 char p[512], buf[64];

 make_dir();
 put_file("t.c", "agent\n", 0644);
 path_of(p, sizeof p, "t.c");
 chk_int("restored", wpe_write_restore_text(p, image, sizeof image, WPE_WRITE_NO_RECORDED_MODE, NULL,
                                            NULL, &o), 0);
 chk_int("all four bytes are back", get_bytes("t.c", buf, sizeof buf), (long)sizeof image);
 chk_int("the same bytes", memcmp(buf, image, sizeof image), 0);
}

/* @scenario SC-write-owner-H2a-07: a file with no recorded mode (a command
   changed it) is put back wearing the bits it has, never another file's. */
static void test_a_revert_with_no_recorded_mode_keeps_the_files_own_bits(void)
{
 WpeWriteOutcome o;
 char p[512], buf[256];

 make_dir();
 put_file("t.c", "agent\n", 0600);
 path_of(p, sizeof p, "t.c");
 chk_int("restored", wpe_write_restore_text(p, "old\n", 4, WPE_WRITE_NO_RECORDED_MODE, NULL, NULL, &o), 0);
 chk_int("the old text is back", strcmp(get_file("t.c", buf, sizeof buf), "old\n"), 0);
 chk_int("its own nine bits", mode_of("t.c"), 0600);
 if (!test_premise(test_premise_special_bits_settable(dir, S_ISGID), "a revert with no mode keeps a bit",
                   "this user cannot set setgid in the test folder"))
  return;
 put_file("p.c", "agent\n", 02755);
 path_of(p, sizeof p, "p.c");
 chk_int("restored a program", wpe_write_restore_text(p, "old\n", 4, WPE_WRITE_NO_RECORDED_MODE, NULL, NULL, &o), 0);
 chk_int("its own bits, the special one too", mode_of("p.c"), 02755);
 chk_int("no backup is made", mode_of("p.bak"), -1);
}

/* @scenario SC-write-owner-H2a-13: what the assistant is told.  A save that
   worked but could not keep a bit gives the clause that follows "written, but",
   and one that kept everything gives none; a failure gives the owner's
   sentence with nothing about "your text in the window", which the assistant
   has none of. */
static void test_the_assistants_sentences_say_a_lost_bit_and_keep_no_window_text(void)
{
 WpeWriteOutcome o;
 char clause[512], say[1024];

 memset(&o, 0, sizeof o);
 o.status = WPE_WRITE_WRITTEN;
 snprintf(o.path, sizeof o.path, "/w/t.c");
 chk_int("a save that kept everything has no clause", wpe_write_written_clause(clause, sizeof clause, &o), 0);
 o.file_kept.lost = S_ISGID;
 o.file_kept.err = EPERM;
 chk_int("a lost bit has a clause", wpe_write_written_clause(clause, sizeof clause, &o) > 0, 1);
 chk_has("the clause names the bit", clause, "the setgid bit was not kept: ");
 o.backup_err = EACCES;
 (void)wpe_write_written_clause(clause, sizeof clause, &o);
 chk_has("and the missing backup", clause, "; no backup was made: ");
 memset(&o, 0, sizeof o);
 o.status = WPE_WRITE_FAILED;
 o.err = ENOSPC;
 snprintf(o.path, sizeof o.path, "/w/t.c");
 (void)wpe_write_sentence(say, sizeof say, &o, WPE_WRITE_THEN_NOTHING_MORE);
 chk_has("a failure names the path", say, "Cannot write /w/t.c: ");
 chk_lacks("and speaks of no window text", say, "Your text is still in the window");
 chk_int("and ends with its own full stop", say[strlen(say) - 1], '.');
 (void)wpe_write_sentence(say, sizeof say, &o, WPE_WRITE_THEN_KEPT);
 chk_has("a Save still says its text is kept", say, "Your text is still in the window");
}

static void stub_mouse_shape(WpeMouseShape s) { (void)s; }
static void stub_mouse_restore(void) { }

/* ---- the editor's own folders and files (we_write.c, we_places.c) ---- */

/** The variables the places are read from, kept so a row puts back what it found. */
typedef struct {
 const char *name;
 char       *saved;     /* a copy of the value, NULL when it was not set */
} EnvKeep;

static EnvKeep env_kept[] = {
 { "HOME", NULL }, { "XDG_STATE_HOME", NULL }, { "XDG_CONFIG_HOME", NULL }, { "XWPE_TEST_FAIL", NULL }
};
#define ENV_KEPT_N ((int)(sizeof env_kept / sizeof env_kept[0]))

/** env_keep_all - Remember the variables the places are read from. */
static void env_keep_all(void)
{
 int i;

 for (i = 0; i < ENV_KEPT_N; i++)
 {
  const char *v = getenv(env_kept[i].name);

  free(env_kept[i].saved);
  env_kept[i].saved = v != NULL ? strdup(v) : NULL;
 }
}

/** env_restore_all - Put back the variables env_keep_all remembered. */
static void env_restore_all(void)
{
 int i;

 for (i = 0; i < ENV_KEPT_N; i++)
 {
  if (env_kept[i].saved != NULL)
   setenv(env_kept[i].name, env_kept[i].saved, 1);
  else
   unsetenv(env_kept[i].name);
 }
}

/** own_row_start - A fresh folder, the state folder's base below it, and the
    run folder named: what every row of the own folders begins with. */
static void own_row_start(void)
{
 char state[512], run[512];

 env_keep_all();
 make_dir();
 path_of(state, sizeof state, "state");
 path_of(run, sizeof run, "run");
 setenv("XDG_STATE_HOME", state, 1);
 if (mkdir(run, 0700) != 0)
  perror("mkdir run");
 wpe_run_dir_set(run);
}

/** own_row_end - Put the environment back. */
static void own_row_end(void)
{
 env_restore_all();
 wpe_run_dir_set(NULL);
}

/** read_at - A file's text by its full path, "" when it cannot be read. */
static char *read_at(const char *path, char *buf, size_t n)
{
 FILE *fp = fopen(path, "r");
 size_t got = 0;

 buf[0] = '\0';
 if (fp == NULL)
  return(buf);
 got = fread(buf, 1, n - 1, fp);
 buf[got] = '\0';
 fclose(fp);
 return(buf);
}

/** mode_at - The twelve bits of a path (a link not followed), or -1. */
static int mode_at(const char *path)
{
 struct stat st;

 return(lstat(path, &st) == 0 ? (int)(st.st_mode & 07777) : -1);
}

/** size_at - The size of a path in bytes, or -1. */
static long size_at(const char *path)
{
 struct stat st;

 return(lstat(path, &st) == 0 ? (long)st.st_size : -1L);
}

/** state_path - A path below the state folder the row made, "<dir>/state/xwpe/<tail>". */
static void state_path(char *out, size_t n, const char *tail)
{
 snprintf(out, n, "%s/state/xwpe/%s", dir, tail);
}

/* @scenario SC-chosen-names-01: an own file is never written through a link. */
static void test_an_own_file_is_never_written_through_a_link(void)
{
 WpeWriteOutcome o;
 char victim[512], link[512], buf[64];

 own_row_start();
 put_file("victim", "KEEP\n", 0644);
 path_of(victim, sizeof victim, "victim");
 chk_int("the folder is made", wpe_write_own_bytes(WPE_OWN_STATE, "ai", "first", "x", 1, &o), 0);
 state_path(link, sizeof link, "ai/x.session");
 if (symlink(victim, link) != 0)
  perror("symlink");
 chk_int("the write is refused", wpe_write_own_bytes(WPE_OWN_STATE, "ai", "x.session", "evil", 4, &o), -1);
 chk_int("it says something else is in the name", o.planted, 1);
 chk_int("status", o.status, WPE_WRITE_FAILED);
 chk_has("the victim keeps its text", read_at(victim, buf, sizeof buf), "KEEP");
 chk_int("the link is still a link", name_is_link(link), 1);
 own_row_end();
}

/* @scenario SC-chosen-names-02: a link that leads nowhere creates nothing. */
static void test_an_own_file_at_a_dangling_link_creates_nothing(void)
{
 WpeWriteOutcome o;
 char target[512], link[512];

 own_row_start();
 chk_int("the folder is made", wpe_write_own_bytes(WPE_OWN_STATE, "ai", "first", "x", 1, &o), 0);
 path_of(target, sizeof target, "made");
 state_path(link, sizeof link, "ai/x.session");
 if (symlink(target, link) != 0)
  perror("symlink");
 chk_int("the write is refused", wpe_write_own_bytes(WPE_OWN_STATE, "ai", "x.session", "evil", 4, &o), -1);
 chk_int("the file the link names was not made", mode_at(target), -1);
 own_row_end();
}

/* @scenario SC-chosen-names-03: an old own file is replaced by a new private file. */
static void test_an_old_own_file_is_replaced_by_a_new_private_file(void)
{
 WpeWriteOutcome o;
 char file[512], buf[64];
 int held;

 own_row_start();
 chk_int("the folder is made", wpe_write_own_bytes(WPE_OWN_STATE, "ai", "first", "x", 1, &o), 0);
 state_path(file, sizeof file, "ai/x.session");
 test_put_mode(dir, "state/xwpe/ai/x.session", "old", 0644);
 held = open(file, O_RDONLY);
 chk_int("the old file is open", held >= 0, 1);
 chk_int("written", wpe_write_own_bytes(WPE_OWN_STATE, "ai", "x.session", "new", 3, &o), 0);
 chk_int("status", o.status, WPE_WRITE_WRITTEN);
 chk_has("the name reads the new text", read_at(file, buf, sizeof buf), "new");
 chk_int("the new file is private", mode_at(file), 0600);
 if (held >= 0)
 {
  ssize_t got = pread(held, buf, sizeof buf - 1, 0);

  buf[got > 0 ? got : 0] = '\0';
  chk_has("the held old file still reads the old text", buf, "old");
  close(held);
 }
 own_row_end();
}

/** put_dir_mode - Make a folder with exactly this mode, whatever the umask. */
static void put_dir_mode(const char *path, mode_t mode)
{
 if (mkdir(path, mode) != 0 && errno != EEXIST)
  perror("mkdir");
 if (chmod(path, mode) != 0)
  perror("chmod");
}

/* @scenario SC-chosen-names-04: the state folder is made the user's alone. */
static void test_the_state_folder_is_made_the_users_alone(void)
{
 WpeOwnDir d;
 WpeWriteOutcome o;
 char base[512], ai[512], state[512];

 own_row_start();
 state_path(base, sizeof base, "");
 base[strlen(base) - 1] = '\0';                 /* "<dir>/state/xwpe" */
 path_of(state, sizeof state, "state");
 state_path(ai, sizeof ai, "ai");
 chk_int("an absent base is made", wpe_own_dir(WPE_OWN_STATE, "ai", WPE_OWN_MAKE, &d, &o), 0);
 chk_int("the base is 0700", mode_at(base), 0700);
 chk_int("the folder below is 0700", mode_at(ai), 0700);
 chk_int("the folder above the base is 0700 too", mode_at(state), 0700);
 put_dir_mode(base, 0755);
 put_dir_mode(ai, 0755);
 chk_int("a folder that others read is accepted", wpe_own_dir(WPE_OWN_STATE, "ai", WPE_OWN_MAKE, &d, &o), 0);
 chk_int("the base is made 0700", mode_at(base), 0700);
 chk_int("the folder below is made 0700", mode_at(ai), 0700);
 chk_has("the path says where it is", d.path, "/state/xwpe/ai");
 own_row_end();
}

/* @scenario SC-chosen-names-04b: another user's folder, as the base or below it, is refused. */
static void test_another_users_folder_is_refused(void)
{
 WpeOwnDir d;
 WpeWriteOutcome o;
 struct stat st;

 own_row_start();
 if (geteuid() == 0 || stat("/usr", &st) != 0 || st.st_uid == geteuid())
 {
  printf("SKIP: another user's folder is refused: no folder of another user to use\n");
  own_row_end();
  return;
 }
 wpe_run_dir_set("/usr");
 chk_int("another user's base is refused", wpe_own_dir(WPE_OWN_RUN, NULL, WPE_OWN_MAKE, &d, &o), -1);
 chk_int("it is said as not ours", o.planted, 1);
 wpe_run_dir_set("/");
 chk_int("another user's folder below the base is refused", wpe_own_dir(WPE_OWN_RUN, "usr", WPE_OWN_MAKE, &d, &o), -1);
 chk_int("it is said as not ours too", o.planted, 1);
 chk_int("a file of ours is not written there", wpe_write_own_bytes(WPE_OWN_RUN, "usr", "x", "x", 1, &o), -1);
 own_row_end();
}

/* @scenario SC-chosen-names-05: a link below the state folder is refused, a link at the base is followed. */
static void test_a_link_below_the_state_folder_is_refused(void)
{
 WpeOwnDir d;
 WpeWriteOutcome o;
 char elsewhere[512], base[512], ai[512], real[512], buf[64], say[1024];

 own_row_start();
 path_of(elsewhere, sizeof elsewhere, "elsewhere");
 put_dir_mode(elsewhere, 0700);
 chk_int("the base is made", wpe_own_dir(WPE_OWN_STATE, NULL, WPE_OWN_MAKE, &d, &o), 0);
 state_path(ai, sizeof ai, "ai");
 if (symlink(elsewhere, ai) != 0)
  perror("symlink");
 chk_int("a link below the base is refused", wpe_own_dir(WPE_OWN_STATE, "ai", WPE_OWN_MAKE, &d, &o), -1);
 chk_int("it is said as not ours", o.planted, 1);
 wpe_write_sentence(say, sizeof say, &o, WPE_WRITE_THEN_NOTHING_MORE);
 chk_has("the sentence names the folder", say, "ai");
 chk_int("a write through it is refused", wpe_write_own_bytes(WPE_OWN_STATE, "ai", "x.session", "x", 1, &o), -1);
 snprintf(real, sizeof real, "%s/x.session", elsewhere);
 chk_int("nothing went to the folder the link names", mode_at(real), -1);
 /* a link at the base itself is the user's own choice and is followed */
 state_path(base, sizeof base, "");
 base[strlen(base) - 1] = '\0';
 unlink(ai);
 rmdir(base);
 put_dir_mode(elsewhere, 0755);
 if (symlink(elsewhere, base) != 0)
  perror("symlink base");
 chk_int("a link at the base is followed", wpe_write_own_bytes(WPE_OWN_STATE, "ai", "x.session", "kept", 4, &o), 0);
 snprintf(real, sizeof real, "%s/ai/x.session", elsewhere);
 chk_has("the file is in the folder the link names", read_at(real, buf, sizeof buf), "kept");
 chk_int("the folder it leads to is made 0700", mode_at(elsewhere), 0700);
 own_row_end();
}

/* @scenario SC-chosen-names-06: no place of the user's is ever relative. */
static void test_no_per_user_place_is_ever_relative(void)
{
 char buf[512];
 int rc;

 env_keep_all();
 unsetenv("HOME");
 unsetenv("XDG_STATE_HOME");
 unsetenv("XDG_CONFIG_HOME");
 setenv("XWPE_TEST_FAIL", "no-passwd-home", 1);
 chk_int("no home, no state folder", wpe_state_dir(buf, sizeof buf), -1);
 chk_int("no home, no configuration folder", wpe_config_dir(buf, sizeof buf), -1);
 chk_int("no home, no home", wpe_user_home(buf, sizeof buf), -1);
 setenv("HOME", "rel", 1);
 setenv("XDG_STATE_HOME", "rel2", 1);
 setenv("XDG_CONFIG_HOME", "rel3", 1);
 chk_int("a relative home is no place", wpe_state_dir(buf, sizeof buf), -1);
 chk_int("a relative configuration base is no place", wpe_config_dir(buf, sizeof buf), -1);
 unsetenv("XWPE_TEST_FAIL");
 unsetenv("XDG_STATE_HOME");
 unsetenv("HOME");
 rc = wpe_state_dir(buf, sizeof buf);
 chk_int("with the password file, a place is absolute or there is none", rc == -1 || buf[0] == '/', 1);
 setenv("HOME", "/h", 1);
 chk_int("an absolute home gives the state folder", wpe_state_dir(buf, sizeof buf), 0);
 chk_has("the state folder", buf, "/h/.local/state/xwpe");
 setenv("XDG_STATE_HOME", "/x", 1);
 chk_int("an absolute base wins", wpe_state_dir(buf, sizeof buf), 0);
 chk_has("the state folder below the base", buf, "/x/xwpe");
 unsetenv("XDG_CONFIG_HOME");
 chk_int("the configuration folder", wpe_config_dir(buf, sizeof buf), 0);
 chk_has("below home", buf, "/h/.config/xwpe");
 env_restore_all();
}

/* @scenario SC-chosen-names-26: a look makes nothing; the faces refuse what they should not take. */
static void test_a_look_makes_nothing_and_a_bad_name_is_refused(void)
{
 WpeOwnDir d;
 WpeWriteOutcome o;
 char base[512], file[512];

 own_row_start();
 state_path(base, sizeof base, "");
 base[strlen(base) - 1] = '\0';
 chk_int("a look at a missing folder fails", wpe_own_dir(WPE_OWN_STATE, "ai", WPE_OWN_LOOK, &d, &o), -1);
 chk_int("with no word, only ENOENT", o.err == ENOENT && !o.planted && !o.no_place, 1);
 chk_int("and nothing was made", mode_at(base), -1);
 chk_int("a sub with a dot dot is refused", wpe_own_dir(WPE_OWN_STATE, "../x", WPE_OWN_MAKE, &d, &o), -1);
 chk_int("three components are refused", wpe_own_dir(WPE_OWN_STATE, "a/b/c", WPE_OWN_MAKE, &d, &o), -1);
 chk_int("a leaf with a slash is refused", wpe_write_own_bytes(WPE_OWN_STATE, "ai", "a/b", "x", 1, &o), -1);
 chk_int("it is EINVAL", o.err, EINVAL);
 chk_int("a leaf of dots is refused", wpe_write_own_bytes(WPE_OWN_STATE, "ai", "..", "x", 1, &o), -1);
 chk_int("a NUL byte is written whole", wpe_write_own_bytes(WPE_OWN_STATE, "ai", "n.bin", "a\0b", 3, &o), 0);
 state_path(file, sizeof file, "ai/n.bin");
 chk_int("the file is private", mode_at(file), 0600);
 chk_int("three bytes are there", (int)size_at(file), 3);
 own_row_end();
}

/* @scenario SC-chosen-names-27: with no home folder there is no place, and it is said. */
static void test_with_no_home_a_write_says_there_is_no_place(void)
{
 WpeWriteOutcome o;
 char say[1024];

 env_keep_all();
 unsetenv("HOME");
 unsetenv("XDG_STATE_HOME");
 setenv("XWPE_TEST_FAIL", "no-passwd-home", 1);
 chk_int("a write with no state folder is refused", wpe_write_own_bytes(WPE_OWN_STATE, "ai", "x.session", "x", 1, &o), -1);
 chk_int("it says there is no place", o.no_place, 1);
 chk_int("and it made no folder under the working folder", mode_at(".local"), -1);
 wpe_write_sentence(say, sizeof say, &o, WPE_WRITE_THEN_NOTHING_MORE);
 chk_has("the sentence", say, "no home folder is known");
 env_restore_all();
}

/* @scenario SC-chosen-names-28: only a regular file of ours is removed. */
static void test_remove_takes_only_a_regular_file_of_ours(void)
{
 WpeWriteOutcome o;
 char file[512], link[512], victim[512], buf[64];

 own_row_start();
 chk_int("a file is made", wpe_write_own_bytes(WPE_OWN_STATE, "ai", "x.session", "x", 1, &o), 0);
 state_path(file, sizeof file, "ai/x.session");
 wpe_write_own_remove(WPE_OWN_STATE, "ai", "x.session");
 chk_int("our file is removed", mode_at(file), -1);
 put_file("victim", "KEEP\n", 0644);
 path_of(victim, sizeof victim, "victim");
 state_path(link, sizeof link, "ai/y.session");
 if (symlink(victim, link) != 0)
  perror("symlink");
 wpe_write_own_remove(WPE_OWN_STATE, "ai", "y.session");
 chk_int("a link is left alone", name_is_link(link), 1);
 chk_has("and what it names", read_at(victim, buf, sizeof buf), "KEEP");
 wpe_write_own_remove(WPE_OWN_STATE, "nothere", "x");
 state_path(link, sizeof link, "nothere");
 chk_int("a remove makes no folder", mode_at(link), -1);
 own_row_end();
}

/* ---- class name-identity: the file the window read, not a file that was given its inode number ----

   A file system gives an inode number back after its file is gone, so the
   rows that need it run on a disk (the build tree, or XWPE_TEST_REUSE_DIR),
   and say which file system they ran on.  A file system that never gives a
   number back is a SKIP with that reason; a file system that does, where the
   number was not reached, is a FAIL. */

/** Make the folder of a reuse row on a disk and keep it for the clean-up. */
static void make_reuse_dir(void)
{
 char whole[sizeof dir];

 if (seen_rows_dir(dir, sizeof dir, "wo") != 0 || realpath(dir, whole) == NULL)
 {
  remove_made_dirs();
  exit(2);
 }
 snprintf(dir, sizeof dir, "%s", whole);     /* absolute: a link made to a file here leads to it */
 remember_made_dir();
}

/** Say where a reuse row ran, so a log shows the row was not skipped. */
static void say_reuse_ran(const char *row)
{
 char fs[SEEN_ROWS_NAME_SIZE];

 seen_rows_fs_name(dir, fs, sizeof fs);
 printf("RAN %s: fs=%s\n", row, fs);
}

/** What a reuse row hands the callbacks: the window that reads the old file. */
typedef struct {
 FENSTER *f;
 const char *name;
} ReuseWindow;

/** look_old of the windowed rows: the window reads the old file while it exists. */
static void reuse_window_reads(const char *path, void *ctx)
{
 ReuseWindow *w = (ReuseWindow *)ctx;

 (void)path;
 window_reads(w->f, w->name);
}

/** look_old of the assistant's row: the look the write is held to. */
static void reuse_look_at(const char *path, void *ctx)
{
 (void)path;
 *(WpeWriteExpect *)ctx = look_at("t.c");
}

/** look_old of the copy row: the identity of the copy this window wrote. */
static void reuse_copy_identity(const char *path, void *ctx)
{
 (void)path;
 (void)wpe_write_identity_of_name(dir, "t.c.ASV", (WpeFileIdentity *)ctx);
}

/**
 * make_the_number_come_again - Give a new file the number of the old one that is gone.
 * @row: what the row is called in the log; @pc: the case (old object, its look, new object).
 * @name: where both are made.
 * Return: 1 when the new file holds the old number; 0 after a SKIP (the file
 * system never gives a number back) or a FAIL (it does, and the row could not
 * make it come again).
 */
static int make_the_number_come_again(const char *row, const SeenRowsCase *pc, const char *name)
{
 if (seen_rows_reuse_the_number(dir, name, pc))
 {
  say_reuse_ran(row);
  return(1);
 }
 if (seen_rows_never_gives_back(dir))
 {
  printf("SKIP: %s: this file system never gives a number back\n", row);
  return(0);
 }
 printf("FAIL: %s: the number was not reached on a file system that gives numbers back\n", row);
 fails++;
 return(0);
}

/** A case whose old and new objects are files of the given texts, the window reading the old one. */
static void reuse_case_of_files(SeenRowsCase *pc, const char *old_text, void (*look)(const char *, void *),
                                void *look_ctx, const char *new_text)
{
 memset(pc, 0, sizeof *pc);
 pc->make_old = seen_rows_make_file_text;
 pc->old_ctx = (void *)old_text;
 pc->look_old = look;
 pc->look_ctx = look_ctx;
 pc->make_new = seen_rows_make_file_text;
 pc->new_ctx = (void *)new_text;
}

/* @scenario SC-name-identity-02: a file that took the window's inode number is
   another file: a plain Save asks and No writes nothing. */
static void test_a_file_that_has_the_windows_inode_number_is_asked_about(void)
{
 ReuseWindow w;
 SeenRowsCase pc;
 WpeWriteOutcome o;
 char buf[256];

 make_reuse_dir();
 w.f = window_of_file_with_mode(0600);
 w.name = "t.c";
 reuse_case_of_files(&pc, "old\n", reuse_window_reads, &w, "theirs\n");
 if (!make_the_number_come_again("row 2 (plain Save, answer N)", &pc, "t.c"))
  return;
 set_rows("mine\n");
 question_answer = 'N';
 chk_int("declined", save_window(w.f, "t.c", &o), -1);
 chk_int("status DECLINED", o.status, WPE_WRITE_DECLINED);
 chk_int("one question", question_count, 1);
 chk_has("the question is the replaced one", question_text,
         "t.c was replaced on disk since this window read it. Overwrite it?");
 chk_has("their text is untouched", get_file("t.c", buf, sizeof buf), "theirs\n");
 chk_int("no backup", mode_of("t.bak"), -1);
}

/* @scenario SC-name-identity-02b: the same file with no one to ask is refused. */
static void test_a_write_with_no_one_to_ask_refuses_a_file_with_the_windows_number(void)
{
 ReuseWindow w;
 SeenRowsCase pc;
 WpeWriteOutcome o;
 char buf[256], p[512];

 make_reuse_dir();
 w.f = window_of_file_with_mode(0600);
 w.name = "t.c";
 reuse_case_of_files(&pc, "old\n", reuse_window_reads, &w, "theirs\n");
 if (!make_the_number_come_again("row 2b (no one to ask)", &pc, "t.c"))
  return;
 path_of(p, sizeof p, "t.c");
 chk_int("refused", wpe_write_save_text(p, "mine\n", &w.f->file_id, WPE_WRITE_NOFOLLOW, NULL, &o), -1);
 chk_int("status FAILED", o.status, WPE_WRITE_FAILED);
 chk_int("another file", o.other_file, 1);
 chk_int("not merely unsure", o.unsure, 0);
 chk_has("their text is untouched", get_file("t.c", buf, sizeof buf), "theirs\n");
 chk_int("no backup", mode_of("t.bak"), -1);
}

/** Add text to a file of the test folder without replacing it (the file is touched in place). */
static void append_in_place(const char *name, const char *text)
{
 char p[512];
 int fd;

 path_of(p, sizeof p, name);
 fd = open(p, O_WRONLY | O_APPEND);
 chk_int("open to append", fd >= 0, 1);
 if (fd < 0)
  return;
 chk_int("append", write(fd, text, strlen(text)) == (ssize_t)strlen(text), 1);
 close(fd);
}

/* @scenario SC-name-identity-03: a file the window read and that was touched in
   place since is still the window's file: Save writes it, with no question. */
static void test_a_touched_file_is_still_the_windows_file(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char buf[256];

 make_dir();
 put_file("t.c", "old\n", 0644);
 f = window_of_file_with_mode(0644);
 window_reads(f, "t.c");
 append_in_place("t.c", "more\n");
 set_rows("mine\n");
 chk_int("write", save_window(f, "t.c", &o), 0);
 chk_int("nothing was asked", question_count, 0);
 chk_has("the text is on disk", get_file("t.c", buf, sizeof buf), "mine\n");
}

/* @scenario SC-name-identity-04: the same on a system with no birth time (played
   by XWPE_TEST_FAIL=seen-no-birth): the file cannot be told from a new one, so
   it is not written silently.  With no one to ask it is refused with the
   "changed or replaced" sentence; a plain Save asks, No writes nothing, Yes
   writes. */
static void test_a_touched_file_without_a_birth_time_is_asked_about(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char buf[256], say[1024], p[512];

 make_dir();
 chk_int("the hook is set", setenv(WPE_SEEN_TEST_FAIL_ENV, WPE_SEEN_TEST_FAIL_NO_BIRTH, 1), 0);
 put_file("t.c", "old\n", 0644);
 f = window_of_file_with_mode(0644);
 window_reads(f, "t.c");
 append_in_place("t.c", "more\n");
 path_of(p, sizeof p, "t.c");
 chk_int("refused when there is no one to ask",
         wpe_write_save_text(p, "mine\n", &f->file_id, 0, NULL, &o), -1);
 chk_int("status FAILED", o.status, WPE_WRITE_FAILED);
 chk_int("not proven the window's file", o.other_file, 1);
 chk_int("unsure", o.unsure, 1);
 save_sentence(&o, say, sizeof say);
 chk_has("the sentence says changed or replaced", say, "was changed or replaced since this window read it");
 chk_has("their text is untouched", get_file("t.c", buf, sizeof buf), "old\nmore\n");
 set_rows("mine\n");
 question_answer = 'N';
 chk_int("No declines", save_window(f, "t.c", &o), -1);
 chk_int("status DECLINED", o.status, WPE_WRITE_DECLINED);
 chk_int("one question", question_count, 1);
 chk_has("the question", question_text, "t.c was changed or replaced since this window read it. Write anyway?");
 chk_has("nothing was written", get_file("t.c", buf, sizeof buf), "old\nmore\n");
 question_answer = 'Y';
 question_count = 0;
 chk_int("Yes writes", save_window(f, "t.c", &o), 0);
 chk_int("asked once", question_count, 1);
 chk_has("the text is on disk", get_file("t.c", buf, sizeof buf), "mine\n");
 unsetenv(WPE_SEEN_TEST_FAIL_ENV);
}

/* @scenario SC-name-identity-05: a second Save with nothing between is the
   window's own: the record is taken after the text, so the file the first Save
   wrote still matches it.  Run with and without a birth time; without one, a
   record made when the file was opened would not match (its ctime and size are
   old) and the second Save would ask. */
static void second_save_is_the_windows_own(const char *hook)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char buf[256];

 make_dir();
 if (hook != NULL)
  chk_int("the hook is set", setenv(WPE_SEEN_TEST_FAIL_ENV, hook, 1), 0);
 put_file("t.c", "old\n", 0644);
 f = window_of_file_with_mode(0644);
 window_reads(f, "t.c");
 set_rows("one\n");
 chk_int("first Save", save_window(f, "t.c", &o), 0);
 chk_int("first Save asked nothing", question_count, 0);
 set_rows("two\n");
 chk_int("second Save", save_window(f, "t.c", &o), 0);
 chk_int("second Save asked nothing", question_count, 0);
 chk_has("the text is on disk", get_file("t.c", buf, sizeof buf), "two\n");
 if (hook != NULL)
  unsetenv(WPE_SEEN_TEST_FAIL_ENV);
}

static void test_a_second_save_is_the_windows_own(void)
{
 second_save_is_the_windows_own(NULL);
 second_save_is_the_windows_own(WPE_SEEN_TEST_FAIL_NO_BIRTH);
}

/* @scenario SC-name-identity-08: the assistant's look is held to the file it saw,
   not to a file that took its number. */
static void test_a_look_is_not_met_by_a_file_with_its_number(void)
{
 WpeWriteExpect look;
 SeenRowsCase pc;
 WpeWriteOutcome o;
 char buf[256], p[512];

 make_reuse_dir();
 memset(&look, 0, sizeof look);
 reuse_case_of_files(&pc, "old\n", reuse_look_at, &look, "theirs\n");
 if (!make_the_number_come_again("row 8 (the assistant's look)", &pc, "t.c"))
  return;
 path_of(p, sizeof p, "t.c");
 chk_int("refused", wpe_write_save_text(p, "new\n", NULL, WPE_WRITE_NOFOLLOW, &look, &o), -1);
 chk_int("FAILED", o.status, WPE_WRITE_FAILED);
 chk_int("swapped", o.swapped, 1);
 chk_has("their text is untouched", get_file("t.c", buf, sizeof buf), "theirs\n");
 chk_int("no backup", mode_of("t.bak"), -1);
}

/* @scenario SC-name-identity-09: the copy a window made is removed only if it is
   still that copy, not a file that took its number. */
static void test_another_file_with_the_copys_number_is_not_removed(void)
{
 WpeFileIdentity own;
 SeenRowsCase pc;
 char p[512];

 make_reuse_dir();
 wpe_write_identity_forget(&own);
 reuse_case_of_files(&pc, "copy\n", reuse_copy_identity, &own, "theirs\n");
 if (!make_the_number_come_again("row 9 (copy removal)", &pc, "t.c.ASV"))
  return;
 path_of(p, sizeof p, "t.c.ASV");
 chk_int("the copy's identity was taken", own.known, 1);
 wpe_write_copy_remove(p, &own);
 chk_int("the other file is still there", mode_of("t.c.ASV"), 0600);
}

/** Row 6 is red until the mode owner moves onto the same judge.  It accepts
    only the one wrong answer known today (@known_wrong) and prints XFAIL; any
    other value is a failure, and so is the right one (XPASS): the pass that
    fixes the mode owner must drop this marker and assert @want. */
static void chk_int_until_the_mode_owner(const char *what, long got, long want, long known_wrong)
{
 if (got == known_wrong)
 {
  printf("XFAIL: %s: got %ld want %ld (the mode owner pass fixes it)\n", what, got, want);
  return;
 }
 if (got == want)
  printf("FAIL: XPASS: %s is right now; replace this marker with chk_int\n", what);
 else
  printf("FAIL: %s: got %ld, neither the known wrong %ld nor the wanted %ld\n", what, got,
         known_wrong, want);
 fails++;
}

/** What row 6 needs of the folder: the names of the sequence. */
typedef struct {
 char p[512], t[512], t_new[512];
 struct stat shown;
 WpeNameSeen seen;
} LinkRow;

/** make_old of row 6: the link z, which leads to t. */
static int make_the_link_to_t(const char *path, void *ctx)
{
 return(symlink(((LinkRow *)ctx)->t, path));
}

/** look_old of row 6: what the dialog showed, then t is replaced by rename. */
static void link_row_look(const char *path, void *ctx)
{
 LinkRow *r = (LinkRow *)ctx;

 put_file("t.new", "y\n", 0644);
 (void)stat(path, &r->shown);
 wpe_write_name_seen(path, &r->seen);
 chk_int("the target is replaced", rename(r->t_new, r->t), 0);
}

/* @scenario SC-name-identity-06: the sequence of the link row, with the number of
   the old link given back to another link: the new link is another one.  The
   mode owner still judges a name by its number, so the last line is expected
   red until that pass. */
static void test_a_link_that_took_the_dialogs_number_is_another_link(void)
{
 LinkRow r;
 SeenRowsCase pc;
 WpeModeKept k;
 char u[512];

 make_reuse_dir();
 put_file("t", "x\n", 0644);
 put_file("u", "z\n", 0644);
 path_of(r.p, sizeof r.p, "z");
 path_of(r.t, sizeof r.t, "t");
 path_of(r.t_new, sizeof r.t_new, "t.new");
 path_of(u, sizeof u, "u");
 memset(&pc, 0, sizeof pc);
 pc.make_old = make_the_link_to_t;
 pc.old_ctx = &r;
 pc.look_old = link_row_look;
 pc.look_ctx = &r;
 pc.make_new = seen_rows_make_link;
 pc.new_ctx = u;
 if (!make_the_number_come_again("row 6 (another link with the number)", &pc, "z"))
  return;
 chk_int("refused for another link", wpe_write_set_perm_seen(r.p, &r.shown, &r.seen, 0600, &k), ESTALE);
 chk_int_until_the_mode_owner("by a link: it is another one", k.replaced_kind, S_IFLNK, 0);
 chk_int("the file it leads to is as it was", mode_of("u"), 0644);
}

/* ---- a file not proven to be the window's (no birth time, played by the hook) ---- */

/** Set the test hook: a system with no birth time, with more hooks after it when wanted. */
static void no_birth_hook(const char *more)
{
 char hook[128];

 snprintf(hook, sizeof hook, "%s%s%s", WPE_SEEN_TEST_FAIL_NO_BIRTH, more[0] ? "," : "", more);
 chk_int("the hook is set", setenv(WPE_SEEN_TEST_FAIL_ENV, hook, 1), 0);
}

/** A window of t.c that read it, then t.c touched in place (so it is not proven the same). */
static FENSTER *window_of_a_touched_file(mode_t mode)
{
 FENSTER *f;

 put_file("t.c", "old\n", mode);
 f = window_of_file_with_mode(mode);
 window_reads(f, "t.c");
 append_in_place("t.c", "more\n");
 return(f);
}

/* @scenario SC-name-identity-12: under no birth time a touched file with a
   special bit, or another owner's, is not asked about: a plain Save refuses
   with the reason and Save As is the way out. */
static void unsure_is_refused_with_the_reason(const char *more, mode_t mode, const char *reason)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char say[1024], buf[256];

 make_dir();
 if (mode & 07000)
 {
  if (!test_premise(test_premise_special_bits_settable(dir, mode & 07000), "unsure and refused",
                    "this user cannot set the special bit in the test folder"))
   return;
 }
 no_birth_hook(more);
 f = window_of_a_touched_file(mode);
 set_rows("mine\n");
 question_answer = 'Y';
 chk_int("refused", save_window(f, "t.c", &o), -1);
 chk_int("status FAILED", o.status, WPE_WRITE_FAILED);
 chk_int("no question is asked", question_count, 0);
 chk_int("another file", o.other_file, 1);
 chk_int("unsure", o.unsure, 1);
 save_sentence(&o, say, sizeof say);
 chk_has("the reason", say, reason);
 chk_has("Save As is the way out", say, "Use Save As");
 chk_has("their text is untouched", get_file("t.c", buf, sizeof buf), "old\nmore\n");
 unsetenv(WPE_SEEN_TEST_FAIL_ENV);
}

static void test_unsure_on_a_file_with_a_special_bit_or_another_owner_is_refused(void)
{
 /* a group that cannot run the file: a write to it does not clear the bit */
 unsure_is_refused_with_the_reason("", 02644, ", and it has a setuid, setgid or sticky bit");
 unsure_is_refused_with_the_reason("write-other-owner", 0644, ", and it belongs to another user");
}

/** What happens while the box is up: the file is touched in place again. */
static void touch_during_the_question(void)
{
 append_in_place("t.c", "again\n");
}

/* @scenario SC-name-identity-13: a Yes is about the file that was asked about:
   when that file is touched again while the box is up, and no birth time can
   say it is the same, the write is refused and the sentence says the name
   changed while the user was asked. */
static void test_a_yes_does_not_reach_a_file_touched_during_the_question(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char buf[256];

 make_dir();
 no_birth_hook("");
 f = window_of_a_touched_file(0644);
 set_rows("mine\n");
 question_during = touch_during_the_question;
 question_answer = 'Y';
 chk_int("refused", save_window(f, "t.c", &o), -1);
 chk_int("asked once", question_count, 1);
 chk_int("status FAILED", o.status, WPE_WRITE_FAILED);
 chk_int("another file", o.other_file, 1);
 chk_int("changed while asked", o.changed_while_asked, 1);
 chk_int("unsure", o.unsure, 1);
 chk_has("their text is untouched", get_file("t.c", buf, sizeof buf), "old\nmore\nagain\n");
 unsetenv(WPE_SEEN_TEST_FAIL_ENV);
}

/* @scenario SC-name-identity-14: the assistant's look is held to the file it
   saw: a file touched in place since, with no birth time to settle it, is
   refused as unproven, and the sentence says it may not be the file checked. */
static void test_a_look_is_not_met_by_a_touched_file_without_a_birth_time(void)
{
 WpeWriteExpect look;
 WpeWriteOutcome o;
 char say[1024], buf[256], p[512];

 make_dir();
 no_birth_hook("");
 put_file("t.c", "old\n", 0644);
 look = look_at("t.c");
 append_in_place("t.c", "more\n");
 path_of(p, sizeof p, "t.c");
 chk_int("refused", wpe_write_save_text(p, "new\n", NULL, WPE_WRITE_NOFOLLOW, &look, &o), -1);
 chk_int("FAILED", o.status, WPE_WRITE_FAILED);
 chk_int("swapped", o.swapped, 1);
 chk_int("unsure", o.unsure, 1);
 save_sentence(&o, say, sizeof say);
 chk_has("the sentence", say, "may not be the file that was checked");
 chk_has("their text is untouched", get_file("t.c", buf, sizeof buf), "old\nmore\n");
 unsetenv(WPE_SEEN_TEST_FAIL_ENV);
}

/* @scenario SC-name-identity-15: the sentences of a caller with no text of the
   user's (the assistant's write, a revert) say "changed or replaced", and give
   the reason when the file is another user's. */
static void test_the_unsure_sentences_of_a_caller_with_no_text(void)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char say[1024], p[512];

 make_dir();
 no_birth_hook("write-other-owner");
 f = window_of_a_touched_file(0644);
 path_of(p, sizeof p, "t.c");
 chk_int("refused", wpe_write_save_text(p, "new\n", &f->file_id, 0, NULL, &o), -1);
 chk_int("unsure", o.unsure, 1);
 (void)wpe_write_sentence(say, sizeof say, &o, WPE_WRITE_THEN_NOTHING_MORE);
 chk_has("a write says changed or replaced", say, "was changed or replaced since the window showed it");
 chk_has("a write gives the reason", say, ", and it belongs to another user");
 chk_has("a write says nothing was written", say, "Nothing was written.");
 chk_int("no advice about Save As", strstr(say, "Save As") == NULL, 1);
 (void)wpe_write_sentence(say, sizeof say, &o, WPE_WRITE_THEN_PUT_BACK);
 chk_has("a revert says put back", say, "Cannot put back");
 chk_has("a revert says changed or replaced", say, "was changed or replaced since the window showed it");
 chk_has("a revert says nothing was changed", say, "Nothing was changed.");
 unsetenv(WPE_SEEN_TEST_FAIL_ENV);
}

/** What happens as the second descriptor of a Save is refused: another file takes the name. */
static void swap_the_name_at_the_dup(void)
{
 another_file_takes_the_name("t.c", "theirs\n", 0644);
}

/* @scenario SC-name-identity-16: a Save that has no second descriptor still
   records the file it wrote, from its own descriptor after the text, never from
   the name: the name swapped meanwhile is not the window's file, so the next
   Save asks and never writes a copy unasked.  Without the swap the record
   matches the file as saved (no question), with and without a birth time. */
static void save_with_no_second_descriptor(const char *hook, int swap)
{
 WpeWriteOutcome o;
 FENSTER *f;
 char buf[256];
 int took;

 make_dir();
 if (hook != NULL)
  no_birth_hook("");
 put_file("t.c", "old\n", 0644);
 f = window_of_file_with_mode(0644);
 window_reads(f, "t.c");
 set_rows("mine\n");
 dup_fail_action = swap ? swap_the_name_at_the_dup : NULL;
 dup_fails_at = 2;    /* the first duplication is the backup's, the second is the save's */
 chk_int("write", save_window(f, "t.c", &o), 0);
 took = (dup_fails_at == 0);
 dup_fails_at = 0;
 dup_fail_action = NULL;
 if (test_observation(took, "a save with no second descriptor",
                      "fcntl is not interposable for F_DUPFD_CLOEXEC in this C library"))
 {
  chk_int("the window still knows its file", f->file_id.known, 1);
  set_rows("again\n");
  question_count = 0;
  question_answer = 'N';
  if (swap)
  {
   chk_int("the next Save is declined", save_window(f, "t.c", &o), -1);
   chk_int("it asked", question_count, 1);
   chk_has("their file is untouched", get_file("t.c", buf, sizeof buf), "theirs\n");
  }
  else
  {
   chk_int("the next Save writes", save_window(f, "t.c", &o), 0);
   chk_int("it asked nothing", question_count, 0);
  }
 }
 if (hook != NULL)
  unsetenv(WPE_SEEN_TEST_FAIL_ENV);
}

static void test_a_save_with_no_second_descriptor_keeps_the_window_its_file(void)
{
 save_with_no_second_descriptor(NULL, 1);
 save_with_no_second_descriptor(NULL, 0);
 save_with_no_second_descriptor("no birth", 0);
}

/* @scenario SC-chosen-names-29: a stream opened with NOFOLLOW is never led through a link. */
static void test_a_stream_with_nofollow_is_not_led_through_a_link(void)
{
 WpeWriteOutcome o;
 char victim[512], link[512], buf[64];
 FILE *fp;

 make_dir();
 put_file("victim", "KEEP\n", 0644);
 path_of(victim, sizeof victim, "victim");
 path_of(link, sizeof link, "xwperc");
 if (symlink(victim, link) != 0)
  perror("symlink");
 fp = wpe_write_stream_open_flags(link, WPE_WRITE_NOFOLLOW, &o);
 chk_int("the open is refused", fp == NULL, 1);
 chk_int("it says a link is in the way", o.replaced_by_link, 1);
 chk_has("the victim keeps its text", read_at(victim, buf, sizeof buf), "KEEP");
 chk_int("the link is still a link", name_is_link(link), 1);
 fp = wpe_write_stream_open_flags(link, 0, &o);
 chk_int("without the flag the link is followed, as a Save follows it", fp != NULL, 1);
 if (fp != NULL)
 {
  fputs("DONE\n", fp);
  chk_int("the stream closes", wpe_write_stream_close(fp, &o), 0);
 }
 chk_has("the followed link wrote its target", read_at(victim, buf, sizeof buf), "DONE");
}

/* @scenario SC-chosen-names-30: a dangling link with NOFOLLOW creates nothing. */
static void test_a_nofollow_stream_at_a_dangling_link_creates_nothing(void)
{
 WpeWriteOutcome o;
 char target[512], link[512];
 FILE *fp;

 make_dir();
 path_of(target, sizeof target, "made");
 path_of(link, sizeof link, "project.prj");
 if (symlink(target, link) != 0)
  perror("symlink");
 fp = wpe_write_stream_open_flags(link, WPE_WRITE_NOFOLLOW, &o);
 chk_int("the open is refused", fp == NULL, 1);
 chk_int("the file the link names was not made", mode_at(target), -1);
}

/* @scenario SC-chosen-names-31: the note of the migration is written in the old folder, new and private. */
static void test_the_legacy_folder_takes_a_new_private_note_and_no_link(void)
{
 WpeWriteOutcome o;
 char home[512], file[512], victim[512], buf[64], leg[512];

 env_keep_all();
 make_dir();
 path_of(home, sizeof home, "home");
 snprintf(leg, sizeof leg, "%s/.xwpe", home);
 if (mkdir(home, 0700) != 0 || mkdir(leg, 0755) != 0)
  perror("mkdir");
 setenv("HOME", home, 1);
 chk_int("the legacy folder is under the home", wpe_legacy_dir(buf, sizeof buf), 0);
 chk_has("its name", buf, "/home/.xwpe");
 chk_int("a note is written", wpe_write_own_bytes(WPE_OWN_LEGACY, NULL, "MOVED.txt", "moved\n", 6, &o), 0);
 snprintf(file, sizeof file, "%s/MOVED.txt", leg);
 chk_has("its text", read_at(file, buf, sizeof buf), "moved");
 chk_int("it is private", mode_at(file), 0600);
 put_file("victim", "KEEP\n", 0644);
 path_of(victim, sizeof victim, "victim");
 snprintf(file, sizeof file, "%s/LINKED.txt", leg);
 if (symlink(victim, file) != 0)
  perror("symlink");
 chk_int("a link at the note's name is refused",
         wpe_write_own_bytes(WPE_OWN_LEGACY, NULL, "LINKED.txt", "x", 1, &o), -1);
 chk_has("the victim keeps its text", read_at(victim, buf, sizeof buf), "KEEP");
 unsetenv("HOME");
 setenv("XWPE_TEST_FAIL", "no-passwd-home", 1);
 chk_int("with no home there is no legacy folder", wpe_legacy_dir(buf, sizeof buf), -1);
 chk_int("nor a database home", wpe_user_database_home(buf, sizeof buf), -1);
 env_restore_all();
}

int main(void)
{
 WpeMouseChangeShape = stub_mouse_shape;
 test_a_stream_with_nofollow_is_not_led_through_a_link();
 test_a_nofollow_stream_at_a_dangling_link_creates_nothing();
 test_the_legacy_folder_takes_a_new_private_note_and_no_link();
 test_an_own_file_is_never_written_through_a_link();
 test_an_own_file_at_a_dangling_link_creates_nothing();
 test_an_old_own_file_is_replaced_by_a_new_private_file();
 test_the_state_folder_is_made_the_users_alone();
 test_another_users_folder_is_refused();
 test_a_link_below_the_state_folder_is_refused();
 test_no_per_user_place_is_ever_relative();
 test_a_look_makes_nothing_and_a_bad_name_is_refused();
 test_with_no_home_a_write_says_there_is_no_place();
 test_remove_takes_only_a_regular_file_of_ours();
 WpeMouseRestoreShape = stub_mouse_restore;
 test_a_swapped_backup_is_not_written_back();
 test_a_swapped_backup_is_not_renamed_into_place();
 test_a_name_that_changed_hands_refuses_the_save();
 test_a_name_that_was_removed_refuses_the_save();
 test_a_new_name_leaves_no_backup();
 test_a_dangling_link_is_saved_into_the_file_it_names();
 test_a_file_that_appears_as_the_name_is_made_is_backed_up();
 test_a_file_that_cannot_be_cut_keeps_its_text_and_its_backup();
 test_an_undo_with_no_descriptor_names_the_backup();
 test_a_file_that_refuses_the_write_leaves_no_new_backup();
 test_every_plain_outcome_closes_what_it_opened();
 test_every_special_bit_outcome_closes_what_it_opened();
 test_a_backup_with_no_descriptor_to_keep_stops_the_save();
 test_a_block_write_to_a_new_name_gets_no_bit();
 test_a_copy_over_an_existing_setgid_file_gets_no_bit();
 test_a_chosen_name_that_is_a_link_is_left_alone_and_said();
 test_a_dangling_chosen_name_creates_nothing_elsewhere();
 test_an_old_copy_of_ours_is_replaced_by_a_new_inode();
 test_another_users_copy_is_left_alone();
 test_a_copy_of_a_private_file_is_private_from_its_first_byte();
 test_copy_remove_leaves_a_link_and_another_users_file_alone();
 test_a_name_planted_between_the_clear_and_the_create_is_refused();
 test_a_bit_removed_on_disk_is_not_restored();
 test_a_bit_added_on_disk_since_loading_is_kept();
 test_a_longer_file_saved_short_leaves_no_tail();
 ftruncate_spares_empty_time = 1;
 test_saving_an_empty_file_over_an_empty_file_marks_it_changed();
 ftruncate_spares_empty_time = 0;
 test_save_as_over_an_existing_setgid_file_gets_no_bit();
 test_save_as_onto_the_windows_own_file_keeps_its_bit();
 test_save_as_to_the_same_name_in_another_folder_gets_no_bit();
 test_save_as_through_a_hard_link_to_its_own_file_keeps_the_bit();
 test_a_save_into_the_inode_the_window_read_keeps_the_bits();
 test_a_hard_link_to_the_windows_file_is_its_own_file();
 test_a_replaced_plain_file_is_asked_about_and_yes_writes_a_copy();
 test_no_and_escape_on_the_question_write_nothing();
 test_a_replaced_file_with_a_special_bit_is_refused();
 test_a_replaced_file_of_another_owner_is_refused();
 test_a_window_that_read_no_file_saves_as_a_copy();
 test_a_removed_file_is_made_again_and_the_window_takes_it();
 test_save_as_onto_a_replaced_name_is_a_copy();
 test_ctrl_f9_on_a_replaced_script_is_refused();
 test_a_device_under_the_name_is_refused();
 test_the_question_is_asked_outside_the_write();
 test_yes_does_not_reach_a_file_that_took_the_name_during_the_question();
 test_a_user_owned_non_regular_file_under_the_name_is_refused();
 test_a_failed_block_write_over_a_setgid_file_restores_its_bit();
 test_the_sentence_of_a_save_that_lost_a_bit();
 test_the_bit_loss_and_the_backup_note_share_one_sentence();
 test_a_restore_that_cannot_put_the_bits_back_names_them();
 test_the_bits_go_back_only_after_the_bytes_are_on_disk();
 test_the_sync_owner_syncs_once_and_only_when_owed();
 test_a_saved_setgid_file_keeps_its_bit();
 test_a_save_names_the_bit_the_system_refuses();
 test_a_save_whose_modes_are_all_refused_is_still_written();
 test_a_save_on_a_file_that_keeps_its_bit_says_nothing_false();
 test_a_save_on_a_file_whose_bit_the_kernel_cleared_names_it();
 test_a_block_write_onto_a_sticky_file_that_cannot_be_chmoded_says_it_stays();
 test_a_copy_over_02644_and_over_01644_gets_no_bit();
 test_the_question_says_a_bit_stays_on_a_file_we_do_not_own();
 test_the_question_for_a_file_we_do_not_own_says_only_sticky_stays();
 test_a_copy_over_a_setgid_file_the_kernel_cleared_says_nothing();
 test_a_save_that_lost_one_bit_and_kept_another_names_only_the_lost();
 test_a_save_with_no_descriptor_does_not_call_a_standing_bit_lost();
 test_a_restore_does_not_call_a_standing_bit_lost();
 test_a_stream_does_not_name_a_bit_the_file_lost_before_the_close();
 test_a_save_whose_file_cannot_be_read_back_says_it_may_not_have_kept_the_bit();
 test_a_save_with_no_descriptor_and_no_read_says_it_may_not_have_kept_the_bit();
 test_the_backup_never_has_the_special_bits();
 test_the_nine_bits_of_a_save_are_unchanged();
 test_a_restore_keeps_the_special_bits();
 test_sentence();
 test_text();
 test_a_text_written_over_a_setgid_file_of_ours_drops_the_bit();
 test_a_text_over_a_file_whose_bit_cannot_be_dropped_says_it_stays();
 test_a_text_written_to_a_link_goes_to_its_target();
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
 test_set_perm_changes_the_mode_by_descriptor();
 test_set_perm_on_a_write_only_file_uses_the_write_descriptor();
 test_set_perm_on_a_file_that_cannot_be_opened_goes_by_name();
 test_set_perm_does_not_follow_a_link_put_in_place();
 test_set_perm_does_not_change_a_file_that_took_the_name();
 test_a_link_swapped_in_before_a_change_by_name_is_reported();
 test_a_link_swapped_in_after_the_dialog_is_not_followed();
 test_a_named_link_shown_by_the_dialog_is_changed();
 test_a_folder_swapped_after_the_dialog_is_refused();
 test_a_link_the_dialog_named_is_not_a_link_that_took_the_name();
 test_a_socket_swapped_for_a_hard_link_changes_nothing();
 test_a_socket_swapped_in_before_the_open_is_replaced();
 test_a_link_put_in_place_is_replaced_whatever_the_system_says();
 test_a_socket_put_in_place_is_replaced_whatever_the_system_says();
 test_an_open_that_fails_with_nothing_at_the_name_keeps_its_errno();
 test_a_fifo_is_changed_by_name();
 test_a_writer_waiting_on_a_fifo_is_not_released_by_a_change_of_mode();
 test_add_perm_adds_x_and_keeps_the_special_bits();
 test_add_perm_on_a_link_the_user_named_follows_it_once();
 test_a_name_that_leads_nowhere_is_not_reached();
 test_the_assistants_write_is_a_save();
 test_the_assistants_write_refuses_a_link_at_the_end();
 test_the_assistants_write_says_a_link_took_the_name_whatever_the_system_says();
 test_the_assistants_write_with_no_window_keeps_the_files_own_bits();
 test_the_assistants_write_over_a_different_file_than_the_windows_is_refused();
 test_the_assistants_write_makes_a_new_file();
 test_a_revert_puts_back_the_recorded_bits();
 test_a_revert_gives_the_recorded_nine_bits_and_refuses_a_link();
 test_a_failed_save_with_no_backup_names_the_lost_bit();
 test_a_failed_save_that_left_the_bit_on_does_not_say_it_is_off();
 test_a_save_refused_before_the_bits_are_dropped_names_none();
 test_a_text_with_no_window_is_written_only_after_the_bit_is_off();
 test_the_assistants_write_refuses_a_file_that_is_not_regular();
 test_the_assistants_write_refuses_a_pipe_and_does_not_wait();
 test_the_assistants_write_refuses_a_file_swapped_in_after_the_look();
 test_the_assistants_write_refuses_a_name_gained_after_the_look();
 test_the_assistants_write_asks_about_a_replaced_file();
 test_the_assistants_write_refuses_a_replaced_program();
 test_a_revert_that_finds_another_file_says_nothing_was_changed();
 test_the_assistants_write_of_a_free_name_must_make_the_file();
 test_the_assistants_write_with_the_look_keeps_the_bit_and_makes_the_backup();
 test_a_file_removed_after_the_look_is_not_made_again();
 test_a_link_at_the_name_is_refused_and_named();
 test_a_revert_puts_back_a_before_image_that_holds_a_nul_byte();
 test_a_revert_with_no_recorded_mode_keeps_the_files_own_bits();
 test_the_assistants_sentences_say_a_lost_bit_and_keep_no_window_text();
 test_a_file_that_has_the_windows_inode_number_is_asked_about();
 test_a_write_with_no_one_to_ask_refuses_a_file_with_the_windows_number();
 test_a_touched_file_is_still_the_windows_file();
 test_a_touched_file_without_a_birth_time_is_asked_about();
 test_a_second_save_is_the_windows_own();
 test_a_look_is_not_met_by_a_file_with_its_number();
 test_another_file_with_the_copys_number_is_not_removed();
 test_a_link_that_took_the_dialogs_number_is_another_link();
 test_unsure_on_a_file_with_a_special_bit_or_another_owner_is_refused();
 test_a_yes_does_not_reach_a_file_touched_during_the_question();
 test_a_look_is_not_met_by_a_touched_file_without_a_birth_time();
 test_the_unsure_sentences_of_a_caller_with_no_text();
 test_a_save_with_no_second_descriptor_keeps_the_window_its_file();
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
