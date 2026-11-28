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

/* What wpe_paint_take_key is asked for when any key will do. */
#define WPE_PAINT_ANY_KEY     0

/* What the owner needs to know of the editor, given once at start. */
typedef struct
{
 int  (*text_on_top)(void);         /* a text window is on top of the desk */
 int  (*read_key)(void);            /* the display's key reader */
 void (*box_opens)(void);           /* a box takes the screen */
 void (*say_lost)(const char *what);   /* a refused paint, said on the status row */
 int  (*take_key)(int want);        /* the display's key taken without waiting */
 unsigned long (*top_count)(void);  /* how often what is on top has changed */
 /* A question is read: the display lets what the editor sent finish, then
    drops every key and click typed before (set *pasted when a paste went)
    and says how many presses went. */
 int  (*settle_and_drop)(const char *what, int *pasted);
 int  (*paste_streaming)(void);     /* a paste is still arriving */
 /* The keys read now answer the question @what, which ignored @dropped
    presses and, when @pasted is set, pasted text. */
 void (*question_begins)(const char *what, int dropped, int pasted);
 void (*question_ends)(void);
} WpePaintDesk;

/* One question put to the user; a new question is a new WpeAsk, made with
   WPE_ASK.  Only we_paint.c writes its fields.  The struct is tagged so a
   box that serves a question and a place to type can carry a pointer to one
   without including this header. */
typedef struct WpeAsk
{
 const char *what;     /* names it in the trace and in a test's dump */
 int shown;            /* its first read has settled and dropped */
 int dropped;          /* key and button presses it ignored */
 int pasted;           /* pasted text it ignored */
 int series;           /* one of a series the user answers on purpose: it
                          keeps what was typed before it (WPE_ASK_SERIES) */
} WpeAsk;
#define WPE_ASK(what) { (what), 0, 0, 0, 0 }
/* The box of a series the user answers on purpose, as a field is typed in:
   each "Replace this occurrence?" of a Find/Replace.  The keys typed ahead
   are the answers to the boxes that follow, so none is dropped. */
#define WPE_ASK_SERIES(what) { (what), 0, 0, 0, 1 }

void wpe_paint_desk_set(const WpePaintDesk *desk);

int  wpe_paint_idle(void);
void wpe_paint_trace_say(const char *line);
void wpe_ui_trace(const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 1, 2)))
#endif
    ;
void wpe_paint_modal_enter(const char *what);
void wpe_paint_modal_leave(void);
int  wpe_paint_modal_depth(void);
int  wpe_paint_modal_getch(void);
int  wpe_paint_ask_settle(WpeAsk *ask);
int  wpe_paint_ask_key(WpeAsk *ask);
int  wpe_paint_box_key(WpeAsk *ask);
int  wpe_paint_ask_dropped(const WpeAsk *ask);
int  wpe_paint_ask_pasted(const WpeAsk *ask);
int  wpe_paint_ask_shown(const WpeAsk *ask);
int  wpe_paint_take_key(int want);

WpePaintVerdict wpe_paint_request(WpePaintReplay fn, void *data,
                                  const char *what);
void wpe_paint_replay_if_idle(void);
int  wpe_paint_in_callback(void);

void wpe_paint_chrome_suppress(int on);
int  wpe_paint_chrome_suppressed(void);

#endif /* WE_PAINT_H */
