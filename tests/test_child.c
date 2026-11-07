/* test_child.c -- unit test for the editor's children's owner (we_child.c).
 * Links we_child.c and we_fdloop.c only; no editor -- always runs.
 *
 * Real children, a SIGCHLD handler that reaps as the editor's does, and the
 * editor's own fd loop: every end is reported once, from the loop, with the
 * child's own status, however many children end at once; a child that
 * ignores SIGTERM is killed after the grace, and the end returns at once
 * while the loop keeps serving its timers; a signal after the end, or
 * between the reap and the report, reaches nobody; a forgotten child leaves
 * no callback, timer or slot; a full table or a full loop starts nothing;
 * the whole group is ended, not only the shell.  The waits are the loop's own, bounded; the alarm turns a hang
 * into a failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "../we_child.h"
#include "../we_fdloop.h"

#define HANG_SECONDS       30
#define MANY_WATCHED       16
#define MANY_UNWATCHED     4     /* ending together with the watched: twenty */
#define FD_LOOP_FILL_MAX   64    /* more than the loop has room for */
#define FD_SCAN_MAX        256   /* descriptors counted for a leak */
#define DID_NOT_END        "did not end"
#define NOT_STARTED        "not started"
#define LOOP_SLICE_MS      20
#define SLACK_MS           700   /* the loop's slack on a loaded machine */
#define END_RETURNS_MS     50    /* wpe_child_end must not wait */
#define TIMER_DUE_MS       100   /* a timer set before the end */
#define QUICK_STARTS       20
#define QUICK_FAILS        2000  /* enough to lose one end without the hold */
#define NO_SUCH_PROGRAM    "/nonexistent/xwpe-test-no-such-program"
#define EXEC_FAILED_EXIT   1     /* the child's exit when exec fails */
#define EARLY_EXIT         7
#define READ_PID_MAX       32
#define MS_PER_S           1000L
#define NS_PER_MS          1000000L
/* A child that ignores SIGTERM, and says so (its pid) once it does. */
#define DEAF_TO_TERM       "trap '' TERM; echo $$; sleep 30"
/* A shell that ends at SIGTERM, and a member of its group that does not. */
#define MEMBER_DEAF_TO_TERM "(trap '' TERM; exec sleep 30) & echo $!; wait"

static int fails;
static pid_t read_pid(int fd);

static void chk(const char *what, long got, long want)
{
	if (got != want) {
		printf("FAIL: %s: got %ld want %ld\n", what, got, want);
		fails++;
	}
}

static void chk_true(const char *what, int cond)
{
	if (!cond) {
		printf("FAIL: %s\n", what);
		fails++;
	}
}

static void on_alarm(int sig)
{
	static const char msg[] = "FAIL: a child's end never came\n";
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

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * MS_PER_S + ts.tv_nsec / NS_PER_MS;
}

/* What the callbacks saw, per child. */
typedef struct {
	WpeChild c;
	int calls;
	int status;
	int how;
	long at_ms;
} Run;

static void on_ended(WpeChild *c, int status, int how, void *data)
{
	Run *r = data;

	chk_true("the callback gets its own child", c == &r->c);
	chk("pid is 0 when the end is reported", c->pid, 0);
	r->calls++;
	r->status = status;
	r->how = how;
	r->at_ms = now_ms();
}

static char said[256];

static void on_say(const char *line)
{
	snprintf(said, sizeof said, "%s", line);
}

/* Run the loop until every run in @runs has been called back, or @ms. */
static void loop_until_ended(Run *runs, int n, long ms)
{
	long deadline = now_ms() + ms;
	int i, left;

	for (;;) {
		left = 0;
		for (i = 0; i < n; i++)
			left += runs[i].calls == 0;
		if (!left || now_ms() >= deadline)
			return;
		wpe_fd_poll(LOOP_SLICE_MS);
	}
}

static void spec_sh(WpeChildSpec *spec, char **argv, const char *script)
{
	memset(spec, 0, sizeof *spec);
	argv[0] = "sh";
	argv[1] = "-c";
	argv[2] = (char *)script;
	argv[3] = NULL;
	spec->argv = argv;
	spec->io = WPE_CHILD_IO_PIPES;
	spec->group = WPE_CHILD_OWN_SESSION;
	spec->stdin_null = 1;
	spec->stderr_to_stdout = 1;
	spec->what = "the test child";
}

static int start_sh(Run *r, const char *script)
{
	WpeChildSpec spec;
	char *argv[4];

	memset(r, 0, sizeof *r);
	spec_sh(&spec, argv, script);
	return wpe_child_start(&spec, &r->c, on_ended, r);
}

static void close_ends(Run *r)
{
	if (r->c.out_fd >= 0)
		close(r->c.out_fd);
	r->c.out_fd = WPE_CHILD_NO_FD;
}

static void until_exited(pid_t pid)
{
	siginfo_t info;

	while (waitid(P_PID, (id_t)pid, &info, WEXITED | WNOWAIT) < 0 &&
	       errno == EINTR)
		;
}

static int exit_code(int status)
{
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* 1: sixteen children and four more processes end at the same moment
 * (SIGCHLD held until all twenty have exited): each owner gets its own
 * child's exit code. */
static void test_twenty_children_ending_at_once_each_report_their_status(void)
{
	static Run runs[MANY_WATCHED];
	char script[64];
	pid_t other[MANY_UNWATCHED];
	sigset_t saved;
	int i;

	WpeChildSignalBlock(&saved);
	for (i = 0; i < MANY_WATCHED; i++) {
		snprintf(script, sizeof script, "exit %d", i + 1);
		chk("start", start_sh(&runs[i], script), 0);
	}
	for (i = 0; i < MANY_UNWATCHED; i++) {
		other[i] = fork();
		if (other[i] == 0)
			_exit(0);
	}
	for (i = 0; i < MANY_WATCHED; i++)
		until_exited(runs[i].c.pid);
	for (i = 0; i < MANY_UNWATCHED; i++)
		until_exited(other[i]);
	WpeChildSignalRestore(&saved);
	loop_until_ended(runs, MANY_WATCHED, SLACK_MS);
	for (i = 0; i < MANY_WATCHED; i++) {
		chk("called back once", runs[i].calls, 1);
		chk("its own exit code", exit_code(runs[i].status), i + 1);
		chk("ended, status known", runs[i].how, WPE_CHILD_ENDED);
		close_ends(&runs[i]);
	}
}

/* 2: a child that ignores SIGTERM is killed after the grace; the callback
 * comes with SIGKILL's status, measured by the loop's own clock. */
static void test_a_child_that_ignores_sigterm_is_killed_after_the_grace(void)
{
	static Run r;
	long t0, took;

	chk("start", start_sh(&r, DEAF_TO_TERM), 0);
	chk_true("it said it is deaf to SIGTERM", read_pid(r.c.out_fd) > 0);
	t0 = now_ms();
	wpe_child_end(&r.c, SIGTERM);
	loop_until_ended(&r, 1, WPE_CHILD_END_GRACE_MS + SLACK_MS);
	took = r.at_ms - t0;
	chk("called back", r.calls, 1);
	chk_true("killed by SIGKILL",
	         WIFSIGNALED(r.status) && WTERMSIG(r.status) == SIGKILL);
	chk_true("not before the grace", took >= WPE_CHILD_END_GRACE_MS);
	chk_true("soon after the grace",
	         took < WPE_CHILD_END_GRACE_MS + SLACK_MS);
	close_ends(&r);
}

static int timer_fired;
static long timer_at;

static void on_timer(void *data)
{
	(void)data;
	timer_fired = 1;
	timer_at = now_ms();
}

/* 3: wpe_child_end returns at once, and a timer set before it fires on
 * time while the child is still being ended; a second end takes the next
 * step (SIGKILL) now. */
static void test_end_returns_at_once_and_the_loop_keeps_running(void)
{
	static Run r;
	long t0, t1, deadline;

	chk("start", start_sh(&r, DEAF_TO_TERM), 0);
	chk_true("it said it is deaf to SIGTERM", read_pid(r.c.out_fd) > 0);
	timer_fired = 0;
	t0 = now_ms();
	wpe_timer_add(TIMER_DUE_MS, 0, on_timer, NULL);
	wpe_child_end(&r.c, SIGTERM);
	t1 = now_ms();
	chk_true("wpe_child_end returns at once", t1 - t0 < END_RETURNS_MS);
	deadline = t0 + TIMER_DUE_MS + SLACK_MS;
	while (!timer_fired && now_ms() < deadline)
		wpe_fd_poll(LOOP_SLICE_MS);
	chk("the timer fired", timer_fired, 1);
	chk_true("on time", timer_at - t0 < TIMER_DUE_MS + SLACK_MS);
	chk("the child is still being ended", r.calls, 0);
	wpe_child_end(&r.c, SIGTERM);
	loop_until_ended(&r, 1, SLACK_MS);
	chk("a second end kills it now", r.calls, 1);
	close_ends(&r);
}

/* One start of a program that does not exist: the child ends a moment after
 * the fork, often before the parent has given it its slot.  Its end is
 * reported all the same, with the exit code of a command that could not run. */
static void start_one_that_cannot_run(Run *r)
{
	WpeChildSpec spec;
	char *argv[4];

	memset(r, 0, sizeof *r);
	spec_sh(&spec, argv, NULL);
	argv[0] = NO_SUCH_PROGRAM;
	argv[1] = NULL;
	chk("start", wpe_child_start(&spec, &r->c, on_ended, r), 0);
	loop_until_ended(r, 1, SLACK_MS);
	chk("called back", r->calls, 1);
	chk("could not run", exit_code(r->status), EXEC_FAILED_EXIT);
	close_ends(r);
}

/* 4: a child that ends before it could be watched still reports its end:
 * SIGCHLD is held across the start.  Without that hold, one or two of the
 * two thousand quick failures lose their end. */
static void test_a_child_reaped_before_it_was_watched_cannot_happen(void)
{
	static Run r;
	int i;

	for (i = 0; i < QUICK_STARTS; i++) {
		chk("start", start_sh(&r, "exit 7"), 0);
		loop_until_ended(&r, 1, SLACK_MS);
		chk("called back", r.calls, 1);
		chk("its exit code", exit_code(r.status), EARLY_EXIT);
		close_ends(&r);
	}
	for (i = 0; i < QUICK_FAILS; i++)
		start_one_that_cannot_run(&r);
}

/* 5: once the end is reported, a signal reaches nobody. */
static void test_signal_after_the_end_reaches_nobody(void)
{
	static Run r;

	chk("start", start_sh(&r, "exit 0"), 0);
	loop_until_ended(&r, 1, SLACK_MS);
	chk("called back", r.calls, 1);
	chk("pid is 0", r.c.pid, 0);
	chk("no signal", wpe_child_signal(&r.c, SIGKILL), -1);
	close_ends(&r);
}

/* Run the loop for @ms, whatever comes. */
static void loop_for(long ms)
{
	long deadline = now_ms() + ms;

	while (now_ms() < deadline)
		wpe_fd_poll(LOOP_SLICE_MS);
}

/* 5, the window: reaped by the handler, not yet reported by the loop.  Its
 * number may already be another process's, so nothing is sent to it; an end
 * asked for in that window sends nothing either, and the callback still
 * comes once, with the real status, and no "did not end" after it. */
static void test_signal_between_reap_and_report_reaches_nobody(void)
{
	static Run r;
	sigset_t saved;

	WpeChildSignalBlock(&saved);
	chk("start", start_sh(&r, "exit 0"), 0);
	until_exited(r.c.pid);
	WpeChildSignalRestore(&saved);          /* the handler reaps it here */
	chk_true("reaped, not yet reported", r.calls == 0 && r.c.pid > 0);
	chk("no signal to the group", wpe_child_signal(&r.c, SIGKILL), -1);
	chk("no signal to the pid", wpe_child_signal_pid(&r.c, SIGINT), -1);
	said[0] = '\0';
	wpe_child_end(&r.c, SIGTERM);
	loop_until_ended(&r, 1, SLACK_MS);
	chk("called back", r.calls, 1);
	chk("with its status", r.how, WPE_CHILD_ENDED);
	chk("its exit code", exit_code(r.status), 0);
	chk("the ladder is over", r.c.end_stage, WPE_CHILD_END_NONE);
	loop_for(WPE_CHILD_END_GRACE_MS + WPE_CHILD_END_KILL_MS + SLACK_MS);
	chk("called back only once", r.calls, 1);
	chk_true("no \"did not end\"", strstr(said, DID_NOT_END) == NULL);
	close_ends(&r);
}

/* After wpe_child_forget the callback never comes, the ladder's timer is
 * gone with it and the slot is free again. */
static void test_forget_leaves_no_callback_no_timer_and_no_slot(void)
{
	static Run r;
	static Run runs[WPE_CHILD_MAX];
	pid_t pid;
	int i;

	chk("start", start_sh(&r, DEAF_TO_TERM), 0);
	pid = read_pid(r.c.out_fd);
	chk_true("it said it is deaf to SIGTERM", pid > 0);
	wpe_child_end(&r.c, SIGTERM);
	wpe_child_forget(&r.c);
	chk("pid is 0", r.c.pid, 0);
	chk("no ladder", r.c.end_stage, WPE_CHILD_END_NONE);
	if (pid > 0)
		kill(-pid, SIGKILL);            /* its session: the shell and its sleep */
	said[0] = '\0';
	loop_for(WPE_CHILD_END_GRACE_MS + SLACK_MS);
	chk("no callback", r.calls, 0);
	chk_true("no line", said[0] == '\0');
	close_ends(&r);
	for (i = 0; i < WPE_CHILD_MAX; i++)
		chk("every slot is free again", start_sh(&runs[i], "exit 0"), 0);
	loop_until_ended(runs, WPE_CHILD_MAX, SLACK_MS);
	for (i = 0; i < WPE_CHILD_MAX; i++)
		close_ends(&runs[i]);
}

static void on_nothing(int fd, void *data)
{
	(void)fd;
	(void)data;
}

/* The open descriptors of this process. */
static int open_fds(void)
{
	int fd, n = 0;

	for (fd = 0; fd < FD_SCAN_MAX; fd++)
		n += fcntl(fd, F_GETFD) >= 0;
	return n;
}

/* 7, the other refusal: the loop has no room for the wake pipe.  Nothing is
 * started, no descriptor is left open, and the user reads why. */
static void test_a_full_loop_fails_the_start_with_nothing_left_open(void)
{
	static Run r;
	int fills[FD_LOOP_FILL_MAX][2];
	int n, i, before;

	for (n = 0; n < FD_LOOP_FILL_MAX; n++) {
		if (pipe(fills[n]) < 0)
			break;
		if (wpe_fd_add(fills[n][0], POLLIN, on_nothing, NULL) != 0) {
			close(fills[n][0]);
			close(fills[n][1]);
			break;
		}
	}
	chk_true("the loop is full", n < FD_LOOP_FILL_MAX);
	before = open_fds();
	said[0] = '\0';
	chk("the start is refused", start_sh(&r, "sleep 30"), -1);
	chk("with nothing running", r.c.pid, 0);
	chk("and no descriptor", r.c.out_fd, WPE_CHILD_NO_FD);
	chk("none left open", open_fds(), before);
	chk_true("the user reads why", strstr(said, NOT_STARTED) != NULL);
	for (i = 0; i < n; i++) {
		wpe_fd_del(fills[i][0]);
		close(fills[i][0]);
		close(fills[i][1]);
	}
}

/* 7: a full table starts nothing and says why. */
static void test_no_slot_fails_the_start_with_nothing_left_running(void)
{
	static Run runs[WPE_CHILD_MAX + 1];
	int i;

	for (i = 0; i < WPE_CHILD_MAX; i++)
		chk("start", start_sh(&runs[i], "sleep 30"), 0);
	said[0] = '\0';
	chk("the next start is refused",
	    start_sh(&runs[WPE_CHILD_MAX], "sleep 30"), -1);
	chk("with nothing running", runs[WPE_CHILD_MAX].c.pid, 0);
	chk("and no descriptor", runs[WPE_CHILD_MAX].c.out_fd, WPE_CHILD_NO_FD);
	chk_true("the user reads why", strstr(said, "not started") != NULL);
	said[0] = '\0';
	for (i = 0; i < WPE_CHILD_MAX; i++)
		wpe_child_end(&runs[i].c, SIGTERM);
	loop_until_ended(runs, WPE_CHILD_MAX, SLACK_MS);
	for (i = 0; i < WPE_CHILD_MAX; i++) {
		chk("ended on SIGTERM", runs[i].calls, 1);
		chk("by SIGTERM, not given up on", runs[i].how, WPE_CHILD_ENDED);
		chk_true("by SIGTERM itself", WIFSIGNALED(runs[i].status) &&
		         WTERMSIG(runs[i].status) == SIGTERM);
		close_ends(&runs[i]);
	}
	chk_true("a full table ended at once: no \"did not end\"",
	         strstr(said, DID_NOT_END) == NULL);
}

static pid_t read_pid(int fd)
{
	char buf[READ_PID_MAX];
	long deadline = now_ms() + SLACK_MS;
	ssize_t n = 0, got;

	while (now_ms() < deadline && (n == 0 || buf[n - 1] != '\n')) {
		got = read(fd, buf + n, sizeof buf - 1 - n);
		if (got > 0)
			n += got;
		else
			wpe_fd_poll(LOOP_SLICE_MS);
	}
	buf[n] = '\0';
	return (pid_t)atol(buf);
}

/* 8: the shell and what it started in the background are both ended. */
static void test_the_group_not_only_the_shell_is_ended(void)
{
	static Run r;
	pid_t bg;
	long deadline;

	chk("start", start_sh(&r, "sleep 30 & echo $!; wait"), 0);
	bg = read_pid(r.c.out_fd);
	chk_true("the background child's pid", bg > 0);
	wpe_child_end(&r.c, SIGTERM);
	loop_until_ended(&r, 1, SLACK_MS);
	chk("the shell ended", r.calls, 1);
	deadline = now_ms() + SLACK_MS;
	while (bg > 0 && kill(bg, 0) == 0 && now_ms() < deadline)
		wpe_fd_poll(LOOP_SLICE_MS);
	chk_true("the background child is gone too", bg > 0 && kill(bg, 0) < 0);
	close_ends(&r);
}

/* 8, a leader that goes first: the shell ends at SIGTERM, what it started
 * ignores it and lives on.  The shell's end is reported at once, and the
 * rest of its group is killed when the grace is over, not left running. */
static void test_a_member_deaf_to_sigterm_is_killed_after_its_leader_ended(void)
{
	static Run r;
	pid_t bg;
	long t0, deadline, gone_ms;

	chk("start", start_sh(&r, MEMBER_DEAF_TO_TERM), 0);
	bg = read_pid(r.c.out_fd);
	chk_true("the member's pid", bg > 0);
	t0 = now_ms();
	wpe_child_end(&r.c, SIGTERM);
	loop_until_ended(&r, 1, SLACK_MS);
	chk("the shell ended at SIGTERM", r.calls, 1);
	chk_true("before the grace", r.at_ms - t0 < WPE_CHILD_END_GRACE_MS);
	chk_true("the member outlives the polite signal",
	         bg > 0 && kill(bg, 0) == 0);
	deadline = t0 + WPE_CHILD_END_GRACE_MS + SLACK_MS;
	while (bg > 0 && kill(bg, 0) == 0 && now_ms() < deadline)
		wpe_fd_poll(LOOP_SLICE_MS);
	chk_true("the member is gone after the grace",
	         bg > 0 && kill(bg, 0) < 0);
	gone_ms = now_ms() - t0;
	if (gone_ms < WPE_CHILD_END_GRACE_MS)
		printf("gone after %ld ms\n", gone_ms);
	chk_true("not before it", gone_ms >= WPE_CHILD_END_GRACE_MS);
	if (bg > 0)
		kill(bg, SIGKILL);
	close_ends(&r);
}

/* @scenario SC-debug-72 */
int main(void)
{
	struct sigaction act;

	signal(SIGALRM, on_alarm);
	alarm(HANG_SECONDS);
	memset(&act, 0, sizeof act);
	act.sa_handler = on_child;
	sigemptyset(&act.sa_mask);
	act.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	WpeChildWakeOpen();
	wpe_child_say_set(on_say);
	sigaction(SIGCHLD, &act, NULL);

	test_twenty_children_ending_at_once_each_report_their_status();
	test_a_child_that_ignores_sigterm_is_killed_after_the_grace();
	test_end_returns_at_once_and_the_loop_keeps_running();
	test_a_child_reaped_before_it_was_watched_cannot_happen();
	test_signal_after_the_end_reaches_nobody();
	test_signal_between_reap_and_report_reaches_nobody();
	test_forget_leaves_no_callback_no_timer_and_no_slot();
	test_no_slot_fails_the_start_with_nothing_left_running();
	test_a_full_loop_fails_the_start_with_nothing_left_open();
	test_the_group_not_only_the_shell_is_ended();
	test_a_member_deaf_to_sigterm_is_killed_after_its_leader_ended();

	if (fails) {
		printf("%d failure(s)\n", fails);
		return 1;
	}
	printf("ok\n");
	return 0;
}
