/* we_cancel.h -- one owner of the key that ends a running activity.
   Copyright (C) 2026 Juan Manuel Méndez Rey
   This is free software; see the file COPYING.

   A build, a debugger that starts, a language server's answer, an
   assistant's task and the chat pane all promise the user the same key:
   Esc ends me.  Which of them one Esc ended was a chain of tests in the
   editor's key loop, so an activity had to add itself to the chain to be
   reachable at all, and nothing but the chain's order said which of two
   running activities the key would end.

   What the user gets from this owner:

   * one Esc ends exactly one activity, the first claim the stated order
     reaches, and a key it takes never reaches the text;
   * an activity whose end is already on its way keeps the key until it is
     gone, so a nervous second Esc ends nothing else;
   * a box, a menu or the chat pane keeps its own Esc: it closes what the
     user is looking at and ends no activity behind it;
   * an activity that promises the key without a claim of its own, or a
     claim whose promise is gone, is named by tests/check-esc-owner.sh.

   The claims are one static table: the order is the point, and a table is
   the order written in one place.  Each activity keeps its own cancel; the
   table only names it. */

#ifndef WE_CANCEL_H
#define WE_CANCEL_H

struct FNST;

/* The keys a claim may list.  Esc is every claim's; Ctrl-C is the one
   debugger wait that answers with it (elsewhere Ctrl-C is Copy). */
#define WPE_CANCEL_ESC     1u
#define WPE_CANCEL_CTRL_C  2u

/* One claim on the key: one activity the user can end with it. */
typedef struct
{
 const char *what;        /* "the build": its lines and the trace name it */
 unsigned    keys;        /* WPE_CANCEL_ESC, or with WPE_CANCEL_CTRL_C    */
 int         row_wait;    /* its waiting line ticks on the bottom row     */
 int         front_only;  /* it claims the key in its own window only     */
 int       (*busy)(void); /* is there something to end                    */
 /* Its cancel was asked and the end is not over yet, or NULL for a claim
    that is gone as soon as its cancel returns. */
 int       (*ending)(void);
 /* Its promise is in the window on top, or NULL for a claim that is
    reached from any window. */
 int       (*in_front)(const struct FNST *top);
 void      (*cancel)(struct FNST *top);
} WpeCancelClaim;

/* The one place a key ends a running activity.  @top: the window on top;
   @c: the key; @by_click: the Esc is a click on the bottom row, which the
   display's reader reports as one.  Return: 1 when the key was taken. */
int e_cancel_key(struct FNST *top, int c, int by_click);
/* The claim the key would end now, or NULL.  For lines and for rows. */
const WpeCancelClaim *e_cancel_next(const struct FNST *top, int c);
/* Would Esc end the claim named @what first? */
int e_cancel_is_next(const struct FNST *top, const char *what);
/* Where the claim named @what stands in the table: 0 is the first one a key
   walks, so a lower number is ended sooner.  WPE_CANCEL_NO_RANK for a name the
   table does not have.  Read only: the table stays the one order. */
#define WPE_CANCEL_NO_RANK (-1)
int e_cancel_rank(const char *what);

#endif /* WE_CANCEL_H */
