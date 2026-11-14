/* test_startplan.c -- unit test for the start plan (we_startplan.c).
 * Links we_startplan.c with the store (we_startcmd.c) and the children's
 * owner; no editor -- always runs.
 *
 * A fresh temporary directory holds the state folder, a home folder, a
 * project folder and a folder outside it.  The cases: the operand rule on
 * every join; a folder value made an option is refused; a path through a
 * link out of the project resolves outside it; a removal outside is
 * dropped and never done while one inside is done; a write outside asks
 * while the user's own does not; the home folder and the folders above it
 * are never "inside"; a plan equal in effect to the user's own runs; an
 * answer is kept for that very plan, and a changed plan asks again; a step
 * whose command line holds a word the plan did not show is refused; only
 * where the user is expands from the environment.
 */
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pwd.h>

#include "../we_startplan.h"
#include "../we_startcmd.h"

#define PATH_ROOM         4096
#define TEST_DIR_PARENT   "/tmp"                 /* when $TMPDIR is not set */
#define TEST_DIR_NAME     "xwpe-test-startplan-XXXXXX"
#define TEXT_ROOM         512
#define TINY_ROOM         8                      /* a join that cannot fit */
#define TRACE_ROOM        8192
#define SHORT_DEADLINE_MS 10000L                 /* echo has answered long before */

/* Every allocation of the planner passes through these two (the test is
   linked with --wrap=malloc and --wrap=realloc, when the linker has them:
   WPE_TEST_LD_WRAP), so a row can make the Nth one fail and see what the
   planner answers.  Not armed, they are malloc and realloc.  Without the
   linker's help the rows that need them print SKIP. */
#define FAIL_NEVER (-1L)
static long fail_countdown = FAIL_NEVER;   /* allocations left before the next one fails */
#ifdef WPE_TEST_LD_WRAP
void *__real_malloc(size_t n);
void *__real_realloc(void *p, size_t n);

/* Whether the allocation now asked for is the one to fail. */
static int allocation_fails_now(void)
{
 if (fail_countdown == FAIL_NEVER)
  return 0;
 if (fail_countdown > 0)
 {
  fail_countdown--;
  return 0;
 }
 fail_countdown = FAIL_NEVER;
 return 1;
}

void *__wrap_malloc(size_t n)
{
 if (allocation_fails_now())
  return NULL;
 return __real_malloc(n);
}

void *__wrap_realloc(void *p, size_t n)
{
 if (allocation_fails_now())
  return NULL;
 return __real_realloc(p, n);
}
#endif /* WPE_TEST_LD_WRAP */

static int failures;
static char base[PATH_ROOM];     /* the temporary directory, resolved */
static char state[PATH_ROOM];    /* base/state: $XDG_STATE_HOME */
static char home[PATH_ROOM];     /* base/home: $HOME */
static char proj[PATH_ROOM];     /* base/proj: the project folder */
static char outside[PATH_ROOM];  /* base/outside: a folder beside it */
static char link_out[PATH_ROOM]; /* base/proj/out: a link to base/outside */
static char dbgbase[PATH_ROOM];  /* base/dbg: where the editor's debug folders are made */
static char traced[TRACE_ROOM];  /* the owner's trace lines */

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
 if (got && strcmp(got, want) == 0) return;
 snprintf(why, sizeof why, "got \"%s\", want \"%s\"", got ? got : "(null)", want);
 fail(name, why);
}

static void expect_has(const char *name, const char *text, const char *part)
{
 char why[PATH_ROOM];
 if (text && strstr(text, part)) return;
 snprintf(why, sizeof why, "\"%s\" not in \"%s\"", part, text ? text : "(null)");
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

static void on_trace(const char *line)
{
 size_t used = strlen(traced);
 if (used + strlen(line) + 2 < sizeof traced) {
  strcat(traced, line);
  strcat(traced, "\n");
 }
}

static void setup(void)
{
 char tmpl[PATH_ROOM], real[PATH_ROOM];
 const char *tmp = getenv("TMPDIR");
 if (!tmp || !*tmp) tmp = TEST_DIR_PARENT;
 snprintf(tmpl, sizeof tmpl, "%s/%s", tmp, TEST_DIR_NAME);
 setvbuf(stdout, NULL, _IONBF, 0);
 if (!mkdtemp(tmpl)) { perror("mkdtemp"); exit(2); }
 if (!realpath(tmpl, real)) { perror("realpath"); exit(2); }
 snprintf(base, sizeof base, "%s", real);
 under_base(state, sizeof state, "state");
 under_base(home, sizeof home, "home");
 under_base(proj, sizeof proj, "proj");
 under_base(outside, sizeof outside, "outside");
 under_base(link_out, sizeof link_out, "proj/out");
 under_base(dbgbase, sizeof dbgbase, "dbg");
 make_dir(home);
 make_dir(proj);
 make_dir(outside);
 make_dir(dbgbase);
 wpe_startplan_debug_base_set(dbgbase);
 if (symlink(outside, link_out) != 0) { perror("symlink"); exit(2); }
 setenv("XDG_STATE_HOME", state, 1);
 setenv("HOME", home, 1);
 wpe_startplan_trace_set(on_trace);
}

/* A build plan of the project folder, its xwperc the supplier. */
static WpeStartPlan *project_plan(void)
{
 WpeStartPlan *p = wpe_startplan_new(WPE_START_BUILD, "C");
 if (!p) { fail("plan", "out of memory"); exit(1); }
 wpe_startplan_set_file(p, "./xwperc", proj);
 wpe_startplan_set_cwd(p, proj);
 wpe_startplan_word(p, "gcc", WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
 wpe_startplan_word(p, "-c", WPE_WORD_OPTION, WPE_FROM_EDITOR, NULL);
 wpe_startplan_word(p, "-o", WPE_WORD_OPTION, WPE_FROM_EDITOR, NULL);
 return p;
}

static void case_operand(void)
{
 char out[PATH_ROOM];
 expect_int("operand -x", wpe_startplan_operand(out, sizeof out, NULL, "-x.c"), 0);
 expect_str("a name like an option is a file", out, "./-x.c");
 wpe_startplan_operand(out, sizeof out, "@r", "t.o");
 expect_str("a folder like a response file is a folder", out, "./@r/t.o");
 wpe_startplan_operand(out, sizeof out, "out/", "t.o");
 expect_str("a trailing separator is not doubled", out, "out/t.o");
 wpe_startplan_operand(out, sizeof out, "", "@lib.rsp");
 expect_str("an empty folder", out, "./@lib.rsp");
 expect_int("a path that does not fit is refused, never cut",
            wpe_startplan_operand(out, TINY_ROOM, "folder", "name.o"), -1);
}

static void case_folder_option(void)
{
 WpeStartPlan *p = project_plan();
 wpe_startplan_word(p, "-fplugin=./x.so", WPE_WORD_OPTION, WPE_FROM_FOLDER, "./xwperc: CompilerOptions");
 expect_int("a folder value made an option is refused", wpe_startplan_judge(p), WPE_PLAN_REFUSED);
 expect_str("and says why", p->refused, WPE_STARTPLAN_REFUSE_OPTION);
 wpe_startplan_free(p);
 p = project_plan();
 wpe_startplan_word(p, "-fplugin=./x.so", WPE_WORD_TEXT, WPE_FROM_FOLDER, "./xwperc: CompilerOptions");
 expect_int("a folder's option text asks", wpe_startplan_judge(p), WPE_PLAN_ASK);
 wpe_startplan_free(p);
}

static void case_clean(void)
{
 WpeStartPlan *p = project_plan();
 wpe_startplan_path(p, "t.o", WPE_PATH_WRITE, WPE_FROM_FOLDER, "./xwperc: Exedir", NULL);
 expect_int("a folder's write inside its folder asks nothing", wpe_startplan_judge(p), WPE_PLAN_CLEAN);
 wpe_startplan_free(p);
}

static void case_link_out(void)
{
 char want[PATH_ROOM];
 WpeStartPlan *p = project_plan();
 wpe_startplan_path(p, "out/t.o", WPE_PATH_WRITE, WPE_FROM_FOLDER, "./xwperc: Exedir", NULL);
 expect_int("a write through a link out of the folder asks", wpe_startplan_judge(p), WPE_PLAN_ASK);
 snprintf(want, sizeof want, "%s/t.o", outside);
 expect_str("the link is followed", p->path[0].resolved, want);
 expect_int("and the path is outside", p->path[0].inside, 0);
 expect_has("the text names the resolved path", wpe_startplan_text(p), want);
 wpe_startplan_free(p);
}

/* A link whose target is not there yet: the linker writes through it and
   creates the target, so the plan must judge the target, not the link. */
static void case_dangling_link_out(void)
{
 char link[PATH_ROOM], target[PATH_ROOM], loop[PATH_ROOM];
 WpeStartPlan *p = project_plan();
 under_base(link, sizeof link, "proj/demo");
 under_base(target, sizeof target, "outside/git");
 under_base(loop, sizeof loop, "proj/loop");
 if (symlink("../outside/git", link) != 0) { perror("symlink"); exit(2); }
 if (symlink("loop", loop) != 0) { perror("symlink"); exit(2); }
 wpe_startplan_path(p, "demo", WPE_PATH_WRITE, WPE_FROM_FOLDER, "p.prj: EXENAME", NULL);
 expect_int("a write through a dangling link out of the folder asks", wpe_startplan_judge(p),
            WPE_PLAN_ASK);
 expect_str("the dangling link is followed", p->path[0].resolved, target);
 wpe_startplan_free(p);
 p = project_plan();
 wpe_startplan_path(p, "loop", WPE_PATH_WRITE, WPE_FROM_FOLDER, "p.prj: EXENAME", NULL);
 expect_int("a link that never ends is not inside", wpe_startplan_judge(p), WPE_PLAN_ASK);
 wpe_startplan_free(p);
 unlink(link);
 unlink(loop);
}

static void case_remove(void)
{
 char inside_obj[PATH_ROOM], outside_obj[PATH_ROOM];
 WpeStartPlan *p = project_plan();
 snprintf(inside_obj, sizeof inside_obj, "%s/t.o", proj);
 snprintf(outside_obj, sizeof outside_obj, "%s/t.o", outside);
 write_file(inside_obj, "stale\n");
 write_file(outside_obj, "marker\n");
 wpe_startplan_path(p, inside_obj, WPE_PATH_REMOVE, WPE_FROM_FOLDER, "./xwperc: Exedir", NULL);
 wpe_startplan_path(p, outside_obj, WPE_PATH_REMOVE, WPE_FROM_FOLDER, "./xwperc: Exedir", NULL);
 wpe_startplan_path(p, outside_obj, WPE_PATH_WRITE, WPE_FROM_FOLDER, "./xwperc: Exedir", NULL);
 traced[0] = '\0';
 expect_int("a write outside asks", wpe_startplan_judge(p), WPE_PLAN_ASK);
 expect_int("a removal outside is dropped", p->path[1].dropped, 1);
 expect_int("a removal inside is kept", p->path[0].dropped, 0);
 expect_has("the drop is traced", traced, "startplan dropped-remove");
 expect_int("the removal inside is done", wpe_startplan_remove(p, inside_obj), WPE_STARTPLAN_REMOVED);
 expect_int("and the file is gone", access(inside_obj, F_OK), -1);
 expect_int("the removal outside is not done", wpe_startplan_remove(p, outside_obj), WPE_STARTPLAN_NOT_REMOVED);
 expect_int("and the file is still there", access(outside_obj, F_OK), 0);
 expect_int("a path the plan does not list is not removed",
            wpe_startplan_remove(p, "elsewhere.o"), WPE_STARTPLAN_NOT_PLANNED);
 unlink(outside_obj);
 wpe_startplan_free(p);
}

static void case_users_own_outside(void)
{
 char obj[PATH_ROOM];
 WpeStartPlan *p = project_plan();
 snprintf(obj, sizeof obj, "%s/t.o", outside);
 wpe_startplan_path(p, obj, WPE_PATH_WRITE, WPE_FROM_USER, "your xwperc: Exedir", NULL);
 wpe_startplan_path(p, obj, WPE_PATH_REMOVE, WPE_FROM_USER, "your xwperc: Exedir", NULL);
 write_file(obj, "stale\n");
 traced[0] = '\0';
 expect_int("the user's own write outside asks nothing", wpe_startplan_judge(p), WPE_PLAN_CLEAN);
 expect_int("his own removal outside is not dropped", p->path[1].dropped, 0);
 expect_int("it is done as it always was", wpe_startplan_remove(p, obj), WPE_STARTPLAN_REMOVED);
 expect_int("and the stale object is gone", access(obj, F_OK), -1);
 expect_int("and nothing is said about it", strstr(traced, "dropped-remove") != NULL, 0);
 wpe_startplan_free(p);
}

/* A project in the home folder links its program over a file beside it:
   @content is what is there (NULL: the program is named ".prog"). */
static void home_named_product_case(const char *name, const char *exe, const char *content,
                                    int want);

static void home_product_case(const char *name, const char *content, int want)
{
 if (!content)
  home_named_product_case(name, ".prog", content, want);
 else
  home_named_product_case(name, "prog", content, want);
}

/* A project in the home folder links its product @exe over what is there
   (@content, or nothing when NULL). */
static void home_named_product_case(const char *name, const char *exe, const char *content,
                                    int want)
{
 char path[PATH_ROOM];
 WpeStartPlan *p = wpe_startplan_new(WPE_START_PROJECT, "p.prj");
 snprintf(path, sizeof path, "%s/%s", home, exe);
 if (content)
  write_file(path, content);
 wpe_startplan_set_file(p, "p.prj", home);
 wpe_startplan_set_cwd(p, home);
 wpe_startplan_word(p, "gcc", WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
 wpe_startplan_path(p, exe, WPE_PATH_WRITE, WPE_FROM_FOLDER, "p.prj: EXENAME", NULL);
 wpe_startplan_mark_output(p, WPE_OUTPUT_PRODUCT);
 expect_int(name, wpe_startplan_judge(p), want);
 wpe_startplan_free(p);
 unlink(path);
}

/* A project in the home folder compiles a.c into a.o over what is there
   (@content); the stale object is removed first. */
static void home_object_case(const char *name, const char *content, int want)
{
 char path[PATH_ROOM];
 WpeStartPlan *p = wpe_startplan_new(WPE_START_PROJECT, "p.prj");
 snprintf(path, sizeof path, "%s/a.o", home);
 write_file(path, content);
 wpe_startplan_set_file(p, "p.prj", home);
 wpe_startplan_set_cwd(p, home);
 wpe_startplan_word(p, "gcc", WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
 wpe_startplan_path(p, path, WPE_PATH_WRITE, WPE_FROM_FOLDER, "p.prj: FILES", NULL);
 wpe_startplan_mark_output(p, WPE_OUTPUT_OBJECT);
 wpe_startplan_path(p, path, WPE_PATH_REMOVE, WPE_FROM_FOLDER, "p.prj: FILES", NULL);
 wpe_startplan_mark_output(p, WPE_OUTPUT_OBJECT);
 expect_int(name, wpe_startplan_judge(p), want);
 if (want == WPE_PLAN_ASK)
 {
  expect_int("and its removal is not done", wpe_startplan_remove(p, path), WPE_STARTPLAN_NOT_REMOVED);
  expect_int("so the text is still there", access(path, F_OK), 0);
 }
 wpe_startplan_free(p);
 unlink(path);
}

static void case_home_is_never_a_root(void)
{
 char bin[PATH_ROOM];
 WpeStartPlan *p = wpe_startplan_new(WPE_START_PROJECT, "p.prj");
 snprintf(bin, sizeof bin, "%s/.local/bin/git", home);
 wpe_startplan_set_file(p, "p.prj", home);
 wpe_startplan_set_cwd(p, home);
 wpe_startplan_word(p, "gcc", WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
 wpe_startplan_path(p, bin, WPE_PATH_WRITE, WPE_FROM_FOLDER, "p.prj: EXENAME", NULL);
 expect_int("the home folder is no root", wpe_startplan_root_usable(home), 0);
 expect_int("nor any folder above it", wpe_startplan_root_usable(base), 0);
 expect_int("nor /", wpe_startplan_root_usable("/"), 0);
 expect_int("a project folder is one", wpe_startplan_root_usable(proj), 1);
 expect_int("a project in the home folder writing ~/.local/bin asks", wpe_startplan_judge(p), WPE_PLAN_ASK);
 wpe_startplan_free(p);

 p = wpe_startplan_new(WPE_START_PROJECT, "p.prj");
 wpe_startplan_set_file(p, "p.prj", home);
 wpe_startplan_set_cwd(p, home);
 wpe_startplan_word(p, "gcc", WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
 wpe_startplan_path(p, "a.o", WPE_PATH_WRITE, WPE_FROM_FOLDER, "p.prj: FILES", NULL);
 wpe_startplan_mark_output(p, WPE_OUTPUT_OBJECT);
 wpe_startplan_path(p, "prog", WPE_PATH_WRITE, WPE_FROM_FOLDER, "p.prj: EXENAME", NULL);
 wpe_startplan_mark_output(p, WPE_OUTPUT_PRODUCT);
 expect_int("a project in the home folder building a new program beside itself asks nothing",
            wpe_startplan_judge(p), WPE_PLAN_CLEAN);
 wpe_startplan_free(p);

 home_product_case("an ELF program beside it is rebuilt unasked", "\177ELF\002\001", WPE_PLAN_CLEAN);
 home_named_product_case("a library named *.a beside it is rebuilt unasked", "libp.a",
                         "!<arch>\nx", WPE_PLAN_CLEAN);
 home_named_product_case("a downloaded package (an ar archive) beside it asks", "tool.deb",
                         "!<arch>\ndebian-binary   ", WPE_PLAN_ASK);
 home_product_case("an ar archive under a program's name asks", "!<arch>\nx", WPE_PLAN_ASK);
 home_object_case("an ar archive named like an object asks", "!<arch>\nx", WPE_PLAN_ASK);
 home_product_case("a text beside it is not the build's to overwrite", "my notes\n", WPE_PLAN_ASK);
 home_product_case("a script beside it asks", "#!/bin/sh\necho hi\n", WPE_PLAN_ASK);
 home_product_case("a dot file beside it asks", NULL, WPE_PLAN_ASK);

 p = wpe_startplan_new(WPE_START_PROJECT, "p.prj");
 wpe_startplan_set_file(p, "p.prj", home);
 wpe_startplan_set_cwd(p, home);
 wpe_startplan_word(p, "gcc", WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
 wpe_startplan_path(p, "notes.txt", WPE_PATH_WRITE, WPE_FROM_FOLDER, "p.prj: EXENAME", NULL);
 expect_int("a path beside it that is no output of the build asks", wpe_startplan_judge(p),
            WPE_PLAN_ASK);
 wpe_startplan_free(p);

 home_object_case("an object over a text beside it asks", "my notes\n", WPE_PLAN_ASK);
 home_object_case("an object a compile made before is rebuilt unasked", "\177ELF\002\001",
                  WPE_PLAN_CLEAN);
}

/* Without $HOME the home folder is still known, from the user database,
   and is still no root. */
static void case_home_unset(void)
{
 struct passwd *pw = getpwuid(getuid());
 char saved[PATH_ROOM];

 if (!pw || !pw->pw_dir || !*pw->pw_dir)
  return;
 snprintf(saved, sizeof saved, "%s", getenv("HOME"));
 unsetenv("HOME");
 expect_int("without $HOME the home folder is no root", wpe_startplan_root_usable(pw->pw_dir), 0);
 setenv("HOME", saved, 1);
}

static void case_own_and_store(void)
{
 WpeStartPlan *p = project_plan(), *own = project_plan();
 wpe_startplan_word(p, "-g", WPE_WORD_TEXT, WPE_FROM_FOLDER, "./xwperc: CompilerOptions");
 wpe_startplan_word(own, "-g", WPE_WORD_OPTION, WPE_FROM_USER, NULL);
 wpe_startplan_set_own(p, own);
 expect_int("a plan equal in effect to the user's own runs", wpe_startplan_judge(p), WPE_PLAN_RUN);
 expect_str("the text never says where a value came from",
            wpe_startplan_text(p), wpe_startplan_text(p->own));
 wpe_startplan_free(p);
 wpe_startcmd_reset();

 p = project_plan();
 wpe_startplan_word(p, "-O2", WPE_WORD_TEXT, WPE_FROM_FOLDER, "./xwperc: CompilerOptions");
 expect_int("a folder's plan asks", wpe_startplan_judge(p), WPE_PLAN_ASK);
 expect_int("Run it is kept", wpe_startplan_remember(p, WPE_PLAN_RUN), 0);
 wpe_startplan_free(p);
 p = project_plan();
 wpe_startplan_word(p, "-O2", WPE_WORD_TEXT, WPE_FROM_FOLDER, "./xwperc: CompilerOptions");
 expect_int("the same plan runs the next time", wpe_startplan_judge(p), WPE_PLAN_RUN);
 wpe_startplan_free(p);
 p = project_plan();
 wpe_startplan_word(p, "-O3", WPE_WORD_TEXT, WPE_FROM_FOLDER, "./xwperc: CompilerOptions");
 expect_int("a changed plan asks again", wpe_startplan_judge(p), WPE_PLAN_ASK);
 expect_int("Use mine is kept too", wpe_startplan_remember(p, WPE_PLAN_USE_OWN), 0);
 wpe_startplan_free(p);
 p = project_plan();
 wpe_startplan_word(p, "-O2", WPE_WORD_TEXT, WPE_FROM_FOLDER, "./xwperc: CompilerOptions");
 expect_int("the first answer still stands beside the second", wpe_startplan_judge(p), WPE_PLAN_RUN);
 wpe_startplan_free(p);
 p = project_plan();
 wpe_startplan_word(p, "-O3", WPE_WORD_TEXT, WPE_FROM_FOLDER, "./xwperc: CompilerOptions");
 expect_int("Use mine is remembered as such", wpe_startplan_judge(p), WPE_PLAN_USE_OWN);
 wpe_startplan_free(p);
}

static void case_own_outside_is_not_usable(void)
{
 char bin[PATH_ROOM];
 WpeStartPlan *p = project_plan(), *own = project_plan();
 snprintf(bin, sizeof bin, "%s/prog", outside);
 wpe_startplan_path(p, bin, WPE_PATH_WRITE, WPE_FROM_FOLDER, "p.prj: EXENAME", NULL);
 wpe_startplan_path(own, bin, WPE_PATH_WRITE, WPE_FROM_FOLDER, "p.prj: EXENAME", NULL);
 wpe_startplan_set_own(p, own);
 expect_int("an own plan writing a folder's path outside does not make it silent",
            wpe_startplan_judge(p), WPE_PLAN_ASK);
 expect_int("and Use mine is not offered for it", wpe_startplan_own_usable(p), 0);
 wpe_startplan_free(p);
}

static void case_covers(void)
{
 char *ok[] = { "gcc", "-c", "t.c", "-o", "t.o", NULL };
 char *bad[] = { "gcc", "-c", "t.c", "-wrapper", NULL };
 const char *word;
 WpeStartPlan *p = project_plan();
 WpeStartIo io;
 WpeChild c;
 wpe_startplan_path(p, "t.c", WPE_PATH_INPUT, WPE_FROM_USER, NULL, NULL);
 wpe_startplan_path(p, "t.o", WPE_PATH_WRITE, WPE_FROM_USER, NULL, NULL);
 memset(&io, 0, sizeof io);
 memset(&c, 0, sizeof c);
 traced[0] = '\0';
 expect_int("an unjudged plan starts nothing", wpe_startplan_start(p, ok, &io, &c, NULL, NULL), -1);
 expect_has("and says so", traced, WPE_STARTPLAN_REFUSE_JUDGE);
 wpe_startplan_judge(p);
 expect_int("every word of the step is the plan's", wpe_startplan_covers(p, ok, &word), 1);
 expect_int("a word the plan did not show is caught", wpe_startplan_covers(p, bad, &word), 0);
 expect_str("and named", word, "-wrapper");
 traced[0] = '\0';
 expect_int("a step with such a word starts nothing", wpe_startplan_start(p, bad, &io, &c, NULL, NULL), -1);
 expect_has("and says which", traced, "-wrapper");
 wpe_startplan_free(p);
}

static void case_env(void)
{
 expect_int("HOME may expand", wpe_startplan_env_name_allowed("HOME"), 1);
 expect_int("TMPDIR may expand", wpe_startplan_env_name_allowed("TMPDIR"), 1);
 expect_int("a key never does", wpe_startplan_env_name_allowed("XWPE_AI_KEY"), 0);
 expect_int("a token never does", wpe_startplan_env_name_allowed("GITHUB_TOKEN"), 0);
 expect_int("nor another name", wpe_startplan_env_name_allowed("CC"), 0);
}

static void case_inside_list(void)
{
 WpeStartPlan *a = project_plan(), *b = project_plan();
 a->list_inside = b->list_inside = 1;
 wpe_startplan_word(a, "-O2", WPE_WORD_TEXT, WPE_FROM_FOLDER, NULL);
 wpe_startplan_word(b, "-O2", WPE_WORD_TEXT, WPE_FROM_FOLDER, NULL);
 wpe_startplan_path(a, "b.c", WPE_PATH_INPUT, WPE_FROM_FOLDER, NULL, NULL);
 wpe_startplan_path(a, "a.c", WPE_PATH_INPUT, WPE_FROM_FOLDER, NULL, NULL);
 wpe_startplan_path(b, "a.c", WPE_PATH_INPUT, WPE_FROM_FOLDER, NULL, NULL);
 wpe_startplan_path(b, "b.c", WPE_PATH_INPUT, WPE_FROM_FOLDER, NULL, NULL);
 wpe_startplan_judge(a);
 wpe_startplan_judge(b);
 expect_str("a project's files are listed sorted", wpe_startplan_text(a), wpe_startplan_text(b));
 expect_has("and listed", wpe_startplan_text(a), "inside a.c\ninside b.c");
 wpe_startplan_free(a);
 wpe_startplan_free(b);
}

/* The editor's SIGCHLD handler, in its essentials. */
static void on_child(int sig)
{
 int saved = errno, st;
 pid_t pid;
 (void)sig;
 while ((pid = waitpid(-1, &st, WNOHANG)) > 0)
  wpe_child_note_ended(pid, st);
 errno = saved;
}

static void on_child_trace(const char *line)
{
 on_trace(line);
}

/* A short command runs from its plan, the plan in the child's spec; one
   whose plan may not run starts nothing; a start with no plan is traced. */
static void case_short_command(void)
{
 char *echo[] = { "echo", "planned", NULL };
 char out[TEXT_ROOM];
 int status = 0;
 struct sigaction act;
 WpeStartPlan *p;

 memset(&act, 0, sizeof act);
 act.sa_handler = on_child;
 sigemptyset(&act.sa_mask);
 act.sa_flags = SA_RESTART | SA_NOCLDSTOP;
 WpeChildWakeOpen();
 sigaction(SIGCHLD, &act, NULL);
 wpe_child_trace_set(on_child_trace);

 p = wpe_startplan_fixed(WPE_START_PROBE, "echo", echo);
 expect_int("a short command's plan is clean", p ? p->verdict : -1, WPE_PLAN_CLEAN);
 traced[0] = '\0';
 expect_int("it runs", wpe_startplan_capture(p, out, sizeof out, SHORT_DEADLINE_MS, &status,
                                              "echo"), WPE_CAPTURE_ENDED);
 expect_str("and its output is read", out, "planned\n");
 expect_int("and no start without a plan is traced", strstr(traced, "child unplanned") != NULL, 0);
 wpe_startplan_free(p);

 p = wpe_startplan_fixed(WPE_START_PROBE, "echo", echo);
 wpe_startplan_word(p, "-O2", WPE_WORD_TEXT, WPE_FROM_FOLDER, NULL);
 wpe_startplan_set_file(p, "./xwperc", proj);
 wpe_startplan_judge(p);
 expect_int("a plan that asks starts nothing",
            wpe_startplan_capture(p, out, sizeof out, SHORT_DEADLINE_MS, &status, "echo"),
            WPE_CAPTURE_NOT_STARTED);
 wpe_startplan_free(p);

 traced[0] = '\0';
 wpe_child_capture(echo, out, sizeof out, SHORT_DEADLINE_MS, &status, "echo");
 expect_has("a start with no plan is traced", traced, "child unplanned echo");
 wpe_child_trace_set(NULL);
}

/* The size of a start-up file the box cannot show whole. */
#define LONG_STARTUP_BYTES (WPE_STARTPLAN_READS_MAX + 4096)

/* A debugger's plan in @dir, its start-up files listed for @tool. */
static WpeStartPlan *debugger_plan(const char *dir, const char *tool)
{
 WpeStartPlan *p = wpe_startplan_new(WPE_START_DEBUGGER, tool);
 if (!p) { fail("plan", "out of memory"); exit(1); }
 /* A debugger that starts apart is planned as the editor plans it: in a folder of its
    own, the program's folder kept for what is listed. */
 if (wpe_startplan_row_is_apart(tool, ""))
  wpe_startplan_contain(p, tool, "", dir, NULL);
 else
  wpe_startplan_set_cwd(p, dir);
 wpe_startplan_set_file(p, "./xwperc", dir);
 wpe_startplan_word(p, tool, WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
 wpe_startplan_tool_reads(p, tool);
 return p;
}

/* The path of @name in @dir. */
static void file_in(char *out, size_t sz, const char *dir, const char *name)
{
 snprintf(out, sz, "%s/%s", dir, name);
}

static void case_startup_listed(void)
{
 char rc[PATH_ROOM], want[PATH_ROOM];
 WpeStartPlan *p;

 file_in(rc, sizeof rc, proj, ".pdbrc");
 write_file(rc, "!open('ran','w').close()\n");
 p = debugger_plan(proj, "pdb");
 expect_int("pdb's .pdbrc in the working folder is listed", p->npath, 1);
 expect_int("as a file pdb runs", p->npath ? p->path[0].role : -1, WPE_PATH_READS);
 expect_int("so the plan asks", wpe_startplan_judge(p), WPE_PLAN_ASK);
 snprintf(want, sizeof want, "reads %s %d\n!open('ran','w').close()\n", rc, 25);
 expect_has("its text is part of the plan's text", wpe_startplan_text(p), want);
 expect_int("a short one is shown whole", wpe_startplan_reads_too_long(p), 0);
 expect_int("the plan is remembered", wpe_startplan_remember(p, WPE_PLAN_RUN), 0);
 wpe_startplan_free(p);

 p = debugger_plan(proj, "pdb");
 expect_int("the same .pdbrc runs once answered", wpe_startplan_judge(p), WPE_PLAN_RUN);
 wpe_startplan_free(p);
 write_file(rc, "!open('ran2','w').close()\n");
 p = debugger_plan(proj, "pdb");
 expect_int("a changed .pdbrc asks again", wpe_startplan_judge(p), WPE_PLAN_ASK);
 wpe_startplan_free(p);

 p = debugger_plan(proj, "gdb");
 expect_int("gdb reads no .pdbrc", p->npath, 0);
 expect_int("and a plan of the user's own words runs", wpe_startplan_judge(p), WPE_PLAN_CLEAN);
 wpe_startplan_free(p);
 unlink(rc);
}

static void case_startup_users_own(void)
{
 char rc[PATH_ROOM];
 WpeStartPlan *p;

 file_in(rc, sizeof rc, home, ".pdbrc");
 write_file(rc, "p 1\n");
 p = debugger_plan(proj, "pdb");
 expect_int("a .pdbrc in the home folder only is the user's, not listed", p->npath, 0);
 wpe_startplan_free(p);
 p = debugger_plan(home, "pdb");
 expect_int("started in the home folder, ./.pdbrc is ~/.pdbrc: not listed", p->npath, 0);
 expect_int("and nothing is asked", wpe_startplan_judge(p), WPE_PLAN_CLEAN);
 wpe_startplan_free(p);
 unlink(rc);
}

static void case_startup_long(void)
{
 char rc[PATH_ROOM], *text = malloc(LONG_STARTUP_BYTES + 1);
 WpeStartPlan *p;

 if (!text) { fail("long .pdbrc", "out of memory"); return; }
 memset(text, 'p', LONG_STARTUP_BYTES);
 text[LONG_STARTUP_BYTES] = '\0';
 file_in(rc, sizeof rc, proj, ".pdbrc");
 write_file(rc, text);
 free(text);
 p = debugger_plan(proj, "pdb");
 expect_int("a 20 kB .pdbrc is listed", p->npath, 1);
 expect_int("but not shown whole: Run it is withheld", wpe_startplan_reads_too_long(p), 1);
 expect_int("its whole size is said", p->npath ? (int)p->path[0].length : -1,
            LONG_STARTUP_BYTES);
 wpe_startplan_free(p);
 unlink(rc);
}

/* A .pdbrc whose harmless first line ends in a NUL byte: the box shows
   text only up to it, pdb reads on, so it is not shown whole. */
static const char NUL_STARTUP[] = "p 1\0\n!open('ran','w').close()\n";

static void write_bytes(const char *path, const char *bytes, size_t n)
{
 FILE *fp = fopen(path, "wb");
 if (!fp) { perror(path); exit(2); }
 fwrite(bytes, 1, n, fp);
 fclose(fp);
}

static void case_startup_nul(void)
{
 char rc[PATH_ROOM];
 WpeStartPlan *p;

 file_in(rc, sizeof rc, proj, ".pdbrc");
 write_bytes(rc, NUL_STARTUP, sizeof NUL_STARTUP - 1);
 p = debugger_plan(proj, "pdb");
 expect_int("a .pdbrc with a NUL is listed", p->npath, 1);
 expect_int("but not shown whole: Run it is withheld", wpe_startplan_reads_too_long(p), 1);
 wpe_startplan_free(p);
 unlink(rc);
}

/* At the limit a file is shown whole; one byte past it, it is not. */
static void case_startup_at_the_limit(void)
{
 char rc[PATH_ROOM], *text = malloc(WPE_STARTPLAN_READS_MAX + 2);
 WpeStartPlan *p;

 if (!text) { fail(".pdbrc at the limit", "out of memory"); return; }
 memset(text, 'p', WPE_STARTPLAN_READS_MAX + 1);
 text[WPE_STARTPLAN_READS_MAX] = '\0';
 file_in(rc, sizeof rc, proj, ".pdbrc");
 write_file(rc, text);
 p = debugger_plan(proj, "pdb");
 expect_int("a .pdbrc of exactly the limit is shown whole", wpe_startplan_reads_too_long(p), 0);
 wpe_startplan_free(p);
 text[WPE_STARTPLAN_READS_MAX] = 'p';
 text[WPE_STARTPLAN_READS_MAX + 1] = '\0';
 write_file(rc, text);
 p = debugger_plan(proj, "pdb");
 expect_int("one byte past the limit it is not", wpe_startplan_reads_too_long(p), 1);
 wpe_startplan_free(p);
 free(text);
 unlink(rc);
}

static void case_startup_rows(void)
{
 char ini[PATH_ROOM], jdbrc[PATH_ROOM], gdbinit[PATH_ROOM], perldb[PATH_ROOM];
 WpeStartPlan *p;

 file_in(ini, sizeof ini, proj, "jdb.ini");
 file_in(jdbrc, sizeof jdbrc, proj, ".jdbrc");
 file_in(gdbinit, sizeof gdbinit, proj, ".gdbinit");
 file_in(perldb, sizeof perldb, proj, ".perldb");
 write_file(jdbrc, "run\n");
 p = debugger_plan(proj, "jdb");
 expect_int("jdb's .jdbrc is listed when there is no jdb.ini", p->npath, 1);
 wpe_startplan_free(p);
 write_file(ini, "run\n");
 p = debugger_plan(proj, "jdb");
 expect_int("jdb.ini wins over .jdbrc in one folder", p->npath, 1);
 expect_has("and it is jdb.ini", p->npath ? p->path[0].path : "", "jdb.ini");
 wpe_startplan_free(p);
 write_file(gdbinit, "shell true\n");
 p = debugger_plan(proj, "gdb");
 expect_int("gdb's ./.gdbinit is listed", p->npath, 1);
 expect_int("with no condition: it runs only when Run it loads it", p->npath && p->path[0].note == NULL, 1);
 wpe_startplan_free(p);
 write_file(perldb, "1;\n");
 p = debugger_plan(proj, "perl");
 expect_int("no perl debugger is started: its row is off", p->npath, 0);
 expect_int("and the table does not claim it", wpe_startplan_tool_reads_known("perl"), 0);
 wpe_startplan_free(p);
 expect_int("sdb reads nothing known", wpe_startplan_tool_reads_known("sdb"), 0);
 unlink(ini);
 unlink(jdbrc);
 unlink(gdbinit);
 unlink(perldb);
}

/* gdb's plan in proj with its start-up files listed for @program. */
static WpeStartPlan *gdb_plan_of(const char *program)
{
 WpeStartPlan *p = wpe_startplan_new(WPE_START_DEBUGGER, "gdb");
 if (!p) { fail("plan", "out of memory"); exit(1); }
 wpe_startplan_contain(p, "gdb", "", proj, NULL);
 wpe_startplan_word(p, "gdb", WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
 wpe_startplan_tool_reads_of(p, "gdb", program);
 return p;
}

static void case_startup_program_scripts(void)
{
 char py[PATH_ROOM], sub[PATH_ROOM], real[PATH_ROOM], script[PATH_ROOM], link[PATH_ROOM];
 WpeStartPlan *p;

 file_in(py, sizeof py, proj, "t.e-gdb.py");
 write_file(py, "open('ran','w').close()\n");
 p = debugger_plan(proj, "gdb");
 expect_int("with no program known, no script named after it is listed", p->npath, 0);
 wpe_startplan_free(p);
 p = gdb_plan_of("t.e");
 expect_int("gdb's t.e-gdb.py beside the program is listed", p->npath, 1);
 expect_str("as the box shows it", p->npath ? p->path[0].path : "", "./t.e-gdb.py");
 expect_int("as a file gdb runs", p->npath ? p->path[0].role : -1, WPE_PATH_READS);
 expect_int("with no condition: it runs only when Run it loads it",
            p->npath && p->path[0].note == NULL, 1);
 expect_int("so the plan asks", wpe_startplan_judge(p), WPE_PLAN_ASK);
 wpe_startplan_free(p);
 p = gdb_plan_of("./t.e");
 expect_str("a program given as ./t.e names the same script",
            p->npath ? p->path[0].path : "", "./t.e-gdb.py");
 wpe_startplan_free(p);
 p = gdb_plan_of("u.e");
 expect_int("another program's script is not this one's", p->npath, 0);
 wpe_startplan_free(p);
 unlink(py);

 file_in(sub, sizeof sub, proj, "sub");
 mkdir(sub, 0700);
 file_in(real, sizeof real, sub, "real.e");
 write_file(real, "");
 file_in(script, sizeof script, sub, "real.e-gdb.gdb");
 write_file(script, "shell true\n");
 file_in(link, sizeof link, proj, "link.e");
 if (symlink(real, link) != 0) { perror(link); exit(2); }
 p = gdb_plan_of("link.e");
 expect_int("a program reached through a link has its real file's script listed", p->npath, 1);
 expect_has("by the real path", p->npath ? p->path[0].path : "", "sub/real.e-gdb.gdb");
 wpe_startplan_free(p);
 unlink(link);
 unlink(script);
 unlink(real);
 rmdir(sub);
}

/* A small ELF file built by hand: a header, one section of gdb scripts, the
   table of the sections' names, three section headers (null, scripts,
   names).  Both classes, both byte orders. */
#define ELF_ROOM        512
#define ELF_SECTIONS    3
#define ELF_NULL        0
#define ELF_SCRIPTS     1
#define ELF_NAMES       2
#define ELF_NAMES_TEXT  "\0.debug_gdb_scripts\0.shstrtab"
#define ELF_NAMES_LEN   30
#define ELF_NAME_SCRIPTS 1
#define ELF_NAME_NAMES   20
#define ELF_PROGBITS    1
#define ELF_STRTAB      3
#define ELF_NOBITS      8
#define ELF_SECTION_NAME ".debug_gdb_scripts"

typedef struct {
 unsigned char b[ELF_ROOM];
 size_t len;
 int wide, big;
 size_t table, entsize;
} elf_image;

static void elf_put(unsigned char *p, unsigned size, int big, unsigned long long v)
{
 unsigned i;
 for (i = 0; i < size; i++)
  p[big ? size - 1 - i : i] = (unsigned char)(v >> (8 * i));
}

/* Where field @at (a 64-bit class offset) of section header @idx lies. */
static unsigned char *elf_header_at(elf_image *e, int idx, unsigned at)
{
 return e->b + e->table + (size_t)idx * e->entsize + at;
}

/* Set one section header's name, type, offset and size. */
static void elf_set_section(elf_image *e, int idx, unsigned long long name, unsigned long long type,
                            unsigned long long offset, unsigned long long size)
{
 elf_put(elf_header_at(e, idx, 0), 4, e->big, name);
 elf_put(elf_header_at(e, idx, 4), 4, e->big, type);
 if (e->wide)
 {
  elf_put(elf_header_at(e, idx, 24), 8, e->big, offset);
  elf_put(elf_header_at(e, idx, 32), 8, e->big, size);
 }
 else
 {
  elf_put(elf_header_at(e, idx, 16), 4, e->big, offset);
  elf_put(elf_header_at(e, idx, 20), 4, e->big, size);
 }
}

static void elf_make(elf_image *e, int wide, int big, const char *payload, size_t plen)
{
 size_t header = wide ? 64 : 52, names;
 memset(e, 0, sizeof *e);
 e->wide = wide;
 e->big = big;
 e->entsize = wide ? 64 : 40;
 memcpy(e->b, "\177ELF", 4);
 e->b[4] = wide ? 2 : 1;
 e->b[5] = big ? 2 : 1;
 e->b[6] = 1;
 memcpy(e->b + header, payload, plen);
 names = header + plen;
 memcpy(e->b + names, ELF_NAMES_TEXT, ELF_NAMES_LEN);
 e->table = names + ELF_NAMES_LEN;
 e->len = e->table + ELF_SECTIONS * e->entsize;
 if (wide)
 {
  elf_put(e->b + 0x28, 8, big, e->table);
  elf_put(e->b + 0x3A, 2, big, e->entsize);
  elf_put(e->b + 0x3C, 2, big, ELF_SECTIONS);
  elf_put(e->b + 0x3E, 2, big, ELF_NAMES);
 }
 else
 {
  elf_put(e->b + 0x20, 4, big, e->table);
  elf_put(e->b + 0x2E, 2, big, e->entsize);
  elf_put(e->b + 0x30, 2, big, ELF_SECTIONS);
  elf_put(e->b + 0x32, 2, big, ELF_NAMES);
 }
 elf_set_section(e, ELF_SCRIPTS, ELF_NAME_SCRIPTS, ELF_PROGBITS, header, plen);
 elf_set_section(e, ELF_NAMES, ELF_NAME_NAMES, ELF_STRTAB, names, ELF_NAMES_LEN);
}

/* The image, or its first @len bytes, as a file; the section read from it. */
static int elf_read(const elf_image *e, size_t len, const char *section, unsigned char *out,
                    size_t max, size_t *size)
{
 char path[PATH_ROOM];
 file_in(path, sizeof path, proj, "t.elf");
 write_bytes(path, (const char *)e->b, len);
 *size = 0;
 return wpe_startplan_elf_section(path, section, out, max, size);
}

static const char ELF_PAYLOAD[] = "\001emb.py\0";
#define ELF_PAYLOAD_LEN (sizeof ELF_PAYLOAD - 1)   /* the entry, its NUL included */

static void case_elf_kinds(void)
{
 static const int wide[] = { 0, 1, 0, 1 }, big[] = { 0, 0, 1, 1 };
 static const char *const name[] = { "32-bit little", "64-bit little", "32-bit big", "64-bit big" };
 unsigned char out[ELF_ROOM];
 char what[TEXT_ROOM];
 elf_image e;
 size_t size;
 int i;

 for (i = 0; i < 4; i++)
 {
  elf_make(&e, wide[i], big[i], ELF_PAYLOAD, ELF_PAYLOAD_LEN);
  snprintf(what, sizeof what, "%s-endian ELF: the section is found", name[i]);
  expect_int(what, elf_read(&e, e.len, ELF_SECTION_NAME, out, sizeof out, &size), 1);
  expect_int("with its whole size", (int)size, (int)ELF_PAYLOAD_LEN);
  expect_int("and its bytes", memcmp(out, ELF_PAYLOAD, ELF_PAYLOAD_LEN), 0);
  expect_int("another section's name is not it",
             elf_read(&e, e.len, ".nothing", out, sizeof out, &size), 0);
 }
 elf_make(&e, 1, 0, ELF_PAYLOAD, ELF_PAYLOAD_LEN);
 expect_int("a short buffer is filled and the size is still the whole",
            elf_read(&e, e.len, ELF_SECTION_NAME, out, 3, &size), 1);
 expect_int("the size", (int)size, (int)ELF_PAYLOAD_LEN);
}

static void case_elf_not_elf(void)
{
 unsigned char out[ELF_ROOM];
 char path[PATH_ROOM];
 elf_image e;
 size_t size;

 file_in(path, sizeof path, proj, "t.elf");
 write_file(path, "#!/bin/sh\necho not a program\n");
 expect_int("a script is not ELF", wpe_startplan_elf_section(path, ELF_SECTION_NAME, out,
                                                           sizeof out, &size), 0);
 write_file(path, "");
 expect_int("an empty file has no section", wpe_startplan_elf_section(path, ELF_SECTION_NAME, out,
                                                                    sizeof out, &size), 0);
 expect_int("a file that is not there has none", wpe_startplan_elf_section(
            "/nonexistent/t.elf", ELF_SECTION_NAME, out, sizeof out, &size), 0);
 expect_int("a folder has none", wpe_startplan_elf_section(proj, ELF_SECTION_NAME, out,
                                                         sizeof out, &size), 0);
 elf_make(&e, 1, 0, ELF_PAYLOAD, ELF_PAYLOAD_LEN);
 e.b[4] = 3;
 expect_int("an unknown class is no section", elf_read(&e, e.len, ELF_SECTION_NAME, out,
                                                       sizeof out, &size), 0);
 elf_make(&e, 1, 0, ELF_PAYLOAD, ELF_PAYLOAD_LEN);
 e.b[5] = 0;
 expect_int("an unknown byte order is no section", elf_read(&e, e.len, ELF_SECTION_NAME, out,
                                                            sizeof out, &size), 0);
 unlink(path);
}

static void case_elf_cut_short(void)
{
 unsigned char out[ELF_ROOM];
 elf_image e;
 size_t size, cuts[] = { 3, 10, 40, 70, 0 }, i;

 elf_make(&e, 1, 0, ELF_PAYLOAD, ELF_PAYLOAD_LEN);
 for (i = 0; cuts[i]; i++)
  expect_int("a file cut inside its header or before its table has no section",
             elf_read(&e, cuts[i], ELF_SECTION_NAME, out, sizeof out, &size), 0);
 expect_int("a file cut inside its last section header has none",
            elf_read(&e, e.len - 1, ELF_SECTION_NAME, out, sizeof out, &size), 0);
 elf_make(&e, 0, 1, ELF_PAYLOAD, ELF_PAYLOAD_LEN);
 expect_int("the same for a 32-bit file",
            elf_read(&e, e.len - 1, ELF_SECTION_NAME, out, sizeof out, &size), 0);
}

static void case_elf_wild_numbers(void)
{
 unsigned char out[ELF_ROOM];
 elf_image e;
 size_t size;

 elf_make(&e, 1, 0, ELF_PAYLOAD, ELF_PAYLOAD_LEN);
 elf_put(e.b + 0x3C, 2, 0, 65535);
 expect_int("e_shnum 65535 in a small file reaches past its end",
            elf_read(&e, e.len, ELF_SECTION_NAME, out, sizeof out, &size), 0);
 elf_make(&e, 1, 0, ELF_PAYLOAD, ELF_PAYLOAD_LEN);
 elf_put(e.b + 0x3C, 2, 0, 0);
 expect_int("e_shnum 0 is no section",
            elf_read(&e, e.len, ELF_SECTION_NAME, out, sizeof out, &size), 0);
 elf_make(&e, 1, 0, ELF_PAYLOAD, ELF_PAYLOAD_LEN);
 elf_put(e.b + 0x3A, 2, 0, 65535);
 expect_int("a section header size of 65535 reaches past the end",
            elf_read(&e, e.len, ELF_SECTION_NAME, out, sizeof out, &size), 0);
 elf_make(&e, 1, 0, ELF_PAYLOAD, ELF_PAYLOAD_LEN);
 elf_put(e.b + 0x3A, 2, 0, 8);
 expect_int("a section header smaller than a header is no section",
            elf_read(&e, e.len, ELF_SECTION_NAME, out, sizeof out, &size), 0);
 elf_make(&e, 1, 0, ELF_PAYLOAD, ELF_PAYLOAD_LEN);
 elf_put(e.b + 0x28, 8, 0, 0xFFFFFFFFFFFFFFF0ULL);
 expect_int("a table at an offset that wraps is no section",
            elf_read(&e, e.len, ELF_SECTION_NAME, out, sizeof out, &size), 0);
 elf_make(&e, 0, 1, ELF_PAYLOAD, ELF_PAYLOAD_LEN);
 elf_put(e.b + 0x30, 2, 1, 65535);
 expect_int("e_shnum 65535 in a small 32-bit file reaches past its end",
            elf_read(&e, e.len, ELF_SECTION_NAME, out, sizeof out, &size), 0);
}

static void case_elf_bad_sections(void)
{
 unsigned char out[ELF_ROOM];
 elf_image e;
 size_t size;

 elf_make(&e, 1, 0, ELF_PAYLOAD, ELF_PAYLOAD_LEN);
 elf_put(e.b + 0x3E, 2, 0, ELF_SECTIONS);
 expect_int("a names section past the table is no section",
            elf_read(&e, e.len, ELF_SECTION_NAME, out, sizeof out, &size), 0);
 elf_make(&e, 1, 0, ELF_PAYLOAD, ELF_PAYLOAD_LEN);
 elf_set_section(&e, ELF_NAMES, ELF_NAME_NAMES, ELF_STRTAB, e.len + 1000, ELF_NAMES_LEN);
 expect_int("a names table out of the file is no section",
            elf_read(&e, e.len, ELF_SECTION_NAME, out, sizeof out, &size), 0);
 elf_make(&e, 1, 0, ELF_PAYLOAD, ELF_PAYLOAD_LEN);
 elf_set_section(&e, ELF_NAMES, ELF_NAME_NAMES, ELF_STRTAB, 64, 0xFFFFFFFFULL);
 expect_int("a names table of 4 GB in a small file is no section",
            elf_read(&e, e.len, ELF_SECTION_NAME, out, sizeof out, &size), 0);
 elf_make(&e, 0, 0, ELF_PAYLOAD, ELF_PAYLOAD_LEN);
 elf_set_section(&e, ELF_NAMES, ELF_NAME_NAMES, ELF_STRTAB, 0xFFFFFFF0ULL, 0x20);
 expect_int("a 32-bit names table whose end wraps is no section",
            elf_read(&e, e.len, ELF_SECTION_NAME, out, sizeof out, &size), 0);
 elf_make(&e, 1, 0, ELF_PAYLOAD, ELF_PAYLOAD_LEN);
 elf_set_section(&e, ELF_SCRIPTS, 1000, ELF_PROGBITS, 64, ELF_PAYLOAD_LEN);
 expect_int("a name past the names table is not found",
            elf_read(&e, e.len, ELF_SECTION_NAME, out, sizeof out, &size), 0);
 elf_make(&e, 1, 0, ELF_PAYLOAD, ELF_PAYLOAD_LEN);
 elf_set_section(&e, ELF_SCRIPTS, ELF_NAME_SCRIPTS, ELF_PROGBITS, 64, 100000);
 expect_int("a section whose bytes pass the end of the file is no section",
            elf_read(&e, e.len, ELF_SECTION_NAME, out, sizeof out, &size), 0);
 elf_make(&e, 1, 0, ELF_PAYLOAD, ELF_PAYLOAD_LEN);
 elf_set_section(&e, ELF_SCRIPTS, ELF_NAME_SCRIPTS, ELF_NOBITS, 64, ELF_PAYLOAD_LEN);
 expect_int("a section with no bytes in the file is no section",
            elf_read(&e, e.len, ELF_SECTION_NAME, out, sizeof out, &size), 0);
 elf_make(&e, 1, 0, ELF_PAYLOAD, ELF_PAYLOAD_LEN);
 elf_set_section(&e, ELF_NAMES, ELF_NAME_NAMES, ELF_STRTAB, 64 + ELF_PAYLOAD_LEN,
                 ELF_NAMES_LEN - 20);
 expect_int("a names table cut inside the wanted name does not find it",
            elf_read(&e, e.len, ELF_SECTION_NAME, out, sizeof out, &size), 0);
}

/* gdb's plan in proj for @program, when the file t.e holds @payload as its
   .debug_gdb_scripts. */
static WpeStartPlan *embedded_plan(const char *payload, size_t plen)
{
 elf_image e;
 char path[PATH_ROOM];

 elf_make(&e, 1, 0, payload, plen);
 file_in(path, sizeof path, proj, "t.e");
 write_bytes(path, (const char *)e.b, e.len);
 return gdb_plan_of("t.e");
}

static void case_embedded_listed(void)
{
 /* gdb's layout, measured on gdb 18.1: a text entry is one NUL-ended
    string whose first line is the script's name. */
 static const char text_entry[] = "\004name\nprint(1)\n\0";
 static const char text_then_file[] = "\004name\nprint(1)\n\0\001emb.py\0";
 static const char scheme_text[] = "\006name\n(display 1)\n\0";
 char py[PATH_ROOM], exe[PATH_ROOM];
 WpeStartPlan *p;

 file_in(exe, sizeof exe, proj, "t.e");
 file_in(py, sizeof py, proj, "emb.py");
 write_file(py, "open('ran','w').close()\n");
 p = embedded_plan(ELF_PAYLOAD, ELF_PAYLOAD_LEN);
 expect_int("a program's gdb script section is listed", p->npath, 1);
 expect_str("as the program with its section", p->npath ? p->path[0].path : "",
            "./t.e (.debug_gdb_scripts)");
 expect_int("as a file gdb runs", p->npath ? p->path[0].role : -1, WPE_PATH_READS);
 expect_int("marked as inside the program", p->npath ? p->path[0].embedded : -1, 1);
 expect_str("its text is the name and the file's lines", p->npath ? p->path[0].content : "",
            "emb.py\nopen('ran','w').close()\n");
 expect_int("with no condition: it runs only when Run it loads it",
            p->npath && p->path[0].note == NULL, 1);
 expect_int("shown whole", wpe_startplan_reads_too_long(p), 0);
 expect_int("so the plan asks", wpe_startplan_judge(p), WPE_PLAN_ASK);
 expect_has("and its text is part of the plan's text", wpe_startplan_text(p),
            "emb.py\nopen('ran','w').close()");
 wpe_startplan_free(p);

 write_file(py, "open('ran2','w').close()\n");
 p = embedded_plan(ELF_PAYLOAD, ELF_PAYLOAD_LEN);
 expect_str("a changed script is a changed text", p->npath ? p->path[0].content : "",
            "emb.py\nopen('ran2','w').close()\n");
 wpe_startplan_free(p);

 p = embedded_plan(text_then_file, sizeof text_then_file - 1);
 expect_str("a file entry after a text entry shows the file's lines too",
            p->npath ? p->path[0].content : "",
            "name\nprint(1)\nemb.py\nopen('ran2','w').close()\n");
 expect_int("and both are shown whole", wpe_startplan_reads_too_long(p), 0);
 wpe_startplan_free(p);
 unlink(py);

 p = embedded_plan(ELF_PAYLOAD, ELF_PAYLOAD_LEN);
 expect_str("a file entry whose file is not there says so",
            p->npath ? p->path[0].content : "", "emb.py\n(not found)\n");
 expect_int("and that is all there is to show", wpe_startplan_reads_too_long(p), 0);
 wpe_startplan_free(p);

 p = embedded_plan(text_entry, sizeof text_entry - 1);
 expect_str("a text entry shows its name and its text", p->npath ? p->path[0].content : "",
            "name\nprint(1)\n");
 expect_int("a text entry is shown whole", wpe_startplan_reads_too_long(p), 0);
 wpe_startplan_free(p);

 p = embedded_plan(scheme_text, sizeof scheme_text - 1);
 expect_str("a Guile text entry (kind 6) is shown", p->npath ? p->path[0].content : "",
            "name\n(display 1)\n");
 wpe_startplan_free(p);

 p = embedded_plan("\001emb.py", 7);
 expect_int("an entry whose name has no end is listed", p->npath, 1);
 expect_int("but not shown whole: Run it is withheld", wpe_startplan_reads_too_long(p), 1);
 wpe_startplan_free(p);

 p = embedded_plan("\011x\0", 3);
 expect_int("an entry of a kind gdb does not know is listed", p->npath, 1);
 expect_int("and not shown whole", wpe_startplan_reads_too_long(p), 1);
 wpe_startplan_free(p);

 p = embedded_plan("\004name\nprint(1)", 14);
 expect_int("a text entry with no end is not shown whole", wpe_startplan_reads_too_long(p), 1);
 wpe_startplan_free(p);

 p = embedded_plan("\005name\0", 6);
 expect_int("kind 5, which gdb calls invalid, is not shown whole",
            wpe_startplan_reads_too_long(p), 1);
 wpe_startplan_free(p);

 unlink(exe);
 p = gdb_plan_of("t.e");
 expect_int("a program that is not there has no section listed", p->npath, 0);
 wpe_startplan_free(p);
 p = debugger_plan(proj, "gdb");
 expect_int("with no program known none is listed", p->npath, 0);
 wpe_startplan_free(p);
}

static void case_embedded_absent(void)
{
 char exe[PATH_ROOM];
 WpeStartPlan *p;

 file_in(exe, sizeof exe, proj, "t.e");
 write_file(exe, "#!/bin/sh\n");
 p = gdb_plan_of("t.e");
 expect_int("a program that is not ELF lists nothing", p->npath, 0);
 expect_int("and a plan of the user's own words runs", wpe_startplan_judge(p), WPE_PLAN_CLEAN);
 wpe_startplan_free(p);
 unlink(exe);
}

static void case_operand_in(void)
{
 char out[PATH_ROOM];

 expect_int("a bare name", wpe_startplan_operand_in(out, sizeof out, ".", "tool"), 0);
 expect_str("is a file of the folder", out, "./tool");
 wpe_startplan_operand_in(out, sizeof out, ".", "sub/tool");
 expect_str("a relative name keeps its folder part", out, "./sub/tool");
 wpe_startplan_operand_in(out, sizeof out, ".", "-x");
 expect_str("a name like an option is still a file", out, "./-x");
 expect_int("a whole path", wpe_startplan_operand_in(out, sizeof out, ".", "/usr/bin/id"), 0);
 expect_str("is kept as typed", out, "/usr/bin/id");
 wpe_startplan_operand_in(out, sizeof out, ".", "/");
 expect_str("the root too", out, "/");
 expect_int("a path that does not fit is refused, never cut",
            wpe_startplan_operand_in(out, TINY_ROOM, ".", "/usr/bin/id"), -1);
 expect_int("a bare name that does not fit too",
            wpe_startplan_operand_in(out, TINY_ROOM, ".", "longname.o"), -1);
}

static void case_input(void)
{
 WpeStartPlan *p;

 expect_int("a plain value may be sent", wpe_startplan_line_ok("x ; touch ran"), 1);
 expect_int("a line break may not", wpe_startplan_line_ok("x\nshell touch ran"), 0);
 expect_int("nor a carriage return", wpe_startplan_line_ok("x\rshell"), 0);
 expect_int("nor a delete", wpe_startplan_line_ok("x\177"), 0);

 p = debugger_plan(proj, "gdb");
 wpe_startplan_input(p, "r x\nshell touch ran", WPE_FROM_FOLDER, "./xwperc: Arguments");
 expect_int("a run command with a line break is refused", wpe_startplan_judge(p),
            WPE_PLAN_REFUSED);
 expect_str("and says why", p->refused, WPE_STARTPLAN_REFUSE_INPUT);
 wpe_startplan_free(p);

 p = debugger_plan(proj, "gdb");
 wpe_startplan_input(p, "r x ; touch ran", WPE_FROM_FOLDER, "./xwperc: Arguments");
 wpe_startplan_set_file(p, "./xwperc", proj);
 expect_int("a folder's Arguments in the run command ask", wpe_startplan_judge(p),
            WPE_PLAN_ASK);
 expect_has("and are part of the plan", wpe_startplan_text(p), "then r x ; touch ran");
 wpe_startplan_free(p);

 p = debugger_plan(proj, "gdb");
 wpe_startplan_input(p, "r -v", WPE_FROM_USER, NULL);
 expect_int("the user's own Arguments do not", wpe_startplan_judge(p), WPE_PLAN_CLEAN);
 wpe_startplan_free(p);
}

static void case_carrier(void)
{
 char **argv;
 WpeStartPlan *p = debugger_plan(proj, "gdb");

 wpe_startplan_word(p, "prog", WPE_WORD_OPERAND, WPE_FROM_USER, NULL);
 wpe_startplan_judge(p);
 wpe_startplan_word(p, "/tmp/we_sys", WPE_WORD_CARRIER, WPE_FROM_EDITOR, NULL);
 argv = wpe_startplan_argv(p);
 expect_str("the command line leaves the carrier out", argv ? argv[0] : NULL, "gdb");
 expect_str("and keeps the words in order", argv ? argv[1] : NULL, "prog");
 expect_int("and ends there", argv && argv[1] ? argv[2] == NULL : 0, 1);
 free(argv);
 wpe_startplan_judge(p);
 expect_int("the carrier is not part of the plan's text",
            strstr(wpe_startplan_text(p), "we_sys") != NULL, 0);
 wpe_startplan_free(p);

 p = debugger_plan(proj, "gdb");
 wpe_startplan_word(p, "./run-me", WPE_WORD_CARRIER, WPE_FROM_FOLDER, NULL);
 expect_int("a folder's value never carries a command", wpe_startplan_judge(p),
            WPE_PLAN_REFUSED);
 wpe_startplan_free(p);
}

/* A start with no plan is refused, whatever it would run; only the
   assistant's children, which say so, start without one. */
static void case_unplanned_refused(void)
{
 char mark[PATH_ROOM], script[PATH_ROOM];
 char *argv[] = { "/bin/sh", "-c", NULL, NULL };
 char *ok[] = { "true", NULL };
 WpeChildSpec spec;
 WpeChild c;
 int rc;

 snprintf(mark, sizeof mark, "%s/unplanned", proj);
 snprintf(script, sizeof script, "touch '%s'", mark);
 argv[2] = script;
 memset(&spec, 0, sizeof spec);
 spec.argv = argv;
 spec.io = WPE_CHILD_IO_PIPES;
 spec.stdin_null = 1;
 spec.what = "a test command";
 spec.origin = WPE_CHILD_ORIGIN_USER;
 errno = 0;
 rc = wpe_child_start(&spec, &c, NULL, NULL);
 expect_int("a user start with no plan is refused", rc, -1);
 expect_int("with EPERM", errno, EPERM);
 if (rc == 0)
  wpe_child_forget(&c);
 spec.origin = WPE_CHILD_ORIGIN_UNSET;
 expect_int("so is a start that says no origin", wpe_child_start(&spec, &c, NULL, NULL), -1);
 expect_int("and nothing ran", access(mark, F_OK), -1);

 spec.argv = ok;
 spec.origin = WPE_CHILD_ORIGIN_AI;
 rc = wpe_child_start(&spec, &c, NULL, NULL);
 expect_int("the assistant's child starts, held by its own control", rc, 0);
 if (rc == 0)
 {
  wpe_child_end(&c, SIGKILL);
  wpe_child_forget(&c);
 }
}

/* A shell line's file names are its "$1" ...: never part of its text. */
static void case_shell_positional(void)
{
 char name[PATH_ROOM], mark[PATH_ROOM], out[TEXT_ROOM], **argv;
 int status = 0;
 WpeStartPlan *p = wpe_startplan_new(WPE_START_RUN, "C");

 snprintf(mark, sizeof mark, "%s/pwned", proj);
 snprintf(name, sizeof name, "./a';touch %s;'.c", mark);
 wpe_startplan_set_file(p, "./xwperc", proj);
 wpe_startplan_set_cwd(p, proj);
 wpe_startplan_shell(p, "printf '%s|' \"$@\"", WPE_FROM_USER, NULL);
 wpe_startplan_word(p, name, WPE_WORD_OPERAND, WPE_FROM_USER, NULL);
 expect_int("a shell line of the editor's words on the user's file runs", wpe_startplan_judge(p),
            WPE_PLAN_CLEAN);
 argv = wpe_startplan_argv(p);
 expect_str("the shell", argv ? argv[0] : NULL, WPE_STARTPLAN_SHELL);
 expect_str("its line holds no file name", argv ? argv[2] : NULL, "printf '%s|' \"$@\"");
 expect_str("$0 is the editor's", argv ? argv[3] : NULL, WPE_STARTPLAN_SHELL_NAME);
 expect_str("the file is $1, as it is", argv ? argv[4] : NULL, name);
 free(argv);
 expect_int("the file name is not part of the plan's text",
            strstr(wpe_startplan_text(p), "touch") != NULL, 0);
 wpe_startplan_capture(p, out, sizeof out, SHORT_DEADLINE_MS, &status, "printf");
 expect_has("the command gets the name as one word", out, "touch");
 expect_int("and nothing in the name ran", access(mark, F_OK), -1);
 wpe_startplan_word(p, ";touch\tx", WPE_WORD_TEXT, WPE_FROM_FOLDER, "./xwperc: Arguments");
 expect_int("a folder's Arguments as words ask", wpe_startplan_judge(p), WPE_PLAN_ASK);
 expect_has("and are in the text", wpe_startplan_text(p), "argv ;touch\tx");
 wpe_startplan_free(p);
}

/* A server that runs the folder's build code asks once per folder. */
static void case_runs_code(void)
{
 WpeStartPlan *p = wpe_startplan_new(WPE_START_LANGUAGE_SERVER, "rust-analyzer");

 wpe_startplan_set_file(p, "this folder", proj);
 wpe_startplan_set_cwd(p, proj);
 wpe_startplan_word(p, "rust-analyzer", WPE_WORD_PROGRAM, WPE_FROM_EDITOR, NULL);
 expect_int("a server that only reads files starts without a question",
            wpe_startplan_judge(p), WPE_PLAN_CLEAN);
 wpe_startplan_runs_code(p, "it runs build scripts");
 expect_int("one that runs the folder's build code asks", wpe_startplan_judge(p), WPE_PLAN_ASK);
 expect_has("and says so in its text", wpe_startplan_text(p), "runs it runs build scripts");
 expect_int("the answer is kept", wpe_startplan_remember(p, WPE_PLAN_RUN), 0);
 expect_int("and the next start runs", wpe_startplan_judge(p), WPE_PLAN_RUN);
 wpe_startplan_free(p);
}

/* A program inside the folder that a folder's value chose asks: Run's
   program is an operand lying in the folder, which is clean on its own, but
   not when the folder's CompilerSwitch, Run or Executable decided which
   file it is. */
static void case_folder_chose_program(void)
{
 WpeStartPlan *p = project_plan();
 WpeStartPlan *other = project_plan();

 wpe_startplan_word(p, "./t", WPE_WORD_OPERAND, WPE_FROM_FOLDER, "./xwperc: Exedir, Executable");
 wpe_startplan_path(p, "./t", WPE_PATH_INPUT, WPE_FROM_FOLDER, "./xwperc: Exedir, Executable", NULL);
 expect_int("a folder's program inside its folder asks nothing", wpe_startplan_judge(p),
            WPE_PLAN_CLEAN);
 expect_int("a plan that says nothing chose holds no key", p->chose == NULL, 1);
 expect_int("the folder's choice is taken", wpe_startplan_folder_chose(p, "CompilerSwitch = 1"), 0);
 expect_int("a program the folder's value chose asks", wpe_startplan_judge(p), WPE_PLAN_ASK);
 expect_has("and the text names the key", wpe_startplan_text(p), "chose CompilerSwitch = 1");
 expect_int("a second key is added to the first",
            wpe_startplan_folder_chose(p, "Executable = evil"), 0);
 wpe_startplan_judge(p);
 expect_has("both are in the text", wpe_startplan_text(p),
            "chose CompilerSwitch = 1, Executable = evil");
 expect_int("the answer is kept", wpe_startplan_remember(p, WPE_PLAN_RUN), 0);
 expect_int("and the same choice runs next time", wpe_startplan_judge(p), WPE_PLAN_RUN);
 wpe_startplan_word(other, "./t", WPE_WORD_OPERAND, WPE_FROM_FOLDER, "./xwperc: Exedir, Executable");
 wpe_startplan_path(other, "./t", WPE_PATH_INPUT, WPE_FROM_FOLDER, "./xwperc: Exedir, Executable", NULL);
 wpe_startplan_folder_chose(other, "Run = program");
 expect_int("another key asks again", wpe_startplan_judge(other), WPE_PLAN_ASK);
 expect_int("no word is no choice", wpe_startplan_folder_chose(other, NULL), -1);
 wpe_startplan_free(other);
 wpe_startplan_free(p);
}

/* The editor's terminal carries a command; a folder's value never does. */
static void case_carry(void)
{
 char *carrier[] = { "xterm", "-e", NULL };
 WpeStartPlan *p = wpe_startplan_new(WPE_START_SHELL, "shell");

 wpe_startplan_set_cwd(p, proj);
 wpe_startplan_word(p, "/bin/true", WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
 expect_int("a planned shell nobody judged has nothing to carry",
            wpe_startplan_carry(p, carrier, WPE_FROM_EDITOR), -1);
 wpe_startplan_judge(p);
 expect_int("a folder's value never carries", wpe_startplan_carry(p, carrier, WPE_FROM_FOLDER), -1);
 expect_int("the editor's terminal does", wpe_startplan_carry(p, carrier, WPE_FROM_EDITOR), 0);
 expect_int("and is not part of the text", strstr(wpe_startplan_text(p), "xterm") != NULL, 0);
 wpe_startplan_free(p);
}

/* ---- A debugger that starts apart from the program's folder ------------ */

#define GDB_PIN "set auto-load safe-path $debugdir:$datadir/auto-load"
#define MI_PIN  "-gdb-set auto-load safe-path $debugdir:$datadir/auto-load"
#define LLDB_PIN "settings set target.load-script-from-symbol-file false"

/* How many entries a folder holds, not counting . and .. */
static int dir_entries(const char *dir)
{
 DIR *d = opendir(dir);
 struct dirent *e;
 int n = 0;

 if (!d)
  return -1;
 while ((e = readdir(d)) != NULL)
  if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
   n++;
 closedir(d);
 return n;
}

/* A plan of a debugger or an adapter made for @folder through contain, the
   store's folder the same, and the program @program when there is one. */
static WpeStartPlan *contained_plan(int kind, const char *name, const char *key,
                                    const char *variant, const char *folder, const char *program)
{
 WpeStartPlan *p = wpe_startplan_new(kind, name);

 if (!p) { fail("plan", "out of memory"); exit(1); }
 wpe_startplan_set_file(p, "this folder", folder);
 wpe_startplan_contain(p, key, variant, folder, program);
 return p;
}

/* End the debug folder of a plan, as the session's end does, and free it. */
static void end_plan(WpeStartPlan *p)
{
 if (p->debug_dir)
  wpe_startplan_debug_dir_end(p->debug_dir);
 wpe_startplan_free(p);
}

/* Remove the line @at of a plan's input, as a site that forgot it would have. */
static void drop_input(WpeStartPlan *p, int at)
{
 free(p->input[at].text);
 free(p->input[at].source);
 memmove(&p->input[at], &p->input[at + 1], (size_t)(p->ninput - at - 1) * sizeof p->input[0]);
 p->ninput--;
}

static void case_debug_dir(void)
{
 struct stat st;
 char *a, *b, path[PATH_ROOM];

 a = wpe_startplan_debug_dir_new(dbgbase);
 b = wpe_startplan_debug_dir_new(dbgbase);
 expect_int("a debug folder is made, and a second", a != NULL && b != NULL, 1);
 if (!a || !b) { free(a); free(b); return; }
 expect_int("two starts get two folders", strcmp(a, b) != 0, 1);
 expect_int("under the base it was given", strncmp(a, dbgbase, strlen(dbgbase)), 0);
 expect_int("it is there", stat(a, &st), 0);
 expect_int("a folder", S_ISDIR(st.st_mode) != 0, 1);
 expect_int("only its owner may enter", (int)(st.st_mode & 0777), 0700);
 expect_int("empty at creation", dir_entries(a), 0);
 file_in(path, sizeof path, a, "x");
 write_file(path, "x");
 expect_int("a folder that holds something is not removed", wpe_startplan_debug_dir_end(a), -1);
 unlink(path);
 expect_int("the session's end removes it", wpe_startplan_debug_dir_end(a), 0);
 expect_int("and it is gone", access(a, F_OK), -1);
 expect_int("a folder the owner did not make is not removed",
            wpe_startplan_debug_dir_end(proj), -1);
 expect_int("and is still there", access(proj, F_OK), 0);
 expect_int("the other folder ends too", wpe_startplan_debug_dir_end(b), 0);
 free(a);
 free(b);
}

/* Sessions whose debugger wrote a file into its folder (a log the user's own init file
   keeps by a relative name) leave their folders behind: a later start still gets one. */
#define LEFT_FOLDERS 40
static void case_debug_dir_left_behind(void)
{
 char *left[LEFT_FOLDERS], *next, path[PATH_ROOM];
 int i, kept = 0;

 for (i = 0; i < LEFT_FOLDERS; i++)
 {
  left[i] = wpe_startplan_debug_dir_new(dbgbase);
  if (!left[i])
   continue;
  kept++;
  file_in(path, sizeof path, left[i], "gdb.txt");
  write_file(path, "log");
  wpe_startplan_debug_dir_end(left[i]);
 }
 expect_int("every session got a folder although each left its own behind", kept,
            LEFT_FOLDERS);
 next = wpe_startplan_debug_dir_new(dbgbase);
 expect_int("and the next start still gets one", next != NULL, 1);
 if (next)
  wpe_startplan_debug_dir_end(next);
 free(next);
 for (i = 0; i < LEFT_FOLDERS; i++)
 {
  if (!left[i])
   continue;
  file_in(path, sizeof path, left[i], "gdb.txt");
  unlink(path);
  expect_int("a folder left behind ends once it is empty", wpe_startplan_debug_dir_end(left[i]), 0);
  free(left[i]);
 }
}

/* What one encoder gives for one path. */
static void expect_enc(const char *name, int (*enc)(char *, size_t, const char *),
                       const char *path, int want_rc, const char *want)
{
 char out[PATH_ROOM], why[TEXT_ROOM];
 int rc;

 out[0] = '\0';
 rc = enc(out, sizeof out, path);
 snprintf(why, sizeof why, "%s on \"%s\"", name, path);
 expect_int(why, rc, want_rc);
 if (want_rc == 0)
  expect_str(why, out, want);
}

static int gdb_raw(char *out, size_t n, const char *path)
{
 return wpe_startplan_gdb_arg(out, n, path, WPE_GDB_ARG_RAW);
}

static int gdb_split(char *out, size_t n, const char *path)
{
 return wpe_startplan_gdb_arg(out, n, path, WPE_GDB_ARG_SPLIT);
}

static void case_encoders(void)
{
 char tiny[TINY_ROOM];

 /* cd and source take the rest of the line as it is (measured on gdb 18.1). */
 expect_enc("gdb raw", gdb_raw, "/a b/c", 0, "/a b/c");
 expect_enc("gdb raw", gdb_raw, "/a/b ", -1, NULL);
 expect_enc("gdb raw", gdb_raw, "/a:b", 0, "/a:b");
 expect_enc("gdb raw", gdb_raw, "/a'b", 0, "/a'b");
 expect_enc("gdb raw", gdb_raw, "/a\"b", 0, "/a\"b");
 expect_enc("gdb raw", gdb_raw, "/a\\b", 0, "/a\\b");
 expect_enc("gdb raw", gdb_raw, "/a\nb", -1, NULL);
 expect_enc("gdb raw", gdb_raw, "/a\xe9", 0, "/a\xe9");
 expect_enc("gdb raw", gdb_raw, "-x", -1, NULL);
 expect_enc("gdb raw", gdb_raw, "~/x", -1, NULL);
 expect_enc("gdb raw", gdb_raw, "", -1, NULL);
 /* file splits the line into words: a blank, a quote or a backslash is escaped. */
 expect_enc("gdb split", gdb_split, "/a b/c", 0, "/a\\ b/c");
 expect_enc("gdb split", gdb_split, "/a/b ", -1, NULL);
 expect_enc("gdb split", gdb_split, "/a:b", 0, "/a:b");
 expect_enc("gdb split", gdb_split, "/a'b", 0, "/a\\'b");
 expect_enc("gdb split", gdb_split, "/a\"b", 0, "/a\\\"b");
 expect_enc("gdb split", gdb_split, "/a\\b", 0, "/a\\\\b");
 expect_enc("gdb split", gdb_split, "/a\nb", -1, NULL);
 expect_enc("gdb split", gdb_split, "/a\xe9", 0, "/a\xe9");
 expect_enc("gdb split", gdb_split, "-x", -1, NULL);
 /* add-auto-load-safe-path: a ':' would admit a prefix, a '$' a variable. */
 expect_enc("safe path", wpe_startplan_gdb_safe_path_arg, "/a b/c", 0, "/a b/c");
 expect_enc("safe path", wpe_startplan_gdb_safe_path_arg, "/a/b ", -1, NULL);
 expect_enc("safe path", wpe_startplan_gdb_safe_path_arg, "/a:b", -1, NULL);
 expect_enc("safe path", wpe_startplan_gdb_safe_path_arg, "/a$b", -1, NULL);
 /* gdb matches an entry as a wildcard pattern: "/a*" would admit "/ab". */
 expect_enc("safe path", wpe_startplan_gdb_safe_path_arg, "/a*/p-gdb.py", -1, NULL);
 expect_enc("safe path", wpe_startplan_gdb_safe_path_arg, "/a?/p-gdb.py", -1, NULL);
 expect_enc("safe path", wpe_startplan_gdb_safe_path_arg, "/a[bc]/p-gdb.py", -1, NULL);
 expect_enc("safe path", wpe_startplan_gdb_safe_path_arg, "/a'b", 0, "/a'b");
 expect_enc("safe path", wpe_startplan_gdb_safe_path_arg, "/a\"b", 0, "/a\"b");
 expect_enc("safe path", wpe_startplan_gdb_safe_path_arg, "/a\\b", 0, "/a\\b");
 expect_enc("safe path", wpe_startplan_gdb_safe_path_arg, "/a\nb", -1, NULL);
 expect_enc("safe path", wpe_startplan_gdb_safe_path_arg, "/a\xe9", 0, "/a\xe9");
 expect_enc("safe path", wpe_startplan_gdb_safe_path_arg, "-x", -1, NULL);
 /* MI and lldb: a c-string with its escapes.  -environment-cd trims a blank
    at the end even inside the quotes, so MI refuses one. */
 expect_enc("mi", wpe_startplan_mi_cstring, "/a b/c", 0, "\"/a b/c\"");
 expect_enc("mi", wpe_startplan_mi_cstring, "/a/b ", -1, NULL);
 expect_enc("mi", wpe_startplan_mi_cstring, "/a:b", 0, "\"/a:b\"");
 expect_enc("mi", wpe_startplan_mi_cstring, "/a'b", 0, "\"/a'b\"");
 expect_enc("mi", wpe_startplan_mi_cstring, "/a\"b", 0, "\"/a\\\"b\"");
 expect_enc("mi", wpe_startplan_mi_cstring, "/a\\b", 0, "\"/a\\\\b\"");
 expect_enc("mi", wpe_startplan_mi_cstring, "/a\nb", -1, NULL);
 expect_enc("mi", wpe_startplan_mi_cstring, "/a\xe9", 0, "\"/a\xe9\"");
 expect_enc("mi", wpe_startplan_mi_cstring, "-x", -1, NULL);
 expect_enc("lldb", wpe_startplan_lldb_source_arg, "/a b/c", 0, "\"/a b/c\"");
 expect_enc("lldb", wpe_startplan_lldb_source_arg, "/a\"b", 0, "\"/a\\\"b\"");
 expect_enc("lldb", wpe_startplan_lldb_source_arg, "/a\\b", 0, "\"/a\\\\b\"");
 expect_enc("lldb", wpe_startplan_lldb_source_arg, "/a\nb", -1, NULL);
 expect_enc("lldb", wpe_startplan_lldb_source_arg, "-x", -1, NULL);
 /* jdb's read takes one word. */
 expect_enc("jdb", wpe_startplan_jdb_read_arg, "/a/b.ini", 0, "/a/b.ini");
 expect_enc("jdb", wpe_startplan_jdb_read_arg, "/a b/c", -1, NULL);
 expect_enc("jdb", wpe_startplan_jdb_read_arg, "/a/b ", -1, NULL);
 expect_enc("jdb", wpe_startplan_jdb_read_arg, "/a:b", 0, "/a:b");
 expect_enc("jdb", wpe_startplan_jdb_read_arg, "/a'b", 0, "/a'b");
 expect_enc("jdb", wpe_startplan_jdb_read_arg, "/a\nb", -1, NULL);
 expect_enc("jdb", wpe_startplan_jdb_read_arg, "-x", -1, NULL);
 /* pdb's move: a bytes literal in pure ASCII. */
 expect_enc("python", wpe_startplan_python_bytes, "/a b/c", 0, "b'/a b/c'");
 expect_enc("python", wpe_startplan_python_bytes, "/a/b ", 0, "b'/a/b '");
 expect_enc("python", wpe_startplan_python_bytes, "/a:b", 0, "b'/a:b'");
 expect_enc("python", wpe_startplan_python_bytes, "/a'b", 0, "b'/a\\'b'");
 expect_enc("python", wpe_startplan_python_bytes, "/a\"b", 0, "b'/a\"b'");
 expect_enc("python", wpe_startplan_python_bytes, "/a\\b", 0, "b'/a\\\\b'");
 expect_enc("python", wpe_startplan_python_bytes, "/a\nb", -1, NULL);
 expect_enc("python", wpe_startplan_python_bytes, "/a\xe9", 0, "b'/a\\xe9'");
 /* pdb cuts a line at ";;": the literal never holds one. */
 expect_enc("python", wpe_startplan_python_bytes, "/a;;b", 0, "b'/a\\x3b\\x3bb'");
 expect_enc("python", wpe_startplan_python_bytes, "-x", -1, NULL);
 /* A path that does not fit is refused, never cut. */
 expect_int("gdb raw does not cut", wpe_startplan_gdb_arg(tiny, sizeof tiny, "/long/path", 0), -1);
 expect_int("gdb split does not cut", wpe_startplan_gdb_arg(tiny, sizeof tiny, "/a b/path", 1), -1);
 expect_int("mi does not cut", wpe_startplan_mi_cstring(tiny, sizeof tiny, "/long/path"), -1);
 expect_int("jdb does not cut", wpe_startplan_jdb_read_arg(tiny, sizeof tiny, "/long/path"), -1);
 expect_int("python does not cut", wpe_startplan_python_bytes(tiny, sizeof tiny, "/long/path"), -1);
}

/* The lines contain adds for each row that starts apart, in order. */
static void case_contain_lines(void)
{
 char prog[PATH_ROOM], want[PATH_ROOM];
 WpeStartPlan *p;

 file_in(prog, sizeof prog, proj, "t.e");
 write_file(prog, "");
 wpe_startplan_rows_designed(1);

 p = contained_plan(WPE_START_DEBUGGER, "gdb", "gdb", NULL, proj, "t.e");
 expect_int("gdb: the pin, the two style lines, the move and the program", p->ninput, 5);
 expect_str("gdb: the pin comes first", p->ninput > 0 ? p->input[0].text : "", GDB_PIN);
 expect_str("gdb: then the error symbol off", p->ninput > 1 ? p->input[1].text : "",
            "set style error-prefix");
 expect_str("gdb: and the warning symbol off", p->ninput > 2 ? p->input[2].text : "",
            "set style warning-prefix");
 snprintf(want, sizeof want, "cd %s", proj);
 expect_str("gdb: then the program's folder", p->ninput > 3 ? p->input[3].text : "", want);
 snprintf(want, sizeof want, "file %s", prog);
 expect_str("gdb: the program loads last", p->ninput > 4 ? p->input[4].text : "", want);
 /* S4: what a refusal of each line means */
 expect_int("gdb: a refused pin stops the start", wpe_startplan_input_outcome(p, 0), WPE_REFUSAL_STOPS);
 expect_int("gdb: a refused style line is ignored", wpe_startplan_input_outcome(p, 1), WPE_REFUSAL_QUIET);
 expect_int("gdb: both of them", wpe_startplan_input_outcome(p, 2), WPE_REFUSAL_QUIET);
 expect_int("gdb: a refused move stops the start", wpe_startplan_input_outcome(p, 3), WPE_REFUSAL_STOPS);
 expect_int("gdb: a refused program stops the start", wpe_startplan_input_outcome(p, 4), WPE_REFUSAL_STOPS);
 expect_int("gdb: no word is added", p->nword, 0);
 expect_int("gdb: it does not start in the program's folder", p->cwd && strcmp(p->cwd, proj) != 0, 1);
 expect_str("gdb: the program's folder is kept", p->program_dir, proj);
 expect_int("gdb: its own folder is empty", dir_entries(p->cwd), 0);
 expect_int("gdb: Run it's lines go before the program", p->load_at, 4);
 expect_int("gdb: the program is a path of the plan", p->npath, 1);
 expect_str("gdb: absolute", p->npath ? p->path[0].path : "", prog);
 expect_int("gdb: and judged clean", wpe_startplan_judge(p), WPE_PLAN_CLEAN);
 snprintf(want, sizeof want, "cwd %s\n", proj);
 expect_has("gdb: the text names the program's folder", wpe_startplan_text(p), want);
 expect_int("gdb: and never the debug folder", strstr(wpe_startplan_text(p), p->cwd) == NULL, 1);
 end_plan(p);

 p = contained_plan(WPE_START_DEBUGGER, "gdb", "gdb", "mi", proj, "t.e");
 expect_int("mi: the pin, the move and the program (no style lines)", p->ninput, 3);
 expect_str("mi: the pin", p->ninput > 0 ? p->input[0].text : "", MI_PIN);
 snprintf(want, sizeof want, "-environment-cd \"%s\"", proj);
 expect_str("mi: the move", p->ninput > 1 ? p->input[1].text : "", want);
 snprintf(want, sizeof want, "-file-exec-and-symbols \"%s\"", prog);
 expect_str("mi: the program", p->ninput > 2 ? p->input[2].text : "", want);
 end_plan(p);

 p = wpe_startplan_new(WPE_START_DEBUG_ADAPTER, "gdb");
 wpe_startplan_set_file(p, "this folder", proj);
 wpe_startplan_word(p, "gdb", WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
 wpe_startplan_word(p, "--interpreter=dap", WPE_WORD_OPTION, WPE_FROM_EDITOR, NULL);
 wpe_startplan_contain(p, "dap", "gdb", proj, NULL);
 expect_int("gdb-dap: two switches", p->nword, 6);
 expect_str("gdb-dap: -iex", p->nword > 2 ? p->word[2].text : "", "-iex");
 expect_str("gdb-dap: the pin", p->nword > 3 ? p->word[3].text : "", GDB_PIN);
 expect_str("gdb-dap: -iex again", p->nword > 4 ? p->word[4].text : "", "-iex");
 snprintf(want, sizeof want, "cd %s", proj);
 expect_str("gdb-dap: the move", p->nword > 5 ? p->word[5].text : "", want);
 expect_str("gdb-dap: the launch names the folder", p->launch_cwd, proj);
 expect_int("gdb-dap: nothing is typed", p->ninput, 0);
 expect_int("gdb-dap: its own folder", p->cwd && strcmp(p->cwd, proj) != 0, 1);
 end_plan(p);

 p = wpe_startplan_new(WPE_START_DEBUG_ADAPTER, "lldb");
 wpe_startplan_set_file(p, "this folder", proj);
 wpe_startplan_word(p, "lldb-dap", WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
 wpe_startplan_contain(p, "dap", "lldb-dap", proj, NULL);
 expect_int("lldb-dap: one init command", p->ninit, 1);
 expect_str("lldb-dap: the pin", p->ninit ? p->init[0].text : "", LLDB_PIN);
 expect_str("lldb-dap: the program runs in the folder", p->launch_cwd, proj);
 expect_int("lldb-dap: the debugger itself stays in its own folder",
            p->cwd && strcmp(p->cwd, proj) != 0, 1);
 expect_int("lldb-dap: no word", p->nword, 1);
 expect_int("lldb-dap: judged clean", wpe_startplan_judge(p), WPE_PLAN_CLEAN);
 end_plan(p);

 p = contained_plan(WPE_START_DEBUGGER, "pdb", "pdb", NULL, proj, NULL);
 expect_int("pdb: one line", p->ninput, 1);
 snprintf(want, sizeof want, "!__import__('os').chdir(b'%s')", proj);
 expect_str("pdb: the move is its first input", p->ninput ? p->input[0].text : "", want);
 end_plan(p);

 p = contained_plan(WPE_START_DEBUGGER, "pdb", "pdb", NULL, proj, "t.e");
 expect_int("pdb: the move is its only line", p->ninput, 1);
 expect_int("pdb: and the script a path of the plan", p->npath, 1);
 expect_str("pdb: absolute", p->npath ? p->path[0].path : "", prog);
 expect_int("pdb: Run it's lines go after the move", p->load_at, 1);
 expect_int("pdb: it does not start in the program's folder", p->cwd && strcmp(p->cwd, proj) != 0, 1);
 expect_int("pdb: judged clean", wpe_startplan_judge(p), WPE_PLAN_CLEAN);
 end_plan(p);

 p = contained_plan(WPE_START_DEBUGGER, "jdb", "jdb", NULL, proj, NULL);
 expect_int("jdb: nothing to pin or move", p->ninput, 0);
 expect_int("jdb: its own folder", p->cwd && strcmp(p->cwd, proj) != 0, 1);
 expect_int("jdb: judged clean", wpe_startplan_judge(p), WPE_PLAN_CLEAN);
 end_plan(p);

 wpe_startplan_rows_designed(0);
 p = contained_plan(WPE_START_DEBUGGER, "gdb", "gdb", NULL, proj, "t.e");
 expect_int("shipped: gdb starts apart: the pin, the style lines, the move, the program", p->ninput, 5);
 expect_int("shipped: gdb has a folder of its own", p->debug_dir != NULL, 1);
 end_plan(p);
 p = contained_plan(WPE_START_DEBUGGER, "pdb", "pdb", NULL, proj, "t.e");
 expect_int("shipped: pdb gets its move and its script", p->ninput + p->nword + p->ninit + p->npath, 2);
 expect_int("shipped: it does not start in the program's folder", p->cwd && strcmp(p->cwd, proj) != 0, 1);
 expect_str("shipped: whose folder it is", p->program_dir, proj);
 expect_int("shipped: a folder of its own", p->debug_dir != NULL, 1);
 expect_int("shipped: its own folder is empty", dir_entries(p->cwd), 0);
 end_plan(p);
 p = contained_plan(WPE_START_DEBUGGER, "jdb", "jdb", NULL, proj, NULL);
 expect_int("shipped: jdb has nothing to pin or move", p->ninput + p->nword + p->ninit + p->npath, 0);
 expect_int("shipped: it does not start in the program's folder", p->cwd && strcmp(p->cwd, proj) != 0, 1);
 expect_int("shipped: a folder of its own", p->debug_dir != NULL, 1);
 expect_int("shipped: its own folder is empty", dir_entries(p->cwd), 0);
 end_plan(p);
 p = contained_plan(WPE_START_DEBUGGER, "dbx", "dbx", NULL, proj, "t.e");
 expect_int("shipped: dbx gets no line", p->ninput + p->nword + p->ninit + p->npath, 0);
 expect_str("shipped: it starts in the program's folder", p->cwd, proj);
 expect_str("shipped: whose folder it is", p->program_dir, proj);
 expect_int("shipped: no folder of its own", p->debug_dir == NULL, 1);
 end_plan(p);
 unlink(prog);
}

/* The table: what starts apart, what does not and why. */
static void case_rows(void)
{
 static const char *const exceptions[] = { "dbx", "xdb", "sdb", "a68g", "dlv", "scala-cli", NULL };
 const char *key, *variant, *reason, *name;
 int i, k, apart, designed, listed, kept = 0, not_yet = 0;

 for (i = 0; (key = wpe_startplan_row_info(i, &variant, &apart, &designed, &reason)) != NULL; i++)
 {
  name = strcmp(key, "dap") == 0 ? variant : key;
  expect_int("only gdb's two rows, pdb's and jdb's ship with the debugger starting apart", apart,
             strcmp(key, "gdb") == 0 || strcmp(key, "pdb") == 0 || strcmp(key, "jdb") == 0);
  if (!apart)
   expect_int("a row that does not start apart says why", reason != NULL && *reason != '\0', 1);
  listed = 0;
  for (k = 0; exceptions[k]; k++)
   if (strcmp(exceptions[k], name) == 0)
    listed = 1;
  if (designed)
   not_yet++;
  else
   kept++;
  expect_int("a row that never starts apart is in the exception list", designed == 0, listed);
 }
 expect_int("six debuggers are to start apart", not_yet, 6);
 expect_int("six stay as they are, by name", kept, 6);
 expect_int("the table ends", wpe_startplan_row_info(i, NULL, NULL, NULL, NULL) == NULL, 1);
}

/* Rule 11: a debugger that starts apart does. */
static void case_rule_apart(void)
{
 char prog[PATH_ROOM], *run_dir;
 WpeStartPlan *p;
 WpeStartIo io;
 WpeChild c;
 const char *key, *variant;
 char *argv[] = { "/bin/true", NULL };
 int i;

 file_in(prog, sizeof prog, proj, "t.e");
 write_file(prog, "");
 wpe_startplan_rows_designed(1);

 /* A debugger plan that never went through contain, started in the folder. */
 p = wpe_startplan_new(WPE_START_DEBUGGER, "gdb");
 wpe_startplan_set_cwd(p, proj);
 wpe_startplan_word(p, "gdb", WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
 expect_int("a gdb plan that starts in the program's folder is refused",
            wpe_startplan_judge(p), WPE_PLAN_REFUSED);
 expect_str("and says why", p->refused, WPE_STARTPLAN_REFUSE_DEBUG_CWD);
 wpe_startplan_free(p);

 /* In a folder the owner made, with no pin and no move. */
 p = wpe_startplan_new(WPE_START_DEBUGGER, "gdb");
 run_dir = wpe_startplan_debug_dir_new(dbgbase);
 wpe_startplan_set_cwd(p, run_dir);
 wpe_startplan_word(p, "gdb", WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
 expect_int("gdb in its own folder without its pin is refused", wpe_startplan_judge(p), WPE_PLAN_REFUSED);
 expect_str("and says why", p->refused, WPE_STARTPLAN_REFUSE_DEBUG_PIN);
 wpe_startplan_free(p);
 wpe_startplan_debug_dir_end(run_dir);
 free(run_dir);

 /* Nothing was listed, and it still holds. */
 p = contained_plan(WPE_START_DEBUGGER, "gdb", "gdb", NULL, proj, "t.e");
 wpe_startplan_word(p, "gdb", WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
 expect_int("a contained gdb plan with nothing listed is clean", wpe_startplan_judge(p), WPE_PLAN_CLEAN);
 drop_input(p, 0);
 expect_int("without its pin line it is refused", wpe_startplan_judge(p), WPE_PLAN_REFUSED);
 expect_str("saying so", p->refused, WPE_STARTPLAN_REFUSE_DEBUG_PIN);
 end_plan(p);
 p = contained_plan(WPE_START_DEBUGGER, "gdb", "gdb", NULL, proj, "t.e");
 drop_input(p, 1);
 expect_int("without its move line it is refused", wpe_startplan_judge(p), WPE_PLAN_REFUSED);
 end_plan(p);

 /* The belt: a plan judged apart, then put back in the folder, does not start. */
 p = contained_plan(WPE_START_DEBUGGER, "gdb", "gdb", NULL, proj, "t.e");
 wpe_startplan_word(p, "/bin/true", WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
 expect_int("a contained plan is judged clean", wpe_startplan_judge(p), WPE_PLAN_CLEAN);
 free(p->cwd);
 p->cwd = strdup(proj);
 memset(&io, 0, sizeof io);
 traced[0] = '\0';
 expect_int("started in the folder it was not made for, it does not start",
            wpe_startplan_start(p, argv, &io, &c, NULL, NULL), -1);
 expect_has("and the trace says why", traced, WPE_STARTPLAN_REFUSE_DEBUG_CWD);
 end_plan(p);

 /* A debugger the table does not know. */
 p = wpe_startplan_new(WPE_START_DEBUGGER, "nosuch");
 expect_int("contain refuses a debugger it does not know",
            wpe_startplan_contain(p, "nosuch", NULL, proj, NULL), -1);
 expect_int("the plan is refused", wpe_startplan_judge(p), WPE_PLAN_REFUSED);
 expect_str("saying so", p->refused, WPE_STARTPLAN_REFUSE_DEBUG_UNKNOWN);
 wpe_startplan_free(p);

 /* The rows that do not start apart take the folder, with the rule on. */
 for (i = 0; (key = wpe_startplan_row_info(i, &variant, NULL, NULL, NULL)) != NULL; i++)
 {
  int designed;
  const char *name = strcmp(key, "dap") == 0 ? variant : key;

  wpe_startplan_row_info(i, NULL, NULL, &designed, NULL);
  if (designed)
   continue;
  p = wpe_startplan_new(strcmp(key, "dap") == 0 ? WPE_START_DEBUG_ADAPTER : WPE_START_DEBUGGER,
                        name);
  wpe_startplan_word(p, name, WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
  expect_int(name, wpe_startplan_contain(p, key, variant, proj, "t.e"), 0);
  expect_str("a row that does not start apart starts in the folder", p->cwd, proj);
  expect_int("and holds no containment line", p->ninput + p->nword + p->ninit + p->npath, 1);
  expect_int("and is not refused for it", wpe_startplan_judge(p), WPE_PLAN_CLEAN);
  wpe_startplan_free(p);
 }

 wpe_startplan_rows_designed(0);
 p = wpe_startplan_new(WPE_START_DEBUGGER, "gdb");
 wpe_startplan_set_cwd(p, proj);
 wpe_startplan_word(p, "gdb", WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
 expect_int("with the shipped rows gdb in the folder is refused", wpe_startplan_judge(p),
            WPE_PLAN_REFUSED);
 expect_str("saying so", p->refused, WPE_STARTPLAN_REFUSE_DEBUG_CWD);
 wpe_startplan_free(p);
 p = wpe_startplan_new(WPE_START_DEBUGGER, "pdb");
 wpe_startplan_set_cwd(p, proj);
 wpe_startplan_word(p, "pdb", WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
 expect_int("with the shipped rows pdb in the folder is refused", wpe_startplan_judge(p),
            WPE_PLAN_REFUSED);
 expect_str("saying so", p->refused, WPE_STARTPLAN_REFUSE_DEBUG_CWD);
 wpe_startplan_free(p);
 p = wpe_startplan_new(WPE_START_DEBUGGER, "jdb");
 wpe_startplan_set_cwd(p, proj);
 wpe_startplan_word(p, "jdb", WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
 expect_int("with the shipped rows jdb in the folder is refused", wpe_startplan_judge(p),
            WPE_PLAN_REFUSED);
 expect_str("saying so", p->refused, WPE_STARTPLAN_REFUSE_DEBUG_CWD);
 wpe_startplan_free(p);
 p = contained_plan(WPE_START_DEBUGGER, "pdb", "pdb", NULL, proj, "t.e");
 drop_input(p, 0);
 expect_int("pdb without its move line is refused", wpe_startplan_judge(p), WPE_PLAN_REFUSED);
 expect_str("saying so", p->refused, WPE_STARTPLAN_REFUSE_DEBUG_PIN);
 end_plan(p);
 p = wpe_startplan_new(WPE_START_DEBUGGER, "a68g");
 wpe_startplan_set_cwd(p, proj);
 wpe_startplan_word(p, "a68g", WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
 expect_int("with the shipped rows a68g starts in the folder as it did", wpe_startplan_judge(p),
            WPE_PLAN_CLEAN);
 wpe_startplan_free(p);
 unlink(prog);
}

/* Listing and the canonical text are the program's folder's, not the debug folder's;
   an answer given before this change is still the answer. */
static void case_contain_text(void)
{
 char rc[PATH_ROOM], dbxrc[PATH_ROOM], *before, *second;
 WpeStartPlan *p, *q;

 file_in(rc, sizeof rc, proj, ".pdbrc");
 write_file(rc, "p 1\n");

 /* A debugger that does not start apart (dbx): started the way every start
    was made before: set_cwd, a word, the listing. */
 file_in(dbxrc, sizeof dbxrc, proj, ".dbxinit");
 write_file(dbxrc, "print 1\n");
 p = wpe_startplan_new(WPE_START_DEBUGGER, "dbx");
 wpe_startplan_set_file(p, "this folder", proj);
 wpe_startplan_set_cwd(p, proj);
 wpe_startplan_word(p, "dbx", WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
 wpe_startplan_tool_reads(p, "dbx");
 expect_int("the folder's .dbxinit asks", wpe_startplan_judge(p), WPE_PLAN_ASK);
 before = strdup(wpe_startplan_text(p));
 expect_int("the user said yes", wpe_startplan_remember(p, WPE_PLAN_RUN), 0);
 wpe_startplan_free(p);

 /* The same start through contain with the shipped rows. */
 p = contained_plan(WPE_START_DEBUGGER, "dbx", "dbx", NULL, proj, NULL);
 wpe_startplan_word(p, "dbx", WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
 wpe_startplan_tool_reads(p, "dbx");
 expect_int("the answer given before is still the answer", wpe_startplan_judge(p), WPE_PLAN_RUN);
 expect_str("because the text did not change", wpe_startplan_text(p), before);
 wpe_startplan_free(p);
 unlink(dbxrc);

 /* Apart: the folder's file is listed from the program's folder, inside it. */
 wpe_startplan_rows_designed(1);
 p = contained_plan(WPE_START_DEBUGGER, "pdb", "pdb", NULL, proj, NULL);
 wpe_startplan_word(p, "pdb", WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
 expect_int("the folder's .pdbrc is listed", wpe_startplan_tool_reads(p, "pdb"), 1);
 expect_int("although the debugger starts elsewhere", p->cwd && strcmp(p->cwd, proj) != 0, 1);
 expect_int("it asks as before", wpe_startplan_judge(p), WPE_PLAN_ASK);
 expect_int("the listed file is the program's folder's", p->npath > 0 && strstr(p->path[0].resolved, proj) == p->path[0].resolved, 1);
 expect_int("and lies inside it", p->npath > 0 ? p->path[0].inside : -1, 1);
 expect_int("the box has the text of it", strstr(wpe_startplan_text(p), "p 1") != NULL, 1);
 expect_int("the plan is remembered", wpe_startplan_remember(p, WPE_PLAN_RUN), 0);
 second = strdup(wpe_startplan_text(p));
 end_plan(p);

 /* Another editor run: another debug folder, the same text, the same answer. */
 q = contained_plan(WPE_START_DEBUGGER, "pdb", "pdb", NULL, proj, NULL);
 wpe_startplan_word(q, "pdb", WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
 wpe_startplan_tool_reads(q, "pdb");
 expect_int("a second start gets a folder of its own", q->debug_dir != NULL, 1);
 expect_int("and the answer remembered covers it", wpe_startplan_judge(q), WPE_PLAN_RUN);
 expect_str("the same plan has the same text", wpe_startplan_text(q), second);
 expect_int("the text holds no debug folder", strstr(second, dbgbase) == NULL, 1);
 end_plan(q);
 wpe_startplan_rows_designed(0);
 free(before);
 free(second);
 unlink(rc);
}

/* What a debugger site relies on when gdb starts apart: the folder's life, the lines
   the editor sends, the history rule's two commands, the program's origin. */
static void case_gdb_apart_owner(void)
{
 char prog[PATH_ROOM], dir[PATH_ROOM], ask[TEXT_ROOM], fix[TEXT_ROOM], want[PATH_ROOM];
 char *taken;
 WpeStartPlan *p;

 file_in(prog, sizeof prog, proj, "t.e");
 write_file(prog, "");

 /* The plan's folder goes with the plan, unless the session took it over. */
 p = contained_plan(WPE_START_DEBUGGER, "gdb", "gdb", NULL, proj, "t.e");
 snprintf(dir, sizeof dir, "%s", p->debug_dir);
 expect_int("gdb starts apart", wpe_startplan_is_apart(p), 1);
 expect_int("by its row too", wpe_startplan_row_is_apart("gdb", ""), 1);
 expect_int("and over MI", wpe_startplan_row_is_apart("gdb", "mi"), 1);
 expect_int("pdb starts apart", wpe_startplan_row_is_apart("pdb", ""), 1);
 expect_int("jdb starts apart", wpe_startplan_row_is_apart("jdb", ""), 1);
 expect_int("a debugger that does not is not", wpe_startplan_row_is_apart("dbx", ""), 0);
 expect_int("nor one the table does not know", wpe_startplan_row_is_apart("nosuch", ""), 0);
 expect_int("its folder exists", access(dir, F_OK), 0);
 wpe_startplan_free(p);
 expect_int("freeing the plan ends its folder", access(dir, F_OK), -1);

 p = contained_plan(WPE_START_DEBUGGER, "gdb", "gdb", NULL, proj, "t.e");
 snprintf(dir, sizeof dir, "%s", p->debug_dir);
 taken = wpe_startplan_debug_dir_take(p);
 expect_str("the session takes the folder", taken, dir);
 expect_int("the plan no longer holds it", p->debug_dir == NULL, 1);
 wpe_startplan_free(p);
 expect_int("so freeing the plan leaves it", access(dir, F_OK), 0);
 expect_int("the session's end removes it", wpe_startplan_debug_dir_end(taken), 0);
 expect_int("and it is gone", access(dir, F_OK), -1);
 free(taken);

 /* Forty starts, each ended: the owner's table never fills. */
 {
  int i, ok = 0;

  for (i = 0; i < 40; i++)
  {
   p = contained_plan(WPE_START_DEBUGGER, "gdb", "gdb", NULL, proj, "t.e");
   taken = wpe_startplan_debug_dir_take(p);
   if (taken && wpe_startplan_debug_dir_end(taken) == 0)
    ok++;
   free(taken);
   wpe_startplan_free(p);
  }
  expect_int("forty starts each made a folder and ended it", ok, 40);
 }

 /* The lines the editor sends are the plan's, ahead of a folder's Arguments. */
 p = contained_plan(WPE_START_DEBUGGER, "gdb", "gdb", NULL, proj, "t.e");
 expect_int("five editor lines", wpe_startplan_editor_inputs(p), 5);
 wpe_startplan_input(p, "r x", WPE_FROM_FOLDER, "./xwperc: Arguments");
 expect_int("a folder's Arguments are not among them", wpe_startplan_editor_inputs(p), 5);
 expect_int("but are an input of the plan", p->ninput, 6);
 expect_str("the program the box names", wpe_startplan_program_path(p), prog);
 expect_int("the program's folder is the plan's", strcmp(p->program_dir, proj), 0);

 /* The history rule's two commands. */
 expect_int("gdb has a history rule", wpe_startplan_history_lines(p, ask, sizeof ask, fix, sizeof fix), 0);
 expect_str("it asks gdb where the history is", ask, "show history filename");
 snprintf(want, sizeof want, "set history filename %s/.gdb_history", proj);
 expect_str("and moves it to the program's folder", fix, want);
 end_plan(p);
 p = contained_plan(WPE_START_DEBUGGER, "gdb", "gdb", "mi", proj, "t.e");
 expect_int("MI has one too", wpe_startplan_history_lines(p, ask, sizeof ask, fix, sizeof fix), 0);
 expect_str("asked in MI's words", ask, "-gdb-show history filename");
 snprintf(want, sizeof want, "-interpreter-exec console \"set history filename %s/.gdb_history\"",
          proj);
 expect_str("and moved through the console", fix, want);
 end_plan(p);
 p = contained_plan(WPE_START_DEBUGGER, "pdb", "pdb", NULL, proj, "t.e");
 expect_int("pdb has none: it keeps no history file in its folder",
            wpe_startplan_history_lines(p, ask, sizeof ask, fix, sizeof fix), -1);
 end_plan(p);
 p = contained_plan(WPE_START_DEBUGGER, "dbx", "dbx", NULL, proj, "t.e");
 expect_int("a debugger that starts in the folder has none",
            wpe_startplan_history_lines(p, ask, sizeof ask, fix, sizeof fix), -1);
 end_plan(p);

 /* A program a folder names that lies outside it asks, as it did as a word of the command. */
 p = wpe_startplan_new(WPE_START_DEBUGGER, "gdb");
 wpe_startplan_program_origin(p, WPE_FROM_FOLDER, "the program the debugger is started on");
 wpe_startplan_set_file(p, "this folder", proj);
 wpe_startplan_contain(p, "gdb", "", proj, "/bin/true");
 wpe_startplan_word(p, "gdb", WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
 expect_int("a folder's program outside the folder asks", wpe_startplan_judge(p), WPE_PLAN_ASK);
 end_plan(p);
 p = wpe_startplan_new(WPE_START_DEBUGGER, "gdb");
 wpe_startplan_set_file(p, "this folder", proj);
 wpe_startplan_contain(p, "gdb", "", proj, "/bin/true");
 wpe_startplan_word(p, "gdb", WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
 expect_int("the user's own program outside it does not", wpe_startplan_judge(p), WPE_PLAN_CLEAN);
 end_plan(p);

 /* A window that knows no build folder gives the editor's own. */
 {
  char here[PATH_ROOM], real[PATH_ROOM];

  p = wpe_startplan_new(WPE_START_DEBUGGER, "gdb");
  expect_int("a start with no folder is made", wpe_startplan_contain(p, "gdb", "", NULL, "t.e"), 0);
  expect_int("the editor's own folder is the program's",
             getcwd(here, sizeof here) != NULL && realpath(here, real) != NULL &&
             p->program_dir != NULL && strcmp(p->program_dir, real) == 0, 1);
  expect_int("and still starts apart", p->debug_dir != NULL, 1);
  end_plan(p);
 }
 unlink(prog);
}

/* ---- A path as long as the system allows: one command holds it ---------- */

#define NAME_ROOM        256        /* NAME_MAX and its terminator */
#define NAME_BYTES       255        /* the longest name a file system takes */
#define LONG_NAME_BYTES  200        /* a name of a deep folder's level */
#define PATH_HEADROOM    200        /* what a deep folder leaves under PATH_ROOM */
#define E_ACUTE          "\xc3\xa9"    /* U+00E9 in UTF-8: two bytes */
#define CJK_ZHONG        "\xe4\xb8\xad" /* U+4E2D in UTF-8: three bytes */
#define PDB_CHDIR        "!__import__('os').chdir(b'"
#define LINE_ROOM        20000      /* a command the request engine takes, with room */
#define HISTORY_FIX_ROOM E_D_REQ_CMD_MAX

/* @unit written @n times, as one name. */
static void repeat_unit(char *out, size_t sz, const char *unit, int n)
{
 int i;

 out[0] = '\0';
 for (i = 0; i < n; i++)
  snprintf(out + strlen(out), sz - strlen(out), "%s", unit);
}

/* Make @depth folders below @from, each named @name; @out is the deepest. */
static void deep_folder(char *out, size_t sz, const char *from, const char *name, int depth)
{
 int i;

 snprintf(out, sz, "%s", from);
 for (i = 0; i < depth; i++)
 {
  snprintf(out + strlen(out), sz - strlen(out), "/%s", name);
  mkdir(out, 0700);
 }
}

/* Make one folder below @from that is @total bytes long: names of
   LONG_NAME_BYTES, then a last name that makes up the rest. */
static void deep_folder_of_size(char *out, size_t sz, const char *from, size_t total)
{
 char name[NAME_ROOM];
 size_t left;

 snprintf(out, sz, "%s", from);
 while ((left = total - strlen(out)) > NAME_BYTES + 1)
 {
  memset(name, 'n', LONG_NAME_BYTES);
  name[LONG_NAME_BYTES] = '\0';
  snprintf(out + strlen(out), sz - strlen(out), "/%s", name);
  mkdir(out, 0700);
 }
 memset(name, 'm', left - 1);        /* the separator takes one byte */
 name[left - 1] = '\0';
 snprintf(out + strlen(out), sz - strlen(out), "/%s", name);
 mkdir(out, 0700);
}

/* Remove what deep_folder or deep_folder_of_size made, down to @stop. */
static void deep_remove(char *folder, const char *stop)
{
 while (strcmp(folder, stop) != 0 && strlen(folder) > strlen(stop))
 {
  rmdir(folder);
  *strrchr(folder, '/') = '\0';
 }
}

/* How many levels of names of @name_bytes a folder can have under @proj
   and keep PATH_HEADROOM bytes to spare. */
static int levels_that_fit(size_t name_bytes)
{
 return (int)((PATH_ROOM - PATH_HEADROOM - strlen(proj)) / (name_bytes + 1));
}

/* How many times @needle is in @text. */
static int count_of(const char *text, const char *needle)
{
 int n = 0;

 for (; (text = strstr(text, needle)) != NULL; text += strlen(needle))
  n++;
 return n;
}

/* How many of a plan's input lines begin with @head. */
static int lines_with(const WpeStartPlan *p, const char *head)
{
 int i, n = 0;

 for (i = 0; i < p->ninput; i++)
  if (strncmp(p->input[i].text, head, strlen(head)) == 0)
   n++;
 return n;
}

/* The plan's line @i, or "" past the end. */
static const char *input_at(const WpeStartPlan *p, int i)
{
 return i >= 0 && i < p->ninput ? p->input[i].text : "";
}

/* @scenario SC-debug-128 */
/* S1, S2: a folder with one name of the longest length a file system takes
   cannot be reached by any shorter command; the start must not be refused. */
static void case_gdb_longest_name(void)
{
 char name[NAME_ROOM], deep[PATH_ROOM], prog[PATH_ROOM], want[PATH_ROOM];
 WpeStartPlan *p;

 repeat_unit(name, sizeof name, "a", NAME_BYTES);
 deep_folder(deep, sizeof deep, proj, name, 1);
 snprintf(prog, sizeof prog, "%s/t.e", deep);
 write_file(prog, "");

 p = contained_plan(WPE_START_DEBUGGER, "gdb", "gdb", NULL, deep, "t.e");
 expect_int("gdb: a folder of one 255-byte name is not refused", p->contain_refused == NULL, 1);
 expect_int("gdb: a pin, two style lines, one cd and the program", p->ninput, 5);
 snprintf(want, sizeof want, "cd %s", deep);
 expect_int("gdb: exactly one cd", lines_with(p, "cd "), 1);
 expect_str("gdb: the cd is the whole absolute folder", input_at(p, 3), want);
 snprintf(want, sizeof want, "file %s", prog);
 expect_str("gdb: the program is loaded by its absolute path", input_at(p, 4), want);
 expect_int("gdb: the plan is judged clean", wpe_startplan_judge(p), WPE_PLAN_CLEAN);
 end_plan(p);

 p = contained_plan(WPE_START_DEBUGGER, "gdb", "gdb", "mi", deep, "t.e");
 expect_int("MI: a pin, one cd and the program", p->ninput, 3);
 snprintf(want, sizeof want, "-environment-cd \"%s\"", deep);
 expect_str("MI: one cd with the whole folder", input_at(p, 1), want);
 snprintf(want, sizeof want, "-file-exec-and-symbols \"%s\"", prog);
 expect_str("MI: the program by its absolute path", input_at(p, 2), want);
 expect_int("MI: the plan is judged clean", wpe_startplan_judge(p), WPE_PLAN_CLEAN);
 end_plan(p);

 unlink(prog);
 deep_remove(deep, proj);
}

/* S3, S4: pdb's move writes every byte over ASCII as four, so a folder
   named in another language, or a long ASCII name, was refused. */
static void case_pdb_wide_names(void)
{
 char name[NAME_ROOM], rest[NAME_ROOM], deep[PATH_ROOM], prog[PATH_ROOM];
 const char *line;
 WpeStartPlan *p;

 repeat_unit(name, sizeof name, CJK_ZHONG, 20);
 deep_folder(deep, sizeof deep, proj, name, 1);
 snprintf(prog, sizeof prog, "%s/t.py", deep);
 write_file(prog, "");
 p = contained_plan(WPE_START_DEBUGGER, "pdb", "pdb", NULL, deep, "t.py");
 expect_int("pdb: a folder of 20 CJK characters is not refused", p->contain_refused == NULL, 1);
 expect_int("pdb: one move line", p->ninput, 1);
 line = input_at(p, 0);
 expect_int("pdb: it is a chdir of bytes", strncmp(line, PDB_CHDIR, strlen(PDB_CHDIR)), 0);
 expect_int("pdb: the name's twenty characters are written byte by byte",
            count_of(line, "\\xe4\\xb8\\xad"), 20);
 expect_int("pdb: the plan is judged clean", wpe_startplan_judge(p), WPE_PLAN_CLEAN);
 end_plan(p);
 unlink(prog);
 deep_remove(deep, proj);

 repeat_unit(name, sizeof name, E_ACUTE, 85);
 repeat_unit(rest, sizeof rest, "x", 85);
 strcat(name, rest);
 deep_folder(deep, sizeof deep, proj, name, 1);
 snprintf(prog, sizeof prog, "%s/t.py", deep);
 write_file(prog, "");
 p = contained_plan(WPE_START_DEBUGGER, "pdb", "pdb", NULL, deep, "t.py");
 expect_int("pdb: a name of 255 bytes, 85 of them two-byte letters, is not refused",
            p->contain_refused == NULL, 1);
 expect_int("pdb: one move line", p->ninput, 1);
 line = input_at(p, 0);
 expect_int("pdb: longer than the old 255", strlen(line) > 255, 1);
 expect_int("pdb: the engine takes it", e_d_req_cmd_fits("pdb", line), 1);
 expect_int("pdb: the 85 letters are written byte by byte", count_of(line, "\\xc3\\xa9"), 85);
 expect_int("pdb: the plan is judged clean", wpe_startplan_judge(p), WPE_PLAN_CLEAN);
 end_plan(p);
 unlink(prog);
 deep_remove(deep, proj);
}

/* S5: the deepest folder the system allows, made of non-ASCII names: pdb's
   move, four bytes per byte, still fits one command, and the engine takes
   it. */
static void case_pdb_deepest_folder(void)
{
 char name[NAME_ROOM], deep[PATH_ROOM], prog[PATH_ROOM];
 const char *line;
 WpeStartPlan *p;
 int levels = levels_that_fit(2 * 127);

 repeat_unit(name, sizeof name, E_ACUTE, 127);
 deep_folder(deep, sizeof deep, proj, name, levels);
 snprintf(prog, sizeof prog, "%s/t.py", deep);
 write_file(prog, "");
 p = contained_plan(WPE_START_DEBUGGER, "pdb", "pdb", NULL, deep, "t.py");
 expect_int("pdb: a folder of about 3.8 KB of two-byte letters is not refused",
            p->contain_refused == NULL, 1);
 expect_int("pdb: one move line", p->ninput, 1);
 line = input_at(p, 0);
 expect_int("pdb: the line is far over the old 255", strlen(line) > 3000, 1);
 expect_int("pdb: and the engine takes it", e_d_req_cmd_fits("pdb", line), 1);
 expect_int("pdb: the plan is judged clean", wpe_startplan_judge(p), WPE_PLAN_CLEAN);
 end_plan(p);
 unlink(prog);
 deep_remove(deep, proj);
}

/* S6: a deep ASCII folder is one move line and the program's absolute path,
   not a chain of cd; the history rule always has its command. */
static void case_gdb_long_folder(void)
{
 char name[NAME_ROOM], deep[PATH_ROOM], prog[PATH_ROOM], want[PATH_ROOM];
 static char ask[HISTORY_FIX_ROOM], fix[HISTORY_FIX_ROOM];
 WpeStartPlan *p;

 memset(name, 'd', LONG_NAME_BYTES);
 name[LONG_NAME_BYTES] = '\0';
 deep_folder(deep, sizeof deep, proj, name, levels_that_fit(LONG_NAME_BYTES));
 snprintf(prog, sizeof prog, "%s/t.e", deep);
 write_file(prog, "");

 p = contained_plan(WPE_START_DEBUGGER, "gdb", "gdb", NULL, deep, "t.e");
 expect_int("a 3.8 KB folder is one cd, not a chain", lines_with(p, "cd "), 1);
 expect_int("the plan has the pin, two style lines, the cd and the program: five lines", p->ninput, 5);
 snprintf(want, sizeof want, "cd %s", deep);
 expect_str("the cd is the whole absolute folder", input_at(p, 3), want);
 snprintf(want, sizeof want, "file %s", prog);
 expect_str("the program loads by its absolute path", input_at(p, 4), want);
 expect_int("the program is still the plan's absolute path", wpe_startplan_program_path(p) != NULL &&
            strcmp(wpe_startplan_program_path(p), prog) == 0, 1);
 expect_int("the plan is judged clean", wpe_startplan_judge(p), WPE_PLAN_CLEAN);
 expect_int("the history rule has its command", wpe_startplan_history_lines(p, ask, sizeof ask,
            fix, sizeof fix), 0);
 snprintf(want, sizeof want, "set history filename %s/.gdb_history", deep);
 expect_str("and it names the folder's own history file", fix, want);
 end_plan(p);

 p = contained_plan(WPE_START_DEBUGGER, "gdb", "gdb", "mi", deep, "t.e");
 expect_int("over MI too: one cd", lines_with(p, "-environment-cd \""), 1);
 expect_int("over MI too: three lines", p->ninput, 3);
 snprintf(want, sizeof want, "-file-exec-and-symbols \"%s\"", prog);
 expect_str("the program by its absolute path", input_at(p, 2), want);
 expect_int("the plan is judged clean", wpe_startplan_judge(p), WPE_PLAN_CLEAN);
 expect_int("the MI history rule has its command", wpe_startplan_history_lines(p, ask, sizeof ask,
            fix, sizeof fix), 0);
 end_plan(p);

 unlink(prog);
 deep_remove(deep, proj);
}

/* S9, S10: a path longer than the system allows is refused for that reason,
   and a path that is merely unsafe still says so. */
static void case_long_path_refusals(void)
{
 static char long_folder[PATH_ROOM + 16];
 char deep[PATH_ROOM], name[NAME_ROOM], bad[PATH_ROOM];
 WpeStartPlan *p;
 int rc;

 /* a folder string longer than any path: realpath cannot take it */
 memset(long_folder, 'q', PATH_ROOM + 10);
 long_folder[0] = '/';
 long_folder[PATH_ROOM + 10] = '\0';
 p = wpe_startplan_new(WPE_START_DEBUGGER, "gdb");
 wpe_startplan_set_file(p, "this folder", proj);
 rc = wpe_startplan_contain(p, "gdb", NULL, long_folder, "t.e");
 expect_int("a folder past the system's limit: contain says no", rc, -1);
 expect_str("and says why", p->contain_refused, WPE_STARTPLAN_REFUSE_DEBUG_LONG);
 expect_int("the plan is refused", wpe_startplan_judge(p), WPE_PLAN_REFUSED);
 expect_str("with that reason", p->refused, WPE_STARTPLAN_REFUSE_DEBUG_LONG);
 end_plan(p);

 /* a folder near the limit and a program name that does not fit under it */
 deep_folder_of_size(deep, sizeof deep, proj, PATH_ROOM - 60);
 memset(name, 'p', 100);
 name[100] = '\0';
 p = contained_plan(WPE_START_DEBUGGER, "gdb", "gdb", NULL, deep, name);
 expect_str("a program whose path passes the limit: the reason", p->contain_refused,
            WPE_STARTPLAN_REFUSE_DEBUG_LONG);
 expect_int("the plan is refused", wpe_startplan_judge(p), WPE_PLAN_REFUSED);
 end_plan(p);
 deep_remove(deep, proj);

 /* the positive control: an unsafe path keeps its own reason */
 snprintf(bad, sizeof bad, "%s/bad\001dir", proj);
 make_dir(bad);
 p = contained_plan(WPE_START_DEBUGGER, "gdb", "gdb", NULL, bad, NULL);
 expect_str("a folder with a control character is still unsafe", p->contain_refused,
            WPE_STARTPLAN_REFUSE_DEBUG_PATH);
 expect_int("and the plan is refused", wpe_startplan_judge(p), WPE_PLAN_REFUSED);
 end_plan(p);
 rmdir(bad);
}

#define FAIL_SCAN_MAX 4000L      /* more allocations than any start makes */

/* Make each allocation of one contain call fail in turn, and check that a
   start that could not be planned for want of memory says so: the memory
   text, never the one for a path that cannot be given safely, no text at
   all, or a plan that goes on as if nothing failed. */
static void scan_allocation_failures(const char *what, const char *key, const char *variant,
                                     const char *program)
{
 char name[TEXT_ROOM];
 WpeStartPlan *p;
 long k;
 int rc, fired;

 for (k = 0; k < FAIL_SCAN_MAX; k++)
 {
  p = wpe_startplan_new(WPE_START_DEBUGGER, key);
  if (!p) { fail("plan", "out of memory"); exit(1); }
  wpe_startplan_set_file(p, "this folder", proj);
  fail_countdown = k;
  rc = wpe_startplan_contain(p, key, variant, proj, program);
  fired = fail_countdown == FAIL_NEVER;
  fail_countdown = FAIL_NEVER;
  if (!fired)
  {
   snprintf(name, sizeof name, "%s: with %ld allocations to spare it plans", what, k);
   expect_int(name, rc, 0);
   end_plan(p);
   return;
  }
  snprintf(name, sizeof name, "%s: allocation %ld failing is reported", what, k);
  expect_int(name, rc, -1);
  snprintf(name, sizeof name, "%s: allocation %ld failing says why", what, k);
  expect_int(name, p->contain_refused != NULL &&
             (strcmp(p->contain_refused, WPE_STARTPLAN_REFUSE_DEBUG_MEMORY) == 0 ||
              strcmp(p->contain_refused, WPE_STARTPLAN_REFUSE_DEBUG_FOLDER) == 0), 1);
  end_plan(p);
 }
 fail(what, "the scan never reached an allocation that is not made: raise FAIL_SCAN_MAX");
}

/* An allocation that fails while a debugger's start is planned is refused
   with the memory text, on every route of the plan: the pin and move lines,
   the program's path and its load line, and the lines put into the plan. */
static void case_allocation_failures(void)
{
#ifndef WPE_TEST_LD_WRAP
 printf("SKIP allocation failures: the linker has no --wrap\n");
 return;
#endif
 scan_allocation_failures("gdb console", "gdb", NULL, NULL);
 scan_allocation_failures("gdb console with a program", "gdb", NULL, "t.e");
 scan_allocation_failures("gdb MI with a program", "gdb", "mi", "t.e");
 scan_allocation_failures("pdb", "pdb", NULL, "t.py");
 scan_allocation_failures("jdb", "jdb", NULL, NULL);
}

/* S6b: a folder within a few bytes of the system's limit: the history file's
   path passes the limit by the separator and its name, and the rule still has
   its command, which gdb is left to judge. */
static void case_gdb_folder_at_the_limit(void)
{
 char deep[PATH_ROOM];
 static char want[HISTORY_FIX_ROOM], ask[HISTORY_FIX_ROOM], fix[HISTORY_FIX_ROOM];
 const size_t total = PATH_ROOM - 8;
 WpeStartPlan *p;

 deep_folder_of_size(deep, sizeof deep, proj, total);
 expect_int("the folder is as long as the test meant", (int)strlen(deep), (int)total);
 p = contained_plan(WPE_START_DEBUGGER, "gdb", "gdb", NULL, deep, NULL);
 expect_int("a folder near the system's limit is not refused", p->contain_refused == NULL, 1);
 expect_int("one cd of the whole folder", lines_with(p, "cd "), 1);
 expect_int("the history file's path passes the limit",
            (int)(strlen(deep) + strlen("/.gdb_history")) >= PATH_ROOM, 1);
 expect_int("and the rule has its command anyway", wpe_startplan_history_lines(p, ask, sizeof ask,
            fix, sizeof fix), 0);
 snprintf(want, sizeof want, "set history filename %s/.gdb_history", deep);
 expect_str("naming the folder's history file", fix, want);
 end_plan(p);
 deep_remove(deep, proj);
}

/* An answer remembered for a gdb start before it started apart asks once more: the
   canonical text now carries the containment lines. */
static void case_contain_gdb_asks_again(void)
{
 char rc[PATH_ROOM], old_text[TEXT_ROOM], key[WPE_STARTPLAN_KEY_MAX];
 WpeStartPlan *p;

 file_in(rc, sizeof rc, proj, ".gdbinit");
 write_file(rc, "set print pretty on\n");
 p = contained_plan(WPE_START_DEBUGGER, "gdb", "gdb", NULL, proj, NULL);
 wpe_startplan_word(p, "gdb", WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
 wpe_startplan_tool_reads(p, "gdb");
 wpe_startplan_key(p, key, sizeof key);
 expect_int("the folder's .gdbinit asks", wpe_startplan_judge(p), WPE_PLAN_ASK);
 snprintf(old_text, sizeof old_text,
          "xwpe-start 2\nkind debug\ncwd %s\nargv gdb\nreads %s 20\nset print pretty on\n",
          proj, rc);
 expect_int("the text of a start before the change was remembered",
            wpe_startcmd_remember(p->folder, key, old_text, WPE_STARTCMD_YES), 0);
 expect_int("a start that now carries its containment asks once more",
            wpe_startplan_judge(p), WPE_PLAN_ASK);
 end_plan(p);
 unlink(rc);
}

/* Run it: what is loaded, by which command, before the program. */
static void case_load_lines(void)
{
 char gi[PATH_ROOM], prog[PATH_ROOM], py[PATH_ROOM], emb[PATH_ROOM], want[PATH_ROOM], *text;
 WpeStartPlan *p;

 file_in(gi, sizeof gi, proj, ".gdbinit");
 file_in(prog, sizeof prog, proj, "t.e");
 file_in(py, sizeof py, proj, "t.e-gdb.py");
 file_in(emb, sizeof emb, proj, "emb.py");
 write_file(gi, "set print pretty on\n");
 write_file(prog, "");
 wpe_startplan_rows_designed(1);

 /* gdb console, a folder .gdbinit */
 p = contained_plan(WPE_START_DEBUGGER, "gdb", "gdb", NULL, proj, "t.e");
 wpe_startplan_word(p, "gdb", WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
 wpe_startplan_tool_reads_of(p, "gdb", "t.e");
 wpe_startplan_judge(p);
 text = strdup(wpe_startplan_text(p));
 expect_int("Run it adds the admit and the source", wpe_startplan_load_lines(p), 2);
 expect_int("seven lines now", p->ninput, 7);
 snprintf(want, sizeof want, "add-auto-load-safe-path %s", gi);
 expect_str("gdb: the file is admitted", p->ninput > 4 ? p->input[4].text : "", want);
 snprintf(want, sizeof want, "source %s", gi);
 expect_str("gdb: then run", p->ninput > 5 ? p->input[5].text : "", want);
 snprintf(want, sizeof want, "file %s", prog);
 expect_str("gdb: the program still loads last", p->ninput > 6 ? p->input[6].text : "", want);
 /* S4: an admit is containment; the user's .gdbinit is the debugger's to refuse */
 expect_int("gdb: a refused admit stops the start", wpe_startplan_input_outcome(p, 4), WPE_REFUSAL_STOPS);
 expect_int("gdb: a refused source is said", wpe_startplan_input_outcome(p, 5), WPE_REFUSAL_SAID);
 expect_str("gdb: and names the file it reads, as the box lists it",
            wpe_startplan_input_file(p, 5), "./.gdbinit");
 expect_int("gdb: an admit names no file", wpe_startplan_input_file(p, 4) == NULL, 1);
 expect_str("gdb: the pin is still first", p->ninput ? p->input[0].text : "", GDB_PIN);
 wpe_startplan_judge(p);
 expect_int("the plan with its load lines is not refused", p->verdict != WPE_PLAN_REFUSED, 1);
 expect_int("its text differs from the plan without them",
            strcmp(wpe_startplan_text(p), text) != 0, 1);
 expect_int("Run it once", wpe_startplan_load_lines(p), 0);
 expect_int("and adds nothing more", p->ninput, 7);
 free(text);
 end_plan(p);

 /* gdb console, a script beside the program */
 write_file(py, "print(1)\n");
 p = contained_plan(WPE_START_DEBUGGER, "gdb", "gdb", NULL, proj, "t.e");
 wpe_startplan_tool_reads_of(p, "gdb", "t.e");
 wpe_startplan_judge(p);
 expect_int("a beside script is admitted, not sourced: three lines with the .gdbinit's two",
            wpe_startplan_load_lines(p), 3);
 snprintf(want, sizeof want, "add-auto-load-safe-path %s", py);
 expect_str("by its own path", p->ninput > 6 ? p->input[6].text : "", want);
 snprintf(want, sizeof want, "file %s", prog);
 expect_str("and before the program", p->ninput > 7 ? p->input[7].text : "", want);
 end_plan(p);
 unlink(py);

 /* gdb: the scripts a program carries, a text entry and a file entry */
 unlink(gi);
 write_file(emb, "open('ran','w').close()\n");
 {
  static const char both[] = "\004name\nprint(1)\n\0\001emb.py\0";
  elf_image e;

  elf_make(&e, 1, 0, both, sizeof both - 1);
  write_bytes(prog, (const char *)e.b, e.len);
 }
 p = contained_plan(WPE_START_DEBUGGER, "gdb", "gdb", NULL, proj, "t.e");
 wpe_startplan_tool_reads_of(p, "gdb", "t.e");
 wpe_startplan_judge(p);
 expect_int("the program's own scripts: the program for the text, the file for the file entry",
            wpe_startplan_load_lines(p), 2);
 snprintf(want, sizeof want, "add-auto-load-safe-path %s", prog);
 expect_str("a text entry is checked by the program's path", p->ninput > 4 ? p->input[4].text : "", want);
 snprintf(want, sizeof want, "add-auto-load-safe-path %s", emb);
 expect_str("a file entry by the script's path", p->ninput > 5 ? p->input[5].text : "", want);
 end_plan(p);
 unlink(emb);
 write_file(prog, "");
 write_file(gi, "set print pretty on\n");

 /* gdb, MI */
 p = contained_plan(WPE_START_DEBUGGER, "gdb", "gdb", "mi", proj, "t.e");
 wpe_startplan_tool_reads_of(p, "gdb", "t.e");
 wpe_startplan_judge(p);
 expect_int("mi: the same two lines", wpe_startplan_load_lines(p), 2);
 snprintf(want, sizeof want, "-interpreter-exec console \"add-auto-load-safe-path %s\"", gi);
 expect_str("mi: admit through the console", p->ninput > 2 ? p->input[2].text : "", want);
 snprintf(want, sizeof want, "-interpreter-exec console \"source %s\"", gi);
 expect_str("mi: source through the console", p->ninput > 3 ? p->input[3].text : "", want);
 snprintf(want, sizeof want, "-file-exec-and-symbols \"%s\"", prog);
 expect_str("mi: the program last", p->ninput > 4 ? p->input[4].text : "", want);
 end_plan(p);

 /* gdb speaking DAP */
 p = wpe_startplan_new(WPE_START_DEBUG_ADAPTER, "gdb");
 wpe_startplan_set_file(p, "this folder", proj);
 wpe_startplan_word(p, "gdb", WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
 wpe_startplan_contain(p, "dap", "gdb", proj, NULL);
 wpe_startplan_tool_reads_of(p, "gdb", NULL);
 wpe_startplan_judge(p);
 expect_int("gdb-dap: admit and -ix", wpe_startplan_load_lines(p), 4);
 expect_int("gdb-dap: nine words in all", p->nword, 9);
 snprintf(want, sizeof want, "add-auto-load-safe-path %s", gi);
 expect_str("gdb-dap: admit", p->nword > 6 ? p->word[6].text : "", want);
 expect_str("gdb-dap: -ix", p->nword > 7 ? p->word[7].text : "", "-ix");
 expect_str("gdb-dap: the file, which gdb runs before the program loads",
            p->nword > 8 ? p->word[8].text : "", gi);
 end_plan(p);

 /* gdb refuses a path with a ':' and Run it is not offered */
 {
  char colon[PATH_ROOM], cgi[PATH_ROOM];

  file_in(colon, sizeof colon, proj, "co:lon");
  make_dir(colon);
  file_in(cgi, sizeof cgi, colon, ".gdbinit");
  write_file(cgi, "set print pretty on\n");
  p = contained_plan(WPE_START_DEBUGGER, "gdb", "gdb", NULL, colon, NULL);
  wpe_startplan_tool_reads(p, "gdb");
  wpe_startplan_judge(p);
  expect_int("a folder with a colon cannot be admitted", wpe_startplan_load_lines(p), -1);
  expect_str("and Run it says why", p->load_refused, WPE_STARTPLAN_LOAD_REFUSED);
  expect_int("nothing was admitted", p->ninput, 4);
  end_plan(p);
  unlink(cgi);
  rmdir(colon);
 }
 unlink(gi);

 /* lldb-dap */
 {
  char li[PATH_ROOM];

  file_in(li, sizeof li, proj, ".lldbinit");
  write_file(li, "settings set target.x 1\n");
  p = wpe_startplan_new(WPE_START_DEBUG_ADAPTER, "lldb");
  wpe_startplan_set_file(p, "this folder", proj);
  wpe_startplan_word(p, "lldb-dap", WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
  wpe_startplan_contain(p, "dap", "lldb-dap", proj, NULL);
  wpe_startplan_tool_reads(p, "lldb");
  wpe_startplan_judge(p);
  expect_int("lldb-dap: one command", wpe_startplan_load_lines(p), 1);
  snprintf(want, sizeof want, "command source \"%s\"", li);
  expect_str("lldb-dap: sourced by its init commands", p->ninit > 1 ? p->init[1].text : "", want);
  expect_str("lldb-dap: after the pin", p->ninit ? p->init[0].text : "", LLDB_PIN);
  end_plan(p);
  unlink(li);
 }

 /* pdb: the .pdbrc's lines, one by one, after the move */
 {
  char rcp[PATH_ROOM];

  file_in(rcp, sizeof rcp, proj, ".pdbrc");
  write_file(rcp, "p 1\n# a comment\n\nbreak 3\n");
  p = contained_plan(WPE_START_DEBUGGER, "pdb", "pdb", NULL, proj, NULL);
  wpe_startplan_tool_reads(p, "pdb");
  wpe_startplan_judge(p);
  expect_int("pdb: two lines, comments and blanks left out", wpe_startplan_load_lines(p), 2);
  expect_int("pdb: after the move", p->ninput, 3);
  expect_str("pdb: the first", p->ninput > 1 ? p->input[1].text : "", "p 1");
  expect_str("pdb: the second", p->ninput > 2 ? p->input[2].text : "", "break 3");
  end_plan(p);
  unlink(rcp);
 }

 /* jdb: read, and a folder with a blank is refused */
 {
  char ji[PATH_ROOM], sp[PATH_ROOM], spi[PATH_ROOM];

  file_in(ji, sizeof ji, proj, "jdb.ini");
  write_file(ji, "stop at A:1\n");
  p = contained_plan(WPE_START_DEBUGGER, "jdb", "jdb", NULL, proj, NULL);
  wpe_startplan_tool_reads(p, "jdb");
  wpe_startplan_judge(p);
  expect_int("jdb: one read", wpe_startplan_load_lines(p), 1);
  snprintf(want, sizeof want, "read %s", ji);
  expect_str("jdb: as its first command", p->ninput ? p->input[0].text : "", want);
  end_plan(p);
  unlink(ji);
  file_in(sp, sizeof sp, proj, "sp ace");
  make_dir(sp);
  file_in(spi, sizeof spi, sp, "jdb.ini");
  write_file(spi, "stop at A:1\n");
  p = contained_plan(WPE_START_DEBUGGER, "jdb", "jdb", NULL, sp, NULL);
  wpe_startplan_tool_reads(p, "jdb");
  wpe_startplan_judge(p);
  expect_int("jdb: a folder with a blank cannot be read", wpe_startplan_load_lines(p), -1);
  expect_int("and Run it is not offered", p->load_refused != NULL, 1);
  end_plan(p);
  unlink(spi);
  rmdir(sp);
 }

 /* The shipped rows: no load line, the debugger reads the file itself. */
 wpe_startplan_rows_designed(0);
 write_file(gi, "p 1\n");
 file_in(py, sizeof py, proj, ".dbxinit");
 write_file(py, "print 1\n");
 p = contained_plan(WPE_START_DEBUGGER, "dbx", "dbx", NULL, proj, "t.e");
 wpe_startplan_tool_reads_of(p, "dbx", "t.e");
 expect_int("a row that does not start apart loads nothing", wpe_startplan_load_lines(p), 0);
 expect_int("and has no line", p->ninput, 0);
 unlink(py);
 end_plan(p);
 unlink(gi);
 unlink(prog);
}

/* pdb's .pdbrc under Run it: the lines pdb can be given one at a time. */
static void case_load_pdbrc(void)
{
 char rcp[PATH_ROOM], longline[400];
 WpeStartPlan *p;

 file_in(rcp, sizeof rcp, proj, ".pdbrc");

 write_file(rcp, "break 3\ncommands 1\nsilent\nend\n");
 p = contained_plan(WPE_START_DEBUGGER, "pdb", "pdb", NULL, proj, NULL);
 wpe_startplan_tool_reads(p, "pdb");
 wpe_startplan_judge(p);
 /* S3: a commands block is offered like any other lines; what pdb answers
    to each line decides at the start, and the file supplies the block's end */
 expect_int("a commands block is sent a line at a time, with its end", wpe_startplan_load_lines(p), 4);
 expect_int("and Run it is offered", p->load_refused == NULL, 1);
 expect_str("the block opens", p->ninput > 2 ? p->input[2].text : "", "commands 1");
 expect_str("its body", p->ninput > 3 ? p->input[3].text : "", "silent");
 expect_str("and its end is sent", p->ninput > 4 ? p->input[4].text : "", "end");
 expect_int("the move is containment: a refusal stops the start",
            wpe_startplan_input_outcome(p, 0), WPE_REFUSAL_STOPS);
 expect_int("a line of the user's .pdbrc is said and the start goes on",
            wpe_startplan_input_outcome(p, 1), WPE_REFUSAL_SAID);
 expect_int("the block's lines too", wpe_startplan_input_outcome(p, 4), WPE_REFUSAL_SAID);
 expect_int("each names the .pdbrc it is a line of",
            wpe_startplan_input_file(p, 2) != NULL &&
            strstr(wpe_startplan_input_file(p, 2), ".pdbrc") != NULL, 1);
 expect_int("the move names no file", wpe_startplan_input_file(p, 0) == NULL, 1);
 end_plan(p);

 write_file(rcp, "  commands\nend\n");
 p = contained_plan(WPE_START_DEBUGGER, "pdb", "pdb", NULL, proj, NULL);
 wpe_startplan_tool_reads(p, "pdb");
 wpe_startplan_judge(p);
 expect_int("an indented commands line is one too", wpe_startplan_load_lines(p), 2);
 end_plan(p);

 write_file(rcp, "commandsx = 1\n!print(commands)\n");
 p = contained_plan(WPE_START_DEBUGGER, "pdb", "pdb", NULL, proj, NULL);
 wpe_startplan_tool_reads(p, "pdb");
 wpe_startplan_judge(p);
 expect_int("other lines that begin with the word are sent", wpe_startplan_load_lines(p), 2);
 end_plan(p);

 /* pdb cuts a line at ";;" into commands: each is sent alone, an alias whole. */
 write_file(rcp, "p 1;;p 2 ;;  p 3\nalias zz p 1;;p 2\n;;p 4\n");
 p = contained_plan(WPE_START_DEBUGGER, "pdb", "pdb", NULL, proj, NULL);
 wpe_startplan_tool_reads(p, "pdb");
 wpe_startplan_judge(p);
 expect_int("a line cut at ;; is sent as its commands", wpe_startplan_load_lines(p), 5);
 expect_str("the first part", p->ninput > 1 ? p->input[1].text : "", "p 1");
 expect_str("trimmed at its end", p->ninput > 2 ? p->input[2].text : "", "p 2");
 expect_str("the rest trimmed at its start", p->ninput > 3 ? p->input[3].text : "", "p 3");
 expect_str("an alias keeps its ;;", p->ninput > 4 ? p->input[4].text : "", "alias zz p 1;;p 2");
 expect_str("a ;; at the start of a line is not a cut", p->ninput > 5 ? p->input[5].text : "", ";;p 4");
 end_plan(p);

 /* pdb leaves out a line of blanks and an indented comment; sent, the blank
    line would repeat the command before it. */
 write_file(rcp, "p 1\n   \n\t# a note\n  p 2\n");
 p = contained_plan(WPE_START_DEBUGGER, "pdb", "pdb", NULL, proj, NULL);
 wpe_startplan_tool_reads(p, "pdb");
 wpe_startplan_judge(p);
 expect_int("blank and comment lines are left out, indented too", wpe_startplan_load_lines(p), 2);
 expect_str("the indented command is sent", p->ninput > 2 ? p->input[2].text : "", "  p 2");
 end_plan(p);

 /* A line of 398 bytes, which was refused, is a command like any other. */
 memset(longline, 'x', sizeof longline - 2);
 longline[0] = 'p';
 longline[1] = ' ';
 longline[sizeof longline - 2] = '\n';
 longline[sizeof longline - 1] = '\0';
 write_file(rcp, longline);
 p = contained_plan(WPE_START_DEBUGGER, "pdb", "pdb", NULL, proj, NULL);
 wpe_startplan_tool_reads(p, "pdb");
 wpe_startplan_judge(p);
 expect_int("a line of 398 bytes is sent", wpe_startplan_load_lines(p), 1);
 expect_int("and Run it is offered", p->load_refused == NULL, 1);
 expect_int("whole", p->ninput > 1 ? (int)strlen(p->input[1].text) : -1, (int)sizeof longline - 2);
 end_plan(p);

 /* The same length cut at ";;" into two commands. */
 longline[200] = ';';
 longline[201] = ';';
 write_file(rcp, longline);
 p = contained_plan(WPE_START_DEBUGGER, "pdb", "pdb", NULL, proj, NULL);
 wpe_startplan_tool_reads(p, "pdb");
 wpe_startplan_judge(p);
 expect_int("a long line cut at ;; into commands is sent as them", wpe_startplan_load_lines(p), 2);
 end_plan(p);
 unlink(rcp);
}

#define PDBRC_WHOLE_BYTES  16000   /* a file under WPE_STARTPLAN_READS_MAX */
#define PDBRC_TOO_BYTES    17000   /* a file over it */

/* S7: a .pdbrc line is sent whole up to the most a start-up file is read; a
   file past that is not run unseen. */
static void case_load_pdbrc_longest_line(void)
{
 char rcp[PATH_ROOM];
 char *text = malloc(PDBRC_TOO_BYTES + 2);
 WpeStartPlan *p;

 if (!text) { fail("long .pdbrc line", "out of memory"); return; }
 file_in(rcp, sizeof rcp, proj, ".pdbrc");

 memset(text, 'x', PDBRC_WHOLE_BYTES);
 text[0] = 'p';
 text[1] = ' ';
 text[PDBRC_WHOLE_BYTES] = '\n';
 text[PDBRC_WHOLE_BYTES + 1] = '\0';
 write_file(rcp, text);
 p = contained_plan(WPE_START_DEBUGGER, "pdb", "pdb", NULL, proj, NULL);
 wpe_startplan_tool_reads(p, "pdb");
 wpe_startplan_judge(p);
 expect_int("a 16000-byte line in a readable file is sent", wpe_startplan_load_lines(p), 1);
 expect_int("whole", p->ninput > 1 ? (int)strlen(p->input[1].text) : -1, PDBRC_WHOLE_BYTES);
 expect_int("the engine takes it", e_d_req_cmd_fits("pdb", input_at(p, 1)), 1);
 end_plan(p);

 memset(text, 'x', PDBRC_TOO_BYTES);
 text[0] = 'p';
 text[1] = ' ';
 text[PDBRC_TOO_BYTES] = '\n';
 text[PDBRC_TOO_BYTES + 1] = '\0';
 write_file(rcp, text);
 p = contained_plan(WPE_START_DEBUGGER, "pdb", "pdb", NULL, proj, NULL);
 wpe_startplan_tool_reads(p, "pdb");
 wpe_startplan_judge(p);
 expect_int("a file past what is read is listed too long", wpe_startplan_reads_too_long(p), 1);
 end_plan(p);

 free(text);
 unlink(rcp);
}

/* A script under a deep folder: pdb moves there with one chdir. */
static void case_pdb_long_folder(void)
{
 char deep[PATH_ROOM], prog[PATH_ROOM], name[NAME_ROOM];
 WpeStartPlan *p;

 memset(name, 'd', LONG_NAME_BYTES);
 name[LONG_NAME_BYTES] = '\0';
 deep_folder(deep, sizeof deep, proj, name, levels_that_fit(LONG_NAME_BYTES));
 snprintf(prog, sizeof prog, "%s/t.py", deep);
 write_file(prog, "");

 p = contained_plan(WPE_START_DEBUGGER, "pdb", "pdb", NULL, deep, "t.py");
 expect_int("a 3.8 KB folder is one chdir, not a chain", lines_with(p, PDB_CHDIR), 1);
 expect_int("and the plan's only line", p->ninput, 1);
 expect_int("the engine takes it", e_d_req_cmd_fits("pdb", input_at(p, 0)), 1);
 expect_int("the script is still the plan's absolute path", wpe_startplan_program_path(p) != NULL &&
            strcmp(wpe_startplan_program_path(p), prog) == 0, 1);
 expect_int("the plan is judged clean", wpe_startplan_judge(p), WPE_PLAN_CLEAN);
 end_plan(p);

 unlink(prog);
 deep_remove(deep, proj);
}

/* S8: jdb's read and lldb's source of a start-up file in a deep folder. */
static void case_jdb_lldb_deep_startup(void)
{
 char name[NAME_ROOM], deep[PATH_ROOM], init[PATH_ROOM], want[PATH_ROOM];
 WpeStartPlan *p;

 memset(name, 'j', LONG_NAME_BYTES);
 name[LONG_NAME_BYTES] = '\0';
 deep_folder(deep, sizeof deep, proj, name, levels_that_fit(LONG_NAME_BYTES));
 snprintf(init, sizeof init, "%s/jdb.ini", deep);
 write_file(init, "stop at Main:3\n");

 p = contained_plan(WPE_START_DEBUGGER, "jdb", "jdb", NULL, deep, NULL);
 wpe_startplan_tool_reads(p, "jdb");
 wpe_startplan_judge(p);
 expect_int("jdb: the start-up file in a deep folder is listed", p->npath >= 1, 1);
 expect_int("jdb: Run it adds its read", wpe_startplan_load_lines(p), 1);
 snprintf(want, sizeof want, "read %s", init);
 expect_str("jdb: one read of the absolute path", input_at(p, p->ninput - 1), want);
 expect_int("jdb: the engine takes the line", e_d_req_cmd_fits("jdb", input_at(p, p->ninput - 1)), 1);
 expect_int("jdb: the line is longer than the old 255", strlen(input_at(p, p->ninput - 1)) > 255, 1);
 end_plan(p);
 unlink(init);

 snprintf(init, sizeof init, "%s/.lldbinit", deep);
 write_file(init, "version\n");
 wpe_startplan_rows_designed(1);
 p = wpe_startplan_new(WPE_START_DEBUG_ADAPTER, "lldb");
 wpe_startplan_set_file(p, "this folder", deep);
 wpe_startplan_word(p, "lldb-dap", WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
 wpe_startplan_contain(p, "dap", "lldb-dap", deep, NULL);
 wpe_startplan_tool_reads(p, "lldb");
 wpe_startplan_judge(p);
 expect_int("lldb: Run it adds its source", wpe_startplan_load_lines(p), 1);
 snprintf(want, sizeof want, "command source \"%s\"", init);
 expect_str("lldb: one source of the absolute path", p->ninit > 1 ? p->init[1].text : "", want);
 end_plan(p);
 wpe_startplan_rows_designed(0);
 unlink(init);
 deep_remove(deep, proj);
}

/* An answer remembered for a pdb or jdb start before it started apart asks once more. */
static void case_contain_pdb_jdb_asks_again(void)
{
 char rc[PATH_ROOM], old_text[TEXT_ROOM], key[WPE_STARTPLAN_KEY_MAX], store[PATH_ROOM];
 WpeStartPlan *p;

 if (wpe_startcmd_store_path(store, sizeof store) == 0)
  unlink(store);                    /* earlier cases' answers are not this one's */
 file_in(rc, sizeof rc, proj, ".pdbrc");
 write_file(rc, "p 1\n");
 p = contained_plan(WPE_START_DEBUGGER, "pdb", "pdb", NULL, proj, NULL);
 wpe_startplan_word(p, "pdb", WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
 wpe_startplan_tool_reads(p, "pdb");
 wpe_startplan_key(p, key, sizeof key);
 expect_int("the folder's .pdbrc asks", wpe_startplan_judge(p), WPE_PLAN_ASK);
 snprintf(old_text, sizeof old_text,
          "xwpe-start 2\nkind debug\ncwd %s\nargv pdb\nreads %s 4\np 1\n", proj, rc);
 expect_int("the text of a start before the change was remembered",
            wpe_startcmd_remember(p->folder, key, old_text, WPE_STARTCMD_YES), 0);
 expect_int("a start that now carries its move asks once more", wpe_startplan_judge(p), WPE_PLAN_ASK);
 end_plan(p);
 unlink(rc);
}

/* Remove exactly what setup and the cases made. */
static void teardown(void)
{
 char path[PATH_ROOM];
 if (wpe_startcmd_store_path(path, sizeof path) == 0) unlink(path);
 snprintf(path, sizeof path, "%s/%s", state, WPE_STARTCMD_STATE_SUBDIR);
 rmdir(path);
 rmdir(state);
 snprintf(path, sizeof path, "%s/t.o", proj);
 unlink(path);
 snprintf(path, sizeof path, "%s/t.elf", proj);
 unlink(path);
 snprintf(path, sizeof path, "%s/t.e", proj);
 unlink(path);
 unlink(link_out);
 rmdir(proj);
 rmdir(outside);
 rmdir(home);
 rmdir(dbgbase);
 rmdir(base);
}

int main(void)
{
 setup();
 case_operand();
 case_folder_option();
 case_clean();
 case_link_out();
 case_dangling_link_out();
 case_remove();
 case_users_own_outside();
 case_home_is_never_a_root();
 case_home_unset();
 case_own_and_store();
 case_own_outside_is_not_usable();
 case_covers();
 case_env();
 case_inside_list();
 case_short_command();
 case_startup_listed();
 case_startup_users_own();
 case_startup_long();
 case_startup_nul();
 case_startup_at_the_limit();
 case_startup_rows();
 case_startup_program_scripts();
 case_elf_kinds();
 case_elf_not_elf();
 case_elf_cut_short();
 case_elf_wild_numbers();
 case_elf_bad_sections();
 case_embedded_listed();
 case_embedded_absent();
 case_operand_in();
 case_input();
 case_carrier();
 case_unplanned_refused();
 case_shell_positional();
 case_runs_code();
 case_folder_chose_program();
 case_carry();
 case_debug_dir();
 case_debug_dir_left_behind();
 case_encoders();
 case_rows();
 case_contain_lines();
 case_rule_apart();
 case_contain_text();
 case_contain_gdb_asks_again();
 case_gdb_apart_owner();
 case_gdb_long_folder();
 case_gdb_longest_name();
 case_pdb_wide_names();
 case_pdb_deepest_folder();
 case_gdb_folder_at_the_limit();
 case_long_path_refusals();
 case_allocation_failures();
 case_load_lines();
 case_load_pdbrc();
 case_load_pdbrc_longest_line();
 case_pdb_long_folder();
 case_jdb_lldb_deep_startup();
 case_contain_pdb_jdb_asks_again();
 teardown();
 if (failures) { printf("%d failure(s)\n", failures); return 1; }
 printf("test_startplan: all cases passed\n");
 return 0;
}
