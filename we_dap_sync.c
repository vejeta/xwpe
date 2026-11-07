/* we_dap_sync.c -- the debug adapter engine driven by poll(), for a caller
   that has no event loop.

   The engine (we_dap.c) never waits: the editor feeds it from its loop, one
   readable descriptor at a time, and is told of every answer by callback.
   The C tests and the adapter integration tests have no such loop, so this
   file gives them the same engine as calls that return once the answer has
   come: each one sends its request through the engine's own API and then
   waits on the transport with poll() until the engine has handed the answer
   over.  The editor does not link it, so no wait of its can hold the
   editor's keys.  See we_dap.h for what each call does. */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#if defined(__linux__)
# include <sys/syscall.h>
#endif
#if defined(HAVE_KQUEUE) && defined(HAVE_SYS_EVENT_H)
# include <sys/event.h>
#endif

#include "we_dap.h"

#define SYNC_NO_FD        (-1)
#define SYNC_ACCEPT_MS    10000   /* dlv gets this long to dial in */
#define SYNC_POLL_FOREVER (-1)
#define SYNC_NO_DEADLINE  (-1L)
#define SYNC_MS_PER_SEC   1000L
#define SYNC_NS_PER_MS    1000000L
#define SYNC_CMD_MAX      64      /* a DAP command's name */

/* What a wait found (sync_wait_readable). */
#define SYNC_WAIT_READY     0    /* the descriptor is readable           */
#define SYNC_WAIT_TIMED_OUT 1    /* the deadline passed                  */
#define SYNC_WAIT_GAVE_UP   2    /* poll failed or the editor is ending  */

/* Poll set size of a session wait: the transport, the pty, the end request. */
#define SYNC_WAIT_FDS 3
/* Of the wait for the adapter's exit: its exit descriptor (pidfd or
   kqueue), the pty, the transport and the end request. */
#define SYNC_EXIT_FDS 4

/* ---- the end request ---------------------------------------------------- */

/* The end request (see e_dap_set_end_watch): a descriptor that becomes
   readable when it comes, the question whether it came, and the save of
   the user's work, run once if it comes while Quit waits. */
static int dap_end_fd = SYNC_NO_FD;
static int (*dap_end_requested)(void) = NULL;
static void (*dap_end_save)(void) = NULL;
static int dap_end_handled = 0;   /* Quit's waits saw it and saved */

void e_dap_set_end_watch(int fd, int (*requested)(void), void (*save)(void))
{
 dap_end_fd = fd;
 dap_end_requested = requested;
 dap_end_save = save;
 dap_end_handled = 0;
}

/**
 * sync_end_watch_add - Put the end request's descriptor in a wait's poll set.
 * @p: the set; @np: how many entries it has, raised by one when added.
 *
 * With it in the set, a SIGTERM or SIGHUP that lands just before the wait
 * starts still wakes it, so a caller is never left waiting for a program
 * that runs for ever.
 * Return: its index in @p, or -1 when there is no end request to watch.
 */
static int sync_end_watch_add(struct pollfd *p, int *np)
{
 if (dap_end_fd == SYNC_NO_FD)
  return -1;
 p[*np].fd = dap_end_fd;
 p[*np].events = POLLIN;
 p[*np].revents = 0;
 return (*np)++;
}

/**
 * sync_end_pending - Must a wait give up because the end was asked for?
 * Return: 1 when it was, else 0.
 */
static int sync_end_pending(void)
{
 if (!dap_end_requested)
  return 0;
 return dap_end_requested() != 0;
}

/**
 * sync_poll_fired - Did entry @i of a poll set wake the poll?
 * @p: the set; @i: the entry, or -1 when it was not in the set; @pret:
 * what poll() returned.
 */
static int sync_poll_fired(const struct pollfd *p, int i, int pret)
{
 if (i < 0 || pret <= 0)
  return 0;
 return p[i].revents != 0;
}

/**
 * sync_quit_end_watch_add - Watch the end request during Quit's waits,
 * until it has been handled.
 * @p, @np: the poll set, as for sync_end_watch_add.
 *
 * Once handled the descriptor stays readable, so it is left out of the
 * later polls, which would otherwise return at once until their deadline.
 * Return: its index in @p, or -1 when it is not watched.
 */
static int sync_quit_end_watch_add(struct pollfd *p, int *np)
{
 if (dap_end_handled)
  return -1;
 return sync_end_watch_add(p, np);
}

/**
 * sync_quit_end_check - The end request came while Quit waits: save the
 * user's work now, then let Quit go on.
 * @fired: the end descriptor woke the poll.
 *
 * Quit's waits are bounded but can last seconds (an adapter that never
 * answers), and a second signal ends the program at once without a save.
 * Saving first means the work is on disk whatever comes next, while the
 * adapter is still ended properly.  The save runs once.
 */
static void sync_quit_end_check(int fired)
{
 if (dap_end_handled)
  return;
 if (!fired && !sync_end_pending())
  return;
 dap_end_handled = 1;
 if (sync_end_pending() && dap_end_save)
  dap_end_save();
}

/* ---- waiting ------------------------------------------------------------ */

/**
 * sync_now_ms - The monotonic clock, in milliseconds.
 *
 * Deadlines count from it, so a clock change during Quit neither cuts the
 * adapter's time short nor stretches it.
 */
static long sync_now_ms(void)
{
 struct timespec ts;

 clock_gettime(CLOCK_MONOTONIC, &ts);
 return (long)ts.tv_sec * SYNC_MS_PER_SEC + ts.tv_nsec / SYNC_NS_PER_MS;
}

/**
 * sync_ms_left - What remains of @deadline, as a poll() timeout.
 * @deadline: a sync_now_ms() instant, or SYNC_NO_DEADLINE.
 * Return: the milliseconds left (0 once passed), or SYNC_POLL_FOREVER.
 */
static int sync_ms_left(long deadline)
{
 long left;

 if (deadline == SYNC_NO_DEADLINE)
  return SYNC_POLL_FOREVER;
 left = deadline - sync_now_ms();
 if (left < 0)
  return 0;
 return (int)left;
}

/**
 * sync_wait_readable - Wait until @fd is readable, passing on the program's
 * output meanwhile.
 * @s: the session; @fd: the descriptor waited on (the transport, or the
 * listening socket while dlv has not dialled in); @deadline: a
 * sync_now_ms() instant, or SYNC_NO_DEADLINE; @quitting: Quit's own wait.
 *
 * dlv's pty is drained here, so a program that prints a lot never stalls
 * on a full pty while the caller waits; the end request is watched, so a
 * SIGTERM or SIGHUP ends the wait at once.  Quit's own waits are the
 * exception: they are bounded, and cutting them short sends lldb-dap
 * SIGTERM before it has acted on `disconnect`, which leaves its running
 * program behind (measured: 5 of 5 times; SIGTERM or SIGKILL alone never
 * ends it).
 * Return: SYNC_WAIT_READY, SYNC_WAIT_TIMED_OUT or SYNC_WAIT_GAVE_UP.
 */
static int sync_wait_readable(e_dap_session *s, int fd, long deadline,
                              int quitting)
{
 for (;;)
 {
  struct pollfd p[SYNC_WAIT_FDS];
  int np = 0, pty_i = -1, end_i, pret, pty = e_dap_output_fd(s);

  if (fd < 0)
   return SYNC_WAIT_GAVE_UP;
  if (deadline != SYNC_NO_DEADLINE && sync_ms_left(deadline) == 0)
   return SYNC_WAIT_TIMED_OUT;
  p[np].fd = fd; p[np].events = POLLIN; p[np].revents = 0; np++;
  if (pty >= 0)
  {
   pty_i = np;
   p[np].fd = pty; p[np].events = POLLIN; p[np].revents = 0;
   np++;
  }
  if (quitting)
   end_i = sync_quit_end_watch_add(p, &np);
  else
   end_i = sync_end_watch_add(p, &np);
  pret = poll(p, np, sync_ms_left(deadline));
  if (!quitting && sync_end_pending())
   return SYNC_WAIT_GAVE_UP;
  if (quitting)
   sync_quit_end_check(sync_poll_fired(p, end_i, pret));
  if (pret < 0 && errno == EINTR)
   continue;
  if (pret < 0)
   return SYNC_WAIT_GAVE_UP;
  if (pret == 0)
   return SYNC_WAIT_TIMED_OUT;
  if (pty_i >= 0 && (p[pty_i].revents & (POLLIN | POLLHUP | POLLERR)))
   e_dap_drain_output(s);
  if (p[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))
   return SYNC_WAIT_READY;
 }
}

/* ---- one request, waited for -------------------------------------------- */

/* What a waited request was told. */
typedef struct {
 int done;
 E_D_REQ_END how;
 E_D_ANSWER ans;               /* its own copy of the answer's text */
} sync_call;

/**
 * sync_call_answered - The engine ended a waited request: keep its answer.
 * @how, @ans: how it ended and what came; @ctx: the sync_call.
 */
static int sync_call_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 sync_call *c = ctx;

 c->done = 1;
 c->how = how;
 memset(&c->ans, 0, sizeof c->ans);
 if (ans && ans->text)
 {
  c->ans.text = strdup(ans->text);
  c->ans.len = c->ans.text ? strlen(c->ans.text) : 0;
 }
 if (ans)
  c->ans.error = ans->error;
 return E_D_STEP_STOP;
}

/**
 * sync_call_wait - Read until the engine has ended request @c.
 * @s: the session; @c: the request, sent with sync_call_answered.
 *
 * Every other message that comes meanwhile goes to its owner: output to
 * the host, a setBreakpoints answer to on_breakpoints, a stop to the run.
 * A wait that gives up (the end request) ends the request too, so the
 * engine never calls back into a request nobody waits for.
 * Return: 1 when the adapter answered, else 0.
 */
static int sync_call_wait(e_dap_session *s, sync_call *c)
{
 while (!c->done)
 {
  int fd = e_dap_fd(s);

  if (fd < 0)
   break;
  if (sync_wait_readable(s, fd, SYNC_NO_DEADLINE, 0) != SYNC_WAIT_READY)
   break;
  e_dap_read_now(s);
 }
 if (!c->done)
  e_dap_requests_cancel(s, E_D_REQ_GONE);
 return c->how == E_D_REQ_DONE;
}

/**
 * sync_call_run - Send the request a stage function built and wait for it.
 * @s: the session; @cmd: the staged command; @c: receives the answer.
 * Return: 1 when the adapter answered, else 0.
 */
static int sync_call_run(e_dap_session *s, const char *cmd, sync_call *c)
{
 memset(c, 0, sizeof(*c));
 if (e_dap_req_request(s, cmd, sync_call_answered, c) != E_D_SEND_OK)
  return 0;
 return sync_call_wait(s, c);
}

char *e_dap_evaluate(e_dap_session *s, const char *expr)
{
 char cmd[SYNC_CMD_MAX];
 sync_call c;
 char *out = NULL;

 if (!s || e_dap_ended(s))
  return NULL;
 if (!e_dap_stage_evaluate(s, expr, cmd, sizeof cmd))
  return NULL;
 if (sync_call_run(s, cmd, &c))
  out = e_dap_answer_result(&c.ans);
 free(c.ans.text);
 return out;
}

int e_dap_stack_trace(e_dap_session *s, e_dap_frame *frames, int max)
{
 char cmd[SYNC_CMD_MAX];
 sync_call c;
 int n = -1;

 if (!s || e_dap_ended(s) || max <= 0)
  return -1;
 if (!e_dap_stage_stack_trace(s, max, cmd, sizeof cmd))
  return -1;
 if (sync_call_run(s, cmd, &c))
  n = e_dap_answer_frames(&c.ans, frames, max);
 free(c.ans.text);
 return n;
}

int e_dap_set_file_breakpoints(e_dap_session *s, const char *file,
                               const int *lines, int n)
{
 char cmd[SYNC_CMD_MAX];
 sync_call c;
 int ret = -1;

 if (!s || e_dap_ended(s))
  return -1;
 if (!e_dap_stage_breakpoints(s, file, lines, n, cmd, sizeof cmd))
  return -1;
 if (sync_call_run(s, cmd, &c))
  ret = 0;
 free(c.ans.text);
 return ret;
}

/* ---- the run, waited for ------------------------------------------------ */

/**
 * sync_run_wait - Wait for the run to stop or end.
 * @s: the session; @deadline: a sync_now_ms() instant, or SYNC_NO_DEADLINE;
 * @quitting: Quit's own wait (see sync_wait_readable).
 * Return: E_DAP_RUN_STOPPED, E_DAP_RUN_ENDED, or E_DAP_RUN_GOING when the
 * wait gave up (deadline, end request).
 */
static int sync_run_wait(e_dap_session *s, long deadline, int quitting)
{
 int st = e_dap_run_pump(s);

 while (st == E_DAP_RUN_GOING)
 {
  int fd = e_dap_fd(s);

  if (fd < 0)
   break;
  if (sync_wait_readable(s, fd, deadline, quitting) != SYNC_WAIT_READY)
   break;
  st = e_dap_run_pump(s);
 }
 return st;
}

int e_dap_run_to_first_stop(e_dap_session *s)
{
 int st;

 if (!s)
  return -1;
 if (e_dap_ended(s))
 {
  e_dap_forward_output(s);
  return 0;
 }
 st = sync_run_wait(s, SYNC_NO_DEADLINE, 0);
 /* Program output is forwarded from here on even without a stop (the
    program ended first); the engine did it itself for a stop. */
 e_dap_forward_output(s);
 if (st == E_DAP_RUN_STOPPED)
  return 0;
 return e_dap_ended(s) ? 0 : -1;
}

int e_dap_step(e_dap_session *s, const char *verb)
{
 if (e_dap_resume(s, verb) != 0)
  return -1;
 sync_run_wait(s, SYNC_NO_DEADLINE, 0);
 return 0;       /* ended is normal for `continue` past the last breakpoint */
}

/* ---- the start, waited for ---------------------------------------------- */

/**
 * sync_start_drive - Run the start to its goal, waiting with poll().
 * @s: the session.
 *
 * The adapter gets SYNC_ACCEPT_MS to dial in; once connected, its answers
 * are waited for without a deadline.
 * Return: E_DAP_START_READY or E_DAP_START_FAILED.
 */
static int sync_start_drive(e_dap_session *s)
{
 int st = e_dap_start_state(s);

 while (st == E_DAP_START_WAITING)
 {
  long deadline = SYNC_NO_DEADLINE;

  if (e_dap_start_dialling(s))
   deadline = sync_now_ms() + SYNC_ACCEPT_MS;
  if (sync_wait_readable(s, e_dap_start_fd(s), deadline, 0) !=
      SYNC_WAIT_READY)
   return e_dap_start_give_up(s);
  st = e_dap_start_pump(s);
 }
 return st;
}

/**
 * sync_open_finish - Drive a session to the initialize answer.
 * @s: the session, its start begun (or NULL).
 * Return: the session, or NULL (and closed) when the adapter did not answer.
 */
static e_dap_session *sync_open_finish(e_dap_session *s)
{
 if (!s)
  return NULL;
 e_dap_start_hold_after_initialize(s);
 if (sync_start_drive(s) != E_DAP_START_READY)
 {
  e_dap_close(s);
  return NULL;
 }
 return s;
}

e_dap_session *e_dap_open(char *const argv[], const char *program,
                          const char *entry_func, const char *cwd,
                          const e_dap_host *host)
{
 return sync_open_finish(e_dap_start(argv, program, entry_func, cwd, host));
}

e_dap_session *e_dap_open_stdio(char *const argv[], const char *program,
                                const char *entry_func, const char *cwd,
                                const e_dap_host *host)
{
 return sync_open_finish(e_dap_start_stdio(argv, program, entry_func, cwd,
                                           host));
}

e_dap_session *e_dap_open_tcp(const char *host, int port, const char *program,
                              const char *entry_func, const e_dap_host *host_cb)
{
 return sync_open_finish(e_dap_start_tcp(host, port, program, entry_func,
                                         host_cb));
}

int e_dap_run(e_dap_session *s)
{
 if (!s)
  return -1;
 e_dap_start_go_on(s);
 if (sync_start_drive(s) != E_DAP_START_READY)
 {
  e_dap_forward_output(s);
  return e_dap_ended(s) ? 0 : -1;
 }
 return e_dap_run_to_first_stop(s);
}

/* ---- Quit, waited for --------------------------------------------------- */

/**
 * sync_quit_and_wait - Quit the adapter and wait, briefly, for its answer.
 * @s: the session.
 *
 * Used when the caller closes a session it did not disconnect itself.  The
 * wait ends at the adapter's answer or the end of its transport, or at
 * E_DAP_DISCONNECT_MS.  The end request does not cut it short: lldb-dap
 * ends a running program only on `disconnect`, and giving up here sends it
 * SIGTERM before it has acted on it, so the program stays behind
 * (measured on a running lldb-dap program, 5 of 5).
 */
static void sync_quit_and_wait(e_dap_session *s)
{
 if (e_dap_disconnect_sent(s))
  return;                        /* the caller disconnected and waited */
 if (e_dap_disconnect(s) != 0)
  return;
 sync_run_wait(s, sync_now_ms() + E_DAP_DISCONNECT_MS, 1);
}

/* The waits below mirror the editor's ladder (we_child.h): after the answer
   the owner's grace to exit, after SIGTERM its kill grace. */
#define SYNC_EXIT_MS WPE_CHILD_END_GRACE_MS
#define SYNC_TERM_MS WPE_CHILD_END_KILL_MS

/**
 * sync_adapter_gone - Has the adapter exited?  Reap it if so.
 * @s: the session.
 *
 * A caller of these waits has no SIGCHLD handler of the editor's, so the
 * adapter is reaped here, and the children's owner then stops following it.
 * Return: 1 once it is gone (and reaped), else 0.
 */
static int sync_adapter_gone(e_dap_session *s)
{
 pid_t pid = e_dap_adapter_pid(s), r;
 int st;

 if (pid <= 0)
  return 1;
 r = waitpid(pid, &st, WNOHANG);
 if (r != pid && !(r < 0 && errno == ECHILD))
  return 0;
 e_dap_adapter_reaped(s);
 return 1;
}

#if defined(DAP_NO_PIDFD)
/* The test build of a system with neither pidfd nor kqueue. */
#elif defined(__linux__) && defined(SYS_pidfd_open)
# define SYNC_EXIT_BY_PIDFD 1
#elif defined(HAVE_KQUEUE) && defined(HAVE_SYS_EVENT_H)
# define SYNC_EXIT_BY_KQUEUE 1
#endif

#if defined(SYNC_EXIT_BY_KQUEUE)
/**
 * sync_kqueue_exit_open - The BSD and macOS form of a pidfd: a kqueue that
 * becomes readable when @pid exits (EVFILT_PROC, NOTE_EXIT).
 * @pid: the adapter, this process's own child.
 * Return: the descriptor, or -1 (the adapter already exited, or no room).
 */
static int sync_kqueue_exit_open(pid_t pid)
{
 struct kevent ev;
 int kq = kqueue();

 if (kq < 0)
  return SYNC_NO_FD;
 EV_SET(&ev, pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
 if (kevent(kq, &ev, 1, NULL, 0, NULL) < 0)
 {
  close(kq);
  return SYNC_NO_FD;
 }
 return kq;
}
#endif

/**
 * sync_exit_fd_open - A descriptor that becomes readable when @pid exits:
 * a pidfd on Linux, a kqueue on the BSDs and macOS, or -1 where the system
 * has neither (then the end of the adapter's output is waited for).
 * @pid: the adapter, this process's own child.
 */
static int sync_exit_fd_open(pid_t pid)
{
#if defined(SYNC_EXIT_BY_PIDFD)
 return (int)syscall(SYS_pidfd_open, pid, 0);
#elif defined(SYNC_EXIT_BY_KQUEUE)
 return sync_kqueue_exit_open(pid);
#else
 (void)pid;
 return SYNC_NO_FD;
#endif
}

/**
 * sync_exit_grace_wait - With no exit descriptor, the adapter has closed
 * every descriptor it held: it may be exiting, or still cleaning up after
 * SIGTERM.  Give it what is left of @deadline, then look once.
 * @s: the session; @deadline: a sync_now_ms() instant.
 *
 * Only systems with neither pidfd nor kqueue reach this, and only after the
 * adapter closed all its descriptors: there a Quit can take the whole bound.
 * Return: 1 once the adapter is gone (and reaped), else 0.
 */
static int sync_exit_grace_wait(e_dap_session *s, long deadline)
{
 for (;;)
 {
  struct pollfd p[1];
  int np = 0, end_i, r;

  if (sync_adapter_gone(s))
   return 1;
  end_i = sync_quit_end_watch_add(p, &np);
  r = poll(p, np, sync_ms_left(deadline));
  if (r < 0 && errno == EINTR)
   continue;
  sync_quit_end_check(sync_poll_fired(p, end_i, r));
  if (r <= 0)
   return sync_adapter_gone(s);
 }
}

/**
 * sync_exit_watch - Wait until the adapter has exited, or @deadline.
 * @s: the session; @exit_fd: from sync_exit_fd_open, or -1; @deadline: a
 * sync_now_ms() instant.
 *
 * What the adapter still writes meanwhile is read and dropped, so it never
 * blocks on a full pipe while it exits; an adapter that writes without end
 * still cannot hold the wait past @deadline.  Without an exit descriptor,
 * the end of every descriptor the adapter holds stands for "may have
 * exited", and the rest of the bound is given by sync_exit_grace_wait.
 * Return: 1 once the adapter is gone (and reaped), 0 at the deadline.
 */
static int sync_exit_watch(e_dap_session *s, int exit_fd, long deadline)
{
 for (;;)
 {
  struct pollfd p[SYNC_EXIT_FDS];
  int np = 0, pty_i = -1, rd_i = -1, end_i, r;
  int pty = e_dap_output_fd(s), rd = e_dap_fd(s);

  if (sync_adapter_gone(s))
   return 1;
  if (sync_ms_left(deadline) == 0)
   return 0;
  if (exit_fd >= 0)
  {
   p[np].fd = exit_fd; p[np].events = POLLIN; p[np].revents = 0; np++;
  }
  if (pty >= 0)
  {
   pty_i = np;
   p[np].fd = pty; p[np].events = POLLIN; p[np].revents = 0;
   np++;
  }
  if (rd >= 0)
  {
   rd_i = np;
   p[np].fd = rd; p[np].events = POLLIN; p[np].revents = 0; np++;
  }
  if (np == 0)
   return sync_exit_grace_wait(s, deadline);
  end_i = sync_quit_end_watch_add(p, &np);
  r = poll(p, np, sync_ms_left(deadline));
  if (r < 0 && errno == EINTR)
   continue;
  sync_quit_end_check(sync_poll_fired(p, end_i, r));
  if (r <= 0)
   return sync_adapter_gone(s);
  if (pty_i >= 0 && p[pty_i].revents)
   e_dap_drain_output(s);
  if (rd_i >= 0 && p[rd_i].revents)
   e_dap_discard_input(s);
 }
}

/**
 * sync_wait_adapter_exit - Give the adapter up to @ms to exit by itself.
 * @s: the session; @ms: the bound.
 * Return: 1 once it is gone and reaped, else 0.
 */
static int sync_wait_adapter_exit(e_dap_session *s, int ms)
{
 int exit_fd = sync_exit_fd_open(e_dap_adapter_pid(s));
 int gone = sync_exit_watch(s, exit_fd, sync_now_ms() + ms);

 if (exit_fd >= 0)
  close(exit_fd);
 return gone;
}

/**
 * sync_reap_adapter - Make sure the adapter is gone, and reap it.
 * @s: the session.
 *
 * An adapter that answered Quit is given SYNC_EXIT_MS to exit by itself
 * and is then only reaped: dlv closes its socket before it is done, so the
 * end of the transport is not taken for its exit.  One still there gets
 * SIGTERM and SYNC_TERM_MS, a chance to end by itself, and SIGKILL after
 * that.
 * Every wait is a poll on the adapter's exit, bounded, never a sleep.
 */
static void sync_reap_adapter(e_dap_session *s)
{
 pid_t pid = e_dap_adapter_pid(s);
 int st;

 if (pid <= 0)
  return;
 e_dap_hang_up(s);
 if (e_dap_disconnect_answered(s) &&
     sync_wait_adapter_exit(s, SYNC_EXIT_MS))
  return;
 if (sync_adapter_gone(s))
  return;
 e_dap_signal(s, SIGTERM);
 if (sync_wait_adapter_exit(s, SYNC_TERM_MS))
  return;
 e_dap_signal(s, SIGKILL);
 while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
  ;
 e_dap_adapter_reaped(s);
}

void e_dap_close(e_dap_session *s)
{
 if (!s)
  return;
 sync_quit_and_wait(s);
 sync_reap_adapter(s);
 e_dap_free(s);
}
