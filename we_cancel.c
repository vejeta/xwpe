/* we_cancel.c -- the one owner of the key that ends a running activity.
   See we_cancel.h for what the user gets from it.

   Copyright (C) 2026 Juan Manuel Méndez Rey
   This file is part of xwpe and is distributed under the GNU General Public
   License, version 2 or later. */

#include "messages.h"
#include "edit.h"
#include "we_cancel.h"
#include "we_ai.h"        /* the assistant's claims (guarded by WPE_AI) */

/* How the trace names the key that arrived: what the user pressed, and the
   Esc that a click on the bottom row comes back as. */
#define E_CANCEL_KEY_ESC      "esc"
#define E_CANCEL_KEY_CTRL_C   "ctrl-c"
#define E_CANCEL_KEY_CLICK    "esc-click"
/* What the trace adds when the key ended nothing: the claim's end was
   already on its way, or its cancel delivered a result that had ended
   behind a box instead of cancelling. */
#define E_CANCEL_STATE_ENDING "ending"
#define E_CANCEL_STATE_HELD   "held"
/* How many claims the table holds. */
#define E_CANCEL_CLAIM_COUNT  (sizeof wpe_cancel_claims / sizeof wpe_cancel_claims[0])

/* What the cancel just called did, for the trace line that follows it.  Two
   claims (the build and the debugger start) can find their result waiting
   behind a box: their cancel then delivers that result instead of ending
   anything, and their adapter says so here.  Written and read inside one
   call of e_cancel_key, in the editor's key loop. */
static int e_cancel_delivered_held;

/**
 * e_cancel_build - End the build the user is waiting for.
 * @top: the window on top; the build writes its own.
 */
static void e_cancel_build(FENSTER *top)
{
 (void)top;
 e_cancel_delivered_held = e_build_result_held();
 e_build_cancel();
}

/**
 * e_cancel_debugger_start - End a debugger that has not answered.
 * @top: the window on top; the start writes its own.
 */
static void e_cancel_debugger_start(FENSTER *top)
{
 (void)top;
 e_cancel_delivered_held = e_d_start_result_held();
 e_d_start_cancel();
}

/**
 * e_cancel_scala_setup - End the scala-cli setup that runs before Metals or
 * a Scala debug session.  One claim, two setups: the language server's and
 * the debugger's both go, as they did before the table.
 * @top: the window on top; the setup writes its own.
 */
static void e_cancel_scala_setup(FENSTER *top)
{
 (void)top;
 e_scala_setup_cancel();
}

/**
 * e_cancel_build_server - Give up on the Scala build server's answer.
 * @top: the window on top; the build server writes its own.
 */
static void e_cancel_build_server(FENSTER *top)
{
 (void)top;
 e_d_bsp_cancel();
}

/**
 * e_cancel_language_server - Give up on the answer a language server owes.
 * @top: the window on top; the answer was asked from another one.
 */
static void e_cancel_language_server(FENSTER *top)
{
 (void)top;
 e_lsp_answer_cancel();
}

/**
 * e_cancel_compile_for_debug - End the compile that a debug session waits for.
 * @top: the window on top; the compile writes its own.
 */
static void e_cancel_compile_for_debug(FENSTER *top)
{
 (void)top;
 e_d_compile_cancel();
}

/**
 * e_cancel_debugger_answer - Give up on the debugger's answer at a stop.
 * @top: the window on top; the answer was asked from another one.
 */
static void e_cancel_debugger_answer(FENSTER *top)
{
 (void)top;
 e_d_answer_cancel();
}

#ifdef WPE_AI
/**
 * e_cancel_assistant_task - End the assistant's background task.
 * @top: the window on top, which the task may have locked.
 */
static void e_cancel_assistant_task(FENSTER *top)
{
 (void)top;
 wpe_ai_cancel();
}

/**
 * e_cancel_assistant_turn - Ask the hosted engine to stop its turn, keeping
 * the session alive for the next task.
 * @top: the window on top; the turn writes to its pane.
 */
static void e_cancel_assistant_turn(FENSTER *top)
{
 (void)top;
 wpe_ai_host_interrupt();
}
#endif

/* The claims, in the order one key walks them.  A row sits under the guard
   of the module that owns the activity, so a build without a feature has no
   claim it could not answer.  The fields say what the user sees: what names
   the activity in its lines and in the trace, row_wait a waiting line that
   ticks on the bottom row, front_only a claim that answers a key typed in
   its own window only. */
static const WpeCancelClaim wpe_cancel_claims[] = {
#ifdef PROG
 { "the build", WPE_CANCEL_ESC, 0, 0,
   e_build_busy, e_build_ending, e_messages_window_on_top, e_cancel_build },
#endif
#ifdef DEBUGGER
 { "the debugger start", WPE_CANCEL_ESC, 0, 0,
   e_d_start_busy, e_d_start_ending, e_messages_window_on_top,
   e_cancel_debugger_start },
 { "scala-cli setup", WPE_CANCEL_ESC, 0, 0,
   e_scala_setup_busy_any, NULL, e_messages_window_on_top,
   e_cancel_scala_setup },
 { "the build server", WPE_CANCEL_ESC, 1, 0,
   e_d_bsp_busy, NULL, NULL, e_cancel_build_server },
 { "the language server", WPE_CANCEL_ESC, 1, 0,
   e_lsp_answer_busy, NULL, NULL, e_cancel_language_server },
 { "the compile for debug", WPE_CANCEL_ESC, 0, 0,
   e_d_compile_busy, NULL, e_messages_window_on_top,
   e_cancel_compile_for_debug },
 { "the debugger's answer", WPE_CANCEL_ESC | WPE_CANCEL_CTRL_C, 1, 0,
   e_d_answer_busy, NULL, NULL, e_cancel_debugger_answer },
#endif
#ifdef WPE_AI
 { "the assistant's task", WPE_CANCEL_ESC, 0, 0,
   wpe_ai_busy, NULL, wpe_ai_task_in_front, e_cancel_assistant_task },
 { "the assistant's turn", WPE_CANCEL_ESC, 0, 0,
   wpe_ai_host_turn_active, wpe_ai_host_interrupt_pending,
   wpe_ai_chat_pane_in_front, e_cancel_assistant_turn },
 { "the chat pane", WPE_CANCEL_ESC, 0, 1,
   wpe_ai_chat_pane_open, NULL, wpe_ai_chat_pane_in_front,
   wpe_ai_chat_pane_close },
#endif
};

/**
 * e_cancel_key_bit - The bit of @c in the table's key masks.
 * @c: the key the reader returned.
 * Return: WPE_CANCEL_ESC, WPE_CANCEL_CTRL_C, or 0 for a key no claim lists.
 */
static unsigned e_cancel_key_bit(int c)
{
 if (c == WPE_ESC)
  return(WPE_CANCEL_ESC);
 if (c == CtrlC)
  return(WPE_CANCEL_CTRL_C);
 return(0);
}

/**
 * e_cancel_key_name - How the trace names the key that arrived.
 * @bit: the key's bit; @by_click: this Esc is a click on the bottom row,
 * which the display's reader reports as one.
 * Return: the trace's word for it.
 */
static const char *e_cancel_key_name(unsigned bit, int by_click)
{
 if (bit == WPE_CANCEL_CTRL_C)
  return(E_CANCEL_KEY_CTRL_C);
 return(by_click ? E_CANCEL_KEY_CLICK : E_CANCEL_KEY_ESC);
}

/**
 * e_cancel_claim_in_front - Is this claim's promise in the window on top?
 * @claim: the claim; @top: the window on top.
 * Return: 1 when the claim says where its promise is and the user sees it.
 */
static int e_cancel_claim_in_front(const WpeCancelClaim *claim,
                                   const FENSTER *top)
{
 return(claim->in_front != NULL && claim->in_front(top));
}

/**
 * e_cancel_claim_takes - Does this claim answer the key?
 * @claim: the claim; @top: the window on top; @bit: the key's bit.
 *
 * A claim answers when it lists the key and has something to end.  One that
 * is only for its own window answers there alone: from any other window the
 * key goes on to the next claim, so Esc in a file never closes a pane the
 * user is not looking at.
 * Return: 1 when this claim takes the key.
 */
static int e_cancel_claim_takes(const WpeCancelClaim *claim,
                                const FENSTER *top, unsigned bit)
{
 if ((claim->keys & bit) == 0)
  return(0);
 if (claim->busy == NULL || !claim->busy())
  return(0);
 if (claim->front_only && !e_cancel_claim_in_front(claim, top))
  return(0);
 return(1);
}

/**
 * e_cancel_next - The claim this key ends.
 * @top: the window on top; @c: the key the reader returned.
 *
 * The walk is the table's order: the first claim that lists the key and has
 * something to end.  Nothing a claim owns is read by another, so the order
 * in the table is the whole answer to "which of two running activities does
 * Esc end".
 * Return: the claim, or NULL when the key is the editor's.
 */
const WpeCancelClaim *e_cancel_next(const FENSTER *top, int c)
{
 unsigned bit = e_cancel_key_bit(c);
 size_t i;

 if (bit == 0)
  return(NULL);
 for (i = 0; i < E_CANCEL_CLAIM_COUNT; i++)
 {
  if (e_cancel_claim_takes(&wpe_cancel_claims[i], top, bit))
   return(&wpe_cancel_claims[i]);
 }
 return(NULL);
}

/**
 * e_cancel_is_next - Would Esc end the claim named @what before any other?
 * @top: the window on top; @what: a claim's name as the table spells it.
 *
 * A line that promises the key asks this first, so what the bottom row says
 * and what Esc does cannot drift apart.
 * Return: 1 when Esc ends @what now.
 */
int e_cancel_is_next(const FENSTER *top, const char *what)
{
 const WpeCancelClaim *claim = e_cancel_next(top, WPE_ESC);

 return(claim != NULL && strcmp(claim->what, what) == 0);
}

/**
 * e_cancel_trace - Say in the trace which claim took the key.
 * @claim: the claim; @key_name: the trace's word for the key; @state: what
 * the key did not do, or NULL for a cancel that was asked of the module.
 *
 * Written only while XWPE_UI_TRACE names a file, as every trace line of the
 * editor is.  A row reads there which of two running activities the key
 * ended; a user chasing "why did nothing happen" reads that the end was
 * already on its way.
 */
static void e_cancel_trace(const WpeCancelClaim *claim, const char *key_name,
                           const char *state)
{
 if (state == NULL)
  wpe_ui_trace("e_cancel_key what=%s key=%s", claim->what, key_name);
 else
  wpe_ui_trace("e_cancel_key what=%s key=%s %s", claim->what, key_name, state);
}

/**
 * e_cancel_key - One key the editor read: does it end a running activity?
 * @top: the window on top; @c: the key; @by_click: this Esc is a click on
 * the bottom row past the last hint, which the display's reader reports as
 * an Esc.
 *
 * The one place a key ends something the user started.  A claim whose end is
 * already on its way keeps the key and does nothing: the user asked for that
 * end once, and answering a second ask would end an activity he can still
 * see running.  A key no claim takes is the editor's, and goes on to the
 * text, to a menu or to the walk out of the loop.
 * Return: 1 when the key was taken.
 */
int e_cancel_key(FENSTER *top, int c, int by_click)
{
 const WpeCancelClaim *claim = e_cancel_next(top, c);
 const char *key_name;
 int held;

 if (claim == NULL)
  return(0);
 key_name = e_cancel_key_name(e_cancel_key_bit(c), by_click);
 if (claim->ending != NULL && claim->ending())
 {
  e_cancel_trace(claim, key_name, E_CANCEL_STATE_ENDING);
  return(1);
 }
 e_cancel_delivered_held = 0;
 claim->cancel(top);
 held = e_cancel_delivered_held;
 e_cancel_trace(claim, key_name, held ? E_CANCEL_STATE_HELD : NULL);
 return(1);
}
