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
int  wpe_fd_mute(int fd);
void wpe_fd_unmute(int fd, int events);
void wpe_fd_release(int fd);
void wpe_fd_dispatch(int fd);
int  wpe_fd_poll(int timeout_ms);
void wpe_fd_refusal(const char *what, int fd, char *line, size_t sz);
int  wpe_timer_add(long interval_ms, int repeat, wpe_timer_callback cb, void *data);
void wpe_timer_del(int id);

/* What wpe_fd_readable_now says about a descriptor. */
#define WPE_FD_NOT_READABLE 0
#define WPE_FD_READABLE     1
#define WPE_FD_INVALID      (-1)
int  wpe_fd_readable_now(int fd);

/* wpe_fd_wait_for: no deadline; and how it ended. */
#define WPE_WAIT_FOREVER (-1L)
#define WPE_WAIT_TIMEOUT 0
#define WPE_WAIT_DONE    1
#define WPE_WAIT_INPUT   2   /* a key or display event for the caller's reader */
typedef int (*wpe_wait_pred)(void);
int  wpe_fd_wait_for(wpe_wait_pred pred, long deadline_ms, const char *what);
const char *wpe_fd_waiting_for(void);
int  wpe_fd_wait_serving(wpe_wait_pred pred, const char *what, int quiet_fd,
                         void (*serve)(void));
/* At the editor's end only: run the loop, the keyboard and displays no
   longer read, until @pred or the deadline. */
int  wpe_fd_end_wait(wpe_wait_pred pred, long deadline_ms);

/* Bytes for a child: queued per descriptor and written by the loop when the
   descriptor is writable, in order and whole, so a child that stops reading
   never freezes the editor.  A message is taken whole or refused whole.
   With nothing waiting one message of any size is taken (the language
   server is sent the whole file on every completed line: 1 MB for the
   largest source here), so a server that lags by one message is never
   ended; behind it the queue takes no more than WPE_FD_WRITE_QUEUE_MAX.
   Memory: one descriptor holds at most the larger of 4 MiB and its largest
   single message, which its sender has just built in memory itself, so the
   copy at most doubles that peak.  Only the language server, the build
   server and an adapter send messages that size: a debugger's commands
   are under a kilobyte and typed keys a few bytes each, so the editor
   holds about 12 MiB at worst (32 x 4 MiB = 128 MiB is the bound no
   session reaches).  65536 would end a language server on a 100 KB file
   as soon as one line's update waited behind another. */
#define WPE_FD_WRITE_QUEUE_MAX   (4L * 1024L * 1024L)
#define WPE_FD_WRITE_STALL_MS    5000L   /* no byte taken for this long: the user is told */
#define WPE_FD_WRITE_SENT        0
#define WPE_FD_WRITE_QUEUED      1       /* some or all bytes wait for the child to read */
#define WPE_FD_WRITE_REFUSED    (-1)     /* queue full, the descriptor closed, or it blocks */
#define WPE_FD_WRITE_CLOSED     (-2)     /* EPIPE: the child's end is gone */

int  wpe_fd_write(int fd, const void *buf, size_t n, const char *what,
                  const char *ends);
int  wpe_fd_write_flush(int fd);
long wpe_fd_write_pending(int fd);
int  wpe_fd_close(int fd);   /* the one way a child's input is closed */
int  wpe_fd_write_stalled(int fd);
void wpe_fd_write_say_set(void (*say)(const char *line));

/* The editor's side (we_hfkt.c): watch an fd and say so on the bottom row
   when the loop cannot; wait for a key whether or not the keyboard is
   watched.  Every callback consumes what made its fd ready, or stops
   watching it: one that returns early and leaves the data there makes the
   loop spin (see e_fd_watch). */
int  e_fd_watch(int fd, int events, wpe_fd_callback cb, void *data, const char *what);
void e_fd_key_wait(int keyboard_watched);

#endif
