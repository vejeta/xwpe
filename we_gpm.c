#ifdef HAVE_LIBGPM
/* we_gpm.c -- GPM routines for xwpe.
   Copyright (C) 1998 Sebastiano Suraci
   Copyright (C) 2026 Juan Manuel Mendez Rey
   This is free software; see the file COPYING (GPL-2).
   Based on we_linux.c -- Created by Sebastiano Suraci */

#include "edit.h"
#include <gpm.h>

int WpeGpmHandler(Gpm_Event *ep, void *data);

int WpeGpmMouseInit(void)
{
 Gpm_Connect c;
 int ret;

 /* Check if gpm daemon is accessible before trying to connect.
    Without this check, libgpm tries to launch /usr/sbin/gpm
    which prints "not found" if gpm is not installed. */
 if (access("/dev/gpmctl", F_OK) != 0 && access("/var/run/gpm.pid", F_OK) != 0)
  return(-1);

 Gpm_GetServerVersion(NULL);
 gpm_zerobased = 1;
 c.eventMask = ~0;
 c.defaultMask = ~GPM_HARD;
 c.minMod = 0;
 c.maxMod = ~0;
 ret = Gpm_Open(&c, 0);
 if (ret == -2)
 {
  /* Xterms returns mouse information through stdin which xwpe currently
    doesn't support */
  Gpm_Close();
  return(-1);
 }
 if (ret == -1)
 {
  return(-1);
 }
 gpm_handler = WpeGpmHandler;
 return(0);
}

int WpeGpmHandler(Gpm_Event *ep, void *data)
{
 extern struct mouse e_mouse;

 GPM_DRAWPOINTER(ep);
 if (ep->buttons & 7)
 {
  e_mouse.x = ep->x;
  e_mouse.y = ep->y;
  e_mouse.k = ep->buttons;
  return(-(ep->buttons ^ 5));
 }
 e_mouse.k = 0;
 return(0);
}

/* Set by WpeGpmReadable when a GPM button event arrives, so the terminal
   input loop knows to return a mouse keycode (e_mouse is already filled). */
int g_gpm_click_pending = 0;

/* The gpm connection fd, for the terminal input loop to poll(). <0 when GPM
   is not connected (e.g. inside a terminal emulator), so the caller skips it. */
int WpeGpmFd(void)
{
 return gpm_fd;
}

/* wpe_fd_poll callback: drain one GPM event when gpm_fd is readable, run our
   handler (draws the pointer via GPM_DRAWPOINTER and fills e_mouse), and flag
   button events for the input loop.  This is the missing pump: without it the
   GPM pointer never moves and clicks never register on the bare console. */
void WpeGpmReadable(int fd, void *data)
{
 Gpm_Event ev;

 (void) fd;
 (void) data;
 if (Gpm_GetEvent(&ev) > 0)
 {
  WpeGpmHandler(&ev, NULL);
  if (ev.buttons & 7)
   g_gpm_click_pending = 1;
 }
}

int WpeGpmMouse(int *g)
{
 Gpm_Event e;

 while (Gpm_GetSnapshot(&e) == 0)
  Gpm_GetEvent(&e);
 g[1] = g[0] = e.buttons;
 g[2] = e.x * 8;
 g[3] = e.y * 8;
 return(1);
}

#endif

