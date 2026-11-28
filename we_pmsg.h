/* we_pmsg.h -- lines for Messages kept behind a box until it closes.
   Copyright (C) 2026 Juan Manuel Mendez Rey
   This file is part of xwpe and is distributed under the GNU General Public
   License, version 2 or later.

   What a language server or a debugger tells while a dialog or a menu is up
   is not lost and not drawn under it: it is kept here, in the order it was
   said, and written into Messages when the box closes.  A writer that draws
   a block of lines again and again (the assistant's checklist) takes its
   older copies out of the keeping before it says the new one, so the
   closed box shows the latest block once. */

#ifndef WE_PMSG_H
#define WE_PMSG_H

#include "edit.h"

/* The function that writes a line into Messages when the screen is free. */
typedef int (*WpePmsgSay)(char *str, FENSTER *f, int sw);

/* Keep @str (a copy) until no box owns the screen; @say writes it then. */
void wpe_pmsg_hold(const char *str, FENSTER *f, int sw, WpePmsgSay say);

/* How many lines are kept now. */
int wpe_pmsg_pending(void);

/* Write every kept line, in order, now: the screen's owner calls this once no
   box owns the screen.  @data is unused. */
void wpe_pmsg_replay(void *data);

/* Forget the kept lines @is_dropped answers 1 for. */
void wpe_pmsg_drop(int (*is_dropped)(const char *line));

#endif /* WE_PMSG_H */
