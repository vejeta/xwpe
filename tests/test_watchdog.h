/* test_watchdog.h -- the one hang guard of the C tests.
 *
 * A test that waits for a child, a socket or a signal must fail with the
 * name of what it waited for, within a known time, and never sit until a
 * CI step limit kills it.  Every C test that starts a child includes this
 * header and arms the watchdog; a case that can wait re-arms it with its
 * own name, so the failure line reads
 *
 *	FAIL: test_close_kills_an_adapter_that_never_answers did not finish within 60 s
 *
 * Static inline, libc only, so it links into a test and into the fake
 * programs a test re-executes.
 *
 * XWPE_TEST_WAIT_SCALE (a float, default 1) multiplies every time given to
 * the watchdog and to test_wait_child, as it does for the
 * pyte suite: slow machines (the macOS runners, the autopkgtest testbed)
 * set 3.  Times a test asserts as a product bound (the Quit bounds, the
 * ladder's constants) are the product's and are never scaled here.
 */
#ifndef TEST_WATCHDOG_H
#define TEST_WATCHDOG_H

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#define TEST_WATCHDOG_DEFAULT_S 60
#define TEST_WATCHDOG_LINE_MAX  320
#define TEST_WAIT_STEP_MS       10

static char test_watchdog_line[TEST_WATCHDOG_LINE_MAX];
static size_t test_watchdog_len;
static char test_case_name[TEST_WATCHDOG_LINE_MAX / 2];
static int  test_case_secs;

/** test_wait_scale - The factor XWPE_TEST_WAIT_SCALE asks for (a float);
    1 when unset, not a number, or not above zero. */
static inline double test_wait_scale(void)
{
	const char *e = getenv("XWPE_TEST_WAIT_SCALE");
	char *end = NULL;
	double v;

	if (!e || !*e)
		return 1.0;
	v = strtod(e, &end);
	if (end == e || !(v > 0.0) || v > 1000.0)
		return 1.0;
	return v;
}

/** test_scaled_ms - @ms times the scale, at least 1. */
static inline long test_scaled_ms(long ms)
{
	double v = (double)ms * test_wait_scale();

	return v < 1.0 ? 1L : (long)v;
}

/** test_watchdog_fire - SIGALRM: say what did not finish, then leave.
    Async-signal-safe: one write of a line built when the alarm was armed. */
static inline void test_watchdog_fire(int sig)
{
	(void)sig;
	if (write(STDOUT_FILENO, test_watchdog_line, test_watchdog_len) < 0) {
		/* nothing more can be said */
	}
	_exit(1);
}

/** test_watchdog_arm - From now on, if the test has not disarmed or re-armed
    within @secs (scaled), print "FAIL: <what> did not finish within N s" and
    _exit(1).  Arming again replaces the previous one.  Earlier output is
    flushed first, so a FAIL line already printed survives the _exit. */
static inline void test_watchdog_arm(const char *what, int secs)
{
	static int first = 1;
	struct sigaction sa;
	long scaled = (long)((double)secs * test_wait_scale() + 0.999);
	int n;

	/* The previous alarm must not fire while its line is being rewritten:
	   the handler would write half of the new line. */
	alarm(0);
	if (scaled < 1)
		scaled = 1;
	if (first) {
		/* Line-buffered, so a FAIL line a case printed before it hung
		   is already out when the alarm's _exit comes (_exit does not
		   flush stdio).  Done at the first arm, before the test's own
		   output where it can be. */
		setvbuf(stdout, NULL, _IOLBF, 0);
		first = 0;
	}
	fflush(stdout);
	n = snprintf(test_watchdog_line, sizeof test_watchdog_line,
	             "FAIL: %s did not finish within %ld s\n",
	             what ? what : "the test", scaled);
	test_watchdog_len = (n < 0) ? 0 : (size_t)n;
	if (test_watchdog_len >= sizeof test_watchdog_line)
		test_watchdog_len = sizeof test_watchdog_line - 1;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = test_watchdog_fire;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGALRM, &sa, NULL);
	alarm((unsigned)scaled);
}

/** test_watchdog_case - Arm for a whole test case and remember it as the
    case: a step inside the case that arms a shorter guard of its own
    (test_watchdog_arm) goes back to the case's with test_watchdog_rearm_case. */
static inline void test_watchdog_case(const char *name, int secs)
{
	snprintf(test_case_name, sizeof test_case_name, "%s", name);
	test_case_secs = secs;
	test_watchdog_arm(test_case_name, test_case_secs);
}

/** test_watchdog_rearm_case - Arm again with the case's name and time. */
static inline void test_watchdog_rearm_case(void)
{
	test_watchdog_arm(test_case_name, test_case_secs);
}

/** test_lifetime_arm - For a fake child (an adapter, a server, a holder): end
    this process by SIGALRM, quietly, after @secs (scaled).  A fake's end by
    its lifetime is not a failure and its stdout may be the pipe under test,
    so nothing is printed; the test that started it has its own watchdog. */
static inline void test_lifetime_arm(int secs)
{
	long scaled = (long)((double)secs * test_wait_scale() + 0.999);

	signal(SIGALRM, SIG_DFL);
	alarm((unsigned)(scaled < 1 ? 1 : scaled));
}

/** test_watchdog_disarm - No guard. */
static inline void test_watchdog_disarm(void)
{
	alarm(0);
	signal(SIGALRM, SIG_DFL);
}

/** test_sleep_ms - Sleep @ms (never scaled: a fake's pause is part of what
    the test measures), resumed after a signal.  nanosleep, because
    poll(NULL, 0, ms) returns at once on Darwin. */
static inline void test_sleep_ms(long ms)
{
	struct timespec ts, rem;

	if (ms <= 0)
		return;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000L;
	while (nanosleep(&ts, &rem) < 0 && errno == EINTR)
		ts = rem;
}

/** test_wait_child - Wait for this process's child @pid, at most @ms
    (scaled).  Returns its wait status, or -1 after SIGKILL at the deadline
    (the child is then reaped).  Only for a process with no SIGCHLD reaper
    installed: a handler that calls waitpid(-1) takes the status first and
    this gets ECHILD, which is returned as -1 too. */
static inline int test_wait_child(pid_t pid, long ms)
{
	long left = test_scaled_ms(ms);
	int st = 0;
	pid_t r;

	for (;;) {
		r = waitpid(pid, &st, WNOHANG);
		if (r == pid)
			return st;
		if (r < 0 && errno != EINTR)
			return -1;
		if (left <= 0)
			break;
		test_sleep_ms(TEST_WAIT_STEP_MS);
		left -= TEST_WAIT_STEP_MS;
	}
	kill(pid, SIGKILL);
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
		;
	return -1;
}

/** test_wait_gone - Wait until @pid (a child or not) is gone, at most @ms.
    Returns 1 when gone, 0 at the deadline.  @ms is not scaled: the callers
    assert how soon a program ends after Quit, a bound of the product.
    kill(pid, 0) between short sleeps: no pidfd, no kqueue, the same on
    every system.  A child not yet reaped is a zombie and still "there"
    until something waits for it. */
static inline int test_wait_gone(pid_t pid, long ms)
{
	long left = ms;

	for (;;) {
		if (kill(pid, 0) != 0 && errno == ESRCH)
			return 1;
		if (left <= 0)
			return 0;
		test_sleep_ms(TEST_WAIT_STEP_MS);
		left -= TEST_WAIT_STEP_MS;
	}
}

#endif /* TEST_WATCHDOG_H */
