/* we_fdloop.c                                            */
/* Copyright (C) 2026 Juan Manuel Mendez Rey               */
/* This is free software; you can redistribute it and/or   */
/* modify it under the terms of the                        */
/* GNU General Public License, see the file COPYING.       */

#include <poll.h>
#include <stdlib.h>
#include "we_fdloop.h"

#define WPE_FD_MAX 16

typedef struct {
 int fd;
 int events;
 wpe_fd_callback callback;
 void *data;
 int active;
} wpe_fd_entry;

static wpe_fd_entry fd_table[WPE_FD_MAX];
static int fd_count = 0;

static wpe_fd_entry *wpe_fd_find(int fd)
{
 int i;
 for (i = 0; i < fd_count; i++)
  if (fd_table[i].active && fd_table[i].fd == fd)
   return &fd_table[i];
 return NULL;
}

int wpe_fd_add(int fd, int events, wpe_fd_callback cb, void *data)
{
 wpe_fd_entry *e;

 if (fd < 0)
  return -1;
 e = wpe_fd_find(fd);
 if (e)
 {
  e->events = events;
  e->callback = cb;
  e->data = data;
  return 0;
 }
 if (fd_count >= WPE_FD_MAX)
  return -1;
 fd_table[fd_count].fd = fd;
 fd_table[fd_count].events = events;
 fd_table[fd_count].callback = cb;
 fd_table[fd_count].data = data;
 fd_table[fd_count].active = 1;
 fd_count++;
 return 0;
}

void wpe_fd_del(int fd)
{
 int i;

 for (i = 0; i < fd_count; i++)
 {
  if (fd_table[i].active && fd_table[i].fd == fd)
  {
   fd_table[i].active = 0;
   while (fd_count > 0 && !fd_table[fd_count - 1].active)
    fd_count--;
   return;
  }
 }
}

int wpe_fd_poll(int timeout_ms)
{
 struct pollfd pfd[WPE_FD_MAX];
 int map[WPE_FD_MAX];
 int n = 0, i, ready;

 for (i = 0; i < fd_count; i++)
 {
  if (!fd_table[i].active)
   continue;
  pfd[n].fd = fd_table[i].fd;
  pfd[n].events = fd_table[i].events;
  pfd[n].revents = 0;
  map[n] = i;
  n++;
 }
 if (n == 0)
  return 0;
 ready = poll(pfd, n, timeout_ms);
 if (ready <= 0)
  return ready;
 for (i = 0; i < n; i++)
 {
  short re = pfd[i].revents;
  wpe_fd_entry *e = &fd_table[map[i]];

  if (!re || !e->active)               /* (a prior callback may have removed it) */
   continue;
  if (re & POLLNVAL)                    /* fd already closed: cannot be polled    */
  {
   wpe_fd_del(e->fd);
   continue;
  }
  /* POLLHUP / POLLERR (EOF or error) MUST dispatch too: the read callback drains
     the last bytes, sees EOF and unregisters the fd.  Without it a dead server
     fd -- an LSP or debug child that exited -- keeps poll() returning instantly
     with revents set but NO POLLIN (the case when no buffered data is left, e.g.
     a server that crashed at start-up), nothing reaps it, and the input loop
     spins at 100% CPU.  The per-callback "detect dead server" guard only fires
     when it is actually called, i.e. on POLLIN; this makes it fire on hang-up
     too, on every platform regardless of how the EOF is delivered. */
  if (re & (POLLIN | POLLPRI | POLLHUP | POLLERR))
  {
   if (e->callback)
    e->callback(e->fd, e->data);
   else if (re & (POLLHUP | POLLERR))  /* wake-only fd (stdin, X11) hung up:      */
    wpe_fd_del(e->fd);                 /* drop it -- nobody can reap it, would spin */
   /* else a wake-only POLLIN just unblocks the poll; the loop reads it itself.  */
  }
 }
 return ready;
}
