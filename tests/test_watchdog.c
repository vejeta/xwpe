/* test_watchdog.c -- the hang guard of the C tests, tested.
 *
 * test_watchdog.h is what turns a test that waits for ever into a failure
 * that names what it waited for.  This program checks the header itself:
 *
 *   - a victim that arms 1 s and sleeps 2 s is stopped: it exits 1 and its
 *     last line is "FAIL: <what> did not finish within 1 s";
 *   - the same victim with XWPE_TEST_WAIT_SCALE=3 (or 2.5, a float) passes;
 *   - a line printed before the alarm fires is still in the output;
 *   - test_wait_child returns a child's status, and kills a child that does
 *     not end within its time;
 *   - test_wait_gone says 1 for a process that ends and 0 for one that stays.
 *
 * The victim is this program started again with --victim.
 *
 * @scenario SC-child-wait-07
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

#define TEST_WANT_CHILD_STATE   /* test_wait_gone: the process table */
#include "test_watchdog.h"

#define VICTIM_FLAG       "--victim"
#define VICTIM_NAME       "the sleeper"
#define VICTIM_ARM_S      1
#define VICTIM_SLEEP_MS   2000
#define OUTPUT_MAX        1024
#define SELF_GUARD_S      60
#define CHILD_SHORT_MS    200
#define CHILD_LONG_S      30
#define WAIT_GONE_MS      2000

static int failures;
static char self_path[4096];

static void check(int cond, const char *what)
{
	if (!cond) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

/** victim - Arm for VICTIM_ARM_S, sleep past it; finishing is the pass. */
static int victim(void)
{
	printf("before the alarm\n");
	test_watchdog_arm(VICTIM_NAME, VICTIM_ARM_S);
	/* A case that fails a check and then hangs: its FAIL line must reach
	   a pipe or a log file although the alarm ends it with _exit. */
	printf("inside the armed case\n");
	test_sleep_ms(VICTIM_SLEEP_MS);
	test_watchdog_disarm();
	printf("PASS: the victim finished\n");
	return 0;
}

/** run_victim - Start the victim with XWPE_TEST_WAIT_SCALE = @scale (NULL:
    unset), collect its output in @out and return its wait status. */
static int run_victim(const char *scale, char *out, size_t room)
{
	int fds[2], st = 0;
	size_t used = 0;
	ssize_t n;
	pid_t pid;

	out[0] = '\0';
	if (pipe(fds) != 0)
		return -1;
	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		close(fds[0]);
		dup2(fds[1], STDOUT_FILENO);
		close(fds[1]);
		if (scale)
			setenv("XWPE_TEST_WAIT_SCALE", scale, 1);
		else
			unsetenv("XWPE_TEST_WAIT_SCALE");
		execl(self_path, self_path, VICTIM_FLAG, (char *)NULL);
		_exit(127);
	}
	close(fds[1]);
	while (used + 1 < room && (n = read(fds[0], out + used, room - used - 1)) > 0)
		used += (size_t)n;
	out[used] = '\0';
	close(fds[0]);
	waitpid(pid, &st, 0);
	return st;
}

static void test_the_alarm_names_what_did_not_finish(void)
{
	char out[OUTPUT_MAX];
	int st = run_victim(NULL, out, sizeof out);

	check(WIFEXITED(st) && WEXITSTATUS(st) == 1,
	      "an alarm that fired did not exit 1");
	check(strstr(out, "FAIL: " VICTIM_NAME " did not finish within 1 s\n") != NULL,
	      "the alarm did not name what did not finish and its time");
	check(strstr(out, "before the alarm\n") != NULL,
	      "a line printed before the alarm was lost");
	check(strstr(out, "inside the armed case\n") != NULL,
	      "a line printed after arming was lost at the alarm's _exit "
	      "(stdout not line-buffered)");
}

static void test_the_scale_stretches_the_alarm(void)
{
	char out[OUTPUT_MAX];
	int st = run_victim("3", out, sizeof out);

	check(WIFEXITED(st) && WEXITSTATUS(st) == 0,
	      "XWPE_TEST_WAIT_SCALE=3 did not let the victim finish");
	check(strstr(out, "PASS: the victim finished") != NULL,
	      "the scaled victim did not say it finished");
	st = run_victim("2.5", out, sizeof out);
	check(WIFEXITED(st) && WEXITSTATUS(st) == 0,
	      "a fractional scale (2.5) was not read as a float");
	st = run_victim("nonsense", out, sizeof out);
	check(WIFEXITED(st) && WEXITSTATUS(st) == 1,
	      "a scale that is not a number was not taken as 1");
}

static pid_t start_child(int exit_status, int stay_s)
{
	pid_t pid = fork();

	if (pid == 0) {
		if (stay_s > 0)
			sleep((unsigned)stay_s);
		_exit(exit_status);
	}
	return pid;
}

static void test_wait_child_returns_the_status_or_kills(void)
{
	pid_t pid = start_child(7, 0);
	int st = test_wait_child(pid, 5000);

	check(st != -1 && WIFEXITED(st) && WEXITSTATUS(st) == 7,
	      "test_wait_child did not return the child's status");
	pid = start_child(0, CHILD_LONG_S);
	st = test_wait_child(pid, CHILD_SHORT_MS);
	check(st == -1, "test_wait_child did not give up on a child that stayed");
	check(child_pid_reaped(pid), "the child that stayed was not ended and reaped");
}

static void test_wait_gone_says_whether_the_process_ended(void)
{
	pid_t pid = start_child(0, 0);

	test_wait_child(pid, 5000);
	check(test_wait_gone(pid, WAIT_GONE_MS) == 1,
	      "test_wait_gone did not see a reaped child gone");
	check(test_wait_gone(getpid(), CHILD_SHORT_MS) == 0,
	      "test_wait_gone called this very process gone");
}

int main(int argc, char **argv)
{
	if (argc >= 2 && !strcmp(argv[1], VICTIM_FLAG))
		return victim();
	if (!realpath(argv[0], self_path))
		snprintf(self_path, sizeof self_path, "%s", argv[0]);
	test_watchdog_arm("test_watchdog", SELF_GUARD_S);
	test_the_alarm_names_what_did_not_finish();
	test_the_scale_stretches_the_alarm();
	test_wait_child_returns_the_status_or_kills();
	test_wait_gone_says_whether_the_process_ended();
	if (failures) {
		printf("test_watchdog: %d FAILURE(S)\n", failures);
		return 1;
	}
	printf("PASS: watchdog (names what did not finish, scales as a float, "
	       "keeps earlier output), test_wait_child, test_wait_gone\n");
	return 0;
}
