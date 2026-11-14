/* we_listsel.c -- the one owner of the selected entry of a list window.
   See we_listsel.h for the two list shapes and what is guaranteed. */
#include <stdlib.h>
#include <string.h>

#include "we_listsel.h"

/* The window kinds that keep a project list: Files, Variables, Install. */
#define LISTSEL_KIND_PROJECT_FIRST 4
#define LISTSEL_KIND_PROJECT_LAST  6

/* Where the sentinel stands when there is one. */
static int listsel_ends_in_sentinel(const struct dirfile *df)
{
  if(df == NULL || df->name == NULL || df->anz <= 0)
    return(0);
  return(df->name[df->anz - 1] != NULL &&
         strcmp(df->name[df->anz - 1], WPE_LISTSEL_SENTINEL) == 0);
}

/* The entries a mutator may touch: all rows, minus the sentinel when the
   list ends in one.  A blank text is never an entry, so a trailing blank row
   is always the sentinel. */
static int listsel_real_count(const struct dirfile *df)
{
  if(df == NULL || df->name == NULL || df->anz <= 0)
    return(0);
  return(df->anz - (listsel_ends_in_sentinel(df) ? 1 : 0));
}

static int listsel_text_is_blank(const char *text)
{
  if(text == NULL)
    return(1);
  while(*text == ' ' || *text == '\t')
    text++;
  return(*text == '\0');
}

static char *listsel_copy(const char *text)
{
  char *p = malloc(strlen(text) + 1);

  if(p != NULL)
    strcpy(p, text);
  return(p);
}

/**
 * wpe_listsel_is_sealed_kind - Tell whether a window kind keeps a sealed list.
 * @ins: the window's kind (FENSTER.ins).
 *
 * The three project lists end in a blank row where the cursor stands to add;
 * the search, index and window lists do not.
 *
 * Return: 1 for the Files, Variables and Install lists, 0 otherwise.
 */
int wpe_listsel_is_sealed_kind(int ins)
{
  return(ins >= LISTSEL_KIND_PROJECT_FIRST && ins <= LISTSEL_KIND_PROJECT_LAST);
}

/**
 * wpe_listsel_has - Tell whether the highlighted row names an entry.
 * @df: the list; @nf: the highlighted row; @sealed: the list ends in a sentinel.
 *
 * A search that found nothing, or the blank row at the end of a project list,
 * has nothing under the cursor to open.
 *
 * Return: 1 when @nf names a real entry, 0 otherwise.
 */
int wpe_listsel_has(const struct dirfile *df, int nf, int sealed)
{
  if(df == NULL || df->name == NULL || df->anz <= 0)
    return(0);
  if(nf < 0 || nf >= df->anz)
    return(0);
  if(sealed && nf == df->anz - 1)
    return(0);
  return(df->name[nf] != NULL);
}

/**
 * wpe_listsel_name - The entry under the highlighted row.
 * @df: the list; @nf: the highlighted row; @sealed: the list ends in a sentinel.
 *
 * Return: the entry's text, or NULL when the row names no entry.
 */
char *wpe_listsel_name(const struct dirfile *df, int nf, int sealed)
{
  if(!wpe_listsel_has(df, nf, sealed))
    return(NULL);
  return(df->name[nf]);
}

/**
 * wpe_listsel_count - How many entries a list holds.
 * @df: the list; @sealed: the list ends in a sentinel.
 *
 * What a Save writes and the assistant lists: the blank row is not counted.
 *
 * Return: the number of real entries.
 */
int wpe_listsel_count(const struct dirfile *df, int sealed)
{
  if(df == NULL || df->name == NULL || df->anz <= 0)
    return(0);
  return(df->anz - (sealed ? 1 : 0));
}

/**
 * wpe_listsel_entry - Entry @i of a list, counted from the first.
 * @df: the list; @i: 0 up to count-1; @sealed: the list ends in a sentinel.
 *
 * Return: the entry's text, or NULL when @i is outside the entries.
 */
char *wpe_listsel_entry(const struct dirfile *df, int i, int sealed)
{
  if(i < 0 || i >= wpe_listsel_count(df, sealed))
    return(NULL);
  return(df->name[i]);
}

/**
 * wpe_listsel_sentinel_row - The row of the blank end of a project list.
 * @df: the sealed list.
 *
 * Project > Add Item puts the cursor there so the new file lands last.
 *
 * Return: the row, 0 for an empty list.
 */
int wpe_listsel_sentinel_row(const struct dirfile *df)
{
  if(df == NULL || df->anz <= 0)
    return(0);
  return(df->anz - 1);
}

/**
 * wpe_listsel_action_needs_entry - Tell whether an action reads the selected entry.
 * @ins: the window's kind; @a: what the key asked.
 *
 * Opening the highlighted file, match or window needs one, and the user is
 * told "No entry in this list" when there is none. Adding, and editing on
 * the blank row of Variables or Install (which adds), do not need one.
 *
 * Return: 1 when the action needs a real entry under the cursor.
 */
int wpe_listsel_action_needs_entry(int ins, enum wpe_list_action a)
{
  (void)ins;
  return(a == WPE_LIST_OPEN);
}

/**
 * wpe_listsel_new_empty - A list with room and no rows.
 *
 * What a builder starts from before it appends entries; it is sealed once,
 * at the end, by wpe_listsel_seal.
 *
 * Return: the list, or NULL when memory is short.
 */
struct dirfile *wpe_listsel_new_empty(void)
{
  struct dirfile *df = malloc(sizeof(struct dirfile));

  if(df == NULL)
    return(NULL);
  df->anz = 0;
  df->name = malloc(sizeof(char *));
  if(df->name == NULL)
  {
    free(df);
    return(NULL);
  }
  return(df);
}

/**
 * wpe_listsel_new_sealed - A project list with nothing in it but the blank row.
 *
 * Return: the list, or NULL when memory is short.
 */
struct dirfile *wpe_listsel_new_sealed(void)
{
  struct dirfile *df = wpe_listsel_new_empty();

  if(df == NULL)
    return(NULL);
  if(wpe_listsel_seal(&df) != 0)
    return(NULL);
  return(df);
}

/**
 * wpe_listsel_seal - Give a project list its blank last row, whatever state it is in.
 * @df: the list to seal; a NULL list is created.
 *
 * A project read from a file that lacks a FILES line or an install section
 * leaves a missing or half-built list; sealing it here means no later Add,
 * Edit or Delete has to ask whether the list is whole.
 *
 * Return: 0, or -1 when memory is short (the list is then left as it was).
 */
int wpe_listsel_seal(struct dirfile **df)
{
  struct dirfile *d;
  char **names;
  char *blank;

  if(*df == NULL)
  {
    *df = wpe_listsel_new_empty();
    if(*df == NULL)
      return(-1);
  }
  d = *df;
  if(d->name == NULL)
  {
    d->anz = 0;
    d->name = malloc(sizeof(char *));
    if(d->name == NULL)
      return(-1);
  }
  if(listsel_ends_in_sentinel(d))
    return(0);
  blank = listsel_copy(WPE_LISTSEL_SENTINEL);
  if(blank == NULL)
    return(-1);
  names = realloc(d->name, (d->anz + 1) * sizeof(char *));
  if(names == NULL)
  {
    free(blank);
    return(-1);
  }
  d->name = names;
  d->name[d->anz] = blank;
  d->anz++;
  return(0);
}

/**
 * wpe_listsel_insert - Add an entry before a row of a list.
 * @df: the list; @nf: the row it goes before; @text: the new entry.
 *
 * The blank row of a project list stays last: adding at the cursor on the
 * blank row puts the entry above it.
 *
 * Return: the row of the new entry, or -1 with the list unchanged.
 */
int wpe_listsel_insert(struct dirfile *df, int nf, const char *text)
{
  char **names;
  char *copy;
  int limit, i;

  if(df == NULL || listsel_text_is_blank(text))
    return(-1);
  if(df->name == NULL)
    df->anz = 0;
  limit = listsel_real_count(df);
  if(nf < 0)
    nf = 0;
  if(nf > limit)
    nf = limit;
  copy = listsel_copy(text);
  if(copy == NULL)
    return(-1);
  names = realloc(df->name, (df->anz + 1) * sizeof(char *));
  if(names == NULL)
  {
    free(copy);
    return(-1);
  }
  df->name = names;
  for(i = df->anz; i > nf; i--)
    df->name[i] = df->name[i - 1];
  df->name[nf] = copy;
  df->anz++;
  return(nf);
}

/**
 * wpe_listsel_set - Change the entry of a row, or add one on the blank row.
 * @df: the list; @nf: the row; @text: the new text.
 *
 * Editing the last real variable changes it in place; editing on the blank
 * row adds a new entry above it.
 *
 * Return: the row written, or -1 with the list unchanged.
 */
int wpe_listsel_set(struct dirfile *df, int nf, const char *text)
{
  char *copy;

  if(df == NULL || listsel_text_is_blank(text))
    return(-1);
  if(nf < 0 || nf >= listsel_real_count(df))
    return(wpe_listsel_insert(df, nf, text));
  copy = listsel_copy(text);
  if(copy == NULL)
    return(-1);
  free(df->name[nf]);
  df->name[nf] = copy;
  return(nf);
}

/**
 * wpe_listsel_remove - Delete the entry of the highlighted row.
 * @df: the list; @nf: the highlighted row, moved onto a valid row afterwards.
 *
 * The blank row at the end of a project list cannot be deleted, so Delete
 * on it does nothing, and the last real file can still go.
 *
 * Return: 0 when an entry was removed, -1 when there was none to remove.
 */
int wpe_listsel_remove(struct dirfile *df, int *nf)
{
  int i;

  if(df == NULL || nf == NULL)
    return(-1);
  if(*nf < 0 || *nf >= listsel_real_count(df))
    return(-1);
  free(df->name[*nf]);
  df->anz--;
  for(i = *nf; i < df->anz; i++)
    df->name[i] = df->name[i + 1];
  if(*nf >= df->anz && *nf > 0)
    (*nf)--;
  return(0);
}

/**
 * wpe_listsel_tree_subdir - The subdirectory the directory tree highlights.
 * @cd: the working directory's ancestors; @dd: its subdirectories;
 * @nf: the highlighted row of the tree window, ancestors first.
 *
 * Dragging or acting on an ancestor row must not name a subdirectory: the
 * row's offset into the subdirectory list would be negative.
 *
 * Return: the subdirectory, or NULL for an ancestor row or a row out of range.
 */
char *wpe_listsel_tree_subdir(const struct dirfile *cd, const struct dirfile *dd, int nf)
{
  int t;

  if(cd == NULL || dd == NULL || dd->name == NULL)
    return(NULL);
  if(nf < cd->anz)
    return(NULL);
  t = nf - cd->anz;
  if(t >= dd->anz)
    return(NULL);
  return(dd->name[t]);
}
