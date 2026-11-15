/* we_dap_sync.c -- the debug adapter engine as calls that wait, for a caller
   that has no editor around it.

   The engine (we_dap.c) never waits: the editor feeds it from its loop, one
   readable descriptor at a time, and is told of every answer by callback.
   The C tests and the adapter integration tests have no editor, so this
   file gives them the same engine as calls that return once the answer has
   come: each one sends its request through the engine's own API and then
   waits on the fd loop (we_fdloop.c), the transport registered with the
   engine's own pump as its callback, until the engine has handed the answer
   over, the adapter has gone, or the wait's budget has passed.  The start,
   the requests, the run and Quit all do so: Quit's own waits are the
   editor's ladder (we_child.c) seen from a caller with no editor, and the
   adapter's end reaches them through the owner's SIGCHLD reaper, the same on
   every system.  The editor does not link it, so no wait of its can hold the
   editor's keys.
   See we_dap.h for what each call does and for the budgets. */

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

#include "we_dap.h"
#include "we_child.h"
#include "we_fdloop.h"

#define SYNC_NO_FD        (-1)
#define SYNC_ACCEPT_MS    10000   /* dlv gets this long to dial in */
#define SYNC_LAUNCH_MS    60000   /* launch to the start's end: dlv builds first */
#define SYNC_REQUEST_MS   30000   /* a request the adapter answers */
#define SYNC_RUN_MS       60000   /* a run to its next stop */
#define SYNC_LEARN_EXIT_MS 300    /* how long a failure line waits to learn how the adapter ended */
#define SYNC_MS_PER_SEC   1000L
#define SYNC_NS_PER_MS    1000000L
#define SYNC_CMD_MAX      64      /* a DAP command's name */

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

/* What a waited request was told. */
typedef struct {
 int done;
 E_D_REQ_END how;
 E_D_ANSWER ans;               /* its own copy of the answer's text */
} sync_call;

/* ---- the budgets -------------------------------------------------------- */

#define SYNC_BUDGETS   5          /* E_DAP_SYNC_START .. E_DAP_SYNC_ACCEPT */
#define SYNC_SCALE_ENV "XWPE_TEST_WAIT_SCALE"
#define SYNC_SCALE_MAX 1000.0
#define SYNC_LAST_MAX  192
#define SYNC_TEXT_MAX  96

static const long sync_budget_default[SYNC_BUDGETS] = {
 E_DAP_START_ANSWER_MS, SYNC_LAUNCH_MS, SYNC_REQUEST_MS, SYNC_RUN_MS,
 SYNC_ACCEPT_MS
};
static long sync_budget_override[SYNC_BUDGETS];
static char sync_last[SYNC_LAST_MAX];
static int  sync_last_hit_budget;   /* the last wait ended at its budget */

void e_dap_sync_budget_set(int which, long ms)
{
 if (which < 0 || which >= SYNC_BUDGETS)
  return;
 sync_budget_override[which] = ms > 0 ? ms : 0;
}

const char *e_dap_sync_last_wait(void)
{
 return sync_last;
}

/**
 * sync_scale - The factor XWPE_TEST_WAIT_SCALE asks for (a float): 1 when it
 * is unset, not a number, or not above zero.
 *
 * What the pyte suite and the macOS workflow set for a slow machine, so a
 * wait given 30 s is given 90 s there.
 */
static double sync_scale(void)
{
 const char *e = getenv(SYNC_SCALE_ENV);
 char *end = NULL;
 double v;

 if (!e || !*e)
  return 1.0;
 v = strtod(e, &end);
 if (end == e || !(v > 0.0) || v > SYNC_SCALE_MAX)
  return 1.0;
 return v;
}

/**
 * sync_scaled - @ms times the scale, at least 1.
 */
static long sync_scaled(long ms)
{
 double v = (double)ms * sync_scale();

 return v < 1.0 ? 1L : (long)v;
}

/**
 * sync_budget - The budget of the wait of kind @which, scaled.
 * @which: E_DAP_SYNC_*.
 */
static long sync_budget(int which)
{
 long ms = sync_budget_override[which] > 0 ? sync_budget_override[which]
                                           : sync_budget_default[which];

 return sync_scaled(ms);
}

/* ---- the waits on the loop ---------------------------------------------- */

/* What a wait on the loop is for. */
#define SYNC_ON_START 0    /* the start's answers, pumped by e_dap_start_pump */
#define SYNC_ON_CALL  1    /* a request's answer, read by e_dap_read_now      */
#define SYNC_ON_RUN   2    /* the run's next stop, pumped by e_dap_run_pump   */
/* Quit's two waits.  An end request does not cut them short: it makes the
   save run once (sync_quit_end_check) and Quit goes on, because lldb-dap ends
   a running program only on `disconnect` (measured: 5 of 5 times, SIGTERM or
   SIGKILL alone leave it behind). */
#define SYNC_ON_QUIT  3    /* the answer to `disconnect`, pumped as a run     */
#define SYNC_ON_EXIT  4    /* the adapter's exit; what it writes is dropped   */

/** sync_quitting - Is the wait that runs one of Quit's? */
static int sync_quitting(int mode)
{
 return mode == SYNC_ON_QUIT || mode == SYNC_ON_EXIT;
}

/* The start's phases, each with its own budget. */
#define SYNC_PHASE_DIAL   0   /* a reverse-TCP adapter has not dialled in */
#define SYNC_PHASE_INIT   1   /* initialize is not answered               */
#define SYNC_PHASE_LAUNCH 2   /* launch, to the start's end               */

/* How a wait on the loop ended. */
#define SYNC_ENDED  0   /* what it waited for came, or the adapter or the   */
                        /* transport ended, or the end was requested        */
#define SYNC_BUDGET 1   /* the budget passed                                */
#define SYNC_LOST   2   /* the loop had nothing to wait on                  */

/* The wait that runs now.  One at a time: the loop's predicate has no
   argument, so what it asks is kept here. */
static struct {
 e_dap_session *s;
 int mode;
 const sync_call *call;
 int st;            /* the last pump's answer (SYNC_ON_START, SYNC_ON_RUN) */
 int phase;         /* the start's phase the wait began in */
 int fd;            /* the transport on the loop, or SYNC_NO_FD */
 int pty_fd;        /* the program's pty on the loop, or SYNC_NO_FD */
 int end_fd;        /* the end request on the loop, or SYNC_NO_FD */
 int drained;       /* the transport was read once after the adapter left */
} sl = { NULL, 0, NULL, 0, 0, SYNC_NO_FD, SYNC_NO_FD, SYNC_NO_FD, 0 };

/**
 * sync_unwatch - Take descriptor @*slot off the loop.
 * @slot: one of sl's descriptors; it is -1 afterwards.
 */
static void sync_unwatch(int *slot)
{
 if (*slot == SYNC_NO_FD)
  return;
 wpe_fd_del(*slot);
 *slot = SYNC_NO_FD;
}

static void sync_on_transport(int fd, void *data);

/**
 * sync_follow_start_fd - Watch the start's descriptor, which changes once
 * (the listening socket, then the transport a reverse-TCP adapter dialled).
 *
 * Once the start is over, or the engine has no descriptor for it, nothing
 * is watched and the wait ends.
 */
static void sync_follow_start_fd(void)
{
 int fd = (sl.st == E_DAP_START_WAITING) ? e_dap_start_fd(sl.s) : SYNC_NO_FD;

 if (fd == sl.fd)
  return;
 sync_unwatch(&sl.fd);
 if (fd < 0)
  return;
 if (wpe_fd_add(fd, POLLIN, sync_on_transport, NULL) == 0)
  sl.fd = fd;
}

/**
 * sync_on_transport - The loop found the transport readable: give it to the
 * engine's own pump, as the editor's loop does.
 * @fd: the descriptor; @data: unused.
 *
 * The loop's contract holds: when the transport reached its end it is read
 * to it and taken off the loop, else its hang-up would wake the loop again
 * and again until the budget.
 */
static void sync_on_transport(int fd, void *data)
{
 (void)fd;
 (void)data;
 if (sl.mode == SYNC_ON_START)
 {
  sl.st = e_dap_start_pump(sl.s);
  sync_follow_start_fd();
  return;
 }
 if (sl.mode == SYNC_ON_RUN || sl.mode == SYNC_ON_QUIT)
 {
  sl.st = e_dap_run_pump(sl.s);
  if (sl.st != E_DAP_RUN_GOING || e_dap_fd(sl.s) < 0)
   sync_unwatch(&sl.fd);
  return;
 }
 if (sl.mode == SYNC_ON_EXIT)
 {
  /* Read and drop, so the adapter never blocks on a full pipe while it
     exits; at the end of the transport read it to it and leave the loop,
     else its hang-up wakes the loop for the whole grace. */
  e_dap_discard_input(sl.s);
  if (e_dap_fd(sl.s) < 0)
   sync_unwatch(&sl.fd);
  return;
 }
 if (!e_dap_read_now(sl.s))
  sync_unwatch(&sl.fd);
}

/**
 * sync_on_pty - The loop found the program's pty readable: pass its output
 * on, so a program that prints a lot never stalls on a full pty.
 */
static void sync_on_pty(int fd, void *data)
{
 (void)fd;
 (void)data;
 e_dap_drain_output(sl.s);
 if (e_dap_output_fd(sl.s) < 0)
  sync_unwatch(&sl.pty_fd);
}

/**
 * sync_on_end_request - The end request came (a SIGTERM or SIGHUP): the
 * wait gives up.  The descriptor stays readable, so it leaves the loop here.
 */
static void sync_on_end_request(int fd, void *data)
{
 (void)fd;
 (void)data;
 if (sync_quitting(sl.mode))
  sync_quit_end_check(1);        /* save the work, once; Quit goes on */
 sync_unwatch(&sl.end_fd);
}

/**
 * sync_start_phase - Which phase of the start @s is in.
 */
static int sync_start_phase(const e_dap_session *s)
{
 if (e_dap_start_dialling(s))
  return SYNC_PHASE_DIAL;
 if (!e_dap_start_answered(s))
  return SYNC_PHASE_INIT;
 return SYNC_PHASE_LAUNCH;
}

/**
 * sync_resolved - Has the engine what the wait is for?  A start's wait ends
 * with its phase too, so the next phase is waited for with its own budget.
 */
static int sync_resolved(void)
{
 if (sl.mode == SYNC_ON_START)
  return sl.st != E_DAP_START_WAITING ||
         sync_start_phase(sl.s) != sl.phase;
 if (sl.mode == SYNC_ON_RUN || sl.mode == SYNC_ON_QUIT)
  return sl.st != E_DAP_RUN_GOING;
 return sl.call->done;
}

/**
 * sync_adapter_left - Has the adapter exited, with its last words still to
 * be read?
 *
 * The owner's reaper says so as soon as the child's end is reaped, before
 * the transport shows it: a grandchild holding the adapter's pipes open
 * keeps it from ever reaching its end.  What the adapter wrote before it
 * left is read once more, never while a reverse-TCP adapter has not dialled
 * in (accepting would wait).  Whatever the adapter then has not said, it
 * will not.
 * Return: 1 when it has left and that read has been made.
 */
static int sync_adapter_left(void)
{
 if (!e_dap_adapter_gone(sl.s))
  return 0;
 if (!sl.drained)
 {
  sl.drained = 1;
  if (sl.fd != SYNC_NO_FD &&
      !(sl.mode == SYNC_ON_START && e_dap_start_dialling(sl.s)))
   sync_on_transport(sl.fd, NULL);
 }
 return 1;
}

/**
 * sync_pred - The loop's question: is the wait over?  Asked once at the
 * start and after each callback the loop runs.
 *
 * Over when the engine has what it waited for, the end was requested, the
 * transport is no longer watched (it ended), or the adapter has gone.  Quit's
 * waits ignore the end request (the save ran in its callback), and the exit
 * wait ignores the transport too: it is over when the adapter is.
 */
static int sync_pred(void)
{
 if (sl.mode == SYNC_ON_EXIT)
  return e_dap_adapter_gone(sl.s);
 if ((!sync_quitting(sl.mode) && sync_end_pending()) ||
     sl.fd == SYNC_NO_FD || sync_resolved())
  return 1;
 if (!sync_adapter_left())
  return 0;
 return 1;
}

/**
 * sync_loop_begin - Put what a wait reads on the loop: the transport, the
 * program's pty and the end request.
 * @s: the session; @mode: SYNC_ON_*; @fd: the transport; @call: the request
 * waited for (SYNC_ON_CALL); @phase: the start's phase; @st: the pump's
 * answer so far.
 */
static void sync_loop_begin(e_dap_session *s, int mode, int fd,
                            const sync_call *call, int phase, int st)
{
 int pty = e_dap_output_fd(s);

 sl.s = s;
 sl.mode = mode;
 sl.call = call;
 sl.st = st;
 sl.phase = phase;
 sl.drained = 0;
 sl.fd = SYNC_NO_FD;
 sl.pty_fd = SYNC_NO_FD;
 sl.end_fd = SYNC_NO_FD;
 if (fd >= 0 && wpe_fd_add(fd, POLLIN, sync_on_transport, NULL) == 0)
  sl.fd = fd;
 if (pty >= 0 && wpe_fd_add(pty, POLLIN, sync_on_pty, NULL) == 0)
  sl.pty_fd = pty;
 /* Once handled the descriptor stays readable: Quit leaves it off the loop,
    which it would wake at once until the budget. */
 if (dap_end_fd != SYNC_NO_FD && !(sync_quitting(mode) && dap_end_handled) &&
     wpe_fd_add(dap_end_fd, POLLIN, sync_on_end_request, NULL) == 0)
  sl.end_fd = dap_end_fd;
}

/** sync_loop_end - Take what the wait watched off the loop. */
static void sync_loop_end(void)
{
 sync_unwatch(&sl.fd);
 sync_unwatch(&sl.pty_fd);
 sync_unwatch(&sl.end_fd);
}

/**
 * sync_wait_on_loop - Wait on the fd loop until the engine has what the wait
 * is for, the adapter has gone, the end was requested, or @budget_ms passes.
 * @s: the session; @mode, @fd, @call, @phase, @st: as sync_loop_begin;
 * @budget_ms: the longest wait, already scaled; @what: names the wait for the
 * loop and for the failure line.
 *
 * The owner's SIGCHLD reaper is in place first, so the adapter's end wakes
 * the loop at once, also when a grandchild holds its pipes open.  A wake the
 * loop could not serve (a key or display event no callback reads) asks the
 * predicate again on what is left of the budget.
 * Return: SYNC_ENDED, SYNC_BUDGET or SYNC_LOST.
 */
static int sync_wait_on_loop(e_dap_session *s, int mode, int fd,
                             const sync_call *call, int phase, int st,
                             long budget_ms, const char *what)
{
 long start = sync_now_ms(), left;
 int r;

 sync_last_hit_budget = 0;
 wpe_child_reap_install();
 sync_loop_begin(s, mode, fd, call, phase, st);
 for (;;)
 {
  left = budget_ms - (sync_now_ms() - start);
  if (left <= 0)
  {
   r = sync_pred() ? SYNC_ENDED : SYNC_BUDGET;
   break;
  }
  r = wpe_fd_wait_for(sync_pred, left, what);
  if (r == WPE_WAIT_INPUT)
   continue;
  if (r == WPE_WAIT_DONE)
   r = SYNC_ENDED;
  else if (sync_now_ms() - start < budget_ms)
   r = SYNC_LOST;
  else
   r = SYNC_BUDGET;
  break;
 }
 sync_loop_end();
 return r;
}

/**
 * sync_say_adapter - How the adapter stands, for a failure line.
 * @s: the session; @dst: receives " (adapter alive, pid N)", " (adapter
 * exited, status N)" and the like, or "" when no process is its own.
 */
static void sync_say_adapter(const e_dap_session *s, char *dst, size_t n)
{
 pid_t pid = e_dap_adapter_pid(s);
 int st = 0;

 dst[0] = '\0';
 if (e_dap_adapter_end_status(s, &st))
 {
  if (WIFEXITED(st))
   snprintf(dst, n, " (adapter exited, status %d)", WEXITSTATUS(st));
  else if (WIFSIGNALED(st))
   snprintf(dst, n, " (adapter killed by signal %d)", WTERMSIG(st));
  else
   snprintf(dst, n, " (adapter gone)");
 }
 else if (pid > 0)
  snprintf(dst, n, " (adapter alive, pid %ld)", (long)pid);
}

/** sync_exit_known - Predicate of sync_learn_adapter_exit. */
static int sync_exit_known(void)
{
 return e_dap_adapter_gone(sl.s);
}

/**
 * sync_learn_adapter_exit - A wait ended with no answer, not at its budget:
 * give the adapter a moment to be reaped, so the failure line can say how it
 * ended.
 * @s: the session.
 *
 * The end of its pipes can be seen a moment before its end is waitable.  A
 * wait on the loop, so the owner's reaper tells it; bounded by
 * SYNC_LEARN_EXIT_MS, scaled.
 */
static void sync_learn_adapter_exit(e_dap_session *s)
{
 if (e_dap_adapter_pid(s) <= 0 || e_dap_adapter_gone(s))
  return;
 sl.s = s;
 wpe_child_reap_install();
 wpe_fd_wait_for(sync_exit_known, sync_scaled(SYNC_LEARN_EXIT_MS),
                 "the adapter's end");
}

/**
 * sync_say_budget - A budget in words: "30 s" for whole seconds, else "250 ms".
 */
static void sync_say_budget(long ms, char *dst, size_t n)
{
 if (ms % SYNC_MS_PER_SEC == 0)
  snprintf(dst, n, "%ld s", ms / SYNC_MS_PER_SEC);
 else
  snprintf(dst, n, "%ld ms", ms);
}

/**
 * sync_note_wait - Say how a wait that did not get its answer ended, for
 * e_dap_sync_last_wait().
 * @s: the session; @what: what it waited for ("initialize"); @how:
 * SYNC_BUDGET, SYNC_LOST or SYNC_ENDED; @budget_ms: the budget it had.
 *
 * The text names the wait, then why (no answer within the budget, or the
 * adapter, the transport or the end request ended it), then how the adapter
 * stands, so a failure line tells a silent adapter from a dead one.
 */
static void sync_note_wait(e_dap_session *s, const char *what, int how,
                           long budget_ms)
{
 char who[SYNC_TEXT_MAX], span[SYNC_TEXT_MAX];

 if (how != SYNC_BUDGET)
  sync_learn_adapter_exit(s);
 sync_say_adapter(s, who, sizeof who);
 sync_say_budget(budget_ms, span, sizeof span);
 sync_last_hit_budget = (how == SYNC_BUDGET);
 if (how == SYNC_BUDGET)
  snprintf(sync_last, sizeof sync_last, "%s: no answer within %s%s", what,
           span, who);
 else if (sync_end_pending())
  snprintf(sync_last, sizeof sync_last, "%s: given up, the end was requested%s",
           what, who);
 else if (how == SYNC_LOST)
  snprintf(sync_last, sizeof sync_last, "%s: nothing on the loop to wait on%s",
           what, who);
 else
  snprintf(sync_last, sizeof sync_last, "%s: no answer%s", what, who);
}

/* ---- one request, waited for -------------------------------------------- */

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
 * @s: the session; @c: the request, sent with sync_call_answered; @what:
 * its command, for the failure line.
 *
 * Every other message that comes meanwhile goes to its owner: output to
 * the host, a setBreakpoints answer to on_breakpoints, a stop to the run.
 * The wait has the request budget: a request not answered within it is
 * ended E_D_REQ_TIMED_OUT, one whose adapter or transport ended, or whose
 * wait was given up (the end request), E_D_REQ_GONE, so the engine never
 * calls back into a request nobody waits for.
 * Return: 1 when the adapter answered, else 0.
 */
static int sync_call_wait(e_dap_session *s, sync_call *c, const char *what)
{
 long budget = sync_budget(E_DAP_SYNC_REQUEST);
 int fd = e_dap_fd(s), how = SYNC_ENDED;

 if (fd >= 0)
  how = sync_wait_on_loop(s, SYNC_ON_CALL, fd, c, 0, 0, budget, what);
 if (!c->done)
 {
  sync_note_wait(s, what, how, budget);
  e_dap_requests_cancel(s, how == SYNC_BUDGET ? E_D_REQ_TIMED_OUT
                                              : E_D_REQ_GONE);
 }
 return c->how == E_D_REQ_DONE;
}

/**
 * sync_call_run - Send the request a stage function built and wait for it.
 * @s: the session; @cmd: the staged command; @c: receives the answer;
 * @what: the command's name, for the failure line.
 * Return: 1 when the adapter answered, else 0.
 */
static int sync_call_run(e_dap_session *s, const char *cmd, sync_call *c,
                         const char *what)
{
 memset(c, 0, sizeof(*c));
 if (e_dap_req_request(s, cmd, sync_call_answered, c) != E_D_SEND_OK)
  return 0;
 return sync_call_wait(s, c, what);
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
 if (sync_call_run(s, cmd, &c, "evaluate"))
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
 if (sync_call_run(s, cmd, &c, "stackTrace"))
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
 if (sync_call_run(s, cmd, &c, "setBreakpoints"))
  ret = 0;
 free(c.ans.text);
 return ret;
}

/* ---- the run, waited for ------------------------------------------------ */

int e_dap_wait_stop(e_dap_session *s, long budget_ms)
{
 long budget = budget_ms > 0 ? sync_scaled(budget_ms)
                             : sync_budget(E_DAP_SYNC_RUN);
 int st, how;

 if (!s)
  return E_DAP_RUN_ENDED;
 st = e_dap_run_pump(s);
 if (st != E_DAP_RUN_GOING || e_dap_fd(s) < 0)
  return st;
 how = sync_wait_on_loop(s, SYNC_ON_RUN, e_dap_fd(s), NULL, 0, st, budget,
                         "run");
 if (sl.st == E_DAP_RUN_GOING)
  sync_note_wait(s, "run to a stop", how, budget);
 return sl.st;
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
 st = e_dap_wait_stop(s, 0);
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
 if (e_dap_wait_stop(s, 0) == E_DAP_RUN_GOING && sync_last_hit_budget)
  return -1;     /* no stop within the budget: e_dap_sync_last_wait says so */
 return 0;       /* ended is normal for `continue` past the last breakpoint */
}

/* ---- the start, waited for ---------------------------------------------- */

/**
 * sync_start_budget - The budget of the start's phase @phase, scaled, and
 * what it waits for.
 * @phase: SYNC_PHASE_*; @what: receives the name for the failure line.
 */
static long sync_start_budget(int phase, const char **what)
{
 if (phase == SYNC_PHASE_DIAL)
 {
  *what = "dial-in";
  return sync_budget(E_DAP_SYNC_ACCEPT);
 }
 if (phase == SYNC_PHASE_INIT)
 {
  *what = "initialize";
  return sync_budget(E_DAP_SYNC_START);
 }
 *what = "launch";
 return sync_budget(E_DAP_SYNC_LAUNCH);
}

/**
 * sync_start_drive - Run the start to its goal on the loop.
 * @s: the session.
 *
 * Each phase has its own budget: the adapter dialling in (dlv), the answer
 * to initialize (the editor's bound), then launch to the start's end (dlv
 * builds the package first).  A phase that ends with no answer, because the
 * budget passed, the adapter left or the end was requested, ends the start.
 * Return: E_DAP_START_READY or E_DAP_START_FAILED.
 */
static int sync_start_drive(e_dap_session *s)
{
 int st = e_dap_start_state(s);

 while (st == E_DAP_START_WAITING)
 {
  int phase = sync_start_phase(s), how;
  const char *what;
  long budget = sync_start_budget(phase, &what);

  if (e_dap_start_fd(s) < 0)
   return e_dap_start_give_up(s);
  how = sync_wait_on_loop(s, SYNC_ON_START, e_dap_start_fd(s), NULL, phase,
                          st, budget, what);
  st = sl.st;
  if (st == E_DAP_START_FAILED && !e_dap_start_error(s))
   sync_note_wait(s, what, how, budget);   /* it hung up, or never dialled */
  if (st != E_DAP_START_WAITING)
   break;
  if (sync_start_phase(s) != phase)
   continue;
  sync_note_wait(s, what, how, budget);
  return e_dap_start_give_up(s);
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
 wpe_child_reap_install();
 return sync_open_finish(e_dap_start(argv, program, entry_func, cwd, host));
}

e_dap_session *e_dap_open_stdio(char *const argv[], const char *program,
                                const char *entry_func, const char *cwd,
                                const e_dap_host *host)
{
 wpe_child_reap_install();
 return sync_open_finish(e_dap_start_stdio(argv, program, entry_func, cwd,
                                           host));
}

e_dap_session *e_dap_open_tcp(const char *host, int port, const char *program,
                              const char *entry_func, const e_dap_host *host_cb)
{
 wpe_child_reap_install();
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
 * wait, on the loop, ends at the adapter's answer or the end of its
 * transport, or at E_DAP_DISCONNECT_MS (not scaled: the editor's own bound).
 * The end request does not cut it short (see SYNC_ON_QUIT).
 */
static void sync_quit_and_wait(e_dap_session *s)
{
 int st;

 if (e_dap_disconnect_sent(s))
  return;                        /* the caller disconnected and waited */
 if (e_dap_disconnect(s) != 0)
  return;
 st = e_dap_run_pump(s);
 if (st == E_DAP_RUN_GOING && e_dap_fd(s) >= 0)
  sync_wait_on_loop(s, SYNC_ON_QUIT, e_dap_fd(s), NULL, 0, st,
                    E_DAP_DISCONNECT_MS, "the adapter's answer to disconnect");
 sync_quit_end_check(0);
}

/* The slack the exit wait has beyond the ladder's own bounds: the ladder
   reports its end by itself, this is only so a lost report cannot hang. */
#define SYNC_EXIT_SLACK_MS 1000

/**
 * sync_reap_adapter - Make sure the adapter is gone, on the owner's ladder.
 * @s: the session.
 *
 * Skipped for a session with no adapter of its own (a loopback or TCP peer:
 * e_dap_adapter_pid is 0) and for one already gone.  Otherwise its input
 * closes (some adapters exit on that), then e_dap_end_adapter hands it to the
 * editor's ladder: one that answered `disconnect` gets WPE_CHILD_END_GRACE_MS
 * to exit by itself and then SIGKILL, one that did not gets SIGTERM, the
 * grace and SIGKILL.  The wait is on the loop and ends when the owner's
 * reaper says the adapter is gone, also when a grandchild holds its pipes;
 * what the adapter writes meanwhile is read and dropped.  Bounded by the
 * ladder's own constants (not scaled) and a slack, so Quit takes at most
 * E_DAP_CLOSE_MAX_MS plus that.
 */
static void sync_reap_adapter(e_dap_session *s)
{
 if (e_dap_adapter_pid(s) <= 0 || e_dap_adapter_gone(s))
  return;
 e_dap_hang_up(s);
 e_dap_end_adapter(s, e_dap_disconnect_answered(s) ? WPE_CHILD_NO_SIGNAL
                                                   : SIGTERM);
 sync_wait_on_loop(s, SYNC_ON_EXIT, e_dap_fd(s), NULL, 0, 0,
                   WPE_CHILD_END_GRACE_MS + WPE_CHILD_END_KILL_MS +
                   SYNC_EXIT_SLACK_MS, "the adapter's end");
 sync_quit_end_check(0);
}

void e_dap_close(e_dap_session *s)
{
 if (!s)
  return;
 sync_quit_and_wait(s);
 sync_reap_adapter(s);
 e_dap_free(s);
}
