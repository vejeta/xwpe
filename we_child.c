/* we_child.c -- the editor's children: started, watched and ended in one place.
 *
 * See we_child.h.  The SIGCHLD handler (WpeSignalChild, we_unix.c) reaps
 * every child with waitpid(-1, WNOHANG) and hands each one to
 * wpe_child_note_ended: a watched child's wait status goes to its own slot
 * and wakes the fd loop through a pipe.  Every child the editor starts is
 * watched from before its fork, so no status is ever kept anywhere else.
 * The loop then calls each ended child's owner back, between the user's
 * keys.
 *
 * pid, ended and status of a slot are written by the handler or with
 * SIGCHLD held; the rest only from the main loop.  The handler does this
 * work only on the editor's own thread (a library thread hands the signal
 * over), so it interrupts the very thread that reads these fields: the reads
 * see its stores in program order, on any CPU, with no fence.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#define WPE_PLATFORM_WANTS_OPENPTY   /* openpty: the header differs per system */
#include "we_platform.h"

#include "we_child.h"
#include "we_childsig.h"
#include "we_path.h"
#include "we_fdloop.h"

#define WPE_CHILD_EXEC_FAILED 1          /* a command that could not run */
#define WPE_CHILD_SAID_FAILED 127        /* one that said why, as a shell's */
#define WPE_CHILD_TTY_PATH_SIZE   WPE_PATH_SIZE  /* a terminal's path */
/* The first half of every sentence a child says about itself: what could not
   be done.  The reason follows, then a line end; the parent prepares the
   heads (WpeChildSaid) and the child only copies them. */
#define WPE_CHILD_TTY_FAILED_PRE   "cannot give "
#define WPE_CHILD_TTY_FAILED_POST  " a terminal: "
#define WPE_CHILD_TTY_FAILED_TEXT \
 WPE_CHILD_TTY_FAILED_PRE "%s" WPE_CHILD_TTY_FAILED_POST "%s"
#define WPE_CHILD_TTY_WITHOUT_PRE  ""
#define WPE_CHILD_TTY_WITHOUT_POST " runs without a controlling terminal: "
#define WPE_CHILD_DIR_FAILED_PRE   "cannot enter "
#define WPE_CHILD_DIR_FAILED_POST  ": "
#define WPE_CHILD_RUN_FAILED_PRE   "cannot run "
#define WPE_CHILD_RUN_FAILED_POST  ": "
#define WPE_CHILD_SAID_END         "\r\n"
#define WPE_CHILD_REASON_FALLBACK  "error "   /* an errno with no prepared text */
#define WPE_CHILD_LINE_MAX    128        /* a line for the status row */
#define WPE_CHILD_NOT_STARTED_FMT "%s not started: too many programs running"
#define WPE_CHILD_DID_NOT_END_FMT "%s did not end"
#define WPE_CHILD_UNNAMED         "a program"   /* a spec that named nothing */
#define WPE_CHILD_PIPE_RD 0
#define WPE_CHILD_PIPE_WR 1
/* The child's own descriptor 0, set up between fork and exec; the editor's
   keyboard is never read here. */
#define WPE_CHILD_FD_IN   0

typedef struct {
 volatile sig_atomic_t pid;      /* the child, or 0 while the slot is free */
 volatile sig_atomic_t ended;    /* the handler reaped it */
 volatile sig_atomic_t status;   /* its wait status, once ended */
 volatile sig_atomic_t group;    /* it leads a group the handlers may end */
 WpeChildEndCallback callback;   /* a pid-based watch (WpeChildWatch) */
 void *data;
 WpeChild *child;                /* or a child of wpe_child_start */
 WpeChildEndedFn child_ended;
 void *child_data;
 const char *what;
 int report_rd;                  /* the editor's end of the child's report pipe
                                    (main loop only), or WPE_CHILD_NO_FD:
                                    read once, when its end is delivered, and
                                    closed with the slot */
} WpeChildSlot;

static WpeChildSlot wpe_child_table[WPE_CHILD_MAX];

/* A group whose leader ended while the ladder waited out its grace: the
   members that ignored the polite signal still get SIGKILL when the grace
   is over.  The number cannot name another group meanwhile: a PID is not
   given out again while a group of that number has members. */
typedef struct {
 int pgid;                       /* 0 while the record is free; with @alone,
                                    the one process, not a group */
 long due_ms;                    /* when its grace is over, or, with @alone,
                                    when it is next looked at */
 int alone;                      /* a process a child named, not the editor's
                                    (wpe_child_end_named): signalled by its
                                    number only, never its group */
 long kill_ms;                   /* @alone: when its grace is over */
} WpeChildLinger;

static WpeChildLinger wpe_child_lingers[WPE_CHILD_MAX];

/* Children whose owner has gone (a language server's session, freed when the
   user switched files): the owner keeps them here while their end runs, so
   the slot's pointer never outlives what it points to.  A record is free
   while its pid is 0. */
static WpeChild wpe_child_detached[WPE_CHILD_MAX];

/* The self-pipe: the handler writes one byte when a watched child ends; the
   fd loop watches the read end while any slot is taken. */
#define WPE_CHILD_WAKE_DRAIN 64
static int wpe_child_wake_rd = WPE_CHILD_NO_FD;
static int wpe_child_wake_wr = WPE_CHILD_NO_FD;

static void (*wpe_child_say_hook)(const char *line);

/**
 * wpe_child_say_set - Where the owner's lines go (the status row).
 * @say: the editor's status line, or NULL (the C tests).
 */
void wpe_child_say_set(void (*say)(const char *line))
{
 wpe_child_say_hook = say;
}

static void (*wpe_child_trace_hook)(const char *line);

/* The trace line of a user start that came from no start plan. */
#define WPE_CHILD_UNPLANNED_FMT "child unplanned %s"

/**
 * wpe_child_trace_set - Where the owner's trace lines go.
 * @trace: the editor's trace, or NULL (the C tests).
 *
 * Lets a test see which starts do not come from a start plan yet, while
 * the sites are converted one pass at a time.
 */
void wpe_child_trace_set(void (*trace)(const char *line))
{
 wpe_child_trace_hook = trace;
}

/**
 * wpe_child_unplanned - Is this a start the editor must refuse, because it
 * did not come from a start plan?
 * @spec: the start.
 *
 * Every program the editor starts for the user comes from a start plan
 * (we_startplan.c), which shows and asks about what a folder decided; a
 * start without one would be a program whose command line nobody judged,
 * so it is refused, said on the status row and traced.  The assistant's
 * children are held by their own control instead and say so in @origin.
 * The owner's own unit test, which exercises the mechanics with hand-built
 * specs, is compiled with WPE_CHILD_MECHANICS_TEST and is the only build in
 * which such a start runs.
 * Return: 1 when it must be refused.
 */
static int wpe_child_unplanned(const WpeChildSpec *spec)
{
 char line[WPE_CHILD_LINE_MAX];
 const char *what = spec->what;

 if (spec->plan || spec->origin == WPE_CHILD_ORIGIN_AI)
  return(0);
 if (!what)
  what = WPE_CHILD_UNNAMED;
 if (wpe_child_trace_hook)
 {
  snprintf(line, sizeof line, WPE_CHILD_UNPLANNED_FMT, what);
  wpe_child_trace_hook(line);
 }
#ifdef WPE_CHILD_MECHANICS_TEST
 return(0);
#else
 if (wpe_child_say_hook)
  wpe_child_say_hook(WPE_CHILD_REFUSE_UNPLANNED_TEXT);
 return(1);
#endif
}

/**
 * wpe_child_say - Tell the user something about a child, naming it.
 * @fmt: a format with one %s for what the child is; @what: that.
 */
static void wpe_child_say(const char *fmt, const char *what)
{
 char line[WPE_CHILD_LINE_MAX];

 if (!wpe_child_say_hook)
  return;
 if (!what)
  what = WPE_CHILD_UNNAMED;
 snprintf(line, sizeof line, fmt, what);
 wpe_child_say_hook(line);
}

/**
 * wpe_child_say_reason - Tell the user why a child could not be started,
 * naming it.
 * @fmt: a format with two %s, what the child is and the reason; @what: that;
 * @err: the errno of the refusal.
 *
 * errno is @err when it returns: the editor's hook draws, and the caller
 * hands the refusal's errno on to its own caller.
 */
static void wpe_child_say_reason(const char *fmt, const char *what, int err)
{
 char line[WPE_CHILD_LINE_MAX];

 if (wpe_child_say_hook)
 {
  snprintf(line, sizeof line, fmt, what ? what : WPE_CHILD_UNNAMED,
           strerror(err));
  wpe_child_say_hook(line);
 }
 errno = err;
}

/**
 * wpe_child_fd_private - Make a descriptor of the editor's non-blocking and
 * closed on exec.
 * @fd: the descriptor.
 *
 * No program the editor starts inherits it, and no read of it ever blocks
 * the editor.
 * Return: 0, or -1 when fcntl refused.
 */
int wpe_child_fd_private(int fd)
{
 int fl = fcntl(fd, F_GETFL, 0);

 if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
  return(-1);
 if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
  return(-1);
 return(0);
}

/**
 * wpe_child_sigchld_install - Take SIGCHLD with @handler, which reaps.
 * @handler: the signal handler.
 *
 * The flags are the point: SA_RESTART, so a read the editor makes on a pipe
 * or a file is not cut short by a child's end where the system would
 * otherwise fail it with EINTR (every system, for a read that waits), and
 * SA_NOCLDSTOP, so only an end is reported.  The handler only reaps and
 * writes the wake pipe; poll() is never restarted, on any system, and the
 * wake pipe tells it.
 * Return: 0, or -1 when the system refused.
 */
int wpe_child_sigchld_install(void (*handler)(int))
{
 struct sigaction act;

 memset(&act, 0, sizeof act);
 act.sa_handler = handler;
 sigemptyset(&act.sa_mask);
 act.sa_flags = SA_NOCLDSTOP | SA_RESTART;
 return(sigaction(SIGCHLD, &act, NULL));
}

/**
 * WpeChildWakeOpen - Make the pipe that wakes the editor when a watched
 * child ends.
 *
 * Called once at start-up, before the SIGCHLD handler is installed.  When
 * no pipe can be made, every watch is refused and its callers do without,
 * so the editor still starts.
 */
void WpeChildWakeOpen(void)
{
 int fds[2];

 if (wpe_child_wake_rd != WPE_CHILD_NO_FD || pipe(fds) < 0)
  return;
 if (wpe_child_fd_private(fds[0]) < 0 || wpe_child_fd_private(fds[1]) < 0)
 {
  close(fds[0]);
  close(fds[1]);
  return;
 }
 wpe_child_wake_rd = fds[0];
 wpe_child_wake_wr = fds[1];
}

/**
 * wpe_child_wake - In the handler: wake the fd loop, a watched child ended.
 *
 * One write() to a non-blocking pipe; when it fails the pipe is full, so a
 * wake-up is waiting in it already.
 */
static void wpe_child_wake(void)
{
 static const char wake = 1;

 if (wpe_child_wake_wr == WPE_CHILD_NO_FD)
  return;
 if (write(wpe_child_wake_wr, &wake, 1) < 0)
  return;
}

/**
 * wpe_child_watch_note - In the handler: keep a watched child's status.
 * @pid: a child just reaped; @status: its wait status.
 * Return: 1 when @pid was watched.
 */
static int wpe_child_watch_note(pid_t pid, int status)
{
 int i;

 for (i = 0; i < WPE_CHILD_MAX; i++)
 {
  if (wpe_child_table[i].pid != pid || wpe_child_table[i].ended)
   continue;
  wpe_child_table[i].status = status;
  wpe_child_table[i].ended = 1;
  wpe_child_wake();
  return(1);
 }
 return(0);
}

/**
 * wpe_child_note_ended - The SIGCHLD handler reaped @pid.
 * @pid: the child; @status: its wait status.
 *
 * Async-signal-safe: stores into sig_atomic_t fields and one write().  A
 * process nobody watches (none the editor starts) needs nothing kept.
 */
void wpe_child_note_ended(pid_t pid, int status)
{
 wpe_child_watch_note(pid, status);
}

/**
 * wpe_child_reap_now - Reap every child that has ended and note each one.
 *
 * The body of the editor's SIGCHLD handler (WpeSignalChild): waitpid with
 * WNOHANG until nothing more has ended, each status handed to
 * wpe_child_note_ended, so a watched child's owner is called back from the
 * loop and a child nobody watches is simply not left a zombie.
 * Async-signal-safe; errno is kept for the code the signal interrupted.
 */
void wpe_child_reap_now(void)
{
 int status, saved_errno = errno;
 pid_t pid;

 while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
  wpe_child_note_ended(pid, status);
 errno = saved_errno;
}

/**
 * wpe_child_reap_owned - Reap only the children the owner follows.
 *
 * A program with no editor around it (a sync layer, a C test) also forks
 * processes it waits for itself, and a reaper that took every status would
 * leave that wait with ECHILD.  This one asks for each followed child by
 * its number, so nothing else is touched.  Async-signal-safe.
 */
static void wpe_child_reap_owned(void)
{
 int i, status;

 for (i = 0; i < WPE_CHILD_MAX; i++)
 {
  pid_t pid = wpe_child_table[i].pid;

  if (pid <= 0 || wpe_child_table[i].ended)
   continue;
  if (waitpid(pid, &status, WNOHANG) == pid)
   wpe_child_note_ended(pid, status);
 }
}

/**
 * wpe_child_on_sigchld - SIGCHLD for a program that runs the loop without
 * the editor: reap the followed children and wake the loop.
 * @sig: SIGCHLD.
 */
static void wpe_child_on_sigchld(int sig)
{
 int saved_errno = errno;

 (void)sig;
 wpe_child_reap_owned();
 errno = saved_errno;
}

/**
 * wpe_child_reap_install - Make the death of a followed child end a wait on
 * the loop, in a program that has no editor around it.
 *
 * What a sync layer or a C test calls before it starts a child: the wake
 * pipe is made and SIGCHLD is taken with the owner's reaper, so a child's end
 * reaches wpe_fd_wait_for at once, also when a grandchild still holds the
 * child's pipes open, instead of at the wait's deadline.  Calling it again
 * changes nothing, and a SIGCHLD handler somebody else installed (the
 * editor's) is left alone.  A followed child that has already ended is
 * reaped and reported by the time it returns.
 * Return: 0, or -1 when the system refused.
 */
int wpe_child_reap_install(void)
{
 struct sigaction old;

 WpeChildWakeOpen();
 if (sigaction(SIGCHLD, NULL, &old) != 0)
  return(-1);
 /* Any handler that is not the default or ignore is somebody else's (or
    ours from an earlier call), a three-argument one included. */
 if ((old.sa_flags & SA_SIGINFO) ||
     (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN))
  return(0);
 if (wpe_child_sigchld_install(wpe_child_on_sigchld) != 0)
  return(-1);
 /* A child that ended before the handler was there left no signal behind. */
 wpe_child_reap_owned();
 return(0);
}

/**
 * wpe_child_end_from_handler - End every child the editor follows, at once.
 *
 * Called by a crash or end signal's handler, and by the editor's own end
 * once the children that can say goodbye have had their time: SIGKILL to
 * each one still running, and to the group it leads, so no build, debugger,
 * server or program the editor started is left behind it.  A child whose
 * signals reach it alone (the build server's launcher) keeps its group.
 * Async-signal-safe: reads sig_atomic_t fields and calls kill() only.
 */
void wpe_child_end_from_handler(void)
{
 int i;

 for (i = 0; i < WPE_CHILD_MAX; i++)
 {
  if (wpe_child_lingers[i].pgid > 0 && wpe_child_lingers[i].alone)
   kill(wpe_child_lingers[i].pgid, SIGKILL);
  else if (wpe_child_lingers[i].pgid > 0)
   kill(-wpe_child_lingers[i].pgid, SIGKILL);
  if (wpe_child_table[i].pid <= 0 || wpe_child_table[i].ended)
   continue;
  if (wpe_child_table[i].group)
   kill(-wpe_child_table[i].pid, SIGKILL);
  kill(wpe_child_table[i].pid, SIGKILL);
 }
}

/**
 * WpeChildSignalBlock - Hold SIGCHLD back until WpeChildSignalRestore.
 * @saved: receives the mask to restore.
 *
 * Wrapped around a fork and the watch of its child, so the child cannot be
 * reaped before it is watched: an end that comes first waits in the kernel
 * and reaches the child's slot when the mask is restored.  The mask is the
 * calling thread's: a library thread that takes the signal meanwhile hands
 * it to this thread, where it waits for WpeChildSignalRestore.
 */
void WpeChildSignalBlock(sigset_t *saved)
{
 sigset_t chld;

 sigemptyset(&chld);
 sigaddset(&chld, SIGCHLD);
 pthread_sigmask(SIG_BLOCK, &chld, saved);
}

/**
 * WpeChildSignalRestore - Let SIGCHLD through again.
 * @saved: what WpeChildSignalBlock kept.
 */
void WpeChildSignalRestore(const sigset_t *saved)
{
 pthread_sigmask(SIG_SETMASK, saved, NULL);
}

/**
 * wpe_child_signal_was_held - Did the caller already hold SIGCHLD?
 * @saved: the mask WpeChildSignalBlock kept, i.e. the caller's.
 *
 * A child forked while SIGCHLD was held cannot have been reaped yet, so its
 * end is sure to be seen; one forked without it may have been reaped
 * already, and nothing could tell its end from an earlier process's.
 */
static int wpe_child_signal_was_held(const sigset_t *saved)
{
 return(sigismember(saved, SIGCHLD) == 1);
}

/** wpe_child_find - The slot of @pid, or NULL. */
static WpeChildSlot *wpe_child_find(int pid)
{
 int i;

 if (pid <= 0)
  return(NULL);
 for (i = 0; i < WPE_CHILD_MAX; i++)
  if (wpe_child_table[i].pid == pid)
   return(&wpe_child_table[i]);
 return(NULL);
}

/** wpe_child_free_slot - A slot no child uses, or NULL. */
static WpeChildSlot *wpe_child_free_slot(void)
{
 int i;

 for (i = 0; i < WPE_CHILD_MAX; i++)
  if (!wpe_child_table[i].pid)
   return(&wpe_child_table[i]);
 return(NULL);
}

/** wpe_child_any - Does any slot still follow a child? */
static int wpe_child_any(void)
{
 int i;

 for (i = 0; i < WPE_CHILD_MAX; i++)
  if (wpe_child_table[i].pid)
   return(1);
 return(0);
}

/**
 * wpe_child_slot_report_close - Close the report pipe a slot in use holds.
 * @w: a slot that is someone's (never one that was never taken: its field
 * is still zero, a valid descriptor number).
 */
static void wpe_child_slot_report_close(WpeChildSlot *w)
{
 if (w->report_rd != WPE_CHILD_NO_FD)
  close(w->report_rd);
 w->report_rd = WPE_CHILD_NO_FD;
}

/**
 * wpe_child_slot_free - Empty a slot.  pid goes last: the handler matches
 * on it alone, so a slot is either someone's with its fields set, or free.
 */
static void wpe_child_slot_free(WpeChildSlot *w)
{
 w->callback = NULL;
 w->data = NULL;
 w->child = NULL;
 w->child_ended = NULL;
 w->child_data = NULL;
 w->what = NULL;
 w->group = 0;
 wpe_child_slot_report_close(w);
 w->pid = 0;
}

/**
 * wpe_child_wake_off_when_idle - Take the wake pipe off the loop once no
 * child is watched, so an idle editor sleeps until a key.  SIGCHLD held.
 */
static void wpe_child_wake_off_when_idle(void)
{
 if (!wpe_child_any())
  wpe_fd_del(wpe_child_wake_rd);
}

/** wpe_child_wake_drain - Empty the wake pipe: one pass serves every end. */
static void wpe_child_wake_drain(void)
{
 char buf[WPE_CHILD_WAKE_DRAIN];

 while (read(wpe_child_wake_rd, buf, sizeof buf) > 0)
  ;
}

/* The ladder's one timer, for every child being ended and every group whose
   leader went first: set for the earliest step due, so any number of ends
   at once take one entry of the loop's timer table. */
static int wpe_child_ladder_timer = WPE_TIMER_NONE;

static void wpe_child_ladder_rearm(void);

/**
 * wpe_child_stop_ladder - A child that has gone, or is forgotten, needs no
 * further step of its end.
 * @c: the child.
 */
static void wpe_child_stop_ladder(WpeChild *c)
{
 c->end_stage = WPE_CHILD_END_NONE;
 wpe_child_ladder_rearm();
}

/** wpe_child_now_ms - The monotonic clock, in milliseconds. */
static long wpe_child_now_ms(void)
{
 struct timespec ts;

 clock_gettime(CLOCK_MONOTONIC, &ts);
 return((long)ts.tv_sec * WPE_TIMER_MS_PER_S + ts.tv_nsec / WPE_TIMER_NS_PER_MS);
}

/**
 * wpe_child_linger_kill - The grace of a group whose leader went first is
 * over: SIGKILL to what is left of it.
 * @l: the record, free afterwards.
 */
static void wpe_child_named_step(WpeChildLinger *l);

static void wpe_child_linger_kill(WpeChildLinger *l)
{
 if (l->alone)
 {
  wpe_child_named_step(l);
  return;
 }
 kill(-l->pgid, SIGKILL);
 l->pgid = 0;
}

/**
 * wpe_child_named_step - Look at a process a child named, whose end the
 * editor follows: gone, it needs nothing more; past its grace, SIGKILL;
 * else it is looked at again shortly.
 * @l: its record, free once the process is done with.
 *
 * Looking every WPE_CHILD_NAMED_POLL_MS rather than once at the grace lets
 * the editor's own end go the moment the program has ended.
 */
static void wpe_child_named_step(WpeChildLinger *l)
{
 long now = wpe_child_now_ms();

 if (kill(l->pgid, 0) != 0)
 {
  memset(l, 0, sizeof(*l));
  return;
 }
 if (now >= l->kill_ms)
 {
  kill(l->pgid, SIGKILL);
  memset(l, 0, sizeof(*l));
  return;
 }
 l->due_ms = now + WPE_CHILD_NAMED_POLL_MS;
 if (l->due_ms > l->kill_ms)
  l->due_ms = l->kill_ms;
}

/**
 * wpe_child_group_has_members - Is anyone left in the group @pgid?
 *
 * Its leader is reaped, so only members can answer; the null signal sends
 * nothing.
 */
static int wpe_child_group_has_members(int pgid)
{
 return(kill(-pgid, 0) == 0);
}

/** wpe_child_linger_free - A free record, or NULL. */
static WpeChildLinger *wpe_child_linger_free(void)
{
 int i;

 for (i = 0; i < WPE_CHILD_MAX; i++)
  if (!wpe_child_lingers[i].pgid)
   return(&wpe_child_lingers[i]);
 return(NULL);
}

/**
 * wpe_child_linger - The leader of @c ended while its ladder waited out the
 * grace: keep the SIGKILL for the rest of its group.
 * @c: the child, its pid still set.
 *
 * Esc on a build whose make ends at SIGTERM while a compiler it started
 * ignores it: the compiler, which holds the build's output open, is ended at
 * the grace, as when make itself had ignored it, and the build ends.  A
 * group with nobody left needs nothing; with no record left, or the grace
 * already over, the rest of the group is killed now.
 */
static void wpe_child_linger(const WpeChild *c)
{
 WpeChildLinger *l;

 if (!wpe_child_group_has_members(c->pid))
  return;
 l = wpe_child_linger_free();
 if (!l || c->end_due_ms <= wpe_child_now_ms())
 {
  kill(-c->pid, SIGKILL);
  return;
 }
 l->pgid = c->pid;
 l->due_ms = c->end_due_ms;
}

/**
 * wpe_child_ladder_outlives_leader - Does @c's group still owe a SIGKILL?
 * @c: a child just reported ended.
 * Return: 1 while the polite step's grace was running.
 */
static int wpe_child_ladder_outlives_leader(const WpeChild *c)
{
 return(c->leads_group && c->end_stage == WPE_CHILD_END_POLITE);
}

/* ---- what a child says about a failed start ------------------------------ */

/* Where a start stopped, as the child tells the editor: one record of two
   integers on the report pipe, written by the child before it ends or goes on
   without what failed.  A start that reached the exec leaves no record: the
   exec closes the pipe, and the editor reads the end of file as "it ran". */
#define WPE_CHILD_STAGE_EDITOR_GONE 1   /* die_with_editor: the editor was gone */
#define WPE_CHILD_STAGE_TTY_OPEN    2   /* the terminal named by path would not open */
#define WPE_CHILD_STAGE_TTY_REFUSED 3   /* the terminal was not given: not run */
#define WPE_CHILD_STAGE_TTY_WITHOUT 4   /* the same, and the child goes on without */
#define WPE_CHILD_STAGE_OUT_FILE    5   /* the file for its output would not open */
#define WPE_CHILD_STAGE_DIR         6   /* its working folder could not be entered */
#define WPE_CHILD_STAGE_EXEC        7   /* nothing ran: the exec failed */
#define WPE_CHILD_REPORT_FMT "child report %s %s %d"
#define WPE_CHILD_RAN_FMT    "child ran %s"

typedef struct {
 int stage;                      /* WPE_CHILD_STAGE_* */
 int err;                        /* errno at that step */
} WpeChildReport;

/** wpe_child_stage_name - A stage as one word of the trace. */
static const char *wpe_child_stage_name(int stage)
{
 static const char *const names[] = { "none", "editor-gone", "tty-open", "tty",
                                      "tty-without", "out-file", "dir", "exec" };

 if (stage < 0 || stage >= (int)(sizeof names / sizeof names[0]))
  return("unknown");
 return(names[stage]);
}

/**
 * wpe_child_report_trace - Trace one thing a child reported, for a test or a
 * user who asks why a program never started.
 * @what: the child; @r: its record, or NULL for "it ran".
 */
static void wpe_child_report_trace(const char *what, const WpeChildReport *r)
{
 char line[WPE_CHILD_LINE_MAX];

 if (!wpe_child_trace_hook)
  return;
 if (r)
  snprintf(line, sizeof line, WPE_CHILD_REPORT_FMT, what,
           wpe_child_stage_name(r->stage), r->err);
 else
  snprintf(line, sizeof line, WPE_CHILD_RAN_FMT, what);
 wpe_child_trace_hook(line);
}

/**
 * wpe_child_report_serve - Read what the ended child of @w told the editor
 * about its start, and trace it.
 * @w: a slot whose child the handler reaped.
 *
 * The child is dead, so its end of the pipe is closed and a non-blocking
 * read answers at once: each record, then the end of file.  Nothing waits
 * and nothing is registered on the loop.  A record is a stage and an errno:
 * the sentence the user reads is the child's own, written where the program's
 * output goes.  An end of file with no record that stopped the child is "it
 * ran".  A read that finds the pipe empty but open (a stray copy of the write
 * end) reports nothing.
 */
static void wpe_child_report_serve(const WpeChildSlot *w)
{
 const char *what = w->what ? w->what : WPE_CHILD_UNNAMED;
 WpeChildReport rec;
 int stopped = 0;
 ssize_t n;

 if (w->report_rd == WPE_CHILD_NO_FD)
  return;
 for (;;)
 {
  n = read(w->report_rd, &rec, sizeof rec);
  if (n < 0 && errno == EINTR)
   continue;
  if (n != (ssize_t)sizeof rec)
   break;
  wpe_child_report_trace(what, &rec);
  if (rec.stage != WPE_CHILD_STAGE_TTY_WITHOUT)
   stopped = 1;
 }
 if (n == 0 && !stopped)
  wpe_child_report_trace(what, NULL);
}

/**
 * wpe_child_deliver - Call back the owner of an ended child.
 * @w: a slot whose child the handler reaped.
 *
 * The slot is freed before the callback runs, so the callback may start
 * another child, or open a dialog whose own loop polls again, and find the
 * table consistent.  What the child reported about its start is read first,
 * while the slot still holds the pipe.
 */
static void wpe_child_deliver(WpeChildSlot *w)
{
 WpeChildEndCallback cb = w->callback;
 void *data = w->data;
 WpeChild *c = w->child;
 WpeChildEndedFn ended = w->child_ended;
 void *child_data = w->child_data;
 int pid = w->pid, status = w->status;

 wpe_child_report_serve(w);
 wpe_child_slot_free(w);
 if (c)
 {
  if (wpe_child_ladder_outlives_leader(c))
   wpe_child_linger(c);
  wpe_child_stop_ladder(c);
  c->pid = 0;
  if (ended)
   ended(c, status, WPE_CHILD_ENDED, child_data);
  return;
 }
 if (cb)
  cb(pid, status, data);
}

/**
 * wpe_child_on_wake - The fd loop's callback for the wake pipe.
 *
 * Runs between the user's keys: each watched child that ended is reported
 * to its owner, which can then tell the user how the program ended.
 * The pipe is emptied first and each slot is freed before its report, so
 * a report that waits on the loop for another child's end (Run after its
 * build, a box the user answers) gets that end: the loop watches the pipe
 * again meanwhile, and a second pass over the table finds it consistent.
 */
static void wpe_child_on_wake(int fd, void *data)
{
 sigset_t saved;
 int i;

 (void)data;
 wpe_child_wake_drain();
 wpe_fd_release(fd);
 for (i = 0; i < WPE_CHILD_MAX; i++)
 {
  if (!wpe_child_table[i].pid || !wpe_child_table[i].ended)
   continue;
  wpe_child_deliver(&wpe_child_table[i]);
 }
 WpeChildSignalBlock(&saved);
 wpe_child_wake_off_when_idle();
 WpeChildSignalRestore(&saved);
}

/**
 * wpe_child_slot_take - Give a free slot to the child @pid.  SIGCHLD held
 * since its fork.
 * Return: the slot, or NULL when the table is full or the loop cannot watch
 * the wake pipe.
 */
static WpeChildSlot *wpe_child_slot_take(int pid)
{
 WpeChildSlot *w = wpe_child_find(pid);
 const int reused = (w != NULL);   /* a number given out again before its report */

 if (!w)
  w = wpe_child_free_slot();
 if (!w || wpe_child_wake_rd == WPE_CHILD_NO_FD)
  return(NULL);
 if (wpe_fd_add(wpe_child_wake_rd, POLLIN, wpe_child_on_wake, NULL) != 0)
  return(NULL);
 if (reused)
  wpe_child_slot_report_close(w);
 w->ended = 0;
 w->status = 0;
 w->group = 0;
 w->callback = NULL;
 w->data = NULL;
 w->child = NULL;
 w->child_ended = NULL;
 w->child_data = NULL;
 w->what = NULL;
 w->report_rd = WPE_CHILD_NO_FD;
 return(w);
}

/**
 * WpeChildWatch - Call @cb from the editor's loop when the child @pid ends.
 * @pid: a child of the editor; @cb: gets its PID and wait status;
 * @data: handed to @cb.
 *
 * For the children not started through wpe_child_start yet.  Must be called
 * between WpeChildSignalBlock and WpeChildSignalRestore around the fork.
 * Return: 0, or -1 when SIGCHLD was not held since the fork, no wake pipe
 * exists, every slot is taken or the fd loop is full.
 */
int WpeChildWatch(int pid, WpeChildEndCallback cb, void *data)
{
 WpeChildSlot *w;
 sigset_t saved;
 int ret = -1;

 if (pid <= 0)
  return(-1);
 WpeChildSignalBlock(&saved);
 if (wpe_child_signal_was_held(&saved))
 {
  w = wpe_child_slot_take(pid);
  if (w)
  {
   w->callback = cb;
   w->data = data;
   w->pid = pid;
   ret = 0;
  }
 }
 WpeChildSignalRestore(&saved);
 return(ret);
}

/**
 * WpeChildUnwatch - Stop waiting for the child @pid; its callback never
 * comes.  A child that ends later is reaped by the handler all the same.
 * @pid: a watched child; anything else is ignored.
 */
void WpeChildUnwatch(int pid)
{
 WpeChildSlot *w;
 sigset_t saved;

 WpeChildSignalBlock(&saved);
 w = wpe_child_find(pid);
 if (w)
 {
  wpe_child_slot_free(w);
  wpe_child_wake_off_when_idle();
 }
 WpeChildSignalRestore(&saved);
}

/**
 * WpeChildEnded - Has the child @pid ended, and how?  Never waits.
 * @pid: a child of the editor; @status: receives its wait status.
 *
 * A watched child is answered from its slot, and its callback still comes.
 * Nothing is asked of the system: the SIGCHLD handler is the one reaper.
 * Return: WPE_CHILD_RUNS, WPE_CHILD_ENDED with *status set, or
 * WPE_CHILD_ENDED_UNKNOWN when @pid is followed no more.
 */
int WpeChildEnded(int pid, int *status)
{
 WpeChildSlot *w = wpe_child_find(pid);

 if (!w)
  return(WPE_CHILD_ENDED_UNKNOWN);
 if (!w->ended)
  return(WPE_CHILD_RUNS);
 *status = w->status;
 return(WPE_CHILD_ENDED);
}

/* ---- starting a child ---------------------------------------------------- */

/** wpe_child_pipes_close - Close every end of a set of pipes still open. */
static void wpe_child_pipes_close(int (*p)[2], int n)
{
 int i, j;

 for (i = 0; i < n; i++)
  for (j = 0; j < 2; j++)
   if (p[i][j] >= 0)
   {
    close(p[i][j]);
    p[i][j] = WPE_CHILD_NO_FD;
   }
}

#define WPE_CHILD_P_IN  0
#define WPE_CHILD_P_OUT 1
#define WPE_CHILD_P_ERR 2
#define WPE_CHILD_P_N   3

/** wpe_child_spec_wants_err_pipe - Does stderr get a pipe of its own? */
static int wpe_child_spec_wants_err_pipe(const WpeChildSpec *spec)
{
 return(!spec->stderr_to_stdout && !spec->stderr_to_null &&
        !spec->stderr_inherit);
}

/* The two ends of a child's pty, WPE_CHILD_NO_FD where it has none, and the
   slave's path. */
typedef struct {
 int master;                     /* the editor's: what the program writes */
 int slave;                      /* WPE_CHILD_IO_PTY: the parent's copy of the
                                    program's end, a carrier of the name, closed
                                    in the parent after the fork.  Never set for
                                    a tty_fd: that descriptor is the caller's */
 char slave_name[WPE_CHILD_TTY_PATH_SIZE];  /* what the child opens, as a
                                    session leader; empty when unknown */
} WpeChildPty;

/**
 * wpe_child_io_has_side_pty - Does the child get a pty beside its own
 * input and output, for a program it starts and names it to?
 */
static int wpe_child_io_has_side_pty(WpeChildIo io)
{
 return(io == WPE_CHILD_IO_PIPES_AND_PTY || io == WPE_CHILD_IO_INHERIT_AND_PTY);
}

/** wpe_child_io_has_pipes - Does the child talk to the editor on pipes? */
static int wpe_child_io_has_pipes(WpeChildIo io)
{
 return(io == WPE_CHILD_IO_PIPES || io == WPE_CHILD_IO_PIPES_AND_PTY);
}

/** wpe_child_pty_close - Close what is still open of a pty. */
static void wpe_child_pty_close(WpeChildPty *t)
{
 if (t->master != WPE_CHILD_NO_FD)
  close(t->master);
 if (t->slave != WPE_CHILD_NO_FD)
  close(t->slave);
 t->master = t->slave = WPE_CHILD_NO_FD;
}

/**
 * wpe_child_tty_name_is - Does the path @name lead to the terminal open on
 * @fd?  Device, inode and device number all agree: a name from another
 * instance of the terminal filesystem (a mount namespace's) can repeat the
 * number of a terminal that is not this one, and opening it would give a
 * session leader that other terminal.  The answer is read without opening
 * the name.
 */
static int wpe_child_tty_name_is(const char *name, int fd)
{
 struct stat by_name, by_fd;

 if (name[0] == '\0')
  return(0);
 if (stat(name, &by_name) != 0 || fstat(fd, &by_fd) != 0)
  return(0);
 return(by_name.st_dev == by_fd.st_dev && by_name.st_ino == by_fd.st_ino &&
        by_name.st_rdev == by_fd.st_rdev);
}

/** XWPE_TEST_FAIL: a comma-separated list of the steps a test makes fail, as
    in the file operations and the writer.  Here: WPE_CHILD_HOOK_OPENPTY. */
#define WPE_CHILD_TEST_FAIL_ENV  "XWPE_TEST_FAIL"
#define WPE_CHILD_HOOK_OPENPTY   "openpty"

/**
 * wpe_child_test_hook - Is @name among the steps the test asked to fail?
 * Read only in the parent, never between fork and exec.  It lets a test show
 * what the user sees when the system has no terminal to give a program (the
 * output still reaches Messages and the exit code is still reported), on
 * every system and without replacing a library function.
 * Return: 1 when XWPE_TEST_FAIL names @name, else 0.
 */
static int wpe_child_test_hook(const char *name)
{
 const char *v = getenv(WPE_CHILD_TEST_FAIL_ENV);
 size_t len = strlen(name);

 while (v != NULL && *v != '\0')
 {
  if (strncmp(v, name, len) == 0 && (v[len] == '\0' || v[len] == ','))
   return(1);
  v = strchr(v, ',');
  if (v != NULL)
   v++;
 }
 return(0);
}

/**
 * wpe_child_openpty - openpty, or its refusal when a test asked for one.
 * Return: what openpty returns; -1 with errno ENOENT for the test's refusal.
 */
static int wpe_child_openpty(int *master, int *slave, char *path)
{
 if (wpe_child_test_hook(WPE_CHILD_HOOK_OPENPTY))
 {
  errno = ENOENT;
  return(-1);
 }
 return(openpty(master, slave, path, NULL, NULL));
}

/**
 * wpe_child_pty_pair - The one openpty of the tree: both ends and the
 * slave's name.
 * @master, @slave: receive the ends; @name, @n: receive the slave's path,
 * proved to lead to the slave: openpty's name, else ttyname_r's (a mount
 * namespace's instance of the terminal filesystem can make openpty's name
 * lead to another terminal).  Empty when neither is proved.
 * @unproved: when not NULL and nothing is proved, receives openpty's own
 * name (empty when it gave none), for a caller that lets the child prove it.
 * Return: 0, or -1 with both ends WPE_CHILD_NO_FD and an empty name.
 */
static int wpe_child_pty_pair(int *master, int *slave, char *name, size_t n,
                              char *unproved, size_t unproved_n)
{
 char path[WPE_CHILD_TTY_PATH_SIZE];
 char by_fd[WPE_CHILD_TTY_PATH_SIZE];

 path[0] = '\0';
 name[0] = '\0';
 if (unproved)
  unproved[0] = '\0';
 *master = *slave = WPE_CHILD_NO_FD;
 if (wpe_child_openpty(master, slave, path) != 0)
 {
  *master = *slave = WPE_CHILD_NO_FD;
  return(-1);
 }
 if (wpe_child_tty_name_is(path, *slave))
  snprintf(name, n, "%s", path);
 else if (ttyname_r(*slave, by_fd, sizeof by_fd) == 0 &&
          wpe_child_tty_name_is(by_fd, *slave))
  snprintf(name, n, "%s", by_fd);
 else if (unproved)
  snprintf(unproved, unproved_n, "%s", path);
 return(0);
}

/**
 * wpe_child_spec_wants_tty - Is this child to have a controlling terminal?
 * IO_PTY, or IO_TTY_PATH with a tty_fd (tty_path alone is the editor's own
 * terminal, written and read, never taken).
 */
static int wpe_child_spec_wants_tty(const WpeChildSpec *spec)
{
 if (spec->io == WPE_CHILD_IO_PTY)
  return(1);
 return(spec->io == WPE_CHILD_IO_TTY_PATH && spec->tty_fd != WPE_CHILD_NO_FD);
}

/**
 * wpe_child_tty_refused - A child that is to have a terminal and cannot: the
 * reason is kept in errno for wpe_child_tty_start_refused, which says it on
 * the status row once SIGCHLD is given back.
 * @err: why.
 * Return: -1, errno @err.
 */
static int wpe_child_tty_refused(int err)
{
 errno = err;
 return(-1);
}

/**
 * wpe_child_pty_raw - Make the editor's end of a pty raw: the terminal then
 * does no output processing, so what the program prints reaches the editor
 * as it wrote it, with no carriage return added to each line end.
 */
static void wpe_child_pty_raw(int master)
{
 struct termios tio;

 if (tcgetattr(master, &tio) == 0)
 {
  cfmakeraw(&tio);
  tcsetattr(master, TCSANOW, &tio);
 }
}

/**
 * wpe_child_tty_prepare_pty - In the parent: the pty of an IO_PTY child,
 * with the slave's name kept for the child to open.  The name is proved when
 * it can be; one that cannot be proved here is still handed over, since the
 * child proves it by stat before it opens it and takes the carrier when it
 * fails.  Refused (ENODEV) only when the system gave no name at all.
 * Return: 0, or -1 with nothing open, said on the status row.
 */
static int wpe_child_tty_prepare_pty(const WpeChildSpec *spec, WpeChildPty *t)
{
 char unproved[WPE_CHILD_TTY_PATH_SIZE];

 if (wpe_child_pty_pair(&t->master, &t->slave, t->slave_name,
                        sizeof t->slave_name, unproved, sizeof unproved) != 0)
  return(wpe_child_tty_refused(errno));
 if (t->slave_name[0] == '\0' && unproved[0] == '\0')
 {
  wpe_child_pty_close(t);
  return(wpe_child_tty_refused(ENODEV));
 }
 if (t->slave_name[0] == '\0')   /* the child proves it before it opens it */
  snprintf(t->slave_name, sizeof t->slave_name, "%s", unproved);
 if (spec->pty_raw)
  wpe_child_pty_raw(t->master);
 return(0);
}

/**
 * wpe_child_tty_prepare_fd - In the parent: the terminal a caller handed
 * over in tty_fd.  Its name is read for the child to open; a name that
 * cannot be read (another instance of the terminal filesystem) leaves the
 * name empty and the child takes the descriptor itself.  The descriptor
 * stays the caller's: t->slave is not set.
 * Return: 0, or -1 for a descriptor that is closed (EBADF) or no terminal
 * (ENOTTY), said on the status row.
 */
static int wpe_child_tty_prepare_fd(const WpeChildSpec *spec, WpeChildPty *t)
{
 if (fcntl(spec->tty_fd, F_GETFD) < 0)
  return(wpe_child_tty_refused(EBADF));
 if (!isatty(spec->tty_fd))
  return(wpe_child_tty_refused(ENOTTY));
 if (ttyname_r(spec->tty_fd, t->slave_name, sizeof t->slave_name) != 0)
  t->slave_name[0] = '\0';
 return(0);
}

/**
 * wpe_child_tty_prepare - In the parent, before the fork: everything the
 * terminal step needs, or a refusal the status row explains.
 * @spec: the child; @t: receives the pty, nothing open when none is wanted.
 *
 * A child that is to have a terminal but not a session of its own cannot
 * take it (EINVAL).  An IO_PTY child gets a pty with its slave's name kept
 * (pty_raw as ever); a tty_fd child, its descriptor's name.  Said once, as
 * "cannot give <what> a terminal: <reason>", before any process exists.
 * Return: 0, or -1 with nothing open and errno set.
 */
static int wpe_child_tty_prepare(const WpeChildSpec *spec, WpeChildPty *t)
{
 t->master = t->slave = WPE_CHILD_NO_FD;
 t->slave_name[0] = '\0';
 if (!wpe_child_spec_wants_tty(spec))
  return(0);
 if (spec->group != WPE_CHILD_OWN_SESSION)
  return(wpe_child_tty_refused(EINVAL));
 if (spec->io == WPE_CHILD_IO_PTY)
  return(wpe_child_tty_prepare_pty(spec, t));
 return(wpe_child_tty_prepare_fd(spec, t));
}

/**
 * wpe_child_pty_make - A pty for a program a child of the editor will start
 * (a line debugger's program), or that the editor starts later itself (jdb's).
 * @master: receives the editor's end, non-blocking; @slave: the program's
 * side; @name, @n: receives the slave's name, which the debugger is told.
 *
 * Both ends are closed on exec, so no other child the editor starts holds
 * the program's terminal by mistake: the child that is to have the slave
 * gets it through its spec (side_pty_slave, or tty_fd), and the program's
 * end then shows on the master as a hangup.
 * Return: 0, or -1 with both set to WPE_CHILD_NO_FD.
 */
int wpe_child_pty_make(int *master, int *slave, char *name, size_t n)
{
 if (wpe_child_pty_pair(master, slave, name, n, NULL, 0) != 0)
  return(-1);
 wpe_child_fd_private(*master);
 fcntl(*slave, F_SETFD, FD_CLOEXEC);
 return(0);
}

/**
 * wpe_child_pipes_open - The pipes the spec asks for.
 * @spec: the child; @p: receives them, -1 where none is wanted.
 *
 * A child on a pty or a terminal has it for 0, 1 and 2, and no pipe; the
 * graphical editors' debugger script keeps the editor's own.
 * Return: 0, or -1 (and nothing open) when a pipe could not be made.
 */
static int wpe_child_pipes_open(const WpeChildSpec *spec, int (*p)[2])
{
 int i;

 for (i = 0; i < WPE_CHILD_P_N; i++)
  p[i][0] = p[i][1] = WPE_CHILD_NO_FD;
 if (!wpe_child_io_has_pipes(spec->io))
  return(0);
 if (!spec->stdout_to_null && !spec->stdout_path &&
     pipe(p[WPE_CHILD_P_OUT]) < 0)
  return(-1);
 if (!spec->stdin_null && pipe(p[WPE_CHILD_P_IN]) < 0)
 {
  wpe_child_pipes_close(p, WPE_CHILD_P_N);
  return(-1);
 }
 if (wpe_child_spec_wants_err_pipe(spec) && pipe(p[WPE_CHILD_P_ERR]) < 0)
 {
  wpe_child_pipes_close(p, WPE_CHILD_P_N);
  return(-1);
 }
 return(0);
}

/* ---- the bytes the parent prepares, the child only carries ---------------- */

/* Between fork and exec the editor's child may call only what takes no lock
   and allocates nothing: the graphical editor runs a library thread (Pango's
   font loader), and a lock that thread holds at the fork stays held in the
   child, for a thread that does not exist there.  So the child formats
   nothing and looks nothing up.  Every sentence it says and every line it
   writes is made here, in the parent, before the fork; the child copies the
   bytes and writes them once. */
#define WPE_CHILD_HEAD_SIZE      (WPE_PATH_SIZE + 128) /* a head: a whole path
                                                         and the longest fixed text */
#define WPE_CHILD_REASON_SIZE    96      /* a reason: strerror's text, cut to fit */
#define WPE_CHILD_DIGITS_MAX     12      /* the digits of an int */
#define WPE_CHILD_SENTENCE_MAX \
 (WPE_CHILD_HEAD_SIZE + WPE_CHILD_REASON_SIZE + sizeof(WPE_CHILD_SAID_END))

/* The errnos a child's sentence names with the system's own words; any other
   reads "error <number>". */
static const int wpe_child_said_errnos[] = {
 EPERM, ENOENT, ENOTDIR, EACCES, ENOEXEC, E2BIG, ENOMEM, ETXTBSY, ELOOP,
 ENAMETOOLONG, EIO, ENOTTY, ENXIO, EBADF, EINVAL, EISDIR
};
#define WPE_CHILD_REASONS ((int)(sizeof wpe_child_said_errnos / sizeof wpe_child_said_errnos[0]))

typedef struct {
 size_t len;
 char text[WPE_CHILD_HEAD_SIZE];
} WpeChildHead;

typedef struct {
 int err;
 size_t len;
 char text[WPE_CHILD_REASON_SIZE];
} WpeChildReason;

/* Everything a child can say about itself, in the words the user reads: the
   four heads and the reason of each errno above. */
typedef struct {
 WpeChildHead dir;               /* "cannot enter <folder>: " */
 WpeChildHead run;               /* "cannot run <program>: " */
 WpeChildHead tty_failed;        /* "cannot give <what> a terminal: " */
 WpeChildHead tty_without;       /* "<what> runs without a controlling terminal: " */
 WpeChildReason reason[WPE_CHILD_REASONS];
} WpeChildSaid;

/* What the child is handed, made before the fork and freed in the parent right
   after it: the child has its own copy of it. */
typedef struct {
 char *echo;                     /* the command line and "\n", or NULL */
 size_t echo_len;
 char *failed;                   /* "<exec_failed> <command line>\n", or NULL */
 size_t failed_len;
 WpeChildSaid *said;             /* NULL for a child that says nothing */
 int fail_code;                  /* the exit of a command that cannot run */
 int report_wr;                  /* the child's end of the report pipe */
} WpeChildWords;

/**
 * wpe_child_line_make - In the parent: the command line as the user reads it,
 * after @prefix when there is one: each word and a space, then a line end.
 * @prefix: what comes first, or NULL or empty; @argv: the words;
 * @len: receives the byte count.
 * Return: the bytes (malloc'd, not a string), or NULL when out of memory.
 */
static char *wpe_child_line_make(const char *prefix, char *const *argv,
                                 size_t *len)
{
 size_t n = 1, plen = 0, wlen;
 char *line, *at;
 int i;

 if (prefix && prefix[0])
  plen = strlen(prefix);
 if (plen)
  n += plen + 1;
 for (i = 0; argv[i]; i++)
  n += strlen(argv[i]) + 1;
 line = malloc(n);
 if (!line)
  return(NULL);
 at = line;
 if (plen)
 {
  memcpy(at, prefix, plen);
  at += plen;
  *at++ = ' ';
 }
 for (i = 0; argv[i]; i++)
 {
  wlen = strlen(argv[i]);
  memcpy(at, argv[i], wlen);
  at += wlen;
  *at++ = ' ';
 }
 *at++ = '\n';
 *len = (size_t)(at - line);
 return(line);
}

/**
 * wpe_child_head_make - In the parent: one head of a sentence, the fixed
 * words around the subject.
 * @h: receives it; @pre, @post: the fixed text before and after the subject
 * (@post ends in ": ", where the reason follows); @subject: what it is about,
 * or NULL for "a program".
 *
 * A subject too long for the head is cut, never the fixed text: the reader
 * always sees where the reason begins.
 */
static void wpe_child_head_make(WpeChildHead *h, const char *pre,
                                const char *post, const char *subject)
{
 size_t pre_len = strlen(pre), post_len = strlen(post), room, n;

 if (!subject)
  subject = WPE_CHILD_UNNAMED;
 room = sizeof h->text - 1 - pre_len - post_len;
 n = strlen(subject);
 if (n > room)
  n = room;
 memcpy(h->text, pre, pre_len);
 memcpy(h->text + pre_len, subject, n);
 memcpy(h->text + pre_len + n, post, post_len);
 h->len = pre_len + n + post_len;
}

/** wpe_child_reason_make - In the parent: the system's words for @err. */
static void wpe_child_reason_make(WpeChildReason *r, int err)
{
 int n = snprintf(r->text, sizeof r->text, "%s", strerror(err));

 r->err = err;
 if (n < 0)
  n = 0;
 if ((size_t)n >= sizeof r->text)
  n = (int)sizeof r->text - 1;
 r->len = (size_t)n;
}

/**
 * wpe_child_said_make - In the parent: the sentences a child may say, for a
 * program that is to have a terminal or whose failures are named to the user
 * (a debugger's Java VM, the Run menu's program).
 * @spec: the child.
 * Return: the table (malloc'd), or NULL when out of memory.
 */
static WpeChildSaid *wpe_child_said_make(const WpeChildSpec *spec)
{
 WpeChildSaid *s = malloc(sizeof *s);
 int i;

 if (!s)
  return(NULL);
 wpe_child_head_make(&s->dir, WPE_CHILD_DIR_FAILED_PRE,
                     WPE_CHILD_DIR_FAILED_POST, spec->cwd ? spec->cwd : "");
 wpe_child_head_make(&s->run, WPE_CHILD_RUN_FAILED_PRE,
                     WPE_CHILD_RUN_FAILED_POST, spec->argv[0]);
 wpe_child_head_make(&s->tty_failed, WPE_CHILD_TTY_FAILED_PRE,
                     WPE_CHILD_TTY_FAILED_POST, spec->what);
 wpe_child_head_make(&s->tty_without, WPE_CHILD_TTY_WITHOUT_PRE,
                     WPE_CHILD_TTY_WITHOUT_POST, spec->what);
 for (i = 0; i < WPE_CHILD_REASONS; i++)
  wpe_child_reason_make(&s->reason[i], wpe_child_said_errnos[i]);
 return(s);
}

/**
 * wpe_child_words_free - In the parent: let go of what the child was handed.
 * @w: the words, empty afterwards.
 */
static void wpe_child_words_free(WpeChildWords *w)
{
 free(w->echo);
 free(w->failed);
 free(w->said);
 w->echo = w->failed = NULL;
 w->said = NULL;
}

/**
 * wpe_child_spec_says - Does the child say anything about itself?  A terminal
 * it may be refused, a folder or program it may not reach and a spec that
 * names its failures (say_failed): the sentences are prepared for those only.
 */
static int wpe_child_spec_says(const WpeChildSpec *spec)
{
 return(wpe_child_spec_wants_tty(spec) || spec->say_failed);
}

/**
 * wpe_child_words_make - In the parent, before the fork: the command line the
 * build shows first, the line of a command that cannot run, and the sentences.
 * @spec: the child; @w: receives them.
 * Return: 0, or -1 (ENOMEM, nothing held) when out of memory.
 */
static int wpe_child_words_make(const WpeChildSpec *spec, WpeChildWords *w)
{
 memset(w, 0, sizeof *w);
 w->report_wr = WPE_CHILD_NO_FD;
 w->fail_code = spec->say_failed ? WPE_CHILD_SAID_FAILED : WPE_CHILD_EXEC_FAILED;
 if (spec->echo_argv)
  w->echo = wpe_child_line_make("", spec->argv, &w->echo_len);
 if (spec->exec_failed)
  w->failed = wpe_child_line_make(spec->exec_failed, spec->argv, &w->failed_len);
 if (wpe_child_spec_says(spec))
  w->said = wpe_child_said_make(spec);
 if ((spec->echo_argv && !w->echo) || (spec->exec_failed && !w->failed) ||
     (wpe_child_spec_says(spec) && !w->said))
 {
  wpe_child_words_free(w);
  errno = ENOMEM;
  return(-1);
 }
 return(0);
}

/**
 * wpe_child_write_all - In the child: write @len bytes to @fd, resuming after
 * a signal and a partial write, and giving up on anything else (a full
 * non-blocking pipe, a closed stream): what cannot be said is not waited for.
 */
static void wpe_child_write_all(int fd, const char *buf, size_t len)
{
 ssize_t n;

 while (len > 0)
 {
  n = write(fd, buf, len);
  if (n < 0 && errno == EINTR)
   continue;
  if (n <= 0)
   return;
  buf += n;
  len -= (size_t)n;
 }
}

/**
 * wpe_child_digits - In the child: the decimal digits of @value, by stores.
 * @out: room for WPE_CHILD_DIGITS_MAX bytes; no terminator.
 * Return: how many.
 */
static size_t wpe_child_digits(int value, char *out)
{
 char rev[WPE_CHILD_DIGITS_MAX];
 unsigned int v = value < 0 ? 0u : (unsigned int)value;
 size_t n = 0, i;

 do
 {
  rev[n++] = (char)('0' + v % 10u);
  v /= 10u;
 } while (v);
 for (i = 0; i < n; i++)
  out[i] = rev[n - 1 - i];
 return(n);
}

/**
 * wpe_child_reason_copy - In the child: the words for @err, from the table
 * the parent prepared, or "error <number>" for any other errno.
 * @out: room for WPE_CHILD_REASON_SIZE bytes.
 * Return: how many bytes.
 */
static size_t wpe_child_reason_copy(char *out, const WpeChildSaid *s, int err)
{
 size_t n = sizeof(WPE_CHILD_REASON_FALLBACK) - 1;
 int i;

 for (i = 0; i < WPE_CHILD_REASONS; i++)
 {
  if (s->reason[i].err != err)
   continue;
  memcpy(out, s->reason[i].text, s->reason[i].len);
  return(s->reason[i].len);
 }
 memcpy(out, WPE_CHILD_REASON_FALLBACK, n);
 return(n + wpe_child_digits(err, out + n));
}

/**
 * wpe_child_utter - In the child: say what could not be done, and why, on
 * its fd 2, where the user reads the program's own output (Messages, for a
 * debugger's program).  One write of bytes the parent prepared.
 * @w: the words; @head: the sentence's head; @err: why.
 */
static void wpe_child_utter(const WpeChildWords *w, const WpeChildHead *head,
                            int err)
{
 char buf[WPE_CHILD_SENTENCE_MAX];
 size_t n;

 if (!w->said || !head)
  return;
 n = head->len;
 memcpy(buf, head->text, n);
 n += wpe_child_reason_copy(buf + n, w->said, err);
 memcpy(buf + n, WPE_CHILD_SAID_END, sizeof(WPE_CHILD_SAID_END) - 1);
 n += sizeof(WPE_CHILD_SAID_END) - 1;
 wpe_child_write_all(STDERR_FILENO, buf, n);
}

/**
 * wpe_child_report_send - In the child: tell the editor where the start
 * stopped and why, as one record on the report pipe.
 * @w: the words (the pipe's end); @stage: WPE_CHILD_STAGE_*; @err: errno.
 *
 * Nothing waits: a record is two integers, far below the pipe's capacity,
 * and the editor reads it only when the child has ended.  The editor may
 * have let go of its end already (the child was forgotten, so its read end
 * was closed with nobody else holding it): the write then fails with EPIPE,
 * which only means nobody listens, so SIGPIPE is ignored around this one
 * write: the child must not die of it before it has said why it cannot run.
 * The one place a record is written.
 */
static void wpe_child_report_send(const WpeChildWords *w, int stage, int err)
{
 WpeChildReport rec;
 struct sigaction ignore, before;

 rec.stage = stage;
 rec.err = err;
 if (w->report_wr == WPE_CHILD_NO_FD)
  return;
 ignore.sa_handler = SIG_IGN;
 sigemptyset(&ignore.sa_mask);
 ignore.sa_flags = 0;
 sigaction(SIGPIPE, &ignore, &before);
 if (write(w->report_wr, &rec, sizeof rec) < 0)
 {
  /* nobody listens: nothing more to do */
 }
 sigaction(SIGPIPE, &before, NULL);
}

/**
 * wpe_child_stop - In the child: a step failed, with nothing to say about
 * it.  The editor is told, and the child ends as a command that cannot run.
 * @w: the words; @stage: WPE_CHILD_STAGE_*.
 */
static WPE_CHILD_NORETURN void wpe_child_stop(const WpeChildWords *w, int stage)
{
 wpe_child_report_send(w, stage, errno);
 _exit(WPE_CHILD_EXEC_FAILED);
}

/**
 * wpe_child_stop_saying - In the child: a step failed.  The editor is told;
 * a spec that names its failures (say_failed) has the sentence written where
 * the user reads the program's output and ends as a shell ends a command it
 * cannot run (127), any other spec ends silently (1).
 * @spec: the child; @w: the words; @stage: WPE_CHILD_STAGE_*; @head: the
 * sentence's head.
 */
static WPE_CHILD_NORETURN void wpe_child_stop_saying(const WpeChildSpec *spec,
                                                     const WpeChildWords *w,
                                                     int stage,
                                                     const WpeChildHead *head)
{
 int err = errno;

 wpe_child_report_send(w, stage, err);
 if (!spec->say_failed)
  _exit(WPE_CHILD_EXEC_FAILED);
 wpe_child_utter(w, head, err);
 _exit(WPE_CHILD_SAID_FAILED);
}

/** wpe_child_dup_null - In the child: /dev/null on @fd, opened @flags. */
static void wpe_child_dup_null(int fd, int flags)
{
 int nul = open("/dev/null", flags);

 if (nul < 0)
  return;
 dup2(nul, fd);
 if (nul > STDERR_FILENO)
  close(nul);
}

/** The mode of a file a child writes its output into: the user's alone. */
#define WPE_CHILD_OUT_FILE_MODE 0600

/**
 * wpe_child_stdout_to_file - In the child: @path, created or emptied, on
 * stdout.
 *
 * A file the child cannot open ends it as a program that cannot run, so
 * the editor never reads an empty result as a good one.
 */
static void wpe_child_stdout_to_file(const char *path, const WpeChildWords *w)
{
 int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, WPE_CHILD_OUT_FILE_MODE);

 if (fd < 0)
  wpe_child_stop(w, WPE_CHILD_STAGE_OUT_FILE);
 dup2(fd, STDOUT_FILENO);
 if (fd > STDERR_FILENO)
  close(fd);
}

/**
 * wpe_child_io_pipes - In the child: the pipes on 0, 1 and 2 as @spec says.
 * @spec: the child; @p: the pipes; @w: the words.
 */
static void wpe_child_io_pipes(const WpeChildSpec *spec, int (*p)[2],
                               const WpeChildWords *w)
{
 if (spec->stdin_null)
  wpe_child_dup_null(WPE_CHILD_FD_IN, O_RDONLY);
 else
  dup2(p[WPE_CHILD_P_IN][WPE_CHILD_PIPE_RD], WPE_CHILD_FD_IN);
 if (spec->stdout_to_null)
  wpe_child_dup_null(STDOUT_FILENO, O_WRONLY);
 else if (spec->stdout_path)
  wpe_child_stdout_to_file(spec->stdout_path, w);
 else
  dup2(p[WPE_CHILD_P_OUT][WPE_CHILD_PIPE_WR], STDOUT_FILENO);
 if (spec->stderr_to_stdout)
  dup2(STDOUT_FILENO, STDERR_FILENO);
 else if (spec->stderr_to_null)
  wpe_child_dup_null(STDERR_FILENO, O_WRONLY);
 else if (!spec->stderr_inherit)
  dup2(p[WPE_CHILD_P_ERR][WPE_CHILD_PIPE_WR], STDERR_FILENO);
 if (spec->stderr_nonblocking)
  fcntl(STDERR_FILENO, F_SETFL,
        fcntl(STDERR_FILENO, F_GETFL, 0) | O_NONBLOCK);
}

/**
 * wpe_child_tty_refuse - In the child: a terminal the system would not give
 * it, said on fd 2 (the terminal by then), where the user reads it in
 * Messages.  The child ends as a command that cannot run (127), unless the
 * spec asks for it to run without (tty_or_without): then it returns.
 * @spec: the child; @w: the words; @err: why.
 *
 * Always said, whatever say_failed: nothing else of the editor reads this
 * terminal, and a program that silently has none is the worse failure.  The
 * editor is told too, which stage it was.
 */
static void wpe_child_tty_refuse(const WpeChildSpec *spec,
                                 const WpeChildWords *w, int err)
{
 if (spec->tty_or_without)
 {
  wpe_child_report_send(w, WPE_CHILD_STAGE_TTY_WITHOUT, err);
  if (w->said)
   wpe_child_utter(w, &w->said->tty_without, err);
  return;
 }
 wpe_child_report_send(w, WPE_CHILD_STAGE_TTY_REFUSED, err);
 if (w->said)
  wpe_child_utter(w, &w->said->tty_failed, err);
 _exit(WPE_CHILD_SAID_FAILED);
}

/**
 * wpe_child_tty_carrier - In the child: the descriptor the terminal can be
 * had from without its name: the pair's slave, or the caller's tty_fd.
 */
static int wpe_child_tty_carrier(const WpeChildSpec *spec, const WpeChildPty *t)
{
 if (spec->io == WPE_CHILD_IO_PTY)
  return(t->slave);
 return(spec->tty_fd);
}

/**
 * wpe_child_tty_open_named - In the child, a session leader by now: open
 * the terminal by its name, so that the system that gives a leader its
 * terminal on an open does.
 * @slave_name: the path; @carrier: the descriptor the parent made it with.
 * Return: the descriptor, or -1 when the name is unknown, names another
 * terminal than the carrier (a mount namespace's) or cannot be opened.
 * The name is proved by stat BEFORE the open, since the open itself gives a
 * leader the terminal it names: a wrong one cannot be given back.
 */
static int wpe_child_tty_open_named(const char *slave_name, int carrier)
{
 if (!wpe_child_tty_name_is(slave_name, carrier))
  return(-1);
 return(open(slave_name, O_RDWR));   /* no O_NOCTTY, as login does */
}

/**
 * wpe_child_tty_take - In the child, a session leader by now: the terminal
 * named in @t becomes its controlling terminal and its 0, 1 and 2.
 * @spec: the child; @t: the pty (the name; the slave of an IO_PTY child);
 * @w: the words.
 *
 * The OpenSSH order with the dup2 before the ioctl, so that a refusal is
 * said on the terminal: the name is opened (System V, Linux and Darwin give
 * a leader its terminal on this open; the BSDs only by the ioctl below),
 * the carrier is used when the name cannot be, TIOCSCTTY is issued (a
 * confirmation where the open already acquired), and tcgetpgrp proves the
 * terminal is the child's own.  The child that cannot is refused
 * (wpe_child_tty_refuse).
 */
static void wpe_child_tty_take(const WpeChildSpec *spec, WpeChildPty *t,
                               const WpeChildWords *w)
{
 int carrier = wpe_child_tty_carrier(spec, t);
 int fd = wpe_child_tty_open_named(t->slave_name, carrier);

 if (fd < 0)
  fd = carrier;
 dup2(fd, WPE_CHILD_FD_IN);
 dup2(fd, STDOUT_FILENO);
 dup2(fd, STDERR_FILENO);
 if (fd > STDERR_FILENO)
  close(fd);
 if (carrier != fd && carrier > STDERR_FILENO)
  close(carrier);
 t->slave = WPE_CHILD_NO_FD;
 if (ioctl(WPE_CHILD_FD_IN, TIOCSCTTY, 0) != 0)
  wpe_child_tty_refuse(spec, w, errno);
 else if (tcgetpgrp(WPE_CHILD_FD_IN) != getpgrp())
  wpe_child_tty_refuse(spec, w, ENOTTY);
}

/**
 * wpe_child_io_tty_path - In the child: the terminal the spec names by its
 * path (the editor's own) on 0, 1 and 2, only written and read, never taken
 * as the controlling one.
 * @spec: the child; @w: the words.
 */
static void wpe_child_io_tty_path(const WpeChildSpec *spec,
                                  const WpeChildWords *w)
{
 int fd;

 fd = open(spec->tty_path, O_RDWR | O_NOCTTY);
 if (fd < 0)
 {
  /* Said on the editor's own fd 2, where the child still is: no dup2 has run. */
  wpe_child_stop_saying(spec, w, WPE_CHILD_STAGE_TTY_OPEN,
                        w->said ? &w->said->tty_failed : NULL);
 }
 dup2(fd, WPE_CHILD_FD_IN);
 dup2(fd, STDOUT_FILENO);
 dup2(fd, STDERR_FILENO);
 if (fd > STDERR_FILENO)
  close(fd);
}

/**
 * wpe_child_side_pty - In the child: keep the program's side of the pty
 * beside its pipes open across exec, for the program it starts and names
 * it to.  The master is never the child's: it is closed on exec.
 * @spec: the child.
 */
static void wpe_child_side_pty(const WpeChildSpec *spec)
{
 if (spec->side_pty_slave != WPE_CHILD_NO_FD)
  fcntl(spec->side_pty_slave, F_SETFD, 0);
}

/**
 * wpe_child_io - In the child: 0, 1 and 2 as the spec says.
 * @spec: the child; @p: the pipes; @t: the pty; @w: the words.
 */
static void wpe_child_io(const WpeChildSpec *spec, int (*p)[2], WpeChildPty *t,
                         const WpeChildWords *w)
{
 if (wpe_child_spec_wants_tty(spec))
 {
  if (t->master != WPE_CHILD_NO_FD)
   close(t->master);   /* never the child's */
  wpe_child_tty_take(spec, t, w);
 }
 else if (spec->io == WPE_CHILD_IO_TTY_PATH)
  wpe_child_io_tty_path(spec, w);
 else if (wpe_child_io_has_side_pty(spec->io))
  wpe_child_side_pty(spec);
 if (wpe_child_io_has_pipes(spec->io))
  wpe_child_io_pipes(spec, p, w);
 wpe_child_pipes_close(p, WPE_CHILD_P_N);
}

/**
 * wpe_child_take_group - In the child: the group or session the spec asks
 * for.  One in the editor's group keeps the editor's terminal, so a script
 * that asks the user something there can still read the answer.
 */
static void wpe_child_take_group(const WpeChildSpec *spec)
{
 if (spec->group == WPE_CHILD_OWN_GROUP)
  setpgid(0, 0);
 else if (spec->group == WPE_CHILD_OWN_SESSION)
  wpe_child_apart_from_terminal();
}

/* The descriptors a child may keep above 0, 1 and 2, and the numbers its
   close step must reach.  What the editor's own libraries, or whatever
   started the editor (a desktop launcher, a CI runner, a shell with a
   coprocess), left open is none of a program's business: a socket that
   reached the Run program kept a connection alive after the editor had
   closed it.  The graphical build runs a library thread (Pango's font
   loader), so between fork and exec only calls that are safe in a child of
   a threaded process are made by the step below: close, close_range,
   closefrom, clock_gettime and plain stores.  Everything it needs to look
   up (the limit, the kept numbers) is read in the parent, before the fork.
   The owner's own report pipe is kept too: the child's end of it, closed by
   the exec itself (close-on-exec), so it is never the program's. */
#define WPE_CHILD_KEEP_MAX     2      /* kept descriptors: the side pty a spec
                                         can name, and the owner's report
                                         pipe.  Grows with the spec, never
                                         for a caller's convenience */
#define WPE_CHILD_FD_FIRST     3      /* the first descriptor above 0, 1, 2 */
#define WPE_CHILD_FD_TO_END    (-1)   /* wpe_child_close_span: all above @lo */
#define WPE_CHILD_FD_CEILING   65536  /* the loop's bound when the hard limit
                                         is larger, infinite or unknown: 18 ms
                                         of closes at 0.27 us each */

/* The table holds the side pty and the report pipe: a build with room for
   fewer is refused here, not at run time between fork and exec. */
typedef char wpe_child_keep_holds_both[(WPE_CHILD_KEEP_MAX >= 2) ? 1 : -1];

typedef struct {
 int fd[WPE_CHILD_KEEP_MAX];  /* ascending, every one above 2 */
 int n;
 int ceiling;                 /* the highest number a close loop must reach,
                                 read in the parent */
} WpeChildKeep;

#ifdef WPE_CHILD_CLOSE_LOOP_ONLY
/* The loop build of the owner's test points this at memory it shares with
   the child: the microseconds the close step took, so a run on another
   system (Darwin has neither call) measures what the loop costs there. */
long *wpe_child_close_timing;

/** wpe_child_close_now_us - A monotonic clock in microseconds, for the loop
 * build's timing of the close step. */
static long wpe_child_close_now_us(void)
{
 struct timespec ts;

 clock_gettime(CLOCK_MONOTONIC, &ts);
 return((long)ts.tv_sec * 1000000L + ts.tv_nsec / 1000L);
}
#endif

/**
 * wpe_child_fd_ceiling - In the parent: the number above which the close
 * loop need not look.
 * @k: the kept descriptors, the ceiling never below the highest of them.
 *
 * The hard RLIMIT_NOFILE when it is finite (a descriptor can sit above the
 * soft limit when the limit was lowered after it was opened), else, and
 * never above, WPE_CHILD_FD_CEILING.  Never the soft limit and never
 * sysconf(_SC_OPEN_MAX), which report the soft limit: where the loop runs
 * (macOS, whose hard limit is unlimited) a leaked descriptor above a lowered
 * soft limit would be left open.
 * Return: the highest descriptor number the loop closes.
 */
static int wpe_child_fd_ceiling(const WpeChildKeep *k)
{
 struct rlimit rl;
 int ceiling = WPE_CHILD_FD_CEILING;
 int i;

 if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_max != RLIM_INFINITY &&
     rl.rlim_max < (rlim_t)WPE_CHILD_FD_CEILING)
  ceiling = (int)rl.rlim_max;
 for (i = 0; i < k->n; i++)
  if (k->fd[i] > ceiling)
   ceiling = k->fd[i];
 return(ceiling);
}

/**
 * wpe_child_keep_list - In the parent, before the fork: the descriptors
 * above 2 this child keeps at exec, by its io kind, and how far the close
 * step must look.
 * @spec: the child; @k: receives the list.
 *
 * The one place a kept descriptor is named: the program's side of the
 * debugger's pty for _PIPES_AND_PTY and _INHERIT_AND_PTY, none for any
 * other kind, and the owner's report pipe @report_wr for every child.  A
 * debugger whose program has no such side (jdb's VM, whose side_pty_slave is
 * WPE_CHILD_NO_FD) keeps only the pipe, and a number of 2 or below is never
 * listed: the step starts at 3 (the report pipe is made above 2).  The list
 * is ascending.  A caller cannot ask for a descriptor by number; a new kind
 * that needs one adds a line here.
 */
static void wpe_child_keep_list(const WpeChildSpec *spec, int report_wr,
                                WpeChildKeep *k)
{
 int side = WPE_CHILD_NO_FD;

 k->n = 0;
 if (wpe_child_io_has_side_pty(spec->io) &&
     spec->side_pty_slave >= WPE_CHILD_FD_FIRST)
  side = spec->side_pty_slave;
 if (side != WPE_CHILD_NO_FD && side < report_wr)
  k->fd[k->n++] = side;
 k->fd[k->n++] = report_wr;
 if (side != WPE_CHILD_NO_FD && side > report_wr)
  k->fd[k->n++] = side;
 k->ceiling = wpe_child_fd_ceiling(k);
}

/**
 * wpe_child_close_span - In the child: close every descriptor from @lo to
 * @hi, or all above @lo for WPE_CHILD_FD_TO_END.
 * @lo, @hi: the span, @lo <= @hi unless @hi is WPE_CHILD_FD_TO_END;
 * @ceiling: how far the loop looks.
 *
 * close_range where the system has it, falling through on any failure (a
 * seccomp filter in a container answers EPERM, a glibc newer than its
 * kernel ENOSYS).  closefrom for an open end where the system has that and
 * no close_range, its return never read (void on some systems, int on
 * others).  A loop to @ceiling otherwise: macOS, and any system where the
 * two fail.  Never closefrom on glibc: it is not a system call there, it
 * lists /proc and aborts the process when it cannot close, which would end
 * the child in a chroot without /proc instead of running it; on glibc
 * close_range runs first.  WPE_CHILD_CLOSE_LOOP_ONLY, set by the test's
 * second build, takes the loop on any system.
 */
static void wpe_child_close_span(int lo, int hi, int ceiling)
{
 int fd, last = ceiling;

#if !defined(WPE_CHILD_CLOSE_LOOP_ONLY) && WPE_HAS_CLOSE_RANGE
 if (close_range((unsigned int)lo,
                 hi == WPE_CHILD_FD_TO_END ? ~0U : (unsigned int)hi, 0) == 0)
  return;
#endif
#if !defined(WPE_CHILD_CLOSE_LOOP_ONLY) && WPE_HAS_CLOSEFROM && !WPE_HAS_CLOSE_RANGE
 if (hi == WPE_CHILD_FD_TO_END)
 {
  closefrom(lo);
  return;
 }
#endif
 if (hi != WPE_CHILD_FD_TO_END && hi < last)
  last = hi;
 for (fd = lo; fd <= last; fd++)
  close(fd);
}

/**
 * wpe_child_close_others - In the child, after 0, 1 and 2 are set and
 * before exec: every descriptor above 2 not in @k is closed, so the
 * program starts with what its spec names and nothing that whatever
 * started the editor left open.
 * @k: the kept descriptors.
 *
 * Walks the spans between the kept numbers, one wpe_child_close_span each,
 * so a kept descriptor is never touched and no flag is read after the fork.
 * A span with no member (the kept number is 3, or two are adjacent) is
 * skipped: close_range refuses it.
 */
static void wpe_child_close_others(const WpeChildKeep *k)
{
 int lo = WPE_CHILD_FD_FIRST;
 int i;
#ifdef WPE_CHILD_CLOSE_LOOP_ONLY
 const long began = wpe_child_close_now_us();
#endif

 for (i = 0; i < k->n; i++)
 {
  if (k->fd[i] > lo)
   wpe_child_close_span(lo, k->fd[i] - 1, k->ceiling);
  lo = k->fd[i] + 1;
 }
 wpe_child_close_span(lo, WPE_CHILD_FD_TO_END, k->ceiling);
#ifdef WPE_CHILD_CLOSE_LOOP_ONLY
 if (wpe_child_close_timing)
  *wpe_child_close_timing = wpe_child_close_now_us() - began;
#endif
}

/**
 * wpe_child_exec - In the child: set it up as @spec says and run it.
 * @spec: the child; @p: the pipes; @t: the pty; @k: the descriptors it
 * keeps; @w: the bytes the parent prepared; @editor: the editor's process
 * ID, read before the fork.
 *
 * Does not return.  The program gets the signal state a shell would give
 * it (none of the editor's handlers), its own group or session, and only
 * the descriptors 0, 1 and 2 and those its spec names (the program's side
 * of a debugger's pty).  It never holds the master of its own pty: the
 * hangup that ends it when the editor dies comes only once every master is
 * closed.  The directory is entered once 0, 1 and 2 are set, so a directory
 * that is gone is named where the program's output goes.
 *
 * Between the fork and the exec this calls only what takes no lock and
 * allocates nothing (system calls, the signal calls, memcpy, and execvp,
 * which only reads the environment and builds its paths on the stack), reads
 * only @w and descriptors, and formats nothing: the command line and every
 * sentence are bytes the parent made.  A failure is also told to the editor
 * on the report pipe, as a stage and an errno.
 */
static WPE_CHILD_NOINLINE WPE_CHILD_NORETURN void
wpe_child_exec(const WpeChildSpec *spec, int (*p)[2], WpeChildPty *t,
               const WpeChildKeep *k, const WpeChildWords *w, pid_t editor)
{
 int exec_err;

 wpe_child_default_signals();
 wpe_child_take_group(spec);
 if (spec->die_with_editor && wpe_child_die_with_editor(editor) != 0)
  wpe_child_stop(w, WPE_CHILD_STAGE_EDITOR_GONE);
 wpe_child_io(spec, p, t, w);
 wpe_child_close_others(k);
 if (spec->cwd && chdir(spec->cwd) != 0)
  wpe_child_stop_saying(spec, w, WPE_CHILD_STAGE_DIR,
                        w->said ? &w->said->dir : NULL);
 if (w->echo)
  wpe_child_write_all(STDERR_FILENO, w->echo, w->echo_len);
 execvp(spec->argv[0], spec->argv);
 exec_err = errno;
 if (w->failed)
  wpe_child_write_all(STDERR_FILENO, w->failed, w->failed_len);
 errno = exec_err;
 wpe_child_stop_saying(spec, w, WPE_CHILD_STAGE_EXEC,
                       w->said ? &w->said->run : NULL);
}

/**
 * wpe_child_keep_end - In the parent: keep one end of a pipe for the
 * editor, non-blocking and closed on exec, and close the child's end.
 * @mine: the editor's end; @theirs: the child's.
 * Return: @mine, or WPE_CHILD_NO_FD when there is no such pipe.
 */
static int wpe_child_keep_end(int mine, int theirs)
{
 if (theirs >= 0)
  close(theirs);
 if (mine < 0)
  return(WPE_CHILD_NO_FD);
 wpe_child_fd_private(mine);
 return(mine);
}

/**
 * wpe_child_take_ends - In the parent: the editor's ends of the pipes.
 * @c: the child; @p: the pipes, all closed or handed over afterwards.
 */
static void wpe_child_take_ends(WpeChild *c, int (*p)[2])
{
 c->in_fd = wpe_child_keep_end(p[WPE_CHILD_P_IN][WPE_CHILD_PIPE_WR],
                               p[WPE_CHILD_P_IN][WPE_CHILD_PIPE_RD]);
 c->out_fd = wpe_child_keep_end(p[WPE_CHILD_P_OUT][WPE_CHILD_PIPE_RD],
                                p[WPE_CHILD_P_OUT][WPE_CHILD_PIPE_WR]);
 c->err_fd = wpe_child_keep_end(p[WPE_CHILD_P_ERR][WPE_CHILD_PIPE_RD],
                                p[WPE_CHILD_P_ERR][WPE_CHILD_PIPE_WR]);
}

/**
 * wpe_child_take_pty - In the parent: the editor's end of the pty.
 * @c: the child; @t: the pty, its slave closed afterwards.
 */
static void wpe_child_take_pty(WpeChild *c, WpeChildPty *t)
{
 c->pty_fd = wpe_child_keep_end(t->master, t->slave);
 t->master = t->slave = WPE_CHILD_NO_FD;
}

/** wpe_child_reset - A WpeChild with no process and no descriptors. */
static void wpe_child_reset(WpeChild *c)
{
 memset(c, 0, sizeof *c);
 c->in_fd = c->out_fd = c->err_fd = c->pty_fd = WPE_CHILD_NO_FD;
 c->end_stage = WPE_CHILD_END_NONE;
}

/** wpe_child_close_fd - Close one of the editor's ends, if open. */
static void wpe_child_close_fd(int fd)
{
 if (fd != WPE_CHILD_NO_FD)
  close(fd);
}

/**
 * wpe_child_drop_ends - A start that failed after the fork: the editor's
 * ends are closed and @c holds no process, so the caller is left with
 * nothing to clean up.
 * @c: the child that was not started.
 */
static void wpe_child_drop_ends(WpeChild *c)
{
 wpe_child_close_fd(c->in_fd);
 wpe_child_close_fd(c->out_fd);
 wpe_child_close_fd(c->err_fd);
 wpe_child_close_fd(c->pty_fd);
 wpe_child_reset(c);
}

/**
 * wpe_child_room - Is there room to follow one more child?  SIGCHLD held.
 *
 * A free slot, and the wake pipe on the loop: both are taken before the
 * fork, so a start the editor could not follow is refused before anything
 * runs, and the user reads why.  The wake pipe is made here if start-up
 * did not make it (a protocol test that starts a server without the
 * editor around it).
 * Return: 1 when there is room.
 */
static int wpe_child_room(void)
{
 WpeChildWakeOpen();
 if (!wpe_child_free_slot() || wpe_child_wake_rd == WPE_CHILD_NO_FD)
  return(0);
 return(wpe_fd_add(wpe_child_wake_rd, POLLIN, wpe_child_on_wake, NULL) == 0);
}

/**
 * wpe_child_refuse - A start that found no room: nothing runs, the status
 * row says so.  SIGCHLD held by the caller, given back here.
 * @spec: the child; @saved: the mask to restore.
 * Return: -1.
 */
static int wpe_child_refuse(const WpeChildSpec *spec, const sigset_t *saved)
{
 wpe_child_wake_off_when_idle();
 WpeChildSignalRestore(saved);
 wpe_child_say(WPE_CHILD_NOT_STARTED_FMT, spec->what);
 errno = EAGAIN;
 return(-1);
}

/**
 * wpe_child_start_failed - The pipes or the fork failed: nothing runs.
 * SIGCHLD held by the caller, given back here.
 * @saved: the mask to restore.
 * Return: -1, errno as the failed call left it.
 */
static int wpe_child_start_failed(const sigset_t *saved)
{
 int err = errno;

 wpe_child_wake_off_when_idle();
 WpeChildSignalRestore(saved);
 errno = err;
 return(-1);
}

/**
 * wpe_child_tty_start_refused - The terminal step refused (errno says why):
 * nothing runs.  SIGCHLD is given back first, then the status row says it.
 * @spec: the child; @saved: the mask to restore.
 * Return: -1, errno as the refusal left it.
 */
static int wpe_child_tty_start_refused(const WpeChildSpec *spec,
                                       const sigset_t *saved)
{
 int err = errno;

 wpe_child_start_failed(saved);
 wpe_child_say_reason(WPE_CHILD_TTY_FAILED_TEXT, spec->what, err);
 return(-1);
}

/**
 * wpe_child_spec_leads_group - Do the child's signals go to its group?
 *
 * Not for one whose group may hold what others share (the build server's
 * launcher), and not for one left in the editor's own group, whose group
 * signal would reach the editor.
 */
static int wpe_child_spec_leads_group(const WpeChildSpec *spec)
{
 if (spec->signal_leader_only)
  return(0);
 return(spec->group != WPE_CHILD_EDITORS_GROUP);
}

/**
 * wpe_child_fd_above - Put a descriptor at a number above 2, closed on exec.
 * @fd: the descriptor, changed to its new number when it was 0, 1 or 2.
 *
 * An editor started with 0, 1 or 2 closed gets those numbers back from
 * pipe(); the child's dup2 onto them would then replace the report pipe.
 * Return: 0, or -1 when the system refused (the descriptor is unchanged).
 */
static int wpe_child_fd_above(int *fd)
{
 int moved;

 if (*fd >= WPE_CHILD_FD_FIRST)
  return(fcntl(*fd, F_SETFD, FD_CLOEXEC));
 moved = fcntl(*fd, F_DUPFD_CLOEXEC, WPE_CHILD_FD_FIRST);
 if (moved < 0)
  return(-1);
 close(*fd);
 *fd = moved;
 return(0);
}

/**
 * wpe_child_report_open - In the parent, before the fork: the pipe a child
 * tells the editor where its start stopped on.
 * @rd: receives the editor's end (non-blocking, closed on exec); @wr: the
 * child's (closed on exec, so a successful exec ends it and the editor reads
 * the end of file as "it ran").  Both are above 2.
 * Return: 0, or -1 with nothing open and errno set.
 */
static int wpe_child_report_open(int *rd, int *wr)
{
 int fds[2];

 if (pipe(fds) < 0)
  return(-1);
 if (wpe_child_fd_above(&fds[WPE_CHILD_PIPE_RD]) < 0 ||
     wpe_child_fd_above(&fds[WPE_CHILD_PIPE_WR]) < 0 ||
     wpe_child_fd_private(fds[WPE_CHILD_PIPE_RD]) < 0)
 {
  int err = errno;

  close(fds[WPE_CHILD_PIPE_RD]);
  close(fds[WPE_CHILD_PIPE_WR]);
  errno = err;
  return(-1);
 }
 *rd = fds[WPE_CHILD_PIPE_RD];
 *wr = fds[WPE_CHILD_PIPE_WR];
 return(0);
}

/**
 * wpe_child_fork_gate_close - Block every signal in this thread across the
 * fork, so no handler of the editor's runs in the child before it has set
 * its signals to the program's (a handler that interrupts the copy would run
 * on memory it cannot use).
 * @held: receives the mask to give back (the caller's, SIGCHLD held).
 */
static void wpe_child_fork_gate_close(sigset_t *held)
{
 sigset_t all;

 sigfillset(&all);
 pthread_sigmask(SIG_SETMASK, &all, held);
}

/** wpe_child_fork_gate_open - Give back the mask wpe_child_fork_gate_close kept. */
static void wpe_child_fork_gate_open(const sigset_t *held)
{
 pthread_sigmask(SIG_SETMASK, held, NULL);
}

/**
 * wpe_child_prepare_words - In the parent, before the fork: the bytes the
 * child carries and the pipe it reports on.
 * @spec: the child; @w: receives the words, @w->report_wr the child's end of
 * the pipe; @report_rd: receives the editor's end.
 * Return: 0, or -1 with nothing held and errno set (ENOMEM, or the pipe's).
 */
static int wpe_child_prepare_words(const WpeChildSpec *spec, WpeChildWords *w,
                                   int *report_rd)
{
 if (wpe_child_words_make(spec, w) < 0)
  return(-1);
 if (wpe_child_report_open(report_rd, &w->report_wr) < 0)
 {
  int err = errno;

  wpe_child_words_free(w);
  errno = err;
  return(-1);
 }
 return(0);
}

/**
 * wpe_child_start - Start a program for the editor, its end watched.
 * @spec: what to run and how; @c: receives the process and its descriptors
 * (the caller's, valid until the end is reported); @ended: called once,
 * from the loop, when it has ended; @data: handed to @ended.
 *
 * SIGCHLD is held from before the fork until the child has its slot, so its
 * end, however soon it comes, is reported with its own status, however many
 * other children end at the same moment.  When no slot is free, or the loop
 * has no room for the wake pipe, nothing is started and the status row says
 * so.
 * Return: 0, or -1 with nothing left running.
 */
int wpe_child_start(const WpeChildSpec *spec, WpeChild *c,
                    WpeChildEndedFn ended, void *data)
{
 int p[WPE_CHILD_P_N][2];
 WpeChildPty t;
 WpeChildKeep keep;
 WpeChildWords words;
 WpeChildSlot *w;
 sigset_t saved, held;
 const pid_t editor = getpid();
 int report_rd = WPE_CHILD_NO_FD;
 pid_t pid;

 wpe_child_reset(c);
 if (wpe_child_unplanned(spec))
 {
  errno = EPERM;
  return(-1);
 }
 WpeChildSignalBlock(&saved);
 if (!wpe_child_room())
  return(wpe_child_refuse(spec, &saved));
 if (wpe_child_tty_prepare(spec, &t) < 0)
  return(wpe_child_tty_start_refused(spec, &saved));
 if (wpe_child_pipes_open(spec, p) < 0)
 {
  wpe_child_pty_close(&t);
  return(wpe_child_start_failed(&saved));
 }
 if (wpe_child_prepare_words(spec, &words, &report_rd) < 0)
 {
  wpe_child_pipes_close(p, WPE_CHILD_P_N);
  wpe_child_pty_close(&t);
  return(wpe_child_start_failed(&saved));
 }
 wpe_child_keep_list(spec, words.report_wr, &keep);
 wpe_child_fork_gate_close(&held);
 pid = fork();
 if (pid == 0)
  wpe_child_exec(spec, p, &t, &keep, &words, editor);
 wpe_child_fork_gate_open(&held);
 wpe_child_words_free(&words);   /* the child has its own copy */
 close(words.report_wr);
 if (pid < 0)
 {
  close(report_rd);
  wpe_child_pipes_close(p, WPE_CHILD_P_N);
  wpe_child_pty_close(&t);
  return(wpe_child_start_failed(&saved));
 }
 if (spec->group == WPE_CHILD_OWN_GROUP)
  setpgid(pid, pid);   /* both sides: the group exists before any signal */
 wpe_child_take_ends(c, p);
 wpe_child_take_pty(c, &t);
 w = wpe_child_slot_take(pid);
 if (!w)
 {
  /* Not reached: wpe_child_room took the slot's room before the fork. */
  close(report_rd);
  kill(-pid, SIGKILL);
  kill(pid, SIGKILL);
  wpe_child_drop_ends(c);
  return(wpe_child_refuse(spec, &saved));
 }
 w->report_rd = report_rd;
 c->pid = pid;
 c->leads_group = wpe_child_spec_leads_group(spec);
 w->child = c;
 w->child_ended = ended;
 w->child_data = data;
 w->what = spec->what;
 w->group = c->leads_group;
 w->pid = pid;
 WpeChildSignalRestore(&saved);
 return(0);
}

/* ---- signalling and ending ----------------------------------------------- */

/**
 * wpe_child_running_slot - The slot of a child that has not been reaped.
 * @c: the child.  SIGCHLD held.
 * Return: the slot, or NULL once the child is gone (its number may then be
 * another process's).
 */
static WpeChildSlot *wpe_child_running_slot(const WpeChild *c)
{
 WpeChildSlot *w = wpe_child_find(c->pid);

 if (!w || w->ended || w->child != c)
  return(NULL);
 return(w);
}

/**
 * wpe_child_signal - Send @sig to the child and the group it leads.
 * @c: the child; @sig: the signal.
 *
 * Nothing is sent once it has been reaped, so a number the system gave to
 * another process is never signalled.
 * Return: 0, or -1 once it has ended.
 */
int wpe_child_signal(WpeChild *c, int sig)
{
 sigset_t saved;
 int ret = -1;

 WpeChildSignalBlock(&saved);
 if (c->pid > 0 && wpe_child_running_slot(c))
 {
  ret = 0;
  if (!c->leads_group || kill(-c->pid, sig) < 0)
   kill(c->pid, sig);
 }
 WpeChildSignalRestore(&saved);
 return(ret);
}

/**
 * wpe_child_signal_pid - Send @sig to the child alone, not its group.
 * @c: the child; @sig: the signal.
 * Return: 0, or -1 once it has ended.
 */
int wpe_child_signal_pid(WpeChild *c, int sig)
{
 sigset_t saved;
 int ret = -1;

 WpeChildSignalBlock(&saved);
 if (c->pid > 0 && wpe_child_running_slot(c))
 {
  ret = 0;
  kill(c->pid, sig);
 }
 WpeChildSignalRestore(&saved);
 return(ret);
}

/**
 * wpe_child_signal_named - Send @sig to a process the child started and
 * named, while the child runs.
 * @c: the child; @pid: the process it named (the graphical editors'
 * debugger script writes down the debugger it runs); @sig: the signal.
 *
 * Pause reaches the debugger itself, not the script around it.  Nothing is
 * sent once the child has ended: the number it wrote down may be another
 * process's by then.
 * Return: 0, or -1 when the child has ended or @pid names nothing.
 */
int wpe_child_signal_named(WpeChild *c, int pid, int sig)
{
 sigset_t saved;
 int ret = -1;

 if (pid <= 0)
  return(-1);
 WpeChildSignalBlock(&saved);
 if (c->pid > 0 && wpe_child_running_slot(c))
  ret = kill(pid, sig);
 WpeChildSignalRestore(&saved);
 return(ret);
}

/**
 * wpe_child_give_up - The ladder's last step: the child did not end even
 * after SIGKILL (it is stuck in the kernel).  The user reads so, its slot is
 * let go (the handler reaps it whenever it ends), and its owner is told.
 * @c: the child.
 */
static void wpe_child_give_up(WpeChild *c)
{
 WpeChildEndedFn ended = NULL;
 void *data = NULL;
 const char *what = NULL;
 WpeChildSlot *w;
 sigset_t saved;

 WpeChildSignalBlock(&saved);
 w = wpe_child_running_slot(c);
 if (w)
 {
  ended = w->child_ended;
  data = w->child_data;
  what = w->what;
  wpe_child_slot_free(w);
  wpe_child_wake_off_when_idle();
 }
 WpeChildSignalRestore(&saved);
 if (!w)
  return;
 c->end_stage = WPE_CHILD_END_GIVEN_UP;
 c->pid = 0;
 wpe_child_say(WPE_CHILD_DID_NOT_END_FMT, what);
 if (ended)
  ended(c, 0, WPE_CHILD_ENDED_UNKNOWN, data);
}

/**
 * wpe_child_end_signal - Send a signal of the ladder, noting a SIGKILL.
 * @c: the child being ended; @sig: the signal.
 *
 * The owner of a child that ran on after its output closed learns from
 * killed_by_end that its status 137 is the editor's kill, so the user is
 * not told the program died by itself.
 */
static void wpe_child_end_signal(WpeChild *c, int sig)
{
 if (sig == SIGKILL)
  c->killed_by_end = 1;
 wpe_child_signal(c, sig);
}

/**
 * wpe_child_end_step - The ladder's next step: SIGKILL after the grace,
 * give up after the kill grace.
 * @c: a child being ended.
 */
static void wpe_child_end_step(WpeChild *c)
{
 if (c->pid <= 0)
  return;
 if (c->end_stage == WPE_CHILD_END_KILL)
 {
  wpe_child_give_up(c);
  return;
 }
 c->end_stage = WPE_CHILD_END_KILL;
 wpe_child_end_signal(c, SIGKILL);
 c->end_due_ms = wpe_child_now_ms() + WPE_CHILD_END_KILL_MS;
}

/**
 * wpe_child_in_ladder - The child of slot @w, when it is being ended and
 * not reaped yet (a reaped one is reported first, which ends its ladder).
 * Return: the child, or NULL.
 */
static WpeChild *wpe_child_in_ladder(const WpeChildSlot *w)
{
 WpeChild *c = w->child;

 if (!w->pid || w->ended || !c)
  return(NULL);
 if (c->end_stage != WPE_CHILD_END_POLITE && c->end_stage != WPE_CHILD_END_KILL)
  return(NULL);
 return(c);
}

/**
 * wpe_child_ladder_due - When the earliest step of any ladder is due.
 * Return: that time on wpe_child_now_ms's clock, or 0 when none is.
 */
static long wpe_child_ladder_due(void)
{
 long due = 0;
 WpeChild *c;
 int i;

 for (i = 0; i < WPE_CHILD_MAX; i++)
 {
  c = wpe_child_in_ladder(&wpe_child_table[i]);
  if (c && (!due || c->end_due_ms < due))
   due = c->end_due_ms;
  if (wpe_child_lingers[i].pgid && (!due || wpe_child_lingers[i].due_ms < due))
   due = wpe_child_lingers[i].due_ms;
 }
 return(due);
}

/**
 * wpe_child_ladder_run - Take the steps that are due.
 * @force: take every ladder's next step now, due or not (the loop had no
 * room for the ladder's timer).
 *
 * A step's callback may start or end other children; each slot is looked at
 * again as it is reached.
 */
static void wpe_child_ladder_run(int force)
{
 long now = wpe_child_now_ms();
 WpeChild *c;
 int i;

 for (i = 0; i < WPE_CHILD_MAX; i++)
 {
  c = wpe_child_in_ladder(&wpe_child_table[i]);
  if (c && (force || c->end_due_ms <= now))
   wpe_child_end_step(c);
  if (wpe_child_lingers[i].pgid && (force || wpe_child_lingers[i].due_ms <= now))
   wpe_child_linger_kill(&wpe_child_lingers[i]);
 }
}

/** wpe_child_ladder_tick - The ladder's timer: its steps, then the next. */
static void wpe_child_ladder_tick(void *data)
{
 (void)data;
 wpe_child_ladder_timer = WPE_TIMER_NONE;
 wpe_child_ladder_run(0);
 wpe_child_ladder_rearm();
}

/**
 * wpe_child_ladder_rearm - Set the ladder's timer for the earliest step due.
 *
 * Called whenever a ladder begins, moves or ends.  A loop with no room for
 * the timer has every ladder take its next step now, so a child is never
 * left half-ended; each call moves a ladder one step on, so this ends.
 */
static void wpe_child_ladder_rearm(void)
{
 long due = wpe_child_ladder_due(), ms;

 if (wpe_child_ladder_timer != WPE_TIMER_NONE)
  wpe_timer_del(wpe_child_ladder_timer);
 wpe_child_ladder_timer = WPE_TIMER_NONE;
 if (!due)
  return;
 ms = due - wpe_child_now_ms();
 if (ms < 0)
  ms = 0;
 wpe_child_ladder_timer = wpe_timer_add(ms, 0, wpe_child_ladder_tick, NULL);
 if (wpe_child_ladder_timer > WPE_TIMER_NONE)
  return;
 wpe_child_ladder_timer = WPE_TIMER_NONE;
 wpe_child_ladder_run(1);
 wpe_child_ladder_rearm();
}

/**
 * wpe_child_end - End a child, without waiting for it.
 * @c: the child; @polite_sig: SIGTERM (or another signal that asks it to
 * end), or WPE_CHILD_NO_SIGNAL when the caller already asked on its pipe.
 *
 * Returns at once.  The child gets WPE_CHILD_END_GRACE_MS to end, then
 * SIGKILL to its group, then WPE_CHILD_END_KILL_MS more before the owner is
 * told it did not end.  Its end is reported as usual, by the callback.  A
 * second call during the grace sends SIGKILL now; once SIGKILL is out, a
 * call changes nothing, so a key pressed again and again never gives up on a
 * child the kernel is still ending.
 */
void wpe_child_end(WpeChild *c, int polite_sig)
{
 if (c->pid <= 0)
  return;
 if (c->end_stage == WPE_CHILD_END_KILL)
  return;
 if (c->end_stage != WPE_CHILD_END_NONE)
 {
  wpe_child_end_step(c);
  wpe_child_ladder_rearm();
  return;
 }
 c->end_stage = WPE_CHILD_END_POLITE;
 if (polite_sig != WPE_CHILD_NO_SIGNAL)
  wpe_child_end_signal(c, polite_sig);
 c->end_due_ms = wpe_child_now_ms() + WPE_CHILD_END_GRACE_MS;
 wpe_child_ladder_rearm();
}

/**
 * wpe_child_detached_ended - The callback of a child whose owner has gone:
 * nobody is told, and its record is free again (its pid is 0 already).
 */
static void wpe_child_detached_ended(WpeChild *c, int status, int how,
                                     void *data)
{
 (void)c;
 (void)status;
 (void)how;
 (void)data;
}

/** wpe_child_detached_record - A free record for a child, or NULL. */
static WpeChild *wpe_child_detached_record(void)
{
 int i;

 for (i = 0; i < WPE_CHILD_MAX; i++)
  if (wpe_child_detached[i].pid <= 0)
   return(&wpe_child_detached[i]);
 return(NULL);
}

/**
 * wpe_child_detach - Move a child into one of the owner's own records.
 * @c: the child, its slot's.  SIGCHLD held.
 * Return: the record now followed in its place, or NULL when @c is not
 * followed (or, never in practice, no record is free).
 */
static WpeChild *wpe_child_detach(WpeChild *c)
{
 WpeChildSlot *w = wpe_child_find(c->pid);
 WpeChild *d;

 if (!w || w->child != c)
  return(NULL);
 d = wpe_child_detached_record();
 if (!d)
  return(NULL);
 *d = *c;
 w->child = d;
 w->child_ended = wpe_child_detached_ended;
 w->child_data = NULL;
 return(d);
}

/**
 * wpe_child_end_detached - End a child whose owner is going away, without
 * waiting for it.
 * @c: the child; @polite_sig: as for wpe_child_end.
 *
 * The owner frees @c at once (a language server's session when the user
 * opens another file, the build server's at the end of a Scala debug
 * session): the end goes on in a record of the owner's, on the same ladder,
 * and nobody is called back; "<what> did not end" is still said.  @c holds
 * no process afterwards; its descriptors are the caller's to close.
 */
void wpe_child_end_detached(WpeChild *c, int polite_sig)
{
 WpeChild *d;
 sigset_t saved;

 if (c->pid <= 0)
  return;
 WpeChildSignalBlock(&saved);
 d = wpe_child_detach(c);
 WpeChildSignalRestore(&saved);
 if (!d)
 {
  wpe_child_signal(c, SIGKILL);
  wpe_child_forget(c);
  return;
 }
 c->pid = 0;
 c->end_stage = WPE_CHILD_END_NONE;
 if (d->end_stage == WPE_CHILD_END_NONE)
  wpe_child_end(d, polite_sig);
 else
  wpe_child_ladder_rearm();
}

/**
 * wpe_child_end_rest - End what is left of a group whose leader has ended.
 * @pgid: the process ID of a child started here that led its group, its end
 * already reported; @polite_sig: as for wpe_child_end.
 *
 * A shell the assistant ran has exited, but a command it put in the
 * background still holds the output the editor reads: at the command's
 * deadline the rest of the group gets the polite signal now and SIGKILL when
 * the grace is over, on the same ladder as any child, so the tool step ends
 * and nothing the command started is left.  A group with nobody left needs
 * nothing, and its number is not signalled.
 */
void wpe_child_end_rest(int pgid, int polite_sig)
{
 WpeChildLinger *l;

 if (pgid <= 0 || !wpe_child_group_has_members(pgid))
  return;
 if (polite_sig != WPE_CHILD_NO_SIGNAL)
  kill(-pgid, polite_sig);
 l = wpe_child_linger_free();
 if (!l)
 {
  kill(-pgid, SIGKILL);
  return;
 }
 l->pgid = pgid;
 l->due_ms = wpe_child_now_ms() + WPE_CHILD_END_GRACE_MS;
 wpe_child_ladder_rearm();
}

/**
 * wpe_child_end_named - End a process the editor did not start but was
 * told about, without waiting for it.
 * @pid: the process a child named (a debug adapter's program, from its
 * `process` event); @polite_sig: as for wpe_child_end, WPE_CHILD_NO_SIGNAL
 * when the child was already asked to end it.
 *
 * Quit of a debug session whose adapter does not end the program it was
 * told to end: the program gets the polite signal, then SIGKILL when
 * WPE_CHILD_END_GRACE_MS is over, on the same ladder as the editor's own
 * children, so it does not run on after the session (or after the editor).
 * Only that process is signalled, never its group: it may share one with a
 * server other editors use.  One that has ended needs nothing, and neither
 * init nor the editor itself is ever signalled, whatever a child says.
 */
void wpe_child_end_named(int pid, int polite_sig)
{
 WpeChildLinger *l;
 long now;

 if (pid <= WPE_CHILD_INIT_PID || pid == (int)getpid() || kill(pid, 0) != 0)
  return;
 if (polite_sig != WPE_CHILD_NO_SIGNAL)
  kill(pid, polite_sig);
 l = wpe_child_linger_free();
 if (!l)
 {
  kill(pid, SIGKILL);
  return;
 }
 now = wpe_child_now_ms();
 l->alone = 1;
 l->kill_ms = now + WPE_CHILD_END_GRACE_MS;
 l->due_ms = now + WPE_CHILD_NAMED_POLL_MS;
 l->pgid = pid;
 wpe_child_ladder_rearm();
}

/**
 * wpe_child_wake_fd - The descriptor that turns readable when a followed
 * child has ended.
 *
 * For the one wait that must hand its caller a short command's status before
 * it returns (the assistant's edit hook and workspace commands): it watches
 * this beside the command's output and lets wpe_fd_dispatch report the end,
 * so the status still comes only from the SIGCHLD handler.
 * Return: the descriptor, or WPE_CHILD_NO_FD when none was made.
 */
int wpe_child_wake_fd(void)
{
 return(wpe_child_wake_rd);
}

/**
 * wpe_child_ending - How many children are being ended and not gone yet.
 *
 * The editor's own end waits, within its bound, until this is 0, so a
 * language server or a build server that ignores SIGTERM does not outlive
 * the editor.
 * Return: the number of ladders still running, groups whose rest is still
 * to be killed included.
 */
int wpe_child_ending(void)
{
 int i, n = 0;

 for (i = 0; i < WPE_CHILD_MAX; i++)
 {
  if (wpe_child_in_ladder(&wpe_child_table[i]))
   n++;
  if (wpe_child_lingers[i].pgid)
   n++;
 }
 return(n);
}

/**
 * wpe_child_forget - Stop following a child: its callback never comes.
 * @c: the child.  It is reaped by the handler whenever it ends.
 */
void wpe_child_forget(WpeChild *c)
{
 if (c->pid <= 0)
  return;
 wpe_child_stop_ladder(c);
 WpeChildUnwatch(c->pid);
 c->pid = 0;
}

/**
 * wpe_child_ended - Has the child ended?  Never waits.
 * @c: the child; @status: receives its wait status when known.
 * Return: WPE_CHILD_RUNS, WPE_CHILD_ENDED, or WPE_CHILD_ENDED_UNKNOWN once
 * its end has been reported (pid is 0).
 */
int wpe_child_ended(const WpeChild *c, int *status)
{
 if (c->pid <= 0)
  return(WPE_CHILD_ENDED_UNKNOWN);
 return(WpeChildEnded(c->pid, status));
}

/* ---- short commands, waited for on the loop ------------------------------ */

#define WPE_CAPTURE_CHUNK        4096    /* what one read takes */
#define WPE_CAPTURE_KILL_WAIT_MS (WPE_CHILD_END_KILL_MS + 500)
#define WPE_CAPTURE_WHAT         "a command"
#define WPE_CAPTURE_STATUS_LOST  (-1)    /* the end was reported without one */
/* A command still running after this long is named on the status row with
   its deadline, so the user knows what the editor waits for and how long. */
#define WPE_CAPTURE_SLOW_MS      1000L
#define WPE_CAPTURE_SLOW_FMT     "Waiting for %s (at most %ld s)"
#define WPE_CAPTURE_STOPPED_FMT  "Esc: stopped waiting for %s"
/* Whether a wait takes the user's Esc (a command the user started) or leaves
   it queued (a probe the editor made on its own). */
#define WPE_CAPTURE_ASKS_ESC     0
#define WPE_CAPTURE_IGNORES_ESC  1
#define WPE_MS_PER_S             1000L

/* The display, while a command is waited for: the descriptor the loop must
   not wake for (the console's keyboard) and what answers the window's
   repaint and resize events.  Unset until the display is open. */
static int (*wpe_capture_quiet_fd_hook)(void);
static void (*wpe_capture_serve_hook)(void);
static int (*wpe_capture_cancel_hook)(void);

/**
 * wpe_child_capture_display_set - Tell the owner how the display is served
 * while it waits for a short command.
 * @quiet_fd: the descriptor whose input stays queued for the editor (the
 * console's keyboard), or NULL; @serve: answers the window's repaint and
 * resize events, or NULL; @cancel_asked: non-zero once the user pressed the
 * key that stops the wait (Esc), or NULL when the display has none.
 *
 * Set once the display is open, so a window covered or resized while the
 * editor waits for the print command or a manual page is drawn again at
 * once, a key typed meanwhile is read by the editor afterwards, and Esc
 * ends the command instead of leaving the user to wait out its deadline.
 */
void wpe_child_capture_display_set(int (*quiet_fd)(void), void (*serve)(void),
                                   int (*cancel_asked)(void))
{
 wpe_capture_quiet_fd_hook = quiet_fd;
 wpe_capture_serve_hook = serve;
 wpe_capture_cancel_hook = cancel_asked;
}

/* The command being waited for.  Only the innermost one is asked about: a
   wait is entered and left in order. */
typedef struct {
 WpeChild child;
 int ended;                      /* the owner reported the end */
 int status;                     /* its wait status, or _STATUS_LOST */
 int eof;                        /* the output pipe was read to its end */
 int cancelled;                  /* the user stopped the wait */
 int killed;                     /* ended by the editor: only its end is waited for */
 int deaf;                       /* the user did not start it: Esc is not asked */
 char *out;                      /* where its output goes, or NULL */
 size_t max, len;
 const char *what;               /* names it on the status row */
} WpeCapture;

static WpeCapture *wpe_capture_now;

/**
 * wpe_capture_read - Take what the command has written so far.
 * @cap: the command.
 *
 * Output past the room given is read and dropped, so the command is never
 * left blocked on a full pipe.
 */
static void wpe_capture_read(WpeCapture *cap)
{
 char dropped[WPE_CAPTURE_CHUNK];
 char *dst;
 size_t room;
 ssize_t n;

 for (;;)
 {
  dst = dropped;
  room = sizeof dropped;
  if (cap->out && cap->len + 1 < cap->max)
  {
   dst = cap->out + cap->len;
   room = cap->max - 1 - cap->len;
  }
  n = read(cap->child.out_fd, dst, room);
  if (n < 0 && errno == EINTR)
   continue;
  if (n <= 0)
  {
   cap->eof = (n == 0);
   return;
  }
  if (dst != dropped)
   cap->len += (size_t)n;
 }
}

/** wpe_capture_on_readable - The loop: the command wrote, or closed its output. */
static void wpe_capture_on_readable(int fd, void *data)
{
 WpeCapture *cap = data;

 wpe_capture_read(cap);
 if (cap->eof)
  wpe_fd_del(fd);
}

/** wpe_capture_on_end - The owner: the command ended. */
static void wpe_capture_on_end(WpeChild *c, int status, int how, void *data)
{
 WpeCapture *cap = data;

 (void)c;
 if (cap->child.out_fd >= 0 && !cap->eof)
  wpe_capture_read(cap);   /* all it wrote before it ended is in the pipe */
 cap->status = (how == WPE_CHILD_ENDED) ? status : WPE_CAPTURE_STATUS_LOST;
 cap->ended = 1;
}

/** wpe_capture_has_ended - The wait's condition: the command ended, or the
 * user stopped waiting for it before the editor ended it. */
static int wpe_capture_has_ended(void)
{
 if (wpe_capture_now->ended)
  return(1);
 return(wpe_capture_now->cancelled && !wpe_capture_now->killed);
}

/**
 * wpe_capture_serve - What the wait does whenever the display has an event
 * for the editor: answer the window's repaints, then look for the key that
 * stops the wait.
 */
static void wpe_capture_serve(void)
{
 wpe_capture_serve_hook();
 if (wpe_capture_now->killed || wpe_capture_now->deaf ||
     !wpe_capture_cancel_hook)
  return;
 if (wpe_capture_cancel_hook())
  wpe_capture_now->cancelled = 1;
}

/* What wpe_fd_wait_serving calls to serve the display. */
typedef void (*WpeCaptureServe)(void);

/**
 * wpe_capture_display_server - What serves the display during the wait:
 * wpe_capture_serve, or NULL while no display is served (the console, or
 * before the window is open).
 */
static WpeCaptureServe wpe_capture_display_server(void)
{
 if (!wpe_capture_serve_hook)
  return(NULL);
 return(wpe_capture_serve);
}

/**
 * wpe_capture_spec - The spec of a short command: no input, errors with the
 * output (the version of a Java is written on its stderr), or its output
 * into a file.
 */
static void wpe_capture_spec(WpeChildSpec *spec, const struct WpeStartPlan *plan,
                             char *const argv[], const char *path, const char *what)
{
 memset(spec, 0, sizeof *spec);
 spec->argv = argv;
 spec->plan = plan;
 if (plan)
  spec->origin = WPE_CHILD_ORIGIN_USER;
 spec->io = WPE_CHILD_IO_PIPES;
 spec->group = WPE_CHILD_OWN_SESSION;
 spec->stdin_null = 1;
 spec->stdout_path = path;
 spec->stderr_to_stdout = (path == NULL);
 spec->stderr_to_null = (path != NULL);
 spec->side_pty_slave = WPE_CHILD_NO_FD;
 spec->tty_fd = WPE_CHILD_NO_FD;
 spec->what = what ? what : WPE_CAPTURE_WHAT;
}

/**
 * wpe_capture_begin - Start the command and watch its output.
 * @cap: the command's record; @spec: what to run.
 * Return: 0, or -1 with nothing left running.
 */
static int wpe_capture_begin(WpeCapture *cap, const WpeChildSpec *spec)
{
 if (wpe_child_start(spec, &cap->child, wpe_capture_on_end, cap) < 0)
  return(-1);
 if (cap->child.out_fd < 0)
  return(0);
 if (wpe_fd_add(cap->child.out_fd, POLLIN, wpe_capture_on_readable, cap) < 0)
 {
  wpe_child_end(&cap->child, SIGKILL);
  close(cap->child.out_fd);
  wpe_child_forget(&cap->child);
  return(-1);
 }
 return(0);
}

/**
 * wpe_capture_serve_until - Wait on the loop, the display served, until the
 * command ends or @ms have passed.
 * @cap: the command; @ms: the longest this part of the wait takes.
 * Return: 1 when the command has ended or the user stopped the wait, else 0.
 */
static int wpe_capture_serve_until(WpeCapture *cap, long ms)
{
 int quiet_fd = -1;

 if (wpe_capture_quiet_fd_hook)
  quiet_fd = wpe_capture_quiet_fd_hook();
 wpe_fd_wait_serving(wpe_capture_has_ended, cap->what, quiet_fd,
                     wpe_capture_display_server(), ms);
 return(wpe_capture_has_ended());
}

/**
 * wpe_capture_kill - End the command now and wait, bounded, for its end to
 * be reported, so its status says how it ended.
 */
static void wpe_capture_kill(WpeCapture *cap)
{
 wpe_child_end(&cap->child, SIGKILL);
 cap->killed = 1;
 wpe_capture_serve_until(cap, WPE_CAPTURE_KILL_WAIT_MS);
}

/**
 * wpe_capture_verdict - The wait is over before the deadline: the command
 * ended by itself, or the user stopped it, and then it is ended now.
 *
 * The user who pressed Esc reads on the status row that the wait was
 * stopped, so the editor's silence afterwards is not taken for a failure
 * of the command.
 * Return: WPE_CAPTURE_ENDED or WPE_CAPTURE_CANCELLED.
 */
static int wpe_capture_verdict(WpeCapture *cap)
{
 if (cap->ended)
  return(WPE_CAPTURE_ENDED);
 wpe_capture_kill(cap);
 wpe_child_say(WPE_CAPTURE_STOPPED_FMT, cap->what);
 return(WPE_CAPTURE_CANCELLED);
}

/**
 * wpe_capture_say_slow - Name the command the editor is waiting for, and
 * the most it will wait, on the status row.
 * @cap: the command; @deadline_ms: its deadline.
 */
static void wpe_capture_say_slow(const WpeCapture *cap, long deadline_ms)
{
 char line[WPE_CHILD_LINE_MAX];

 if (!wpe_child_say_hook)
  return;
 snprintf(line, sizeof line, WPE_CAPTURE_SLOW_FMT, cap->what,
          (deadline_ms + WPE_MS_PER_S - 1) / WPE_MS_PER_S);
 wpe_child_say_hook(line);
}

/**
 * wpe_capture_wait - Wait on the loop until the command ends or the deadline
 * comes; at the deadline, end it.
 * @cap: the command; @deadline_ms: as wpe_child_capture.
 *
 * A command that answers at once is waited for without a word; one that
 * takes longer is named on the status row with its deadline, so the user
 * reads why the editor does not take keys yet and for how long at most.
 * Esc, on a display that has it, ends the command at once.
 * Return: WPE_CAPTURE_ENDED, WPE_CAPTURE_CANCELLED or WPE_CAPTURE_TIMED_OUT.
 */
static int wpe_capture_wait(WpeCapture *cap, long deadline_ms)
{
 long first = deadline_ms;

 wpe_capture_now = cap;
 if (first > WPE_CAPTURE_SLOW_MS)
  first = WPE_CAPTURE_SLOW_MS;
 if (wpe_capture_serve_until(cap, first))
  return(wpe_capture_verdict(cap));
 if (deadline_ms > first)
 {
  wpe_capture_say_slow(cap, deadline_ms);
  if (wpe_capture_serve_until(cap, deadline_ms - first))
   return(wpe_capture_verdict(cap));
 }
 wpe_capture_kill(cap);
 return(WPE_CAPTURE_TIMED_OUT);
}

/**
 * wpe_capture_finish - Release the command's descriptors and record.
 * @cap: the command, ended or given up on.
 *
 * One given up on is forgotten, so the owner never calls back into a record
 * that is gone.
 */
static void wpe_capture_finish(WpeCapture *cap)
{
 if (!cap->ended)
  wpe_child_forget(&cap->child);
 if (cap->child.out_fd >= 0)
 {
  if (!cap->eof)
   wpe_fd_del(cap->child.out_fd);
  close(cap->child.out_fd);
 }
 if (cap->out && cap->max)
  cap->out[cap->len] = '\0';
}

/**
 * wpe_capture_run - Run one short command and wait for it on the loop.
 * @path: where its output goes as a file, or NULL to read it into @out.
 * Return: WPE_CAPTURE_*; the rest as wpe_child_capture.
 */
static int wpe_capture_run(const struct WpeStartPlan *plan, char *const argv[],
                           const char *path, char *out, size_t max, long deadline_ms,
                           int *status, const char *what, int deaf)
{
 WpeCapture cap, *outer = wpe_capture_now;
 WpeChildSpec spec;
 int result;

 memset(&cap, 0, sizeof cap);
 cap.out = out;
 cap.max = max;
 cap.deaf = deaf;
 cap.status = WPE_CAPTURE_STATUS_LOST;
 if (out && max)
  out[0] = '\0';
 wpe_capture_spec(&spec, plan, argv, path, what);
 cap.what = spec.what;
 if (wpe_capture_begin(&cap, &spec) < 0)
  return(WPE_CAPTURE_NOT_STARTED);
 result = wpe_capture_wait(&cap, deadline_ms);
 wpe_capture_finish(&cap);
 wpe_capture_now = outer;
 if (status)
  *status = cap.status;
 return(result);
}

/**
 * wpe_child_capture - Run a short command and read what it prints.
 * @argv: the program and its arguments, NULL-ended; no shell is involved;
 * @out: receives its output (errors too), always ended with a NUL;
 * @max: its size; @deadline_ms: the longest the command may take;
 * @status: receives its wait status (WPE_CAPTURE_STATUS_LOST when none);
 * @what: names the command on the status row ("the print command"), or NULL.
 *
 * For what the editor must know before it goes on (the version of the user's
 * Java, the system's font): a program that hangs costs the user the
 * deadline, never the editor.  The editor answers its other work and the
 * window's repaints while it waits, says what it waits for once the command
 * takes longer than a moment, and kills a command still running at the
 * deadline.  There is always a deadline: the keys typed meanwhile wait for
 * the command, so a command without one could hold the editor for ever.  On
 * X11 and Wayland, Esc ends it sooner and the status row says the wait was
 * stopped; the caller then shows no error, for the user chose it.
 * Return: WPE_CAPTURE_ENDED, WPE_CAPTURE_TIMED_OUT, WPE_CAPTURE_CANCELLED or
 * WPE_CAPTURE_NOT_STARTED.
 */
int wpe_child_capture(char *const argv[], char *out, size_t max,
                      long deadline_ms, int *status, const char *what)
{
 return(wpe_capture_run(NULL, argv, NULL, out, max, deadline_ms, status, what,
                        WPE_CAPTURE_ASKS_ESC));
}

/**
 * wpe_child_capture_plan - wpe_child_capture for a command a start plan
 * describes.
 * @plan: the plan, which the child's spec carries; the rest as there.
 * Return: as wpe_child_capture.
 */
int wpe_child_capture_plan(const struct WpeStartPlan *plan, char *const argv[], char *out,
                           size_t max, long deadline_ms, int *status, const char *what)
{
 return(wpe_capture_run(plan, argv, NULL, out, max, deadline_ms, status, what,
                        WPE_CAPTURE_ASKS_ESC));
}

/**
 * wpe_child_probe - Run a short command the user did not start and read
 * what it prints, without taking Esc.
 * @argv, @out, @max, @deadline_ms, @status, @what: as wpe_child_capture.
 *
 * For what the editor asks while it does something else the user did start
 * (the version of a Java before a Scala session, the desktop's font before
 * the window opens): an Esc typed then is meant for that work, so it stays
 * queued and the probe ends at its answer or its deadline only.
 * Return: WPE_CAPTURE_ENDED, WPE_CAPTURE_TIMED_OUT or WPE_CAPTURE_NOT_STARTED.
 */
int wpe_child_probe(char *const argv[], char *out, size_t max,
                    long deadline_ms, int *status, const char *what)
{
 return(wpe_capture_run(NULL, argv, NULL, out, max, deadline_ms, status, what,
                        WPE_CAPTURE_IGNORES_ESC));
}

/**
 * wpe_child_probe_plan - wpe_child_probe for a command a start plan describes.
 * @plan: the plan, which the child's spec carries; the rest as there.
 * Return: as wpe_child_probe.
 */
int wpe_child_probe_plan(const struct WpeStartPlan *plan, char *const argv[], char *out,
                         size_t max, long deadline_ms, int *status, const char *what)
{
 return(wpe_capture_run(plan, argv, NULL, out, max, deadline_ms, status, what,
                        WPE_CAPTURE_IGNORES_ESC));
}

/**
 * wpe_child_run_to_file - Run a command that writes its output into a file,
 * and wait for it on the loop.
 * @argv: as wpe_child_capture; @path: the file, created or emptied;
 * @deadline_ms, @status, @what: as there.
 *
 * Opening a compressed file or reading a manual page: the editor shows the
 * file or page when the command is done, and the user sees a failure
 * rather than a half-written file taken for the whole.
 * Return: WPE_CAPTURE_*.
 */
int wpe_child_run_to_file(char *const argv[], const char *path,
                          long deadline_ms, int *status, const char *what)
{
 return(wpe_capture_run(NULL, argv, path, NULL, 0, deadline_ms, status, what,
                        WPE_CAPTURE_ASKS_ESC));
}

/**
 * wpe_child_run_to_file_plan - wpe_child_run_to_file for a command a start
 * plan describes.
 * @plan: the plan, which the child's spec carries; the rest as there.
 * Return: as wpe_child_run_to_file.
 */
int wpe_child_run_to_file_plan(const struct WpeStartPlan *plan, char *const argv[],
                               const char *path, long deadline_ms, int *status,
                               const char *what)
{
 return(wpe_capture_run(plan, argv, path, NULL, 0, deadline_ms, status, what,
                        WPE_CAPTURE_ASKS_ESC));
}

/**
 * wpe_capture_succeeded - Did the command run and exit with status 0?
 * @result: what wpe_child_capture or wpe_child_run_to_file returned;
 * @status: the status it gave.
 */
int wpe_capture_succeeded(int result, int status)
{
 return(result == WPE_CAPTURE_ENDED && status != WPE_CAPTURE_STATUS_LOST &&
        WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
