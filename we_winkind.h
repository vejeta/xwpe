/* we_winkind.h -- the one owner of "which kind of window is this, and what
   does it hold".

   A window carries one of four different things behind one field: the text
   of a file, the File Manager's directory lists, a list window's rows (the
   project, the window list, the search results) or a dropdown's.  Nothing
   but convention used to stop a command from reading one as another, and a
   key could reach text code with a window that held none.

   What the user gets from this owner:

   * a command that works on text, asked of a window that has none, says one
     sentence on the status row and changes nothing;
   * a key pressed after a build ended is handled by the window the user is
     looking at, not by the one the key loop fetched before it waited;
   * a window the editor opens itself (Messages, Watches, Stack, Help, the
     clipboard, the assistant's pane) is known by the intent that opened it,
     never by the title it happens to carry, so a file the user names
     "Messages" stays the user's file.

   The payload lives in a block this file's module owns: the three pointers
   are separate typed members of a struct no other file may name, so the
   compiler refuses an alias, a macro, a pasted name or a cast that would
   read one kind's payload as another's.

   Copyright (C) 2026 Juan Manuel Méndez Rey
   This file is part of xwpe and is distributed under the GNU General Public
   License, version 2 or later. */

#ifndef WE_WINKIND_H
#define WE_WINKIND_H

struct FNST;
struct CNT;
struct BFF;
struct SCHRM;
struct FLBFF;
struct fl_wnd;

/* What a window holds.  The tag of the held block: it is written by the
   e_window_hold_* functions and by nothing else, so what a window holds and
   which pointer is valid cannot disagree. */
typedef enum {
 WPE_HOLDS_NOTHING = 0,   /* a window built and not yet given its contents */
 WPE_HOLDS_TEXT,          /* the text of a file, or of a pane the editor writes */
 WPE_HOLDS_FILES,         /* the File Manager's directory lists */
 WPE_HOLDS_LIST,          /* a list window's rows */
 WPE_HOLDS_DROPDOWN       /* a dropdown's rows: local to its box, never stacked */
} WpeHolds;

/* Which list a list window shows.  The values are the ones the editor has
   always used: three of them index the project's remembered lists, so they
   are named here and asked through e_project_list_slot instead of by
   arithmetic at each reader.  The spelling keeps clear of the list window's
   key actions, which are WPE_LIST_* of we_listsel.h. */
typedef enum {
 WPE_LISTKIND_NONE = 0,             /* not a list window */
 WPE_LISTKIND_FUNCTIONS = 1,        /* the manual page's function index */
 WPE_LISTKIND_GREP = 2,             /* the hits of a recursive search */
 WPE_LISTKIND_FOUND = 3,            /* the hits of a search in the open files */
 WPE_LISTKIND_PROJECT_FILES = 4,    /* the project's files */
 WPE_LISTKIND_PROJECT_VARIABLES = 5,
 WPE_LISTKIND_PROJECT_INSTALL = 6,
 WPE_LISTKIND_WINDOWS = 7           /* the list of the open windows */
} WpeListKind;

/* The first and last of the three project lists, whose remembered rows are
   kept in a table of their own. */
#define WPE_LISTKIND_PROJECT_FIRST WPE_LISTKIND_PROJECT_FILES
#define WPE_LISTKIND_PROJECT_LAST  WPE_LISTKIND_PROJECT_INSTALL

/* Which window the editor opened itself.  A file the user opened is
   WPE_PANE_FILE whatever its name, so a file called "Messages" is the
   user's file and a build never parses its errors into it. */
typedef enum {
 WPE_PANE_FILE = 0,     /* a file the user opened, or a new unnamed one */
 WPE_PANE_MESSAGES,     /* the build's output and its list of errors */
 WPE_PANE_WATCHES,      /* the debugger's watched expressions */
 WPE_PANE_STACK,        /* the debugger's call stack */
 WPE_PANE_HELP,         /* the help viewer */
 WPE_PANE_CLIPBOARD,    /* the paste buffer shown as a window */
 WPE_PANE_ASSISTANT     /* the assistant's pane */
} WpePane;

/* What a window holds.  Each answers for one kind and NULL (or
   WPE_LISTKIND_NONE, WPE_PANE_FILE) for every other, so a caller cannot take a
   list's rows for a file's text even with a wrong tag: there is no union to
   reinterpret. */
int e_window_is_text(const struct FNST *f);
struct BFF *e_window_text(const struct FNST *f);
struct SCHRM *e_window_screen(const struct FNST *f);
struct FLBFF *e_window_files(const struct FNST *f);
struct fl_wnd *e_window_list(const struct FNST *f);
WpeListKind e_window_list_kind(const struct FNST *f);
WpePane e_window_pane(const struct FNST *f);
WpeHolds e_window_holds(const struct FNST *f);

/* Giving a window what it holds: once per window, at its construction.  A
   hold that cannot allocate its block says so the way the constructors
   already do and leaves the window holding nothing. */
void e_window_held_forget(struct FNST *f);
int e_window_hold_text(struct FNST *f, struct BFF *b, struct SCHRM *s);
int e_window_hold_files(struct FNST *f, struct FLBFF *fb);
int e_window_hold_list(struct FNST *f, struct fl_wnd *fw, WpeListKind kind);
int e_window_hold_dropdown(struct FNST *f, struct fl_wnd *fw);
void e_window_set_pane(struct FNST *f, WpePane pane);

/* A window whose text is replaced (the help and info viewers take the pane's
   own buffer over the one just allocated): the old text is released, then
   the new one is held. */
int e_window_replace_text(struct FNST *f, struct BFF *b);

/* Taking the payload back at close: the pointer is returned for the caller
   to free and the window is left holding nothing, so a stale reader gets
   NULL and not a freed block. */
struct BFF *e_window_release_text(struct FNST *f, struct SCHRM **s);
struct FLBFF *e_window_release_files(struct FNST *f);
struct fl_wnd *e_window_release_list(struct FNST *f);

/* Freeing the block itself, once the payload has been released: the last
   call before the window's own structure is freed, so a window that is
   closed leaves nothing behind.  Safe for a window that holds nothing. */
void e_window_drop(struct FNST *f);

/* Windows chosen by kind, never by title. */
struct FNST *e_window_top(const struct CNT *cn);
struct FNST *e_window_text_under(const struct CNT *cn);
struct FNST *e_window_top_except_pane(const struct CNT *cn, WpePane pane);
struct FNST *e_window_pane_find(const struct CNT *cn, WpePane pane);

/* The slot of the remembered list a project list kind reads and writes.
   Return: 0 to 2 for the three project lists, -1 for any other kind. */
int e_project_list_slot(WpeListKind kind);

/* A user command that works on text, asked of the window on top.  When that
   window holds no text the command says "<action> needs a text window" on
   the status row, once for this key, and does nothing at all: no box, no
   beep, no change to a file the user is not looking at.
   Return: the window on top, or NULL when the command was refused. */
struct FNST *e_action_text_window(struct FNST *f, const char *action);

/* The refusal's voice, given once at start by the editor, the same way the
   screen's owner is told how to see the desk.  This owner knows which window
   holds no text; the closed-window owner knows how a refusal is worded and
   kept to one line per key.  A test gives its own voice and records what it
   was asked to say, so neither owner has to link the other. */
void e_window_refusal_voice_set(void (*say)(struct FNST *f, const char *action));

/* The window stack and its count.  Every change of what is on top -- a
   window pushed, popped or raised, and a reorder that leaves the number of
   windows alone -- is told to this owner, which counts.  A key loop that
   waited for a key compares the count it saw before the wait: a count, not
   a pointer, because a list window closed and another one opened can get
   the same address from the allocator. */
void e_window_stack_changed(void);
unsigned long e_window_top_count(void);

#endif /* WE_WINKIND_H */
