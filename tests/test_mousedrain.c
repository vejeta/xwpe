/* test_mousedrain.c -- unit test for emptying the curses mouse queue.
 * Links we_mousedrain.c only; no editor, no curses -- always runs (no skip).
 *
 * OpenBSD's base curses answers getmouse() with "ok, empty event" forever.
 * The editor's drain used to loop until getmouse() failed, so it never came
 * back and the editor took no more keys after any dialog closed.  The fake
 * queue here answers the same way; the alarm turns a hang into a failure.
 */
#include <stdio.h>
#include <unistd.h>
#include <signal.h>

#include "../we_mousedrain.h"

#define HANG_SECONDS 5

static int fails;

static void chk(const char *what, int got, int want)
{
	if (got != want) {
		printf("FAIL: %s: got %d want %d\n", what, got, want);
		fails++;
	}
}

static void on_alarm(int sig)
{
	static const char msg[] = "FAIL: drain did not return (spins on a queue that never ends)\n";
	(void)sig;
	if (write(1, msg, sizeof msg - 1) < 0) { /* nothing to do */ }
	_exit(1);
}

/* A queue that never runs out and only ever holds empty events. */
static int pull_empty_forever(void *arg)
{
	(void)arg;
	return 0;
}

/* A queue that never runs out and keeps handing out real events. */
static int pull_real_forever(void *arg)
{
	(void)arg;
	return 1;
}

/* A queue holding *(int *)arg real events, then reporting empty. */
static int pull_counted(void *arg)
{
	int *left = (int *)arg;
	if (*left <= 0)
		return -1;
	(*left)--;
	return 1;
}

static void test_empty_events_end_the_drain(void)
{
	chk("empty event ends the drain", wpe_mouse_drain(pull_empty_forever, 0), 0);
}

static void test_real_events_are_bounded(void)
{
	chk("endless real events are bounded",
	    wpe_mouse_drain(pull_real_forever, 0), WPE_MOUSE_QUEUE_MAX);
}

static void test_short_queue_is_emptied(void)
{
	int left = 3;
	chk("three queued events are all thrown away",
	    wpe_mouse_drain(pull_counted, &left), 3);
	chk("none left behind", left, 0);
}

static void test_empty_queue(void)
{
	int left = 0;
	chk("empty queue", wpe_mouse_drain(pull_counted, &left), 0);
}

int main(void)
{
	signal(SIGALRM, on_alarm);
	alarm(HANG_SECONDS);

	test_empty_events_end_the_drain();
	test_real_events_are_bounded();
	test_short_queue_is_emptied();
	test_empty_queue();

	if (fails) {
		printf("%d failure(s)\n", fails);
		return 1;
	}
	printf("ok\n");
	return 0;
}
