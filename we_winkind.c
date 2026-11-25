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
