/* test_ai_scope.c -- unit test for the assistant's one scope (we_ai_path.c).
 * Links we_ai_path.c alone; no editor -- always runs.
 *
 * A tree is made in a fresh temporary directory: a workspace, folders beside
 * it, and links of every kind between them.  Each case names a path as a
 * model could write it and says where it lands for a read or a write: inside
 * the workspace, inside a folder the user granted for reads, or outside.  The
 * same tree checks that a grant never widens a write, that "/ws2" is not
 * inside "/ws", that ".." out of a grant leaves it, that a grant given
 * through a link covers the place the link leads, that the root "/" is never
 * a grant, and that a search's count
 * of links leading out and the opener of a written file follow links as the
 * scope does.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../we_ai_path.h"

#define PATH_ROOM         2048
#define TEST_DIR_TEMPLATE "/tmp/xwpe-test-scope-XXXXXX"
#define MAX_GRANTS        4

static int failures;
static char base[PATH_ROOM];   /* the temporary directory, resolved */
static char ws[PATH_ROOM];     /* the workspace: base/ws */

static void fail(const char *name, const char *why)
{
 printf("FAIL %s: %s\n", name, why);
 failures++;
}

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

static void make_link(const char *rel, const char *target)
{
 char p[PATH_ROOM];
 under_base(p, sizeof p, rel);
 if (symlink(target, p) != 0) { perror(p); exit(2); }
}

static const char *name_of(int where)
{
 if (where == WPE_SCOPE_INSIDE) return "inside";
 if (where == WPE_SCOPE_GRANTED) return "granted";
 return "outside";
}

/* One case: the model's path @rel below base, read or written, with the
   granted folders @g0 and @g1 (below base, or NULL), must land at @want. */
static void expect(const char *name, const char *rel, int access, const char *g0,
                   const char *g1, int want)
{
 char path[PATH_ROOM], grant0[PATH_ROOM], grant1[PATH_ROOM], msg[200];
 const char *grants[MAX_GRANTS];
 int n = 0, got;
 under_base(path, sizeof path, rel);
 if (g0) { under_base(grant0, sizeof grant0, g0); grants[n++] = grant0; }
 if (g1) { under_base(grant1, sizeof grant1, g1); grants[n++] = grant1; }
 got = wpe_scope_classify(ws, grants, n, path, access, NULL, 0);
 if (got == want) return;
 snprintf(msg, sizeof msg, "judged %s, wanted %s", name_of(got), name_of(want));
 fail(name, msg);
}

static void build_tree(void)
{
 char tmpl[] = TEST_DIR_TEMPLATE, resolved[PATH_ROOM], abs[PATH_ROOM];
 if (!mkdtemp(tmpl)) { perror("mkdtemp"); exit(2); }
 if (wpe_path_resolve(tmpl, resolved, sizeof resolved) != WPE_PATH_RESOLVED) exit(2);
 snprintf(base, sizeof base, "%s", resolved);
 snprintf(ws, sizeof ws, "%s/ws", base);
 make_dir("ws"); make_dir("ws/sub"); make_dir("ws2"); make_dir("out"); make_dir("out/deep");
 make_dir("inc"); make_dir("inc2"); make_dir("elsewhere");
 make_file("ws/inside.txt"); make_file("ws2/x.txt"); make_file("out/f.txt");
 make_file("inc/h.h"); make_file("inc2/h.h"); make_file("elsewhere/e.txt");
 make_file("out/deep/d.txt");
 make_link("ws/vendor", "../out");              /* a directory leading out   */
 make_link("ws/inlink", "sub");                 /* a directory staying inside */
 make_link("ws/fileout", "../out/f.txt");       /* an existing file outside  */
 make_link("ws/dang", "../out/new.txt");        /* dangling, leads out       */
 make_link("ws/sub/deepout", "../../out");      /* a link one level down     */
 make_link("ws/loopa", "loopb");
 make_link("ws/loopb", "loopa");
 make_link("grantlink", "out");                 /* a way to name out         */
 snprintf(abs, sizeof abs, "%s/inc", base);
 make_link("out/inner", abs);                   /* in out, leading elsewhere */
}

static void remove_tree(void)
{
 char p[PATH_ROOM];
 int i;
 static const char *links[] = { "ws/vendor", "ws/inlink", "ws/fileout", "ws/dang",
   "ws/sub/deepout", "ws/loopa", "ws/loopb", "grantlink", "out/inner", NULL };
 static const char *files[] = { "ws/inside.txt", "ws2/x.txt", "out/f.txt", "inc/h.h",
   "inc2/h.h", "elsewhere/e.txt", "out/deep/d.txt", "out/new.txt", "ws/written.txt",
   "ws/made.txt", NULL };
 static const char *dirs[] = { "ws/sub", "ws", "ws2", "out/deep", "out", "inc", "inc2",
   "elsewhere", NULL };
 for (i = 0; links[i]; i++) { under_base(p, sizeof p, links[i]); unlink(p); }
 for (i = 0; files[i]; i++) { under_base(p, sizeof p, files[i]); unlink(p); }
 for (i = 0; dirs[i]; i++) { under_base(p, sizeof p, dirs[i]); rmdir(p); }
 rmdir(base);
}

/* A grant of the root, or of a folder whose name leads to it, would hold every
   file: it must grant nothing, while a relative or empty grant is ignored. */
static void root_grant_cases(void)
{
 char path[PATH_ROOM], uplink[PATH_ROOM];
 const char *root_only[] = { "/" };
 const char *odd[] = { "", "out", NULL };
 under_base(path, sizeof path, "out/f.txt");
 if (wpe_scope_classify(ws, root_only, 1, path, WPE_SCOPE_READ, NULL, 0) != WPE_SCOPE_OUTSIDE)
  fail("a grant of /", "the root opened every file for reading");
 snprintf(uplink, sizeof uplink, "%s/../../../../../../../../../../..", base);
 root_only[0] = uplink;
 if (wpe_scope_classify(ws, root_only, 1, path, WPE_SCOPE_READ, NULL, 0) != WPE_SCOPE_OUTSIDE)
  fail("a grant climbing to /", "a grant that folds to the root opened every file");
 if (wpe_scope_classify(ws, odd, 3, path, WPE_SCOPE_READ, NULL, 0) != WPE_SCOPE_OUTSIDE)
  fail("empty, relative and missing grants", "a grant that is not an absolute folder opened a file");
}

static void classify_cases(void)
{
 char path[PATH_ROOM], real[PATH_ROOM], want[PATH_ROOM];
 expect("a file in the workspace, read", "ws/inside.txt", WPE_SCOPE_READ, NULL, NULL, WPE_SCOPE_INSIDE);
 expect("a file in the workspace, written", "ws/inside.txt", WPE_SCOPE_WRITE, NULL, NULL, WPE_SCOPE_INSIDE);
 expect("a link that stays inside", "ws/inlink/x.txt", WPE_SCOPE_READ, NULL, NULL, WPE_SCOPE_INSIDE);
 expect("a file outside, no grant, read", "out/f.txt", WPE_SCOPE_READ, NULL, NULL, WPE_SCOPE_OUTSIDE);
 expect("a file outside, no grant, written", "out/f.txt", WPE_SCOPE_WRITE, NULL, NULL, WPE_SCOPE_OUTSIDE);
 expect("a file below a granted folder, read", "out/f.txt", WPE_SCOPE_READ, "out", NULL, WPE_SCOPE_GRANTED);
 expect("the granted folder itself, read", "out", WPE_SCOPE_READ, "out", NULL, WPE_SCOPE_GRANTED);
 expect("a file below a folder deeper in a grant", "out/deep/d.txt", WPE_SCOPE_READ, "out", NULL, WPE_SCOPE_GRANTED);
 expect("a grant does not allow a write", "out/f.txt", WPE_SCOPE_WRITE, "out", NULL, WPE_SCOPE_OUTSIDE);
 expect("a grant does not allow a new file", "out/new.txt", WPE_SCOPE_WRITE, "out", NULL, WPE_SCOPE_OUTSIDE);
 expect("another folder is not granted", "elsewhere/e.txt", WPE_SCOPE_READ, "out", NULL, WPE_SCOPE_OUTSIDE);
 expect("the second of two grants", "elsewhere/e.txt", WPE_SCOPE_READ, "out", "elsewhere", WPE_SCOPE_GRANTED);
 expect("a link inside leading out, read, no grant", "ws/vendor/f.txt", WPE_SCOPE_READ, NULL, NULL, WPE_SCOPE_OUTSIDE);
 expect("a link inside leading out, read, granted", "ws/vendor/f.txt", WPE_SCOPE_READ, "out", NULL, WPE_SCOPE_GRANTED);
 expect("a link inside leading out, written, granted", "ws/vendor/f.txt", WPE_SCOPE_WRITE, "out", NULL, WPE_SCOPE_OUTSIDE);
 expect("a file link leading out, read", "ws/fileout", WPE_SCOPE_READ, NULL, NULL, WPE_SCOPE_OUTSIDE);
 expect("a dangling link leading out, read", "ws/dang", WPE_SCOPE_READ, NULL, NULL, WPE_SCOPE_OUTSIDE);
 expect("dots out of a grant leave it", "inc/../elsewhere/e.txt", WPE_SCOPE_READ, "inc", NULL, WPE_SCOPE_OUTSIDE);
 expect("a sibling sharing the workspace's name as a prefix", "ws2/x.txt", WPE_SCOPE_READ, NULL, NULL, WPE_SCOPE_OUTSIDE);
 expect("a sibling sharing a grant's name as a prefix", "inc2/h.h", WPE_SCOPE_READ, "inc", NULL, WPE_SCOPE_OUTSIDE);
 expect("a link loop", "ws/loopa", WPE_SCOPE_READ, NULL, NULL, WPE_SCOPE_OUTSIDE);
 expect("a grant given through a link covers where it leads", "out/f.txt", WPE_SCOPE_READ, "grantlink", NULL, WPE_SCOPE_GRANTED);
 expect("a link in a grant leading out of it is judged by where it leads", "out/inner/h.h", WPE_SCOPE_READ, "out", NULL, WPE_SCOPE_OUTSIDE);
 expect("a link in a grant leading to another grant", "out/inner/h.h", WPE_SCOPE_READ, "out", "inc", WPE_SCOPE_GRANTED);
 expect("a grant that cannot be followed grants nothing", "out/f.txt", WPE_SCOPE_READ, "ws/loopa", NULL, WPE_SCOPE_OUTSIDE);

 under_base(path, sizeof path, "ws/vendor/f.txt");
 wpe_scope_classify(ws, NULL, 0, path, WPE_SCOPE_READ, real, sizeof real);
 under_base(want, sizeof want, "out/f.txt");
 if (strcmp(real, want) != 0) fail("the real place", "a path through a link did not name where it lands");
 under_base(path, sizeof path, "ws/inside.txt");
 if (wpe_scope_classify("/", NULL, 0, path, WPE_SCOPE_READ, NULL, 0) != WPE_SCOPE_INSIDE)
  fail("root /", "nothing is outside /");
 if (wpe_scope_classify(NULL, NULL, 0, path, WPE_SCOPE_READ, NULL, 0) != WPE_SCOPE_INSIDE)
  fail("no root", "cannot be told, so inside");
 if (wpe_scope_classify(ws, NULL, 0, "ws/inside.txt", WPE_SCOPE_READ, NULL, 0) != WPE_SCOPE_OUTSIDE)
  fail("relative path", "not followable, so outside");
 root_grant_cases();
}

/* The links below ws that lead out or cannot be followed: vendor, fileout,
   dang, loopa and loopb among ws's own entries, and one level down
   sub/deepout.  inlink stays inside and is not counted. */
#define LINKS_ONE_LEVEL  5
#define LINKS_ALL_LEVELS 6

static void scan_cases(void)
{
 int truncated = -1;
 if (wpe_path_count_outward_links(ws, WPE_SCAN_ONE_LEVEL, &truncated) != LINKS_ONE_LEVEL)
  fail("links out, one level", "the count of links leading out of the folder's own entries is wrong");
 if (truncated != 0) fail("links out, one level", "a small tree reported a truncated look");
 if (wpe_path_count_outward_links(ws, WPE_SCAN_ALL_LEVELS, &truncated) != LINKS_ALL_LEVELS)
  fail("links out, all levels", "the count of links leading out of the tree is wrong");
 if (wpe_path_count_outward_links(NULL, WPE_SCAN_ALL_LEVELS, NULL) != 0)
  fail("links out, no folder", "no folder has no links");
}

/* The fd opened for @rel written through must be the file at @placed. */
static int opened_at_place(int fd, const char *placed_rel)
{
 char p[PATH_ROOM];
 struct stat a, b;
 under_base(p, sizeof p, placed_rel);
 if (fd < 0 || fstat(fd, &a) != 0 || stat(p, &b) != 0) return 0;
 return a.st_ino == b.st_ino && a.st_dev == b.st_dev;
}

static void open_cases(void)
{
 char p[PATH_ROOM];
 int fd;
 under_base(p, sizeof p, "ws/made.txt");
 fd = wpe_path_open_write(p);
 if (!opened_at_place(fd, "ws/made.txt")) fail("open a new file", "not created where named");
 if (fd >= 0) close(fd);
 under_base(p, sizeof p, "ws/dang");
 fd = wpe_path_open_write(p);
 if (!opened_at_place(fd, "out/new.txt")) fail("open through a dangling link", "not created where the link leads");
 if (fd >= 0) close(fd);
 under_base(p, sizeof p, "ws/fileout");
 fd = wpe_path_open_write(p);
 if (!opened_at_place(fd, "out/f.txt")) fail("open through a file link", "not opened where the link leads");
 if (fd >= 0) close(fd);
 under_base(p, sizeof p, "ws/loopa");
 errno = 0;
 fd = wpe_path_open_write(p);
 if (fd >= 0 || errno != ELOOP) fail("open through a loop", "a loop was opened or gave another error");
 if (fd >= 0) close(fd);
 errno = 0;
 if (wpe_path_open_write("") >= 0 || errno != ENOENT) fail("open no name", "an empty name was opened");
}

/* @scenario SC-ai-agent-91 */
int main(void)
{
 build_tree();
 classify_cases();
 scan_cases();
 open_cases();
 remove_tree();
 if (failures) { printf("%d failure(s)\n", failures); return 1; }
 printf("test_ai_scope: all cases passed\n");
 return 0;
}
