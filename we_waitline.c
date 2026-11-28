/* we_waitline.c -- the one owner of the waiting lines at the foot of Messages. */
/* Copyright (C) 2026 Juan Manuel Mendez Rey                    */
/* This is free software; you can redistribute it and/or        */
/* modify it under the terms of the                             */
/* GNU General Public License, see the file COPYING.            */
/*
   What the user gets: a build and a starting debugger may run together, and
   both lines ("building ... 4s", "starting dlv ... 6s") stay on screen, each
   with its own seconds, until their own wait ends.  No output, no empty and no
   Esc takes another activity's line, and no program output is written into a
   waiting line or deleted with it.

   The rule that keeps it so: a row of Messages is removed or rewritten here
   only, only for the waiter that owns it, and only while it still reads
   exactly as that waiter's text.  The text of every laid waiter is kept
   whole, so the foot is found from the bottom of the buffer by comparing, and
   never by a row number that something else could have moved.  Writers of
   output bracket their append with wpe_wait_lift and wpe_wait_lay, so output
   never lands below a waiting line. */

#include <stdarg.h>
#include <string.h>

#include "edit.h"
#include "we_buffer.h"
#include "we_lineend.h"
#include "we_rowread.h"
#include "we_cancel.h"
#include "we_fmt.h"
#include "we_paint.h"
#include "we_waitline.h"

extern BUFFER *e_p_m_buffer;

/* Where an unknown claim stands in the foot: above every claim of the table. */
#define WAIT_RANK_UNKNOWN 0x7fffffff

/* The live waiters, in the order they began, and how deep the output
   primitives' brackets are (the foot is out of the buffer while it is > 0). */
static WpeWait *g_waiters[WPE_WAIT_MAX];
static int g_nwaiters;
static int g_lift_depth;

/**
 * wait_rank - Where a waiter stands in the Esc order.
 * @w: a waiter.
 * Return: the claim's place in the table (0 is ended first), or a number
 * above every claim when the table does not name it.
 */
static int wait_rank(const WpeWait *w)
{
 int rank = e_cancel_rank(w->what);

 if (rank == WPE_CANCEL_NO_RANK)
  return(WAIT_RANK_UNKNOWN);
 return(rank);
}

/**
 * wait_foot_order - The live waiters in the order their lines are laid.
 * @order: receives indexes into g_waiters, the top line first.
 * Return: how many.
 *
 * What the user reads: the bottom line is the one the next Esc ends, so the
 * highest rank is laid first.  Two waiters of one rank keep the order they
 * began in.
 */
static int wait_foot_order(int *order)
{
 int i, j, k;

 for (i = 0; i < g_nwaiters; i++)
 {
  for (j = i; j > 0; j--)
  {
   k = order[j - 1];
   if (wait_rank(g_waiters[k]) >= wait_rank(g_waiters[i]))
    break;
   order[j] = k;
  }
  order[j] = i;
 }
 return(g_nwaiters);
}

/**
 * wait_take_out - Remove the laid waiters' rows from the bottom of @b.
 * @b: the Messages text.
 * Return: the rows removed.
 *
 * The rows are looked for from the bottom, in the foot's order, and each one
 * is removed only while it reads as its waiter's own text.  A row that does
 * not (the user typed into it, a command took it) is left as it is, and that
 * waiter is marked not laid, so its line is laid again as a new row.
 */
static int wait_take_out(BUFFER *b)
{
 int order[WPE_WAIT_MAX], n, k, y, taken = 0;
 WpeWait *w;

 n = wait_foot_order(order);
 y = b->mxlines - 1;
 for (k = n - 1; k >= 0; k--, y--)
 {
  w = g_waiters[order[k]];
  if (!w->laid)
  {
   y++;                 /* no row of its own: the row stays for the next */
   continue;
  }
  w->laid = 0;
  if (!wpe_row_is(b, y, w->text))
   continue;
  if (e_buffer_delete_lines(b, y, 1, E_BUFFER_STEP_NEW) == E_BUFFER_OK)
   taken++;
 }
 return(taken);
}

/**
 * wait_put_back - Append every live waiter's line at the end of @b, in the
 * foot's order.
 * @b: the Messages text.
 */
static void wait_put_back(BUFFER *b)
{
 int order[WPE_WAIT_MAX], n, k;
 WpeWait *w;

 n = wait_foot_order(order);
 for (k = 0; k < n; k++)
 {
  w = g_waiters[order[k]];
  if (w->text[0] == '\0')
   continue;
  if (e_eol_append_rows(b, w->text, E_EOL_NORMALIZE) != E_BUFFER_OK)
   continue;
  w->laid = 1;
  wpe_ui_trace("waitline laid %s row=%d", w->what, b->mxlines - 1);
 }
}

/**
 * wait_relay - Take the foot out and lay it again, so every line is in its
 * place.
 * @b: the Messages text, or NULL when it does not exist yet.
 *
 * Nothing is done while an output primitive has the foot out; its lay puts
 * the lines back.
 */
static void wait_relay(BUFFER *b)
{
 if (b == NULL || g_lift_depth > 0)
  return;
 wait_take_out(b);
 wait_put_back(b);
}

/**
 * wait_paint - Show the latest rows of Messages.
 *
 * One repaint for a change of a waiting line.  Under a box or a question the
 * paint waits for the screen's owner (e_messages_show_tail asks it); the text
 * may change under a box, the screen may not.
 */
static void wait_paint(void)
{
 if (g_lift_depth > 0 || WpeEditor == NULL || WpeEditor->mxedt < 1)
  return;
 e_messages_show_tail(WpeEditor);
}

/**
 * wait_index - The place of @w among the live waiters.
 * Return: the index, or -1 when @w is not live.
 */
static int wait_index(const WpeWait *w)
{
 int i;

 for (i = 0; i < g_nwaiters; i++)
  if (g_waiters[i] == w)
   return(i);
 return(-1);
}

/**
 * wait_row_of - The row of @b a laid waiter's line is in now.
 * @b: the Messages text; @w: a laid waiter.
 * Return: the row, or -1 when its place in the foot is not a row.
 *
 * It is counted from the bottom: one row for each laid waiter below it in
 * the foot's order.  The caller checks that the row reads as the text.
 */
static int wait_row_of(const BUFFER *b, const WpeWait *w)
{
 int order[WPE_WAIT_MAX], n, k, below = 0;

 n = wait_foot_order(order);
 for (k = n - 1; k >= 0 && g_waiters[order[k]] != w; k--)
  if (g_waiters[order[k]]->laid)
   below++;
 if (k < 0)
  return(-1);
 return(b->mxlines - 1 - below);
}

/**
 * wpe_wait_begin - A waiter arrives.
 * @w: its handle, made with WPE_WAIT.
 *
 * It has no line until it first speaks.  A waiter already live, or one past
 * the owner's room, is traced and left as it was.
 */
void wpe_wait_begin(WpeWait *w)
{
 if (w->live)
 {
  wpe_ui_trace("waitline begin %s refused: already live", w->what);
  return;
 }
 if (g_nwaiters >= WPE_WAIT_MAX)
 {
  wpe_ui_trace("waitline begin %s refused: no room", w->what);
  return;
 }
 w->live = 1;
 w->laid = 0;
 w->text[0] = '\0';
 g_waiters[g_nwaiters++] = w;
 wpe_ui_trace("waitline begin %s", w->what);
}

/**
 * wait_rewrite_own_row - Change the text of a laid line in place.
 * @b: the Messages text; @w: its waiter; @old: what it said before.
 * Return: 1 when the row was found as @old and is now @w's text, 0 when the
 * foot has to be laid again.
 */
static int wait_rewrite_own_row(BUFFER *b, WpeWait *w, const char *old)
{
 int y = wait_row_of(b, w);

 if (!wpe_row_is(b, y, old))
  return(0);
 if (strcmp(old, w->text) == 0)
  return(1);
 return(e_buffer_set_line(b, y, w->text, (int)strlen(w->text)) == E_BUFFER_OK);
}

/**
 * wpe_wait_say - A waiter's line, as it reads now.
 * @w: the waiter; @fmt: the text, as printf takes it.
 *
 * What the user sees: the line turns (its seconds count) without moving, and
 * the lines of the other waiters stay where they are.  With Messages closed
 * the text is kept, and laid when the window opens.
 */
void wpe_wait_say(WpeWait *w, const char *fmt, ...)
{
 char old[WPE_WAIT_LINE_MAX];
 va_list ap;
 BUFFER *b = e_p_m_buffer;

 if (!w->live)
 {
  wpe_ui_trace("waitline say %s refused: not live", w->what);
  return;
 }
 WPE_FMT_SHOWN(old, "%s", w->text);
 va_start(ap, fmt);
 wpe_fmt_vshown(w->text, sizeof w->text, fmt, ap);
 va_end(ap);
 if (g_lift_depth > 0 || b == NULL)
  return;
 if (w->laid && wait_rewrite_own_row(b, w, old))
 {
  if (strcmp(old, w->text) != 0)
   wait_paint();
  return;
 }
 wait_relay(b);
 wait_paint();
}

/**
 * wait_forget - Take a waiter out of the live list.
 * @w: a live waiter.
 */
static void wait_forget(WpeWait *w)
{
 int i = wait_index(w);

 for (; i < g_nwaiters - 1; i++)
  g_waiters[i] = g_waiters[i + 1];
 g_nwaiters--;
 w->live = 0;
 w->laid = 0;
 w->text[0] = '\0';
}

/**
 * wpe_wait_end - A waiter's wait is over.
 * @w: the waiter.
 *
 * Its line goes, the lines of the others close up, and Messages is drawn
 * without it.  Ending a waiter that is not live does nothing and is traced.
 */
void wpe_wait_end(WpeWait *w)
{
 BUFFER *b = e_p_m_buffer;

 if (!w->live)
 {
  wpe_ui_trace("waitline end %s refused: not live", w->what);
  return;
 }
 if (b != NULL && g_lift_depth == 0)
  wait_take_out(b);
 wait_forget(w);
 if (b != NULL && g_lift_depth == 0)
  wait_put_back(b);
 wpe_ui_trace("waitline end %s", w->what);
 wait_paint();
}

/**
 * wpe_wait_shown - Is the waiter's line in Messages now?
 * @w: the waiter.
 */
int wpe_wait_shown(const WpeWait *w)
{
 return(w->live && w->laid);
}

/**
 * wpe_wait_lift - Take the waiting lines out of Messages before output is
 * appended.
 * @b: the buffer the writer appends to.
 *
 * A writer of output calls this before it appends and wpe_wait_lay after, so
 * the last row is output again for the time between (which keeps a program's
 * half line a half line), and the output lands above the foot.  Another
 * buffer is not Messages and is left alone.
 * Return: the rows taken out.
 */
int wpe_wait_lift(BUFFER *b)
{
 if (b == NULL || b != e_p_m_buffer)
  return(0);
 g_lift_depth++;
 if (g_lift_depth > 1)
  return(0);
 return(wait_take_out(b));
}

/**
 * wpe_wait_lay - Put the waiting lines back after output was appended.
 * @b: the buffer the writer appended to.
 */
void wpe_wait_lay(BUFFER *b)
{
 if (b == NULL || b != e_p_m_buffer || g_lift_depth == 0)
  return;
 g_lift_depth--;
 if (g_lift_depth > 0)
  return;
 wait_put_back(b);
}

/**
 * wpe_wait_foot_rows - How many rows of the foot @b holds now.
 * @b: a buffer.
 * Return: the laid lines when @b is the Messages text, else 0.
 *
 * A writer that records where its own rows begin counts the output rows
 * only: the rows above the foot.
 */
int wpe_wait_foot_rows(const BUFFER *b)
{
 int i, rows = 0;

 if (b == NULL || b != e_p_m_buffer)
  return(0);
 for (i = 0; i < g_nwaiters; i++)
  if (g_waiters[i]->laid)
   rows++;
 return(rows);
}

/**
 * wpe_wait_messages_clear - Empty Messages, keeping the waiting lines.
 * @b: the buffer to empty.
 *
 * What the user gets: a new build, or an assistant run, starts Messages
 * afresh and the line of a debugger that is still starting is back at once,
 * not at its next tick.
 * Return: as e_buffer_clear.
 */
int wpe_wait_messages_clear(BUFFER *b)
{
 int i, r;

 if (b == NULL)
  return(E_BUFFER_OK);
 r = e_buffer_clear(b);
 if (b != e_p_m_buffer)
  return(r);
 for (i = 0; i < g_nwaiters; i++)
  g_waiters[i]->laid = 0;
 if (r == E_BUFFER_OK && g_lift_depth == 0)
  wait_put_back(b);
 wpe_ui_trace("waitline cleared, %d line(s) laid again", wpe_wait_foot_rows(b));
 return(r);
}

/**
 * wpe_wait_messages_opened - Messages was (re)opened on its text.
 * @b: its buffer.
 *
 * A line said while the window was closed is in the text now, and one that
 * was not laid yet is laid here, so the window shows every waiter once.
 */
void wpe_wait_messages_opened(BUFFER *b)
{
 if (b == NULL || b != e_p_m_buffer)
  return;
 wait_relay(b);
 wpe_wait_trace_all();
}

/**
 * wpe_wait_trace_all - One trace line for each live waiter.
 *
 * For a row that asks what the owner holds when a screen is wrong.
 */
void wpe_wait_trace_all(void)
{
 int i;

 for (i = 0; i < g_nwaiters; i++)
  wpe_ui_trace("waitline what=%s laid=%d text=%s", g_waiters[i]->what,
               g_waiters[i]->laid, g_waiters[i]->text);
}
