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

#include "../we_child.h"

#define HANG_SECONDS      30
#define STORM_CHILDREN    40
#define STORM_STEP_US     20000      /* one child ends every 20 ms */
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

static void on_alarm(int sig)
{
	static const char msg[] = "FAIL: the read never returned\n";
	(void)sig;
	if (write(1, msg, sizeof msg - 1) < 0) { /* nothing to do */ }
	_exit(1);
}

/* The handler the editor installs (WpeSignalChild), in its essentials. */
static void on_child(int sig)
{
	int saved = errno, st;
	pid_t pid;

	(void)sig;
	while ((pid = waitpid(-1, &st, WNOHANG)) > 0)
		wpe_child_note_ended(pid, st);
	errno = saved;
}

/* A child that ends after @after_us. */
static void start_ender(useconds_t after_us)
{
	if (fork() == 0) {
		usleep(after_us);
		_exit(0);
	}
}

/* A child that writes the message after @after_us and ends. */
static void start_writer(int wr, useconds_t after_us)
{
	if (fork() == 0) {
		usleep(after_us);
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

int main(void)
{
	signal(SIGALRM, on_alarm);
	alarm(HANG_SECONDS);
	chk("the handler is installed", wpe_child_sigchld_install(on_child), 0);
	test_a_pipe_read_survives_a_sigchld_storm();
	if (fails) {
		printf("%d failure(s)\n", fails);
		return 1;
	}
	printf("ok\n");
	return 0;
}
