/* test_fdloop.c -- the fd-loop must reap a hung-up fd, not spin on it.
 *
 * Regression for the 100% CPU spin: when a registered fd (an LSP / debug
 * server that exited) reaches EOF, poll() reports POLLHUP -- and, once the
 * last bytes are drained, POLLHUP WITHOUT POLLIN.  wpe_fd_poll() used to
 * dispatch only on POLLIN|POLLPRI, so the read callback that reaps the dead
 * fd never ran; poll() then returned instantly forever and the input loop
 * burned a core.  wpe_fd_poll() now dispatches on POLLHUP/POLLERR too (and
 * drops wake-only fds that hang up), so the fd is reaped on the first poll.
 *
 * The timers of the same loop are checked here too: a waiting line that must
 * turn, and a deadline that must end a wait on a silent child, rely on them
 * firing on time with no fd awake, without bursts, and surviving callbacks
 * that change the timer table.
 */
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include "we_fdloop.h"

static int cb_calls = 0;
static int reaped_fd = -1;

/* Mimics e_lsp_on_fd_readable / e_d_on_gdb_readable: drain, and on EOF reap. */
static void on_ready(int fd, void *data)
{
 char buf[64];
 ssize_t n = read(fd, buf, sizeof buf);
 (void)data;
 cb_calls++;
 if (n <= 0) { reaped_fd = fd; wpe_fd_del(fd); }
}

static int eof_pipe(void)
{
 int p[2];
 if (pipe(p) != 0) return -1;
 close(p[1]);              /* write end closed -> read end is at EOF (POLLHUP) */
 return p[0];
}

/* A callback fd at EOF is dispatched (reaped) once, then the loop is quiet. */
static int test_callback_fd_reaped_on_hup(void)
{
 int fd = eof_pipe();
 int r1, r2;
 if (fd < 0) return 1;
 cb_calls = 0; reaped_fd = -1;
 wpe_fd_add(fd, POLLIN, on_ready, NULL);
 r1 = wpe_fd_poll(0);     /* HUP must dispatch the callback, which reaps fd */
 r2 = wpe_fd_poll(0);     /* fd gone -> no work, NO spin */
 if (cb_calls != 1)  { printf("FAIL: callback not dispatched on HUP (calls=%d)\n", cb_calls); return 1; }
 if (reaped_fd != fd){ printf("FAIL: fd not reaped (%d)\n", reaped_fd); return 1; }
 if (r1 <= 0)        { printf("FAIL: poll did not report the hung-up fd (r1=%d)\n", r1); return 1; }
 if (r2 != 0)        { printf("FAIL: loop still busy after reap (r2=%d -> spin)\n", r2); return 1; }
 return 0;
}

/* A wake-only fd (NULL callback, like stdin/X11) that hangs up is dropped,
 * not left to spin. */
static int test_wakeonly_fd_dropped_on_hup(void)
{
 int fd = eof_pipe();
 int r1, r2;
 if (fd < 0) return 1;
 wpe_fd_add(fd, POLLIN, NULL, NULL);
 r1 = wpe_fd_poll(0);     /* HUP on a NULL-callback fd -> drop it */
 r2 = wpe_fd_poll(0);     /* dropped -> quiet */
 close(fd);
 if (r1 <= 0) { printf("FAIL: poll did not report wake-only HUP (r1=%d)\n", r1); return 1; }
 if (r2 != 0) { printf("FAIL: wake-only hung-up fd not dropped (r2=%d -> spin)\n", r2); return 1; }
 return 0;
}

/* --- timers: the waiting lines and deadlines tick on these, on every system. */

#define TICK_MS        20
#define SLOW_TICK_MS   60
#define LATE_MS        150
#define TIMING_SLACK_MS 10
#define NEVER_MS       2000
#define HANG_GUARD_S   10

static int fired_a = 0;
static int fired_b = 0;
static int timer_a = 0;
static int fd_seen = 0;

static long now_ms(void)
{
 struct timespec ts;
 clock_gettime(CLOCK_MONOTONIC, &ts);
 return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void count_a(void *data) { (void)data; fired_a++; }
static void count_b(void *data) { (void)data; fired_b++; }

/* A repeating timer that replaces itself with a one-shot due at once. */
static void a_replaces_itself(void *data)
{
 (void)data;
 fired_a++;
 wpe_timer_del(timer_a);
 wpe_timer_add(0, 0, count_b, NULL);
}

static void on_data(int fd, void *data)
{
 char buf[16];
 (void)data;
 if (read(fd, buf, sizeof buf) > 0)
  fd_seen++;
}

/* A one-shot runs exactly once; deleting it afterwards, or an id never given, is harmless. */
static int test_one_shot_fires_once(void)
{
 int id;
 fired_a = 0;
 id = wpe_timer_add(TICK_MS, 0, count_a, NULL);
 if (id <= 0) { printf("FAIL: one-shot not added (id=%d)\n", id); return 1; }
 wpe_fd_poll(-1);
 wpe_fd_poll(TICK_MS);
 wpe_fd_poll(TICK_MS);
 wpe_timer_del(id);
 wpe_timer_del(0);
 wpe_timer_del(id + 1000);
 if (fired_a != 1) { printf("FAIL: one-shot fired %d times\n", fired_a); return 1; }
 return 0;
}

/* A repeating timer keeps firing until it is deleted, then stays quiet. */
static int test_repeating_fires_until_deleted(void)
{
 int id, i, after;
 fired_a = 0;
 id = wpe_timer_add(TICK_MS, 1, count_a, NULL);
 for (i = 0; i < 4; i++)
  wpe_fd_poll(-1);
 after = fired_a;
 wpe_timer_del(id);
 if (wpe_fd_poll(TICK_MS) != 0) { printf("FAIL: a deleted timer still woke the loop\n"); return 1; }
 if (after != 4) { printf("FAIL: repeating timer fired %d times in 4 polls\n", after); return 1; }
 if (fired_a != after) { printf("FAIL: repeating timer fired after its delete\n"); return 1; }
 return 0;
}

/* With no fd but a timer pending, poll(-1) sleeps until the timer: not at once, not forever. */
static int test_poll_sleeps_until_the_timer(void)
{
 long t0, took;
 fired_a = 0;
 wpe_timer_add(SLOW_TICK_MS, 0, count_a, NULL);
 t0 = now_ms();
 wpe_fd_poll(-1);
 took = now_ms() - t0;
 if (fired_a != 1) { printf("FAIL: poll(-1) returned before the timer (fired=%d)\n", fired_a); return 1; }
 if (took < SLOW_TICK_MS - TIMING_SLACK_MS) { printf("FAIL: poll(-1) returned at once (%ld ms)\n", took); return 1; }
 if (took > NEVER_MS) { printf("FAIL: poll(-1) slept %ld ms for a %d ms timer\n", took, SLOW_TICK_MS); return 1; }
 return 0;
}

/* A repeating timer left unpolled for many intervals fires once, not in a burst. */
static int test_late_timer_does_not_burst(void)
{
 int id;
 fired_a = 0;
 id = wpe_timer_add(TICK_MS, 1, count_a, NULL);
 usleep(LATE_MS * 1000);
 wpe_fd_poll(0);
 wpe_fd_poll(0);
 wpe_timer_del(id);
 if (fired_a != 1) { printf("FAIL: a late timer fired %d times at once\n", fired_a); return 1; }
 return 0;
}

/* A callback deletes its own timer and adds another: the new one runs on the next poll. */
static int test_callback_deletes_itself_and_adds_another(void)
{
 fired_a = 0; fired_b = 0;
 timer_a = wpe_timer_add(TICK_MS, 1, a_replaces_itself, NULL);
 wpe_fd_poll(-1);
 if (fired_a != 1 || fired_b != 0) { printf("FAIL: after the first poll a=%d b=%d\n", fired_a, fired_b); return 1; }
 wpe_fd_poll(-1);
 if (fired_a != 1 || fired_b != 1) { printf("FAIL: after the second poll a=%d b=%d\n", fired_a, fired_b); return 1; }
 if (wpe_fd_poll(-1) != 0) { printf("FAIL: the loop is busy with no timer left\n"); return 1; }
 return 0;
}

/* A readable fd and a due timer in the same poll both run. */
static int test_timer_and_fd_in_one_poll(void)
{
 int p[2];
 if (pipe(p) != 0) return 1;
 fired_a = 0; fd_seen = 0;
 if (write(p[1], "x", 1) != 1) return 1;
 wpe_fd_add(p[0], POLLIN, on_data, NULL);
 wpe_timer_add(0, 0, count_a, NULL);
 wpe_fd_poll(-1);
 wpe_fd_del(p[0]);
 close(p[0]); close(p[1]);
 if (fd_seen != 1 || fired_a != 1) { printf("FAIL: one poll ran fd=%d timer=%d\n", fd_seen, fired_a); return 1; }
 return 0;
}

int main(void)
{
 int rc = 0;
 alarm(HANG_GUARD_S);      /* a poll that never wakes fails the test, not the build */
 rc |= test_callback_fd_reaped_on_hup();
 rc |= test_wakeonly_fd_dropped_on_hup();
 rc |= test_one_shot_fires_once();
 rc |= test_repeating_fires_until_deleted();
 rc |= test_poll_sleeps_until_the_timer();
 rc |= test_late_timer_does_not_burst();
 rc |= test_callback_deletes_itself_and_adds_another();
 rc |= test_timer_and_fd_in_one_poll();
 if (rc == 0) printf("test_fdloop: OK\n");
 return rc;
}
