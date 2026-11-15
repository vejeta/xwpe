/* test_childsig.c -- a read that waits survives the editor's SIGCHLD.
 *
 * The editor takes SIGCHLD with wpe_child_sigchld_install.  A read on a pipe
 * that was interrupted by the handler fails with EINTR unless the handler
 * was installed with SA_RESTART (every system does this for a read that has
 * to wait); a reader that does not retry takes it for the end of its data.
 * Here the handler is installed by the same function, a blocking read waits
 * on a pipe, and children end while it waits: the read must still deliver
 * what the writer wrote, whole.
 *
 * Links we_child.c and we_fdloop.c only; no editor.  The alarm turns a hang
 * into a failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <poll.h>
#include "../we_child.h"
#include "../we_childsig.h"
#include "test_watchdog.h"

#define HANG_SECONDS      30
#define STORM_CHILDREN    40
#define US_PER_MS         1000L      /* the delays below are whole milliseconds */
#define STORM_STEP_US     20000     /* one child ends every 20 ms */
#define WRITER_DELAY_US   (STORM_CHILDREN * STORM_STEP_US + 200000)
#define MESSAGE           "all the data\n"
#define MESSAGE_LEN       13

static int fails;

static void chk(const char *what, long got, long want)
{
	if (got != want) {
		printf("FAIL: %s: got %ld want %ld\n", what, got, want);
		fails++;
	}
}

/* The handler the editor installs (WpeSignalChild), in its essentials. */
static void on_child(int sig)
{
	(void)sig;
	wpe_child_reap_now();
}

/* A child that ends after @after_us. */
static void start_ender(useconds_t after_us)
{
	if (fork() == 0) {
		test_sleep_ms((long)after_us / US_PER_MS);
		_exit(0);
	}
}

/* A child that writes the message after @after_us and ends. */
static void start_writer(int wr, useconds_t after_us)
{
	if (fork() == 0) {
		test_sleep_ms((long)after_us / US_PER_MS);
		if (write(wr, MESSAGE, MESSAGE_LEN) < 0)
			_exit(1);
		_exit(0);
	}
}

/* @scenario SC-debug-102 */
static void test_a_pipe_read_survives_a_sigchld_storm(void)
{
	char buf[64];
	int fds[2], i;
	ssize_t n;

	chk("pipe", pipe(fds), 0);
	start_writer(fds[1], WRITER_DELAY_US);
	for (i = 0; i < STORM_CHILDREN; i++)
		start_ender((useconds_t)i * STORM_STEP_US);
	close(fds[1]);   /* only the writer child holds the write end now */
	/* A blocking read, as a reader of a pipe that does not use the loop
	 * does: children end all the while it waits. */
	n = read(fds[0], buf, sizeof buf);
	chk("the read returned what was written, not EINTR", n, MESSAGE_LEN);
	if (n == MESSAGE_LEN)
		chk("and it is the message whole",
		    memcmp(buf, MESSAGE, MESSAGE_LEN), 0);
	close(fds[0]);
	while (wait(NULL) > 0 || errno == EINTR)
		;
}

static void foreign_info_handler(int sig, siginfo_t *info, void *ctx)
{
	(void)sig;
	(void)info;
	(void)ctx;
}

/* A SIGCHLD handler somebody else took, in the three-argument form, is not
   replaced by wpe_child_reap_install (the plain form is the other row of
   the same rule, in test_child.c). */
static void test_the_reaper_leaves_a_foreign_siginfo_handler_alone(void)
{
	struct sigaction mine, now, none;

	memset(&mine, 0, sizeof mine);
	mine.sa_sigaction = foreign_info_handler;
	mine.sa_flags = SA_SIGINFO;
	sigemptyset(&mine.sa_mask);
	chk("a foreign handler is installed", sigaction(SIGCHLD, &mine, NULL), 0);
	chk("reap_install accepts it", wpe_child_reap_install(), 0);
	chk("reading SIGCHLD back", sigaction(SIGCHLD, NULL, &now), 0);
	chk("it is still the foreign one",
	    (now.sa_flags & SA_SIGINFO) &&
	    now.sa_sigaction == foreign_info_handler, 1);
	memset(&none, 0, sizeof none);
	none.sa_handler = SIG_DFL;
	sigemptyset(&none.sa_mask);
	sigaction(SIGCHLD, &none, NULL);
}

#define DEATH_WAIT_MS 5000
#define MIDDLE_NOT_ARMED 2     /* the middle's exit status: the grandchild never armed */

/* The grandchild of a row: arms the death signal on its middle parent, tells
   the middle through @armed_wr, and waits, holding @alive_wr open until it
   dies.  The alarm bounds its life, so a system (or a regression) that sends
   no signal leaves no process behind holding the test's output open. */
static void grandchild_waits_for_its_parent(pid_t middle, int alive_rd, int alive_wr,
                                            int armed_rd, int armed_wr)
{
	close(alive_rd);
	close(armed_rd);
	test_lifetime_arm(HANG_SECONDS);   /* quiet: a fake's end is no failure */
	if (wpe_child_die_with_editor(middle) != 0)
		_exit(1);
	if (write(armed_wr, "a", 1) != 1)
		_exit(1);
	close(armed_wr);
	pause();
	(void)alive_wr;
	_exit(0);
}

/* The middle process: starts the grandchild and ends once the grandchild has
   armed the signal.  Ending earlier would race it: a grandchild that arms
   after its parent is gone sees another parent, gives up and closes the pipe,
   and the row would pass with no signal at all. */
static void middle_leaves_its_child(int alive_rd, int alive_wr)
{
	pid_t middle = getpid();
	int armed[2];
	char c;

	if (pipe(armed) != 0)
		_exit(1);
	if (fork() == 0)
		grandchild_waits_for_its_parent(middle, alive_rd, alive_wr, armed[0], armed[1]);
	close(armed[1]);
	close(alive_wr);
	if (read(armed[0], &c, 1) != 1)
		_exit(MIDDLE_NOT_ARMED);
	_exit(0);
}

/* @scenario SC-platform-05 */

/* Where the system has a parent-death signal (WPE_HAS_PARENT_DEATH_SIGNAL),
   a child that armed it ends when its parent does: the descriptor it holds
   open reaches end of file.  Where it has none, the row says so, which is
   what a macOS, NetBSD or OpenBSD run shows instead of silence. */
static void test_a_child_ends_with_the_parent_where_the_system_says_so(void)
{
	int pfd[2];
	pid_t middle;
	struct pollfd p;
	char c;
	int status = 0;

#if !WPE_HAS_PARENT_DEATH_SIGNAL
	printf("SKIP: SC-platform-05: no parent-death signal on this system\n");
	return;
#endif
	chk("a pipe for the row", pipe(pfd), 0);
	middle = fork();
	if (middle == 0)
		middle_leaves_its_child(pfd[0], pfd[1]);
	close(pfd[1]);
	waitpid(middle, &status, 0);
	chk("the child armed the signal before its parent ended",
	    WIFEXITED(status) && WEXITSTATUS(status) == 0, 1);
	p.fd = pfd[0];
	p.events = POLLIN;
	chk("the child ended with its parent",
	    poll(&p, 1, DEATH_WAIT_MS) == 1 && read(pfd[0], &c, 1) == 0, 1);
	close(pfd[0]);
}

int main(void)
{
	test_watchdog_case("test_a_pipe_read_survives_a_sigchld_storm: the read "
	                   "never returned", HANG_SECONDS);
	test_the_reaper_leaves_a_foreign_siginfo_handler_alone();
	chk("the handler is installed", wpe_child_sigchld_install(on_child), 0);
	test_a_pipe_read_survives_a_sigchld_storm();
	test_a_child_ends_with_the_parent_where_the_system_says_so();
	if (fails) {
		printf("%d failure(s)\n", fails);
		return 1;
	}
	printf("ok\n");
	return 0;
}
