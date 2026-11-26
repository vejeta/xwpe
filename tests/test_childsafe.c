/* test_childsafe.c -- what the child of the editor does between fork and exec.
 * Links we_child.c and we_fdloop.c only; no editor -- always runs.
 *
 * The graphical editor runs a library thread (Pango's font loader) from
 * start-up, so a lock that thread holds at the moment the editor forks a
 * program is held in the child by a thread that does not exist there.  The
 * child of the owner therefore calls nothing that takes a lock or allocates:
 * the parent prepares every byte it says and writes, the child copies them.
 *
 * The hang is staged without a clock: a thread of this test locks a mutex
 * and holds it while the owner forks, and the linker's --wrap puts a lock of
 * that mutex around the names a child of the editor must not call (stdio, the
 * formatting calls, strerror), in any process but this one.  A child that
 * calls one of them blocks for ever; one that does not ends in milliseconds.
 * The row's bound is the harness's limit, not the mechanism: the wrapper
 * writes the name of the call it was reached by into memory the child shares
 * with the test, so a red row says which call, at once.  This models the
 * hazard (a mutex another thread held at the fork) around the exact names
 * the child once called; it does not claim that a libc's own stdio lock hangs.
 * Where the linker has no --wrap the rows that need it say SKIP.
 *
 * The other rows keep what the user reads (the words of a start that failed,
 * in each stream), the report a child sends the editor (traced, and no
 * descriptor left open), and that no handler of the editor's runs in a child.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "../we_child.h"
#include "../we_fdloop.h"
#include "../we_path.h"
#include "test_watchdog.h"

#ifndef MAP_ANONYMOUS
# define MAP_ANONYMOUS MAP_ANON
#endif

#define HANG_SECONDS       60
#define LOOP_SLICE_MS      20
#define ENDED_WAIT_MS      test_scaled_ms(10000)
#define HUNG_BOUND_MS      test_scaled_ms(5000)
#define MS_PER_S           1000L
#define NS_PER_MS          1000000L
#define OUT_MAX            512
#define ENTERED_MAX        32
#define TRACE_LINES        64
#define TRACE_LINE_SIZE    160
#define FD_SCAN_MAX        256
#define PTY_NAME_MAX       64
#define NO_SUCH_DIR        "/nonexistent/xwpe-test-no-such-dir"
#define NO_SUCH_PROGRAM    "/nonexistent/xwpe-test-no-such-program"
#define NO_SUCH_TTY        "/nonexistent/xwpe-test-no-such-tty"
#define EXEC_FAILED_EXIT   1     /* a command that could not run */
#define SAID_FAILED_EXIT   127   /* one that said why, as a shell's */
#define SEGV_HANDLER_EXIT  99    /* what the test's SIGSEGV handler ends with */
#define WHAT               "the test child"
#define FAILED_PREFIX      "Error at Command: "
#define REASON_ENOENT      "No such file or directory"
#define REASON_EPERM       "Operation not permitted"

static int fails;

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

static void chk_str(const char *what, const char *got, const char *want)
{
	if (strcmp(got, want) != 0) {
		printf("FAIL: %s: got \"%s\" want \"%s\"\n", what, got, want);
		fails++;
	}
}

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * MS_PER_S + ts.tv_nsec / NS_PER_MS;
}

/* ---- memory a child shares with the test --------------------------------- */

typedef struct {
	char entered[ENTERED_MAX];     /* the first wrapped call a child reached */
	volatile int handler_pid;      /* the process a handler of the test ran in */
	volatile int handler_sig;
} Shared;

static Shared *shared;
static pid_t test_pid;

static void shared_reset(void)
{
	memset(shared, 0, sizeof *shared);
}

/* ---- the lock another thread holds ---------------------------------------- */

static pthread_mutex_t held = PTHREAD_MUTEX_INITIALIZER;
static pthread_t holder;
static int hold_pipe[2], release_pipe[2];

/** holder_main - The thread that holds the lock: says so, then holds it until
 * it is told to let go (the editor's library thread, in the middle of a
 * font lookup). */
static void *holder_main(void *arg)
{
	char b = 'h';

	(void)arg;
	pthread_mutex_lock(&held);
	if (write(hold_pipe[1], &b, 1) < 0)
		return NULL;
	if (read(release_pipe[0], &b, 1) < 0)
		return NULL;
	pthread_mutex_unlock(&held);
	return NULL;
}

/** holder_start - Have another thread hold the lock; returns once it does. */
static void holder_start(void)
{
	char b;

	chk("the holder's pipes", pipe(hold_pipe) + pipe(release_pipe), 0);
	chk("the holder starts", pthread_create(&holder, NULL, holder_main, NULL), 0);
	chk("the holder holds", (int)read(hold_pipe[0], &b, 1), 1);
}

/** holder_stop - Let the holder go: this process's copy of the lock is free
 * again; a copy a child was forked with stays held. */
static void holder_stop(void)
{
	char b = 'r';

	if (write(release_pipe[1], &b, 1) < 0)
		return;
	pthread_join(holder, NULL);
	close(hold_pipe[0]);
	close(hold_pipe[1]);
	close(release_pipe[0]);
	close(release_pipe[1]);
}

/** enter_wrapped - A wrapped call was reached.  In this process nothing
 * happens.  In any other (a child) its name is written to the memory the
 * test reads, and the lock is taken: for ever, when another thread held it at
 * the fork. */
static void enter_wrapped(const char *name)
{
	size_t n;

	if (getpid() == test_pid)
		return;
	if (shared->entered[0] == '\0') {
		n = strlen(name);
		if (n >= ENTERED_MAX)
			n = ENTERED_MAX - 1;
		memcpy(shared->entered + 1, name + 1, n - 1);
		shared->entered[n] = '\0';
		shared->entered[0] = name[0];
	}
	pthread_mutex_lock(&held);
	pthread_mutex_unlock(&held);
}

#ifdef WPE_TEST_LD_WRAP
/* The names a child of the editor must not call.  Each is wrapped: the lock
   above, then the real call.  The _chk spellings are what a fortified build
   calls for the same functions. */
int __real_fflush(FILE *);
int __real_fputc(int, FILE *);
int __real_putc(int, FILE *);
int __real_fputs(const char *, FILE *);
size_t __real_fwrite(const void *, size_t, size_t, FILE *);
int __real_vsnprintf(char *, size_t, const char *, va_list);
char *__real_strerror(int);

int __wrap_fflush(FILE *f)
{
	enter_wrapped("fflush");
	return __real_fflush(f);
}

int __wrap_fputc(int c, FILE *f)
{
	enter_wrapped("fputc");
	return __real_fputc(c, f);
}

int __wrap_putc(int c, FILE *f)
{
	enter_wrapped("putc");
	return __real_putc(c, f);
}

int __wrap_fputs(const char *s, FILE *f)
{
	enter_wrapped("fputs");
	return __real_fputs(s, f);
}

size_t __wrap_fwrite(const void *p, size_t size, size_t n, FILE *f)
{
	enter_wrapped("fwrite");
	return __real_fwrite(p, size, n, f);
}

int __wrap_fprintf(FILE *f, const char *fmt, ...)
{
	va_list ap;
	int r;

	enter_wrapped("fprintf");
	va_start(ap, fmt);
	r = vfprintf(f, fmt, ap);
	va_end(ap);
	return r;
}

int __wrap_vsnprintf(char *s, size_t n, const char *fmt, va_list ap)
{
	enter_wrapped("vsnprintf");
	return __real_vsnprintf(s, n, fmt, ap);
}

int __wrap_snprintf(char *s, size_t n, const char *fmt, ...)
{
	va_list ap;
	int r;

	enter_wrapped("snprintf");
	va_start(ap, fmt);
	r = __real_vsnprintf(s, n, fmt, ap);
	va_end(ap);
	return r;
}

char *__wrap_strerror(int err)
{
	enter_wrapped("strerror");
	return __real_strerror(err);
}

# ifdef __GLIBC__
int __real___vsnprintf_chk(char *, size_t, int, size_t, const char *, va_list);
int __vfprintf_chk(FILE *, int, const char *, va_list);   /* not wrapped */
char *__real_strerror_r(int, char *, size_t);
int __real___xpg_strerror_r(int, char *, size_t);

int __wrap___fprintf_chk(FILE *f, int flag, const char *fmt, ...)
{
	va_list ap;
	int r;

	enter_wrapped("__fprintf_chk");
	va_start(ap, fmt);
	r = __vfprintf_chk(f, flag, fmt, ap);
	va_end(ap);
	return r;
}

int __wrap___vsnprintf_chk(char *s, size_t n, int flag, size_t slen,
                           const char *fmt, va_list ap)
{
	enter_wrapped("__vsnprintf_chk");
	return __real___vsnprintf_chk(s, n, flag, slen, fmt, ap);
}

int __wrap___snprintf_chk(char *s, size_t n, int flag, size_t slen,
                          const char *fmt, ...)
{
	va_list ap;
	int r;

	enter_wrapped("__snprintf_chk");
	va_start(ap, fmt);
	r = __real___vsnprintf_chk(s, n, flag, slen, fmt, ap);
	va_end(ap);
	return r;
}

char *__wrap_strerror_r(int err, char *buf, size_t n)
{
	enter_wrapped("strerror_r");
	return __real_strerror_r(err, buf, n);
}

int __wrap___xpg_strerror_r(int err, char *buf, size_t n)
{
	enter_wrapped("__xpg_strerror_r");
	return __real___xpg_strerror_r(err, buf, n);
}
# endif /* __GLIBC__ */
#endif /* WPE_TEST_LD_WRAP */

/* ---- starting children ---------------------------------------------------- */

typedef struct {
	WpeChild c;
	int calls;
	int status;
	char at_end[OUT_MAX];      /* what out_fd held when the end was reported */
	ssize_t at_end_len;
	char at_end_err[OUT_MAX];  /* the same for err_fd */
	ssize_t at_end_err_len;
} Run;

/** read_now - What @fd holds now, without waiting; "" when it has nothing. */
static ssize_t read_now(int fd, char *buf, size_t n)
{
	ssize_t got;

	buf[0] = '\0';
	if (fd < 0)
		return 0;
	got = read(fd, buf, n - 1);
	if (got < 0)
		got = 0;
	buf[got] = '\0';
	return got;
}

static void on_ended(WpeChild *c, int status, int how, void *data)
{
	Run *r = data;

	(void)how;
	r->at_end_len = read_now(c->out_fd, r->at_end, sizeof r->at_end);
	r->at_end_err_len = read_now(c->err_fd, r->at_end_err, sizeof r->at_end_err);
	r->status = status;
	r->calls++;
}

static void on_say(const char *line)
{
	(void)line;
}

static int exit_code(int status)
{
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/** loop_until_ended - Run the editor's loop until @r's child has ended, or
 * @ms.  Return: 1 when it ended. */
static int loop_until_ended(Run *r, long ms)
{
	long deadline = now_ms() + ms;

	while (r->calls == 0 && now_ms() < deadline)
		wpe_fd_poll(LOOP_SLICE_MS);
	return r->calls > 0;
}

/** spec_for - A child of this test: its own session, no input, one merged
 * output stream, dying with the editor. */
static void spec_for(WpeChildSpec *spec, char **argv)
{
	memset(spec, 0, sizeof *spec);
	spec->argv = argv;
	spec->io = WPE_CHILD_IO_PIPES;
	spec->group = WPE_CHILD_OWN_SESSION;
	spec->stdin_null = 1;
	spec->stderr_to_stdout = 1;
	spec->die_with_editor = 1;
	spec->side_pty_slave = WPE_CHILD_NO_FD;
	spec->tty_fd = WPE_CHILD_NO_FD;
	spec->what = WHAT;
}

static void run_close_ends(Run *r)
{
	if (r->c.out_fd >= 0)
		close(r->c.out_fd);
	if (r->c.err_fd >= 0)
		close(r->c.err_fd);
	if (r->c.pty_fd >= 0)
		close(r->c.pty_fd);
	r->c.out_fd = r->c.err_fd = r->c.pty_fd = WPE_CHILD_NO_FD;
}

/** end_hung - A child that blocked: end it by its exact number and wait for
 * its end to be reported. */
static void end_hung(Run *r)
{
	if (r->c.pid > 0)
		kill(r->c.pid, SIGKILL);
	loop_until_ended(r, ENDED_WAIT_MS);
}

/** await_end_or_entry - Wait for @r's end, but stop as soon as the child has
 * reached a wrapped call while the lock was held: it will never end.
 * Return: 1 when it ended by itself. */
static int await_end_or_entry(Run *r)
{
	long deadline = now_ms() + HUNG_BOUND_MS;

	while (r->calls == 0 && shared->entered[0] == '\0' && now_ms() < deadline)
		wpe_fd_poll(LOOP_SLICE_MS);
	return r->calls > 0;
}

/** await_entry - Wait until some child has reached a wrapped call, or the
 * bound: for a staging child that is not one the owner follows. */
static void await_entry(void)
{
	long deadline = now_ms() + HUNG_BOUND_MS;

	while (shared->entered[0] == '\0' && now_ms() < deadline)
		wpe_fd_poll(LOOP_SLICE_MS);
}

/** say_what_hung - Name the call a child blocked in. */
static void say_what_hung(const char *row)
{
	if (shared->entered[0] != '\0')
		printf("FAIL: %s: the child called %s while another thread held a lock\n",
		       row, shared->entered);
	else
		printf("FAIL: %s: the child did not end within its bound\n", row);
	fails++;
}

/** read_master - What a terminal's editor end reads until a line end, or @ms.
 *
 * A row reads the master before it waits for the child's end: the BSD pty
 * masters keep no text once the child that wrote it has ended, so a row that
 * waits first reads an empty buffer and misses what the child said. */
static void read_master(int master, char *buf, size_t n, long ms)
{
	long deadline = now_ms() + ms;
	size_t len = 0;
	ssize_t got;

	buf[0] = '\0';
	while (now_ms() < deadline && strchr(buf, '\n') == NULL) {
		got = read(master, buf + len, n - 1 - len);
		if (got > 0) {
			len += (size_t)got;
			buf[len] = '\0';
		} else
			wpe_fd_poll(LOOP_SLICE_MS);
	}
}

/* ---- the staging proves itself --------------------------------------------- */

/* The wrap is what makes rows 01 and 02 mean something, so it is proved live
 * in this build's flags (fortify, optimisation) before they run: a process
 * forked while another thread holds the lock blocks in strerror and the test
 * reads its name; with no thread holding it, the same call goes through. */
/* @scenario SC-child-exec-01 */
static void test_the_staging_blocks_a_child_in_a_wrapped_call(void)
{
	int st = 0;
	pid_t pid;

	shared_reset();
	holder_start();
	pid = fork();
	if (pid == 0) {
		(void)strerror(ENOENT);
		_exit(0);
	}
	chk_true("a staging child", pid > 0);
	await_entry();
	chk_str("the held child reached strerror and blocked", shared->entered,
	        "strerror");
	kill(pid, SIGKILL);
	waitpid(pid, &st, 0);
	holder_stop();

	shared_reset();
	pid = fork();
	if (pid == 0) {
		(void)strerror(ENOENT);
		_exit(0);
	}
	chk_true("a staging child with no holder", pid > 0);
	waitpid(pid, &st, 0);
	chk("with no holder the same call goes through", exit_code(st), 0);
	chk_str("and it is still seen", shared->entered, "strerror");
}

/* ---- 01: a build step while another thread holds a lock --------------------- */

/* A build step shows its command line first and runs.  Another thread holds a
 * lock at the fork: the child still runs the program and the line is in the
 * output, in front of the program's own. */
/* @scenario SC-child-exec-01 */
static void test_a_build_step_runs_while_another_thread_holds_a_lock(void)
{
	static Run r;
	WpeChildSpec spec;
	char *argv[] = { "sh", "-c", "exit 0", NULL };
	int ended;

	shared_reset();
	memset(&r, 0, sizeof r);
	spec_for(&spec, argv);
	spec.echo_argv = 1;
	spec.exec_failed = FAILED_PREFIX;
	holder_start();
	chk("the build step starts", wpe_child_start(&spec, &r.c, on_ended, &r), 0);
	ended = await_end_or_entry(&r);
	if (!ended) {
		say_what_hung("a build step with a lock held by another thread");
		end_hung(&r);
	} else {
		chk("it ran", exit_code(r.status), 0);
		chk_str("the command line is the output", r.at_end, "sh -c exit 0 \n");
	}
	holder_stop();
	run_close_ends(&r);
}

/* ---- 02: a refusal said while another thread holds a lock -------------------- */

/* A folder the child cannot enter is named on its terminal, the way a shell
 * names it (jdb's Java VM), with another thread holding a lock at the fork. */
/* @scenario SC-child-exec-02 */
static void test_a_refusal_is_said_while_another_thread_holds_a_lock(void)
{
	static Run r;
	WpeChildSpec spec;
	char *argv[] = { "sh", "-c", "exit 0", NULL };
	char buf[OUT_MAX], name[PTY_NAME_MAX];
	int master, slave, ended;

	shared_reset();
	memset(&r, 0, sizeof r);
	chk("a pty", wpe_child_pty_make(&master, &slave, name, sizeof name), 0);
	spec_for(&spec, argv);
	spec.io = WPE_CHILD_IO_TTY_PATH;
	spec.tty_fd = slave;
	spec.say_failed = 1;
	spec.cwd = NO_SUCH_DIR;
	holder_start();
	chk("the start", wpe_child_start(&spec, &r.c, on_ended, &r), 0);
	close(slave);
	read_master(master, buf, sizeof buf, HUNG_BOUND_MS);
	ended = await_end_or_entry(&r);
	if (!ended) {
		say_what_hung("a refusal with a lock held by another thread");
		end_hung(&r);
	} else {
		chk_true("the folder is named on its terminal (lock held)",
		         strstr(buf, "cannot enter " NO_SUCH_DIR ": " REASON_ENOENT) != NULL);
		chk("it ended as a command that cannot run", exit_code(r.status),
		    SAID_FAILED_EXIT);
	}
	holder_stop();
	close(master);
}

/* ---- 02b: a terminal the system refuses, said while a lock is held ----------- */

/** ctty_owner_start - Have another session own the terminal @slave_name as
 * its controlling terminal, so that no other session can be given it.
 * Return: the owner's process number; it waits until it is killed. */
static pid_t ctty_owner_start(const char *slave_name)
{
	int up[2];
	pid_t pid;
	char b = 'o';
	int fd;

	chk("the owner's pipe", pipe(up), 0);
	pid = fork();
	if (pid == 0) {
		close(up[0]);
		setsid();
		fd = open(slave_name, O_RDWR);
		if (fd >= 0 && ioctl(fd, TIOCSCTTY, 0) == 0 &&
		    write(up[1], &b, 1) == 1)
			pause();
		_exit(1);
	}
	close(up[1]);
	chk_true("another session owns the terminal", read(up[0], &b, 1) == 1);
	close(up[0]);
	return pid;
}

/** ctty_owner_stop - End the owner by its exact number. */
static void ctty_owner_stop(pid_t pid)
{
	int st;

	if (pid <= 0)
		return;
	kill(pid, SIGKILL);
	waitpid(pid, &st, 0);
}

/* A child that is to have a terminal and is refused it (the terminal belongs
 * to another session) says why on that terminal and ends as a command that
 * cannot run.  Another thread holds a lock at the fork: the refusal must not
 * call anything that takes one. */
/* @scenario SC-child-exec-02b */
static void test_a_terminal_refusal_is_said_while_another_thread_holds_a_lock(void)
{
	static Run r;
	WpeChildSpec spec;
	char *argv[] = { "sh", "-c", "exit 0", NULL };
	char buf[OUT_MAX], name[PTY_NAME_MAX];
	int master, slave, ended;
	long deadline;
	size_t len = 0;
	ssize_t got;
	pid_t owner;

	shared_reset();
	memset(&r, 0, sizeof r);
	chk("a pty", wpe_child_pty_make(&master, &slave, name, sizeof name), 0);
	owner = ctty_owner_start(name);
	spec_for(&spec, argv);
	spec.io = WPE_CHILD_IO_TTY_PATH;
	spec.tty_fd = slave;
	holder_start();
	chk("the start", wpe_child_start(&spec, &r.c, on_ended, &r), 0);
	close(slave);
	ended = await_end_or_entry(&r);
	if (!ended) {
		say_what_hung("a terminal refusal with a lock held by another thread");
		end_hung(&r);
	} else {
		buf[0] = '\0';
		deadline = now_ms() + HUNG_BOUND_MS;
		while (now_ms() < deadline && strstr(buf, "\n") == NULL) {
			got = read(master, buf + len, sizeof buf - 1 - len);
			if (got > 0) {
				len += (size_t)got;
				buf[len] = '\0';
			} else
				wpe_fd_poll(LOOP_SLICE_MS);
		}
		chk_true("the refusal is named on its terminal",
		         strstr(buf, "cannot give " WHAT " a terminal: " REASON_EPERM)
		         != NULL);
		chk("it ended as a command that cannot run", exit_code(r.status),
		    SAID_FAILED_EXIT);
	}
	holder_stop();
	ctty_owner_stop(owner);
	close(master);
}

/* ---- 03: the words of a failed start, in each stream -------------------------- */

/** words_build_shape - The build's merged stream: the command line, then the
 * failed line, both in the order the child wrote them, already in the pipe
 * when the end is reported. */
static void words_build_shape(void)
{
	static Run r;
	WpeChildSpec spec;
	char *argv[] = { NO_SUCH_PROGRAM, NULL };

	memset(&r, 0, sizeof r);
	spec_for(&spec, argv);
	spec.echo_argv = 1;
	spec.exec_failed = FAILED_PREFIX;
	chk("the build starts", wpe_child_start(&spec, &r.c, on_ended, &r), 0);
	chk_true("it ended", loop_until_ended(&r, ENDED_WAIT_MS));
	chk_str("the build's words, command line first",  r.at_end,
	        NO_SUCH_PROGRAM " \n" FAILED_PREFIX " " NO_SUCH_PROGRAM " \n");
	chk("the build's status", exit_code(r.status), EXEC_FAILED_EXIT);
	run_close_ends(&r);
}

/** words_jdb_shape - A program that cannot run, said on its terminal. */
static void words_jdb_shape(void)
{
	static Run r;
	WpeChildSpec spec;
	char *argv[] = { NO_SUCH_PROGRAM, NULL };
	char buf[OUT_MAX], name[PTY_NAME_MAX];
	int master, slave;

	memset(&r, 0, sizeof r);
	chk("a pty", wpe_child_pty_make(&master, &slave, name, sizeof name), 0);
	spec_for(&spec, argv);
	spec.io = WPE_CHILD_IO_TTY_PATH;
	spec.tty_fd = slave;
	spec.say_failed = 1;
	chk("the start", wpe_child_start(&spec, &r.c, on_ended, &r), 0);
	close(slave);
	read_master(master, buf, sizeof buf, ENDED_WAIT_MS);
	chk_true("the program is named on its terminal",
	         strstr(buf, "cannot run " NO_SUCH_PROGRAM ": " REASON_ENOENT "\r") != NULL);
	chk_true("it ended", loop_until_ended(&r, ENDED_WAIT_MS));
	chk("as a command that cannot run", exit_code(r.status), SAID_FAILED_EXIT);
	close(master);
}

/** words_jdb_folder - A folder that is gone, said on its terminal. */
static void words_jdb_folder(void)
{
	static Run r;
	WpeChildSpec spec;
	char *argv[] = { "sh", "-c", "exit 0", NULL };
	char buf[OUT_MAX], name[PTY_NAME_MAX];
	int master, slave;

	memset(&r, 0, sizeof r);
	chk("a pty", wpe_child_pty_make(&master, &slave, name, sizeof name), 0);
	spec_for(&spec, argv);
	spec.io = WPE_CHILD_IO_TTY_PATH;
	spec.tty_fd = slave;
	spec.say_failed = 1;
	spec.cwd = NO_SUCH_DIR;
	chk("the start", wpe_child_start(&spec, &r.c, on_ended, &r), 0);
	close(slave);
	read_master(master, buf, sizeof buf, ENDED_WAIT_MS);
	chk_true("the folder is named on its terminal (plain)",
	         strstr(buf, "cannot enter " NO_SUCH_DIR ": " REASON_ENOENT "\r") != NULL);
	chk_true("it ended", loop_until_ended(&r, ENDED_WAIT_MS));
	chk("as a command that cannot run", exit_code(r.status), SAID_FAILED_EXIT);
	close(master);
}

/** words_own_stderr - A child with a stderr pipe of its own: the failed line
 * on that pipe, nothing on stdout. */
static void words_own_stderr(void)
{
	static Run r;
	WpeChildSpec spec;
	char *argv[] = { NO_SUCH_PROGRAM, NULL };

	memset(&r, 0, sizeof r);
	spec_for(&spec, argv);
	spec.stderr_to_stdout = 0;
	spec.exec_failed = FAILED_PREFIX;
	chk("the start", wpe_child_start(&spec, &r.c, on_ended, &r), 0);
	chk_true("it ended", loop_until_ended(&r, ENDED_WAIT_MS));
	chk_str("the failed line is on the error pipe", r.at_end_err,
	        FAILED_PREFIX " " NO_SUCH_PROGRAM " \n");
	chk_str("nothing is on the output pipe", r.at_end, "");
	chk("status", exit_code(r.status), EXEC_FAILED_EXIT);
	run_close_ends(&r);
}

/** words_stderr_to_null - A child whose errors go nowhere says nothing. */
static void words_stderr_to_null(void)
{
	static Run r;
	WpeChildSpec spec;
	char *argv[] = { NO_SUCH_PROGRAM, NULL };

	memset(&r, 0, sizeof r);
	spec_for(&spec, argv);
	spec.stderr_to_stdout = 0;
	spec.stderr_to_null = 1;
	spec.echo_argv = 1;
	spec.exec_failed = FAILED_PREFIX;
	chk("the start", wpe_child_start(&spec, &r.c, on_ended, &r), 0);
	chk_true("it ended", loop_until_ended(&r, ENDED_WAIT_MS));
	chk_str("nothing is said on the output", r.at_end, "");
	chk("status", exit_code(r.status), EXEC_FAILED_EXIT);
	run_close_ends(&r);
}

/** words_terminal_path - A terminal named by path that will not open, on a
 * child with no pty of its own: said on the editor's own stderr, where the
 * child still is, and ended as a command that cannot run. */
static void words_terminal_path(void)
{
	static Run r;
	WpeChildSpec spec;
	char *argv[] = { "sh", "-c", "exit 0", NULL };
	char buf[OUT_MAX];
	int keep, p[2];

	memset(&r, 0, sizeof r);
	spec_for(&spec, argv);
	spec.io = WPE_CHILD_IO_TTY_PATH;
	spec.tty_path = NO_SUCH_TTY;
	spec.say_failed = 1;
	chk("a pipe for stderr", pipe(p), 0);
	keep = dup(STDERR_FILENO);
	dup2(p[1], STDERR_FILENO);
	chk("the start", wpe_child_start(&spec, &r.c, on_ended, &r), 0);
	dup2(keep, STDERR_FILENO);
	close(keep);
	close(p[1]);
	chk_true("it ended", loop_until_ended(&r, ENDED_WAIT_MS));
	read_now(p[0], buf, sizeof buf);
	close(p[0]);
	chk_str("the terminal is named on the editor's stderr", buf,
	        "cannot give " WHAT " a terminal: " REASON_ENOENT "\r\n");
	chk("as a command that cannot run", exit_code(r.status), SAID_FAILED_EXIT);
	run_close_ends(&r);
}

#define LONG_BUF           8192
#define LONG_COMPONENT     199      /* one folder name, under the system's limit */
/* The two lengths that depend on the system's own limit come from the
 * platform owner's number (we_path.h), not from Linux's 4096: the BSDs and
 * macOS refuse a path over 1024 bytes, so a fixed 4000 is refused there. */
#define LONG_FOLDER_FITS_UNDER 96   /* bytes below the limit: a path that fits a head whole */
#define LONG_FOLDER_REFUSED_OVER 55 /* bytes over the limit: one the system refuses for its length */
#define LONG_FOLDER_FITS   (WPE_PLATFORM_PATH_MAX - LONG_FOLDER_FITS_UNDER)
#define LONG_FOLDER_REFUSED (WPE_PLATFORM_PATH_MAX + LONG_FOLDER_REFUSED_OVER)
#define LONG_FOLDER_TOO    4300     /* one that does not fit the head either */
#define LONG_ROOT          "/xwpe-test-no-such"
#define REASON_TOO_LONG    "File name too long"

/** make_long_path - A path of exactly @total bytes in a folder that is not
 * there, made of folder names of LONG_COMPONENT letters. */
static void make_long_path(char *out, size_t total)
{
	size_t len = strlen(LONG_ROOT);

	memcpy(out, LONG_ROOT, len);
	while (len < total) {
		out[len++] = '/';
		while (len < total && (len - strlen(LONG_ROOT)) % (LONG_COMPONENT + 1) != 0)
			out[len++] = 'a';
	}
	out[total] = '\0';
}

/** say_long_folder_lengths - Print the three path lengths the row below used.
 * A failure on another system then shows whether the lengths were the ones
 * that system's limit asks for. */
static void say_long_folder_lengths(void)
{
	printf("note: long folder lengths used: fits %d, refused %d, too long %d (the system's path limit is %d)\n",
	       LONG_FOLDER_FITS, LONG_FOLDER_REFUSED, LONG_FOLDER_TOO,
	       (int)WPE_PLATFORM_PATH_MAX);
}

/** words_long_folder - A folder name as long as a path can be: a name that
 * fits is said whole, with the reason after its ": "; one too long for the
 * head is cut, never the fixed words, so the reader sees where the reason
 * begins. */
static void words_long_folder(void)
{
	static Run r;
	static char cwd[LONG_FOLDER_TOO + 1], buf[LONG_BUF], want[LONG_BUF];
	WpeChildSpec spec;
	char *argv[] = { "sh", "-c", "exit 0", NULL };
	char name[PTY_NAME_MAX];
	int master, slave, i;
	size_t totals[3] = { LONG_FOLDER_FITS, LONG_FOLDER_REFUSED, LONG_FOLDER_TOO };

	say_long_folder_lengths();
	for (i = 0; i < 3; i++) {
		make_long_path(cwd, totals[i]);
		memset(&r, 0, sizeof r);
		chk("a pty", wpe_child_pty_make(&master, &slave, name, sizeof name), 0);
		spec_for(&spec, argv);
		spec.io = WPE_CHILD_IO_TTY_PATH;
		spec.tty_fd = slave;
		spec.say_failed = 1;
		spec.cwd = cwd;
		chk("the start", wpe_child_start(&spec, &r.c, on_ended, &r), 0);
		close(slave);
		read_master(master, buf, sizeof buf, ENDED_WAIT_MS);
		if (i == 0) {
			snprintf(want, sizeof want, "cannot enter %s: %s\r", cwd, REASON_ENOENT);
			chk_true("a path that fits is said whole, then its reason",
			         strstr(buf, want) != NULL);
		} else if (i == 1) {
			snprintf(want, sizeof want, "cannot enter %s: %s\r", cwd, REASON_TOO_LONG);
			chk_true("a whole path the system refuses is said whole, then its reason",
			         strstr(buf, want) != NULL);
		} else {
			chk_true("a path too long is cut before the fixed words",
			         strncmp(buf, "cannot enter " LONG_ROOT, 13 + strlen(LONG_ROOT)) == 0 &&
			         strstr(buf, ": " REASON_TOO_LONG "\r") != NULL);
		}
		chk_true("it ended", loop_until_ended(&r, ENDED_WAIT_MS));
		chk("as a command that cannot run", exit_code(r.status), SAID_FAILED_EXIT);
		close(master);
	}
}

/* A start that fails says the same words in the stream the user reads, in the
 * same order, exit 1 for a build and 127 for a program that names its
 * failures. */
/* @scenario SC-child-exec-03 */
static void test_the_words_of_a_failed_start_are_the_same_in_each_stream(void)
{
	words_build_shape();
	words_jdb_shape();
	words_jdb_folder();
	words_own_stderr();
	words_stderr_to_null();
	words_terminal_path();
	words_long_folder();
}

/* ---- 05: the report ----------------------------------------------------------- */

static char trace_lines[TRACE_LINES][TRACE_LINE_SIZE];
static int trace_n;

static void on_trace(const char *line)
{
	if (trace_n < TRACE_LINES)
		snprintf(trace_lines[trace_n++], TRACE_LINE_SIZE, "%s", line);
}

/** trace_count - How many trace lines are exactly @line. */
static int trace_count(const char *line)
{
	int i, n = 0;

	for (i = 0; i < trace_n; i++)
		if (strcmp(trace_lines[i], line) == 0)
			n++;
	return n;
}

/** open_fds - How many descriptors above 2 this process holds. */
static int open_fds(void)
{
	int fd, n = 0;

	for (fd = 3; fd < FD_SCAN_MAX; fd++)
		if (fcntl(fd, F_GETFD) >= 0)
			n++;
	return n;
}

/** report_case - One start, its end, and what the editor traced and kept.
 * @argv: the program; @cwd: its folder or NULL; @want: the exact trace line
 * that must appear once. */
static void report_case(const char *label, char **argv, const char *cwd,
                        const char *want)
{
	static Run r;
	WpeChildSpec spec;
	char what[TRACE_LINE_SIZE];
	int before, after;

	snprintf(what, sizeof what, "%s: the trace says it", label);
	memset(&r, 0, sizeof r);
	spec_for(&spec, argv);
	spec.cwd = cwd;
	trace_n = 0;
	before = open_fds();
	chk("the start", wpe_child_start(&spec, &r.c, on_ended, &r), 0);
	chk_true("it ended", loop_until_ended(&r, ENDED_WAIT_MS));
	run_close_ends(&r);
	after = open_fds();
	chk(what, trace_count(want), 1);
	snprintf(what, sizeof what, "%s: no descriptor is left open", label);
	chk(what, after, before);
}

/* A start that cannot run is traced with its stage and errno, one that runs
 * with "ran", and in neither case does the editor keep a descriptor. */
/* @scenario SC-child-exec-05 */
static void test_the_child_reports_what_failed_and_the_editor_traces_it(void)
{
	char *none[] = { NO_SUCH_PROGRAM, NULL };
	char *quick[] = { "sh", "-c", "exit 0", NULL };
	char want[TRACE_LINE_SIZE];

	wpe_child_trace_set(on_trace);
	report_case("a warm-up start", quick, NULL, "child ran " WHAT);
	snprintf(want, sizeof want, "child report " WHAT " exec %d", ENOENT);
	report_case("an exec that fails", none, NULL, want);
	report_case("a program that runs", quick, NULL, "child ran " WHAT);
	snprintf(want, sizeof want, "child report " WHAT " dir %d", ENOENT);
	report_case("a folder that is gone", quick, NO_SUCH_DIR, want);
	wpe_child_trace_set(NULL);
}

/* ---- 05b: an editor whose 0, 1 and 2 are closed -------------------------------- */

#define LOW_FDS_RESULT_MAX 512
#define LOW_FDS_FILE       "xwpe-test-lowfd"

/** low_fds_fail - In the process with no 0, 1 and 2: note one failure on the
 * result pipe @res (the test's own output would land in the program's file). */
static void low_fds_fail(int res, const char *what)
{
	if (write(res, what, strlen(what)) < 0)
		return;
	if (write(res, "\n", 1) < 0)
		return;
}

/** low_fds_start - In the process with no 0, 1 and 2: start a program that
 * cannot run, its output to the file @path, and wait for its end.
 * Return: 0 when it started and ended. */
static int low_fds_start(int res, const char *path)
{
	static Run r;
	WpeChildSpec spec;
	char *argv[] = { NO_SUCH_PROGRAM, NULL };

	memset(&r, 0, sizeof r);
	spec_for(&spec, argv);
	spec.stdout_path = path;
	trace_n = 0;
	if (wpe_child_start(&spec, &r.c, on_ended, &r) != 0) {
		low_fds_fail(res, "the start was refused");
		return -1;
	}
	if (!loop_until_ended(&r, ENDED_WAIT_MS)) {
		low_fds_fail(res, "the child did not end");
		return -1;
	}
	run_close_ends(&r);
	return 0;
}

/** low_fds_check - In the process with no 0, 1 and 2: the editor's trace says
 * the start stopped at its exec, and the program's file holds nothing: the
 * record went to the editor's end of the report pipe, never to the program's
 * stream. */
static void low_fds_check(int res, const char *path, const char *want)
{
	struct stat st;

	if (trace_count(want) != 1)
		low_fds_fail(res, "the trace does not say the exec failed");
	if (stat(path, &st) != 0)
		low_fds_fail(res, "the program's file was not made");
	else if (st.st_size != 0)
		low_fds_fail(res, "the report reached the program's file");
}

/** low_fds_process - The editor with its 0, 1 and 2 closed after its own
 * set-up (the wake pipe is open already): the report pipe of a start would
 * be given 0 and 1 by the system.  Ends the process. */
static void low_fds_process(int res, const char *path, const char *want)
{
	WpeChildWakeOpen();
	wpe_child_trace_set(on_trace);
	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);
	if (low_fds_start(res, path) == 0)
		low_fds_check(res, path, want);
	_exit(0);
}

/** low_fds_collect - Read what the process with no 0, 1 and 2 noted as
 * failed, and say each line.
 * @res: the read end of its result pipe. */
static void low_fds_collect(int res)
{
	char buf[LOW_FDS_RESULT_MAX];
	ssize_t got = read(res, buf, sizeof buf - 1);

	if (got > 0) {
		buf[got] = '\0';
		printf("FAIL: with 0, 1 and 2 closed: %s", buf);
		fails++;
	}
}

/* An editor started with its standard streams closed gets those numbers for
 * the report pipe of a start.  The child's redirection of its own streams
 * must not replace it, and its record must not reach the program's file. */
/* @scenario SC-child-exec-05b */
static void test_a_start_with_the_standard_streams_closed_reports_to_the_editor(void)
{
	int result[2], st = 0;
	char want[TRACE_LINE_SIZE], path[PATH_MAX];
	pid_t pid;

	snprintf(want, sizeof want, "child report " WHAT " exec %d", ENOENT);
	snprintf(path, sizeof path, LOW_FDS_FILE ".%ld", (long)test_pid);
	unlink(path);
	chk("the result pipe", pipe(result), 0);
	fflush(stdout);
	pid = fork();
	if (pid == 0) {
		close(result[0]);
		low_fds_process(result[1], path, want);
	}
	close(result[1]);
	low_fds_collect(result[0]);
	close(result[0]);
	waitpid(pid, &st, 0);
	chk("the process ended by itself", exit_code(st), 0);
	unlink(path);
	shared_reset();
}

/* ---- 10: no handler of the editor's runs in a child ---------------------------- */

#ifdef WPE_TEST_LD_WRAP
static int chdir_raises;
static const char *gate_path;     /* a named pipe a child waits on before chdir */

int __real_chdir(const char *);

/** wait_at_gate - In a child: wait until the test has opened the gate. */
static void wait_at_gate(void)
{
	char b;
	int fd = open(gate_path, O_RDONLY);

	if (fd < 0)
		return;
	if (read(fd, &b, 1) < 0)
		b = 0;
	close(fd);
}

/* The step before the exec that every child with a folder takes: in a child,
   wait at the gate when a row set one, and raise the signal the row picked,
   so a handler it inherited would run. */
int __wrap_chdir(const char *path)
{
	if (getpid() != test_pid && gate_path)
		wait_at_gate();
	if (getpid() != test_pid && chdir_raises) {
		test_child_dies_on_purpose();
		raise(chdir_raises);
	}
	return __real_chdir(path);
}

static void on_signal(int sig)
{
	shared->handler_sig = sig;
	shared->handler_pid = getpid();
}

static void on_fault(int sig)
{
	on_signal(sig);
	_exit(SEGV_HANDLER_EXIT);
}

/** handler_row - One signal raised in a child whose spec enters a folder.
 * @sig: the signal; @handler: what the test installed for it.
 * Return: the child's wait status, or -1 when it did not end. */
static int handler_row(int sig, void (*handler)(int))
{
	static Run r;
	WpeChildSpec spec;
	char *argv[] = { "sh", "-c", "exit 0", NULL };
	struct sigaction act, old;
	int status = -1;

	shared_reset();
	memset(&r, 0, sizeof r);
	memset(&act, 0, sizeof act);
	act.sa_handler = handler;
	sigemptyset(&act.sa_mask);
	sigaction(sig, &act, &old);
	spec_for(&spec, argv);
	spec.cwd = "/";
	chdir_raises = sig;
	chk("the start", wpe_child_start(&spec, &r.c, on_ended, &r), 0);
	if (loop_until_ended(&r, ENDED_WAIT_MS))
		status = r.status;
	chdir_raises = 0;
	sigaction(sig, &old, NULL);
	run_close_ends(&r);
	return status;
}
#endif

/* A signal a child of the editor receives before its exec runs the default
 * action for it, never a handler the editor installed: a handler would act on
 * a copy of the editor's memory.  An ignored-by-default signal reaches no
 * handler, and a fault ends the child as it ends any program.  (That every
 * signal is blocked across the fork cannot be staged: the window is a few
 * instructions in the parent.) */
/* @scenario SC-child-exec-10 */
static void test_a_child_never_runs_a_handler_of_the_editor(void)
{
#ifdef WPE_TEST_LD_WRAP
	int status;

	status = handler_row(SIGURG, on_signal);
	chk("a signal ignored by default: the child ends cleanly", exit_code(status), 0);
	chk("and no handler of the editor ran in it", shared->handler_pid, 0);
	status = handler_row(SIGSEGV, on_fault);
	chk_true("a fault ends the child by the signal",
	         WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
	chk("and no handler of the editor ran in it", shared->handler_pid, 0);
#else
	printf("SKIP: the handler rows need a linker with --wrap\n");
#endif
}

/* ---- a report nobody reads --------------------------------------------------------- */

/* The editor let go of a child (it was forgotten) before the child told it
 * where its start stopped.  The child closed its own copy of the report
 * pipe's read end with the other descriptors, so its record is written to a
 * pipe nobody reads: that must not kill it by SIGPIPE before it has said why
 * it cannot run.  The child waits in the step before its folder is entered
 * (a named pipe the test opens once the editor has let go). */
/* @scenario SC-child-exec-05 */
static void test_a_report_nobody_reads_does_not_kill_the_child(void)
{
#ifdef WPE_TEST_LD_WRAP
	static Run r;
	WpeChildSpec spec;
	char *argv[] = { "sh", "-c", "exit 0", NULL };
	char buf[OUT_MAX], name[PTY_NAME_MAX], path[PATH_MAX];
	int master, slave, gate, st = 0;
	long deadline;
	size_t len = 0;
	ssize_t got;
	pid_t pid;

	memset(&r, 0, sizeof r);
	snprintf(path, sizeof path, "xwpe-test-gate.%ld", (long)test_pid);
	unlink(path);
	chk("a named pipe for the gate", mkfifo(path, 0600), 0);
	chk("a pty", wpe_child_pty_make(&master, &slave, name, sizeof name), 0);
	spec_for(&spec, argv);
	spec.io = WPE_CHILD_IO_TTY_PATH;
	spec.tty_fd = slave;
	spec.say_failed = 1;
	spec.cwd = NO_SUCH_DIR;
	gate_path = path;
	chk("the start", wpe_child_start(&spec, &r.c, on_ended, &r), 0);
	close(slave);
	pid = r.c.pid;
	wpe_child_forget(&r.c);          /* the editor's end of the report is closed */
	gate = open(path, O_WRONLY);     /* meets the child's open: it goes on */
	chk_true("the gate opens", gate >= 0 && write(gate, "g", 1) == 1);
	if (gate >= 0)
		close(gate);
	buf[0] = '\0';
	deadline = now_ms() + HUNG_BOUND_MS;
	while (now_ms() < deadline && strstr(buf, "\n") == NULL) {
		got = read(master, buf + len, sizeof buf - 1 - len);
		if (got > 0) {
			len += (size_t)got;
			buf[len] = '\0';
		} else
			wpe_fd_poll(LOOP_SLICE_MS);
	}
	chk_true("the folder is still named on its terminal",
	         strstr(buf, "cannot enter " NO_SUCH_DIR ": " REASON_ENOENT) != NULL);
	if (strstr(buf, "cannot enter") == NULL)
		kill(pid, SIGKILL);
	waitpid(pid, &st, 0);
	chk("it ended by itself as a command that cannot run, not by SIGPIPE",
	    exit_code(st), SAID_FAILED_EXIT);
	gate_path = NULL;
	unlink(path);
	close(master);
#else
	printf("SKIP: the unread report row needs a linker with --wrap\n");
#endif
}

/* ---- the program ----------------------------------------------------------------- */

int main(void)
{
	test_pid = getpid();
	shared = mmap(NULL, sizeof *shared, PROT_READ | PROT_WRITE,
	              MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (shared == MAP_FAILED) {
		printf("FAIL: no shared memory\n");
		return 1;
	}
	test_watchdog_case("test_childsafe (what a child may do between fork and "
	                   "exec): a child never ended", HANG_SECONDS);
	WpeChildWakeOpen();
	wpe_child_say_set(on_say);
	chk("the reaper installs", wpe_child_reap_install(), 0);

#ifdef WPE_TEST_LD_WRAP
	test_the_staging_blocks_a_child_in_a_wrapped_call();
	test_a_build_step_runs_while_another_thread_holds_a_lock();
	test_a_refusal_is_said_while_another_thread_holds_a_lock();
	test_a_terminal_refusal_is_said_while_another_thread_holds_a_lock();
#else
	printf("SKIP: rows 01, 02 and 02b need a linker with --wrap\n");
#endif
	test_the_words_of_a_failed_start_are_the_same_in_each_stream();
	test_the_child_reports_what_failed_and_the_editor_traces_it();
	test_a_child_never_runs_a_handler_of_the_editor();
	test_a_report_nobody_reads_does_not_kill_the_child();
	test_a_start_with_the_standard_streams_closed_reports_to_the_editor();

	if (fails) {
		printf("%d failure(s)\n", fails);
		return 1;
	}
	printf("ok\n");
	return 0;
}
