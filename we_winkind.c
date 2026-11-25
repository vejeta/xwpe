/* we_winkind.c -- the one owner of "which kind of window is this, and what
   does it hold".  See we_winkind.h for what the user gets from it.

   Copyright (C) 2026 Juan Manuel Méndez Rey
   This file is part of xwpe and is distributed under the GNU General Public
   License, version 2 or later. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit.h"
#include "we_winkind.h"

/* The block a window's payload lives in.  This is the only file that names
   it, which is the whole point: the three payloads are separate typed
   pointers, so there is no union to reinterpret and a wrong tag cannot turn
   one kind's payload into another's.  The compiler then refuses an alias, a
   macro with code, a pasted name, a helper in another file and a void *
   round trip that tries to read a member from outside. */
struct wpe_window_held {
 BUFFER *text;            /* the text of a file or of a pane: WPE_HOLDS_TEXT */
 SCHIRM *screen;          /* the view state of that text, or NULL */
 FLBFFR *files;           /* the File Manager's lists: WPE_HOLDS_FILES */
 FLWND *list;             /* a list's or a dropdown's rows: _LIST, _DROPDOWN */
 unsigned char holds;     /* WpeHolds: which one of the four is valid */
 unsigned char list_kind; /* WpeListKind: which list, for WPE_HOLDS_LIST */
 unsigned char pane;      /* WpePane: which pane, for WPE_HOLDS_TEXT */
};

/* The window's block, or NULL for a window that holds nothing (and for no
   window at all, so every accessor answers its "not this kind" value
   instead of reading through NULL). */
static struct wpe_window_held *held_of(const FENSTER *f)
{
 return(f ? f->held_ : NULL);
}

/**
 * e_window_holds - What kind of payload a window has.
 * @f: the window.
 *
 * What the user gains: one answer that cannot disagree with the pointer the
 * accessors hand out, because the same block carries both and only the hold
 * functions below write either.
 * Return: the kind, or WPE_HOLDS_NOTHING for a window with no payload.
 */
WpeHolds e_window_holds(const FENSTER *f)
{
 const struct wpe_window_held *h = held_of(f);

 return(h ? (WpeHolds)h->holds : WPE_HOLDS_NOTHING);
}

/**
 * e_window_is_text - Does this window hold text a command can work on?
 * @f: the window.
 *
 * The one question behind every refusal this owner says: a command that
 * inserts, marks, searches or asks a language server answers no here for the
 * File Manager, a list window and a dropdown, and so says one sentence
 * instead of reading a list's corner numbers as a row pointer.
 * Return: non-zero for a text window.
 */
int e_window_is_text(const FENSTER *f)
{
 return(e_window_holds(f) == WPE_HOLDS_TEXT);
}

/**
 * e_window_text - The text a window holds.
 * @f: the window.
 * Return: the text, or NULL when the window holds none.
 */
BUFFER *e_window_text(const FENSTER *f)
{
 const struct wpe_window_held *h = held_of(f);

 return(h && h->holds == WPE_HOLDS_TEXT ? h->text : NULL);
}

/**
 * e_window_screen - The view state of a window's text.
 * @f: the window.
 *
 * A window that holds no text never had one allocated, so the answer is NULL
 * rather than whatever the heap held where the pointer was never set.
 * Return: the view state, or NULL when the window holds no text.
 */
SCHIRM *e_window_screen(const FENSTER *f)
{
 const struct wpe_window_held *h = held_of(f);

 return(h && h->holds == WPE_HOLDS_TEXT ? h->screen : NULL);
}

/**
 * e_window_files - The File Manager's directory lists.
 * @f: the window.
 * Return: the lists, or NULL for any other kind of window.
 */
FLBFFR *e_window_files(const FENSTER *f)
{
 const struct wpe_window_held *h = held_of(f);

 return(h && h->holds == WPE_HOLDS_FILES ? h->files : NULL);
}

/**
 * e_window_list - The rows of a list window or of a dropdown.
 * @f: the window.
 * Return: the rows, or NULL for any other kind of window.
 */
FLWND *e_window_list(const FENSTER *f)
{
 const struct wpe_window_held *h = held_of(f);

 if (!h)
  return(NULL);
 if (h->holds != WPE_HOLDS_LIST && h->holds != WPE_HOLDS_DROPDOWN)
  return(NULL);
 return(h->list);
}

/**
 * e_window_list_kind - Which list a list window shows.
 * @f: the window.
 * Return: the kind, or WPE_LISTKIND_NONE for a window that is not a list
 * window.
 */
WpeListKind e_window_list_kind(const FENSTER *f)
{
 const struct wpe_window_held *h = held_of(f);

 if (!h || h->holds != WPE_HOLDS_LIST)
  return(WPE_LISTKIND_NONE);
 return((WpeListKind)h->list_kind);
}

/**
 * e_window_pane - Which window the editor opened itself.
 * @f: the window.
 *
 * What the user gains: a file the user opened is never taken for the
 * Messages pane, the Watches pane or the clipboard because of the name it
 * happens to carry, so a build parses its errors into its own pane and a
 * file called "Messages" stays the user's writable file.
 * Return: the pane, or WPE_PANE_FILE for a file the user opened.
 */
WpePane e_window_pane(const FENSTER *f)
{
 const struct wpe_window_held *h = held_of(f);

 return(h ? (WpePane)h->pane : WPE_PANE_FILE);
}

/**
 * e_window_is_dock_pane - Is this one of the editor's docked tool strips?
 * @f: the window.
 *
 * What the user gains: a dialog opened over the Messages, Watches, Stack or
 * assistant pane is centred on the whole screen, because those panes are thin
 * strips; a dialog opened over a locked (read-only) file or over the Help
 * viewer is centred on that window like over any other text window.  The
 * read-only flag cannot tell them apart: a locked file and the viewers carry
 * it too.
 * Return: 1 for a dock pane, 0 for every other window.
 */
int e_window_is_dock_pane(const FENSTER *f)
{
 switch (e_window_pane(f))
 {
  case WPE_PANE_MESSAGES:
  case WPE_PANE_WATCHES:
  case WPE_PANE_STACK:
  case WPE_PANE_ASSISTANT:
   return(1);
  default:
   return(0);
 }
}

/* The block a window holds its payload in, made on the first hold.  One
   allocation per window: a failure answers NULL and the constructor says so
   the way it says its own allocation failures. */
static struct wpe_window_held *held_make(FENSTER *f)
{
 struct wpe_window_held *h;

 if (!f)
  return(NULL);
 if (f->held_)
  return(f->held_);
 h = (struct wpe_window_held *)MALLOC(sizeof(*h));
 if (!h)
  return(NULL);
 memset(h, 0, sizeof(*h));
 h->pane = WPE_PANE_FILE;
 f->held_ = h;
 return(h);
}

/**
 * held_no_insert_mode - A window that is not text has no insert mode.
 * @f: the window that was just given its list, lists or rows.
 *
 * The insert mode is the typing mode of text.  A list window used to carry
 * its kind there, and a text command's guard read that number as "insert
 * control characters"; now the kind is held beside the rows and the insert
 * mode stays 0, whatever the constructor wrote before holding.
 */
static void held_no_insert_mode(FENSTER *f)
{
 f->ins = 0;
}

/**
 * e_window_held_forget -A window structure was just allocated: it holds nothing.
 * @f: the window.
 *
 * The editor allocates a window with plain malloc, so every field starts as
 * whatever the memory held before.  This is the owner's own forget, called
 * beside the ones the write owner and the copy owner already have, and it is
 * what keeps the first hold from reading a block that was never made.
 */
void e_window_held_forget(FENSTER *f)
{
 if (f)
  f->held_ = NULL;
}

/**
 * e_window_hold_text - Give a window its text and its view state.
 * @f: the window; @b: the text; @s: the view state.
 *
 * What the user gains: from here on the window answers every text question
 * with this buffer and no other kind's payload can be read as text.
 * Return: 1, or 0 when the block could not be allocated (the window is then
 * left holding nothing, and every accessor answers NULL).
 */
int e_window_hold_text(FENSTER *f, BUFFER *b, SCHIRM *s)
{
 struct wpe_window_held *h = held_make(f);

 if (!h)
  return(0);
 h->text = b;
 h->screen = s;
 h->files = NULL;
 h->list = NULL;
 h->list_kind = WPE_LISTKIND_NONE;
 h->holds = WPE_HOLDS_TEXT;
 return(1);
}

/**
 * e_window_hold_files - Give a window the File Manager's lists.
 * @f: the window; @fb: the lists.
 * Return: 1, or 0 when the block could not be allocated.
 */
int e_window_hold_files(FENSTER *f, FLBFFR *fb)
{
 struct wpe_window_held *h = held_make(f);

 if (!h)
  return(0);
 h->text = NULL;
 h->screen = NULL;
 h->files = fb;
 h->list = NULL;
 h->list_kind = WPE_LISTKIND_NONE;
 h->holds = WPE_HOLDS_FILES;
 held_no_insert_mode(f);
 return(1);
}

/**
 * e_window_hold_list - Give a window its list and say which list it is.
 * @f: the window; @fw: the rows; @kind: which list they are.
 *
 * What the user gains: the kind travels with the rows, so the project's
 * remembered selections, the window list and the two search result lists are
 * told apart by a name and not by a number compared at thirty-eight places.
 * Return: 1, or 0 when the block could not be allocated.
 */
int e_window_hold_list(FENSTER *f, FLWND *fw, WpeListKind kind)
{
 struct wpe_window_held *h = held_make(f);

 if (!h)
  return(0);
 h->text = NULL;
 h->screen = NULL;
 h->files = NULL;
 h->list = fw;
 h->list_kind = (unsigned char)kind;
 h->holds = WPE_HOLDS_LIST;
 held_no_insert_mode(f);
 return(1);
}

/**
 * e_window_hold_dropdown - Give a dropdown its rows.
 * @f: the window; @fw: the rows.
 *
 * A dropdown belongs to the box that opened it: it is never pushed on the
 * window stack, so no key loop is ever chosen for it.
 * Return: 1, or 0 when the block could not be allocated.
 */
int e_window_hold_dropdown(FENSTER *f, FLWND *fw)
{
 struct wpe_window_held *h = held_make(f);

 if (!h)
  return(0);
 h->text = NULL;
 h->screen = NULL;
 h->files = NULL;
 h->list = fw;
 h->list_kind = WPE_LISTKIND_NONE;
 h->holds = WPE_HOLDS_DROPDOWN;
 held_no_insert_mode(f);
 return(1);
}

/**
 * e_window_set_pane - Say which pane the editor is opening.
 * @f: the window; @pane: the pane.
 *
 * The tag is set by the intent that opens the window, at its creation, and
 * never read back from its title: that is what keeps a file the user named
 * "Messages" out of the build's error list.
 */
void e_window_set_pane(FENSTER *f, WpePane pane)
{
 struct wpe_window_held *h = held_make(f);

 if (h)
  h->pane = (unsigned char)pane;
}

/**
 * e_window_replace_text - Give a window another text, keeping its view.
 * @f: the window; @b: the text it shows from now on.
 *
 * What the user gains: the help and the info viewer take the pane's own
 * buffer over the one just allocated without losing where the reader was,
 * and the buffer they gave up is returned to its own owner to free.
 * Return: 1, or 0 when the block could not be allocated.
 */
int e_window_replace_text(FENSTER *f, BUFFER *b)
{
 SCHIRM *s = e_window_screen(f);

 (void)e_window_release_text(f, NULL);
 return(e_window_hold_text(f, b, s));
}

/**
 * e_window_release_text - Take a window's text back at close.
 * @f: the window; @s: where to leave the view state, or NULL to drop it.
 *
 * The window is left holding nothing, so a reader that outlives the close
 * gets NULL and not a freed block.
 * Return: the text for the caller to free, or NULL when there was none.
 */
BUFFER *e_window_release_text(FENSTER *f, SCHIRM **s)
{
 struct wpe_window_held *h = held_of(f);
 BUFFER *b;

 if (s)
  *s = NULL;
 if (!h || h->holds != WPE_HOLDS_TEXT)
  return(NULL);
 b = h->text;
 if (s)
  *s = h->screen;
 h->text = NULL;
 h->screen = NULL;
 h->holds = WPE_HOLDS_NOTHING;
 return(b);
}

/**
 * e_window_release_files - Take the File Manager's lists back at close.
 * @f: the window.
 * Return: the lists for the caller to free, or NULL when there were none.
 */
FLBFFR *e_window_release_files(FENSTER *f)
{
 struct wpe_window_held *h = held_of(f);
 FLBFFR *fb;

 if (!h || h->holds != WPE_HOLDS_FILES)
  return(NULL);
 fb = h->files;
 h->files = NULL;
 h->holds = WPE_HOLDS_NOTHING;
 return(fb);
}

/**
 * e_window_release_list - Take a list's or a dropdown's rows back at close.
 * @f: the window.
 * Return: the rows for the caller to free, or NULL when there were none.
 */
FLWND *e_window_release_list(FENSTER *f)
{
 struct wpe_window_held *h = held_of(f);
 FLWND *fw;

 if (!h)
  return(NULL);
 if (h->holds != WPE_HOLDS_LIST && h->holds != WPE_HOLDS_DROPDOWN)
  return(NULL);
 fw = h->list;
 h->list = NULL;
 h->list_kind = WPE_LISTKIND_NONE;
 h->holds = WPE_HOLDS_NOTHING;
 return(fw);
}

/**
 * e_window_drop - Free the block a window held its payload in.
 * @f: the window, whose own structure the caller frees next.
 *
 * What the user gains: a window that is closed leaves nothing behind, so a
 * long session that opens and closes thousands of windows does not slowly
 * run out of memory.  A window that holds nothing is left alone.
 */
void e_window_drop(FENSTER *f)
{
 if (!f || !f->held_)
  return;
 FREE(f->held_);
 f->held_ = NULL;
}

/**
 * e_window_top - The window the user is looking at.
 * @cn: the desktop.
 * Return: the window on top, or NULL when none is open.
 */
FENSTER *e_window_top(const ECNT *cn)
{
 if (!cn || cn->mxedt < 1 || cn->mxedt > MAXEDT)
  return(NULL);
 return(cn->f[cn->mxedt]);
}

/**
 * e_window_slot - Where a window is on the desktop's stack.
 * @cn: the desktop; @w: the window, or NULL.
 *
 * For a caller that was handed a window by the opener and keeps its places by
 * index: it asks where that very window is and never assumes the window it
 * got is the one on top.
 * Return: the slot (1 to mxedt, the window on top first), or 0 when @w is
 * NULL or not on the stack.
 */
int e_window_slot(const ECNT *cn, const FENSTER *w)
{
 int i;

 if (!cn || !w)
  return(0);
 for (i = cn->mxedt; i > 0; i--)
  if (cn->f[i] == w)
   return(i);
 return(0);
}

/**
 * e_window_text_under - The topmost window that holds text.
 * @cn: the desktop.
 *
 * Asks only: it raises nothing, so a painter or a command that must not move
 * the user's windows can ask which one it would get.
 * Return: that window, or NULL when no open window holds text.
 */
FENSTER *e_window_text_under(const ECNT *cn)
{
 int i;

 if (!cn)
  return(NULL);
 for (i = cn->mxedt; i > 0; i--)
  if (e_window_is_text(cn->f[i]))
   return(cn->f[i]);
 return(NULL);
}

/**
 * e_window_pane_find - The window of a pane the editor opened.
 * @cn: the desktop; @pane: which pane.
 *
 * Found by the tag set at creation and never by the title, so a file the
 * user named after a pane does not answer for it.
 * Return: that window, or NULL when the pane is not open.
 */
FENSTER *e_window_pane_find(const ECNT *cn, WpePane pane)
{
 int i;

 if (!cn || pane == WPE_PANE_FILE)
  return(NULL);
 for (i = cn->mxedt; i > 0; i--)
  if (e_window_pane(cn->f[i]) == pane)
   return(cn->f[i]);
 return(NULL);
}

/* How many bytes of a label a view's title keeps: the room a file name has,
   less its terminator. */
#define LABEL_KEEP (WPE_NAME_SIZE - 1)

/* The part of @label a view's title keeps.  The one cut, so the title made
   and the title compared agree for a label longer than a file name. */
static size_t label_len(const char *label)
{
 size_t len = strlen(label);

 return(len > LABEL_KEEP ? LABEL_KEEP : len);
}

/**
 * e_window_label_title - The title a labelled view carries.
 * @label: the label the editor gave the view.
 *
 * What the user gains: a view's frame shows the label as it was given, with
 * a '/' in it where it has one (a manual page asked for as "a/b"), instead of
 * the end of a path made from it.
 * Return: the title, for the caller to free, or NULL when out of memory.
 */
char *e_window_label_title(const char *label)
{
 size_t len;
 char *title;

 if (!label)
  return(NULL);
 len = label_len(label);
 title = (char *)MALLOC(len + 1);
 if (!title)
  return(NULL);
 memcpy(title, label, len);
 title[len] = '\0';
 return(title);
}

/* Does this window carry @label as the title of a view? */
static int view_has_label(const FENSTER *f, const char *label)
{
 size_t len;

 if (!f || !f->datnam || !label)
  return(0);
 len = label_len(label);
 return(strlen(f->datnam) == len && !memcmp(f->datnam, label, len));
}

/**
 * e_window_is_file - Is this window the user's file at this place?
 * @f: the window; @dir: the folder; @name: the file's own name.
 *
 * What the user gains: opening a file that is already open raises that
 * window and does not make a second one, whatever the file is called.  Only
 * a window that holds the text of a file the user opened answers yes: the
 * editor's own windows (Messages, the clipboard, a manual page) and the lists
 * never do, so a file named like one of them is its own window.
 * Return: 1 for that file's window, else 0.
 */
int e_window_is_file(const FENSTER *f, const char *dir, const char *name)
{
 if (!f || !name || !dir || e_window_pane(f) != WPE_PANE_FILE)
  return(0);
 if (!e_window_is_text(f) || !f->datnam || !f->dirct)
  return(0);
 return(!strcmp(f->datnam, name) && !strcmp(f->dirct, dir));
}

/* Is @path the folder and the name of this window, joined the way a path is
   (one separator between them, none when the folder is empty or ends in one)? */
static int path_joins(const char *dir, const char *name, const char *path)
{
 size_t dlen = strlen(dir);

 if (strncmp(path, dir, dlen) != 0)
  return(0);
 path += dlen;
 if (dlen > 0 && dir[dlen - 1] != '/')
 {
  if (*path != '/')
   return(0);
  path++;
 }
 return(!strcmp(path, name));
}

/**
 * e_window_is_path - Is this window the user's file that a path names?
 * @f: the window; @path: what a compiler, a debugger or a definition names;
 * @whole_path: 1 when @path holds the folder as well (it starts at the root),
 * 0 when it is the file's own name.
 *
 * What the user gets: the line of an error, a stop or a definition opens the
 * file's own window when it is open, whatever the other windows are called;
 * a pane titled like the file is never taken for it.
 * Return: 1 for that file's window, else 0.
 */
int e_window_is_path(const FENSTER *f, const char *path, int whole_path)
{
 if (!f || !path || e_window_pane(f) != WPE_PANE_FILE || !f->datnam ||
     !e_window_is_text(f))
  return(0);
 if (!whole_path)
  return(!strcmp(f->datnam, path));
 return(f->dirct != NULL && path_joins(f->dirct, f->datnam, path));
}

/**
 * e_window_title_text - The title of a window, as text to paint or print.
 * @f: the window.
 *
 * What the user gets: a frame, a list and a message name the window by what
 * its title says.  This is for showing and for joining into a path; whether a
 * window is a file is a question put to the owner, never read from this text.
 * Return: the title, "" when the window has none.
 */
const char *e_window_title_text(const FENSTER *f)
{
 return(f && f->datnam ? f->datnam : "");
}

/**
 * e_window_title_detach - Take the window's title away for the caller to keep.
 * @f: the window.
 *
 * What the user gets: Save As keeps the old name until the file is written, so
 * a write that fails can put it back, and a window with no title never has a
 * made-up one freed.  The window is left with no title and the caller sets the
 * new one at once.
 * Return: the title, owned by the caller, or NULL when the window had none.
 */
char *e_window_title_detach(FENSTER *f)
{
 char *title;

 if (!f)
  return(NULL);
 title = f->datnam;
 f->datnam = NULL;
 return(title);
}

/**
 * e_window_file_name - The name of the file a window holds.
 * @f: the window.
 *
 * What the user gets: a command about files (a breakpoint, a language, a
 * debugger) is answered by a file's own name and never by the title of the
 * editor's own window, so a pane called Messages is not a file named Messages.
 * Return: the name, or NULL for every window that is not a file of the user's.
 */
const char *e_window_file_name(const FENSTER *f)
{
 if (!f || !f->datnam || e_window_pane(f) != WPE_PANE_FILE ||
     !e_window_is_text(f))
  return(NULL);
 return(f->datnam);
}

/* The window of a file the user opened that an ask names: never slot 0 (the
   clipboard), never a window without text, and never the empty name, which is
   a new unnamed file and so always a window of its own. */
static FENSTER *find_file(const ECNT *cn, const WpeWindowAsk *ask)
{
 int i;

 if (!ask->name || !ask->name[0])
  return(NULL);
 for (i = cn->mxedt; i >= 1; i--)
  if (e_window_is_file(cn->f[i], ask->dir, ask->name))
   return(cn->f[i]);
 return(NULL);
}

/* The clipboard is the one window in slot 0; any other pane is on the stack.
   Before the first window is made (mxedt below 0) slot 0 holds nothing yet. */
static FENSTER *find_clipboard(const ECNT *cn)
{
 if (cn->mxedt < 0)
  return(NULL);
 return(e_window_pane(cn->f[0]) == WPE_PANE_CLIPBOARD ? cn->f[0] : NULL);
}

/* The window of a labelled view: its kind and its label. */
static FENSTER *find_view(const ECNT *cn, const WpeWindowAsk *ask)
{
 int i;

 for (i = cn->mxedt; i >= 1; i--)
  if (e_window_pane(cn->f[i]) == ask->pane && view_has_label(cn->f[i], ask->name))
   return(cn->f[i]);
 return(NULL);
}

/**
 * e_window_find - The window an ask names, when it is open.
 * @cn: the desktop; @ask: what is asked for.
 *
 * What the user gains: opening a file that is already open, asking for a
 * manual page that is shown, or writing to the build's Messages, goes to the
 * window that is there and does not make a second one; and a file the user
 * named after a pane is never taken for the pane, nor a pane for the file,
 * because the kind in the ask decides what can answer.  Asks only: it raises
 * nothing and opens nothing.
 * Return: the window, or NULL when none is open.
 */
FENSTER *e_window_find(const ECNT *cn, const WpeWindowAsk *ask)
{
 if (!cn || !ask)
  return(NULL);
 switch (ask->pane)
 {
  case WPE_PANE_FILE:
   return(find_file(cn, ask));
  case WPE_PANE_CLIPBOARD:
   return(find_clipboard(cn));
  case WPE_PANE_COMPOSER:
   return(NULL);                    /* each request writes in its own */
  case WPE_PANE_REPORT:
  case WPE_PANE_MANPAGE:
   return(find_view(cn, ask));
  default:
   return(e_window_pane_find(cn, ask->pane));
 }
}

/**
 * e_window_title_ends_with - Does a window's file name end this way?
 * @f: the window; @suffix: the ending asked about, such as ".sh".
 *
 * What the user gains: a shell script runs as a script and a worksheet gets
 * its inline results, told by the name of the file the user opened.  Only a
 * file of the user's has such a name: the editor's own windows answer no.
 * Return: 1 when the title ends with @suffix, else 0.
 */
int e_window_title_ends_with(const FENSTER *f, const char *suffix)
{
 size_t len, tail;

 if (!f || !f->datnam || !suffix || e_window_pane(f) != WPE_PANE_FILE ||
     !e_window_is_text(f))
  return(0);
 len = strlen(f->datnam);
 tail = strlen(suffix);
 return(len >= tail && !strcmp(f->datnam + len - tail, suffix));
}

/**
 * e_window_title_is - Does a window carry this file name as its title?
 * @f: the window; @name: the file name asked about.
 *
 * What the user gains: a command that asks about a file (a breakpoint, a
 * build's error list, the file under a stop) is answered by the file's own
 * window and never by one of the editor's panes that happens to carry the
 * same words as its title.
 * Return: 1 when the window is a file of the user's titled @name, else 0.
 */
int e_window_title_is(const FENSTER *f, const char *name)
{
 return(f && f->datnam && name && e_window_pane(f) == WPE_PANE_FILE &&
        e_window_is_text(f) && !strcmp(f->datnam, name));
}

/**
 * e_window_top_except_pane - The window the user was in, past one pane.
 * @cn: the desktop; @pane: the pane to walk past.
 *
 * What the user gains: after a build the keys go back to the window the user
 * was in, whatever its kind, and the build's own output window is stepped
 * over by what it is rather than by the name it carries.
 * Return: that window, or NULL when nothing but the pane is open.
 */
FENSTER *e_window_top_except_pane(const ECNT *cn, WpePane pane)
{
 int i;

 if (!cn)
  return(NULL);
 for (i = cn->mxedt; i > 0; i--)
  if (e_window_pane(cn->f[i]) != pane)
   return(cn->f[i]);
 return(NULL);
}

/**
 * e_project_list_slot - The remembered list a project list kind belongs to.
 * @kind: the list kind.
 *
 * What the user gains: the project keeps the selection of each of its three
 * lists, and the slot is asked by name instead of computed from the kind at
 * every reader, so a new list kind cannot index outside the table.
 * Return: 0 to 2 for the three project lists, -1 for any other kind.
 */
int e_project_list_slot(WpeListKind kind)
{
 if (kind < WPE_LISTKIND_PROJECT_FIRST || kind > WPE_LISTKIND_PROJECT_LAST)
  return(-1);
 return(kind - WPE_LISTKIND_PROJECT_FIRST);
}

/* How many times what is on top has changed.  A count and not a pointer: a
   list window closed and another one opened can get the same address from
   the allocator, and a loop that compared pointers would go on with the
   payload it fetched for the old one. */
static unsigned long wpe_top_count;

/**
 * e_window_stack_changed - What is on top has changed.
 *
 * Told by every push, pop, raise and reorder of the window stack, which are
 * the only changes there are.  What the user gains: a key loop that was
 * waiting for a key learns that the window it fetched before the wait is not
 * the one on screen now, leaves, and the dispatcher enters the loop of the
 * window the user is looking at.
 */
void e_window_stack_changed(void)
{
 wpe_top_count++;
}

/**
 * e_window_top_count - How many top-window changes there have been.
 *
 * A display's key wait reads this before it waits and compares after, and
 * answers WPE_TOP_CHANGED when the two differ.
 * Return: the count.
 */
unsigned long e_window_top_count(void)
{
 return(wpe_top_count);
}

/* The refusal's voice, given once at start (e_window_refusal_voice_set).
   Kept as a hook so this owner links into a unit test on its own: it knows
   which window holds no text, and the closed-window owner knows how a
   refusal is worded and kept to one line per key. */
static void (*winkind_say_needs_text)(FENSTER *f, const char *action);

/**
 * e_window_refusal_voice_set - Tell the owner how a refusal is said.
 * @say: the voice, or NULL to take it back.
 *
 * What the user gains: the one line a refused command says is the same
 * whether the command came from a key, a prefix letter or a menu row, because
 * one voice, with its one-line-per-key latch, speaks for all of them.
 */
void e_window_refusal_voice_set(void (*say)(FENSTER *f, const char *action))
{
 winkind_say_needs_text = say;
}

/**
 * e_action_text_window - The window a command that works on text acts on.
 * @f: the window the command came from; @action: the command's own name, as
 * the key reference and the menus print it, or NULL for the generic word.
 *
 * What the user gains: a command acts on the window the user is looking at,
 * or says why not.  With the File Manager, a list window or a dropdown on
 * top the command says "<action> needs a text window" on the status row,
 * once for this key, and changes nothing at all: no box to dismiss, no beep,
 * and no file behind the window is edited that the user cannot see.
 * Return: the window on top, or NULL when the command was refused.
 */
FENSTER *e_action_text_window(FENSTER *f, const char *action)
{
 FENSTER *top;

 if (!f)
  return(NULL);
 top = e_window_top(f->ed);
 if (e_window_is_text(top))
  return(top);
 if (winkind_say_needs_text)
  winkind_say_needs_text(top ? top : f, action);
 return(NULL);
}
