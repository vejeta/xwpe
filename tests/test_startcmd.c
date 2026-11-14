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
#include <dirent.h>
#include <fcntl.h>
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
 chmod(path, 0600);      /* a store is the user's alone: a umask of 002 must not change that */
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

/* The answers survive a state folder an editor made with umask 002; a store
   the group could write is never believed, and is left for the user. */
static void case_umask_folder(void)
{
 char dir[PATH_ROOM];
 struct stat st;
 snprintf(dir, sizeof dir, "%s/%s", state, WPE_STARTCMD_STATE_SUBDIR);
 expect_int("remember before", wpe_startcmd_remember(proj, "PrintCmd", "./umask-lpr", WPE_STARTCMD_YES), 0);
 chmod(dir, 0775);
 expect_int("a 0775 folder of the user's keeps the answer",
            wpe_startcmd_check(proj, "PrintCmd", "./umask-lpr"), WPE_STARTCMD_RUN);
 expect_int("the folder is the user's alone again", stat(dir, &st) == 0 ? (int)(st.st_mode & 0777) : -1,
            0700);
 chmod(store, 0664);
 expect_int("a store the group may write asks", wpe_startcmd_check(proj, "PrintCmd", "./umask-lpr"),
            WPE_STARTCMD_ASK);
 expect_int("and is a refused store", wpe_startcmd_last_store_state(), WPE_STARTCMD_STORE_BAD);
 expect_int("which is not written over", wpe_startcmd_remember(proj, "PrintCmd", "./umask-lpr", WPE_STARTCMD_YES), -1);
 expect_int("and keeps its mode", stat(store, &st) == 0 ? (int)(st.st_mode & 0777) : -1, 0664);
 chmod(store, 0600);
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

/* ---- What this editor built (built-outputs) ---------------------------- */

#define BUILT_SETTLE_US   30000   /* past the kernel's time tick, so a change shows */
#define BUILT_MANY        2000    /* records of one folder that must survive a write */
#define BUILT_DEAD_COUNT  3

static char built_dir[PATH_ROOM];       /* base/built: a build folder */
static char built_store[PATH_ROOM];     /* the built-outputs store */

static void built_paths(void)
{
 under_base(built_dir, sizeof built_dir, "built");
 snprintf(built_store, sizeof built_store, "%s/%s/%s", state, WPE_STARTCMD_STATE_SUBDIR,
          WPE_BUILT_STORE_NAME);
}

/* Make a file with a text in a folder; its path in @out. */
static void built_file(const char *folder, const char *name, const char *text, char *out, size_t sz)
{
 snprintf(out, sz, "%s/%s", folder, name);
 write_file(out, text);
}

static int built_stat(const char *path, struct stat *st)
{
 if (stat(path, st) != 0) { perror(path); exit(2); }
 return 0;
}

/* Note @name of @folder as built, and save; the set stays loaded in @set. */
static int built_note(WpeBuiltSet *set, const char *folder, const char *name)
{
 char path[PATH_ROOM];
 struct stat st;
 snprintf(path, sizeof path, "%s/%s", folder, name);
 built_stat(path, &st);
 wpe_startcmd_built_load(folder, set);
 if (wpe_startcmd_note_built(set, name, &st) != 0) return -1;
 return wpe_startcmd_built_save(set);
}

/* Is @name of @folder, as it is now, this editor's? A fresh load each time. */
static int built_asks(const char *folder, const char *name)
{
 char path[PATH_ROOM];
 WpeBuiltSet set;
 struct stat st;
 int r;
 snprintf(path, sizeof path, "%s/%s", folder, name);
 built_stat(path, &st);
 wpe_startcmd_built_load(folder, &set);
 r = wpe_startcmd_built_here(&set, name, &st);
 wpe_startcmd_built_free(&set);
 return r;
}

static void built_reset_store(void)
{
 char dir[PATH_ROOM];
 snprintf(dir, sizeof dir, "%s/%s", state, WPE_STARTCMD_STATE_SUBDIR);
 chmod(dir, 0700);
 unlink(built_store);
}

static void case_built_round_trip(void)
{
 char path[PATH_ROOM];
 WpeBuiltSet set;
 struct stat st;
 built_reset_store();
 make_dir(built_dir);
 built_file(built_dir, "t.e", "one", path, sizeof path);
 expect_int("no record: not built here", built_asks(built_dir, "t.e"), 0);
 expect_int("note and save", built_note(&set, built_dir, "t.e"), 0);
 wpe_startcmd_built_free(&set);
 expect_int("noted: built here", built_asks(built_dir, "t.e"), 1);
 built_stat(path, &st);
 wpe_startcmd_built_load(built_dir, &set);
 expect_int("the caller's stat answers the same", wpe_startcmd_built_here(&set, path, &st), 1);
 expect_int("an absolute path is the same file", wpe_startcmd_built_here(&set, "./t.e", &st), 1);
 wpe_startcmd_built_free(&set);
 struct stat sb;
 expect_int("the store is the user's alone", stat(built_store, &sb) == 0 ? (int)(sb.st_mode & 0777) : -1, 0600);
}

static void case_built_changed(void)
{
 char path[PATH_ROOM];
 WpeBuiltSet set;
 FILE *fp;
 built_reset_store();
 built_file(built_dir, "u.e", "one", path, sizeof path);
 built_note(&set, built_dir, "u.e");
 wpe_startcmd_built_free(&set);
 usleep(BUILT_SETTLE_US);
 fp = fopen(path, "w");                         /* a rewrite: size, times */
 if (fp) { fputs("another text", fp); fclose(fp); }
 expect_int("a rewrite is not the built file", built_asks(built_dir, "u.e"), 0);
 built_note(&set, built_dir, "u.e");
 wpe_startcmd_built_free(&set);
 expect_int("built again: here", built_asks(built_dir, "u.e"), 1);
 usleep(BUILT_SETTLE_US);
 {
  struct timespec ts[2] = { { 1000, 0 }, { 1000, 0 } };
  utimensat(AT_FDCWD, path, ts, 0);
 }
 expect_int("a new date is not the built file", built_asks(built_dir, "u.e"), 0);
 built_note(&set, built_dir, "u.e");
 wpe_startcmd_built_free(&set);
 usleep(BUILT_SETTLE_US);
 chmod(path, 0700);
 expect_int("chmod changes the change time: not the built file", built_asks(built_dir, "u.e"), 0);
}

static void case_built_other_folder(void)
{
 char other[PATH_ROOM], path[PATH_ROOM], elsewhere[PATH_ROOM], real[PATH_ROOM];
 WpeBuiltSet set;
 built_reset_store();
 built_file(built_dir, "v.e", "one", path, sizeof path);
 built_note(&set, built_dir, "v.e");
 wpe_startcmd_built_free(&set);
 under_base(other, sizeof other, "copy");
 make_dir(other);
 built_file(other, "v.e", "one", path, sizeof path);
 expect_int("the same name in another real folder: not built here", built_asks(other, "v.e"), 0);
 /* the recorded output, replaced by a link to itself elsewhere */
 under_base(elsewhere, sizeof elsewhere, "elsewhere");
 make_dir(elsewhere);
 snprintf(real, sizeof real, "%s/v.e", built_dir);
 built_file(elsewhere, "v.e", "one", path, sizeof path);
 unlink(real);
 if (symlink(path, real) != 0) { perror("symlink"); exit(2); }
 expect_int("a link to a file elsewhere: not built here", built_asks(built_dir, "v.e"), 0);
 unlink(real);
}

static void case_built_refused_store(void)
{
 char dir[PATH_ROOM], path[PATH_ROOM], victim[PATH_ROOM];
 WpeBuiltSet set;
 struct stat st;
 built_reset_store();
 built_file(built_dir, "w.e", "one", path, sizeof path);
 built_stat(path, &st);
 snprintf(dir, sizeof dir, "%s/%s", state, WPE_STARTCMD_STATE_SUBDIR);
 /* a link at the store */
 under_base(victim, sizeof victim, "victim-built");
 write_file(victim, "xwpe-built-outputs 1\n");
 if (symlink(victim, built_store) != 0) { perror("symlink"); exit(2); }
 wpe_startcmd_built_load(built_dir, &set);
 expect_int("a link at the store is refused", set.state, WPE_STARTCMD_STORE_BAD);
 expect_int("nothing is built here", wpe_startcmd_built_here(&set, "w.e", &st), 0);
 expect_int("nothing can be noted", wpe_startcmd_note_built(&set, "w.e", &st), -1);
 wpe_startcmd_built_free(&set);
 unlink(built_store);
 unlink(victim);
 /* a store others may write */
 built_note(&set, built_dir, "w.e");
 wpe_startcmd_built_free(&set);
 chmod(built_store, 0666);
 expect_int("a store others write is refused", built_asks(built_dir, "w.e"), 0);
 chmod(built_store, 0600);
 expect_int("the same store, mode 0600, is believed", built_asks(built_dir, "w.e"), 1);
 /* a state folder of the user's that others may write, as an editor made
    it under umask 002: made the user's alone again, and its stores read */
 chmod(dir, 0777);
 expect_int("the user's own folder, others writing, is tightened and read",
            built_asks(built_dir, "w.e"), 1);
 expect_int("and it is the user's alone again", stat(dir, &st) == 0 ? (int)(st.st_mode & 0777) : -1,
            0700);
 chmod(dir, 0775);
 chmod(built_store, 0664);
 expect_int("a store the group may write is still refused", built_asks(built_dir, "w.e"), 0);
 chmod(built_store, 0600);
 built_stat(path, &st);
 /* a folder where the store should be: it cannot be opened as one */
 unlink(built_store);
 if (mkdir(built_store, 0700) != 0) { perror("mkdir"); exit(2); }
 wpe_startcmd_built_load(built_dir, &set);
 expect_int("a store that cannot be opened is bad", set.state, WPE_STARTCMD_STORE_BAD);
 expect_int("so built_here is 0", wpe_startcmd_built_here(&set, "w.e", &st), 0);
 expect_int("and note_built fails", wpe_startcmd_note_built(&set, "w.e", &st), -1);
 wpe_startcmd_built_free(&set);
 rmdir(built_store);
}

static void case_built_many(void)
{
 char path[PATH_ROOM], name[TEXT_ROOM];
 WpeBuiltSet set;
 struct stat st;
 int i, here = 0;
 built_reset_store();
 wpe_startcmd_built_load(built_dir, &set);
 for (i = 0; i < BUILT_MANY; i++) {
  snprintf(name, sizeof name, "m%d.o", i);
  built_file(built_dir, name, "x", path, sizeof path);
  built_stat(path, &st);
  if (wpe_startcmd_note_built(&set, name, &st) != 0) { fail("many", "note failed"); break; }
 }
 expect_int("2000 records of one folder are written", wpe_startcmd_built_save(&set), 0);
 wpe_startcmd_built_free(&set);
 wpe_startcmd_built_load(built_dir, &set);
 for (i = 0; i < BUILT_MANY; i++) {
  snprintf(name, sizeof name, "m%d.o", i);
  snprintf(path, sizeof path, "%s/%s", built_dir, name);
  built_stat(path, &st);
  here += wpe_startcmd_built_here(&set, name, &st);
 }
 expect_int("all of them survive: no per-folder cap", here, BUILT_MANY);
 wpe_startcmd_built_free(&set);
}

static void case_built_dead_folder(void)
{
 char dead[PATH_ROOM], path[PATH_ROOM], text[PATH_ROOM];
 WpeBuiltSet set;
 char line[PATH_ROOM * 2];
 FILE *fp;
 int i, lines = 0;
 built_reset_store();
 under_base(dead, sizeof dead, "dead");
 make_dir(dead);
 for (i = 0; i < BUILT_DEAD_COUNT; i++) {
  snprintf(text, sizeof text, "d%d.o", i);
  built_file(dead, text, "x", path, sizeof path);
  built_note(&set, dead, text);
  wpe_startcmd_built_free(&set);
 }
 for (i = 0; i < BUILT_DEAD_COUNT; i++) {
  snprintf(path, sizeof path, "%s/d%d.o", dead, i);
  unlink(path);
 }
 rmdir(dead);
 built_file(built_dir, "live.o", "x", path, sizeof path);
 built_note(&set, built_dir, "live.o");
 wpe_startcmd_built_free(&set);
 fp = fopen(built_store, "r");
 if (!fp) { fail("dead folder", "no store"); return; }
 while (fgets(line, sizeof line, fp)) {
  lines++;
  if (strstr(line, "/dead/")) fail("dead folder", "a dead folder's record was kept");
 }
 fclose(fp);
 if (lines < 2) fail("dead folder", "the live record is gone");
}

static void case_built_cap(void)
{
 char path[PATH_ROOM], name[TEXT_ROOM];
 WpeBuiltSet set;
 struct stat st;
 int i, total = WPE_BUILT_MAX_RECORDS + CAP_EXTRA, n = 0;
 char line[PATH_ROOM * 2];
 FILE *fp;
 built_reset_store();
 wpe_startcmd_built_load(built_dir, &set);
 for (i = 0; i < total; i++) {
  snprintf(name, sizeof name, "c%d.o", i);
  built_file(built_dir, name, "x", path, sizeof path);
  built_stat(path, &st);
  wpe_startcmd_note_built(&set, name, &st);
 }
 expect_int("past the cap: still written", wpe_startcmd_built_save(&set), 0);
 wpe_startcmd_built_free(&set);
 fp = fopen(built_store, "r");
 if (!fp) { fail("built cap", "no store"); return; }
 while (fgets(line, sizeof line, fp)) n++;
 fclose(fp);
 expect_int("the oldest go: the cap and the first line", n, WPE_BUILT_MAX_RECORDS + 1);
 expect_int("the oldest is gone", built_asks(built_dir, "c0.o"), 0);
 expect_int("the newest is kept", built_asks(built_dir, "c8199.o"), 1);
}

/* Remove what the built-outputs cases made, file by file, in the test's own
   folders under the temporary directory. */
static void built_cleanup(void)
{
 static const char *folders[] = { "built", "copy", "elsewhere" };
 char path[PATH_ROOM];
 struct dirent *e;
 DIR *d;
 size_t i;
 for (i = 0; i < sizeof folders / sizeof folders[0]; i++) {
  under_base(path, sizeof path, folders[i]);
  d = opendir(path);
  if (d) {
   while ((e = readdir(d)) != NULL) {
    char file[PATH_ROOM * 2];
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    snprintf(file, sizeof file, "%s/%s", path, e->d_name);
    unlink(file);
   }
   closedir(d);
  }
  rmdir(path);
 }
 under_base(path, sizeof path, "victim-built");
 unlink(path);
 unlink(built_store);
}

int main(void)
{
 setup();
 built_paths();
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
 case_umask_folder();
 case_built_round_trip();
 case_built_changed();
 case_built_other_folder();
 case_built_refused_store();
 case_built_many();
 case_built_dead_folder();
 case_built_cap();
 built_cleanup();
 teardown();
 if (failures) { printf("%d failure(s)\n", failures); return 1; }
 printf("test_startcmd: all cases passed\n");
 return 0;
}
