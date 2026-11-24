/* we_closed.h -- the one owner of "this window refuses this edit".

   A window the user may not change (a file opened read-only, the clipboard
   viewer, Help, Messages, Watches, Stack, the assistant's pane, a man page,
   a window an assistant reply is about to be written into) is asked here and
   nowhere else.  This module also spells the one line said for each reason,
   so a refusal reads the same from a key, a prefix letter or a menu row.

   Copyright (C) 2026 Juan Manuel Mendez Rey
   This file is part of xwpe and is distributed under the GNU General Public
   License, version 2 or later. */

#ifndef WE_CLOSED_H
#define WE_CLOSED_H

struct FNST;

/* Why a window is closed to the user's changes. */
typedef enum {
 WPE_CLOSED_NO = 0,      /* the user may change the text */
 WPE_CLOSED_READ_ONLY,   /* WPE_INS_READONLY: a read-only file, a man page, the
                            clipboard viewer, Help, Messages, Watches, Stack,
                            the assistant's pane */
 WPE_CLOSED_ASSISTANT    /* a reply is about to be spliced into this window */
} WpeClosedWhy;

/* Why @f is closed, or WPE_CLOSED_NO.  Asks only: says and traces nothing. */
WpeClosedWhy wpe_window_closed_why(struct FNST *f);

/* Is @f closed to the user's changes, for any reason?  Silent. */
int wpe_window_closed(struct FNST *f);

/* Is @f closed because its flag says so (not for a pending reply)?  Silent.
   The insert/overwrite toggles and the frame's mouse toggles ask this, since
   the flag shares a field with the insert mode. */
int wpe_window_read_only(struct FNST *f);

/* Say the one line for @why on the status row, kept until the next key, at
   most once per key (and once per bracketed paste). */
void wpe_closed_say(struct FNST *f, WpeClosedWhy why);

/* Say that a command was not run because the window on top holds no text:
   "<action> needs a text window", on the status row, kept until the next key
   and at most once per key exactly as the reasons above are, so a prefix
   letter, a function key and a menu row all say it the same way.  A NULL
   @action names no command of its own and gets the generic word. */
void wpe_closed_say_needs_text(struct FNST *f, const char *action);

/* The key loop has read the next key: the next refusal may say its line
   again.  Holds while a bracketed paste streams. */
int wpe_closed_key_read(void);

/* The key loop is about to wait for the next key: the event is over, and a
   refusal said while waiting is always said. */
void wpe_closed_key_wait(void);

/* A reason ended (the assistant's reply landed or was cancelled): drop the
   line said for it. */
void wpe_closed_reason_over(WpeClosedWhy why);

/* The ask of a command about to change @f's text: when @f is closed, say why
   and return 1; else 0. */
int wpe_window_refuses_edit(struct FNST *f);

/* The window a user edit command acts on: the window on top when it holds
   text and is not closed, else NULL with the reason said.  @action is the
   command's own name, which the refusal line carries. */
struct FNST *e_edit_window(struct FNST *f, const char *action);

/* Transitional: what the old e_window_text_locked did at every call site that
   has not moved to the owner's ask yet.  A read-only window answers 1 in
   silence; a window waiting for an assistant reply says its line.  Deleted
   when the key loop and the commands ask the owner. */
int wpe_window_locked_for_now(struct FNST *f);

#endif /* WE_CLOSED_H */
