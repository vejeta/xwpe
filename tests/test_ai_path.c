/* test_ai_path.c -- unit test for where a path really lands (we_ai_path.c).
 * Links we_ai_path.c alone; no editor -- always runs.
 *
 * A tree is made in a fresh temporary directory: a workspace, a sibling
 * directory beside it, and links of every kind between them.  Each case names
 * the path as a model could write it and says whether writing it would put
 * bytes outside the workspace: a link inside that leads out is outside, a link
 * that stays inside is not, a file made under a linked directory or through a
 * dangling link is judged by where the link leads, a loop is outside.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../we_ai_path.h"

#define PATH_ROOM        2048
#define LOOP_CHAIN       (WPE_PATH_MAX_HOPS + 5)
#define TEST_DIR_TEMPLATE "/tmp/xwpe-test-path-XXXXXX"
#define OUTSIDE 1
#define INSIDE  0

static int failures;
static char base[PATH_ROOM];     /* the temporary directory, resolved */
static char ws[PATH_ROOM];       /* the workspace: base/ws */
static char wsalias[PATH_ROOM];  /* the workspace named through base/wslink */
static char wsslash[PATH_ROOM];  /* the workspace written "base/ws/" */

static void fail(const char *name, const char *why)
{
 printf("FAIL %s: %s\n", name, why);
 failures++;
}

/* The absolute path "<base>/<rel>". */
static void under_base(char *dst, size_t sz, const char *rel)
{
 snprintf(dst, sz, "%s/%s", base, rel);
}

static void make_dir(const char *rel)
{
 char p[PATH_ROOM];
 under_base(p, sizeof p, rel);
 if (mkdir(p, 0700) != 0) { perror(p); exit(2); }
}

static void make_file(const char *rel)
{
 char p[PATH_ROOM];
 FILE *fp;
 under_base(p, sizeof p, rel);
 fp = fopen(p, "w");
 if (!fp) { perror(p); exit(2); }
 fputs("x\n", fp);
 fclose(fp);
}

/* A link at "<base>/<rel>" with @target as written (absolute or relative). */
static void make_link(const char *rel, const char *target)
{
 char p[PATH_ROOM];
 under_base(p, sizeof p, rel);
 if (symlink(target, p) != 0) { perror(p); exit(2); }
}

/* One case: @rel is the model's path below base, @want OUTSIDE or INSIDE,
   @real_rel the place it must resolve to below base, or NULL to not check. */
static void expect(const char *name, const char *rel, int want, const char *real_rel)
{
 char path[PATH_ROOM], real[PATH_ROOM], wantreal[PATH_ROOM];
 int got;
 under_base(path, sizeof path, rel);
 got = wpe_path_lands_outside(ws, path, real, sizeof real);
 if (got != want) { fail(name, want == OUTSIDE ? "judged inside, lands outside" : "judged outside, lands inside"); return; }
 if (!real_rel) return;
 under_base(wantreal, sizeof wantreal, real_rel);
 if (strcmp(real, wantreal) != 0) {
  char msg[2 * PATH_ROOM + 40];
  snprintf(msg, sizeof msg, "real %s, wanted %s", real, wantreal);
  fail(name, msg);
 }
}

static void build_tree(void)
{
 char tmpl[] = TEST_DIR_TEMPLATE, resolved[PATH_ROOM];
 char name[64], next[64];
 int i;
 if (!mkdtemp(tmpl)) { perror("mkdtemp"); exit(2); }
 /* /tmp may itself be reached through a link on some systems; the cases
    compare against the resolved place, like the product does. */
 if (wpe_path_resolve(tmpl, resolved, sizeof resolved) != WPE_PATH_RESOLVED) exit(2);
 snprintf(base, sizeof base, "%s", resolved);
 snprintf(ws, sizeof ws, "%s/ws", base);
 snprintf(wsalias, sizeof wsalias, "%s/wslink", base);
 snprintf(wsslash, sizeof wsslash, "%s/ws/", base);
 make_dir("ws"); make_dir("ws/sub"); make_dir("out"); make_dir("ws2");
 make_file("ws/inside.txt"); make_file("out/f.txt");
 snprintf(name, sizeof name, "%s/ws/sub", base);
 make_link("ws/inabs", name);                 /* absolute, stays inside  */
 make_link("ws/inrel", "sub");                /* relative, stays inside  */
 make_link("ws/vend", "../out");              /* directory leading out   */
 make_link("ws/dang", "../out/new.txt");      /* dangling, leads out     */
 make_link("ws/fileout", "../out/f.txt");     /* existing file outside   */
 make_link("ws/loopa", "loopb");
 make_link("ws/loopb", "loopa");
 make_link("wslink", "ws");                   /* the workspace by another name */
 make_link("ws/viahop", "vend");              /* a link to a link that leads out */
 for (i = 0; i < LOOP_CHAIN; i++) {           /* a chain longer than the bound   */
  snprintf(name, sizeof name, "ws/chain%d", i);
  snprintf(next, sizeof next, "chain%d", i + 1);
  make_link(name, i + 1 < LOOP_CHAIN ? next : "inside.txt");
 }
}

static void remove_tree(void)
{
 char p[PATH_ROOM];
 int i;
 static const char *links[] = { "ws/inabs", "ws/inrel", "ws/vend", "ws/dang",
   "ws/fileout", "ws/loopa", "ws/loopb", "wslink", "ws/viahop", NULL };
 static const char *files[] = { "ws/inside.txt", "out/f.txt", NULL };
 static const char *dirs[] = { "ws/sub", "ws", "out", "ws2", NULL };
 for (i = 0; links[i]; i++) { under_base(p, sizeof p, links[i]); unlink(p); }
 for (i = 0; i < LOOP_CHAIN; i++) {
  char rel[64];
  snprintf(rel, sizeof rel, "ws/chain%d", i);
  under_base(p, sizeof p, rel); unlink(p);
 }
 for (i = 0; files[i]; i++) { under_base(p, sizeof p, files[i]); unlink(p); }
 for (i = 0; dirs[i]; i++) { under_base(p, sizeof p, dirs[i]); rmdir(p); }
 rmdir(base);
}

static void run_cases(void)
{
 char path[PATH_ROOM];
 expect("plain file inside", "ws/inside.txt", INSIDE, "ws/inside.txt");
 expect("new file inside", "ws/new.txt", INSIDE, "ws/new.txt");
 expect("new file under a new directory inside", "ws/a/b/c.txt", INSIDE, "ws/a/b/c.txt");
 expect("link inside, absolute target", "ws/inabs/x.txt", INSIDE, "ws/sub/x.txt");
 expect("link inside, relative target", "ws/inrel/x.txt", INSIDE, "ws/sub/x.txt");
 expect("dots that stay inside", "ws/sub/../inside.txt", INSIDE, "ws/inside.txt");
 expect("the workspace by another name", "wslink/sub/x.txt", INSIDE, "ws/sub/x.txt");
 expect("file under a directory link leading out", "ws/vend/f.txt", OUTSIDE, "out/f.txt");
 expect("new file under a directory link leading out", "ws/vend/new.txt", OUTSIDE, "out/new.txt");
 expect("new directory and file under a link leading out", "ws/vend/d/new.txt", OUTSIDE, "out/d/new.txt");
 expect("dangling link as the last component", "ws/dang", OUTSIDE, "out/new.txt");
 expect("link to an existing file outside", "ws/fileout", OUTSIDE, "out/f.txt");
 expect("link to a link that leads out", "ws/viahop/f.txt", OUTSIDE, "out/f.txt");
 expect("dots after a link are folded after it", "ws/vend/../x.txt", OUTSIDE, "x.txt");
 expect("link loop", "ws/loopa", OUTSIDE, NULL);
 expect("more links than the bound", "ws/chain0", OUTSIDE, NULL);
 expect("plain path outside", "out/f.txt", OUTSIDE, "out/f.txt");
 expect("sibling that shares the root's name as a prefix", "ws2/x.txt", OUTSIDE, "ws2/x.txt");
 expect("climbing out with dots", "ws/../out/x.txt", OUTSIDE, "out/x.txt");

 under_base(path, sizeof path, "ws/sub/x.txt");
 if (wpe_path_lands_outside(wsalias, path, NULL, 0) != INSIDE)
  fail("root reached through a link", "a file in the tree judged outside");
 if (wpe_path_lands_outside(wsslash, path, NULL, 0) != INSIDE)
  fail("root with a trailing slash", "a file in the tree judged outside");
 under_base(path, sizeof path, "ws2/x.txt");
 if (wpe_path_lands_outside(wsslash, path, NULL, 0) != OUTSIDE)
  fail("root with a trailing slash, sibling", "a sibling judged inside");
 under_base(path, sizeof path, "ws/sub/x.txt");
 if (wpe_path_lands_outside("/", path, NULL, 0) != INSIDE) fail("root /", "nothing is outside /");
 if (wpe_path_lands_outside(NULL, path, NULL, 0) != INSIDE) fail("no root", "cannot be told, so inside");
 if (wpe_path_lands_outside(ws, NULL, NULL, 0) != INSIDE) fail("no path", "cannot be told, so inside");
 if (wpe_path_lands_outside("ws", path, NULL, 0) != INSIDE) fail("relative root", "cannot be told, so inside");
 if (wpe_path_lands_outside(ws, "ws/sub/x.txt", NULL, 0) != OUTSIDE) fail("relative path", "not followable, so outside");
}

int main(void)
{
 build_tree();
 run_cases();
 remove_tree();
 if (failures) { printf("%d failure(s)\n", failures); return 1; }
 printf("test_ai_path: all cases passed\n");
 return 0;
}
