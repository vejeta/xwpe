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
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#if defined(HAVE_PTY_H)
# include <pty.h>          /* glibc: openpty */
#elif defined(HAVE_UTIL_H)
# include <util.h>         /* macOS, *BSD */
#elif defined(HAVE_LIBUTIL_H)
# include <libutil.h>      /* some BSDs */
#endif

#include "we_child.h"
#include "we_childsig.h"
#include "we_fdloop.h"

#define WPE_CHILD_EXEC_FAILED 1          /* a command that could not run */
#define WPE_CHILD_SAID_FAILED 127        /* one that said why, as a shell's */
#define WPE_CHILD_SAY_MAX     (PATH_MAX + 128)
#define WPE_CHILD_TTY_FAILED_FMT  "cannot give %s a terminal: %s\r\n"
#define WPE_CHILD_DIR_FAILED_FMT  "cannot enter %s: %s\r\n"
#define WPE_CHILD_RUN_FAILED_FMT  "cannot run %s: %s\r\n"
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
} WpeChildSlot;

static WpeChildSlot wpe_child_table[WPE_CHILD_MAX];

/* A group whose leader ended while the ladder waited out its grace: the
   members that ignored the polite signal still get SIGKILL when the grace
   is over.  The number cannot name another group meanwhile: a PID is not
   given out again while a group of that number has members. */
typedef struct {
 int pgid;                       /* 0 while the record is free */
 long due_ms;                    /* when its grace is over */
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
  if (wpe_child_lingers[i].pgid > 0)
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
static void wpe_child_linger_kill(WpeChildLinger *l)
{
 kill(-l->pgid, SIGKILL);
 l->pgid = 0;
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

/**
 * wpe_child_deliver - Call back the owner of an ended child.
 * @w: a slot whose child the handler reaped.
 *
 * The slot is freed before the callback runs, so the callback may start
 * another child, or open a dialog whose own loop polls again, and find the
 * table consistent.
 */
static void wpe_child_deliver(WpeChildSlot *w)
{
 WpeChildEndCallback cb = w->callback;
 void *data = w->data;
 WpeChild *c = w->child;
 WpeChildEndedFn ended = w->child_ended;
 void *child_data = w->child_data;
 int pid = w->pid, status = w->status;

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

 if (!w)
  w = wpe_child_free_slot();
 if (!w || wpe_child_wake_rd == WPE_CHILD_NO_FD)
  return(NULL);
 if (wpe_fd_add(wpe_child_wake_rd, POLLIN, wpe_child_on_wake, NULL) != 0)
  return(NULL);
 w->ended = 0;
 w->status = 0;
 w->group = 0;
 w->callback = NULL;
 w->data = NULL;
 w->child = NULL;
 w->child_ended = NULL;
 w->child_data = NULL;
 w->what = NULL;
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

/* The two ends of a child's pty, WPE_CHILD_NO_FD where it has none. */
typedef struct {
 int master;                     /* the editor's: what the program writes */
 int slave;                      /* the child's terminal */
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
 * wpe_child_pty_open - The pty the spec asks for.
 * @spec: the child; @t: receives it, both ends WPE_CHILD_NO_FD when none
 * is wanted.
 *
 * With pty_raw the terminal does no output processing, so what the program
 * prints reaches the editor as it wrote it, with no carriage return added
 * to each line end.
 * Return: 0, or -1 (and nothing open) when no pty could be had.
 */
static int wpe_child_pty_open(const WpeChildSpec *spec, WpeChildPty *t)
{
 struct termios tio;

 t->master = t->slave = WPE_CHILD_NO_FD;
 if (spec->io != WPE_CHILD_IO_PTY)
  return(0);
 if (openpty(&t->master, &t->slave, NULL, NULL, NULL) != 0)
 {
  t->master = t->slave = WPE_CHILD_NO_FD;
  return(-1);
 }
 if (spec->pty_raw && tcgetattr(t->master, &tio) == 0)
 {
  cfmakeraw(&tio);
  tcsetattr(t->master, TCSANOW, &tio);
 }
 return(0);
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
 *master = *slave = WPE_CHILD_NO_FD;
 if (openpty(master, slave, NULL, NULL, NULL) != 0)
 {
  *master = *slave = WPE_CHILD_NO_FD;
  return(-1);
 }
 if (ttyname_r(*slave, name, n) != 0)
  name[0] = '\0';
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
 if (!spec->stdout_to_null && pipe(p[WPE_CHILD_P_OUT]) < 0)
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

/** wpe_child_print_argv - In the child: one line of @prefix and the words. */
static void wpe_child_print_argv(FILE *fp, const char *prefix,
                                 char *const *argv)
{
 int i;

 if (prefix && prefix[0])
  fprintf(fp, "%s ", prefix);
 for (i = 0; argv[i]; i++)
  fprintf(fp, "%s ", argv[i]);
 fprintf(fp, "\n");
 fflush(fp);
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

/**
 * wpe_child_io_pipes - In the child: the pipes on 0, 1 and 2 as @spec says.
 * @spec: the child; @p: the pipes.
 */
static void wpe_child_io_pipes(const WpeChildSpec *spec, int (*p)[2])
{
 if (spec->stdin_null)
  wpe_child_dup_null(WPE_CHILD_FD_IN, O_RDONLY);
 else
  dup2(p[WPE_CHILD_P_IN][WPE_CHILD_PIPE_RD], WPE_CHILD_FD_IN);
 if (spec->stdout_to_null)
  wpe_child_dup_null(STDOUT_FILENO, O_WRONLY);
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
 * wpe_child_say_failed - In the child: name what could not be done, and
 * why, on its stderr, then end it as a shell ends a command it cannot run.
 * @spec: the child (nothing is said unless say_failed); @fmt: the sentence,
 * with the subject and the reason as its two %s; @subject: what failed.
 *
 * The user reads the line where the program's own output goes (Messages,
 * for jdb's program), instead of a session that never starts unexplained.
 */
static void wpe_child_say_failed(const WpeChildSpec *spec, const char *fmt,
                                 const char *subject)
{
 char line[WPE_CHILD_SAY_MAX];
 int err = errno, len;

 if (!spec->say_failed)
  _exit(WPE_CHILD_EXEC_FAILED);
 len = snprintf(line, sizeof line, fmt, subject, strerror(err));
 if (len > 0 && write(STDERR_FILENO, line, strlen(line)) < 0)
  _exit(WPE_CHILD_SAID_FAILED);
 _exit(WPE_CHILD_SAID_FAILED);
}

static void wpe_child_io_pty(WpeChildPty *t);   /* (below) */

/**
 * wpe_child_io_tty - In the child: the terminal the spec names on 0, 1 and
 * 2.  A pty slave of the editor's (tty_fd) becomes its controlling terminal,
 * so the program reads what the user types and a hangup ends it; a terminal
 * named by its path (the editor's own) is only written and read.
 * @spec: the child.
 */
static void wpe_child_io_tty(const WpeChildSpec *spec)
{
 WpeChildPty t;
 int fd;

 if (spec->tty_fd != WPE_CHILD_NO_FD)
 {
  t.master = WPE_CHILD_NO_FD;
  t.slave = spec->tty_fd;
  if (ioctl(t.slave, TIOCSCTTY, 0) != 0)
   wpe_child_say_failed(spec, WPE_CHILD_TTY_FAILED_FMT, spec->what);
  wpe_child_io_pty(&t);
  return;
 }
 fd = open(spec->tty_path, O_RDWR | O_NOCTTY);
 if (fd < 0)
  wpe_child_say_failed(spec, WPE_CHILD_TTY_FAILED_FMT, spec->what);
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
 * @spec: the child; @p: the pipes; @t: the pty.
 */
static void wpe_child_io(const WpeChildSpec *spec, int (*p)[2], WpeChildPty *t)
{
 if (spec->io == WPE_CHILD_IO_PTY)
 {
  close(t->master);
  wpe_child_io_pty(t);
 }
 else if (spec->io == WPE_CHILD_IO_TTY_PATH)
  wpe_child_io_tty(spec);
 else if (wpe_child_io_has_side_pty(spec->io))
  wpe_child_side_pty(spec);
 if (wpe_child_io_has_pipes(spec->io))
  wpe_child_io_pipes(spec, p);
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

/**
 * wpe_child_io_pty - In the child, in a session of its own: the pty
 * becomes its controlling terminal and its 0, 1 and 2.
 * @t: the pty; the slave is closed here once on 0, 1 and 2.
 *
 * dlv runs its program in the foreground of that terminal, which a pipe
 * cannot be, and the program's output then reaches the editor's master.
 */
static void wpe_child_io_pty(WpeChildPty *t)
{
 ioctl(t->slave, TIOCSCTTY, 0);
 dup2(t->slave, WPE_CHILD_FD_IN);
 dup2(t->slave, STDOUT_FILENO);
 dup2(t->slave, STDERR_FILENO);
 if (t->slave > STDERR_FILENO)
  close(t->slave);
 t->slave = WPE_CHILD_NO_FD;
}

/**
 * wpe_child_exec - In the child: set it up as @spec says and run it.
 * @spec: the child; @p: the pipes; @t: the pty; @editor: the editor's
 * process ID, read before the fork.
 *
 * Does not return.  The program gets the signal state a shell would give
 * it, its own group or session, and only the descriptors 0, 1 and 2.  It
 * never holds the master of its own pty: the hangup that ends it when the
 * editor dies comes only once every master is closed.  The directory is
 * entered once 0, 1 and 2 are set, so a directory that is gone is named
 * where the program's output goes.
 */
static void wpe_child_exec(const WpeChildSpec *spec, int (*p)[2],
                           WpeChildPty *t, pid_t editor)
{
 wpe_child_default_signals();
 wpe_child_take_group(spec);
 if (spec->die_with_editor && wpe_child_die_with_editor(editor) != 0)
  _exit(WPE_CHILD_EXEC_FAILED);
 wpe_child_io(spec, p, t);
 if (spec->cwd && chdir(spec->cwd) != 0)
  wpe_child_say_failed(spec, WPE_CHILD_DIR_FAILED_FMT, spec->cwd);
 if (spec->echo_argv)
  wpe_child_print_argv(stderr, "", spec->argv);
 execvp(spec->argv[0], spec->argv);
 if (spec->exec_failed)
  wpe_child_print_argv(stderr, spec->exec_failed, spec->argv);
 wpe_child_say_failed(spec, WPE_CHILD_RUN_FAILED_FMT, spec->argv[0]);
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
 WpeChildSlot *w;
 sigset_t saved;
 const pid_t editor = getpid();
 pid_t pid;

 wpe_child_reset(c);
 WpeChildSignalBlock(&saved);
 if (!wpe_child_room())
  return(wpe_child_refuse(spec, &saved));
 if (wpe_child_pty_open(spec, &t) < 0)
  return(wpe_child_start_failed(&saved));
 if (wpe_child_pipes_open(spec, p) < 0)
 {
  wpe_child_pty_close(&t);
  return(wpe_child_start_failed(&saved));
 }
 pid = fork();
 if (pid == 0)
  wpe_child_exec(spec, p, &t, editor);
 if (pid < 0)
 {
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
  kill(-pid, SIGKILL);
  kill(pid, SIGKILL);
  wpe_child_drop_ends(c);
  return(wpe_child_refuse(spec, &saved));
 }
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
 wpe_child_signal(c, SIGKILL);
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
  wpe_child_signal(c, polite_sig);
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
