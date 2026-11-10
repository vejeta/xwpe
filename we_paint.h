/* we_paint.h -- the screen has one owner: who may draw, and when.
   Copyright (C) 2026 Juan Manuel Méndez Rey
   This is free software; see the file COPYING.

   Content drawn from outside the user's own keys (a build's output, the
   program's output, a stop, a reply, a language server's answer) is drawn
   only when no box owns the screen and a text window is on top.  What
   arrives while a box is up is kept here and drawn, once, when the editor
   is back in its key loop.  A box, a menu or a prefix tells the owner it
   has the screen through one call.  Display repaints of what is already
   on the screen (an expose, a resize) are the display's and are not
   gated.

   The owner is plain C: it knows the editor's desk only through the hooks
   of WpePaintDesk, so it can be tested on its own. */

#ifndef WE_PAINT_H
#define WE_PAINT_H

/* Distinct paints kept while a box is up; one more is refused, and the
   status row names it once the editor is back. */
#define WPE_PAINT_DEFER_MAX   16
/* Nested boxes; deeper is a bug, written to the trace. */
#define WPE_PAINT_DEPTH_MAX   32
/* Paints run by one replay: one that keeps asking for itself is a bug and
   must not hold the key loop. */
#define WPE_PAINT_REPLAY_MAX  (4 * WPE_PAINT_DEFER_MAX)

typedef void (*WpePaintReplay)(void *data);

typedef enum
{
 WPE_PAINT_NOW,          /* drawn at once */
 WPE_PAINT_DEFERRED,     /* kept; drawn when the editor is back */
 WPE_PAINT_REFUSED       /* the queue was full: named on the status row */
} WpePaintVerdict;

/* What the owner needs to know of the editor, given once at start. */
typedef struct
{
 int  (*text_on_top)(void);         /* a text window is on top of the desk */
 int  (*read_key)(void);            /* the display's key reader */
 void (*box_opens)(void);           /* a box takes the screen */
 void (*say_lost)(const char *what);   /* a refused paint, said on the status row */
} WpePaintDesk;

void wpe_paint_desk_set(const WpePaintDesk *desk);

int  wpe_paint_idle(void);
void wpe_paint_modal_enter(const char *what);
void wpe_paint_modal_leave(void);
int  wpe_paint_modal_depth(void);
int  wpe_paint_modal_getch(void);

WpePaintVerdict wpe_paint_request(WpePaintReplay fn, void *data,
                                  const char *what);
void wpe_paint_replay_if_idle(void);

void wpe_paint_chrome_suppress(int on);
int  wpe_paint_chrome_suppressed(void);

#endif /* WE_PAINT_H */
