/* we_waitline.h -- the waiting lines at the foot of Messages have one owner.
   Copyright (C) 2026 Juan Manuel Mendez Rey
   This file is part of xwpe and is distributed under the GNU General Public
   License, version 2 or later.

   A build, a starting debugger, a compile for a debug session and a scala-cli
   setup each show a line that says they wait, for how long, and that Esc ends
   them.  Those lines are the last rows of the Messages text, below every row
   of output, one row for each waiter, in the order Esc ends them (the bottom
   line is the one the next Esc ends).  Only we_waitline.c puts such a row in,
   rewrites it, takes it out or empties Messages; a waiter never names a row
   of Messages. */

#ifndef WE_WAITLINE_H
#define WE_WAITLINE_H

#include "edit.h"

/* The longest text a waiting line holds, with its end. */
#define WPE_WAIT_LINE_MAX 160

/* One waiting line in Messages.  A waiter keeps one, made with WPE_WAIT;
   only we_waitline.c writes its fields. */
typedef struct WpeWait
{
 const char *what;              /* the Esc claim's name ("the build")        */
 int live;                      /* begun and not ended                       */
 int laid;                      /* its row is in the buffer now              */
 char text[WPE_WAIT_LINE_MAX];  /* what it last said, exactly as written     */
} WpeWait;

#define WPE_WAIT(what) { (what), 0, 0, "" }

/* The most waiters that may be live at once; a later begin is traced and its
   line is not shown. */
#define WPE_WAIT_MAX 8

/* A waiter arrives; it takes its place in the foot when it first speaks. */
void wpe_wait_begin(WpeWait *w);
/* Its text now; only its own row is rewritten. */
void wpe_wait_say(WpeWait *w, const char *fmt, ...);
/* Its wait is over: its row goes and the foot closes up. */
void wpe_wait_end(WpeWait *w);
/* Is its row in the buffer now? */
int wpe_wait_shown(const WpeWait *w);

/* For the writers of Messages, called from the output primitives only: the
   foot is taken out before output is appended and put back after it, so output
   never lands below a waiting line.  Both do nothing for any buffer but the
   Messages text; a pair may nest.  wpe_wait_lift returns the rows taken. */
int wpe_wait_lift(BUFFER *b);
void wpe_wait_lay(BUFFER *b);

/* The rows of the foot that are in @b now, for a writer that records where
   its own rows begin. */
int wpe_wait_foot_rows(const BUFFER *b);

/* The one way to empty Messages: the buffer is cleared and the foot is put
   back at once.  Return: as e_buffer_clear. */
int wpe_wait_messages_clear(BUFFER *b);
/* Messages was (re)opened on its text: the foot is laid again. */
void wpe_wait_messages_opened(BUFFER *b);

/* One trace line for each live waiter. */
void wpe_wait_trace_all(void);

#endif /* WE_WAITLINE_H */
