/* we_closed.c -- the one owner of "this window refuses this edit".
   See we_closed.h for what counts as closed and what is said.

   Copyright (C) 2026 Juan Manuel Mendez Rey
   This file is part of xwpe and is distributed under the GNU General Public
   License, version 2 or later. */

#include "edit.h"
#include "we_ai.h"
#include "we_buffer.h"
#include "we_closed.h"

/* The latch of one input event.  A command made of several primitives, or a
   paste that arrives one character at a time, then says its line once.
   closed_in_event: a key has been read and its command is running.
   closed_said_for_this_key: a refusal was said in this event.
   closed_paste_seen: the key loop saw a bracketed paste at its last read. */
static int closed_in_event;
static int closed_said_for_this_key;
static int closed_paste_seen;

/**
 * wpe_window_closed_why - Why is this window closed to the user's changes?
 * @f: the window about to be changed.
 *
 * Asks only: says and traces nothing.  The one place that compares the
 * window's flag with WPE_INS_READONLY or asks whether an assistant reply is
 * pending for it.
 * Return: the reason, or WPE_CLOSED_NO when the user may change the text.
 */
WpeClosedWhy wpe_window_closed_why(FENSTER *f)
{
 if (!f)
  return(WPE_CLOSED_NO);
 if (f->ins == WPE_INS_READONLY)
  return(WPE_CLOSED_READ_ONLY);
#ifdef WPE_AI
 if (wpe_ai_window_locked(f))
  return(WPE_CLOSED_ASSISTANT);
#endif
 return(WPE_CLOSED_NO);
}

/**
 * wpe_window_closed - Is this window closed to the user's changes?
 * @f: the window.
 *
 * For the state dump, the padlock and a command that chooses its route; the
 * answer is silent.
 */
int wpe_window_closed(FENSTER *f)
{
 return(wpe_window_closed_why(f) != WPE_CLOSED_NO);
}

/**
 * wpe_window_read_only - Is this window closed by its flag alone?
 * @f: the window.
 *
 * What the insert and overwrite toggles ask: the flag shares a field with
 * the insert mode, so flipping it would open a closed window.  A window
 * waiting for a reply is not read-only, only busy.
 */
int wpe_window_read_only(FENSTER *f)
{
 return(wpe_window_closed_why(f) == WPE_CLOSED_READ_ONLY);
}

/**
 * wpe_closed_key_read - The key loop has read the next key.
 *
 * The latch covers one input event.  What the user sees: every refused key
 * gets its line, even right after another refused key; a long paste into a
 * closed window gets one line, not one repaint per character, and that line
 * is the paste's own, not the one said for a letter typed before it.  Only
 * the characters of a bracketed paste after its first share the latch, and
 * the line said for the paste stays on the row while it streams.
 * Return: 1 when this key goes on a paste already refused (its line is to
 * stay), 0 for any other key.
 */
int wpe_closed_key_read(void)
{
 extern int g_bracketed_paste;   /* set while a terminal paste streams */
 int goes_on = g_bracketed_paste && closed_paste_seen && closed_said_for_this_key;

 if (!(g_bracketed_paste && closed_paste_seen))
  closed_said_for_this_key = 0;
 closed_paste_seen = g_bracketed_paste;
 closed_in_event = 1;
 return(goes_on);
}

/**
 * wpe_closed_key_wait - The key loop is about to wait for the next key.
 *
 * The event is over.  What the user sees: a refusal said while the editor
 * waits (an assistant reply landing in a window, the buffer's backstop on an
 * asynchronous edit) is always said, whatever a key refused earlier for
 * another reason.  The latch stays only across a paste still streaming.
 */
void wpe_closed_key_wait(void)
{
 extern int g_bracketed_paste;

 closed_in_event = 0;
 if (!g_bracketed_paste)
  closed_said_for_this_key = 0;
}

/**
 * wpe_closed_reason_over - A reason for a window's refusals has ended.
 * @why: the reason that ended.
 *
 * What the user sees: the line said for the refusal goes with the reason.
 * Once the assistant's reply has landed or the task is cancelled, the status
 * row no longer claims that the assistant is working; the next repaint of
 * the bar names the keys again.
 */
void wpe_closed_reason_over(WpeClosedWhy why)
{
 int dropped = 0;

 if (why == WPE_CLOSED_ASSISTANT)
  dropped = e_status_drop_held_line(E_SAY_ASSISTANT_BUSY);
 else if (why == WPE_CLOSED_READ_ONLY)
  dropped = e_status_drop_held_line(E_SAY_READ_ONLY);
 if (dropped && WpeEditor && WpeEditor->fb)
 {
  e_pr_uul(WpeEditor->fb);        /* the key hints take the row back */
  e_refresh();
 }
}

/**
 * wpe_closed_say - Say why a window refused a change.
 * @f: the window on top.
 * @why: the reason; WPE_CLOSED_NO says nothing.
 *
 * The one place that spells either line into the status row.  The line stays
 * until the next key, so the repaint that follows the refused command does
 * not take it away before it is read, and the state dump counts it.  Inside
 * a key's event it is said at most once; outside any event it always says.
 */
void wpe_closed_say(FENSTER *f, WpeClosedWhy why)
{
 const char *line;

 if (why == WPE_CLOSED_READ_ONLY)
  line = E_SAY_READ_ONLY;
 else if (why == WPE_CLOSED_ASSISTANT)
  line = E_SAY_ASSISTANT_BUSY;
 else
  return;
 if (closed_in_event)
 {
  if (closed_said_for_this_key)
   return;
  closed_said_for_this_key = 1;
 }
 e_status_say_until_key(f, line);
}

/**
 * wpe_closed_say_needs_text - Say that a command needs a window with text.
 * @f: the window on top; @action: the command's own name as the key reference
 * and the menus print it, or NULL for the generic word.
 *
 * What the user sees: one line on the status row naming the command and what
 * it needs, kept until the next key and said at most once for this key, the
 * same latch the refusals above use, so a prefix letter, a function key and a
 * menu row all answer alike.  The window stays on top: no box to dismiss, no
 * beep, and nothing behind it is changed.
 */
void wpe_closed_say_needs_text(FENSTER *f, const char *action)
{
 char line[E_SAY_NEEDS_TEXT_ROOM];

 if (closed_in_event)
 {
  if (closed_said_for_this_key)
   return;
  closed_said_for_this_key = 1;
 }
 /* A command name too long for the row is left out whole rather than cut,
    and the generic word takes its place: the user still learns that the
    command needs a text window.  When even that does not fit, the row keeps
    what it had. */
 if (WPE_FMT_WHOLE(line, E_SAY_NEEDS_TEXT,
                   action ? action : E_SAY_NEEDS_TEXT_DEFAULT) != WPE_FMT_OK &&
     WPE_FMT_WHOLE(line, E_SAY_NEEDS_TEXT,
                   E_SAY_NEEDS_TEXT_DEFAULT) != WPE_FMT_OK)
  return;
 e_status_say_until_key(f, line);
}

/**
 * wpe_window_refuses_edit - The ask of a command about to change a window.
 * @f: the window the command would change.
 * Return: 1 when @f is closed (the reason is said), 0 when the command may go on.
 */
int wpe_window_refuses_edit(FENSTER *f)
{
 WpeClosedWhy why = wpe_window_closed_why(f);

 if (why == WPE_CLOSED_NO)
  return(0);
 wpe_closed_say(f, why);
 return(1);
}

/**
 * e_edit_window - The window a user edit command acts on, if it may.
 * @f: the window the command came from; @action: the command's own name, as
 * the key reference and the menus print it.
 *
 * The first line of every command that changes text.  What the user gets:
 * the command acts on the window he is looking at, or says why not.  A window
 * that holds no text (the File Manager, a list window, a dropdown) is refused
 * with one line naming the command, and nothing behind it is changed: the old
 * walk down to the first text window is gone, so a paste with the File
 * Manager on top no longer edits a file the user cannot see.
 * Return: the window, or NULL when it holds no text (said) or refuses the
 * edit (said).
 */
FENSTER *e_edit_window(FENSTER *f, const char *action)
{
 FENSTER *top = e_action_text_window(f, action);

 if (top == NULL)
  return(NULL);
 if (wpe_window_refuses_edit(top))
  return(NULL);
 return(top);
}

/**
 * wpe_window_locked_for_now - The old yes-or-no every call site asked.
 * @f: the window about to receive an inserting, deleting or replacing key.
 * Return: 1 when the key must be refused, 0 when the window is editable.
 *
 * Transitional: a read-only window answers in silence, a window waiting for
 * an assistant reply says its line.  Goes away when the key loop and the
 * commands ask wpe_window_refuses_edit.
 */
int wpe_window_locked_for_now(FENSTER *f)
{
 WpeClosedWhy why = wpe_window_closed_why(f);

 if (why == WPE_CLOSED_ASSISTANT)
  wpe_closed_say(f, why);
 return(why != WPE_CLOSED_NO);
}

/**
 * wpe_buffer_say_read_only - Say on the status row that a command was not
 * made because its window is closed to changes (a file opened read-only,
 * the clipboard viewer); the text is as it was.
 * @b: the buffer the command would have changed.
 *
 * The buffer owner's hook: its backstop says through here, and the unit
 * tests of the buffer stub it.
 */
void wpe_buffer_say_read_only(BUFFER *b)
{
 (void) b;
 if (WpeEditor != NULL && WpeEditor->mxedt >= 0)
  wpe_closed_say(WpeEditor->f[WpeEditor->mxedt], WPE_CLOSED_READ_ONLY);
}
