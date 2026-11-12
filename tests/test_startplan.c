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

static int failures;
static char base[PATH_ROOM];     /* the temporary directory, resolved */
static char state[PATH_ROOM];    /* base/state: $XDG_STATE_HOME */
static char home[PATH_ROOM];     /* base/home: $HOME */
static char proj[PATH_ROOM];     /* base/proj: the project folder */
static char outside[PATH_ROOM];  /* base/outside: a folder beside it */
static char link_out[PATH_ROOM]; /* base/proj/out: a link to base/outside */
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
 make_dir(home);
 make_dir(proj);
 make_dir(outside);
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
 expect_has("with the condition gdb runs it under", p->npath && p->path[0].note ?
            p->path[0].note : "", "auto-load");
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
 wpe_startplan_set_cwd(p, proj);
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
 expect_has("with the condition gdb runs it under", p->npath && p->path[0].note ?
            p->path[0].note : "", "auto-load");
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
 unlink(link_out);
 rmdir(proj);
 rmdir(outside);
 rmdir(home);
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
 case_input();
 case_carrier();
 case_unplanned_refused();
 case_shell_positional();
 case_runs_code();
 case_carry();
 teardown();
 if (failures) { printf("%d failure(s)\n", failures); return 1; }
 printf("test_startplan: all cases passed\n");
 return 0;
}
