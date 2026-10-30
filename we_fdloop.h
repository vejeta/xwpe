#ifndef __WE_FDLOOP_H
#define __WE_FDLOOP_H
/*-------------------------------------------------------------------------*\
  <we_fdloop.h> -- File descriptor event loop for xwpe

  Copyright (C) 2026 Juan Manuel Mendez Rey
  This is free software; see the file COPYING (GPL-2).

  Multiplexes multiple fd sources (X11 display, gdb pipes, pty master)
  via poll().  Each fd has a callback that fires when data is available.
  Follows GDB's event-loop.c pattern adapted to Kruse's architecture.

  Timers live in the same loop, as in GLib's main loop and libuv: the
  nearest due timer bounds the poll() timeout, and due timers run after
  the fds.  Only poll() and the monotonic clock are used, so a waiting
  line or a deadline ticks the same on every POSIX system.

  Copyright (C) 2026 Juan Manuel Mendez Rey
  This is free software; see the file COPYING.
\*-------------------------------------------------------------------------*/

#include <stddef.h>

typedef void (*wpe_fd_callback)(int fd, void *data);
typedef void (*wpe_timer_callback)(void *data);

/* Poll cadence (ms) for the debugger/run event loops: short enough to stay
   responsive to the keyboard, long enough not to spin.  (-1 = block.) */
#define WPE_FD_POLL_MS 50

/* Unit conversions for callers whose intervals are kept in other units. */
#define WPE_TIMER_MS_PER_S  1000L
#define WPE_TIMER_NS_PER_MS 1000000L
/* No timer: what a caller keeps in place of an id it has not been given. */
#define WPE_TIMER_NONE 0

int  wpe_fd_add(int fd, int events, wpe_fd_callback cb, void *data);
void wpe_fd_del(int fd);
void wpe_fd_dispatch(int fd);
int  wpe_fd_poll(int timeout_ms);
void wpe_fd_refusal(const char *what, int fd, char *line, size_t sz);
int  wpe_timer_add(long interval_ms, int repeat, wpe_timer_callback cb, void *data);
void wpe_timer_del(int id);

/* The editor's side (we_hfkt.c): watch an fd and say so on the bottom row
   when the loop cannot; wait for a key whether or not the keyboard is
   watched.  Every callback consumes what made its fd ready, or stops
   watching it: one that returns early and leaves the data there makes the
   loop spin (see e_fd_watch). */
int  e_fd_watch(int fd, int events, wpe_fd_callback cb, void *data, const char *what);
void e_fd_key_wait(int keyboard_watched);

#endif
