#ifdef HAVE_LIBGPM
/* we_gpm.c -- GPM routines for xwpe.
   Copyright (C) 1998 Sebastiano Suraci
   Copyright (C) 2026 Juan Manuel Mendez Rey
   This is free software; see the file COPYING (GPL-2).
   Based on we_linux.c -- Created by Sebastiano Suraci */

#include "edit.h"
#include <gpm.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#ifdef __linux__
#include <linux/kd.h>
#include <linux/vt.h>
#endif

/* GPM packs the three mouse buttons into the low bits of ep->buttons
   (4=left, 2=middle, 1=right); this masks "any button down". */
#define GPM_BUTTON_MASK 7

/* How long one mouse-state poll waits for the console to deliver.  Menus and
   dialog loops poll once per iteration, so this bound is what keeps the
   keyboard alive when the console has nothing to say; it is the same order as
   the terminal path's own getch timeout (INPUT_POLL_MS, we_term.c).  A real
   console answers at once, so a drag there is not slowed by it. */
#define GPM_POLL_MS 50

int WpeGpmHandler(Gpm_Event *ep, void *data);

/**
 * WpeGpmIsConsoleTty - Whether this terminal can be served by GPM.
 *
 * GPM serves the physical screen.  A pseudo-terminal -- ssh, tmux, screen, any
 * terminal emulator -- gets its mouse from the terminal's own reports, and
 * asking GPM there waits for an event that can never arrive.  The kernel is
 * asked first because it cannot be wrong the way TERM is: a virtual console
 * answers the VT ioctls and a pty fails them with ENOTTY.
 *
 * The fallback excludes pseudo-terminals only, and both ends of one: the slave
 * is /dev/pts/N and the master is /dev/ptmx.  Any other tty -- a serial
 * console, for one -- is left exactly as it was, because gpm may well be what
 * serves it and this test has no business taking a working mouse away.
 * Return: 1 when the console path applies, else 0.
 */
static int WpeGpmIsConsoleTty(void)
{
 int fd = STDIN_FILENO;
#ifdef __linux__
 int mode = 0;
 struct vt_stat vts;
#endif
 const char *name;

 if (!isatty(fd))
  return(0);
#ifdef __linux__
 if (ioctl(fd, KDGETMODE, &mode) == 0)
  return(1);
 memset(&vts, 0, sizeof vts);
 if (ioctl(fd, VT_GETSTATE, &vts) == 0)
  return(1);
#endif
 name = ttyname(fd);
 if (!name)
  return(0);
 if (strcmp(name, "/dev/ptmx") == 0)
  return(0);
 return (strncmp(name, "/dev/pts/", 9) != 0);
}

int WpeGpmMouseInit(void)
{
 Gpm_Connect c;
 int ret;

 /* Check if gpm daemon is accessible before trying to connect.
    Without this check, libgpm tries to launch /usr/sbin/gpm
    which prints "not found" if gpm is not installed. */
 if (access("/dev/gpmctl", F_OK) != 0 && access("/var/run/gpm.pid", F_OK) != 0)
  return(-1);

 /* A daemon on this machine says nothing about THIS terminal: gpm serves the
    console, so a pty keeps the terminal's own mouse reports even while the
    daemon runs.  Without this test every ssh, tmux and screen session on such
    a box took the console path, and its menus stopped reading keys while
    waiting for a console event (SC-dialog-engine-45).  Gpm_Open below refuses
    only xterm-like TERM values, which is why xterm escaped this by accident
    while screen and tmux did not. */
 if (!WpeGpmIsConsoleTty())
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

/**
 * WpeGpmButtonsToXwpe - Put GPM's button bits in the editor's own order.
 *
 * GPM numbers the buttons right-to-left (GPM_B_LEFT is 4, GPM_B_RIGHT is 1);
 * the editor's input layer and the dialog engine read e_mouse.k the other way
 * round (left = 1, middle = 2, right = 4), the order the ncurses reader uses,
 * so a console user's left click is a left click in a dialog too.
 */
static int WpeGpmButtonsToXwpe(int buttons)
{
 int k = 0;

 if (buttons & GPM_B_LEFT)   k |= 1;
 if (buttons & GPM_B_MIDDLE) k |= 2;
 if (buttons & GPM_B_RIGHT)  k |= 4;
 return(k);
}

int WpeGpmHandler(Gpm_Event *ep, void *data)
{
 extern struct mouse e_mouse;

 GPM_DRAWPOINTER(ep);
 if (ep->buttons & GPM_BUTTON_MASK)
 {
  e_mouse.x = ep->x;
  e_mouse.y = ep->y;
  e_mouse.k = WpeGpmButtonsToXwpe(ep->buttons);
  /* Inherited Suraci (1998) gpm_handler return convention: a negative value
     tells libgpm's Gpm_Getc() that this event was consumed, and its magnitude
     carries the button bitmask back to xwpe's input layer (XOR 5 swaps the
     left/right bits to the order that layer expects).  Left verbatim. */
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
  if (ev.buttons & GPM_BUTTON_MASK)
   g_gpm_click_pending = 1;
 }
}

/* Monotonic milliseconds, for the poll bound below.  The same clock the
   language-server and the debugger reads use (we_lsp.c, we_bsp.c). */
static long WpeGpmNowMs(void)
{
 struct timespec ts;

 clock_gettime(CLOCK_MONOTONIC, &ts);
 return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/**
 * WpeGpmMouse - The console's mouse state, within a bounded wait.
 * @g: the drag-poll result: g[1] the button mask, g[2]/g[3] the position in
 *     eighths of a cell, exactly as the terminal backend publishes it.
 *
 * Waits at most GPM_POLL_MS for the console to report, so a menu or dialog loop
 * that polls once per iteration cannot lose the keyboard when no event is ever
 * going to arrive (SC-dialog-engine-46).  gpm_fd is polled before
 * Gpm_GetEvent because it is a blocking descriptor: that call is where the
 * unbounded wait used to be, and reaching libgpm's error reporting once per
 * attempt is what filled the system log while it lasted.
 *
 * Running out of time is NOT a button release.  A console in use reports at
 * once, so the bound is only reached when nothing has happened, and repeating
 * the last state is what keeps a drag on the bare console going until gpm says
 * the button came up.  With nothing ever reported there is no button held and
 * the position is the one the editor already has, so the pointer does not jump
 * to the corner.
 * Return: 1, as before.
 */
int WpeGpmMouse(int *g)
{
 extern struct mouse e_mouse;
 static Gpm_Event s_last;         /* the last state the console reported */
 static int s_have_last = 0;
 Gpm_Event e;
 long deadline;
 int got = 0;

 memset(&e, 0, sizeof e);
 deadline = WpeGpmNowMs() + GPM_POLL_MS;
 while (!got)
 {
  struct pollfd pf;
  int left, n = Gpm_GetSnapshot(&e);

  if (n > 0)
  {
   got = 1;
   break;
  }
  if (n < 0)
   break;                        /* the console cannot answer: stop asking */
  left = (int)(deadline - WpeGpmNowMs());
  if (left <= 0)
   break;
  pf.fd = gpm_fd;
  pf.events = POLLIN;
  pf.revents = 0;
  if (poll(&pf, 1, left) <= 0)
   break;
  if (Gpm_GetEvent(&e) > 0)
   got = 1;
 }

 if (got)
 {
  s_last = e;
  s_have_last = 1;
 }
 else if (!s_have_last)
 {
  g[1] = g[0] = 0;
  g[2] = e_mouse.x * 8;
  g[3] = e_mouse.y * 8;
  return(1);
 }
 else
  e = s_last;

 g[1] = g[0] = e.buttons;
 g[2] = e.x * 8;
 g[3] = e.y * 8;
 return(1);
}

#endif

