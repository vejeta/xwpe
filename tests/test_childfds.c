/* test_childfds.c -- which descriptors a program the editor starts holds.
 * Links we_child.c and we_fdloop.c only; no editor -- always runs.
 *
 * The owner's close step (wpe_child_close_others) leaves a child 0, 1 and 2
 * and what its io kind names: the program's side of a debugger's pty, and
 * nothing for any other kind.  What the editor inherited from whatever
 * started it (a desktop launcher, a CI runner, a shell with a coprocess)
 * never reaches a program.  The rows plant descriptors without close-on-exec
 * in this process, start every kind of child with a probe (this program run
 * again with --fd-probe FILE) that lists every descriptor it holds above 2,
 * and read the list from FILE.  The probe scans to PROBE_SCAN_MAX, above
 * every number a row plants, so a descriptor left open high up is seen.
 *
 * Built twice (Makefile.am): test_childfds takes the system's calls
 * (close_range, closefrom) and test_childfds_loop, compiled with
 * WPE_CHILD_CLOSE_LOOP_ONLY, the loop over the descriptor table that macOS
 * runs, so Linux proves the loop and its ceiling.  The loop build prints
 * what the close step took.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "../we_child.h"
#include "../we_fdloop.h"
#include "test_watchdog.h"

#ifndef MAP_ANONYMOUS
# define MAP_ANONYMOUS MAP_ANON
#endif

#define HANG_SECONDS       30
#define FD_PROBE_ARG       "--fd-probe"
#define PROBE_SCAN_MAX     1024  /* above every number a row plants */
#define REPORT_MAX         64
#define PROBE_TYPE_MAX     16
#define PROBE_LINE_MAX     160
#define LOOP_SLICE_MS      20
#define ENDED_WAIT_MS      test_scaled_ms(10000)
#define PTY_NAME_MAX       64
#define HIGH_LEAK_FD       300   /* above the soft limit a row sets */
#define LOW_SOFT_LIMIT     256
#define CLOSE_NOT_TIMED    (-1L)
#define MS_PER_S           1000L
#define NS_PER_MS          1000000L

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

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * MS_PER_S + ts.tv_nsec / NS_PER_MS;
}

/* ---- the probe: the program the rows start ---------------------------- */

/** probe_type - The kind of object a descriptor is, as one word. */
static const char *probe_type(const struct stat *st)
{
	if (S_ISSOCK(st->st_mode))
		return "sock";
	if (S_ISFIFO(st->st_mode))
		return "fifo";
	if (S_ISCHR(st->st_mode))
		return "chr";
	if (S_ISREG(st->st_mode))
		return "reg";
	return "other";
}

/** probe_main - What a child holds above 0, 1 and 2, one line a descriptor
 * in @path: its number, kind, close-on-exec flag, device and whether it is
 * a terminal.  The file is opened after the scan, so it is never listed. */
static int probe_main(const char *path)
{
	struct { int fd, cloexec, tty; unsigned long rdev; const char *type; } e[REPORT_MAX];
	struct stat st;
	FILE *out;
	int fd, flags, n = 0, i;

	for (fd = 3; fd < PROBE_SCAN_MAX && n < REPORT_MAX; fd++) {
		flags = fcntl(fd, F_GETFD);
		if (flags < 0)
			continue;
		e[n].fd = fd;
		e[n].cloexec = (flags & FD_CLOEXEC) != 0;
		e[n].tty = isatty(fd);
		e[n].rdev = 0;
		e[n].type = "other";
		if (fstat(fd, &st) == 0) {
			e[n].type = probe_type(&st);
			e[n].rdev = S_ISCHR(st.st_mode) ? (unsigned long)st.st_rdev : 0;
		}
		n++;
	}
	out = fopen(path, "w");
	if (out == NULL)
		return 2;
	for (i = 0; i < n; i++)
		fprintf(out, "fd=%d type=%s cloexec=%d rdev=%lx tty=%d\n", e[i].fd,
		        e[i].type, e[i].cloexec, e[i].rdev, e[i].tty);
	return fclose(out) == 0 ? 0 : 2;
}

/* ---- reading what a probe said ---------------------------------------- */

typedef struct {
	int fd, cloexec, tty;
	unsigned long rdev;
	char type[PROBE_TYPE_MAX];
} ProbeEntry;

typedef struct {
	ProbeEntry e[REPORT_MAX];
	int n;
} Report;

/** report_read - The descriptors a probe listed in @path.
 * Return: how many, or -1 when the file is missing (the probe did not run). */
static int report_read(const char *path, Report *rep)
{
	char line[PROBE_LINE_MAX];
	FILE *in = fopen(path, "r");
	ProbeEntry *p;

	rep->n = 0;
	if (in == NULL)
		return -1;
	while (rep->n < REPORT_MAX && fgets(line, sizeof line, in) != NULL) {
		p = &rep->e[rep->n];
		if (sscanf(line, "fd=%d type=%15s cloexec=%d rdev=%lx tty=%d", &p->fd,
		           p->type, &p->cloexec, &p->rdev, &p->tty) == 5)
			rep->n++;
	}
	fclose(in);
	return rep->n;
}

/** report_find - The entry for descriptor @fd, or NULL. */
static const ProbeEntry *report_find(const Report *rep, int fd)
{
	int i;

	for (i = 0; i < rep->n; i++)
		if (rep->e[i].fd == fd)
			return &rep->e[i];
	return NULL;
}

/** report_print - Name every descriptor a child held, for a failure. */
static void report_print(const char *what, const Report *rep)
{
	int i;

	printf("  %s held %d:", what, rep->n);
	for (i = 0; i < rep->n; i++)
		printf(" [fd=%d %s cloexec=%d]", rep->e[i].fd, rep->e[i].type,
		       rep->e[i].cloexec);
	printf("\n");
}

/* ---- starting a probe -------------------------------------------------- */

typedef struct {
	WpeChild c;
	int calls;
} Run;

static const char *self_path;
static char report_path[PATH_MAX];

static void on_ended(WpeChild *c, int status, int how, void *data)
{
	Run *r = data;

	(void)c;
	(void)status;
	(void)how;
	r->calls++;
}

static void on_say(const char *line)
{
	(void)line;
}

/** loop_until_ended - Run the editor's loop until @r's child has ended. */
static void loop_until_ended(Run *r)
{
	long deadline = now_ms() + ENDED_WAIT_MS;

	while (r->calls == 0 && now_ms() < deadline)
		wpe_fd_poll(LOOP_SLICE_MS);
	chk_true("the probe ended", r->calls > 0);
}

/** spec_probe - A start of this program as the probe of @path, of io kind
 * @io, in a session of its own, as every start plan of the editor's. */
static void spec_probe(WpeChildSpec *spec, char **argv, WpeChildIo io)
{
	memset(spec, 0, sizeof *spec);
	argv[0] = (char *)self_path;
	argv[1] = (char *)FD_PROBE_ARG;
	argv[2] = report_path;
	argv[3] = NULL;
	spec->argv = argv;
	spec->io = io;
	spec->group = WPE_CHILD_OWN_SESSION;
	spec->stdin_null = 1;
	spec->stderr_to_stdout = 1;
	spec->side_pty_slave = WPE_CHILD_NO_FD;
	spec->tty_fd = WPE_CHILD_NO_FD;
	spec->say_failed = 1;
	spec->what = "the test probe";
}

/** start_and_read - Start @spec, wait for its end, read what it listed.
 * @r: receives the child, whose ends the caller closes. */
static void start_and_read(WpeChildSpec *spec, Run *r, Report *rep,
                           const char *what)
{
	memset(r, 0, sizeof *r);
	unlink(report_path);
	chk(what, wpe_child_start(spec, &r->c, on_ended, r), 0);
	loop_until_ended(r);
	chk("the probe's report was read", report_read(report_path, rep) >= 0, 1);
}

/** run_close_ends - Close the editor's ends of a finished child. */
static void run_close_ends(Run *r)
{
	if (r->c.out_fd >= 0)
		close(r->c.out_fd);
	if (r->c.pty_fd >= 0)
		close(r->c.pty_fd);
	r->c.out_fd = WPE_CHILD_NO_FD;
	r->c.pty_fd = WPE_CHILD_NO_FD;
}

/* ---- the six kinds of child -------------------------------------------- */

typedef struct {
	const char *name;
	WpeChildIo io;
	int side_pty;    /* the editor's pty slave is named to the child */
	int tty_fd;      /* the editor's pty slave is handed as tty_fd */
} KindCase;

static const KindCase kinds[] = {
	{ "PIPES (build, language server, assistant host)", WPE_CHILD_IO_PIPES, 0, 0 },
	{ "PTY (Run, dlv)", WPE_CHILD_IO_PTY, 0, 0 },
	{ "PIPES_AND_PTY (gdb, the line debuggers)", WPE_CHILD_IO_PIPES_AND_PTY, 1, 0 },
	{ "INHERIT_AND_PTY (the graphical debugger script)",
	  WPE_CHILD_IO_INHERIT_AND_PTY, 1, 0 },
	{ "TTY_PATH with tty_fd (jdb's VM)", WPE_CHILD_IO_TTY_PATH, 0, 1 },
	{ "INHERIT (Shell, Execute)", WPE_CHILD_IO_INHERIT, 0, 0 },
};
#define KIND_COUNT ((int)(sizeof kinds / sizeof kinds[0]))

typedef struct {
	Report rep;
	int master, slave;           /* the editor's pty, or WPE_CHILD_NO_FD */
	unsigned long slave_rdev;    /* the slave's device, as the editor sees it */
} KindRun;

/** kind_run - Start a probe of kind @k and read what it held.
 * The caller closes the pty with kind_close. */
static void kind_run(const KindCase *k, KindRun *kr)
{
	WpeChildSpec spec;
	static Run r;
	char *argv[4], name[PTY_NAME_MAX];
	struct stat st;

	kr->master = WPE_CHILD_NO_FD;
	kr->slave = WPE_CHILD_NO_FD;
	kr->slave_rdev = 0;
	spec_probe(&spec, argv, k->io);
	if (k->io == WPE_CHILD_IO_PTY)
		spec.pty_raw = 1;
	if (k->side_pty || k->tty_fd) {
		chk("a pty", wpe_child_pty_make(&kr->master, &kr->slave, name,
		                                 sizeof name), 0);
		if (fstat(kr->slave, &st) == 0)
			kr->slave_rdev = (unsigned long)st.st_rdev;
	}
	if (k->side_pty)
		spec.side_pty_slave = kr->slave;
	if (k->tty_fd)
		spec.tty_fd = kr->slave;
	start_and_read(&spec, &r, &kr->rep, k->name);
	run_close_ends(&r);
}

/** kind_close - Close the editor's pty of a kind's run. */
static void kind_close(KindRun *kr)
{
	if (kr->master >= 0)
		close(kr->master);
	if (kr->slave >= 0)
		close(kr->slave);
}

/* ---- the descriptors the editor "inherited" ---------------------------- */

/** plant_inherited - What a launcher or a CI runner leaves open in the
 * editor, none with close-on-exec: a file, a socket and a pipe. */
static void plant_inherited(void)
{
	int sv[2], pp[2];

	chk_true("a leaked file", open("/dev/null", O_RDONLY) >= 0);
	chk("a leaked socketpair", socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
	chk("a leaked pipe", pipe(pp), 0);
	chk("the socket is not close-on-exec", fcntl(sv[0], F_GETFD) & FD_CLOEXEC, 0);
	chk("the pipe is not close-on-exec", fcntl(pp[0], F_GETFD) & FD_CLOEXEC, 0);
}

/* @scenario SC-child-fds-01 */
static void test_every_kind_of_child_starts_with_only_its_kept_descriptors(void)
{
	static KindRun kr;
	int i, want;
	const KindCase *k;

	for (i = 0; i < KIND_COUNT; i++) {
		k = &kinds[i];
		kind_run(k, &kr);
		want = k->side_pty ? 1 : 0;
		chk(k->name, kr.rep.n, want);
		if (kr.rep.n != want)
			report_print(k->name, &kr.rep);
		if (k->side_pty)
			chk_true("and that one is the program's side of the pty",
			         report_find(&kr.rep, kr.slave) != NULL);
		kind_close(&kr);
	}
}

/* @scenario SC-child-fds-02 */
static void test_the_debuggers_side_pty_is_still_the_programs(void)
{
	static KindRun kr;
	const ProbeEntry *e;
	int i;

	for (i = 0; i < KIND_COUNT; i++) {
		if (!kinds[i].side_pty)
			continue;
		kind_run(&kinds[i], &kr);
		e = report_find(&kr.rep, kr.slave);
		chk_true("the side pty's number is open in the child", e != NULL);
		if (e != NULL) {
			chk_true("it is a terminal device", strcmp(e->type, "chr") == 0);
			chk_true("it is the editor's slave, not another device",
			         e->rdev == kr.slave_rdev);
			chk("and isatty says so", e->tty, 1);
		}
		chk_true("the editor's master is not in the child",
		         report_find(&kr.rep, kr.master) == NULL);
		kind_close(&kr);
	}
}

/* @scenario SC-child-fds-03 */
static void test_a_leaked_descriptor_above_the_soft_limit_is_closed(void)
{
	static KindRun kr;
	struct rlimit old, low;
	int leak;

	getrlimit(RLIMIT_NOFILE, &old);
	if (old.rlim_max != RLIM_INFINITY && old.rlim_max <= HIGH_LEAK_FD) {
		printf("SKIP: SC-child-fds-03: the hard limit %lu is below %d\n",
		       (unsigned long)old.rlim_max, HIGH_LEAK_FD);
		return;
	}
	leak = open("/dev/null", O_RDONLY);
	chk("a leak", leak >= 0, 1);
	chk("moved above the soft limit to come", dup2(leak, HIGH_LEAK_FD),
	    HIGH_LEAK_FD);
	close(leak);
	low = old;
	low.rlim_cur = LOW_SOFT_LIMIT;
	chk("the soft limit is lowered", setrlimit(RLIMIT_NOFILE, &low), 0);
	kind_run(&kinds[0], &kr);
	chk("the limit is restored", setrlimit(RLIMIT_NOFILE, &old), 0);
	chk_true("the leak above the soft limit is not in the child",
	         report_find(&kr.rep, HIGH_LEAK_FD) == NULL);
	chk("and the child holds nothing above 2", kr.rep.n, 0);
	if (kr.rep.n != 0)
		report_print("the child", &kr.rep);
	close(HIGH_LEAK_FD);
}

/* @scenario SC-child-fds-04 */
#ifdef WPE_CHILD_CLOSE_LOOP_ONLY
extern long *wpe_child_close_timing;

/* The loop build: the close step ran as the descriptor-by-descriptor loop
 * (rows 01-03 above ran on it), and what it took in the child is printed, so
 * a run on another system measures that system's cost. */
static void test_the_loop_build_closes_by_loop_and_says_what_it_cost(void)
{
	static KindRun kr;
	struct rlimit hard;
	long *slot = mmap(NULL, sizeof *slot, PROT_READ | PROT_WRITE,
	                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);

	chk_true("shared memory for the timing", slot != MAP_FAILED);
	if (slot == MAP_FAILED)
		return;
	*slot = CLOSE_NOT_TIMED;
	wpe_child_close_timing = slot;
	kind_run(&kinds[0], &kr);
	wpe_child_close_timing = NULL;
	chk_true("the close step reported its time", *slot != CLOSE_NOT_TIMED);
	getrlimit(RLIMIT_NOFILE, &hard);
	printf("INFO: close step, loop build: %ld us (hard limit %lu, ceiling "
	       "at most 65536)\n", *slot, (unsigned long)hard.rlim_max);
	munmap(slot, sizeof *slot);
}
#else
static void test_the_loop_build_closes_by_loop_and_says_what_it_cost(void)
{
	printf("SKIP: SC-child-fds-04: the system build; rows 01-03 ran on its "
	       "calls, test_childfds_loop runs them on the loop\n");
}
#endif

int main(int argc, char **argv)
{
	char resolved[PATH_MAX];

	if (argc > 2 && strcmp(argv[1], FD_PROBE_ARG) == 0)
		return probe_main(argv[2]);
	self_path = realpath(argv[0], resolved) ? strdup(resolved) : argv[0];
	snprintf(report_path, sizeof report_path, "%s/childfds-report.%ld",
	         getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (long)getpid());
	test_watchdog_case("test_childfds (the descriptors a child keeps): a "
	                   "child's end never came", HANG_SECONDS);
	WpeChildWakeOpen();
	wpe_child_say_set(on_say);
	chk("the reaper installs", wpe_child_reap_install(), 0);
	plant_inherited();

	test_every_kind_of_child_starts_with_only_its_kept_descriptors();
	test_the_debuggers_side_pty_is_still_the_programs();
	test_a_leaked_descriptor_above_the_soft_limit_is_closed();
	test_the_loop_build_closes_by_loop_and_says_what_it_cost();

	unlink(report_path);
	if (fails) {
		printf("%d failure(s)\n", fails);
		return 1;
	}
	printf("ok\n");
	return 0;
}
