/* we_fdloop.c                                            */
/* Copyright (C) 2026 Juan Manuel Mendez Rey               */
/* This is free software; you can redistribute it and/or   */
/* modify it under the terms of the                        */
/* GNU General Public License, see the file COPYING.       */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "we_fdloop.h"

/* Descriptors watched at once.  Counted for the most the editor holds at
   once: the keyboard, the display, the end pipe (we_unix.c) and the
   children's wake pipe (we_child.c), four; a debug session's transport and
   its program's pty, two; four adapters Quit is still ending, each with its
   transport and pty, eight; the language server, the build, the assistant's
   reply and its host, four; the console mouse and the clipboard pipe, two.
   That is twenty; twice sixteen leaves room for what is added later. */
#define WPE_FD_MAX 32
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

/* The events a ready fd is served for: data, urgent data, a hang-up or an
   error (the callback reads the EOF and unregisters), and POLLOUT for an fd
   that asked for it (a connect or a TLS handshake waiting to write). */
#define WPE_FD_SERVED_EVENTS (POLLIN | POLLPRI | POLLHUP | POLLERR | POLLOUT)

/* One registration.  Readiness is served the way timers fire: what poll()
   reported is kept in the entry (pending) and taken off it before the
   callback runs, as a timer's in_pass is, so a nested poll that served it
   already leaves nothing for the outer pass, and a registration made after
   the poll starts with nothing.  While its callback runs the entry is busy
   and a nested poll does not watch it, as a firing timer is re-armed before
   its callback so a nested poll cannot fire it again. */
typedef struct {
 int fd;
 int events;
 wpe_fd_callback callback;
 void *data;
 int active;
 short pending;          /* revents poll() reported, not served yet           */
 int busy;               /* its callback is running                           */
} wpe_fd_entry;

static wpe_fd_entry fd_table[WPE_FD_MAX];
static int fd_count = 0;

/* Descriptors with bytes waiting for their child to read them: at most one
   per watched descriptor, and a child's input is one of those. */
#define WPE_FD_WRITERS_MAX WPE_FD_MAX
/* The longest line the stall says: a child's name, the bytes and the way
   out. */
#define WPE_FD_WRITE_LINE_MAX 256

/* The bytes queued for one descriptor: buf[off..len) waits, in order. */
typedef struct {
 int used;
 int fd;
 char *buf;
 size_t off, len, cap;
 const char *what;       /* "gdb": names the child in the stall line          */
 const char *ends;       /* what ends it, or NULL                             */
 int64_t stall_due_ms;   /* no byte taken by then: the stall line             */
 int stalled;            /* the stall line was said, no byte taken since      */
} wpe_fd_writer;

static wpe_fd_writer writer_table[WPE_FD_WRITERS_MAX];
static void (*wpe_fd_write_say_hook)(const char *line);

/* Callbacks, timers and stall lines the loop has run, ever: a wait compares
   it before and after a poll to tell work done from a wake that only a
   display reads. */
static unsigned long wpe_loop_work = 0;

static wpe_fd_entry *wpe_fd_find(int fd)
{
 int i;
 for (i = 0; i < fd_count; i++)
  if (fd_table[i].active && fd_table[i].fd == fd)
   return &fd_table[i];
 return NULL;
}

/**
 * wpe_fd_free_slot - A slot no fd uses.
 *
 * A debugger, a language server or the assistant that closes an older stream
 * must not stop the editor from opening a new one once the table has been
 * full: a slot freed anywhere in the table is reused before the table grows.
 * fd_count is the high-water mark the loops walk, so it moves only when the
 * slot is taken past its end.
 * Return: the slot, or NULL when every slot is taken.
 */
static wpe_fd_entry *wpe_fd_free_slot(void)
{
 int i;

 for (i = 0; i < fd_count; i++)
  if (!fd_table[i].active)
   return &fd_table[i];
 if (fd_count < WPE_FD_MAX)
  return &fd_table[fd_count];
 return NULL;
}

/**
 * wpe_fd_add - Watch @fd, or change what an fd already watched waits for.
 * @fd: the descriptor; @events: the poll() events; @cb: what runs when it
 * wakes, or NULL for an fd that only wakes the loop (the keyboard).
 *
 * A new registration starts with no readiness: an fd opened by a callback
 * and given the number of one it just closed is not served by what poll()
 * said about the old one, so its reader never blocks on data that is not
 * there.
 * Return: 0, or -1 when @fd is negative or every slot is taken (the caller
 * then says so and ends what it started, see e_fd_watch).
 */
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
 e = wpe_fd_free_slot();
 if (!e)
  return -1;
 e->fd = fd;
 e->events = events;
 e->callback = cb;
 e->data = data;
 e->active = 1;
 e->pending = 0;
 e->busy = 0;
 if (e == &fd_table[fd_count])
  fd_count++;
 return 0;
}

/**
 * wpe_fd_del - Stop watching @fd for input.
 * @fd: the descriptor.
 *
 * Bytes queued for it stay queued: a debug adapter's socket is read by the
 * run and then by the stop's requests, unwatched and watched again between
 * them, and a request queued meanwhile must still reach the adapter (a
 * program's pty is read and typed into the same way).  Closing is what
 * forgets the queue: wpe_fd_close.
 */
void wpe_fd_del(int fd)
{
 int i;

 for (i = 0; i < fd_count; i++)
 {
  if (fd_table[i].active && fd_table[i].fd == fd)
  {
   fd_table[i].active = 0;
   fd_table[i].pending = 0;
   while (fd_count > 0 && !fd_table[fd_count - 1].active)
    fd_count--;
   return;
  }
 }
}

/**
 * wpe_fd_refusal - The line that says a stream cannot be watched.
 * @what: what the user would have seen ("the AI reply", "the build output");
 * @fd: the descriptor wpe_fd_add refused; @line, @sz: where to write it.
 *
 * Without it a reply, a build's output or a debugger's answer that the loop
 * cannot watch simply never arrives, and the user waits for it.
 */
void wpe_fd_refusal(const char *what, int fd, char *line, size_t sz)
{
 if (fd < 0)
  snprintf(line, sz, "could not watch %s: it is not open", what);
 else
  snprintf(line, sz, "could not watch %s: too many open streams", what);
}

/**
 * wpe_fd_run_callback - Serve one registration.
 * @e: an active entry with a callback.
 *
 * Its readiness is taken before the callback runs, so neither an outer pass
 * nor a later dispatch serves it a second time; it is busy meanwhile, so a
 * dialog the callback opens does not call it again on data it has not read
 * yet.  A blocking read in a callback therefore never waits on readiness that
 * was already used, which would freeze the editor.
 *
 * Busy belongs to the slot, not to the callback: a callback that deletes its
 * own fd, adds it again and then polls can be entered again on fresh data.
 * Only the debugger's run does that (e_d_run_on_answer, when a stop is
 * followed by the next run, e_d_run_begin); it returns right after the add,
 * as any other caller that needs it must.
 */
static void wpe_fd_run_callback(wpe_fd_entry *e)
{
 e->pending = 0;
 e->busy = 1;
 wpe_loop_work++;
 e->callback(e->fd, e->data);
 /* The slot may hold another registration now; it is not running (its
    callback could only have run inside this one, and has returned). */
 e->busy = 0;
}

/**
 * wpe_fd_dispatch - Run the callback registered for fd as if it had woken.
 * @fd: a registered descriptor.
 *
 * Used by a timer that finds a source overdue (a server that never answered):
 * its own callback ends it the same way a hang-up would, so there is one
 * path to the line the user reads, not two.  A callback that is already
 * running (it opened a dialog) is not entered again; the timer finds the
 * source on a later tick.
 */
void wpe_fd_dispatch(int fd)
{
 wpe_fd_entry *e = wpe_fd_find(fd);
 if (e && e->callback && !e->busy)
  wpe_fd_run_callback(e);
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

/* ==== bytes for a child: one queue per descriptor ====

   A child that stops reading (a language server busy indexing, gdb stopped,
   an adapter wedged) fills its pipe; a plain write would then wait inside
   the editor until it reads again, and every key with it.  Here a write
   takes what the descriptor takes now and queues the rest, the loop writes
   the queue when poll() says the descriptor is writable, and a child that
   takes nothing for WPE_FD_WRITE_STALL_MS is named on the status row. */

/**
 * wpe_fd_writer_find - The queue of @fd, if it has one.
 * Return: its slot, or NULL.
 */
static wpe_fd_writer *wpe_fd_writer_find(int fd)
{
 int i;

 for (i = 0; i < WPE_FD_WRITERS_MAX; i++)
  if (writer_table[i].used && writer_table[i].fd == fd)
   return &writer_table[i];
 return NULL;
}

/**
 * wpe_fd_writer_take - The queue of @fd, made when it has none.
 * Return: its slot, or NULL when every slot holds another descriptor's bytes.
 */
static wpe_fd_writer *wpe_fd_writer_take(int fd)
{
 wpe_fd_writer *w = wpe_fd_writer_find(fd);
 int i;

 if (w)
  return w;
 for (i = 0; i < WPE_FD_WRITERS_MAX; i++)
 {
  if (writer_table[i].used)
   continue;
  memset(&writer_table[i], 0, sizeof writer_table[i]);
  writer_table[i].used = 1;
  writer_table[i].fd = fd;
  return &writer_table[i];
 }
 return NULL;
}

/**
 * wpe_fd_writer_waiting - The bytes @w still has to write.
 */
static size_t wpe_fd_writer_waiting(const wpe_fd_writer *w)
{
 return w->len - w->off;
}

/**
 * wpe_fd_writer_free - Give @w's slot and memory back.
 */
static void wpe_fd_writer_free(wpe_fd_writer *w)
{
 free(w->buf);
 memset(w, 0, sizeof *w);
}

/**
 * wpe_fd_writer_room - Make room in @w for @n more bytes.
 *
 * Asked before anything is written, so a message that could not be queued
 * is refused whole rather than cut in the middle.
 * Return: 0, or -1 when the memory is not there.
 */
static int wpe_fd_writer_room(wpe_fd_writer *w, size_t n)
{
 size_t waiting = wpe_fd_writer_waiting(w);
 char *grown;

 if (w->off > 0)
 {
  memmove(w->buf, w->buf + w->off, waiting);
  w->off = 0;
  w->len = waiting;
 }
 if (w->cap >= waiting + n)
  return 0;
 grown = realloc(w->buf, waiting + n);
 if (!grown)
  return -1;
 w->buf = grown;
 w->cap = waiting + n;
 return 0;
}

/**
 * wpe_fd_write_say_set - Where the stall line goes (the status row).
 * @say: the editor's status line, or NULL (the C tests).
 */
void wpe_fd_write_say_set(void (*say)(const char *line))
{
 wpe_fd_write_say_hook = say;
}

/**
 * wpe_fd_writer_say_stalled - Tell the user which child stopped reading,
 * how much waits for it, and what ends it.
 */
static void wpe_fd_writer_say_stalled(const wpe_fd_writer *w)
{
 char line[WPE_FD_WRITE_LINE_MAX];

 if (!wpe_fd_write_say_hook)
  return;
 if (w->ends)
  snprintf(line, sizeof line, "%s is not reading its input (%lu bytes waiting): %s",
           w->what, (unsigned long)wpe_fd_writer_waiting(w), w->ends);
 else
  snprintf(line, sizeof line, "%s is not reading its input (%lu bytes waiting)",
           w->what, (unsigned long)wpe_fd_writer_waiting(w));
 wpe_fd_write_say_hook(line);
}

/**
 * wpe_fd_writer_say_reading - Tell the user a child named as stalled reads
 * again, so the stall line does not stand for a child that recovered.
 */
static void wpe_fd_writer_say_reading(const wpe_fd_writer *w)
{
 char line[WPE_FD_WRITE_LINE_MAX];

 if (!wpe_fd_write_say_hook)
  return;
 snprintf(line, sizeof line, "%s is reading its input again", w->what);
 wpe_fd_write_say_hook(line);
}

/**
 * wpe_fd_writer_may_stall - Could @w's child still be named as stalled?
 * Bytes wait for it and the line has not been said since its last byte.
 */
static int wpe_fd_writer_may_stall(const wpe_fd_writer *w)
{
 return w->used && !w->stalled && wpe_fd_writer_waiting(w) > 0;
}

/**
 * wpe_fd_writers_ms_to_stall - How long until a child with bytes waiting
 * has taken none for WPE_FD_WRITE_STALL_MS.
 *
 * The loop bounds its sleep by it as it does by its timers, but it takes
 * no timer: the line that tells the user why nothing happens must come
 * even when every timer is held by waiting lines and children's ends.
 * Return: 0 when one is due, WPE_POLL_FOREVER when none can stall.
 */
static int wpe_fd_writers_ms_to_stall(void)
{
 int64_t now = wpe_clock_ms(), due = 0;
 int i, any = 0;

 for (i = 0; i < WPE_FD_WRITERS_MAX; i++)
 {
  const wpe_fd_writer *w = &writer_table[i];

  if (!wpe_fd_writer_may_stall(w))
   continue;
  if (!any || w->stall_due_ms < due)
   due = w->stall_due_ms;
  any = 1;
 }
 if (!any)
  return WPE_POLL_FOREVER;
 if (due <= now)
  return 0;
 if (due - now > INT_MAX)
  return INT_MAX;
 return (int)(due - now);
}

/**
 * wpe_fd_writers_say_due - Name each child that has taken no byte for
 * WPE_FD_WRITE_STALL_MS, once, after each poll.
 * Return: the lines said (each counts as the loop's work).
 */
static int wpe_fd_writers_say_due(void)
{
 int64_t now = wpe_clock_ms();
 int i, said = 0;

 for (i = 0; i < WPE_FD_WRITERS_MAX; i++)
 {
  wpe_fd_writer *w = &writer_table[i];

  if (!wpe_fd_writer_may_stall(w) || w->stall_due_ms > now)
   continue;
  w->stalled = 1;
  wpe_loop_work++;
  wpe_fd_writer_say_stalled(w);
  said++;
 }
 return said;
}

/**
 * wpe_fd_writer_progress - The child took bytes: its stall clock starts
 * again, and a stall line said for it is taken back.
 */
static void wpe_fd_writer_progress(wpe_fd_writer *w)
{
 w->stall_due_ms = wpe_clock_ms() + WPE_FD_WRITE_STALL_MS;
 if (!w->stalled)
  return;
 w->stalled = 0;
 wpe_fd_writer_say_reading(w);
}

/**
 * wpe_fd_is_nonblocking - Does a write on @fd return at once?
 *
 * The queue cannot keep the editor going on a descriptor that blocks: the
 * one write it makes would wait for the child.  A descriptor that is not
 * open answers no as well.
 */
static int wpe_fd_is_nonblocking(int fd)
{
 int fl = fcntl(fd, F_GETFL, 0);

 return fl >= 0 && (fl & O_NONBLOCK);
}

/**
 * wpe_fd_write_once - One write(2) of what @fd takes now.
 * @taken: set to the bytes written (0 when the child's pipe is full).
 * Return: WPE_FD_WRITE_SENT (whatever was taken), WPE_FD_WRITE_CLOSED when
 * the child's end is gone, WPE_FD_WRITE_REFUSED on any other error.
 */
static int wpe_fd_write_once(int fd, const char *buf, size_t n, size_t *taken)
{
 ssize_t r;

 *taken = 0;
 do
  r = write(fd, buf, n);
 while (r < 0 && errno == EINTR);
 if (r >= 0)
 {
  *taken = (size_t)r;
  return WPE_FD_WRITE_SENT;
 }
 if (errno == EAGAIN || errno == EWOULDBLOCK)
  return WPE_FD_WRITE_SENT;
 if (errno == EPIPE)
  return WPE_FD_WRITE_CLOSED;
 return WPE_FD_WRITE_REFUSED;
}

/**
 * wpe_fd_writer_flush - Write what @w's descriptor takes now.
 * Return: WPE_FD_WRITE_SENT once nothing waits (@w is then free),
 * WPE_FD_WRITE_QUEUED while bytes wait, or WPE_FD_WRITE_CLOSED /
 * WPE_FD_WRITE_REFUSED when the descriptor failed (the queue is dropped).
 */
static int wpe_fd_writer_flush(wpe_fd_writer *w)
{
 size_t taken;
 int r = wpe_fd_write_once(w->fd, w->buf + w->off, wpe_fd_writer_waiting(w),
                           &taken);

 if (r != WPE_FD_WRITE_SENT)
 {
  wpe_fd_writer_free(w);
  return r;
 }
 if (taken > 0)
 {
  w->off += taken;
  wpe_fd_writer_progress(w);
 }
 if (wpe_fd_writer_waiting(w))
  return WPE_FD_WRITE_QUEUED;
 wpe_fd_writer_free(w);
 return WPE_FD_WRITE_SENT;
}

/**
 * wpe_fd_writer_queue_rest - Write @buf now as far as @w's descriptor takes
 * it, and keep the rest, room for it made before the write.
 * Return: as wpe_fd_write.
 */
static int wpe_fd_writer_queue_rest(wpe_fd_writer *w, const char *buf, size_t n)
{
 size_t taken;
 int r;

 if (wpe_fd_writer_room(w, n) != 0)
  return WPE_FD_WRITE_REFUSED;
 r = wpe_fd_write_once(w->fd, buf, n, &taken);
 if (r != WPE_FD_WRITE_SENT)
  return r;
 if (taken == n)
  return WPE_FD_WRITE_SENT;
 memcpy(w->buf + w->len, buf + taken, n - taken);
 w->len += n - taken;
 w->stall_due_ms = wpe_clock_ms() + WPE_FD_WRITE_STALL_MS;
 return WPE_FD_WRITE_QUEUED;
}

/**
 * wpe_fd_writer_append - Put @buf behind the bytes @w already holds.
 *
 * Nothing is written now: what waits goes first, so a command is never
 * mixed into the one before it.  The queue is bounded: bytes that would take
 * it past WPE_FD_WRITE_QUEUE_MAX are refused whole.
 * Return: WPE_FD_WRITE_QUEUED or WPE_FD_WRITE_REFUSED.
 */
static int wpe_fd_writer_append(wpe_fd_writer *w, const char *buf, size_t n)
{
 if (n > (size_t)WPE_FD_WRITE_QUEUE_MAX ||
     wpe_fd_writer_waiting(w) > (size_t)WPE_FD_WRITE_QUEUE_MAX - n)
  return WPE_FD_WRITE_REFUSED;
 if (wpe_fd_writer_room(w, n) != 0)
  return WPE_FD_WRITE_REFUSED;
 memcpy(w->buf + w->len, buf, n);
 w->len += n;
 return WPE_FD_WRITE_QUEUED;
}

/**
 * wpe_fd_write - Send @n bytes of @buf to a child, never waiting for it.
 * @fd: the child's input, non-blocking; @what: names the child in the stall
 * line ("gdb", "the language server"); @ends: what ends it, said after the
 * stall line ("Debug > Quit ends the session"), or NULL.
 *
 * The bytes go out whole and in the order given, behind what is queued for
 * @fd already; what the child does not take now is written by the loop when
 * it can take more, so the user keeps typing while a child is busy.  If the
 * child takes nothing for WPE_FD_WRITE_STALL_MS, the status row says which
 * child stopped reading and how to end it, once.
 * Return: WPE_FD_WRITE_SENT when every byte went out; WPE_FD_WRITE_QUEUED
 * when some wait; WPE_FD_WRITE_REFUSED, nothing written, when the queue is
 * full, @fd is closed or would block, or no queue is free;
 * WPE_FD_WRITE_CLOSED when the child closed its input (its queue is gone).
 */
int wpe_fd_write(int fd, const void *buf, size_t n, const char *what,
                 const char *ends)
{
 wpe_fd_writer *w;
 int r;

 if (fd < 0 || !wpe_fd_is_nonblocking(fd))
  return WPE_FD_WRITE_REFUSED;
 if (n == 0)
  return WPE_FD_WRITE_SENT;
 w = wpe_fd_writer_take(fd);
 if (!w)
  return WPE_FD_WRITE_REFUSED;
 w->what = what;
 w->ends = ends;
 if (wpe_fd_writer_waiting(w))
  return wpe_fd_writer_append(w, (const char *)buf, n);
 r = wpe_fd_writer_queue_rest(w, (const char *)buf, n);
 if (r != WPE_FD_WRITE_QUEUED)
  wpe_fd_writer_free(w);
 return r;
}

/**
 * wpe_fd_write_flush - Write now what @fd's child takes of its queue.
 *
 * For a wait that polls a child itself until its answer comes (the language
 * server's handshake), so the request it waits on leaves the queue.
 * Return: as wpe_fd_writer_flush; WPE_FD_WRITE_SENT when nothing waits.
 */
int wpe_fd_write_flush(int fd)
{
 wpe_fd_writer *w = wpe_fd_writer_find(fd);

 if (!w)
  return WPE_FD_WRITE_SENT;
 return wpe_fd_writer_flush(w);
}

/**
 * wpe_fd_write_pending - The bytes still queued for @fd.
 */
long wpe_fd_write_pending(int fd)
{
 const wpe_fd_writer *w = wpe_fd_writer_find(fd);

 if (!w)
  return 0;
 return (long)wpe_fd_writer_waiting(w);
}

/**
 * wpe_fd_writer_forget - Drop @fd's queue, if any.
 */
static void wpe_fd_writer_forget(int fd)
{
 wpe_fd_writer *w = wpe_fd_writer_find(fd);

 if (!w)
  return;
 wpe_fd_writer_free(w);
}

/**
 * wpe_fd_close - Close a descriptor the loop may know: its queued bytes are
 * forgotten, it is no longer watched, then it is closed.
 * @fd: the descriptor; a negative one is left alone.
 *
 * The one way a child's input is closed.  A child being ended will not
 * read what waits for it, and the number is free again the moment close()
 * returns: the next pipe, socket or file the editor opens may get it, and
 * must never be written the old child's bytes or be served its old reader.
 * Return: what close() returned, or 0 for a negative @fd.
 */
int wpe_fd_close(int fd)
{
 if (fd < 0)
  return 0;
 wpe_fd_writer_forget(fd);
 wpe_fd_del(fd);
 return close(fd);
}

/**
 * wpe_fd_write_stalled - Has the user been told @fd's child stopped reading?
 * Return: 1 from the stall line until the child takes a byte, else 0.
 */
int wpe_fd_write_stalled(int fd)
{
 const wpe_fd_writer *w = wpe_fd_writer_find(fd);

 return w && w->stalled;
}

/**
 * wpe_fd_writers_waiting - Does any child have bytes waiting?
 */
static int wpe_fd_writers_waiting(void)
{
 int i;

 for (i = 0; i < WPE_FD_WRITERS_MAX; i++)
  if (writer_table[i].used && wpe_fd_writer_waiting(&writer_table[i]))
   return 1;
 return 0;
}

/**
 * wpe_fd_collect_writers - Add each descriptor with bytes waiting to the
 * poll() set, for POLLOUT.
 * @pfd: the set, @n entries filled already; @wmap: for each entry added,
 * its writer slot.
 * Return: the entries added.
 *
 * A writable descriptor with nothing to write is never in the set, so an
 * idle child's pipe does not wake the loop.
 */
static int wpe_fd_collect_writers(struct pollfd *pfd, int n, int *wmap)
{
 int i, added = 0;

 for (i = 0; i < WPE_FD_WRITERS_MAX; i++)
 {
  if (!writer_table[i].used || !wpe_fd_writer_waiting(&writer_table[i]))
   continue;
  pfd[n + added].fd = writer_table[i].fd;
  pfd[n + added].events = POLLOUT;
  pfd[n + added].revents = 0;
  wmap[added] = i;
  added++;
 }
 return added;
}

/**
 * wpe_fd_flush_ready - Write the queues poll() found writable.
 * @pfd: the writers' part of the set; @wmap: their slots; @n: how many.
 *
 * A hang-up or an error is written to as well: the write says EPIPE and the
 * queue goes, while the child's read side reports its end as it always has.
 * Each flush counts as the loop's work, so a wait that a flush woke asks its
 * condition again instead of taking it for a key.
 */
static void wpe_fd_flush_ready(const struct pollfd *pfd, const int *wmap, int n)
{
 int i;

 for (i = 0; i < n; i++)
 {
  wpe_fd_writer *w = &writer_table[wmap[i]];

  if (!pfd[i].revents || !w->used || w->fd != pfd[i].fd)
   continue;
  wpe_loop_work++;
  if (pfd[i].revents & POLLNVAL)   /* closed without wpe_fd_close */
  {
   wpe_fd_writer_forget(w->fd);
   continue;
  }
  wpe_fd_writer_flush(w);
 }
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
 * wpe_fd_poll_bound - The poll() timeout once the timers and the children
 * that may stall are counted.
 * @timeout_ms: the caller's bound, or WPE_POLL_FOREVER.
 *
 * An editor sleeping until a key still wakes in time to say which child
 * stopped reading its input.
 */
static int wpe_fd_poll_bound(int timeout_ms)
{
 int bound = wpe_timer_bound_wait(timeout_ms);
 int stall = wpe_fd_writers_ms_to_stall();

 if (stall == WPE_POLL_FOREVER)
  return bound;
 if (bound == WPE_POLL_FOREVER || stall < bound)
  return stall;
 return bound;
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
 wpe_loop_work++;
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
 * wpe_fd_watched - Does this poll watch the entry?
 * @e: a row of fd_table.
 *
 * Not while its callback runs: a dialog that callback opens polls again, and
 * the data the callback is about to read would call it once more, or wake
 * the dialog's loop without end.  An fd the running code turned into a
 * wake-only watch (to wait on it itself) is watched, since nothing is called.
 */
static int wpe_fd_watched(const wpe_fd_entry *e)
{
 if (!e->active)
  return 0;
 if (e->busy && e->callback)
  return 0;
 return 1;
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
  if (!wpe_fd_watched(&fd_table[i]))
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
 * wpe_fd_note_ready - Keep what poll() reported on each fd it watched.
 * @pfd: the set poll() filled; @map: its rows in fd_table; @n: its size.
 *
 * A nested poll replaces the outer one's report with a fresher one, and
 * serves it, so the outer pass finds nothing left on that fd.
 */
static void wpe_fd_note_ready(const struct pollfd *pfd, const int *map, int n)
{
 int i;

 for (i = 0; i < n; i++)
  fd_table[map[i]].pending = pfd[i].revents;
}

/**
 * wpe_fd_serve - Act on what poll() reported for one fd.
 * @e: the entry; @re: its report.
 */
static void wpe_fd_serve(wpe_fd_entry *e, short re)
{
 e->pending = 0;
 if (re & POLLNVAL)                     /* fd already closed: cannot be polled    */
 {
  wpe_fd_del(e->fd);
  return;
 }
 /* POLLHUP / POLLERR (EOF or error) MUST dispatch too: the read callback drains
    the last bytes, sees EOF and unregisters the fd.  Without it a dead server
    fd -- an LSP or debug child that exited -- keeps poll() returning instantly
    with revents set but NO POLLIN (the case when no buffered data is left, e.g.
    a server that crashed at start-up), nothing reaps it, and the input loop
    spins at 100% CPU.  The per-callback "detect dead server" guard only fires
    when it is actually called, i.e. on POLLIN; this makes it fire on hang-up
    too, on every platform regardless of how the EOF is delivered.
    POLLOUT is dispatched for an fd that asked for it; only fds registered
    with POLLOUT can report it, so read-only fds are not woken by it. */
 if (!(re & WPE_FD_SERVED_EVENTS))
  return;
 if (e->callback)
  wpe_fd_run_callback(e);
 else if (re & (POLLHUP | POLLERR))     /* wake-only fd (stdin, X11) hung up:      */
  wpe_fd_del(e->fd);                    /* drop it -- nobody can reap it, would spin */
 /* else a wake-only POLLIN just unblocks the poll; the loop reads it itself.  */
}

/**
 * wpe_fd_run_ready - Serve every report this poll left unserved.
 * @map: the rows in fd_table this poll watched; @n: how many.
 *
 * Each report is served at most once, however deep the dialogs a callback
 * opens: a row whose report a nested poll or a dispatch already served, that
 * a callback removed, or that a callback filled with a new registration, has
 * none left and waits for the next poll.
 */
static void wpe_fd_run_ready(const int *map, int n)
{
 int i;

 for (i = 0; i < n; i++)
 {
  wpe_fd_entry *e = &fd_table[map[i]];

  if (!e->active || !e->pending)
   continue;
  wpe_fd_serve(e, e->pending);
 }
}

/**
 * wpe_fd_poll - Wait for an fd or a timer, then run what is ready.
 * @timeout_ms: the longest wait, or -1 for no bound other than the timers.
 *
 * The fds that woke run first, then every due timer.  With nothing
 * registered at all it returns at once; with only a timer it sleeps until
 * that timer, so a wait never spins.
 * Bytes queued for a child are written first, when its descriptor can take
 * them, so a callback that answers the child sends behind them; a child
 * that has taken none of them for WPE_FD_WRITE_STALL_MS is named last.
 * Return: the number of ready fds plus the timers that ran and the stall
 * lines said, 0 when the wait ended with nothing to do, -1 when poll()
 * failed (a signal: call again).
 */
int wpe_fd_poll(int timeout_ms)
{
 struct pollfd pfd[WPE_FD_MAX + WPE_FD_WRITERS_MAX];
 int map[WPE_FD_MAX];
 int wmap[WPE_FD_WRITERS_MAX];
 int n, nw, ready;

 n = wpe_fd_collect(pfd, map);
 nw = wpe_fd_collect_writers(pfd, n, wmap);
 if (n + nw == 0 && !wpe_timer_pending())
  return 0;
 ready = poll(pfd, n + nw, wpe_fd_poll_bound(timeout_ms));
 if (ready < 0)
  return ready;
 wpe_fd_note_ready(pfd, map, n);
 if (ready > 0)
 {
  wpe_fd_flush_ready(pfd + n, wmap, nw);
  wpe_fd_run_ready(map, n);
 }
 ready += wpe_timer_run_due();
 return ready + wpe_fd_writers_say_due();
}

/**
 * wpe_fd_readable_now - Can a read on this descriptor answer at once?
 * @fd: a descriptor, registered with the loop or not.
 *
 * Asked without waiting, so a code path that needs to know whether the
 * debugger or the terminal has already written something (a race check, an
 * end-of-input test) never stalls the editor and never reads what the loop
 * is about to serve.  A hang-up or an error counts as readable: the read
 * that follows reports the end.
 * A signal that interrupts the probe (a child ending) asks again, so the
 * answer is never "invalid" for a descriptor that is merely idle.
 * Return: WPE_FD_READABLE (1), WPE_FD_NOT_READABLE (0), or WPE_FD_INVALID
 * (-1) when the descriptor is closed or poll() failed.
 */
int wpe_fd_readable_now(int fd)
{
 struct pollfd pfd;
 int n;

 pfd.fd = fd;
 pfd.events = POLLIN;
 do
 {
  pfd.revents = 0;
  n = poll(&pfd, 1, 0);
 } while (n < 0 && errno == EINTR);
 if (n < 0)
  return WPE_FD_INVALID;
 if (pfd.revents & POLLNVAL)
  return WPE_FD_INVALID;
 if (pfd.revents & (POLLIN | POLLPRI | POLLHUP | POLLERR))
  return WPE_FD_READABLE;
 return WPE_FD_NOT_READABLE;
}

/* What the innermost wpe_fd_wait_for is waiting for, for a status line. */
static const char *wpe_wait_what = NULL;

/**
 * wpe_fd_waiting_for - Name the wait the editor is inside, if any.
 *
 * Return: the @what of the innermost wpe_fd_wait_for still running, or NULL
 * when none is.  A waiting line or a diagnostic can then say "waiting for
 * the clipboard" instead of leaving the user with a silent editor.
 */
const char *wpe_fd_waiting_for(void)
{
 return wpe_wait_what;
}

/* What wpe_wait_bound says once the deadline has come. */
#define WPE_WAIT_EXPIRED 0

/**
 * wpe_loop_has_sources - Could the loop run anything at all?
 *
 * A wait whose loop watches no descriptor and holds no timer could only
 * sleep: nothing could make its condition hold, so it ends at once and the
 * user is not kept waiting until a deadline for an answer that cannot come.
 */
static int wpe_loop_has_sources(void)
{
 int i;

 if (wpe_timer_pending() || wpe_fd_writers_waiting())
  return 1;
 for (i = 0; i < WPE_FD_MAX; i++)
 {
  if (wpe_fd_watched(&fd_table[i]))
   return 1;
 }
 return 0;
}

/**
 * wpe_wait_bound - How long the loop may sleep inside a wait.
 * @start: when the wait began; @deadline_ms: its deadline, or
 * WPE_WAIT_FOREVER.
 *
 * The loop sleeps until something wakes it or the deadline comes, never in
 * slices, so a wait costs nothing while the user's editor is idle.
 * Return: WPE_POLL_FOREVER without a deadline, WPE_WAIT_EXPIRED once it has
 * come, else the milliseconds left.
 */
static int wpe_wait_bound(int64_t start, long deadline_ms)
{
 int64_t left;

 if (deadline_ms == WPE_WAIT_FOREVER)
  return WPE_POLL_FOREVER;
 left = deadline_ms - (wpe_clock_ms() - start);
 if (left <= WPE_WAIT_EXPIRED)
  return WPE_WAIT_EXPIRED;
 if (left > INT_MAX)
  return INT_MAX;
 return (int)left;
}

/**
 * wpe_wait_run - The body of wpe_fd_wait_for, without the naming.
 * @pred: the condition; @start: when the wait began; @deadline_ms: as there.
 *
 * The condition is asked again only after the loop has run a callback or a
 * timer, the only things that can change it.  A wake that ran nothing is a
 * key or a display event waiting on a descriptor no callback reads: the wait
 * hands it back so the caller's display can take it, instead of waking on
 * it again and again until the deadline.
 */
static int wpe_wait_run(wpe_wait_pred pred, int64_t start, long deadline_ms)
{
 unsigned long work_before;
 int bound, ready;

 for (;;)
 {
  if (pred())
   return WPE_WAIT_DONE;
  if (!wpe_loop_has_sources())
   return WPE_WAIT_TIMEOUT;
  bound = wpe_wait_bound(start, deadline_ms);
  if (bound == WPE_WAIT_EXPIRED)
   return WPE_WAIT_TIMEOUT;
  work_before = wpe_loop_work;
  ready = wpe_fd_poll(bound);
  if (ready > 0 && wpe_loop_work == work_before)
   return WPE_WAIT_INPUT;
 }
}

/**
 * wpe_fd_wait_for - Wait for a condition while the editor keeps serving
 * everything else.
 * @pred: non-zero ends the wait; it can only turn true from a callback or a
 * timer the loop runs, so it is asked once at the start and after each.
 * @deadline_ms: the longest wait, or WPE_WAIT_FOREVER.
 * @what: names the wait for wpe_fd_waiting_for().
 *
 * The one place code that must wait for something outside the loop (the
 * clipboard owner's answer, a handshake) does so: the wait sleeps in the
 * loop, so the build's output, the debugger and the timers are served
 * meanwhile, and nothing here reads a descriptor itself.  The user keeps an
 * editor that answers, a deadline always ends the wait, and a key typed
 * meanwhile comes back to the caller rather than being lost or spun on.
 * Return: WPE_WAIT_DONE when @pred held; WPE_WAIT_TIMEOUT when the deadline
 * came first, or at once when the loop has nothing that could make @pred
 * hold; WPE_WAIT_INPUT when a key or a display event waits for the caller's
 * own reader.
 */
int wpe_fd_wait_for(wpe_wait_pred pred, long deadline_ms, const char *what)
{
 const char *outer = wpe_wait_what;
 int64_t start = wpe_clock_ms();
 int result;

 wpe_wait_what = what;
 result = wpe_wait_run(pred, start, deadline_ms);
 wpe_wait_what = outer;
 return result;
}

/**
 * wpe_fd_forget_unread - Stop watching every descriptor no callback reads
 * (the keyboard, a display connection).
 *
 * At the editor's end nothing will read them again; left on the loop, a key
 * typed then would wake every wait at once without end.
 */
static void wpe_fd_forget_unread(void)
{
 int i;

 for (i = 0; i < fd_count; i++)
 {
  if (fd_table[i].active && !fd_table[i].callback)
   wpe_fd_del(fd_table[i].fd);
 }
}

/**
 * wpe_fd_end_wait - At the editor's end, keep the loop running until the
 * children it must not leave behind are gone.
 * @pred: non-zero once they are; @deadline_ms: the longest wait.
 *
 * A debug adapter told to end its program answers, exits and is reaped on
 * the loop's descriptors, timers and child watch, so a logout during a debug
 * session leaves no program behind.  The keyboard and the displays are no
 * longer read: the editor is ending, and the wait ends only on @pred or the
 * deadline, however many keys are typed meanwhile.  It is the editor's one
 * nested wait, for its end only (e_exit_teardown): a signal, an error box
 * or a closed window ends the editor from inside any dialog, and the end
 * never returns to the main loop.
 * Return: WPE_WAIT_DONE when @pred held, else WPE_WAIT_TIMEOUT.
 */
int wpe_fd_end_wait(wpe_wait_pred pred, long deadline_ms)
{
 wpe_fd_forget_unread();
 if (wpe_fd_wait_for(pred, deadline_ms, "the editor's children to end") ==
     WPE_WAIT_DONE)
  return WPE_WAIT_DONE;
 return WPE_WAIT_TIMEOUT;
}
