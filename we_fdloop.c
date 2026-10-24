/* we_fdloop.c                                            */
/* Copyright (C) 2026 Juan Manuel Mendez Rey               */
/* This is free software; you can redistribute it and/or   */
/* modify it under the terms of the                        */
/* GNU General Public License, see the file COPYING.       */

#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include "we_fdloop.h"

#define WPE_FD_MAX 16
/* Timers live at once: a debugger start, a build, the assistant's spinner and
   the agent host's deadline, with room to spare. */
#define WPE_TIMER_MAX 16
/* poll()'s "no bound": wait until an fd wakes. */
#define WPE_POLL_FOREVER (-1)

typedef struct {
 int id;                 /* WPE_TIMER_NONE while the slot is free         */
 int64_t due_ms;         /* monotonic time it is due at                   */
 long interval_ms;
 int repeat;
 int in_pass;            /* was live when the current firing pass began   */
 wpe_timer_callback callback;
 void *data;
} wpe_timer_entry;

static wpe_timer_entry timer_table[WPE_TIMER_MAX];
static int timer_last_id = WPE_TIMER_NONE;

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

/**
 * wpe_fd_dispatch - Run the callback registered for fd as if it had woken.
 * @fd: a registered descriptor.
 *
 * Used by a timer that finds a source overdue (a server that never answered):
 * its own callback ends it the same way a hang-up would, so there is one
 * path to the line the user reads, not two.
 */
void wpe_fd_dispatch(int fd)
{
 wpe_fd_entry *e = wpe_fd_find(fd);
 if (e && e->callback)
  e->callback(e->fd, e->data);
}

/**
 * wpe_clock_ms - Milliseconds on the monotonic clock.
 *
 * A timer measured on it keeps its pace when the user or NTP sets the wall
 * clock, so a waiting line never jumps and a deadline never fires early.
 */
static int64_t wpe_clock_ms(void)
{
 struct timespec ts;

 clock_gettime(CLOCK_MONOTONIC, &ts);
 return (int64_t)ts.tv_sec * WPE_TIMER_MS_PER_S + ts.tv_nsec / WPE_TIMER_NS_PER_MS;
}

/**
 * wpe_timer_find - The live timer with this id.
 * @id: an id wpe_timer_add returned, or anything else.
 * Return: its slot, or NULL when no live timer has that id.
 */
static wpe_timer_entry *wpe_timer_find(int id)
{
 int i;

 if (id == WPE_TIMER_NONE)
  return NULL;
 for (i = 0; i < WPE_TIMER_MAX; i++)
  if (timer_table[i].id == id)
   return &timer_table[i];
 return NULL;
}

/**
 * wpe_timer_free_slot - A slot no timer uses.
 * Return: the slot, or NULL when every slot is taken.
 */
static wpe_timer_entry *wpe_timer_free_slot(void)
{
 int i;

 for (i = 0; i < WPE_TIMER_MAX; i++)
  if (timer_table[i].id == WPE_TIMER_NONE)
   return &timer_table[i];
 return NULL;
}

/**
 * wpe_timer_next_id - An id no live timer has.
 *
 * Ids are not reused while their timer lives, so a caller that deletes a
 * timer it no longer owns cannot stop someone else's.
 */
static int wpe_timer_next_id(void)
{
 do
 {
  if (timer_last_id == INT_MAX)
   timer_last_id = WPE_TIMER_NONE;
  timer_last_id++;
 } while (wpe_timer_find(timer_last_id));
 return timer_last_id;
}

/**
 * wpe_timer_add - Call @cb after @interval_ms, once or every @interval_ms.
 * @interval_ms: the delay (and the period of a repeating timer), in ms.
 * @repeat: 1 to keep firing until wpe_timer_del, 0 for a one-shot.
 * @cb: what runs, from wpe_fd_poll; @data: handed to it.
 *
 * This is what turns a waiting line ("building ... 3s") and ends a wait at
 * its deadline while the child is silent: the editor's own poll wakes for it,
 * on every system, with no thread and no signal.  The callback may add or
 * delete any timer, itself included, and any fd.
 * Return: the timer's id (> 0), or -1 when the table is full or the request
 * is invalid (a repeating timer of 0 ms would wake the loop without end).
 */
int wpe_timer_add(long interval_ms, int repeat, wpe_timer_callback cb, void *data)
{
 wpe_timer_entry *t;

 if (!cb || interval_ms < 0)
  return -1;
 if (repeat && interval_ms == 0)
  return -1;
 t = wpe_timer_free_slot();
 if (!t)
  return -1;
 t->id = wpe_timer_next_id();
 t->due_ms = wpe_clock_ms() + interval_ms;
 t->interval_ms = interval_ms;
 t->repeat = repeat;
 t->in_pass = 0;
 t->callback = cb;
 t->data = data;
 return t->id;
}

/**
 * wpe_timer_del - Stop a timer.
 * @id: what wpe_timer_add returned; 0, an unknown id or one that has already
 * run as a one-shot is ignored.
 *
 * Called when the wait it measured is over, so the idle editor sleeps until
 * a key again instead of waking for a line nobody shows.
 */
void wpe_timer_del(int id)
{
 wpe_timer_entry *t = wpe_timer_find(id);

 if (t)
  t->id = WPE_TIMER_NONE;
}

/**
 * wpe_timer_ms_to_nearest - How long until the next timer is due.
 * Return: 0 when one is overdue, WPE_POLL_FOREVER when there is none.
 */
static int wpe_timer_ms_to_nearest(void)
{
 int64_t now = wpe_clock_ms();
 int64_t nearest = INT_MAX;
 int i, any = 0;

 for (i = 0; i < WPE_TIMER_MAX; i++)
 {
  if (timer_table[i].id == WPE_TIMER_NONE)
   continue;
  any = 1;
  if (timer_table[i].due_ms - now < nearest)
   nearest = timer_table[i].due_ms - now;
 }
 if (!any)
  return WPE_POLL_FOREVER;
 if (nearest < 0)
  return 0;
 return (int)nearest;
}

/**
 * wpe_timer_bound_wait - The poll() timeout once the timers are counted.
 * @timeout_ms: the caller's bound, or WPE_POLL_FOREVER.
 *
 * An editor sleeping until a key still wakes in time to turn a waiting line
 * or to end a wait whose deadline has come.
 */
static int wpe_timer_bound_wait(int timeout_ms)
{
 int until = wpe_timer_ms_to_nearest();

 if (until == WPE_POLL_FOREVER)
  return timeout_ms;
 if (timeout_ms == WPE_POLL_FOREVER || until < timeout_ms)
  return until;
 return timeout_ms;
}

/**
 * wpe_timer_rearm - Set a repeating timer's next due time.
 * @t: the timer that is firing; @now: the time of this pass.
 *
 * The next tick follows the last due time, so the pace does not drift; a
 * timer that fell an interval or more behind (the editor was busy) restarts
 * from now, so the user never sees a burst of catch-up ticks.
 */
static void wpe_timer_rearm(wpe_timer_entry *t, int64_t now)
{
 t->due_ms += t->interval_ms;
 if (t->due_ms <= now)
  t->due_ms = now + t->interval_ms;
}

/**
 * wpe_timer_fire - Run one due timer.
 * @t: the timer; @now: the time of this pass.
 *
 * The timer is re-armed or freed before its callback runs, so a callback
 * that deletes it, adds another into its slot, or polls again in a nested
 * key loop (a dialog) finds the table consistent.
 */
static void wpe_timer_fire(wpe_timer_entry *t, int64_t now)
{
 wpe_timer_callback cb = t->callback;
 void *data = t->data;

 if (t->repeat)
  wpe_timer_rearm(t, now);
 else
  t->id = WPE_TIMER_NONE;
 cb(data);
}

/**
 * wpe_timer_mark_pass - Note which timers a firing pass may run.
 *
 * A timer added by a callback during the pass waits for the next poll, so a
 * callback that re-adds a timer due at once cannot keep the pass from ending.
 */
static void wpe_timer_mark_pass(void)
{
 int i;

 for (i = 0; i < WPE_TIMER_MAX; i++)
  timer_table[i].in_pass = (timer_table[i].id != WPE_TIMER_NONE);
}

/**
 * wpe_timer_run_due - Run every timer that is due.
 * Return: how many ran.
 */
static int wpe_timer_run_due(void)
{
 int64_t now = wpe_clock_ms();
 int i, fired = 0;

 wpe_timer_mark_pass();
 for (i = 0; i < WPE_TIMER_MAX; i++)
 {
  wpe_timer_entry *t = &timer_table[i];

  if (t->id == WPE_TIMER_NONE || !t->in_pass)
   continue;
  t->in_pass = 0;
  if (t->due_ms > now)
   continue;
  wpe_timer_fire(t, now);
  fired++;
 }
 return fired;
}

/**
 * wpe_timer_pending - Is any timer live?
 */
static int wpe_timer_pending(void)
{
 return wpe_timer_ms_to_nearest() != WPE_POLL_FOREVER;
}

/**
 * wpe_fd_collect - Fill the poll() set from the registered fds.
 * @pfd: the set; @map: for each entry of the set, its row in fd_table.
 * Return: how many entries were filled.
 */
static int wpe_fd_collect(struct pollfd *pfd, int *map)
{
 int n = 0, i;

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
 return n;
}

/**
 * wpe_fd_run_ready - Run the callbacks of the fds poll() reported.
 * @pfd: the set poll() filled; @map: its rows in fd_table; @n: its size.
 */
static void wpe_fd_run_ready(struct pollfd *pfd, int *map, int n)
{
 int i;

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
  /* POLLOUT is dispatched for an fd that asked for it: a connect or a TLS
     handshake in progress waits to write, and only fds registered with
     POLLOUT can report it, so read-only fds are not woken by it. */
  if (re & (POLLIN | POLLPRI | POLLHUP | POLLERR | POLLOUT))
  {
   if (e->callback)
    e->callback(e->fd, e->data);
   else if (re & (POLLHUP | POLLERR))  /* wake-only fd (stdin, X11) hung up:      */
    wpe_fd_del(e->fd);                 /* drop it -- nobody can reap it, would spin */
   /* else a wake-only POLLIN just unblocks the poll; the loop reads it itself.  */
  }
 }
}

/**
 * wpe_fd_poll - Wait for an fd or a timer, then run what is ready.
 * @timeout_ms: the longest wait, or -1 for no bound other than the timers.
 *
 * The fds that woke run first, then every due timer.  With nothing
 * registered at all it returns at once; with only a timer it sleeps until
 * that timer, so a wait never spins.
 * Return: the number of ready fds plus the timers that ran, 0 when the wait
 * ended with nothing to do, -1 when poll() failed (a signal: call again).
 */
int wpe_fd_poll(int timeout_ms)
{
 struct pollfd pfd[WPE_FD_MAX];
 int map[WPE_FD_MAX];
 int n, ready;

 n = wpe_fd_collect(pfd, map);
 if (n == 0 && !wpe_timer_pending())
  return 0;
 ready = poll(pfd, n, wpe_timer_bound_wait(timeout_ms));
 if (ready < 0)
  return ready;
 if (ready > 0)
  wpe_fd_run_ready(pfd, map, n);
 return ready + wpe_timer_run_due();
}
