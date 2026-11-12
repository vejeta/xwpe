/* test_startcmd.c -- unit test for the answers about a folder's commands
 * (we_startcmd.c).  Links we_startcmd.c alone; no editor -- always runs.
 *
 * A fresh temporary directory holds the state folder ($XDG_STATE_HOME) and
 * the project folders.  The cases: a command the user answered for runs and
 * a changed one asks again; "Use mine" is remembered as such; escapes in a
 * command survive the store; one record per folder, key and text (store
 * version 2: two texts answered for in one folder both stay answered); the
 * cap, in all and per folder, drops the oldest; a version-1 store reads as
 * empty and is rewritten as version 2; a store that is missing,
 * of an unknown version, holds a bad line
 * or is a link asks and is never written over; a folder reached through a
 * link is the same folder; the user's own texts run; the printable form of
 * a command writes every control byte out.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "../we_startcmd.h"

#define PATH_ROOM         4096
#define TEST_DIR_PARENT   "/tmp"                 /* when $TMPDIR is not set */
#define TEST_DIR_NAME     "xwpe-test-startcmd-XXXXXX"
#define CAP_EXTRA         8      /* folders answered for past the cap */
#define TEXT_ROOM         256
#define FIFO_ALARM_S      5      /* a lookup that blocks on a FIFO dies by then */

static int failures;
static char base[PATH_ROOM];     /* the temporary directory, resolved */
static char state[PATH_ROOM];    /* base/state: $XDG_STATE_HOME */
static char store[PATH_ROOM];    /* the store's path */
static char proj[PATH_ROOM];     /* base/proj: a project folder */
static char alias[PATH_ROOM];    /* base/alias: a link to base/proj */

static void fail(const char *name, const char *why)
{
 printf("FAIL %s: %s\n", name, why);
 failures++;
}

static void expect_int(const char *name, int got, int want)
{
 char why[TEXT_ROOM];
 if (got == want) return;
 snprintf(why, sizeof why, "got %d, want %d", got, want);
 fail(name, why);
}

static void expect_str(const char *name, const char *got, const char *want)
{
 char why[PATH_ROOM];
 if (strcmp(got, want) == 0) return;
 snprintf(why, sizeof why, "got \"%s\", want \"%s\"", got, want);
 fail(name, why);
}

static void under_base(char *dst, size_t sz, const char *rel)
{
 snprintf(dst, sz, "%s/%s", base, rel);
}

static void make_dir(const char *path)
{
 if (mkdir(path, 0700) != 0) { perror(path); exit(2); }
}

static void write_file(const char *path, const char *text)
{
 FILE *fp = fopen(path, "w");
 if (!fp) { perror(path); exit(2); }
 fputs(text, fp);
 fclose(fp);
}

/* How many lines the store holds, its first one included. */
static int store_lines(void)
{
 FILE *fp = fopen(store, "r");
 int n = 0, c;
 if (!fp) return -1;
 while ((c = fgetc(fp)) != EOF)
  if (c == '\n') n++;
 fclose(fp);
 return n;
}

static void drop_store(void)
{
 unlink(store);
}

/* A lookup that blocked on a FIFO: say so and end, rather than hang. */
static void on_blocked(int sig)
{
 static const char line[] = "FAIL a FIFO at the store: the lookup blocked\n";
 (void)sig;
 if (write(STDOUT_FILENO, line, sizeof line - 1) < 0) _exit(1);
 _exit(1);
}

static void setup(void)
{
 char tmpl[PATH_ROOM];
 char real[PATH_ROOM];
 const char *tmp = getenv("TMPDIR");
 if (!tmp || !*tmp) tmp = TEST_DIR_PARENT;
 snprintf(tmpl, sizeof tmpl, "%s/%s", tmp, TEST_DIR_NAME);
 setvbuf(stdout, NULL, _IONBF, 0);
 signal(SIGALRM, on_blocked);
 if (!mkdtemp(tmpl)) { perror("mkdtemp"); exit(2); }
 if (!realpath(tmpl, real)) { perror("realpath"); exit(2); }
 snprintf(base, sizeof base, "%s", real);
 under_base(state, sizeof state, "state");
 under_base(proj, sizeof proj, "proj");
 under_base(alias, sizeof alias, "alias");
 make_dir(proj);
 if (symlink(proj, alias) != 0) { perror("symlink"); exit(2); }
 setenv("XDG_STATE_HOME", state, 1);
 if (wpe_startcmd_store_path(store, sizeof store) != 0) { fail("store path", "none"); exit(1); }
}

static void case_printable(void)
{
 char out[TEXT_ROOM];
 size_t need;
 wpe_startcmd_printable("lpr\x1b[2J -P\tq\n\\x", out, sizeof out);
 expect_str("printable controls", out, "lpr\\x1b[2J -P\\tq\\n\\\\x");
 wpe_startcmd_printable("caf\xc3\xa9", out, sizeof out);
 expect_str("printable keeps UTF-8", out, "caf\xc3\xa9");
 wpe_startcmd_printable("a\xc2\x9b" "b\x9b" "c\x7f", out, sizeof out);
 expect_str("printable C1 and stray bytes", out, "a\\xc2\\x9bb\\x9bc\\x7f");
 wpe_startcmd_printable("rm \xe2\x80\xae" "fdp.exe", out, sizeof out);
 expect_str("printable writes a bidi override out", out, "rm \\u202efdp.exe");
 wpe_startcmd_printable("a\xe2\x80\x8b" "b\xef\xbb\xbf" "c\xe2\x81\xa6" "d", out, sizeof out);
 expect_str("printable writes zero-width, BOM and isolates out", out, "a\\u200bb\\ufeffc\\u2066d");
 need = wpe_startcmd_printable("ab\x1b", out, 4);
 expect_int("printable needs", (int)need, 6);
 expect_str("printable cut on a whole escape", out, "ab");
}

static void case_round_trip(void)
{
 drop_store();
 expect_int("missing store asks", wpe_startcmd_check(proj, "PrintCmd", "./fake-lpr"), WPE_STARTCMD_ASK);
 expect_int("missing store is missing", wpe_startcmd_last_store_state(), WPE_STARTCMD_STORE_MISSING);
 expect_int("remember yes", wpe_startcmd_remember(proj, "PrintCmd", "./fake-lpr", WPE_STARTCMD_YES), 0);
 expect_int("remembered yes runs", wpe_startcmd_check(proj, "PrintCmd", "./fake-lpr"), WPE_STARTCMD_RUN);
 expect_int("a changed command asks", wpe_startcmd_check(proj, "PrintCmd", "./fake-lpr -x"), WPE_STARTCMD_ASK);
 expect_int("another key asks", wpe_startcmd_check(proj, "Compiler/C", "./fake-lpr"), WPE_STARTCMD_ASK);
 expect_int("remember no", wpe_startcmd_remember(proj, "PrintCmd", "./fake-lpr -x", WPE_STARTCMD_NO), 0);
 expect_int("remembered no uses own", wpe_startcmd_check(proj, "PrintCmd", "./fake-lpr -x"), WPE_STARTCMD_USE_OWN);
 expect_int("another text's answer is kept", wpe_startcmd_check(proj, "PrintCmd", "./fake-lpr"), WPE_STARTCMD_RUN);
 expect_int("one record per folder, key and text", store_lines(), 3);
 expect_int("a new answer for the same text", wpe_startcmd_remember(proj, "PrintCmd", "./fake-lpr", WPE_STARTCMD_NO), 0);
 expect_int("replaces the old one", wpe_startcmd_check(proj, "PrintCmd", "./fake-lpr"), WPE_STARTCMD_USE_OWN);
 expect_int("and adds no line", store_lines(), 3);
}

static void case_modes(void)
{
 struct stat st;
 if (stat(store, &st) != 0) { fail("store mode", "no store"); return; }
 expect_int("store is the user's alone", (int)(st.st_mode & 0777), 0600);
 if (stat(state, &st) != 0) { fail("state mode", "no folder"); return; }
 expect_int("state folder below is made", S_ISDIR(st.st_mode), 1);
}

static void case_escapes(void)
{
 const char *odd = "cc\t-DX=\"a\\b\"\n-o\rout";
 expect_int("remember escapes", wpe_startcmd_remember(proj, "Compiler/C", odd, WPE_STARTCMD_YES), 0);
 expect_int("escapes come back whole", wpe_startcmd_check(proj, "Compiler/C", odd), WPE_STARTCMD_RUN);
 expect_int("escapes stay one line", store_lines(), 4);
}

static void case_alias(void)
{
 expect_int("a link to the folder is the folder",
            wpe_startcmd_check(alias, "Compiler/C", "cc\t-DX=\"a\\b\"\n-o\rout"), WPE_STARTCMD_RUN);
 expect_int("a folder that is not there asks",
            wpe_startcmd_check("/nonexistent/xwpe/folder", "PrintCmd", "lpr"), WPE_STARTCMD_ASK);
}

static void case_own(void)
{
 drop_store();
 wpe_startcmd_note_own("Compiler/C", "gcc -g");
 wpe_startcmd_note_own("Compiler/C", "clang -O0");
 expect_int("the user's own runs", wpe_startcmd_check(proj, "Compiler/C", "clang -O0"), WPE_STARTCMD_RUN);
 expect_int("the default runs", wpe_startcmd_check(proj, "Compiler/C", "gcc -g"), WPE_STARTCMD_RUN);
 expect_int("own is per key", wpe_startcmd_check(proj, "PrintCmd", "gcc -g"), WPE_STARTCMD_ASK);
 wpe_startcmd_reset();
 expect_int("reset forgets own", wpe_startcmd_check(proj, "Compiler/C", "gcc -g"), WPE_STARTCMD_ASK);
}

/* A store in place, then a check and a remember that must both fail closed. */
static void expect_fails_closed(const char *name)
{
 char before[PATH_ROOM], after[PATH_ROOM];
 FILE *fp;
 size_t n;
 expect_int(name, wpe_startcmd_check(proj, "PrintCmd", "lpr"), WPE_STARTCMD_ASK);
 expect_int(name, wpe_startcmd_last_store_state(), WPE_STARTCMD_STORE_BAD);
 fp = fopen(store, "r");
 n = fp ? fread(before, 1, sizeof before - 1, fp) : 0;
 before[n] = '\0';
 if (fp) fclose(fp);
 expect_int(name, wpe_startcmd_remember(proj, "PrintCmd", "lpr", WPE_STARTCMD_YES), -1);
 fp = fopen(store, "r");
 n = fp ? fread(after, 1, sizeof after - 1, fp) : 0;
 after[n] = '\0';
 if (fp) fclose(fp);
 expect_str(name, after, before);
}

static void case_fails_closed(void)
{
 char text[PATH_ROOM], victim[PATH_ROOM];
 snprintf(text, sizeof text, "xwpe-start-commands 3\nyes\tPrintCmd\t%s\tlpr\n", proj);
 write_file(store, text);
 expect_fails_closed("another version asks and is kept");
 /* A version-1 store's answers are about single commands, which version 2
    no longer asks about: it reads as empty (its yes does not count), and
    the next answer writes the store anew as version 2. */
 snprintf(text, sizeof text, "xwpe-start-commands 1\nyes\tPrintCmd\t%s\tlpr\n", proj);
 write_file(store, text);
 expect_int("a version-1 store's yes does not count", wpe_startcmd_check(proj, "PrintCmd", "lpr"), WPE_STARTCMD_ASK);
 expect_int("a version-1 store reads as empty", wpe_startcmd_last_store_state(), WPE_STARTCMD_STORE_OK);
 expect_int("the next answer is kept", wpe_startcmd_remember(proj, "PrintCmd", "lpr", WPE_STARTCMD_YES), 0);
 expect_int("in a version-2 store", wpe_startcmd_check(proj, "PrintCmd", "lpr"), WPE_STARTCMD_RUN);
 expect_int("which holds that one answer", store_lines(), 2);
 drop_store();
 snprintf(text, sizeof text, "%s\nyes\tPrintCmd\t%s\tlpr\nmaybe\tPrintCmd\t%s\tlpr\n",
          WPE_STARTCMD_STORE_MAGIC, proj, proj);
 write_file(store, text);
 expect_fails_closed("a bad line asks and is kept");
 snprintf(text, sizeof text, "%s\nyes\tPrintCmd\t%s\n", WPE_STARTCMD_STORE_MAGIC, proj);
 write_file(store, text);
 expect_fails_closed("a short line asks and is kept");
 snprintf(text, sizeof text, "%s\nyes\tPrintCmd\t%s\tlp\\q\n", WPE_STARTCMD_STORE_MAGIC, proj);
 write_file(store, text);
 expect_fails_closed("an unknown escape asks and is kept");
 under_base(victim, sizeof victim, "victim");
 snprintf(text, sizeof text, "%s\nyes\tPrintCmd\t%s\tlpr\n", WPE_STARTCMD_STORE_MAGIC, proj);
 write_file(victim, text);
 drop_store();
 if (symlink(victim, store) != 0) { perror("symlink"); exit(2); }
 expect_int("a linked store asks", wpe_startcmd_check(proj, "PrintCmd", "lpr"), WPE_STARTCMD_ASK);
 expect_int("a linked store is not followed", wpe_startcmd_remember(proj, "PrintCmd", "x", WPE_STARTCMD_YES), -1);
 drop_store();
 unlink(victim);
}

/* A FIFO planted at the store's name: the lookup must not wait for a writer. */
static void case_fifo(void)
{
 drop_store();
 if (mkfifo(store, 0600) != 0) { perror("mkfifo"); exit(2); }
 alarm(FIFO_ALARM_S);               /* a read that blocks on the FIFO fails the test */
 expect_int("a FIFO at the store asks", wpe_startcmd_check(proj, "PrintCmd", "lpr"), WPE_STARTCMD_ASK);
 expect_int("a FIFO at the store is not a store", wpe_startcmd_last_store_state(), WPE_STARTCMD_STORE_BAD);
 alarm(0);
 drop_store();
}

/* An existing state folder others may read is made the user's alone. */
static void case_dir_mode(void)
{
 char dir[PATH_ROOM];
 struct stat st;
 snprintf(dir, sizeof dir, "%s/%s", state, WPE_STARTCMD_STATE_SUBDIR);
 drop_store();
 chmod(dir, 0755);
 expect_int("remember into an open folder", wpe_startcmd_remember(proj, "PrintCmd", "lpr", WPE_STARTCMD_YES), 0);
 if (stat(dir, &st) != 0) { fail("dir mode", "no folder"); return; }
 expect_int("the state folder is tightened", (int)(st.st_mode & 0777), 0700);
}

static void case_no_home(void)
{
 char *home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
 unsetenv("XDG_STATE_HOME");
 unsetenv("HOME");
 expect_int("nowhere to keep answers asks", wpe_startcmd_check(proj, "PrintCmd", "lpr"), WPE_STARTCMD_ASK);
 expect_int("nowhere to keep answers keeps none", wpe_startcmd_remember(proj, "PrintCmd", "lpr", WPE_STARTCMD_YES), -1);
 if (home) { setenv("HOME", home, 1); free(home); }
 setenv("XDG_STATE_HOME", state, 1);
}

static void case_cap(void)
{
 char dir[PATH_ROOM], first[PATH_ROOM], last[PATH_ROOM];
 int i, total = WPE_STARTCMD_MAX_RECORDS + CAP_EXTRA;
 drop_store();
 for (i = 0; i < total; i++) {
  snprintf(dir, sizeof dir, "%s/cap%d", base, i);
  make_dir(dir);
  if (wpe_startcmd_remember(dir, "PrintCmd", "lpr", WPE_STARTCMD_YES) != 0) {
   fail("cap", "a remember failed");
   break;
  }
 }
 expect_int("the store is capped", store_lines(), WPE_STARTCMD_MAX_RECORDS + 1);
 snprintf(first, sizeof first, "%s/cap0", base);
 snprintf(last, sizeof last, "%s/cap%d", base, total - 1);
 expect_int("the oldest answer went first", wpe_startcmd_check(first, "PrintCmd", "lpr"), WPE_STARTCMD_ASK);
 expect_int("the newest answer stays", wpe_startcmd_check(last, "PrintCmd", "lpr"), WPE_STARTCMD_RUN);
 for (i = 0; i < total; i++) {
  snprintf(dir, sizeof dir, "%s/cap%d", base, i);
  rmdir(dir);
 }
}

/* One folder answers for more texts than it may keep: its oldest go first,
   and another folder's answer is never pushed out by them. */
static void case_folder_cap(void)
{
 char text[TEXT_ROOM], other[PATH_ROOM];
 int i, total = WPE_STARTCMD_MAX_PER_FOLDER + CAP_EXTRA;
 drop_store();
 under_base(other, sizeof other, "other");
 make_dir(other);
 expect_int("another folder's answer", wpe_startcmd_remember(other, "build/C", "x", WPE_STARTCMD_YES), 0);
 for (i = 0; i < total; i++) {
  snprintf(text, sizeof text, "text %d", i);
  if (wpe_startcmd_remember(proj, "build/C", text, WPE_STARTCMD_YES) != 0) {
   fail("folder cap", "a remember failed");
   break;
  }
 }
 expect_int("a folder keeps its cap", store_lines(), WPE_STARTCMD_MAX_PER_FOLDER + 2);
 expect_int("its oldest text went first", wpe_startcmd_check(proj, "build/C", "text 0"), WPE_STARTCMD_ASK);
 snprintf(text, sizeof text, "text %d", total - 1);
 expect_int("its newest text stays", wpe_startcmd_check(proj, "build/C", text), WPE_STARTCMD_RUN);
 expect_int("the other folder's stays", wpe_startcmd_check(other, "build/C", "x"), WPE_STARTCMD_RUN);
 drop_store();
 rmdir(other);
}

/* Remove exactly what setup and the cases made. */
static void teardown(void)
{
 char dir[PATH_ROOM];
 drop_store();
 snprintf(dir, sizeof dir, "%s/%s", state, WPE_STARTCMD_STATE_SUBDIR);
 rmdir(dir);
 rmdir(state);
 unlink(alias);
 rmdir(proj);
 rmdir(base);
}

int main(void)
{
 setup();
 case_printable();
 case_round_trip();
 case_modes();
 case_escapes();
 case_alias();
 case_own();
 case_fails_closed();
 case_no_home();
 case_dir_mode();
 case_cap();
 case_folder_cap();
 case_fifo();
 teardown();
 if (failures) { printf("%d failure(s)\n", failures); return 1; }
 printf("test_startcmd: all cases passed\n");
 return 0;
}
