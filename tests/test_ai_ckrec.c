/* test_ai_ckrec.c -- unit test for the checkpoint's record (we_ai_ckrec.c).
 * Links we_ai_ckrec.c, we_read.c, we_seen.c, we_places.c and we_path.c;
 * always runs, on real folders the test makes.
 *
 * What goes wrong if the record breaks: a file of the user's that the run
 * renamed (alone or with its folder) is called new and `r` deletes the only
 * copy; a file the run removed or made unreadable is told as "unchanged"; a
 * folder nobody could read is told as "nothing there"; the review says
 * everything is back while part of the workspace was never seen.
 *
 * The first row is a table: every state a name can have at the checkpoint
 * (held with a copy, listed, asked and absent, not there in a folder read to
 * the end, in a folder the bound cut, in a folder that was refused, in a
 * folder nobody reads) against every state it can have afterwards, with the
 * verdict each pair must give, written from the design and not from the
 * owner.  The other rows are the cases the table cannot say: renames, order,
 * bounds, the two spellings of a path, the words.
 */
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "testdir.h"
#include "seen_rows.h"
#include "../we_ai_ckrec.h"

#define ROW_NO_VERDICT   (-1)
#define BOUND_SMALL      5
#define BOUND_BIG        10
#define FILES_MANY       8
#define NEW_FEW          3
#define NAME_MAX_TEST    TEST_DIR_PATH_SIZE

static int fails;
static int rows;
static char dir[TEST_DIR_PATH_SIZE];
static int folder_serial;
/* The folder the walk's test seam swaps for a link, once, just before the walk enters its subfolders. */
static char swap_w[NAME_MAX_TEST];
static char swap_to[NAME_MAX_TEST];

/* ---- the folder the rows work in ------------------------------------------ */

static void say_fail(const char *row, const char *fmt, ...)
{
 va_list ap;
 fails++;
 printf("FAIL: %s: ", row);
 va_start(ap, fmt);
 vprintf(fmt, ap);
 va_end(ap);
 printf("\n");
}

/** One folder per row, under the test's own folder, with a name no row shares. */
static void row_folder(char *out, size_t n)
{
 char name[32];
 snprintf(name, sizeof name, "w%d", ++folder_serial);
 if (test_path_join(out, n, dir, name) != 0) return;
 if (mkdir(out, 0700) != 0) say_fail("setup", "mkdir %s: %s", out, strerror(errno));
}

static void path_of(char *out, size_t n, const char *w, const char *rel)
{
 (void)test_path_join(out, n, w, rel);
}

/** A folder beside the row folders that a link can lead to: the place "outside" the workspace. */
static void outside_folder(char *out, size_t n)
{
 if (test_path_join(out, n, dir, "elsewhere") != 0) return;
 if (mkdir(out, 0700) != 0 && errno != EEXIST) say_fail("setup", "mkdir %s: %s", out, strerror(errno));
}

static void make_folder(const char *w, const char *rel)
{
 char p[NAME_MAX_TEST];
 path_of(p, sizeof p, w, rel);
 if (mkdir(p, 0700) != 0 && errno != EEXIST) say_fail("setup", "mkdir %s: %s", p, strerror(errno));
}

static void put_file(const char *w, const char *rel, const char *text)
{
 char p[NAME_MAX_TEST];
 FILE *fp;
 path_of(p, sizeof p, w, rel);
 fp = fopen(p, "w");
 if (!fp) { say_fail("setup", "fopen %s: %s", p, strerror(errno)); return; }
 fputs(text, fp);
 fclose(fp);
}

static void rename_to(const char *w, const char *from, const char *to)
{
 char a[NAME_MAX_TEST], b[NAME_MAX_TEST];
 path_of(a, sizeof a, w, from);
 path_of(b, sizeof b, w, to);
 if (rename(a, b) != 0) say_fail("setup", "rename %s: %s", a, strerror(errno));
}

static void remove_file(const char *w, const char *rel)
{
 char p[NAME_MAX_TEST];
 path_of(p, sizeof p, w, rel);
 if (unlink(p) != 0) say_fail("setup", "unlink %s: %s", p, strerror(errno));
}

static void link_file(const char *w, const char *from, const char *to)
{
 char a[NAME_MAX_TEST], b[NAME_MAX_TEST];
 path_of(a, sizeof a, w, from);
 path_of(b, sizeof b, w, to);
 if (link(a, b) != 0) say_fail("setup", "link %s: %s", a, strerror(errno));
}

static void symlink_folder(const char *w, const char *rel, const char *target)
{
 char p[NAME_MAX_TEST];
 path_of(p, sizeof p, w, rel);
 if (symlink(target, p) != 0) say_fail("setup", "symlink %s: %s", p, strerror(errno));
}

static void remove_entry_quiet(const char *w, const char *rel)
{
 char p[NAME_MAX_TEST];
 path_of(p, sizeof p, w, rel);
 (void)remove(p);
}

/** Replace a file by another object under its name: the new one is made first, so it cannot get the old one's number. */
static void replace_file(const char *w, const char *rel, const char *text)
{
 char tmp[NAME_MAX_TEST];
 snprintf(tmp, sizeof tmp, "%s.new", rel);
 put_file(w, tmp, text);
 rename_to(w, tmp, rel);
}

/* ---- the record ------------------------------------------------------------ */

static WpeCkRecord *new_record(int bound)
{
 WpeCkAsk ask;
 memset(&ask, 0, sizeof ask);
 ask.bound = bound;
 return wpe_ckrec_new(&ask);
}

/**
 * swap_folder_for_link - The test seam: put a link where the folder "d" was, once.
 *
 * A walk that is about to enter "d" meets a link instead and must refuse it.
 * The folder itself is kept as "d.real" so a row can put it back.
 */
static void swap_folder_for_link(void)
{
 char d[NAME_MAX_TEST], real[NAME_MAX_TEST];
 if (!swap_w[0]) return;
 path_of(d, sizeof d, swap_w, "d");
 path_of(real, sizeof real, swap_w, "d.real");
 if (rename(d, real) != 0 || symlink(swap_to, d) != 0) say_fail("setup", "swap of d: %s", strerror(errno));
 swap_w[0] = '\0';
}

static WpeCkRecord *new_record_with_swap(int bound, const char *w, const char *to)
{
 WpeCkAsk ask;
 memset(&ask, 0, sizeof ask);
 ask.bound = bound;
 ask.before_enter = swap_folder_for_link;
 snprintf(swap_w, sizeof swap_w, "%s", w);
 snprintf(swap_to, sizeof swap_to, "%s", to);
 return wpe_ckrec_new(&ask);
}

static void hold_image(WpeCkRecord *r, const char *w, const char *rel, int image)
{
 char p[NAME_MAX_TEST];
 WpeSeen seen;
 path_of(p, sizeof p, w, rel);
 if (wpe_seen_look_name(p, 0, &seen) != 0) { say_fail("setup", "look %s", p); return; }
 wpe_ckrec_then_image(r, p, image, &seen);
}

static void walk_side(WpeCkRecord *r, WpeCkSide side, const char *w)
{
 if (side == WPE_CK_NOW) wpe_ckrec_now_begin(r);
 wpe_ckrec_walk(r, side, w);
}

/** The verdict kind of one path, or ROW_NO_VERDICT. */
static int kind_at(WpeCkRecord *r, const char *w, const char *rel, WpeCkVerdict *v)
{
 char p[NAME_MAX_TEST];
 path_of(p, sizeof p, w, rel);
 if (!wpe_ckrec_verdict_of(r, p, v)) return ROW_NO_VERDICT;
 return (int)v->kind;
}

static int note_count(WpeCkRecord *r)
{
 const WpeCkNote *n;
 return wpe_ckrec_notes(r, WPE_CK_THEN, &n) + wpe_ckrec_notes(r, WPE_CK_NOW, &n);
}

static const char *kind_name(int kind)
{
 static const char *names[] = { "STAYED", "CREATED", "REMOVED", "REMOVED_NO_COPY", "MOVED", "MOVED_HIDDEN", "LINKED",
                                "MAYBE_COPY", "REPLACED", "NOT_READABLE_NOW", "CANNOT_SAY" };
 if (kind == ROW_NO_VERDICT) return "none";
 return names[kind];
}

static void expect_kind(const char *row, WpeCkRecord *r, const char *w, const char *rel, int want)
{
 WpeCkVerdict v;
 int got = kind_at(r, w, rel, &v);
 rows++;
 if (got != want) say_fail(row, "%s is %s, wanted %s", rel, kind_name(got), kind_name(want));
}

static void expect_why(const char *row, WpeCkRecord *r, const char *w, const char *rel, WpeCkWhy want)
{
 WpeCkVerdict v;
 rows++;
 if (kind_at(r, w, rel, &v) != (int)WPE_CK_CANNOT_SAY) { say_fail(row, "%s is not CANNOT_SAY", rel); return; }
 if (v.why != want) say_fail(row, "%s cannot be said for why %d, wanted %d", rel, (int)v.why, (int)want);
}

static void expect_count(const char *row, const char *what, int got, int want)
{
 rows++;
 if (got != want) say_fail(row, "%s: %d, wanted %d", what, got, want);
}

/* ---- row 1: the table ------------------------------------------------------- */

/** The states of a name at the checkpoint. */
enum { T_HELD, T_LISTED, T_ASKED_ABSENT, T_NOT_THERE, T_FOLDER_CUT, T_FOLDER_REFUSED, T_FOLDER_UNREAD };
/** The states it can have afterwards. */
enum { N_SAME, N_REPLACED, N_ABSENT };

typedef struct { int then, now, want; } TableRow;

/* What each pair must give, from the design (section 2.3), not from the owner. */
static const TableRow table[] = {
 { T_HELD,           N_SAME,     WPE_CK_STAYED },
 { T_HELD,           N_REPLACED, WPE_CK_STAYED },
 { T_HELD,           N_ABSENT,   WPE_CK_REMOVED },
 { T_LISTED,         N_SAME,     ROW_NO_VERDICT },
 { T_LISTED,         N_REPLACED, WPE_CK_REPLACED },
 { T_LISTED,         N_ABSENT,   WPE_CK_REMOVED_NO_COPY },
 { T_ASKED_ABSENT,   N_SAME,     WPE_CK_CREATED },
 { T_ASKED_ABSENT,   N_ABSENT,   ROW_NO_VERDICT },
 { T_NOT_THERE,      N_SAME,     WPE_CK_CREATED },
 { T_NOT_THERE,      N_ABSENT,   ROW_NO_VERDICT },
 { T_FOLDER_CUT,     N_SAME,     WPE_CK_CANNOT_SAY },
 { T_FOLDER_REFUSED, N_SAME,     WPE_CK_CANNOT_SAY },
 { T_FOLDER_UNREAD,  N_SAME,     WPE_CK_CANNOT_SAY },
};

/** The reason a name cannot be said, for the states that cannot. */
static WpeCkWhy table_why(int then)
{
 if (then == T_FOLDER_CUT) return WPE_CK_WHY_BOUND;
 if (then == T_FOLDER_REFUSED) return WPE_CK_WHY_REFUSED;
 return WPE_CK_WHY_NOT_LISTED;
}

/** The checkpoint's side of a table row: the folder "d", and the name "d/x" in the state the row names. */
static void table_make_then(WpeCkRecord *r, const char *w, int then)
{
 WpeReadOutcome gone;
 make_folder(w, "d");
 wpe_read_outcome_errno(&gone, ENOENT);
 switch (then) {
 case T_HELD:
  put_file(w, "d/x", "A\n");
  walk_side(r, WPE_CK_THEN, w);
  hold_image(r, w, "d/x", 0);
  return;
 case T_LISTED:
  put_file(w, "d/x", "A\n");
  break;
 case T_ASKED_ABSENT: {
  char p[NAME_MAX_TEST];
  path_of(p, sizeof p, w, "d/x");
  wpe_ckrec_then_asked(r, p, &gone);
  break;
 }
 case T_NOT_THERE:
  break;
 case T_FOLDER_CUT:
  put_file(w, "d/f1", "1\n"); put_file(w, "d/f2", "2\n"); put_file(w, "d/f3", "3\n");
  put_file(w, "d/f4", "4\n"); put_file(w, "d/f5", "5\n"); put_file(w, "d/f6", "6\n");
  break;
 case T_FOLDER_REFUSED:
 case T_FOLDER_UNREAD:
  break;
 }
 walk_side(r, WPE_CK_THEN, w);
 if (then == T_FOLDER_UNREAD) {
  char p[NAME_MAX_TEST];
  path_of(p, sizeof p, w, "d");
  wpe_ckrec_unread(r, WPE_CK_THEN, p);
 }
}

/** Bring the workspace to a state for the "now" side. */
static void table_make_now(const char *w, int then, int now)
{
 if (now == N_ABSENT) {
  if (then == T_HELD || then == T_LISTED) remove_file(w, "d/x");
  return;
 }
 if (then == T_FOLDER_REFUSED) {
  remove_entry_quiet(w, "d");
  rename_to(w, "d.real", "d");
 }
 if (now == N_REPLACED) { replace_file(w, "d/x", "B-is-longer-than-A\n"); return; }
 if (then == T_HELD || then == T_LISTED) return;
 put_file(w, "d/x", "X\n");
}

/** The "now" side of a table row: a walk, or what a tool lists where a walk is not allowed to go. */
static void table_fill_now(WpeCkRecord *r, const char *w, int then)
{
 char p[NAME_MAX_TEST], root[NAME_MAX_TEST];
 if (then != T_FOLDER_CUT && then != T_FOLDER_REFUSED && then != T_FOLDER_UNREAD) {
  walk_side(r, WPE_CK_NOW, w);
  return;
 }
 wpe_ckrec_now_begin(r);
 snprintf(root, sizeof root, "%s", w);
 wpe_ckrec_tool_begin(r, WPE_CK_NOW, root);
 path_of(p, sizeof p, w, "d/x");
 wpe_ckrec_listed(r, WPE_CK_NOW, p);
 wpe_ckrec_complete(r, WPE_CK_NOW, root);
}

static void row_table(void)
{
 size_t i;
 for (i = 0; i < sizeof table / sizeof table[0]; i++) {
  char w[NAME_MAX_TEST], outside[NAME_MAX_TEST], row[80];
  const TableRow *t = &table[i];
  WpeCkRecord *r;
  row_folder(w, sizeof w);
  outside_folder(outside, sizeof outside);
  r = t->then == T_FOLDER_REFUSED ? new_record_with_swap(BOUND_SMALL, w, outside) : new_record(BOUND_SMALL);
  snprintf(row, sizeof row, "table then=%d now=%d", t->then, t->now);
  table_make_then(r, w, t->then);
  table_make_now(w, t->then, t->now);
  table_fill_now(r, w, t->then);
  expect_kind(row, r, w, "d/x", t->want);
  if (t->want == WPE_CK_CANNOT_SAY) expect_why(row, r, w, "d/x", table_why(t->then));
  wpe_ckrec_free(r);
 }
}

/** A folder nobody read is a note when the walk is the one that was refused or cut. */
static void row_table_notes(void)
{
 char w[NAME_MAX_TEST], outside[NAME_MAX_TEST];
 WpeCkRecord *r;
 WpeCkVerdict v;
 outside_folder(outside, sizeof outside);
 row_folder(w, sizeof w);
 r = new_record_with_swap(BOUND_SMALL, w, outside);
 table_make_then(r, w, T_FOLDER_REFUSED);
 table_make_now(w, T_FOLDER_REFUSED, N_SAME);
 walk_side(r, WPE_CK_NOW, w);
 expect_count("refused folder", "notes", note_count(r), 1);
 expect_count("refused folder", "whole", wpe_ckrec_whole(r), 0);
 expect_count("refused folder", "no verdict for what nobody read", kind_at(r, w, "d/x", &v), ROW_NO_VERDICT);
 wpe_ckrec_free(r);
}

/* ---- row 2: a rename is not a creation -------------------------------------- */

static void row_renames(void)
{
 char w[NAME_MAX_TEST];
 WpeCkRecord *r = new_record(BOUND_BIG);
 WpeCkVerdict v;
 row_folder(w, sizeof w);
 make_folder(w, "d");
 put_file(w, "d/f.c", "USER DATA\n");
 put_file(w, "n.txt", "NOTES\n");
 walk_side(r, WPE_CK_THEN, w);
 rename_to(w, "d", "e");
 rename_to(w, "n.txt", "m.txt");
 put_file(w, "e/new.c", "NEW\n");
 walk_side(r, WPE_CK_NOW, w);
 expect_kind("renamed file", r, w, "m.txt", WPE_CK_MOVED);
 expect_kind("renamed folder", r, w, "e", WPE_CK_MOVED);
 expect_kind("file in a renamed folder", r, w, "e/f.c", WPE_CK_MOVED);
 rows++;
 if (kind_at(r, w, "e/f.c", &v) == WPE_CK_MOVED && !v.under_folder) say_fail("file in a renamed folder", "the folder's move does not act for it");
 expect_kind("file made in a renamed folder", r, w, "e/new.c", WPE_CK_CREATED);
 expect_kind("the old name of a moved file", r, w, "n.txt", ROW_NO_VERDICT);
 wpe_ckrec_free(r);
}

static void row_second_name(void)
{
 char w[NAME_MAX_TEST], p[NAME_MAX_TEST];
 WpeCkRecord *r = new_record(BOUND_BIG);
 WpeCkVerdict v;
 row_folder(w, sizeof w);
 put_file(w, "a", "SAME BYTES\n");
 walk_side(r, WPE_CK_THEN, w);
 link_file(w, "a", "b");
 walk_side(r, WPE_CK_NOW, w);
 expect_kind("a second name", r, w, "b", WPE_CK_LINKED);
 expect_kind("the first name", r, w, "a", ROW_NO_VERDICT);
 path_of(p, sizeof p, w, "b");
 rows++;
 if (wpe_ckrec_may_remove(r, p, &v)) say_fail("a second name", "may_remove said yes for a name of an object the checkpoint saw");
 wpe_ckrec_free(r);
}

static void row_replaced_then_moved(void)
{
 char w[NAME_MAX_TEST];
 WpeCkRecord *r = new_record(BOUND_BIG);
 WpeCkVerdict v;
 row_folder(w, sizeof w);
 make_folder(w, "s");
 put_file(w, "s/a.c", "OLD A\n");
 walk_side(r, WPE_CK_THEN, w);
 rename_to(w, "s/a.c", "s/b.c");
 put_file(w, "s/a.c", "BRAND NEW CONTENT\n");
 walk_side(r, WPE_CK_NOW, w);
 expect_kind("moved away", r, w, "s/b.c", WPE_CK_MOVED);
 expect_kind("new file under the old name", r, w, "s/a.c", WPE_CK_REPLACED);
 rows++;
 if (kind_at(r, w, "s/a.c", &v) == WPE_CK_REPLACED && v.object != WPE_CK_CREATED)
  say_fail("new file under the old name", "the object now at the name is not told: %d", (int)v.object);
 wpe_ckrec_free(r);
}

/* ---- row 3: nothing depends on the order the system listed ------------------- */

/** One workspace made in the order of @perm, judged, as one string. */
static void order_text(const int *perm, char *out, size_t n)
{
 static const char *names[] = { "a.c", "b.c", "c.c", "d.c" };
 char w[NAME_MAX_TEST];
 WpeCkRecord *r = new_record(BOUND_BIG);
 const WpeCkVerdict *all;
 int i, nv;
 row_folder(w, sizeof w);
 for (i = 0; i < 4; i++) put_file(w, names[perm[i]], "then\n");
 walk_side(r, WPE_CK_THEN, w);
 /* what happens to each name does not depend on the order it was made in */
 replace_file(w, "a.c", "now and longer\n");
 remove_file(w, "b.c");
 replace_file(w, "c.c", "now and longer\n");
 remove_file(w, "d.c");
 for (i = 0; i < 4; i++) {
  char f[16];
  if (perm[i] == 3) continue;
  snprintf(f, sizeof f, "n%d.c", perm[i]);
  put_file(w, f, "new\n");
 }
 walk_side(r, WPE_CK_NOW, w);
 nv = wpe_ckrec_judge_all(r, &all);
 out[0] = '\0';
 for (i = 0; i < nv; i++) {
  size_t len = strlen(out);
  snprintf(out + len, n - len, "%s=%s;", strrchr(all[i].path, '/') + 1, kind_name((int)all[i].kind));
 }
 wpe_ckrec_free(r);
}

static void row_order(void)
{
 static const int p1[] = { 0, 1, 2, 3 }, p2[] = { 3, 2, 1, 0 }, p3[] = { 2, 0, 3, 1 };
 char a[1024], b[1024], c[1024];
 order_text(p1, a, sizeof a);
 order_text(p2, b, sizeof b);
 order_text(p3, c, sizeof c);
 rows++;
 if (strcmp(a, b) != 0 || strcmp(a, c) != 0)
  say_fail("order", "the verdicts depend on the creation order:\n  %s\n  %s\n  %s", a, b, c);
}

/* ---- row 4 and 5: the bound ------------------------------------------------- */

static void many_files(const char *w, const char *prefix, int n)
{
 int i;
 for (i = 0; i < n; i++) {
  char f[64];
  snprintf(f, sizeof f, "%s%02d", prefix, i);
  put_file(w, f, "x\n");
 }
}

static void row_then_bound(void)
{
 char w[NAME_MAX_TEST];
 WpeCkRecord *r = new_record(BOUND_SMALL);
 WpeCkVerdict v;
 const WpeCkNote *notes;
 row_folder(w, sizeof w);
 many_files(w, "k", FILES_MANY);
 walk_side(r, WPE_CK_THEN, w);
 wpe_ckrec_now_begin(r);
 {
  char p[NAME_MAX_TEST];
  wpe_ckrec_tool_begin(r, WPE_CK_NOW, w);
  path_of(p, sizeof p, w, "late");
  put_file(w, "late", "L\n");
  wpe_ckrec_listed(r, WPE_CK_NOW, p);
  wpe_ckrec_complete(r, WPE_CK_NOW, w);
 }
 expect_kind("a new name in a folder the bound cut", r, w, "late", WPE_CK_CANNOT_SAY);
 expect_why("a new name in a folder the bound cut", r, w, "late", WPE_CK_WHY_BOUND);
 expect_count("the bound", "then notes", wpe_ckrec_notes(r, WPE_CK_THEN, &notes), 1);
 rows++;
 if (notes && notes[0].why != WPE_CK_WHY_BOUND) say_fail("the bound", "the note is not the bound's");
 rows++;
 if (kind_at(r, w, "k00", &v) == WPE_CK_CANNOT_SAY) say_fail("the bound", "a name the walk never met is judged");
 wpe_ckrec_free(r);
}

static void row_now_bound_counts_only_new_names(void)
{
 char w[NAME_MAX_TEST];
 WpeCkRecord *r = new_record(BOUND_BIG);
 const WpeCkNote *notes;
 int i, created = 0;
 row_folder(w, sizeof w);
 many_files(w, "k", FILES_MANY);
 walk_side(r, WPE_CK_THEN, w);
 many_files(w, "n", BOUND_BIG - 1);
 walk_side(r, WPE_CK_NOW, w);
 for (i = 0; i < BOUND_BIG - 1; i++) {
  char f[64];
  WpeCkVerdict v;
  snprintf(f, sizeof f, "n%02d", i);
  if (kind_at(r, w, f, &v) == WPE_CK_CREATED) created++;
 }
 expect_count("the now bound", "new names found beside known ones", created, BOUND_BIG - 1);
 expect_count("the now bound", "now notes", wpe_ckrec_notes(r, WPE_CK_NOW, &notes), 0);
 many_files(w, "z", NEW_FEW + 1);
 walk_side(r, WPE_CK_NOW, w);
 expect_count("the now bound", "now notes when new names pass the bound", wpe_ckrec_notes(r, WPE_CK_NOW, &notes), 1);
 rows++;
 if (notes && notes[0].why != WPE_CK_WHY_BOUND) say_fail("the now bound", "the note is not the bound's");
 expect_count("the now bound", "whole after a bound", wpe_ckrec_whole(r), 0);
 wpe_ckrec_free(r);
}

/** A removed name in a folder read to the end is "cannot say" when the review's bound cut another place. */
static void row_removed_beside_a_cut_place(void)
{
 char w[NAME_MAX_TEST];
 WpeCkRecord *r = new_record(BOUND_BIG);
 row_folder(w, sizeof w);
 make_folder(w, "a");
 put_file(w, "gone", "G\n");
 walk_side(r, WPE_CK_THEN, w);
 remove_file(w, "gone");
 {
  int i;
  for (i = 0; i < BOUND_BIG + 1; i++) {
   char f[64];
   snprintf(f, sizeof f, "a/n%02d", i);
   put_file(w, f, "n\n");
  }
 }
 walk_side(r, WPE_CK_NOW, w);
 expect_kind("removed beside a cut place", r, w, "gone", WPE_CK_CANNOT_SAY);
 expect_why("removed beside a cut place", r, w, "gone", WPE_CK_WHY_BOUND);
 wpe_ckrec_free(r);
}

/**
 * clear_seen_hook - Stop playing a system whose birth time proves nothing, or only goes back.
 *
 * What the user gets: the rows after this one judge identity on the file
 * system they really run on.  The hook is a lie told to every look made while
 * it is set, so a row that sets it clears it on every way out of itself, and
 * the clearing lives here in one place instead of one copy per path.
 */
static void clear_seen_hook(void)
{
 unsetenv(WPE_SEEN_TEST_FAIL_ENV);
}

/**
 * play_back_only_system - Play FreeBSD: the owner can move a birth back and no call moves it forward.
 *
 * Every look made until clear_seen_hook reads its statx birth as back-only,
 * which is how the real system's stat reports it there.
 */
static void play_back_only_system(void)
{
 setenv(WPE_SEEN_TEST_FAIL_ENV, WPE_SEEN_TEST_FAIL_BIRTH_BACK_ONLY, 1);
}

/**
 * row_claimed_removal_under_a_cut - A removal a new name claims, in a review the bound cut.
 *
 * What the user gets: an answer for the file the run removed, even when a file
 * the review found looks like it.  A system that hands a removed file's number
 * back to the next file made, and whose birth time proves nothing (the three
 * BSDs and macOS, and any file system that keeps no birth), lets the new name
 * claim the removed one.  The review was cut, so nobody can tell a move from a
 * number that came back: the removed name reads CANNOT_SAY with the bound's
 * cause instead of going silent, and the new name still reads MOVED.  The row
 * plays that system with the identity owner's own hook, and skips by name on a
 * file system that never gives a number back.
 */
static void row_claimed_removal_under_a_cut(void)
{
 char w[NAME_MAX_TEST], p[NAME_MAX_TEST], f[64];
 WpeCkRecord *r = new_record(BOUND_BIG);
 struct stat removed, taken;
 const char *row = "a claimed removal under a cut";
 int i, reused;
 row_folder(w, sizeof w);
 make_folder(w, "a");
 put_file(w, "gone", "GONE\n");
 path_of(p, sizeof p, w, "gone");
 if (stat(p, &removed) != 0) {
  say_fail(row, "stat gone: %s", strerror(errno));
  clear_seen_hook();
  wpe_ckrec_free(r);
  return;
 }
 setenv(WPE_SEEN_TEST_FAIL_ENV, WPE_SEEN_TEST_FAIL_NO_BIRTH, 1);
 walk_side(r, WPE_CK_THEN, w);
 remove_file(w, "gone");
 /* the next file made is the one a recycling system gives the number to, and
    it sits in a folder the walk reads to its end, so the cut below never hides it */
 put_file(w, "a/taken", "T\n");
 path_of(p, sizeof p, w, "a/taken");
 if (stat(p, &taken) != 0) {
  say_fail(row, "stat a/taken: %s", strerror(errno));
  clear_seen_hook();
  wpe_ckrec_free(r);
  return;
 }
 reused = removed.st_dev == taken.st_dev && removed.st_ino == taken.st_ino;
 if (!test_observation(reused, row, "this folder's file system never gives a removed file's number back")) {
  clear_seen_hook();
  wpe_ckrec_free(r);
  return;
 }
 make_folder(w, "a/deep");
 for (i = 0; i < BOUND_BIG + 1; i++) {
  snprintf(f, sizeof f, "a/deep/n%02d", i);
  put_file(w, f, "n\n");
 }
 walk_side(r, WPE_CK_NOW, w);
 clear_seen_hook();
 expect_kind(row, r, w, "gone", WPE_CK_CANNOT_SAY);
 expect_why(row, r, w, "gone", WPE_CK_WHY_BOUND);
 expect_kind(row, r, w, "a/taken", WPE_CK_MOVED);
 wpe_ckrec_free(r);
}

/**
 * later_birth_possible - Can a row about a birth that goes later run on this system and folder?
 * @w: the row's folder; @row: the name a skip carries.
 *
 * What the user gets: a row that plays FreeBSD says by name where it cannot
 * (macOS, a build with no birth, a file system with none) instead of failing
 * for a reason that is the system's.
 * Return: 1 when the row can run.
 */
static int later_birth_possible(const char *w, const char *row)
{
 if (!test_observation(seen_rows_birth_back_only_route(), row, "a birth time can go either way on this system"))
  return 0;
 return test_observation(seen_rows_birth_fact(w) != SEEN_BIRTH_ABSENT, row, "this folder's file system keeps no birth time");
}

/** Did the file system give the removed object's device and number to the object made after it? */
static int number_came_back(const struct stat *removed, const struct stat *made)
{
 return removed->st_dev == made->st_dev && removed->st_ino == made->st_ino;
}

/** Is the birth of the second look later than the first's? */
static int birth_is_later(const WpeSeen *first, const WpeSeen *second)
{
 if (second->seen_birth.tv_sec != first->seen_birth.tv_sec)
  return second->seen_birth.tv_sec > first->seen_birth.tv_sec;
 return second->seen_birth.tv_nsec > first->seen_birth.tv_nsec;
}

/**
 * later_birth_reached - Did the row make the two objects it is about: one number, a later birth?
 * @removed/@made: what stat said of each; @first/@second: what the identity owner looked at.
 *
 * Two births on one tick of the file system's clock prove nothing in either
 * direction, so that is a skip by name and not a failure.
 * Return: 1 when the row may go on.
 */
static int later_birth_reached(const char *row, const struct stat *removed, const struct stat *made,
                               const WpeSeen *first, const WpeSeen *second)
{
 if (!test_observation(number_came_back(removed, made), row, "this folder's file system never gives a removed file's number back"))
  return 0;
 return test_observation(birth_is_later(first, second), row, "the two births fall on one tick of the clock here");
}

/**
 * removal_beside_a_later_birth - Remove a file, make another that takes its number, on a system where a birth only goes back.
 * @row: the row's name; @made_text: the text of the file made after the removal; @made_want: the verdict it must get.
 *
 * What the user gets: a file the run removed is told as removed even when a
 * file the run made afterwards took its number.  FreeBSD hands the number
 * back and cannot prove the two objects apart by equality, but no call moves
 * a birth forward, so the newer birth proves another object.  The row plays
 * that system with the identity owner's own hook.  The text of the new file
 * decides whether it can be a copy: another size is CREATED, the same size is
 * MAYBE_COPY (kept), and the removed name reads REMOVED_NO_COPY either way.
 */
static void removal_beside_a_later_birth(const char *row, const char *made_text, int made_want)
{
 char w[NAME_MAX_TEST], p[NAME_MAX_TEST];
 WpeCkRecord *r = new_record(BOUND_BIG);
 struct stat removed, made;
 WpeSeen first, second;
 row_folder(w, sizeof w);
 make_folder(w, "a");
 put_file(w, "gone", "GONE\n");
 if (!later_birth_possible(w, row)) {
  wpe_ckrec_free(r);
  return;
 }
 play_back_only_system();
 path_of(p, sizeof p, w, "gone");
 if (stat(p, &removed) != 0 || wpe_seen_look_name(p, 0, &first) != 0) {
  say_fail(row, "look at gone: %s", strerror(errno));
  clear_seen_hook();
  wpe_ckrec_free(r);
  return;
 }
 walk_side(r, WPE_CK_THEN, w);
 remove_file(w, "gone");
 put_file(w, "a/taken", made_text);
 path_of(p, sizeof p, w, "a/taken");
 if (stat(p, &made) != 0 || wpe_seen_look_name(p, 0, &second) != 0) {
  say_fail(row, "look at a/taken: %s", strerror(errno));
  clear_seen_hook();
  wpe_ckrec_free(r);
  return;
 }
 if (!later_birth_reached(row, &removed, &made, &first, &second)) {
  clear_seen_hook();
  wpe_ckrec_free(r);
  return;
 }
 walk_side(r, WPE_CK_NOW, w);
 clear_seen_hook();
 expect_kind(row, r, w, "gone", WPE_CK_REMOVED_NO_COPY);
 expect_kind(row, r, w, "a/taken", made_want);
 wpe_ckrec_free(r);
}

/** A removal beside a later birth, the new file another size: the removed file is told, the new one is new. */
static void row_removed_beside_a_later_birth(void)
{
 removal_beside_a_later_birth("a removal beside a later birth", "A LONGER TEXT THAN GONE\n", WPE_CK_CREATED);
}

/** The same with a file as large as the removed one: it may be a copy, so r keeps it. */
static void row_removed_beside_a_later_birth_same_size(void)
{
 removal_beside_a_later_birth("a removal beside a later birth, same size", "GONE\n", WPE_CK_MAYBE_COPY);
}

/**
 * row_removed_folder_beside_a_later_birth - A removed folder, and a folder that took its number.
 *
 * What the user gets: the same answer for a folder as for a file.  A file in a
 * folder the run removed is told as removed, and a file in the folder the run
 * made after it is new, instead of the new folder reading as the old one moved
 * (which says MOVED for a file that is new, and nothing for the one removed).
 */
static void row_removed_folder_beside_a_later_birth(void)
{
 char w[NAME_MAX_TEST], p[NAME_MAX_TEST];
 WpeCkRecord *r = new_record(BOUND_BIG);
 const char *row = "a removed folder beside a later birth";
 struct stat removed, made;
 WpeSeen first, second;
 row_folder(w, sizeof w);
 make_folder(w, "d");
 put_file(w, "d/x", "X\n");
 if (!later_birth_possible(w, row)) {
  wpe_ckrec_free(r);
  return;
 }
 play_back_only_system();
 path_of(p, sizeof p, w, "d");
 if (stat(p, &removed) != 0 || wpe_seen_look_name(p, 0, &first) != 0) {
  say_fail(row, "look at d: %s", strerror(errno));
  clear_seen_hook();
  wpe_ckrec_free(r);
  return;
 }
 walk_side(r, WPE_CK_THEN, w);
 remove_file(w, "d/x");
 remove_entry_quiet(w, "d");
 make_folder(w, "e");
 put_file(w, "e/x", "NEW\n");
 path_of(p, sizeof p, w, "e");
 if (stat(p, &made) != 0 || wpe_seen_look_name(p, 0, &second) != 0) {
  say_fail(row, "look at e: %s", strerror(errno));
  clear_seen_hook();
  wpe_ckrec_free(r);
  return;
 }
 if (!later_birth_reached(row, &removed, &made, &first, &second)) {
  clear_seen_hook();
  wpe_ckrec_free(r);
  return;
 }
 walk_side(r, WPE_CK_NOW, w);
 clear_seen_hook();
 expect_kind(row, r, w, "d/x", WPE_CK_REMOVED_NO_COPY);
 expect_kind(row, r, w, "e/x", WPE_CK_CREATED);
 wpe_ckrec_free(r);
}

/**
 * row_a_rename_on_a_back_only_birth - A rename keeps its birth, so it is still a move on FreeBSD.
 *
 * What the user gets: the guard against a question that is too eager.  A file
 * the run renamed keeps its birth time, so on a system where a birth only goes
 * back the two looks agree and the new name is still MOVED, with no row of its
 * own for the old name.  Green before the fix and after: it pins that the new
 * question never turns a real move into a removal.
 */
static void row_a_rename_on_a_back_only_birth(void)
{
 char w[NAME_MAX_TEST];
 WpeCkRecord *r = new_record(BOUND_BIG);
 const char *row = "a rename on a back-only birth";
 row_folder(w, sizeof w);
 put_file(w, "a.c", "USER\n");
 if (!later_birth_possible(w, row)) {
  wpe_ckrec_free(r);
  return;
 }
 play_back_only_system();
 walk_side(r, WPE_CK_THEN, w);
 rename_to(w, "a.c", "b.c");
 walk_side(r, WPE_CK_NOW, w);
 clear_seen_hook();
 expect_kind(row, r, w, "b.c", WPE_CK_MOVED);
 expect_kind(row, r, w, "a.c", ROW_NO_VERDICT);
 wpe_ckrec_free(r);
}

/**
 * row_order_on_a_back_only_system - The verdicts do not depend on the order the files were made, on FreeBSD's rules.
 *
 * What the user gets: the same answer whichever of the new files the file
 * system gave a removed file's number to.  This is the red a FreeBSD run
 * reported; the run here plays that system's birth rule and so guards the
 * reported symptom on every Linux run, not only the synthetic row.
 */
/* What row_order's run is: two names replaced, two removed with no copy and
   three created.  Equal answers in every order are not enough on their own:
   the three orders can agree on a wrong story, two removed names silent. */
#define ORDER_TRUE_STORY \
 "a.c=REPLACED;b.c=REMOVED_NO_COPY;c.c=REPLACED;d.c=REMOVED_NO_COPY;" \
 "n0.c=CREATED;n1.c=CREATED;n2.c=CREATED;"

static void row_order_on_a_back_only_system(void)
{
 static const int first[] = { 0, 1, 2, 3 };
 char story[1024];
 play_back_only_system();
 row_order();
 order_text(first, story, sizeof story);
 rows++;
 if (strcmp(story, ORDER_TRUE_STORY) != 0)
  say_fail("order on a back-only system", "the run's story is not the true one:\n  got  %s\n  want %s",
           story, ORDER_TRUE_STORY);
 clear_seen_hook();
}

/**
 * row_replaced_names_its_source - A name another file was moved over names where that file came from.
 *
 * What the user gets: when the run moves b.c over a c.c that was already
 * there, the sentence for c.c names b.c, the file the checkpoint listed.  The
 * claim lets b.c go without a row of its own, so its name must be said
 * somewhere; saying "another file" left the user not knowing b.c had gone.
 */
static void row_replaced_names_its_source(void)
{
 char w[NAME_MAX_TEST], p[NAME_MAX_TEST], line[WPE_CK_SENTENCE_MAX];
 WpeCkRecord *r = new_record(BOUND_BIG);
 const char *row = "a replaced name names its source";
 WpeCkVerdict v;
 row_folder(w, sizeof w);
 put_file(w, "b.c", "BEE\n");
 put_file(w, "c.c", "SEA, THE OLD ONE\n");
 walk_side(r, WPE_CK_THEN, w);
 rename_to(w, "b.c", "c.c");
 walk_side(r, WPE_CK_NOW, w);
 expect_kind(row, r, w, "c.c", WPE_CK_REPLACED);
 expect_kind(row, r, w, "b.c", ROW_NO_VERDICT);
 path_of(p, sizeof p, w, "c.c");
 rows++;
 if (!wpe_ckrec_verdict_of(r, p, &v) || v.from == NULL) {
  say_fail(row, "the verdict of c.c carries no source");
  wpe_ckrec_free(r);
  return;
 }
 (void)wpe_ckrec_sentence(line, sizeof line, &v);
 rows++;
 if (!strstr(line, v.from)) say_fail(row, "the sentence does not name the file that moved here: %s", line);
 wpe_ckrec_free(r);
}

/**
 * row_replaced_folder_names_its_source - A folder moved over another folder is not silent either.
 *
 * What the user gets: the same as for a file.  An empty folder b renamed over
 * an empty folder c leaves one verdict, for c, and b is named nowhere else, so
 * c's sentence must name b.
 */
static void row_replaced_folder_names_its_source(void)
{
 char w[NAME_MAX_TEST], p[NAME_MAX_TEST], line[WPE_CK_SENTENCE_MAX];
 WpeCkRecord *r = new_record(BOUND_BIG);
 const char *row = "a replaced folder names its source";
 WpeCkVerdict v;
 row_folder(w, sizeof w);
 make_folder(w, "b");
 make_folder(w, "c");
 walk_side(r, WPE_CK_THEN, w);
 rename_to(w, "b", "c");
 walk_side(r, WPE_CK_NOW, w);
 expect_kind(row, r, w, "c", WPE_CK_REPLACED);
 expect_kind(row, r, w, "b", ROW_NO_VERDICT);
 path_of(p, sizeof p, w, "c");
 rows++;
 if (!wpe_ckrec_verdict_of(r, p, &v) || v.from == NULL) {
  say_fail(row, "the verdict of c carries no source");
  wpe_ckrec_free(r);
  return;
 }
 rows++;
 if (wpe_ckrec_sentence(line, sizeof line, &v) <= 0) {
  say_fail(row, "the verdict of c has no sentence");
  wpe_ckrec_free(r);
  return;
 }
 rows++;
 if (!strstr(line, v.from)) say_fail(row, "the sentence does not name the folder that moved here: %s", line);
 wpe_ckrec_free(r);
}

/**
 * row_folder_renamed_under_a_cut - A folder renamed, in a review the bound cut, with no birth time.
 *
 * What the user gets: an answer for every name the checkpoint listed, even when
 * the name moved with its folder and the review stopped early.  Renaming a
 * folder leaves the status-change time of the files inside it alone, so on a
 * system that keeps no birth time the two looks of such a file agree on every
 * field the judge reads and agreement alone says SAME.  That is not proof: a
 * number the file system gave back to a file the run made agrees the same way,
 * which is what the row above is about.  So the claim is not proven, a review
 * the bound cut cannot tell this move from a removal, and the old name reads
 * CANNOT_SAY with the bound's cause instead of going silent, while the new name
 * still reads MOVED.  The row plays that system with the identity owner's own
 * hook, so it runs on a file system that keeps a birth time too.
 */
static void row_folder_renamed_under_a_cut(void)
{
 char w[NAME_MAX_TEST], f[64];
 WpeCkRecord *r = new_record(BOUND_BIG);
 const char *row = "a renamed folder's child under a cut";
 int i;
 row_folder(w, sizeof w);
 make_folder(w, "a");
 put_file(w, "a/child", "CHILD\n");
 setenv(WPE_SEEN_TEST_FAIL_ENV, WPE_SEEN_TEST_FAIL_NO_BIRTH, 1);
 walk_side(r, WPE_CK_THEN, w);
 rename_to(w, "a", "b");
 /* the bound is spent below the renamed folder, so the folder and the child in
    it are both listed before the walk stops */
 make_folder(w, "b/deep");
 for (i = 0; i < BOUND_BIG + 1; i++) {
  snprintf(f, sizeof f, "b/deep/n%02d", i);
  put_file(w, f, "n\n");
 }
 walk_side(r, WPE_CK_NOW, w);
 clear_seen_hook();
 expect_kind(row, r, w, "b", WPE_CK_MOVED);
 expect_kind(row, r, w, "b/child", WPE_CK_MOVED);
 expect_kind(row, r, w, "a/child", WPE_CK_CANNOT_SAY);
 expect_why(row, r, w, "a/child", WPE_CK_WHY_BOUND);
 wpe_ckrec_free(r);
}

/**
 * birth_time_here - Can a look in this row's folder prove an object is the one?
 * @w: the row's folder; @rel: a name in it to look at; @row: the name a skip carries.
 *
 * What the user gets: a row whose answer rests on a birth time says by name
 * when the file system keeps none, instead of failing for a reason that is the
 * system's and not the record's.
 * Return: 1 when a birth time is there to judge with.
 */
static int birth_time_here(const char *w, const char *rel, const char *row)
{
 char p[NAME_MAX_TEST];
 WpeSeen seen;

 path_of(p, sizeof p, w, rel);
 if (wpe_seen_look_name(p, 0, &seen) != 0) {
  say_fail(row, "look %s: %s", rel, strerror(errno));
  return 0;
 }
 return test_observation(seen.seen_has_birth, row, "this folder's file system keeps no birth time");
}

/**
 * row_proven_rename_under_a_cut - A rename the birth time proves, in a review the bound cut.
 *
 * What the user gets: a file the run renamed is still told as moved when the
 * review stopped early, and the name it had is not a second row of its own.
 * The proof is the birth time the file system keeps, so this row plays no
 * hook: the object at the new name is the object the checkpoint saw, and that
 * answer does not wait on how far the review read.  Losing it would turn every
 * rename in a workspace too large for one review into a "cannot say", so the
 * user would be asked about a file that is sitting under the name the run gave
 * it, and a revert would leave the old name alone as though nothing held it.
 * The row skips by name on a file system that keeps no birth time.
 */
static void row_proven_rename_under_a_cut(void)
{
 char w[NAME_MAX_TEST], f[64];
 WpeCkRecord *r = new_record(BOUND_BIG);
 const char *row = "a proven rename under a cut";
 int i;
 row_folder(w, sizeof w);
 make_folder(w, "a");
 put_file(w, "a/notes", "USER\n");
 if (!birth_time_here(w, "a/notes", row)) {
  wpe_ckrec_free(r);
  return;
 }
 walk_side(r, WPE_CK_THEN, w);
 rename_to(w, "a/notes", "a/notes.moved");
 /* the bound is spent in a deeper folder, so the folder of the rename is read to its end */
 make_folder(w, "a/deep");
 for (i = 0; i < BOUND_BIG + 1; i++) {
  snprintf(f, sizeof f, "a/deep/n%02d", i);
  put_file(w, f, "n\n");
 }
 walk_side(r, WPE_CK_NOW, w);
 expect_kind(row, r, w, "a/notes.moved", WPE_CK_MOVED);
 expect_kind(row, r, w, "a/notes", ROW_NO_VERDICT);
 wpe_ckrec_free(r);
}

/**
 * row_proven_claim_survives_a_later_one - Two names claim one old name in a review the bound cut.
 *
 * What the user gets: one row for the file the run moved, and no question about
 * the name it had.  A file moved out of a folder the run then renamed leaves
 * two names claiming the old one: the name it moved to, which proves the move,
 * and a new file at its old place below the renamed folder, which the review
 * reads as a move of the same old name and proves nothing.  The proof the first
 * claim gave is kept, so a workspace too large for one review does not ask about
 * a file that is sitting under the name the run gave it.  The row skips by name
 * on a file system that keeps no birth time, where no claim can be proven.
 */
static void row_proven_claim_survives_a_later_one(void)
{
 char w[NAME_MAX_TEST], f[64];
 WpeCkRecord *r = new_record(BOUND_BIG);
 const char *row = "a proven claim beside a later one";
 int i;
 row_folder(w, sizeof w);
 make_folder(w, "d");
 put_file(w, "d/f.c", "USER\n");
 if (!birth_time_here(w, "d/f.c", row)) {
  wpe_ckrec_free(r);
  return;
 }
 walk_side(r, WPE_CK_THEN, w);
 rename_to(w, "d/f.c", "b.c");
 rename_to(w, "d", "e");
 put_file(w, "e/f.c", "BRAND NEW\n");
 /* the bound is spent in a deeper folder, so the folders of both claims are read to their end */
 make_folder(w, "e/deep");
 for (i = 0; i < BOUND_BIG + 1; i++) {
  snprintf(f, sizeof f, "e/deep/n%02d", i);
  put_file(w, f, "n\n");
 }
 walk_side(r, WPE_CK_NOW, w);
 expect_kind(row, r, w, "b.c", WPE_CK_MOVED);
 expect_kind(row, r, w, "e/f.c", WPE_CK_MOVED);
 expect_kind(row, r, w, "d/f.c", ROW_NO_VERDICT);
 wpe_ckrec_free(r);
}

/** A folder a tool ignored, replaced by another folder, keeps its cause: nothing under it is new. */
static void row_unread_folder_replaced(void)
{
 char w[NAME_MAX_TEST], p[NAME_MAX_TEST];
 WpeCkRecord *r = new_record(BOUND_BIG);
 row_folder(w, sizeof w);
 make_folder(w, "build");
 put_file(w, "build/x", "USER\n");
 wpe_ckrec_tool_begin(r, WPE_CK_THEN, w);
 path_of(p, sizeof p, w, "build");
 wpe_ckrec_unread(r, WPE_CK_THEN, p);
 wpe_ckrec_complete(r, WPE_CK_THEN, w);
 rename_to(w, "build", "b2");
 make_folder(w, "build");
 rename_to(w, "b2/x", "build/x");
 wpe_ckrec_now_begin(r);
 wpe_ckrec_tool_begin(r, WPE_CK_NOW, w);
 path_of(p, sizeof p, w, "build/x");
 wpe_ckrec_listed(r, WPE_CK_NOW, p);
 wpe_ckrec_complete(r, WPE_CK_NOW, w);
 expect_kind("a replaced ignored folder", r, w, "build/x", WPE_CK_CANNOT_SAY);
 expect_why("a replaced ignored folder", r, w, "build/x", WPE_CK_WHY_NOT_LISTED);
 wpe_ckrec_free(r);
}

/** An ignored file is present on both sides: never removed, never new. */
static void row_ignored_files_on_both_sides(void)
{
 char w[NAME_MAX_TEST], p[NAME_MAX_TEST], q[NAME_MAX_TEST];
 WpeCkRecord *r = new_record(BOUND_BIG);
 row_folder(w, sizeof w);
 put_file(w, "foo.o", "OBJ\n");
 path_of(p, sizeof p, w, "foo.o");
 wpe_ckrec_tool_begin(r, WPE_CK_THEN, w);
 wpe_ckrec_listed_ignored(r, WPE_CK_THEN, p);
 wpe_ckrec_complete(r, WPE_CK_THEN, w);
 put_file(w, "bar.o", "NEW OBJ\n");
 path_of(q, sizeof q, w, "bar.o");
 wpe_ckrec_now_begin(r);
 wpe_ckrec_tool_begin(r, WPE_CK_NOW, w);
 wpe_ckrec_listed_ignored(r, WPE_CK_NOW, p);
 wpe_ckrec_listed_ignored(r, WPE_CK_NOW, q);
 wpe_ckrec_complete(r, WPE_CK_NOW, w);
 expect_kind("an ignored file that stayed", r, w, "foo.o", ROW_NO_VERDICT);
 expect_kind("an ignored file the run made", r, w, "bar.o", ROW_NO_VERDICT);
 expect_count("ignored files", "whole", wpe_ckrec_whole(r), 1);
 wpe_ckrec_free(r);
}

/** A watched root that a link replaced is not walked as the checkpoint's folder. */
static void row_root_replaced_by_a_link(void)
{
 char w[NAME_MAX_TEST], other[NAME_MAX_TEST];
 WpeCkRecord *r = new_record(BOUND_BIG);
 const WpeCkNote *notes;
 WpeCkVerdict v;
 row_folder(w, sizeof w);
 make_folder(w, "out");
 put_file(w, "out/f.txt", "OWN\n");
 outside_folder(other, sizeof other);
 {
  char root[NAME_MAX_TEST], file[NAME_MAX_TEST], theirs[NAME_MAX_TEST];
  path_of(root, sizeof root, w, "out");
  path_of(theirs, sizeof theirs, other, "f.txt");
  wpe_ckrec_walk(r, WPE_CK_THEN, root);
  put_file(other, "f.txt", "THEIRS\n");
  rename_to(w, "out", "out.real");
  symlink_folder(w, "out", other);
  path_of(file, sizeof file, w, "out/f.txt");
  wpe_ckrec_now_begin(r);
  wpe_ckrec_walk(r, WPE_CK_NOW, root);
  expect_count("a root replaced by a link", "now notes", wpe_ckrec_notes(r, WPE_CK_NOW, &notes), 1);
  rows++;
  if (notes && notes[0].why != WPE_CK_WHY_ROOT_REPLACED) say_fail("root link", "the note is not the root's");
  rows++;
  if (wpe_ckrec_may_remove(r, file, &v)) say_fail("root link", "may_remove said yes under a replaced root");
 }
 wpe_ckrec_free(r);
}

/** A held file under a watched root that a link replaced is never read, diffed or restored. */
static void row_held_file_under_a_replaced_root(void)
{
 char w[NAME_MAX_TEST], other[NAME_MAX_TEST], root[NAME_MAX_TEST], file[NAME_MAX_TEST];
 WpeCkRecord *r = new_record(BOUND_BIG);
 WpeCkVerdict v;
 size_t len;
 int image = -1;
 row_folder(w, sizeof w);
 make_folder(w, "out");
 put_file(w, "out/x.c", "SEED\n");
 outside_folder(other, sizeof other);
 put_file(other, "x.c", "THEIRS\n");
 path_of(root, sizeof root, w, "out");
 wpe_ckrec_walk(r, WPE_CK_THEN, root);
 hold_image(r, w, "out/x.c", 3);
 rename_to(w, "out", "out.real");
 symlink_folder(w, "out", other);
 wpe_ckrec_now_begin(r);
 wpe_ckrec_walk(r, WPE_CK_NOW, root);
 path_of(file, sizeof file, w, "out/x.c");
 expect_kind("held file under a replaced root", r, w, "out/x.c", WPE_CK_CANNOT_SAY);
 expect_why("held file under a replaced root", r, w, "out/x.c", WPE_CK_WHY_ROOT_REPLACED);
 rows++;
 if (wpe_ckrec_may_restore(r, file, &image)) say_fail("replaced root", "may_restore said yes");
 rows++;
 if (wpe_ckrec_now_text(r, 3, &len, &v) != NULL) say_fail("replaced root", "now_text read the file behind the link");
 rows++;
 if (v.kind != WPE_CK_CANNOT_SAY) say_fail("replaced root", "now_text did not say it cannot say");
 wpe_ckrec_free(r);
}

/** Which verdicts are about a path that is not there now: the answer the jump list uses. */
static void row_gone_kinds(void)
{
 rows++;
 if (!wpe_ckrec_kind_is_gone(WPE_CK_REMOVED) || !wpe_ckrec_kind_is_gone(WPE_CK_REMOVED_NO_COPY) ||
     !wpe_ckrec_kind_is_gone(WPE_CK_MOVED_HIDDEN))
  say_fail("gone kinds", "a removed or hidden-moved path is said to be there");
 rows++;
 if (wpe_ckrec_kind_is_gone(WPE_CK_CREATED) || wpe_ckrec_kind_is_gone(WPE_CK_MOVED) ||
     wpe_ckrec_kind_is_gone(WPE_CK_STAYED) || wpe_ckrec_kind_is_gone(WPE_CK_REPLACED))
  say_fail("gone kinds", "a path that is there is said to be gone");
}

/** A listed file renamed to a hidden name gets its own verdict, on the old name. */
static void row_renamed_to_a_hidden_name(void)
{
 char w[NAME_MAX_TEST];
 WpeCkRecord *r = new_record(BOUND_BIG);
 row_folder(w, sizeof w);
 make_folder(w, "d");
 put_file(w, "d/notes.txt", "USER\n");
 walk_side(r, WPE_CK_THEN, w);
 rename_to(w, "d/notes.txt", "d/.notes");
 walk_side(r, WPE_CK_NOW, w);
 expect_kind("renamed to a hidden name", r, w, "d/notes.txt", WPE_CK_MOVED_HIDDEN);
 expect_kind("the hidden name itself", r, w, "d/.notes", ROW_NO_VERDICT);
 wpe_ckrec_free(r);
}

/** More hidden entries than the cap are said, not dropped in silence; a failed look is a note. */
static void row_hidden_cap_and_unlooked(void)
{
 char w[NAME_MAX_TEST];
 WpeCkRecord *r = new_record(BOUND_SMALL);
 const WpeCkNote *notes;
 int i;
 row_folder(w, sizeof w);
 for (i = 0; i < BOUND_SMALL + 2; i++) {
  char f[32];
  snprintf(f, sizeof f, ".h%d", i);
  put_file(w, f, "h\n");
 }
 walk_side(r, WPE_CK_THEN, w);
 expect_count("hidden cap", "then notes", wpe_ckrec_notes(r, WPE_CK_THEN, &notes), 1);
 wpe_ckrec_free(r);
 r = new_record(BOUND_BIG);
 {
  char gone[NAME_MAX_TEST];
  path_of(gone, sizeof gone, w, "nowhere");
  wpe_ckrec_complete(r, WPE_CK_THEN, gone);
 }
 expect_count("a folder that cannot be looked at", "then notes", wpe_ckrec_notes(r, WPE_CK_THEN, &notes), 1);
 wpe_ckrec_free(r);
}

/* ---- row 6: places that cannot be opened ------------------------------------ */

static void row_refusals(void)
{
 char w[NAME_MAX_TEST], outside[NAME_MAX_TEST], line[WPE_CK_SENTENCE_MAX];
 WpeCkRecord *r = new_record(BOUND_BIG);
 const WpeCkNote *notes;
 row_folder(w, sizeof w);
 outside_folder(outside, sizeof outside);
 symlink_folder(w, "lnk", outside);
 put_file(w, "plain", "p\n");
 walk_side(r, WPE_CK_THEN, w);
 expect_count("refusal", "notes for a link met in a listing (one entry, never entered)",
              wpe_ckrec_notes(r, WPE_CK_THEN, &notes), 0);
 wpe_ckrec_free(r);
 r = new_record(BOUND_BIG);
 wpe_ckrec_walk(r, WPE_CK_THEN, outside);
 {
  char file[NAME_MAX_TEST];
  path_of(file, sizeof file, w, "plain");
  wpe_ckrec_walk(r, WPE_CK_THEN, file);
 }
 expect_count("refusal", "notes for a root that is a file", wpe_ckrec_notes(r, WPE_CK_THEN, &notes), 1);
 rows++;
 line[0] = '\0';
 if (notes) wpe_ckrec_note_sentence(line, sizeof line, &notes[0]);
 if (!strstr(line, "not watched")) say_fail("refusal", "the sentence of a refused folder is %s", line);
 wpe_ckrec_free(r);
}

/** A name asked for at the checkpoint and not there is absent: its folder is not a place nobody reached. */
static void row_absent_is_not_unreached(void)
{
 char w[NAME_MAX_TEST], p[NAME_MAX_TEST];
 WpeCkRecord *r = new_record(BOUND_BIG);
 WpeReadOutcome gone;
 row_folder(w, sizeof w);
 wpe_read_outcome_errno(&gone, ENOENT);
 path_of(p, sizeof p, w, "out/x.c");
 wpe_ckrec_then_asked(r, p, &gone);
 walk_side(r, WPE_CK_THEN, w);
 make_folder(w, "out");
 put_file(w, "out/x.c", "X\n");
 put_file(w, "out/y.c", "Y\n");
 walk_side(r, WPE_CK_NOW, w);
 expect_kind("asked and absent", r, w, "out/x.c", WPE_CK_CREATED);
 expect_kind("beside it", r, w, "out/y.c", WPE_CK_CREATED);
 expect_count("asked and absent", "notes", note_count(r), 0);
 wpe_ckrec_free(r);
}

/* ---- row 7: no birth time ---------------------------------------------------- */

static void row_no_birth(void)
{
 char w[NAME_MAX_TEST];
 WpeCkRecord *r = new_record(BOUND_BIG);
 row_folder(w, sizeof w);
 put_file(w, "a.c", "USER\n");
 walk_side(r, WPE_CK_THEN, w);
 rename_to(w, "a.c", "b.c");
 setenv(WPE_SEEN_TEST_FAIL_ENV, WPE_SEEN_TEST_FAIL_NO_BIRTH, 1);
 walk_side(r, WPE_CK_NOW, w);
 unsetenv(WPE_SEEN_TEST_FAIL_ENV);
 expect_kind("a rename on a system with no birth time", r, w, "b.c", WPE_CK_MOVED);
 /* the review was read to its end, so a claim it could not prove still answers for the old name */
 expect_kind("a rename on a system with no birth time", r, w, "a.c", ROW_NO_VERDICT);
 wpe_ckrec_free(r);
}

/* ---- row 8: the same size is kept ---------------------------------------------- */

static void row_maybe_copy(void)
{
 char w[NAME_MAX_TEST], p[NAME_MAX_TEST];
 WpeCkRecord *r = new_record(BOUND_BIG);
 WpeCkVerdict v;
 row_folder(w, sizeof w);
 put_file(w, "lost", "TWELVE BYTES\n");
 walk_side(r, WPE_CK_THEN, w);
 put_file(w, "same", "SAME SIZE!!!\n");
 put_file(w, "bigger", "ONE BYTE LONGER\n");
 remove_file(w, "lost");
 walk_side(r, WPE_CK_NOW, w);
 expect_kind("a new file as large as a removed one", r, w, "same", WPE_CK_MAYBE_COPY);
 expect_kind("a larger one", r, w, "bigger", WPE_CK_CREATED);
 expect_kind("the removed one", r, w, "lost", WPE_CK_REMOVED_NO_COPY);
 path_of(p, sizeof p, w, "same");
 rows++;
 if (wpe_ckrec_may_remove(r, p, &v)) say_fail("maybe a copy", "may_remove said yes");
 path_of(p, sizeof p, w, "bigger");
 rows++;
 if (!wpe_ckrec_may_remove(r, p, &v)) say_fail("created", "may_remove said no for a created file");
 wpe_ckrec_free(r);
}

/** The fresh look refuses a file that is no longer the object the review judged. */
static void row_may_remove_looks_again(void)
{
 char w[NAME_MAX_TEST], p[NAME_MAX_TEST];
 WpeCkRecord *r = new_record(BOUND_BIG);
 WpeCkVerdict v;
 row_folder(w, sizeof w);
 walk_side(r, WPE_CK_THEN, w);
 put_file(w, "made", "RUN\n");
 walk_side(r, WPE_CK_NOW, w);
 path_of(p, sizeof p, w, "made");
 rows++;
 if (!wpe_ckrec_may_remove(r, p, &v)) say_fail("may_remove", "no for a file the run made");
 replace_file(w, "made", "USER PUT THIS HERE\n");
 rows++;
 if (wpe_ckrec_may_remove(r, p, &v)) say_fail("may_remove", "yes for another object put under the name since");
 wpe_ckrec_free(r);
}

/* ---- row 9: one spelling per path ------------------------------------------- */

static void row_spelling(void)
{
 char w[NAME_MAX_TEST], p[NAME_MAX_TEST], odd[NAME_MAX_TEST];
 WpeCkRecord *r = new_record(BOUND_BIG);
 int image = -1;
 row_folder(w, sizeof w);
 make_folder(w, "d");
 put_file(w, "d/f.c", "F\n");
 snprintf(odd, sizeof odd, "%s/d//f.c", w);
 {
  WpeSeen seen;
  path_of(p, sizeof p, w, "d/f.c");
  if (wpe_seen_look_name(p, 0, &seen) != 0) say_fail("spelling", "look");
  wpe_ckrec_then_image(r, odd, 7, &seen);
 }
 rows++;
 if (!wpe_ckrec_may_restore(r, p, &image) || image != 7) say_fail("spelling", "one spelling does not find the image of the other");
 rows++;
 if (!wpe_ckrec_may_restore(r, odd, &image) || image != 7) say_fail("spelling", "the odd spelling does not find its own image");
 wpe_ckrec_free(r);
}

/* ---- row 10: a record with no look ------------------------------------------ */

static void row_no_look_is_no_identity(void)
{
 char w[NAME_MAX_TEST], p[NAME_MAX_TEST];
 WpeCkRecord *r = new_record(BOUND_BIG);
 row_folder(w, sizeof w);
 walk_side(r, WPE_CK_THEN, w);
 path_of(p, sizeof p, w, "ghost");
 wpe_ckrec_unread(r, WPE_CK_THEN, p);
 make_folder(w, "other");
 walk_side(r, WPE_CK_NOW, w);
 expect_kind("a folder with no look then", r, w, "other", WPE_CK_CREATED);
 wpe_ckrec_free(r);
}

/* ---- row 11: the words --------------------------------------------------------- */

/**
 * kind_names_its_source - Is this a kind whose verdict carries the name its object came from, so its sentence must print it?
 * @is_folder: 1 for a folder's verdict, which speaks only for a move and a replacement.
 */
static int kind_names_its_source(int kind, int is_folder)
{
 if (kind == WPE_CK_MOVED || kind == WPE_CK_REPLACED)
  return 1;
 if (is_folder)
  return 0;
 return kind == WPE_CK_MOVED_HIDDEN || kind == WPE_CK_LINKED || kind == WPE_CK_MAYBE_COPY;
}

/**
 * words_name_their_source - Every verdict that carries a source prints it, for a file and for a folder.
 *
 * What the user gets: no name the checkpoint listed goes unmentioned.  The
 * check is over kinds, not over one bug, so the next kind that gains a source
 * is caught too.  A REPLACED verdict names its source only when the object at
 * the name is one the checkpoint knew, so the object is MOVED.
 */
static void words_name_their_source(void)
{
 static const WpeCkVerdictKind kinds[] = { WPE_CK_MOVED, WPE_CK_MOVED_HIDDEN, WPE_CK_LINKED, WPE_CK_MAYBE_COPY, WPE_CK_REPLACED };
 char line[WPE_CK_SENTENCE_MAX];
 size_t i;
 int is_folder;
 for (is_folder = 0; is_folder <= 1; is_folder++) {
  for (i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
   WpeCkVerdict v;
   if (!kind_names_its_source((int)kinds[i], is_folder)) continue;
   memset(&v, 0, sizeof v);
   v.kind = kinds[i];
   v.object = WPE_CK_MOVED;
   v.is_folder = is_folder;
   v.path = "/w/p";
   v.from = "/w/q";
   v.folder_now = WPE_CK_FOLDER_THERE;
   (void)wpe_ckrec_sentence(line, sizeof line, &v);
   rows++;
   if (!strstr(line, "/w/q"))
    say_fail("words", "%s%s carries a source and its sentence does not name it: %s", is_folder ? "a folder's " : "",
             kind_name((int)kinds[i]), line);
  }
 }
}

static void row_words(void)
{
 static const WpeCkVerdictKind kinds[] = { WPE_CK_STAYED, WPE_CK_CREATED, WPE_CK_REMOVED, WPE_CK_REMOVED_NO_COPY,
                                           WPE_CK_MOVED, WPE_CK_MOVED_HIDDEN, WPE_CK_LINKED, WPE_CK_MAYBE_COPY,
                                           WPE_CK_REPLACED,
                                           WPE_CK_NOT_READABLE_NOW, WPE_CK_CANNOT_SAY };
 static const WpeCkWhy whys[] = { WPE_CK_WHY_REFUSED, WPE_CK_WHY_BOUND, WPE_CK_WHY_NOT_LISTED,
                                  WPE_CK_WHY_ROOT_REPLACED, WPE_CK_WHY_TOOL, WPE_CK_WHY_IMAGE_LOST };
 char line[WPE_CK_SENTENCE_MAX];
 size_t i;
 words_name_their_source();
 for (i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
  WpeCkVerdict v;
  const char *text;
  int len;
  memset(&v, 0, sizeof v);
  v.kind = kinds[i];
  v.path = "/w/p";
  v.from = "/w/q";
  v.why = WPE_CK_WHY_NOT_LISTED;
  v.folder_now = WPE_CK_FOLDER_THERE;
  text = wpe_ckrec_row_text(&v);
  len = wpe_ckrec_sentence(line, sizeof line, &v);
  rows++;
  if (kinds[i] != WPE_CK_STAYED && kinds[i] != WPE_CK_CANNOT_SAY && (!text || !*text))
   say_fail("words", "%s has no row text", kind_name((int)kinds[i]));
  if (text && strlen(text) > WPE_CK_ROW_MAX) say_fail("words", "%s: the row text is too long for Messages", text);
  if (kinds[i] != WPE_CK_STAYED && kinds[i] != WPE_CK_CREATED && len <= 0)
   say_fail("words", "%s has no sentence", kind_name((int)kinds[i]));
 }
 for (i = 0; i < sizeof whys / sizeof whys[0]; i++) {
  WpeCkNote n;
  char trace[WPE_CK_SENTENCE_MAX];
  memset(&n, 0, sizeof n);
  n.side = i % 2 ? WPE_CK_NOW : WPE_CK_THEN;
  n.why = whys[i];
  n.where = "/w/d";
  snprintf(n.reason, sizeof n.reason, "Permission denied");
  rows++;
  if (wpe_ckrec_note_sentence(line, sizeof line, &n) <= 0) say_fail("words", "a note of why %d has no sentence", (int)whys[i]);
  if (wpe_ckrec_note_trace(trace, sizeof trace, &n, "changeset") <= 0) say_fail("words", "a note of why %d has no trace line", (int)whys[i]);
 }
 rows++;
 wpe_ckrec_last_line(line, sizeof line, 0, 0, 0);
 if (!strstr(line, "ALL changes reverted")) say_fail("words", "the full revert is not said when nothing is left: %s", line);
 wpe_ckrec_last_line(line, sizeof line, 0, 1, 0);
 if (strstr(line, "ALL changes reverted")) say_fail("words", "a kept file is said as a full revert: %s", line);
 wpe_ckrec_last_line(line, sizeof line, 0, 0, 1);
 if (strstr(line, "ALL changes reverted")) say_fail("words", "an unseen place is said as a full revert: %s", line);
 wpe_ckrec_header(line, sizeof line, 0, 0);
 if (!strstr(line, "no files changed")) say_fail("words", "the header of an empty review: %s", line);
 wpe_ckrec_header(line, sizeof line, 0, 2);
 if (strstr(line, "no files changed")) say_fail("words", "no files changed is said with places unseen: %s", line);
}

/* ---- the work folder ----------------------------------------------------------- */

static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
 (void)st;
 (void)flag;
 (void)ftw;
 return remove(path);
}

static void remove_test_folder(void)
{
 (void)nftw(dir, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}

int main(void)
{
 if (test_dir_make(dir, sizeof dir, "ckrec") != 0) {
  printf("test_ai_ckrec: no test folder\n");
  return 1;
 }
 row_table();
 row_table_notes();
 row_renames();
 row_second_name();
 row_replaced_then_moved();
 row_order();
 row_order_on_a_back_only_system();
 row_then_bound();
 row_now_bound_counts_only_new_names();
 row_refusals();
 row_absent_is_not_unreached();
 row_no_birth();
 row_maybe_copy();
 row_may_remove_looks_again();
 row_spelling();
 row_no_look_is_no_identity();
 row_removed_beside_a_cut_place();
 row_claimed_removal_under_a_cut();
 row_removed_beside_a_later_birth();
 row_removed_beside_a_later_birth_same_size();
 row_removed_folder_beside_a_later_birth();
 row_a_rename_on_a_back_only_birth();
 row_replaced_names_its_source();
 row_replaced_folder_names_its_source();
 row_folder_renamed_under_a_cut();
 row_proven_rename_under_a_cut();
 row_proven_claim_survives_a_later_one();
 row_unread_folder_replaced();
 row_ignored_files_on_both_sides();
 row_root_replaced_by_a_link();
 row_renamed_to_a_hidden_name();
 row_held_file_under_a_replaced_root();
 row_gone_kinds();
 row_hidden_cap_and_unlooked();
 row_words();
 remove_test_folder();
 fails += test_dir_failures;
 printf("test_ai_ckrec: %d checks, %d failed\n", rows, fails);
 return fails != 0;
}
