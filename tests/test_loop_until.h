/* test_loop_until.h -- the one way a C row waits for the fd loop to do
 * something.
 *
 * A row that needs the loop to tell an owner, run a call or write a queue
 * does not sleep and does not count time slices: it runs the loop until its
 * predicate holds, under a bound that only says how long a RED run takes.
 * A green run ends the moment the predicate holds.
 *
 *     if (!test_loop_until(told_once, &state, 2000)) ... the row failed
 *
 * The bound is multiplied by XWPE_TEST_WAIT_SCALE as every wait of the C
 * tests is (tests/test_watchdog.h).  The wait is wpe_fd_wait_for, the
 * loop's own wait, so every callback, timer and owed call is served while
 * it runs.
 *
 * Static inline, libc and we_fdloop.h only, so it is included by a test
 * that compiles the loop's source in and by one that links it.
 */
#ifndef TEST_LOOP_UNTIL_H
#define TEST_LOOP_UNTIL_H

#include <time.h>
#include "we_fdloop.h"
#include "test_watchdog.h"

/* What a row asks of the loop: non-zero once the row's state is reached. */
typedef int (*test_loop_pred)(void *ctx);

/* The predicate the loop's wait asks, which takes no argument: the row's own
   is kept here for the length of one test_loop_until.  The wait nests (a
   row's callback may wait again), so each call saves the outer one. */
static test_loop_pred test_loop_cur_pred;
static void *test_loop_cur_ctx;

/** test_loop_now_ms - A monotonic clock in milliseconds, for the bound only. */
static inline long test_loop_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/** test_loop_ask - What wpe_fd_wait_for asks: the row's predicate. */
static int test_loop_ask(void)
{
	return test_loop_cur_pred(test_loop_cur_ctx);
}

/** test_loop_left_ms - What is left of @bound_ms (scaled) since @start. */
static inline long test_loop_left_ms(long start, long bound_ms)
{
	long left = (long)((double)bound_ms * test_wait_scale()) -
	            (test_loop_now_ms() - start);

	return left > 0 ? left : 0;
}

/**
 * test_loop_until - Run the fd loop until @pred holds or @bound_ms is spent.
 * @pred: the row's condition, asked once at the start and after every
 * callback, timer or owed call the loop runs; @ctx: handed to it;
 * @bound_ms: the longest a failing run waits, before XWPE_TEST_WAIT_SCALE.
 *
 * A wake the loop hands back for a descriptor nobody reads (a key) is
 * asked again, never counted as an end.  A loop with nothing to run ends the
 * wait at once: the predicate is then asked once more, and a row whose state
 * cannot be reached fails now instead of at the bound.
 * Return: 1 when @pred held, 0 when it did not.
 */
static inline int test_loop_until(test_loop_pred pred, void *ctx, long bound_ms)
{
	test_loop_pred outer_pred = test_loop_cur_pred;
	void *outer_ctx = test_loop_cur_ctx;
	long start = test_loop_now_ms();
	int result;

	test_loop_cur_pred = pred;
	test_loop_cur_ctx = ctx;
	do
		result = wpe_fd_wait_for(test_loop_ask,
		                         test_loop_left_ms(start, bound_ms),
		                         "a test row");
	while (result == WPE_WAIT_INPUT && test_loop_left_ms(start, bound_ms) > 0);
	test_loop_cur_pred = outer_pred;
	test_loop_cur_ctx = outer_ctx;
	return result == WPE_WAIT_DONE || pred(ctx);
}

/**
 * test_loop_passes - Run the loop @n times without sleeping.
 *
 * For a row that asserts something did NOT happen: a fixed number of passes
 * is a count of work, not a wait.  A pass with nothing to do returns at once.
 */
static inline void test_loop_passes(int n)
{
	int i;

	for (i = 0; i < n; i++)
		wpe_fd_poll(0);
}

#endif
