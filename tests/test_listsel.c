/* test_listsel.c -- unit test for the owner of the selected entry of a list
 * window (we_listsel.c).  Always runs; run it under ASan+UBSan+LSan too.
 *
 * What the user sees behind each group:
 *  - a search that found nothing, or the blank row that ends a project list,
 *    has no entry under the cursor, so Enter says "No entry in this list";
 *  - a Save never writes the blank row;
 *  - Delete on the blank row does nothing, and the last real file can go;
 *  - editing the last variable changes it in place;
 *  - dragging an ancestor row of the directory tree names no folder.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../we_listsel.h"

static int fails;

static void chk_int(const char *what, long got, long want)
{
  if (got != want)
  {
    printf("FAIL: %s: got %ld want %ld\n", what, got, want);
    fails++;
  }
}

static void chk_str(const char *what, const char *got, const char *want)
{
  if (got == NULL || want == NULL ? got != want : strcmp(got, want) != 0)
  {
    printf("FAIL: %s: got %s want %s\n", what, got ? got : "(null)", want ? want : "(null)");
    fails++;
  }
}

static void free_list(struct dirfile *df)
{
  int i;

  if (df == NULL)
    return;
  for (i = 0; i < df->anz; i++)
    free(df->name[i]);
  free(df->name);
  free(df);
}

static struct dirfile *sealed_with(const char *a, const char *b)
{
  struct dirfile *df = wpe_listsel_new_sealed();

  if (df == NULL)
    exit(2);
  if (a != NULL && wpe_listsel_insert(df, 0, a) < 0)
    exit(2);
  if (b != NULL && wpe_listsel_insert(df, 1, b) < 0)
    exit(2);
  return df;
}

static void test_queries(void)
{
  struct dirfile *df = sealed_with("a.c", "b.c");
  struct dirfile empty = { 0, NULL };

  chk_int("sealed list: 2 entries + sentinel", df->anz, 3);
  chk_int("has row 0", wpe_listsel_has(df, 0, 1), 1);
  chk_int("has row 1", wpe_listsel_has(df, 1, 1), 1);
  chk_int("sentinel row is no entry", wpe_listsel_has(df, 2, 1), 0);
  chk_int("row past the end", wpe_listsel_has(df, 3, 1), 0);
  chk_int("negative row", wpe_listsel_has(df, -1, 1), 0);
  chk_str("name of row 1", wpe_listsel_name(df, 1, 1), "b.c");
  chk_str("name of the sentinel row", wpe_listsel_name(df, 2, 1), NULL);
  chk_int("count excludes the sentinel", wpe_listsel_count(df, 1), 2);
  chk_str("entry 1", wpe_listsel_entry(df, 1, 1), "b.c");
  chk_str("entry 2 is the sentinel", wpe_listsel_entry(df, 2, 1), NULL);
  chk_int("sentinel row", wpe_listsel_sentinel_row(df), 2);

  chk_int("unsealed list: last row is an entry", wpe_listsel_has(df, 2, 0), 1);
  chk_int("empty list has no entry", wpe_listsel_has(&empty, 0, 0), 0);
  chk_int("NULL list has no entry", wpe_listsel_has(NULL, 0, 0), 0);
  chk_int("empty list count", wpe_listsel_count(&empty, 1), 0);
  chk_int("NULL list count", wpe_listsel_count(NULL, 0), 0);
  chk_int("empty list sentinel row", wpe_listsel_sentinel_row(&empty), 0);

  chk_int("open needs an entry", wpe_listsel_action_needs_entry(4, WPE_LIST_OPEN), 1);
  chk_int("add needs none", wpe_listsel_action_needs_entry(5, WPE_LIST_ADD), 0);
  chk_int("edit on the blank row needs none", wpe_listsel_action_needs_entry(6, WPE_LIST_EDIT), 0);
  chk_int("project kinds", wpe_listsel_is_sealed_kind(4) + wpe_listsel_is_sealed_kind(5) +
                           wpe_listsel_is_sealed_kind(6), 3);
  chk_int("search kinds are not sealed", wpe_listsel_is_sealed_kind(2) + wpe_listsel_is_sealed_kind(7), 0);
  free_list(df);
}

static void test_seal(void)
{
  struct dirfile *df = NULL;
  struct dirfile *half;

  chk_int("seal creates a NULL list", wpe_listsel_seal(&df), 0);
  chk_int("created list holds the sentinel", df ? df->anz : -1, 1);
  chk_int("seal is idempotent", wpe_listsel_seal(&df), 0);
  chk_int("still one row", df ? df->anz : -1, 1);
  free_list(df);

  /* A list allocated without its name array (a project with no FILES line). */
  half = malloc(sizeof *half);
  if (half == NULL)
    exit(2);
  half->anz = 0;
  half->name = NULL;
  chk_int("seal allocates a missing name array", wpe_listsel_seal(&half), 0);
  chk_int("and holds the sentinel", half->anz, 1);
  chk_int("sentinel row is no entry", wpe_listsel_has(half, 0, 1), 0);
  free_list(half);

  df = wpe_listsel_new_empty();
  if (df == NULL)
    exit(2);
  df->name = realloc(df->name, sizeof(char *));
  df->name[0] = strdup("x=1");
  df->anz = 1;
  chk_int("seal appends to a list of entries", wpe_listsel_seal(&df), 0);
  chk_int("entries then sentinel", df->anz, 2);
  chk_str("entry kept", df->name[0], "x=1");
  free_list(df);
}

static void test_insert_set(void)
{
  struct dirfile *df = sealed_with("a", NULL);

  chk_int("insert at the sentinel row goes above it",
          wpe_listsel_insert(df, wpe_listsel_sentinel_row(df), "b"), 1);
  chk_int("rows", df->anz, 3);
  chk_str("b is row 1", df->name[1], "b");
  chk_str("sentinel still last", df->name[2], WPE_LISTSEL_SENTINEL);
  chk_int("insert past the end clamps above the sentinel", wpe_listsel_insert(df, 99, "c"), 2);
  chk_str("c is row 2", df->name[2], "c");
  chk_str("sentinel still last after the clamp", df->name[3], WPE_LISTSEL_SENTINEL);
  chk_int("a blank text is refused", wpe_listsel_insert(df, 0, "  "), -1);
  chk_int("an empty text is refused", wpe_listsel_insert(df, 0, ""), -1);
  chk_int("a NULL text is refused", wpe_listsel_insert(df, 0, NULL), -1);
  chk_int("rows unchanged by refusals", df->anz, 4);

  chk_int("set replaces in place", wpe_listsel_set(df, 2, "C"), 2);
  chk_str("row 2 changed", df->name[2], "C");
  chk_int("rows unchanged by an edit", df->anz, 4);
  chk_int("edit of the last real entry stays in place", wpe_listsel_set(df, 2, "D"), 2);
  chk_str("sentinel untouched", df->name[3], WPE_LISTSEL_SENTINEL);
  chk_int("set on the sentinel row adds", wpe_listsel_set(df, 3, "E"), 3);
  chk_int("one more row", df->anz, 5);
  chk_str("E above the sentinel", df->name[3], "E");
  chk_str("sentinel last", df->name[4], WPE_LISTSEL_SENTINEL);
  free_list(df);
}

static void test_remove(void)
{
  struct dirfile *df = sealed_with("a", "b");
  int nf;

  nf = 2;
  chk_int("Delete on the sentinel row is refused", wpe_listsel_remove(df, &nf), -1);
  chk_int("rows unchanged", df->anz, 3);
  nf = 7;
  chk_int("Delete out of range is refused", wpe_listsel_remove(df, &nf), -1);
  nf = -1;
  chk_int("Delete of a negative row is refused", wpe_listsel_remove(df, &nf), -1);
  nf = 1;
  chk_int("Delete of the last real entry works", wpe_listsel_remove(df, &nf), 0);
  chk_int("highlight lands on the sentinel row", nf, 1);
  chk_int("rows", df->anz, 2);
  nf = 0;
  chk_int("Delete of the only file works", wpe_listsel_remove(df, &nf), 0);
  chk_int("only the sentinel is left", df->anz, 1);
  chk_str("it is the sentinel", df->name[0], WPE_LISTSEL_SENTINEL);
  nf = 0;
  chk_int("Delete on the lone sentinel is refused", wpe_listsel_remove(df, &nf), -1);
  chk_int("and it stays", df->anz, 1);
  free_list(df);
  chk_int("Delete on NULL is refused", wpe_listsel_remove(NULL, &nf), -1);
}

static struct dirfile *list_of(int n, const char *const *names)
{
  struct dirfile *df = wpe_listsel_new_empty();
  int i;

  if (df == NULL)
    exit(2);
  df->name = realloc(df->name, (n > 0 ? n : 1) * sizeof(char *));
  for (i = 0; i < n; i++)
    df->name[i] = strdup(names[i]);
  df->anz = n;
  return df;
}

static void test_tree_subdir(void)
{
  const char *anc[] = { "/", "home", "user" };
  const char *sub[] = { "docs", "src" };
  struct dirfile *cd = list_of(3, anc);
  struct dirfile *dd = list_of(2, sub);
  struct dirfile *none = list_of(0, NULL);

  chk_str("root row names no subdirectory", wpe_listsel_tree_subdir(cd, dd, 0), NULL);
  chk_str("ancestor row names no subdirectory", wpe_listsel_tree_subdir(cd, dd, 1), NULL);
  chk_str("working directory row names none", wpe_listsel_tree_subdir(cd, dd, 2), NULL);
  chk_str("first subdirectory", wpe_listsel_tree_subdir(cd, dd, 3), "docs");
  chk_str("second subdirectory", wpe_listsel_tree_subdir(cd, dd, 4), "src");
  chk_str("past the last subdirectory", wpe_listsel_tree_subdir(cd, dd, 5), NULL);
  chk_str("negative row", wpe_listsel_tree_subdir(cd, dd, -1), NULL);
  chk_str("no subdirectories at all", wpe_listsel_tree_subdir(cd, none, 3), NULL);
  chk_str("NULL lists", wpe_listsel_tree_subdir(NULL, dd, 3), NULL);
  free_list(cd);
  free_list(dd);
  free_list(none);
}

int main(void)
{
  test_queries();
  test_seal();
  test_insert_set();
  test_remove();
  test_tree_subdir();
  if (fails)
  {
    printf("test_listsel: %d failure(s)\n", fails);
    return 1;
  }
  printf("test_listsel: ok\n");
  return 0;
}
