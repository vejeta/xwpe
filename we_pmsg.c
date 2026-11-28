/* we_pmsg.c -- lines for Messages kept behind a box until it closes. */
/* Copyright (C) 2026 Juan Manuel Mendez Rey                    */
/* This is free software; you can redistribute it and/or        */
/* modify it under the terms of the                             */
/* GNU General Public License, see the file COPYING.            */
/* See we_pmsg.h for what the user gets. */

#include <stdio.h>
#include <string.h>

#include "edit.h"
#include "we_paint.h"
#include "we_pmsg.h"

/* How many lines are kept, and how long a kept line may be (a longer one is
   cut). */
#define PMSG_HELD_MAX  32
#define PMSG_LINE_MAX  1024
#define PMSG_HELD_WHAT "a message for Messages"

static struct {
 char *line[PMSG_HELD_MAX];
 int   sw[PMSG_HELD_MAX];
 ECNT *cn;
 WpePmsgSay say;
 int   count;
 int   lost;
} g_held;

/**
 * wpe_pmsg_pending - How many lines wait behind a box.
 * Return: the count.
 */
int wpe_pmsg_pending(void)
{
 return(g_held.count);
}

/**
 * wpe_pmsg_replay - Write the kept lines into Messages, in the order they
 * were said.
 * @data: unused.
 *
 * The screen's owner runs this once no box owns the screen.  Lines that did
 * not fit the keeping are counted and named in one last line, so the user
 * knows the list is not whole.
 */
void wpe_pmsg_replay(void *data)
{
 ECNT *cn = g_held.cn;
 WpePmsgSay say = g_held.say;
 FENSTER *top;
 char tail[PMSG_LINE_MAX];
 int i, count = g_held.count, lost = g_held.lost;
 char *lines[PMSG_HELD_MAX];
 int sw[PMSG_HELD_MAX];

 (void)data;
 if (!cn || cn->mxedt < 1 || !say)
  return;
 for (i = 0; i < count; i++)
 {
  lines[i] = g_held.line[i];
  sw[i] = g_held.sw[i];
 }
 g_held.count = 0;
 g_held.lost = 0;
 top = cn->f[cn->mxedt];
 for (i = 0; i < count; i++)
 {
  say(lines[i], top, sw[i]);
  FREE(lines[i]);
 }
 if (lost > 0)
 {
  snprintf(tail, sizeof tail, "(%d more messages were not shown)", lost);
  say(tail, top, 0);
 }
}

/**
 * wpe_pmsg_hold - Keep a line said from a callback until no box owns the
 * screen.
 * @str: the line; @f: a window of the desktop; @sw: as e_d_p_message;
 * @say: what writes the line into Messages when the box has closed.
 *
 * What a language server or a debugger tells while a dialog or a menu is up
 * is not lost and not drawn under it: it is in Messages when the box
 * closes.  A full keeping drops the newest lines and counts them.
 */
void wpe_pmsg_hold(const char *str, FENSTER *f, int sw, WpePmsgSay say)
{
 char *copy;
 size_t room = strlen(str) + 1;

 if (g_held.count >= PMSG_HELD_MAX)
 {
  g_held.lost++;
  return;
 }
 if (room > PMSG_LINE_MAX)
  room = PMSG_LINE_MAX;
 copy = MALLOC(room);
 snprintf(copy, room, "%s", str);
 g_held.cn = f->ed;
 g_held.say = say;
 g_held.line[g_held.count] = copy;
 g_held.sw[g_held.count] = sw;
 g_held.count++;
 wpe_paint_request(wpe_pmsg_replay, NULL, PMSG_HELD_WHAT);
}

/**
 * wpe_pmsg_drop - Forget the kept lines a writer is about to say again.
 * @is_dropped: answers 1 for a kept line the writer will say anew.
 *
 * What the user gets: a block of lines that redraws itself in place (the
 * assistant's checklist) while a dialog is open is shown once when the dialog
 * closes, in its latest form.  Only the old copies wait behind the dialog;
 * the lines that other writers kept stay, in their order.
 */
void wpe_pmsg_drop(int (*is_dropped)(const char *line))
{
 int i, kept = 0;

 for (i = 0; i < g_held.count; i++)
 {
  if (is_dropped(g_held.line[i]))
  {
   FREE(g_held.line[i]);
   continue;
  }
  g_held.line[kept] = g_held.line[i];
  g_held.sw[kept] = g_held.sw[i];
  kept++;
 }
 g_held.count = kept;
}
