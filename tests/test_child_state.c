/* test_child_state.c -- the canary of tests/child_state.h, the one reader of
 * "is this process still alive".
 *
 * On every system it runs, a child is made to be running, then a zombie, then
 * gone, and the reader must say so in that order.  The first run on a system
 * is what proves the names and constants that system's reader uses (Linux
 * /proc, FreeBSD KERN_PROC, NetBSD KERN_PROC2, OpenBSD KERN_PROC): a wrong
 * zombie state turns "ended but not reaped" into "running" and the row below
 * fails with the line that says what the table answered.
 *
 * The reader must also never reap: after it has been asked about a zombie,
 * waitpid still returns that child's status.  A system with no reader
 * (CHILD_UNKNOWN) says SKIP for the rows that need one, with its name.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>

#include "test_watchdog.h"
#include "child_state.h"

#define CASE_SECONDS      30
#define ENDS_WITHIN_MS    5000   /* a child that exits at once is seen ended within this */
#define EXIT_CODE         7
#define SLEEP_FOREVER_S   60
#define ASKS_BEFORE_REAP  3      /* the reader is asked this often about a zombie before it is waited for */
#define PID_ONE           1      /* a process that is not ours, and runs */

static int failures;
static int skips;

static void check(int ok, const char *what)
{
 if (ok)
  return;
 printf("FAIL: %s\n", what);
 failures++;
}

static void skip_by_system(const char *what)
{
 struct utsname u;

 if (uname(&u) != 0)
  snprintf(u.sysname, sizeof u.sysname, "this system");
 printf("SKIP: %s: %s gives no process state this reader has measured (only kill(pid, 0))\n", what, u.sysname);
 skips++;
}

/** start_sleeper - A child that does nothing until it is killed. */
static pid_t start_sleeper(void)
{
 pid_t pid = fork();

 if (pid == 0)
 {
  sleep(SLEEP_FOREVER_S);
  _exit(0);
 }
 return pid;
}

/** start_quitter - A child that exits at once with a known status. */
static pid_t start_quitter(void)
{
 pid_t pid = fork();

 if (pid == 0)
  _exit(EXIT_CODE);
 return pid;
}

/**
 * wait_until_not_running - Ask the reader until the watched process is not running, at most @ms.
 * Observed, never a fixed sleep: the row goes on the moment the table says so.
 * Return: the state at the end.
 */
static ChildState wait_until_not_running(const ChildWatch *w, long ms)
{
 long left = ms;
 ChildState s = child_state(w, NULL);

 while (s == CHILD_RUNNING && left > 0)
 {
  test_sleep_ms(TEST_WAIT_STEP_MS);
  left -= TEST_WAIT_STEP_MS;
  s = child_state(w, NULL);
 }
 return(s);
}

/**
 * is_running - Does a state say "running"?  On a system with no reader the
 * only answer for a process that exists is CHILD_UNKNOWN, which then says it.
 */
static int is_running(ChildState s)
{
 if (s == CHILD_RUNNING)
  return(1);
 return(s == CHILD_UNKNOWN && !child_system_reads_state());
}

/** A process that sleeps is running: not ended, not reaped. */
static void test_a_sleeping_child_is_running(void)
{
 pid_t pid = start_sleeper();
 ChildWatch w = child_watch(pid);
 int st;

 check(is_running(child_state(&w, NULL)), "a sleeping child is running");
 check(!child_ended(&w), "a sleeping child has not ended");
 check(!child_reaped(&w), "a sleeping child is not reaped");
 kill(pid, SIGKILL);
 waitpid(pid, &st, 0);
}

/**
 * A child that has ended and has not been waited for is a zombie: ended, not
 * reaped.  Asked several times, the reader does not take its status: waitpid
 * still returns it.  After the wait the pid is gone.
 */
static void test_an_ended_child_is_a_zombie_until_it_is_waited_for(void)
{
 pid_t pid = start_quitter();
 ChildWatch w = child_watch(pid);
 ChildState s = wait_until_not_running(&w, ENDS_WITHIN_MS);
 int st = 0, i;

 if (!child_system_reads_state())
  skip_by_system("an ended child is a zombie until it is waited for");
 else
 {
  check(s == CHILD_ZOMBIE, "a child that exited and was not waited for is a zombie");
  check(child_ended(&w), "a zombie has ended");
  check(!child_reaped(&w), "a zombie is not reaped");
 }
 for (i = 0; i < ASKS_BEFORE_REAP; i++)
  (void)child_state(&w, NULL);
 check(waitpid(pid, &st, 0) == pid, "the reader never took the zombie's status: waitpid still returns it");
 check(WIFEXITED(st) && WEXITSTATUS(st) == EXIT_CODE, "the status is the child's own");
 check(child_state(&w, NULL) == CHILD_GONE || child_state(&w, NULL) == CHILD_UNKNOWN,
       "after the wait nothing holds the pid");
 check(child_ended(&w), "a reaped child has ended");
 check(child_reaped(&w), "a reaped child is reaped");
}

/** A killed child is running, then a zombie, then gone, in that order. */
static void test_a_killed_child_goes_running_zombie_gone(void)
{
 pid_t pid = start_sleeper();
 ChildWatch w = child_watch(pid);
 int st;

 check(is_running(child_state(&w, NULL)), "before the kill it runs");
 kill(pid, SIGKILL);
 if (!child_system_reads_state())
  skip_by_system("a killed child is a zombie before it is waited for");
 else
  check(wait_until_not_running(&w, ENDS_WITHIN_MS) == CHILD_ZOMBIE, "after the kill it is a zombie");
 waitpid(pid, &st, 0);
 check(child_reaped(&w), "after the wait it is gone");
}

/** A process that is not ours (pid 1) is running; the reader asks about it without being its parent. */
static void test_a_process_that_is_not_ours_is_running(void)
{
 ChildWatch w = child_watch(PID_ONE);
 ChildState s = child_state(&w, NULL);

 check(is_running(s), "pid 1 is running, though it is nobody's child here");
 check(!child_reaped(&w), "pid 1 is not reaped");
}

/**
 * A pid reused by another process is told by its start time: the process that
 * was watched has ended and been reaped, and the new one is not it.
 */
static void test_a_reused_pid_is_another_process(void)
{
 pid_t pid = start_sleeper();
 ChildWatch w = child_watch(pid);
 int st;

 if (!w.has_start)
 {
  skip_by_system("a reused pid is told by its start time");
 }
 else
 {
  w.start += 1;     /* the start time the earlier process had */
  check(child_state(&w, NULL) == CHILD_NOT_OURS, "a pid whose start time changed is another process");
  check(child_ended(&w), "the process that was watched has ended");
  check(child_reaped(&w), "and is reaped");
 }
 kill(pid, SIGKILL);
 waitpid(pid, &st, 0);
}

/** A pid that cannot be a process is gone. */
static void test_a_pid_that_is_not_a_process_is_gone(void)
{
 check(child_pid_reaped(-1), "pid -1 is gone");
 check(child_pid_ended(0), "pid 0 is gone");
}

int main(void)
{
 test_watchdog_case("test_child_state", CASE_SECONDS);
 test_a_sleeping_child_is_running();
 test_an_ended_child_is_a_zombie_until_it_is_waited_for();
 test_a_killed_child_goes_running_zombie_gone();
 test_a_process_that_is_not_ours_is_running();
 test_a_reused_pid_is_another_process();
 test_a_pid_that_is_not_a_process_is_gone();
 test_watchdog_disarm();
 printf("test_child_state: %d failure(s), %d skip(s)\n", failures, skips);
 return(failures > 0 ? 1 : 0);
}
