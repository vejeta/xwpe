#ifndef __WE_FDLOOP_H
#define __WE_FDLOOP_H
/*-------------------------------------------------------------------------*\
  <we_fdloop.h> -- File descriptor event loop for xwpe

  Copyright (C) 2026 Juan Manuel Mendez Rey
  This is free software; see the file COPYING (GPL-2).

  Multiplexes multiple fd sources (X11 display, gdb pipes, pty master)
  via poll().  Each fd has a callback that fires when data is available.
  Follows GDB's event-loop.c pattern adapted to Kruse's architecture.

  Copyright (C) 2026 Juan Manuel Mendez Rey
  This is free software; see the file COPYING.
\*-------------------------------------------------------------------------*/

typedef void (*wpe_fd_callback)(int fd, void *data);

int  wpe_fd_add(int fd, int events, wpe_fd_callback cb, void *data);
void wpe_fd_del(int fd);
int  wpe_fd_poll(int timeout_ms);

#endif
