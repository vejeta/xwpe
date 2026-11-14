/* we_listsel.h -- the one owner of the selected entry of a list window.

   A list window (Function-Index, Grep, Find, Windows, and the three project
   lists Files, Variables and Install) shows a `struct dirfile`: a count and
   an array of names, with the highlighted row kept by the window.  Whether
   the highlight names an entry, who may read that entry, and what shape the
   list keeps are answered here and nowhere else.

   Two shapes, decided by the kind of window:

   - Function-Index, Grep, Find and Windows lists hold entries only and may
     be empty.
   - A project list (Files, Variables, Install) is always sealed: it ends in
     one blank row, the sentinel, which is where the cursor stands to add a
     new entry.  The sentinel is not an entry.  No query returns it, no count
     includes it, a save never writes it, Delete cannot remove it.

   Plain C over `struct dirfile`: no editor state, so a unit test uses it
   without the editor. */
#ifndef WE_LISTSEL_H
#define WE_LISTSEL_H

#ifndef WPE_DIRFILE_DEFINED
#define WPE_DIRFILE_DEFINED
struct dirfile {
 int anz;      /* number elements in the list */
 char **name;  /* the list elements */
};
#endif

/** What a key asks of a list window, once the key is told apart from the kind. */
enum wpe_list_action {
  WPE_LIST_NONE,
  WPE_LIST_OPEN,
  WPE_LIST_ADD,
  WPE_LIST_EDIT,
  WPE_LIST_DELETE,
  WPE_LIST_OPTIONS
};

/** The text of the blank row that ends a project list. */
#define WPE_LISTSEL_SENTINEL " "

/* ---- Queries: the only reads of a selected entry ---- */

/** 1 when the window kind @ins keeps a sealed list (a project list). */
int wpe_listsel_is_sealed_kind(int ins);

/** 1 when row @nf names a real entry; 0 on an empty list, out of range, or on
    the sentinel row of a @sealed list. */
int wpe_listsel_has(const struct dirfile *df, int nf, int sealed);

/** The entry of row @nf, or NULL when it names none. */
char *wpe_listsel_name(const struct dirfile *df, int nf, int sealed);

/** The number of real entries, the sentinel of a @sealed list excluded. */
int wpe_listsel_count(const struct dirfile *df, int sealed);

/** Entry @i of 0..count-1, never the sentinel, or NULL. */
char *wpe_listsel_entry(const struct dirfile *df, int i, int sealed);

/** The row of the sentinel of a sealed list (where Add Item selects), 0 for
    an empty one. */
int wpe_listsel_sentinel_row(const struct dirfile *df);

/** 1 when the action @a needs an entry under the cursor in a list of kind
    @ins: opening one does; adding and editing on the blank row do not. */
int wpe_listsel_action_needs_entry(int ins, enum wpe_list_action a);

/* ---- Builders and mutators of a project list ---- */

/** A new list with room for entries and none in it (a builder appends to it). */
struct dirfile *wpe_listsel_new_empty(void);

/** A new sealed list: the sentinel alone. NULL when memory is short. */
struct dirfile *wpe_listsel_new_sealed(void);

/** Make *@df sealed whatever state it is in: create it when NULL, allocate its
    array when missing, append the sentinel when absent. Idempotent.
    Return: 0, or -1 when memory is short. */
int wpe_listsel_seal(struct dirfile **df);

/** Insert a copy of @text before row @nf (the sentinel row included); the
    sentinel stays last. A blank @text is refused.
    Return: the row of the new entry, or -1 (list unchanged). */
int wpe_listsel_insert(struct dirfile *df, int nf, const char *text);

/** Replace the entry of row @nf with a copy of @text; on the sentinel row, or
    past the entries, add it as a new entry instead. A blank @text is refused.
    Return: the row of the entry written, or -1 (list unchanged). */
int wpe_listsel_set(struct dirfile *df, int nf, const char *text);

/** Remove the entry of row *@nf and keep *@nf on a valid row. Refuses the
    sentinel row, an empty list and a row out of range.
    Return: 0 when removed, -1 when refused. */
int wpe_listsel_remove(struct dirfile *df, int *nf);

/* ---- The file manager's directory tree ---- */

/** The subdirectory selected at row @nf of the directory tree window: the tree
    lists the working directory's ancestors (@cd) and then its subdirectories
    (@dd). NULL for an ancestor row, which names no subdirectory to act on. */
char *wpe_listsel_tree_subdir(const struct dirfile *cd, const struct dirfile *dd, int nf);

#endif /* WE_LISTSEL_H */
