/* child_state.h -- the one place a C test asks "is this process still alive".
 *
 * kill(pid, 0) answers "does the pid exist", not "is the process running":
 * an unreaped zombie exists, so on NetBSD a launcher that had ended was said
 * to be alive until something waited for it, while /proc says "gone" on Linux.
 * A test that wants to know whether a process has ended, or whether it has
 * been reaped, reads the system's own process table instead:
 *
 *	Linux		/proc/PID/stat: state field (Z and X: zombie), start time
 *	FreeBSD		sysctl KERN_PROC_PID, struct kinfo_proc: ki_stat (SZOMB),
 *			ki_start
 *	NetBSD		sysctl KERN_PROC2, struct kinfo_proc2: p_realstat
 *			(SZOMB, SDYING, SDEAD; p_stat is the LWP's state and
 *			reads "asleep" for a dying process), p_ustart_sec and
 *			p_ustart_usec
 *	OpenBSD		sysctl KERN_PROC, struct kinfo_proc: p_stat (a zombie is
 *			SDEAD; SZOMB is accepted too), p_ustart_sec and
 *			p_ustart_usec
 *	any other	kill(pid, 0) alone, the answer CHILD_UNKNOWN: the system
 *			gives no state this header has measured, and a test
 *			that needs one says SKIP by the system's name
 *
 * Measured (test_child_state on each system, and a probe that printed the
 * table): a pid that is gone gives no row (sysctl succeeds with length 0 on
 * NetBSD and OpenBSD, fails with ESRCH on FreeBSD); NetBSD 10.0 and OpenBSD
 * 7.4 give a zombie the start time 0, so a start time is compared only for a
 * running process; OpenBSD 7.4 shows a zombie as SDEAD (6); NetBSD's p_realstat
 * shows it as SZOMB (5) (and a dying process as SDYING, which p_stat hides).
 *
 * This header only READS.  It never calls wait, so it never takes a child's
 * exit status from the owner that is meant to reap it: a row that asks "the
 * launcher is gone and reaped" would otherwise pass on a product that leaked
 * a zombie.  The processes it asks about are often not the test's children
 * (the members of a group, the adapter of a debugger), so it does not use the
 * parent's pid either.
 *
 * Two questions, because the rows ask two:
 *
 *	child_ended	has the process stopped running (a zombie, or gone)?
 *	child_reaped	is it gone from the process table?
 *
 * A pid can be reused.  A row that wants to be sure takes a ChildWatch at
 * first sight (child_watch records the start time); a pid whose start time
 * has changed is another process, CHILD_NOT_OURS, and the one that was watched
 * has ended and been reaped.  The functions that take a bare pid have no
 * earlier sight and cannot tell.
 *
 * tests/check-child-state.sh fails a test that asks kill(pid, 0) or
 * killpg(pgid, 0) about a process outside this header.
 *
 * Static inline, libc only, like testdir.h.
 */
#ifndef TEST_CHILD_STATE_H
#define TEST_CHILD_STATE_H

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__FreeBSD__)
# include <sys/param.h>
# include <sys/time.h>
# include <sys/queue.h>
# include <sys/sysctl.h>
# include <sys/user.h>
# include <sys/proc.h>
#elif defined(__NetBSD__) || defined(__OpenBSD__)
# include <sys/param.h>
# include <sys/time.h>
# include <sys/sysctl.h>
# include <sys/proc.h>
#endif

/** What the process table says about one pid. */
typedef enum
{
 CHILD_RUNNING,     /* a process with this pid is there and not ended */
 CHILD_ZOMBIE,      /* it has ended and nobody has waited for it yet */
 CHILD_GONE,        /* no process has this pid */
 CHILD_NOT_OURS,    /* the pid is now another process (its start time changed) */
 CHILD_UNKNOWN      /* this system gives no process state: only kill(pid, 0) was asked */
} ChildState;

/** A process, as first seen: its pid and the start time that tells it from a later process with the same pid. */
typedef struct
{
 pid_t pid;
 long long start;      /* the system's start time of the process, in the unit the reader gives; 0 when not known */
 int has_start;        /* start is a real reading */
} ChildWatch;

/** What one read of the process table gave. */
typedef struct
{
 ChildState state;
 long long start;
 int has_start;
 int err;              /* errno of the read when it failed, else 0 */
} ChildRead;

#define CHILD_USEC_PER_S 1000000LL

/** child_state_name - A state in words, for the lines a row prints. */
static inline const char *child_state_name(ChildState s)
{
 if (s == CHILD_RUNNING)
  return("running");
 if (s == CHILD_ZOMBIE)
  return("zombie");
 if (s == CHILD_GONE)
  return("gone");
 if (s == CHILD_NOT_OURS)
  return("another process with this pid");
 return("unknown (only kill(pid, 0) was asked)");
}

/** child_read_unknown - The answer of a system whose process table this header has not measured: kill(pid, 0) alone. */
static inline void child_read_unknown(pid_t pid, ChildRead *r)
{
 r->state = CHILD_UNKNOWN;
 if (kill(pid, 0) != 0)
 {
  r->err = errno;
  if (errno == ESRCH)
   r->state = CHILD_GONE;
 }
}

#if defined(__linux__)

/**
 * child_read_proc_stat - Read the state and start time out of the text of /proc/PID/stat.
 * @text: the line.  @r: receives them.
 *
 * The command name is in parentheses and may hold a parenthesis, so the
 * fields are counted from the LAST one: the state is the first field after it
 * and the start time is the twentieth.
 * Return: 1 when the line had a state, else 0.
 */
static inline int child_read_proc_stat(const char *text, ChildRead *r)
{
 const char *close = strrchr(text, ')');
 char state = '?';
 unsigned long long start = 0;
 int got;

 if (close == NULL)
  return(0);
 got = sscanf(close + 1, " %c %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %llu",
              &state, &start);
 if (got < 1)
  return(0);
 r->state = (state == 'Z' || state == 'X') ? CHILD_ZOMBIE : CHILD_RUNNING;
 if (got == 2)
 {
  r->start = (long long)start;
  r->has_start = 1;
 }
 return(1);
}

/** child_read_system - Read one pid from /proc (Linux). */
static inline void child_read_system(pid_t pid, ChildRead *r)
{
 char path[64], line[1024];
 FILE *f;

 if (access("/proc/self/stat", R_OK) != 0)
 {
  child_read_unknown(pid, r);
  return;
 }
 snprintf(path, sizeof path, "/proc/%ld/stat", (long)pid);
 f = fopen(path, "r");
 if (f == NULL)
 {
  r->err = errno;
  r->state = (errno == ENOENT || errno == ESRCH) ? CHILD_GONE : CHILD_UNKNOWN;
  return;
 }
 if (fgets(line, sizeof line, f) == NULL || !child_read_proc_stat(line, r))
 {
  r->err = EIO;
  r->state = CHILD_GONE;      /* it went between open and read */
 }
 fclose(f);
}

#elif defined(__FreeBSD__)

/** child_read_system - Read one pid with sysctl KERN_PROC_PID (FreeBSD). */
static inline void child_read_system(pid_t pid, ChildRead *r)
{
 int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)pid };
 struct kinfo_proc kp;
 size_t len = sizeof kp;

 memset(&kp, 0, sizeof kp);
 if (sysctl(mib, 4, &kp, &len, NULL, 0) != 0)
 {
  r->err = errno;
  r->state = (errno == ESRCH) ? CHILD_GONE : CHILD_UNKNOWN;
  return;
 }
 if (len == 0)
 {
  r->state = CHILD_GONE;
  return;
 }
 r->state = (kp.ki_stat == SZOMB) ? CHILD_ZOMBIE : CHILD_RUNNING;
 r->start = (long long)kp.ki_start.tv_sec * CHILD_USEC_PER_S + kp.ki_start.tv_usec;
 r->has_start = 1;
}

#elif defined(__NetBSD__)

/** child_read_system - Read one pid with sysctl KERN_PROC2 (NetBSD). */
static inline void child_read_system(pid_t pid, ChildRead *r)
{
 int mib[6] = { CTL_KERN, KERN_PROC2, KERN_PROC_PID, (int)pid, (int)sizeof(struct kinfo_proc2), 1 };
 struct kinfo_proc2 kp;
 size_t len = sizeof kp;

 memset(&kp, 0, sizeof kp);
 if (sysctl(mib, 6, &kp, &len, NULL, 0) != 0)
 {
  r->err = errno;
  r->state = (errno == ESRCH) ? CHILD_GONE : CHILD_UNKNOWN;
  return;
 }
 if (len == 0)
 {
  r->state = CHILD_GONE;
  return;
 }
 /* p_stat is the LWP's state (a process that is dying still reads 3, asleep),
    p_realstat is the process's own: the kernel's set for "ended" is SZOMB,
    SDYING and SDEAD. */
 r->state = (kp.p_realstat == SZOMB || kp.p_realstat == SDEAD || kp.p_realstat == SDYING)
            ? CHILD_ZOMBIE : CHILD_RUNNING;
 r->start = (long long)kp.p_ustart_sec * CHILD_USEC_PER_S + kp.p_ustart_usec;
 r->has_start = 1;
}

#elif defined(__OpenBSD__)

/** child_read_system - Read one pid with sysctl KERN_PROC (OpenBSD). */
static inline void child_read_system(pid_t pid, ChildRead *r)
{
 int mib[6] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)pid, (int)sizeof(struct kinfo_proc), 1 };
 struct kinfo_proc kp;
 size_t len = sizeof kp;

 memset(&kp, 0, sizeof kp);
 if (sysctl(mib, 6, &kp, &len, NULL, 0) != 0)
 {
  r->err = errno;
  r->state = (errno == ESRCH) ? CHILD_GONE : CHILD_UNKNOWN;
  return;
 }
 if (len == 0)
 {
  r->state = CHILD_GONE;
  return;
 }
 r->state = (kp.p_stat == SZOMB || kp.p_stat == SDEAD) ? CHILD_ZOMBIE : CHILD_RUNNING;
 r->start = (long long)kp.p_ustart_sec * CHILD_USEC_PER_S + kp.p_ustart_usec;
 r->has_start = 1;
}

#else

/** child_read_system - A system this header has not measured: kill(pid, 0) alone. */
static inline void child_read_system(pid_t pid, ChildRead *r)
{
 child_read_unknown(pid, r);
}

#endif

/**
 * child_read - Read the state of a pid from the system, once.
 * @pid: the process.  @r: receives the state, its start time and the error of a failed read.
 *
 * Never waits, never signals (but on a system with no reader, where it is
 * kill(pid, 0)); the state it reports is not changed by being read.
 */
static inline void child_read(pid_t pid, ChildRead *r)
{
 memset(r, 0, sizeof *r);
 if (pid <= 0)
 {
  r->state = CHILD_GONE;
  return;
 }
 child_read_system(pid, r);
}

/**
 * child_system_reads_state - Does this system's reader tell a zombie from a running process?
 *
 * 0 where only kill(pid, 0) is asked (CHILD_UNKNOWN): a row that needs the
 * difference says SKIP by the system's name there.
 */
static inline int child_system_reads_state(void)
{
 ChildRead r;

 child_read(getpid(), &r);
 return(r.state != CHILD_UNKNOWN);
}

/**
 * child_watch - Take a process at first sight: its pid and its start time.
 * @pid: the process.
 *
 * Call it as soon as the pid is known (right after the fork, or when the pid
 * is read from the process that printed it).  A later process that gets the
 * same pid has another start time and is told apart.
 */
static inline ChildWatch child_watch(pid_t pid)
{
 ChildWatch w;
 ChildRead r;

 child_read(pid, &r);
 w.pid = pid;
 w.start = r.start;
 w.has_start = r.has_start;
 return(w);
}

/**
 * child_state - What the process table says of a watched process now.
 * @w: the process as first seen.  @err: receives the errno of a failed read, or NULL.
 *
 * Return: CHILD_RUNNING, CHILD_ZOMBIE, CHILD_GONE, CHILD_NOT_OURS when the pid
 * belongs to a running process that started at another time, or CHILD_UNKNOWN.
 */
static inline ChildState child_state(const ChildWatch *w, int *err)
{
 ChildRead r;

 child_read(w->pid, &r);
 if (err != NULL)
  *err = r.err;
 /* Only a running process is compared by its start time.  NetBSD gives a zombie
    a start time of 0 (measured on NetBSD 10.0), and no other process can have
    become a zombie under a reused pid within a row. */
 if (r.state == CHILD_RUNNING && w->has_start && r.has_start && w->start != r.start)
  return(CHILD_NOT_OURS);
 return(r.state);
}

/**
 * child_ended - Has the watched process stopped running?
 * @w: the process as first seen.
 *
 * True for a zombie (ended, not yet reaped), for a process that is gone, and
 * for a pid that now belongs to another process.  On a system with no reader
 * (CHILD_UNKNOWN) it is the old answer: gone when kill(pid, 0) says ESRCH,
 * which a zombie does not.
 */
static inline int child_ended(const ChildWatch *w)
{
 ChildState s = child_state(w, NULL);

 return(s == CHILD_ZOMBIE || s == CHILD_GONE || s == CHILD_NOT_OURS);
}

/**
 * child_reaped - Is the watched process gone from the process table?
 * @w: the process as first seen.
 *
 * True only when nothing holds the pid (or another process does).  A zombie
 * is not reaped: it is what a product that never waits leaves behind.
 */
static inline int child_reaped(const ChildWatch *w)
{
 ChildState s = child_state(w, NULL);

 return(s == CHILD_GONE || s == CHILD_NOT_OURS);
}

/** child_pid_ended - child_ended for a pid with no earlier sight: a reused pid is not told apart. */
static inline int child_pid_ended(pid_t pid)
{
 ChildWatch w;

 w.pid = pid;
 w.start = 0;
 w.has_start = 0;
 return(child_ended(&w));
}

/** child_pid_reaped - child_reaped for a pid with no earlier sight: a reused pid is not told apart. */
static inline int child_pid_reaped(pid_t pid)
{
 ChildWatch w;

 w.pid = pid;
 w.start = 0;
 w.has_start = 0;
 return(child_reaped(&w));
}

/**
 * child_describe - The watched process in words, for the line of a row that fails.
 * @w: the process.  @out, @n: receive "pid=N state=S errno=E".
 *
 * A BSD run is the only evidence there is of what its process table said, so
 * the line of a failed check names the pid, the answer and the errno.
 */
static inline void child_describe(const ChildWatch *w, char *out, size_t n)
{
 int err = 0;
 ChildState s = child_state(w, &err);

 snprintf(out, n, "pid=%ld state=%s errno=%d", (long)w->pid, child_state_name(s), err);
}

#endif /* TEST_CHILD_STATE_H */
