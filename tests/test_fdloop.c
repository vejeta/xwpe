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
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <netinet/in.h>
#include <arpa/inet.h>
/* The loop's source is compiled into this test, not linked, so the id wrap
 * test can put the private id counter next to INT_MAX instead of making two
 * billion calls to reach it.  Its tables are static: so is its table size. */
#include "we_fdloop.c"
#include "test_watchdog.h"
#include "test_loop_until.h"

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

/* --- re-entry: a callback may poll again, as a dialog's key loop does. */

static int nested_ret = -1;
static int nested_a_fired = 0;

/* A due one-shot whose callback polls the loop again, with another timer due. */
static void a_polls_again(void *data)
{
 (void)data;
 nested_a_fired++;
 nested_ret = wpe_fd_poll(0);
}

/* The outer pass and the nested one each run a due timer once, never twice. */
static int test_nested_poll_runs_each_due_timer_once(void)
{
 fired_a = 0; fired_b = 0; nested_a_fired = 0; nested_ret = -1;
 wpe_timer_add(0, 0, a_polls_again, NULL);
 wpe_timer_add(0, 0, count_b, NULL);
 wpe_fd_poll(0);
 if (nested_a_fired != 1) { printf("FAIL: the outer callback ran %d times\n", nested_a_fired); return 1; }
 if (fired_b != 1) { printf("FAIL: the other due timer ran %d times, not once\n", fired_b); return 1; }
 if (nested_ret != 1) { printf("FAIL: the nested poll reported %d, not the one timer it ran\n", nested_ret); return 1; }
 if (wpe_fd_poll(0) != 0) { printf("FAIL: the loop is busy after a nested poll\n"); return 1; }
 return 0;
}

static int rep_depth = 0;
static int rep_calls = 0;

/* A repeating timer whose callback polls again while it is overdue. */
static void repeating_polls_again(void *data)
{
 (void)data;
 rep_calls++;
 if (rep_depth == 0)
 {
  rep_depth++;
  wpe_fd_poll(0);
  rep_depth--;
 }
}

/* A repeating timer is re-armed before its callback runs, so the callback's
 * own nested poll does not fire it again and recurse. */
static int test_nested_poll_does_not_refire_the_running_timer(void)
{
 int id;
 rep_calls = 0; rep_depth = 0;
 id = wpe_timer_add(TICK_MS, 1, repeating_polls_again, NULL);
 usleep(LATE_MS * 1000);
 wpe_fd_poll(0);
 wpe_timer_del(id);
 if (rep_calls != 1) { printf("FAIL: the running timer fired %d times through its nested poll\n", rep_calls); return 1; }
 return 0;
}

static int nested_fd_seen = 0;

/* An fd callback that drains its data, then polls again with a timer due. */
static void on_data_then_poll(int fd, void *data)
{
 char buf[16];
 (void)data;
 if (read(fd, buf, sizeof buf) > 0)
  nested_fd_seen++;
 nested_ret = wpe_fd_poll(0);
}

/* A nested poll from an fd callback runs the due timer.  The fd whose
 * callback is running is not watched by that poll at all, drained or not
 * (test_nested_poll_does_not_reenter_the_running_fd covers the undrained
 * case); here its data is already read, so only the timer counts. */
static int test_nested_poll_from_an_fd_callback(void)
{
 int p[2];
 if (pipe(p) != 0) return 1;
 fired_a = 0; nested_fd_seen = 0; nested_ret = -1;
 if (write(p[1], "x", 1) != 1) return 1;
 wpe_fd_add(p[0], POLLIN, on_data_then_poll, NULL);
 wpe_timer_add(0, 0, count_a, NULL);
 wpe_fd_poll(0);
 wpe_fd_del(p[0]);
 close(p[0]); close(p[1]);
 if (nested_fd_seen != 1) { printf("FAIL: the fd callback ran %d times\n", nested_fd_seen); return 1; }
 if (fired_a != 1) { printf("FAIL: the due timer ran %d times\n", fired_a); return 1; }
 if (nested_ret != 1) { printf("FAIL: the nested poll reported %d, wanted the one timer\n", nested_ret); return 1; }
 return 0;
}

/* --- full tables: the request that does not fit is refused, not lost or overwritten. */

static int timer_ids[WPE_TIMER_MAX];

/* Fill every timer slot with a timer that never comes due during the test. */
static int fill_timer_table(void)
{
 int i;
 for (i = 0; i < WPE_TIMER_MAX; i++)
 {
  timer_ids[i] = wpe_timer_add(NEVER_MS, 0, count_a, NULL);
  if (timer_ids[i] <= 0) { printf("FAIL: timer %d of %d was refused (%d)\n", i + 1, WPE_TIMER_MAX, timer_ids[i]); return 1; }
 }
 return 0;
}

static void drop_timer_table(void)
{
 int i;
 for (i = 0; i < WPE_TIMER_MAX; i++)
  wpe_timer_del(timer_ids[i]);
}

/* The timer after the last slot gets -1, the others keep running, and
 * deleting any one gives its slot back. */
static int test_timer_table_full_refuses_one_more(void)
{
 int extra, again, i, rc = 0;

 if (fill_timer_table()) { drop_timer_table(); return 1; }
 extra = wpe_timer_add(NEVER_MS, 0, count_a, NULL);
 if (extra != -1) { printf("FAIL: a timer past the table got %d, wanted -1\n", extra); rc = 1; }
 for (i = 0; i < WPE_TIMER_MAX; i++)
  if (!wpe_timer_find(timer_ids[i])) { printf("FAIL: timer %d was lost when the table filled\n", i); rc = 1; }
 wpe_timer_del(timer_ids[WPE_TIMER_MAX / 2]);
 again = wpe_timer_add(NEVER_MS, 0, count_a, NULL);
 if (again <= 0) { printf("FAIL: no slot after a delete (%d)\n", again); rc = 1; }
 else timer_ids[WPE_TIMER_MAX / 2] = again;
 drop_timer_table();
 if (wpe_fd_poll(0) != 0) { printf("FAIL: a timer is left over\n"); rc = 1; }
 return rc;
}

static int null_fds[WPE_FD_MAX + 1];

static void close_null_fds(int n)
{
 int i;
 for (i = 0; i < n; i++)
 {
  wpe_fd_del(null_fds[i]);
  close(null_fds[i]);
 }
}

/* The fd after the last slot gets -1, a registered fd can still be updated
 * while the table is full, and deleting the newest frees a slot. */
static int test_fd_table_full_refuses_one_more(void)
{
 int i, rc = 0, extra, update, again;

 for (i = 0; i <= WPE_FD_MAX; i++)
 {
  null_fds[i] = open("/dev/null", O_RDONLY);
  if (null_fds[i] < 0) return 1;
 }
 for (i = 0; i < WPE_FD_MAX; i++)
  if (wpe_fd_add(null_fds[i], POLLIN, NULL, NULL) != 0) { printf("FAIL: fd %d of %d was refused\n", i + 1, WPE_FD_MAX); rc = 1; }
 extra = wpe_fd_add(null_fds[WPE_FD_MAX], POLLIN, NULL, NULL);
 if (extra != -1) { printf("FAIL: an fd past the table got %d, wanted -1\n", extra); rc = 1; }
 update = wpe_fd_add(null_fds[0], POLLIN, on_data, NULL);
 if (update != 0) { printf("FAIL: updating a registered fd in a full table got %d\n", update); rc = 1; }
 wpe_fd_del(null_fds[WPE_FD_MAX - 1]);
 again = wpe_fd_add(null_fds[WPE_FD_MAX], POLLIN, NULL, NULL);
 if (again != 0) { printf("FAIL: no slot after deleting the newest fd (%d)\n", again); rc = 1; }
 close_null_fds(WPE_FD_MAX + 1);
 return rc;
}

#define REFUSAL_LINE_MAX 128

/* The fd a full table refuses gets -1, and the line the editor shows for it
 * names what will not arrive and why, so the user is not left waiting for a
 * reply or a build output that is never read. */
static int test_fd_refusal_says_why(void)
{
 char line[REFUSAL_LINE_MAX];
 int i, rc = 0, extra;

 for (i = 0; i <= WPE_FD_MAX; i++)
 {
  null_fds[i] = open("/dev/null", O_RDONLY);
  if (null_fds[i] < 0) return 1;
 }
 for (i = 0; i < WPE_FD_MAX; i++)
  wpe_fd_add(null_fds[i], POLLIN, NULL, NULL);
 extra = wpe_fd_add(null_fds[WPE_FD_MAX], POLLIN, on_data, NULL);
 if (extra != -1) { printf("FAIL: a full table took another fd (%d)\n", extra); rc = 1; }
 wpe_fd_refusal("the AI reply", null_fds[WPE_FD_MAX], line, sizeof line);
 if (strcmp(line, "could not watch the AI reply: too many open streams") != 0)
 { printf("FAIL: the refusal reads \"%s\"\n", line); rc = 1; }
 wpe_fd_refusal("the AI reply", -1, line, sizeof line);
 if (strcmp(line, "could not watch the AI reply: it is not open") != 0)
 { printf("FAIL: the refusal of a closed fd reads \"%s\"\n", line); rc = 1; }
 close_null_fds(WPE_FD_MAX + 1);
 return rc;
}

/* --- slots: a delete anywhere in a full table frees a slot, and each fd keeps its own callback. */

#define SLOT_PIPES (WPE_FD_MAX + 2)

static int slot_pipe[SLOT_PIPES][2];
static int slot_hits[SLOT_PIPES];

/* Drains its pipe and counts the wake-up under the index handed in as data. */
static void on_slot_ready(int fd, void *data)
{
 char buf[16];
 int idx = (int)(long)data;
 if (read(fd, buf, sizeof buf) > 0)
  slot_hits[idx]++;
}

static int open_slot_pipes(void)
{
 int i;
 for (i = 0; i < SLOT_PIPES; i++)
  if (pipe(slot_pipe[i]) != 0) return 1;
 return 0;
}

static void close_slot_pipes(void)
{
 int i;
 for (i = 0; i < SLOT_PIPES; i++)
 {
  wpe_fd_del(slot_pipe[i][0]);
  close(slot_pipe[i][0]);
  close(slot_pipe[i][1]);
 }
}

static int add_slot_pipe(int idx)
{
 return wpe_fd_add(slot_pipe[idx][0], POLLIN, on_slot_ready, (void *)(long)idx);
}

/* Make the pipes in the live set readable, poll once, and check that each ran
 * its own callback once and a pipe outside the set did not run. */
static int poll_slot_set(const int *live, int n, int skipped)
{
 int i, rc = 0;

 for (i = 0; i < SLOT_PIPES; i++)
  slot_hits[i] = 0;
 for (i = 0; i < n; i++)
  if (write(slot_pipe[live[i]][1], "x", 1) != 1) return 1;
 wpe_fd_poll(0);
 for (i = 0; i < n; i++)
  if (slot_hits[live[i]] != 1) { printf("FAIL: fd %d dispatched %d times, wanted 1\n", live[i], slot_hits[live[i]]); rc = 1; }
 if (skipped >= 0 && slot_hits[skipped] != 0) { printf("FAIL: the deleted fd %d was dispatched\n", skipped); rc = 1; }
 return rc;
}

/* A fd closed in the front or the middle of a full table frees its slot: the
 * next one registers, is dispatched, and every other fd still reaches its own
 * callback. */
static int test_fd_slot_reused_after_a_delete_anywhere(void)
{
 int i, n, rc = 0, live[WPE_FD_MAX];

 if (open_slot_pipes()) return 1;
 for (i = 0; i < WPE_FD_MAX; i++)
  if (add_slot_pipe(i) != 0) { printf("FAIL: fd %d of %d was refused\n", i + 1, WPE_FD_MAX); rc = 1; }
 wpe_fd_del(slot_pipe[0][0]);                         /* the FIRST of a full table */
 if (add_slot_pipe(WPE_FD_MAX) != 0) { printf("FAIL: no slot after deleting the first fd\n"); rc = 1; }
 n = 0;
 for (i = 1; i <= WPE_FD_MAX; i++)
  live[n++] = i;
 rc |= poll_slot_set(live, n, 0);
 wpe_fd_del(slot_pipe[WPE_FD_MAX / 2][0]);            /* one from the MIDDLE */
 if (add_slot_pipe(WPE_FD_MAX + 1) != 0) { printf("FAIL: no slot after deleting a middle fd\n"); rc = 1; }
 n = 0;
 for (i = 1; i <= WPE_FD_MAX + 1; i++)
  if (i != WPE_FD_MAX / 2)
   live[n++] = i;
 rc |= poll_slot_set(live, n, WPE_FD_MAX / 2);
 close_slot_pipes();
 return rc;
}

/* Serves pipe 0, then replaces pipe 1 (also reported ready) by pipe 2. */
static void on_first_replaces_second(int fd, void *data)
{
 on_slot_ready(fd, data);
 wpe_fd_del(slot_pipe[1][0]);
 add_slot_pipe(2);
}

/* An fd added into the slot of one a callback just removed is not served by
 * the readiness poll() reported for the old one; it waits for the next poll. */
static int test_fd_added_into_a_removed_slot_waits_for_next_poll(void)
{
 int rc = 0, i;

 if (open_slot_pipes()) return 1;
 for (i = 0; i < SLOT_PIPES; i++)
  slot_hits[i] = 0;
 wpe_fd_add(slot_pipe[0][0], POLLIN, on_first_replaces_second, (void *)0);
 add_slot_pipe(1);
 for (i = 0; i < 3; i++)
  if (write(slot_pipe[i][1], "x", 1) != 1) return 1;
 wpe_fd_poll(0);
 if (slot_hits[0] != 1 || slot_hits[1] != 0 || slot_hits[2] != 0)
 { printf("FAIL: first poll served %d/%d/%d, wanted 1/0/0\n", slot_hits[0], slot_hits[1], slot_hits[2]); rc = 1; }
 wpe_fd_poll(0);
 if (slot_hits[2] != 1) { printf("FAIL: the replacing fd was served %d times by the next poll\n", slot_hits[2]); rc = 1; }
 close_slot_pipes();
 return rc;
}

/* Serves pipe 0, then closes pipe 1 (also reported ready) and opens pipe 2
 * on its NUMBER, as the lowest free descriptor is handed out again. */
static void on_first_reopens_second(int fd, void *data)
{
 on_slot_ready(fd, data);
 wpe_fd_del(slot_pipe[1][0]);
 dup2(slot_pipe[2][0], slot_pipe[1][0]);
 wpe_fd_add(slot_pipe[1][0], POLLIN, on_slot_ready, (void *)2L);
}

/* A stream opened by a callback on the number of one it just closed is not
 * served by the readiness poll() reported for the old one: its reader would
 * block on data that is not there.  It waits for the next poll. */
static int test_fd_reopened_on_a_removed_number_waits_for_next_poll(void)
{
 int rc = 0, i;

 if (open_slot_pipes()) return 1;
 for (i = 0; i < SLOT_PIPES; i++)
  slot_hits[i] = 0;
 wpe_fd_add(slot_pipe[0][0], POLLIN, on_first_reopens_second, (void *)0);
 add_slot_pipe(1);
 for (i = 0; i < 3; i++)
  if (write(slot_pipe[i][1], "x", 1) != 1) return 1;
 wpe_fd_poll(0);
 if (slot_hits[0] != 1 || slot_hits[1] != 0 || slot_hits[2] != 0)
 { printf("FAIL: first poll served %d/%d/%d, wanted 1/0/0 (the new stream ran on the old one's readiness)\n", slot_hits[0], slot_hits[1], slot_hits[2]); rc = 1; }
 wpe_fd_poll(0);
 if (slot_hits[2] != 1) { printf("FAIL: the reopened fd was served %d times by the next poll\n", slot_hits[2]); rc = 1; }
 close_slot_pipes();
 return rc;
}

/* --- one readiness report is served once, however deep the dialogs. */

static int nest_pipe[2][2];
static int nest_calls[2];
static int nest_empty[2];
static int nest_depth = 0;

static int open_nest_pipes(void)
{
 int i;
 for (i = 0; i < 2; i++)
 {
  if (pipe(nest_pipe[i]) != 0) return 1;
  fcntl(nest_pipe[i][0], F_SETFL, O_NONBLOCK);  /* a stale run counts, never hangs */
  nest_calls[i] = 0;
  nest_empty[i] = 0;
 }
 nest_depth = 0;
 return 0;
}

static void close_nest_pipes(void)
{
 int i;
 for (i = 0; i < 2; i++)
 {
  wpe_fd_del(nest_pipe[i][0]);
  close(nest_pipe[i][0]);
  close(nest_pipe[i][1]);
 }
}

/* Reads one wake-up's data; a call with nothing to read is a stale one. */
static void nest_read(int idx)
{
 char buf[16];
 nest_calls[idx]++;
 if (read(nest_pipe[idx][0], buf, sizeof buf) <= 0)
  nest_empty[idx]++;
}

/* X opens a dialog (polls again) before it reads, as a permission box does. */
static void x_polls_before_reading(int fd, void *data)
{
 (void)fd; (void)data;
 if (nest_depth == 0)
 {
  nest_depth++;
  wpe_fd_poll(0);
  nest_depth--;
 }
 nest_read(0);
}

static void y_reads(int fd, void *data)
{
 (void)fd; (void)data;
 nest_read(1);
}

static int nest_write(int idx)
{
 if (write(nest_pipe[idx][1], "x", 1) == 1)
  return 0;
 return 1;
}

/* The fd whose callback opened the dialog is not served again by the
 * dialog's poll on the data it has not read yet. */
static int test_nested_poll_does_not_reenter_the_running_fd(void)
{
 int rc = 0;

 if (open_nest_pipes()) return 1;
 wpe_fd_add(nest_pipe[0][0], POLLIN, x_polls_before_reading, NULL);
 if (nest_write(0)) return 1;
 wpe_fd_poll(0);
 if (nest_calls[0] != 1 || nest_empty[0] != 0)
 { printf("FAIL: the running fd was served %d times, %d with nothing to read\n", nest_calls[0], nest_empty[0]); rc = 1; }
 close_nest_pipes();
 return rc;
}

/* Another fd the dialog's poll served is not served again by the outer pass
 * on the report it already used. */
static int test_outer_pass_skips_what_a_nested_poll_served(void)
{
 int rc = 0;

 if (open_nest_pipes()) return 1;
 wpe_fd_add(nest_pipe[0][0], POLLIN, x_polls_before_reading, NULL);
 wpe_fd_add(nest_pipe[1][0], POLLIN, y_reads, NULL);
 if (nest_write(0) || nest_write(1)) return 1;
 wpe_fd_poll(0);
 if (nest_calls[1] != 1 || nest_empty[1] != 0)
 { printf("FAIL: the other fd was served %d times, %d with nothing to read\n", nest_calls[1], nest_empty[1]); rc = 1; }
 if (nest_calls[0] != 1)
 { printf("FAIL: the fd that opened the dialog was served %d times\n", nest_calls[0]); rc = 1; }
 close_nest_pipes();
 return rc;
}

/* A deadline timer that fires inside the dialog and dispatches X. */
static void dispatch_x(void *data)
{
 (void)data;
 wpe_fd_dispatch(nest_pipe[0][0]);
}

/* A timer's dispatch does not enter a callback that is still running either:
 * the overdue check finds the source again on a later tick. */
static int test_dispatch_does_not_reenter_the_running_fd(void)
{
 int rc = 0;

 if (open_nest_pipes()) return 1;
 wpe_fd_add(nest_pipe[0][0], POLLIN, x_polls_before_reading, NULL);
 wpe_timer_add(0, 0, dispatch_x, NULL);
 if (nest_write(0)) return 1;
 wpe_fd_poll(0);
 if (nest_calls[0] != 1 || nest_empty[0] != 0)
 { printf("FAIL: a dispatch entered the running fd: %d runs, %d with nothing to read\n", nest_calls[0], nest_empty[0]); rc = 1; }
 close_nest_pipes();
 if (wpe_fd_poll(0) != 0) { printf("FAIL: the loop is busy after the dispatch\n"); rc = 1; }
 return rc;
}

/* --- ids: unique among live timers, also across the wrap of the counter. */

static int timer_id_is_live_once(int id)
{
 int i, n = 0;
 for (i = 0; i < WPE_TIMER_MAX; i++)
  if (timer_table[i].id == id)
   n++;
 return n == 1;
}

/* The counter wraps past INT_MAX to 1 (never 0, the "no timer" value), and an
 * id still held by a live timer is skipped, so a delete by an old owner cannot
 * stop the timer that took the id later. */
static int test_timer_ids_stay_unique_across_the_wrap(void)
{
 int last, wrapped, skipped, rc = 0;

 timer_last_id = INT_MAX - 1;
 last = wpe_timer_add(NEVER_MS, 0, count_a, NULL);
 wrapped = wpe_timer_add(NEVER_MS, 0, count_a, NULL);
 if (last != INT_MAX) { printf("FAIL: the id before the wrap is %d\n", last); rc = 1; }
 if (wrapped != 1) { printf("FAIL: the id after the wrap is %d, wanted 1\n", wrapped); rc = 1; }
 timer_last_id = INT_MAX;                   /* the next id would be 1 again: it is live */
 skipped = wpe_timer_add(NEVER_MS, 0, count_a, NULL);
 if (skipped == wrapped || skipped <= 0) { printf("FAIL: a live id was given out again (%d)\n", skipped); rc = 1; }
 if (wrapped > 0 && !timer_id_is_live_once(wrapped)) { printf("FAIL: id %d is held by more than one timer\n", wrapped); rc = 1; }
 wpe_timer_del(last); wpe_timer_del(wrapped); wpe_timer_del(skipped);
 if (wpe_fd_poll(0) != 0) { printf("FAIL: a timer is left over\n"); rc = 1; }
 return rc;
}

/* --- wpe_fd_readable_now: the probe asks without waiting and without reading. */

/* An empty pipe is not readable, a pipe with a byte is, and the byte is
 * still there afterwards (the probe consumed nothing). */
static int test_readable_now_tracks_data(void)
{
 int p[2];
 char c = 'x', got = 0;

 if (pipe(p) != 0) return 1;
 if (wpe_fd_readable_now(p[0]) != WPE_FD_NOT_READABLE)
 { printf("FAIL: empty pipe reported readable\n"); return 1; }
 if (write(p[1], &c, 1) != 1) return 1;
 if (wpe_fd_readable_now(p[0]) != WPE_FD_READABLE)
 { printf("FAIL: pipe with a byte not readable\n"); return 1; }
 if (read(p[0], &got, 1) != 1 || got != 'x')
 { printf("FAIL: the probe consumed the byte\n"); return 1; }
 if (wpe_fd_readable_now(p[0]) != WPE_FD_NOT_READABLE)
 { printf("FAIL: drained pipe still readable\n"); return 1; }
 close(p[0]); close(p[1]);
 return 0;
}

/* A hung-up pipe is readable (the read reports the end); a closed descriptor
 * is invalid, not "just idle". */
static int test_readable_now_hangup_and_closed(void)
{
 int fd = eof_pipe();

 if (fd < 0) return 1;
 if (wpe_fd_readable_now(fd) != WPE_FD_READABLE)
 { printf("FAIL: hung-up pipe not readable\n"); return 1; }
 close(fd);
 if (wpe_fd_readable_now(fd) != WPE_FD_INVALID)
 { printf("FAIL: closed descriptor not invalid\n"); return 1; }
 return 0;
}

/* --- wpe_fd_wait_for: waits on the loop, with a deadline, without spinning. */

#define WAIT_DEADLINE_MS   120
#define WAIT_EARLY_MS      (WAIT_DEADLINE_MS - 20)
#define WAIT_LATE_MS       (WAIT_DEADLINE_MS + 150)
#define WAIT_FIRE_MS       40
#define WAIT_LABEL         "the test's answer"

static int wait_flag = 0;
static const char *wait_label_seen = NULL;

static int pred_flag(void) { return wait_flag; }
static int pred_never(void) { return 0; }
static void set_flag(void *data) { (void)data; wait_flag = 1; }

static int pred_records_label(void)
{
 wait_label_seen = wpe_fd_waiting_for();
 return wait_flag;
}

/* A predicate already true ends the wait before any waiting. */
static int test_wait_for_true_predicate_returns_at_once(void)
{
 long t0 = now_ms();

 wait_flag = 1;
 if (wpe_fd_wait_for(pred_flag, WPE_WAIT_FOREVER, WAIT_LABEL) != WPE_WAIT_DONE)
 { printf("FAIL: true predicate did not end the wait\n"); return 1; }
 if (now_ms() - t0 > WAIT_FIRE_MS)
 { printf("FAIL: true predicate waited\n"); return 1; }
 return 0;
}

/* A deadline ends a wait on a descriptor that stays silent, on time: the
 * loop slept on it rather than returning early. */
static int test_wait_for_deadline_on_a_silent_descriptor(void)
{
 int p[2], r;
 long t0, took;

 if (pipe(p) != 0) return 1;
 wpe_fd_add(p[0], POLLIN, NULL, NULL);
 t0 = now_ms();
 r = wpe_fd_wait_for(pred_never, WAIT_DEADLINE_MS, WAIT_LABEL);
 took = now_ms() - t0;
 wpe_fd_del(p[0]);
 close(p[0]); close(p[1]);
 if (r != WPE_WAIT_TIMEOUT)
 { printf("FAIL: deadline gave %d\n", r); return 1; }
 if (took < WAIT_EARLY_MS || took > WAIT_LATE_MS)
 { printf("FAIL: deadline took %ld ms\n", took); return 1; }
 return 0;
}

/* With nothing registered nothing can answer the wait: it ends at once,
 * long before a deadline it could only have slept through. */
#define WAIT_IDLE_DEADLINE_MS (NEVER_MS * 30)

static int test_wait_for_nothing_registered_ends_at_once(void)
{
 long t0 = now_ms(), took;
 int r;

 r = wpe_fd_wait_for(pred_never, WAIT_IDLE_DEADLINE_MS, WAIT_LABEL);
 took = now_ms() - t0;
 if (r != WPE_WAIT_TIMEOUT)
 { printf("FAIL: idle wait gave %d\n", r); return 1; }
 if (took > NEVER_MS)
 { printf("FAIL: idle wait slept %ld ms with nothing to wake it\n", took); return 1; }
 r = wpe_fd_wait_for(pred_never, WPE_WAIT_FOREVER, WAIT_LABEL);
 if (r != WPE_WAIT_TIMEOUT)
 { printf("FAIL: idle wait with no deadline gave %d\n", r); return 1; }
 return 0;
}

/* A deadline of zero asks once and answers. */
static int test_wait_for_zero_deadline_asks_once(void)
{
 wait_flag = 0;
 if (wpe_fd_wait_for(pred_flag, 0, WAIT_LABEL) != WPE_WAIT_TIMEOUT)
 { printf("FAIL: zero deadline did not time out\n"); return 1; }
 return 0;
}

/* The loop runs while waiting: a timer that sets the flag ends the wait,
 * long before its deadline, and the wait named itself meanwhile. */
static int test_wait_for_serves_timers_and_names_the_wait(void)
{
 long t0 = now_ms();
 int r;

 wait_flag = 0;
 wait_label_seen = NULL;
 wpe_timer_add(WAIT_FIRE_MS, 0, set_flag, NULL);
 r = wpe_fd_wait_for(pred_records_label, NEVER_MS, WAIT_LABEL);
 if (r != WPE_WAIT_DONE)
 { printf("FAIL: timer did not end the wait (%d)\n", r); return 1; }
 if (now_ms() - t0 > WAIT_LATE_MS)
 { printf("FAIL: the wait ran to its deadline\n"); return 1; }
 if (wait_label_seen == NULL || strcmp(wait_label_seen, WAIT_LABEL) != 0)
 { printf("FAIL: the wait did not name itself\n"); return 1; }
 if (wpe_fd_waiting_for() != NULL)
 { printf("FAIL: the name outlived the wait\n"); return 1; }
 return 0;
}

/* A registered fd's callback runs during the wait and answers it. */
static void on_data_sets_flag(int fd, void *data)
{
 char buf[16];

 (void)data;
 if (read(fd, buf, sizeof buf) > 0)
  wait_flag = 1;
}

static int test_wait_for_serves_fd_callbacks(void)
{
 int p[2], r;

 if (pipe(p) != 0) return 1;
 wait_flag = 0;
 wpe_fd_add(p[0], POLLIN, on_data_sets_flag, NULL);
 if (write(p[1], "x", 1) != 1) return 1;
 r = wpe_fd_wait_for(pred_flag, NEVER_MS, WAIT_LABEL);
 wpe_fd_del(p[0]);
 close(p[0]); close(p[1]);
 if (r != WPE_WAIT_DONE)
 { printf("FAIL: fd callback did not answer the wait (%d)\n", r); return 1; }
 return 0;
}

/* How often a wait may ask its predicate when nothing it could change ran:
 * once before waiting, once after the loop came back. */
#define WAIT_ASKS_WITHOUT_WORK 2

static int wait_asks = 0;

static int pred_counts_asks(void)
{
 wait_asks++;
 return 0;
}

/* A key waiting on a wake-only descriptor (the keyboard, a display
 * connection) is read by the display's getter, not by a callback, so the
 * loop can do nothing with it: the wait hands it back instead of asking
 * its predicate again and again until the deadline. */
static int test_wait_for_hands_back_unread_wake_only_input(void)
{
 int p[2], r;

 if (pipe(p) != 0) return 1;
 wait_asks = 0;
 wpe_fd_add(p[0], POLLIN, NULL, NULL);
 if (write(p[1], "k", 1) != 1) return 1;
 r = wpe_fd_wait_for(pred_counts_asks, WAIT_DEADLINE_MS, WAIT_LABEL);
 wpe_fd_del(p[0]);
 close(p[0]); close(p[1]);
 if (wait_asks > WAIT_ASKS_WITHOUT_WORK)
 { printf("FAIL: the wait asked %d times over a key it cannot read\n", wait_asks); return 1; }
 if (r != WPE_WAIT_INPUT)
 { printf("FAIL: the unread key was not handed back (%d)\n", r); return 1; }
 return 0;
}

/* A command that has the keyboard: the wait does not wake for the keys the
 * user types for it, serves the display once at the start, and leaves the
 * keys unread for the command. */
#define SERVING_TIMER_MS 30
static int serving_done, serving_served;

static void serving_timer(void *data) { (void)data; serving_done = 1; }
static int serving_pred(void) { return serving_done; }
static void serving_serve(void) { serving_served++; }

/* @scenario SC-debug-87 */
static int test_wait_serving_does_not_wake_for_a_muted_keyboard(void)
{
 int p[2], r, id;
 char c;

 if (pipe(p) != 0) return 1;
 serving_done = serving_served = 0;
 wpe_fd_add(p[0], POLLIN, NULL, NULL);
 if (write(p[1], "k", 1) != 1) return 1;
 id = wpe_timer_add(SERVING_TIMER_MS, 0, serving_timer, NULL);
 r = wpe_fd_wait_serving(serving_pred, WAIT_LABEL, p[0], serving_serve,
                         WPE_WAIT_FOREVER);
 (void)id;
 if (r != WPE_WAIT_DONE)
 { printf("FAIL: the muted wait ended with %d\n", r); return 1; }
 if (serving_served != 1)
 { printf("FAIL: the display was served %d times, not once\n", serving_served); return 1; }
 if (read(p[0], &c, 1) != 1 || c != 'k')
 { printf("FAIL: the key was taken from the command\n"); return 1; }
 /* Given back: the next key wakes a plain wait again. */
 if (write(p[1], "k", 1) != 1) return 1;
 serving_done = 0;
 r = wpe_fd_wait_for(serving_pred, WAIT_DEADLINE_MS, WAIT_LABEL);
 wpe_fd_del(p[0]);
 close(p[0]); close(p[1]);
 if (r != WPE_WAIT_INPUT)
 { printf("FAIL: the keyboard was not given back (%d)\n", r); return 1; }
 return 0;
}

/* ---- bytes for a child: wpe_fd_write ------------------------------------
 *
 * A pipe whose reader does not read stands for a child that stopped reading
 * its input (a busy language server, a stopped gdb); a socketpair whose peer
 * closes stands for a child that ended.  The pipe is filled first, so every
 * byte the test writes after it has to wait in the loop's queue. */

#define WRITE_WHAT      "the test's child"
#define WRITE_ENDS      "the test ends it"
#define WRITE_FILL_CHUNK 4096
#define WRITE_SAID_MAX   8
#define WRITE_SAID_LINE  256
#define WRITE_BOUND_SLACK 10
#define WRITE_OVER_BOUND  20

static char write_said[WRITE_SAID_MAX][WRITE_SAID_LINE];
static int write_said_n = 0;

static void on_write_say(const char *line)
{
 if (write_said_n < WRITE_SAID_MAX)
  snprintf(write_said[write_said_n], WRITE_SAID_LINE, "%s", line);
 write_said_n++;
}

static void set_nonblocking(int fd)
{
 fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

/* Fill @fd (non-blocking) until it takes no more.  Return: bytes written. */
static long fill_until_full(int fd)
{
 char chunk[WRITE_FILL_CHUNK];
 long total = 0;
 ssize_t w;

 memset(chunk, 'f', sizeof chunk);
 while ((w = write(fd, chunk, sizeof chunk)) > 0)
  total += w;
 return total;
}

/* A pipe with both ends non-blocking and its pipe full.  Return: the fill. */
static long full_pipe(int p[2])
{
 if (pipe(p) != 0)
  return -1;
 set_nonblocking(p[0]);
 set_nonblocking(p[1]);
 return fill_until_full(p[1]);
}

/* Read and throw away @n bytes of @fd: the child reads again. */
static int drain_bytes(int fd, long n)
{
 char buf[WRITE_FILL_CHUNK];

 while (n > 0)
 {
  size_t want = n < (long)sizeof buf ? (size_t)n : sizeof buf;
  ssize_t r = read(fd, buf, want);
  if (r <= 0)
   return -1;
  n -= r;
 }
 return 0;
}

/* Read what is in @fd now, up to @sz - 1 bytes, as a string. */
static ssize_t read_now(int fd, char *out, size_t sz)
{
 ssize_t r = read(fd, out, sz - 1);

 out[r > 0 ? r : 0] = '\0';
 return r;
}

static int test_write_queues_when_the_pipe_is_full_and_flushes_on_pollout(void)
{
 int p[2], blocking[2], r;
 long fill = full_pipe(p);
 char got[64];

 if (fill <= 0) return 1;
 r = wpe_fd_write(p[1], "cmd\n", 4, WRITE_WHAT, WRITE_ENDS);
 if (r != WPE_FD_WRITE_QUEUED)
 { printf("FAIL: a write to a full pipe was not queued (%d)\n", r); return 1; }
 if (wpe_fd_write_pending(p[1]) != 4)
 { printf("FAIL: queued %ld bytes, not 4\n", wpe_fd_write_pending(p[1])); return 1; }
 wpe_fd_poll(0);
 if (wpe_fd_write_pending(p[1]) != 4)
 { printf("FAIL: the queue moved while the child read nothing\n"); return 1; }
 if (drain_bytes(p[0], fill) != 0) return 1;
 wpe_fd_poll(0);
 if (wpe_fd_write_pending(p[1]) != 0)
 { printf("FAIL: POLLOUT did not flush the queue (%ld left)\n", wpe_fd_write_pending(p[1])); return 1; }
 read_now(p[0], got, sizeof got);
 if (strcmp(got, "cmd\n"))
 { printf("FAIL: the child read \"%s\", not the command\n", got); return 1; }
 if (wpe_fd_poll(0) != 0)
 { printf("FAIL: an idle writable pipe still wakes the loop\n"); return 1; }
 close(p[0]); close(p[1]);
 /* A descriptor that would block cannot be queued on: refused, not written. */
 if (pipe(blocking) != 0) return 1;
 r = wpe_fd_write(blocking[1], "x", 1, WRITE_WHAT, WRITE_ENDS);
 close(blocking[0]); close(blocking[1]);
 if (r != WPE_FD_WRITE_REFUSED)
 { printf("FAIL: a blocking descriptor was written (%d)\n", r); return 1; }
 return 0;
}

static int test_two_writes_arrive_whole_and_in_order(void)
{
 int p[2];
 long fill = full_pipe(p);
 char got[64];

 if (fill <= 0) return 1;
 if (wpe_fd_write(p[1], "first\n", 6, WRITE_WHAT, WRITE_ENDS) != WPE_FD_WRITE_QUEUED)
 { printf("FAIL: the first command was not queued\n"); return 1; }
 /* Room for a whole command now (a pipe frees its room a page at a time):
    the second command must still wait behind the first, never slip in
    ahead of it or in its middle. */
 if (drain_bytes(p[0], WRITE_FILL_CHUNK) != 0) return 1;
 if (wpe_fd_write(p[1], "second\n", 7, WRITE_WHAT, WRITE_ENDS) != WPE_FD_WRITE_QUEUED)
 { printf("FAIL: the second command was not queued behind the first\n"); return 1; }
 if (drain_bytes(p[0], fill - WRITE_FILL_CHUNK) != 0)
 { printf("FAIL: the second command was written ahead of the queue\n"); return 1; }
 wpe_fd_poll(0);
 read_now(p[0], got, sizeof got);
 close(p[0]); close(p[1]);
 if (strcmp(got, "first\nsecond\n"))
 { printf("FAIL: the child read \"%s\"\n", got); return 1; }
 return 0;
}

static int test_a_write_past_the_queue_bound_is_refused_whole(void)
{
 int p[2], r;
 long fill = full_pipe(p);
 size_t big = (size_t)WPE_FD_WRITE_QUEUE_MAX - WRITE_BOUND_SLACK;
 char *buf = malloc((size_t)WPE_FD_WRITE_QUEUE_MAX + 1);
 char over[WRITE_OVER_BOUND];

 if (fill <= 0 || !buf) return 1;
 memset(buf, 'b', (size_t)WPE_FD_WRITE_QUEUE_MAX + 1);
 memset(over, 'o', sizeof over);
 if (wpe_fd_write(p[1], "head\n", 5, WRITE_WHAT, WRITE_ENDS) != WPE_FD_WRITE_QUEUED)
 { printf("FAIL: the first command was not queued\n"); free(buf); return 1; }
 r = wpe_fd_write(p[1], buf, big, WRITE_WHAT, WRITE_ENDS);
 if (r != WPE_FD_WRITE_QUEUED)
 { printf("FAIL: a write inside the bound was refused (%d)\n", r); free(buf); return 1; }
 r = wpe_fd_write(p[1], over, sizeof over, WRITE_WHAT, WRITE_ENDS);
 if (r != WPE_FD_WRITE_REFUSED)
 { printf("FAIL: a write past the bound was taken (%d)\n", r); free(buf); return 1; }
 if (wpe_fd_write_pending(p[1]) != (long)(5 + big))
 { printf("FAIL: a refused write left %ld bytes queued\n", wpe_fd_write_pending(p[1])); free(buf); return 1; }
 wpe_fd_writer_forget(p[1]);
 /* Nothing waiting: one message larger than the bound is taken whole (a
    language server is sent a whole file), and the next one waits for it. */
 r = wpe_fd_write(p[1], buf, (size_t)WPE_FD_WRITE_QUEUE_MAX + 1, WRITE_WHAT, WRITE_ENDS);
 if (r != WPE_FD_WRITE_QUEUED)
 { printf("FAIL: one message past the bound with nothing waiting was refused (%d)\n", r); free(buf); return 1; }
 r = wpe_fd_write(p[1], over, sizeof over, WRITE_WHAT, WRITE_ENDS);
 free(buf);
 close(p[0]); wpe_fd_close(p[1]);
 if (r != WPE_FD_WRITE_REFUSED)
 { printf("FAIL: a write behind an oversized message was taken (%d)\n", r); return 1; }
 return 0;
}

/* Bring the queue's stall due now, as if WPE_FD_WRITE_STALL_MS had gone by:
 * the loop's next poll then says it, without the test sleeping. */
static void stall_due_now(int fd)
{
 wpe_fd_writer *w = wpe_fd_writer_find(fd);

 if (w)
  w->stall_due_ms = wpe_clock_ms();
}

static int test_the_stall_line_comes_once_after_the_bound_and_clears_on_progress(void)
{
 int p[2];
 long fill = full_pipe(p);

 if (fill <= 0) return 1;
 write_said_n = 0;
 wpe_fd_write_say_set(on_write_say);
 if (wpe_fd_write(p[1], "cmd\n", 4, WRITE_WHAT, WRITE_ENDS) != WPE_FD_WRITE_QUEUED)
 { printf("FAIL: not queued\n"); return 1; }
 wpe_fd_poll(0);
 if (write_said_n != 0 || wpe_fd_write_stalled(p[1]))
 { printf("FAIL: the stall line came before the bound\n"); return 1; }
 stall_due_now(p[1]);
 wpe_fd_poll(NEVER_MS);
 if (write_said_n != 1 || !wpe_fd_write_stalled(p[1]))
 { printf("FAIL: the stall line came %d times at the bound\n", write_said_n); return 1; }
 if (!strstr(write_said[0], WRITE_WHAT " is not reading its input (4 bytes waiting): " WRITE_ENDS))
 { printf("FAIL: the stall line says \"%s\"\n", write_said[0]); return 1; }
 stall_due_now(p[1]);
 wpe_fd_poll(0);
 if (write_said_n != 1)
 { printf("FAIL: the stall line was said again (%d)\n", write_said_n); return 1; }
 if (drain_bytes(p[0], fill) != 0) return 1;
 wpe_fd_poll(0);
 wpe_fd_write_say_set(NULL);
 close(p[0]); close(p[1]);
 if (wpe_fd_write_stalled(p[1]))
 { printf("FAIL: still stalled after the child read\n"); return 1; }
 if (write_said_n != 2 || !strstr(write_said[1], "reading its input again"))
 { printf("FAIL: progress was not said (%d lines)\n", write_said_n); return 1; }
 if (wpe_fd_writers_ms_to_stall() != WPE_POLL_FOREVER)
 { printf("FAIL: the loop still wakes for a stall the queue cannot have\n"); return 1; }
 return 0;
}

/* Never called: it only holds a timer slot. */
static void on_timer_held(void *data)
{
 (void)data;
}

/* The stall line is the one thing that tells the user why a command does
 * nothing; it must come even when waiting lines and children's ends hold
 * every timer the loop has. */
static int test_the_stall_line_comes_when_every_timer_is_taken(void)
{
 int p[2], ids[WPE_TIMER_MAX], held = 0, i;
 long fill = full_pipe(p);

 if (fill <= 0) return 1;
 while (held < WPE_TIMER_MAX &&
        (ids[held] = wpe_timer_add(NEVER_MS * 30, 0, on_timer_held, NULL)) > 0)
  held++;
 if (wpe_timer_add(NEVER_MS, 0, on_timer_held, NULL) > 0)
 { printf("FAIL: the timer table was not full\n"); return 1; }
 write_said_n = 0;
 wpe_fd_write_say_set(on_write_say);
 if (wpe_fd_write(p[1], "cmd\n", 4, WRITE_WHAT, WRITE_ENDS) != WPE_FD_WRITE_QUEUED)
 { printf("FAIL: not queued\n"); return 1; }
 stall_due_now(p[1]);
 wpe_fd_poll(NEVER_MS);
 wpe_fd_write_say_set(NULL);
 for (i = 0; i < held; i++)
  wpe_timer_del(ids[i]);
 close(p[0]); wpe_fd_close(p[1]);
 if (write_said_n != 1)
 { printf("FAIL: with every timer taken the stall line came %d times\n", write_said_n); return 1; }
 return 0;
}

static int test_epipe_drops_the_queue_and_reports_closed(void)
{
 int s[2], r;
 long fill;

 signal(SIGPIPE, SIG_IGN);
 if (socketpair(AF_UNIX, SOCK_STREAM, 0, s) != 0) return 1;
 set_nonblocking(s[0]);
 fill = fill_until_full(s[0]);
 if (fill <= 0) return 1;
 if (wpe_fd_write(s[0], "cmd\n", 4, WRITE_WHAT, WRITE_ENDS) != WPE_FD_WRITE_QUEUED)
 { printf("FAIL: not queued\n"); return 1; }
 close(s[1]);                       /* the child ends */
 r = wpe_fd_write_flush(s[0]);
 if (r != WPE_FD_WRITE_CLOSED)
 { printf("FAIL: a write to an ended child said %d, not CLOSED\n", r); return 1; }
 if (wpe_fd_write_pending(s[0]) != 0)
 { printf("FAIL: the queue outlived the child\n"); return 1; }
 r = wpe_fd_write(s[0], "more\n", 5, WRITE_WHAT, WRITE_ENDS);
 close(s[0]);
 if (r != WPE_FD_WRITE_CLOSED)
 { printf("FAIL: a new write to an ended child said %d\n", r); return 1; }
 return 0;
}

/* No callback: the test only watches the socket, as a run watches an
 * adapter's transport. */
static void on_socket_watched(int fd, void *data)
{
 (void)fd;
 (void)data;
}

static int test_del_keeps_the_queue_and_close_forgets_it(void)
{
 int p[2], q[2], s[2], number;
 long fill = full_pipe(p), sfill;
 char got[64];

 if (fill <= 0) return 1;
 /* An adapter's socket unwatched between the run and the stop's requests
    keeps what is queued for it. */
 if (socketpair(AF_UNIX, SOCK_STREAM, 0, s) != 0) return 1;
 set_nonblocking(s[0]);
 set_nonblocking(s[1]);
 sfill = fill_until_full(s[0]);
 wpe_fd_add(s[0], POLLIN, on_socket_watched, NULL);
 if (wpe_fd_write(s[0], "req\n", 4, WRITE_WHAT, WRITE_ENDS) != WPE_FD_WRITE_QUEUED)
 { printf("FAIL: not queued on the socket\n"); return 1; }
 wpe_fd_del(s[0]);
 if (wpe_fd_write_pending(s[0]) != 4)
 { printf("FAIL: unwatching the socket lost its queue\n"); return 1; }
 if (drain_bytes(s[1], sfill) != 0) return 1;
 wpe_fd_poll(0);
 read_now(s[1], got, sizeof got);
 close(s[0]); close(s[1]);
 if (strcmp(got, "req\n"))
 { printf("FAIL: the unwatched socket's request read \"%s\"\n", got); return 1; }
 /* A child's input closed by the loop: nothing is left for it, and it is
    no longer watched. */
 if (wpe_fd_write(p[1], "old\n", 4, WRITE_WHAT, WRITE_ENDS) != WPE_FD_WRITE_QUEUED)
 { printf("FAIL: not queued\n"); return 1; }
 number = p[1];
 wpe_fd_add(number, POLLIN, on_socket_watched, NULL);
 wpe_fd_close(p[1]);
 close(p[0]);
 if (wpe_fd_write_pending(number) != 0)
 { printf("FAIL: the queue outlived wpe_fd_close\n"); return 1; }
 if (wpe_fd_find(number))
 { printf("FAIL: a closed descriptor is still watched\n"); return 1; }
 /* A new child's input on the same number gets nothing of the old one's. */
 if (pipe(q) != 0) return 1;
 if (q[1] != number && dup2(q[1], number) == number)
 {  close(q[1]);  q[1] = number;  }
 set_nonblocking(q[0]);
 set_nonblocking(q[1]);
 wpe_fd_poll(0);
 read_now(q[0], got, sizeof got);
 close(q[0]); close(q[1]);
 if (got[0])
 { printf("FAIL: the new descriptor was written \"%s\"\n", got); return 1; }
 return 0;
}

/* Is the writer's end of the pipe whose reader is @rd closed?  A read of an
 * emptied pipe says 0 (end of file) then, and EAGAIN while it is open. */
static int writer_closed(int rd)
{
 char c;

 return read(rd, &c, 1) == 0;
}

static int test_close_when_written_sends_every_byte_then_end_of_file(void)
{
 int p[2], q[2], r;
 long fill = full_pipe(p);
 char got[64];

 if (fill <= 0) return 1;
 if (wpe_fd_write(p[1], "last\n", 5, WRITE_WHAT, WRITE_ENDS) != WPE_FD_WRITE_QUEUED)
 { printf("FAIL: not queued\n"); return 1; }
 r = wpe_fd_close_when_written(p[1]);
 if (r != 1)
 { printf("FAIL: the input was closed with 5 bytes waiting (%d)\n", r); return 1; }
 if (fcntl(p[1], F_GETFD) < 0)
 { printf("FAIL: the descriptor is gone before its bytes went out\n"); return 1; }
 wpe_fd_poll(0);
 if (fcntl(p[1], F_GETFD) < 0)
 { printf("FAIL: closed while the child read nothing\n"); return 1; }
 if (drain_bytes(p[0], fill) != 0) return 1;
 wpe_fd_poll(0);
 read_now(p[0], got, sizeof got);
 if (strcmp(got, "last\n"))
 { printf("FAIL: the child read \"%s\", not the last bytes\n", got); return 1; }
 if (!writer_closed(p[0]))
 { printf("FAIL: no end of file after the last byte\n"); close(p[1]); close(p[0]); return 1; }
 close(p[0]);
 /* Nothing waiting: closed at once. */
 if (pipe(q) != 0) return 1;
 set_nonblocking(q[0]);
 set_nonblocking(q[1]);
 r = wpe_fd_close_when_written(q[1]);
 if (r != 0 || !writer_closed(q[0]))
 { printf("FAIL: an input with nothing queued was not closed at once (%d)\n", r); return 1; }
 close(q[0]);
 return 0;
}

/* ==== a queue the loop drops is told to its owner (rows F-1 .. F-8c) ====
 *
 * Each row is red on the loop before the owner existed or under the named
 * mutation of its comment, and waits only through test_loop_until: the bound
 * is how long a red run takes, a green run ends when its predicate holds. */

#define FAIL_BOUND_MS     2000
#define FAIL_TEXT         "cmd\n"
#define FAIL_PASSES       3
#define FAIL_ROWS_GUARD_S 60
#define CAPACITY_MAX      (32L * 1024L * 1024L)
#define REPORT_WRITE_TRIES 2   /* one more byte may still be taken: try twice */
#define F4_TEXT_MAX       64
#define REPORT_NO_HOW     (-999)
#define REPORT_NAME_MAX   32
#define PAIR_KINDS        3

/* What an owner saw when the loop told it. */
typedef struct {
 int calls;
 int fd;
 int how;
 long pending_at_call;     /* bytes still queued for fd when it was told   */
 int hook_depth_at_call;   /* loop hooks open when it was told             */
} told_t;

static int hook_depth = 0, hooks_before = 0, hooks_after = 0;

static void hook_before(void) { hook_depth++; hooks_before++; }
static void hook_after(void) { hook_depth--; hooks_after++; }

static void install_counting_hooks(void)
{
 hook_depth = hooks_before = hooks_after = 0;
 wpe_loop_hooks_set(hook_before, hook_after);
}

static void remove_counting_hooks(void)
{
 wpe_loop_hooks_set(NULL, NULL);
}

/* The owner every row but F-4 and F-5 registers: it only writes down what
   it was told. */
static void on_failed(int fd, int how, void *data)
{
 told_t *t = data;

 t->calls++;
 t->fd = fd;
 t->how = how;
 t->pending_at_call = wpe_fd_write_pending(fd);
 t->hook_depth_at_call = hook_depth;
}

static int told_once(void *ctx)
{
 return ((told_t *)ctx)->calls >= 1;
}

/* Give @number to the descriptor @fd, as an open of the same number would. */
static int claim_number(int number, int fd)
{
 if (fd != number)
 {
  if (dup2(fd, number) != number)
   return -1;
  close(fd);
 }
 return number;
}

/* The lowest number the reader of a test pipe may take: above every number
   a row reuses, so a number freed and claimed again never lands on it. */
#define PIPE_SPARE_MIN 100

/* A new pipe whose write end has the number @number, which is free: what an
   open that follows a close of the number gets.  The reader is moved above
   PIPE_SPARE_MIN first, since pipe() may itself return @number for it.
   Return: @number, or -1; @reader is the read end. */
static int pipe_on_number(int number, int *reader)
{
 int zp[2], high;

 if (pipe(zp) != 0)
  return -1;
 high = fcntl(zp[0], F_DUPFD, PIPE_SPARE_MIN);
 if (high < 0)
 {
  close(zp[0]);
  close(zp[1]);
  return -1;
 }
 close(zp[0]);
 *reader = high;
 return claim_number(number, zp[1]);
}

/* A pipe, full, with FAIL_TEXT queued behind the fill.  Return: the fill,
   or -1. */
static long open_queued_pipe(int p[2])
{
 long fill = full_pipe(p);

 if (fill <= 0)
  return -1;
 if (wpe_fd_write(p[1], FAIL_TEXT, strlen(FAIL_TEXT), WRITE_WHAT, WRITE_ENDS) !=
     WPE_FD_WRITE_QUEUED)
  return -1;
 return fill;
}

/* A queued pipe whose owner is @t.  Return: 0, or -1. */
static int open_owned_pipe(int p[2], told_t *t)
{
 if (open_queued_pipe(p) <= 0)
  return -1;
 return wpe_fd_write_failed_set(p[1], on_failed, t);
}

/* A pipe on @number, as the open that follows a close of the number gets it:
   full, FAIL_TEXT queued, its reader already closed, no owner.  Return: 0,
   or -1. */
static int open_reused_pipe_that_fails(int number)
{
 int reader;

 if (pipe_on_number(number, &reader) != number)
  return -1;
 set_nonblocking(reader);
 set_nonblocking(number);
 if (fill_until_full(number) <= 0)
  return -1;
 if (wpe_fd_write(number, FAIL_TEXT, strlen(FAIL_TEXT), WRITE_WHAT, WRITE_ENDS) !=
     WPE_FD_WRITE_QUEUED)
  return -1;
 close(reader);
 return 0;
}

/* Has the queue of the descriptor @ctx points to left the loop? */
static int queue_gone(void *ctx)
{
 return wpe_fd_write_pending(*(int *)ctx) == 0;
}

/* Is the descriptor @ctx points to closed? */
static int descriptor_closed(void *ctx)
{
 return fcntl(*(int *)ctx, F_GETFD) < 0;
}

/* Numbers no descriptor of the test holds, for entries that only fill the
   owner table. */
#define OWNER_FAKE_BASE 900

/* -- what each system makes of a closed peer, for the report -- */

enum { PAIR_PIPE, PAIR_SOCKETPAIR, PAIR_TCP };

static const char *pair_name(int kind)
{
 if (kind == PAIR_PIPE)
  return "pipe";
 if (kind == PAIR_SOCKETPAIR)
  return "socketpair";
 return "tcp-loopback";
}

static int report_wanted(void)
{
 const char *e = getenv("XWPE_TEST_REPORT");

 return e && *e && strcmp(e, "0") != 0;
}

/* The bytes @fd takes until it answers EAGAIN, with no F_GETPIPE_SZ: a pipe
   is written one byte at a time, the same count on every system.  A socket
   holds megabytes, so it takes whole chunks first and the last bytes one at
   a time, which gives the same edge in a few thousand writes.
   Return: the count, or -1 when it takes more than CAPACITY_MAX. */
static long fill_to_capacity(int fd, int chunked)
{
 char chunk[WRITE_FILL_CHUNK];
 long n = 0;
 ssize_t w;

 memset(chunk, 'f', sizeof chunk);
 while (chunked && (w = write(fd, chunk, sizeof chunk)) == (ssize_t)sizeof chunk)
 {
  n += w;
  if (n > CAPACITY_MAX)
   return -1;
 }
 if (chunked && w > 0)
  n += w;
 while ((w = write(fd, "f", 1)) == 1)
 {
  if (++n > CAPACITY_MAX)
   return -1;
 }
 return n;
}

/* A connected pair of TCP sockets on the loopback, @ours and @peer, both
   non-blocking.  Return: 0, or -1 when the system has no loopback. */
static int make_tcp_pair(int *ours, int *peer)
{
 struct sockaddr_in addr;
 socklen_t len = sizeof addr;
 int listener = socket(AF_INET, SOCK_STREAM, 0), c, a;

 if (listener < 0)
  return -1;
 memset(&addr, 0, sizeof addr);
 addr.sin_family = AF_INET;
 addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
 if (bind(listener, (struct sockaddr *)&addr, sizeof addr) != 0 ||
     listen(listener, 1) != 0 ||
     getsockname(listener, (struct sockaddr *)&addr, &len) != 0)
 {
  close(listener);
  return -1;
 }
 c = socket(AF_INET, SOCK_STREAM, 0);
 if (c < 0 || connect(c, (struct sockaddr *)&addr, sizeof addr) != 0)
 {
  if (c >= 0)
   close(c);
  close(listener);
  return -1;
 }
 a = accept(listener, NULL, NULL);
 close(listener);
 if (a < 0)
 {
  close(c);
  return -1;
 }
 set_nonblocking(c);
 set_nonblocking(a);
 *ours = c;
 *peer = a;
 return 0;
}

/* A connected pair of @kind, both non-blocking: @ours is what a client
   writes to, @peer is the child's end.  @full: @ours is filled until it
   takes no more (the child never read), @fill is the count.
   Return: 0, or -1. */
static int make_pair(int kind, int full, int *ours, int *peer, long *fill)
{
 int p[2];

 *fill = 0;
 if (kind == PAIR_PIPE)
 {
  if (pipe(p) != 0)
   return -1;
  *ours = p[1];
  *peer = p[0];
 }
 else if (kind == PAIR_SOCKETPAIR)
 {
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, p) != 0)
   return -1;
  *ours = p[0];
  *peer = p[1];
 }
 else if (make_tcp_pair(ours, peer) != 0)
  return -1;
 set_nonblocking(*ours);
 set_nonblocking(*peer);
 if (full)
 {
  *fill = fill_to_capacity(*ours, kind != PAIR_PIPE);
  if (*fill <= 0)
   return -1;
 }
 return 0;
}

static const char *errno_name(int e)
{
 if (e == 0)
  return "none";
 if (e == EPIPE)
  return "EPIPE";
 if (e == ECONNRESET)
  return "ECONNRESET";
 if (e == EAGAIN)
  return "EAGAIN";
 if (e == ENOTCONN)
  return "ENOTCONN";
 return "other";
}

/* Print one measured line (XWPE_TEST_REPORT=1): what poll() says of the
   write end @fd now and what one more byte written to it says. */
static void report_case(const char *row, int kind, const char *label,
                        long fill, int fd, int how)
{
 struct utsname u;
 struct pollfd pf;
 int err = 0, tries;
 char howtext[REPORT_NAME_MAX];

 pf.fd = fd;
 pf.events = POLLOUT;
 pf.revents = 0;
 poll(&pf, 1, 0);
 for (tries = 0; tries < REPORT_WRITE_TRIES; tries++)
 {
  errno = 0;
  err = write(fd, "x", 1) < 0 ? errno : 0;
  if (err)
   break;
 }
 if (how == REPORT_NO_HOW)
  snprintf(howtext, sizeof howtext, "-");
 else
  snprintf(howtext, sizeof howtext, "%d", how);
 if (uname(&u) != 0)
  snprintf(u.sysname, sizeof u.sysname, "unknown");
 printf("REPORT session-end row=%s sys=%s kind=%s case=%s pipe_fill=%ld "
        "revents=0x%x write_errno=%d(%s) how=%s\n", row, u.sysname,
        pair_name(kind), label, fill, (unsigned)pf.revents, err,
        errno_name(err), howtext);
}

/* The state without a queue: the peer closed, nothing waits. */
static void report_peer_closed(const char *row, int kind)
{
 int ours, peer;
 long fill;

 if (make_pair(kind, 0, &ours, &peer, &fill) != 0)
 {
  printf("REPORT session-end row=%s kind=%s unavailable\n", row, pair_name(kind));
  return;
 }
 close(peer);
 report_case(row, kind, kind == PAIR_PIPE ? "reader-closed" : "peer-closed",
             fill, ours, REPORT_NO_HOW);
 close(ours);
}

/* The state with the child's input full and a message queued behind it: the
   peer closes and the loop is run until its owner is told, or the bound.
   Return: what the owner was told, REPORT_NO_HOW when nobody was. */
static int run_full_pair(int kind, told_t *t, long *fill_out)
{
 int ours, peer;
 long fill;

 *fill_out = 0;
 memset(t, 0, sizeof *t);
 if (make_pair(kind, 1, &ours, &peer, &fill) != 0)
  return REPORT_NO_HOW;
 *fill_out = fill;
 memset(t, 0, sizeof *t);
 if (wpe_fd_write(ours, FAIL_TEXT, strlen(FAIL_TEXT), WRITE_WHAT, WRITE_ENDS) !=
     WPE_FD_WRITE_QUEUED ||
     wpe_fd_write_failed_set(ours, on_failed, t) != 0)
 {
  wpe_fd_close(ours);
  close(peer);
  return REPORT_NO_HOW;
 }
 close(peer);
 test_loop_until(told_once, t, FAIL_BOUND_MS);
 test_loop_passes(FAIL_PASSES);
 wpe_fd_close(ours);
 return t->calls ? t->how : REPORT_NO_HOW;
}

/* A pair of @kind in the state a closed peer leaves, run through the loop:
   the full one, with its owner told or not.  Prints the measured lines when
   XWPE_TEST_REPORT=1 asks.  The row asserts on @t, never on a line.
   Return: 0, or -1 when the system could not make the pair. */
static int run_kind(const char *row, int kind, told_t *t)
{
 long fill, twin_fill;
 int twin_ours, twin_peer;
 int how = run_full_pair(kind, t, &fill);

 if (report_wanted())
 {
  report_peer_closed(row, kind);
  if (make_pair(kind, 1, &twin_ours, &twin_peer, &twin_fill) == 0)
  {
   close(twin_peer);
   report_case(row, kind, kind == PAIR_PIPE ? "reader-closed-full" :
               "peer-closed-unread", twin_fill, twin_ours, how);
   close(twin_ours);
  }
  else
   printf("REPORT session-end row=%s kind=%s unavailable\n", row, pair_name(kind));
 }
 return fill > 0 ? 0 : -1;
}

/* Measure @kind for @row and print the lines, only when XWPE_TEST_REPORT=1
   asks: a row calls this at its end and asserts nothing on it. */
static void report_kind_when_asked(const char *row, int kind)
{
 told_t measured;

 if (report_wanted())
  run_kind(row, kind, &measured);
}

/* F-1.  A queue that fails on the loop tells its owner once, with CLOSED,
   after the queue is gone and between the loop's two hooks.
   Breaks when: the loop drops the writer's result, so the failed queue is
   never told (the loop before the owner existed). */
/* @scenario SC-session-end-01 */
static int test_failed_queue_is_told_to_its_owner_once(void)
{
 int p[2];
 told_t t;
 int told;

 memset(&t, 0, sizeof t);
 if (open_owned_pipe(p, &t) != 0) return 1;
 install_counting_hooks();
 close(p[0]);                         /* the child closed its input */
 told = test_loop_until(told_once, &t, FAIL_BOUND_MS);
 test_loop_passes(FAIL_PASSES);
 remove_counting_hooks();
 wpe_fd_close(p[1]);
 if (!told || t.calls != 1)
 { printf("FAIL: F-1: the owner was told %d times, not once\n", t.calls); return 1; }
 if (t.fd != p[1])
 { printf("FAIL: F-1: told about descriptor %d, not %d\n", t.fd, p[1]); return 1; }
 if (t.how != WPE_FD_WRITE_CLOSED)
 { printf("FAIL: F-1: told %d, not CLOSED\n", t.how); return 1; }
 if (t.pending_at_call != 0)
 { printf("FAIL: F-1: %ld bytes still queued when it was told\n", t.pending_at_call); return 1; }
 if (t.hook_depth_at_call != 1 || hooks_before != 1 || hooks_after != 1)
 { printf("FAIL: F-1: the call was not made inside the loop's two hooks "
          "(depth %d, before %d, after %d)\n", t.hook_depth_at_call, hooks_before, hooks_after); return 1; }
 report_kind_when_asked("F-1", PAIR_PIPE);
 return 0;
}

/* F-1b.  An owner told once is no owner any more: its descriptor closed
   behind the loop and its number taken by a new pipe whose queue fails, the
   old owner is not told a second time.
   Breaks when: the entry of a told owner stays in the table. */
/* @scenario SC-session-end-02 */
static int test_an_owner_told_once_is_not_told_for_its_number_again(void)
{
 int p[2], number;
 told_t t;

 memset(&t, 0, sizeof t);
 if (open_owned_pipe(p, &t) != 0) return 1;
 number = p[1];
 close(p[0]);
 if (!test_loop_until(told_once, &t, FAIL_BOUND_MS))
 { printf("FAIL: F-1b: the owner was never told\n"); wpe_fd_close(number); return 1; }
 close(number);                       /* not through wpe_fd_close */
 if (open_reused_pipe_that_fails(number) != 0) return 1;
 if (!test_loop_until(queue_gone, &number, FAIL_BOUND_MS))
 { printf("FAIL: F-1b: the reused number's queue never failed\n"); wpe_fd_close(number); return 1; }
 test_loop_passes(FAIL_PASSES);
 wpe_fd_close(number);
 if (t.calls != 1)
 { printf("FAIL: F-1b: the owner was told %d times, not once\n", t.calls); return 1; }
 return 0;
}

/* F-2a.  A write refused at the call is the return value's: nobody is
   called.  A second pipe that fails on the loop makes the loop tell its
   owners, so an owner marked by the write would be told in that pass.
   Breaks when: wpe_fd_write marks the owner of a queue it drops. */
/* @scenario SC-session-end-03 */
static int test_a_write_refused_at_the_call_tells_nobody(void)
{
 int a[2], b[2], r;
 told_t ta, tb;

 memset(&ta, 0, sizeof ta);
 memset(&tb, 0, sizeof tb);
 if (pipe(a) != 0) return 1;
 set_nonblocking(a[0]);
 set_nonblocking(a[1]);
 if (wpe_fd_write_failed_set(a[1], on_failed, &ta) != 0) return 1;
 if (open_owned_pipe(b, &tb) != 0) return 1;
 close(a[0]);
 r = wpe_fd_write(a[1], FAIL_TEXT, strlen(FAIL_TEXT), WRITE_WHAT, WRITE_ENDS);
 close(b[0]);
 test_loop_until(told_once, &tb, FAIL_BOUND_MS);
 test_loop_passes(FAIL_PASSES);
 wpe_fd_close(a[1]);
 wpe_fd_close(b[1]);
 if (r != WPE_FD_WRITE_CLOSED)
 { printf("FAIL: F-2a: the write to a closed input said %d\n", r); return 1; }
 if (tb.calls != 1)
 { printf("FAIL: F-2a: the second pipe's owner was told %d times\n", tb.calls); return 1; }
 if (ta.calls != 0)
 { printf("FAIL: F-2a: a write refused at the call also told its owner (%d)\n", ta.calls); return 1; }
 return 0;
}

/* F-2b.  A flush that fails returns the result, drops the queue, keeps the
   owner and calls nobody, in that call or in the passes after it.
   Breaks when: wpe_fd_write_flush marks the owner as the loop does. */
/* @scenario SC-session-end-04 */
static int test_a_failing_flush_returns_the_result_and_tells_nobody(void)
{
 int a[2], b[2], r;
 told_t ta, tb;
 const wpe_fd_write_owner *o;
 int owner_kept;

 memset(&ta, 0, sizeof ta);
 memset(&tb, 0, sizeof tb);
 if (open_owned_pipe(a, &ta) != 0) return 1;
 if (open_owned_pipe(b, &tb) != 0) return 1;
 close(a[0]);
 r = wpe_fd_write_flush(a[1]);
 o = wpe_fd_write_owner_find(a[1]);
 owner_kept = o != NULL && !o->failed;
 close(b[0]);
 test_loop_until(told_once, &tb, FAIL_BOUND_MS);
 test_loop_passes(FAIL_PASSES);
 if (r != WPE_FD_WRITE_CLOSED)
 { printf("FAIL: F-2b: the flush said %d, not CLOSED\n", r); wpe_fd_close(a[1]); wpe_fd_close(b[1]); return 1; }
 if (wpe_fd_write_pending(a[1]) != 0)
 { printf("FAIL: F-2b: the failed queue was kept\n"); wpe_fd_close(a[1]); wpe_fd_close(b[1]); return 1; }
 if (!owner_kept)
 { printf("FAIL: F-2b: the owner entry was dropped or marked\n"); wpe_fd_close(a[1]); wpe_fd_close(b[1]); return 1; }
 wpe_fd_close(a[1]);
 wpe_fd_close(b[1]);
 if (ta.calls != 0 || tb.calls != 1)
 { printf("FAIL: F-2b: told %d and %d times, not 0 and 1\n", ta.calls, tb.calls); return 1; }
 return 0;
}

/* F-3.  wpe_fd_close forgets the owner: the number, reused by a new pipe
   whose queue fails, does not reach it.
   Breaks when: wpe_fd_close does not forget the owner. */
/* @scenario SC-session-end-05 */
static int test_close_forgets_the_owner_so_a_reused_number_reaches_nobody(void)
{
 int p[2], number;
 told_t t;

 memset(&t, 0, sizeof t);
 if (open_owned_pipe(p, &t) != 0) return 1;
 number = p[1];
 wpe_fd_close(number);
 close(p[0]);
 if (open_reused_pipe_that_fails(number) != 0) return 1;
 if (!test_loop_until(queue_gone, &number, FAIL_BOUND_MS))
 { printf("FAIL: F-3: the reused number's queue never failed\n"); wpe_fd_close(number); return 1; }
 test_loop_passes(FAIL_PASSES);
 wpe_fd_close(number);
 if (t.calls != 0)
 { printf("FAIL: F-3: a closed descriptor's owner was told about its number's next user (%d)\n", t.calls); return 1; }
 return 0;
}

/* F-4.  Two queues fail in one pass.  The first owner is told after the
   walk (the second queue is dropped already), closes the second writer's
   descriptor and opens a pipe that takes its number and queues bytes on it.
   The second owner is never told, the new queue is intact and the next pass
   writes it.  Run under ASan.
   Breaks when: wpe_fd_flush_one calls the owner itself, inside the walk. */
/* @scenario SC-session-end-06 */
typedef struct {
 told_t x, y;
 int y_fd;
 long y_pending_at_x;      /* bytes queued for y when x was told          */
 int z_reader;
 long z_fill;
} f4_t;

static void on_x_failed(int fd, int how, void *data)
{
 f4_t *s = data;

 on_failed(fd, how, &s->x);
 s->y_pending_at_x = wpe_fd_write_pending(s->y_fd);
 wpe_fd_close(s->y_fd);
 if (pipe_on_number(s->y_fd, &s->z_reader) != s->y_fd)
  return;
 set_nonblocking(s->z_reader);
 set_nonblocking(s->y_fd);
 s->z_fill = fill_until_full(s->y_fd);
 wpe_fd_write(s->y_fd, FAIL_TEXT, strlen(FAIL_TEXT), WRITE_WHAT, WRITE_ENDS);
}

static int z_written(void *ctx)
{
 f4_t *s = ctx;

 return s->z_reader > 0 && wpe_fd_write_pending(s->y_fd) == 0;
}

static int test_an_owner_told_after_the_walk_may_close_another_writer(void)
{
 int xp[2], yp[2];
 f4_t s;
 char got[F4_TEXT_MAX];

 memset(&s, 0, sizeof s);
 s.z_reader = -1;
 if (open_queued_pipe(xp) <= 0 || open_queued_pipe(yp) <= 0) return 1;
 s.y_fd = yp[1];
 if (wpe_fd_write_failed_set(xp[1], on_x_failed, &s) != 0) return 1;
 if (wpe_fd_write_failed_set(yp[1], on_failed, &s.y) != 0) return 1;
 close(xp[0]);
 close(yp[0]);
 test_loop_until(told_once, &s.x, FAIL_BOUND_MS);
 test_loop_passes(FAIL_PASSES);
 if (s.x.calls != 1)
 { printf("FAIL: F-4: the first owner was told %d times\n", s.x.calls); return 1; }
 if (s.y_pending_at_x != 0)
 { printf("FAIL: F-4: the first owner was told inside the walk (%ld bytes still queued for the second)\n", s.y_pending_at_x); return 1; }
 if (s.y.calls != 0)
 { printf("FAIL: F-4: an owner whose descriptor was closed was told (%d)\n", s.y.calls); return 1; }
 if (s.z_reader < 0 || s.z_fill <= 0)
 { printf("FAIL: F-4: the new pipe was not made\n"); return 1; }
 if (wpe_fd_write_pending(s.y_fd) != (long)strlen(FAIL_TEXT))
 { printf("FAIL: F-4: the new queue holds %ld bytes\n", wpe_fd_write_pending(s.y_fd)); return 1; }
 if (drain_bytes(s.z_reader, s.z_fill) != 0) return 1;
 test_loop_until(z_written, &s, FAIL_BOUND_MS);
 read_now(s.z_reader, got, sizeof got);
 wpe_fd_close(xp[1]);
 wpe_fd_close(s.y_fd);
 close(s.z_reader);
 if (strcmp(got, FAIL_TEXT))
 { printf("FAIL: F-4: the new queue's bytes read \"%s\"\n", got); return 1; }
 return 0;
}

/* F-4b.  In one pass a reader is readable and a queue fails.  The queue's
   owner is told before any reader is served: it closes the reader, opens a
   pipe whose read end takes the reader's number and watches it.  Neither
   the closed reader nor the new one is served with the readiness poll()
   reported for the old descriptor.
   Breaks when: owners are told after the readers are served. */
/* @scenario SC-session-end-07 */
typedef struct {
 told_t q;
 int reader_fd;            /* the watched reader; its number is reused    */
 int new_writer;           /* write end of the pipe on the reused number  */
 int old_served;           /* the closed reader's callback ran            */
 int new_served;           /* the new reader's callback ran               */
} f4b_t;

static void on_old_reader(int fd, void *data)
{
 char c;

 ((f4b_t *)data)->old_served++;
 read_now(fd, &c, 1);
}

static void on_new_reader(int fd, void *data)
{
 (void)fd;
 ((f4b_t *)data)->new_served++;
}

/* A new pipe whose read end has the number @number, which is free.
   Return: @number, or -1; @writer is the write end. */
static int reader_on_number(int number, int *writer)
{
 int zp[2], high;

 if (pipe(zp) != 0)
  return -1;
 high = fcntl(zp[1], F_DUPFD, PIPE_SPARE_MIN);
 close(zp[1]);
 if (high < 0)
 {
  close(zp[0]);
  return -1;
 }
 *writer = high;
 return claim_number(number, zp[0]);
}

static void on_q_failed_closes_reader(int fd, int how, void *data)
{
 f4b_t *s = data;
 int number = s->reader_fd;

 on_failed(fd, how, &s->q);
 wpe_fd_close(number);
 if (reader_on_number(number, &s->new_writer) != number)
  return;
 set_nonblocking(number);
 wpe_fd_add(number, POLLIN, on_new_reader, s);
}

static int test_an_owner_is_told_before_any_reader_is_served(void)
{
 int q[2], r[2];
 f4b_t s;

 memset(&s, 0, sizeof s);
 s.new_writer = -1;
 if (pipe(r) != 0) return 1;
 set_nonblocking(r[0]);
 s.reader_fd = r[0];
 if (write(r[1], "r", 1) != 1) return 1;
 if (open_queued_pipe(q) <= 0) return 1;
 if (wpe_fd_write_failed_set(q[1], on_q_failed_closes_reader, &s) != 0) return 1;
 wpe_fd_add(r[0], POLLIN, on_old_reader, &s);
 close(q[0]);
 test_loop_until(told_once, &s.q, FAIL_BOUND_MS);
 test_loop_passes(FAIL_PASSES);
 wpe_fd_close(q[1]);
 wpe_fd_close(s.reader_fd);
 close(r[1]);
 if (s.new_writer >= 0)
  close(s.new_writer);
 if (s.q.calls != 1)
 { printf("FAIL: F-4b: the queue's owner was told %d times\n", s.q.calls); return 1; }
 if (s.old_served != 0)
 { printf("FAIL: F-4b: a reader was served before the owner was told (%d)\n", s.old_served); return 1; }
 if (s.new_served != 0)
 { printf("FAIL: F-4b: a reader opened on a reused number was served with the old one's readiness (%d)\n", s.new_served); return 1; }
 return 0;
}

/* F-5.  A number closed behind the loop (POLLNVAL) tells its owner with
   REFUSED and is not closed by the loop: a descriptor the owner opens on the
   number inside its call is still open afterwards.
   Breaks when: wpe_fd_writer_lost_number does not mark the owner (the
   queue was forgotten without a word).  The "never closes" half cannot go
   red by itself: the fitness script (clause C6, the one close) guards it. */
/* @scenario SC-session-end-08 */
typedef struct {
 told_t told;
 int opened;
} f5_t;

static void on_lost_number(int fd, int how, void *data)
{
 f5_t *s = data;
 int reader;

 on_failed(fd, how, &s->told);
 if (pipe_on_number(fd, &reader) == fd)
 {
  s->opened = 1;
  close(reader);
 }
}

static int f5_told(void *ctx)
{
 return ((f5_t *)ctx)->told.calls >= 1;
}

static int test_a_number_closed_behind_the_loop_tells_its_owner(void)
{
 int p[2], number;
 f5_t s;

 memset(&s, 0, sizeof s);
 if (open_queued_pipe(p) <= 0) return 1;
 number = p[1];
 if (wpe_fd_write_failed_set(number, on_lost_number, &s) != 0) return 1;
 close(number);                       /* closed without wpe_fd_close */
 test_loop_until(f5_told, &s, FAIL_BOUND_MS);
 test_loop_passes(FAIL_PASSES);
 close(p[0]);
 if (s.told.calls != 1)
 { printf("FAIL: F-5: a closed number told its owner %d times\n", s.told.calls); return 1; }
 if (s.told.how != WPE_FD_WRITE_REFUSED)
 { printf("FAIL: F-5: told %d, not REFUSED\n", s.told.how); return 1; }
 if (!s.opened || fcntl(number, F_GETFD) < 0)
 { printf("FAIL: F-5: the loop closed a number that was no longer its own\n"); return 1; }
 wpe_fd_close(number);
 return 0;
}

/* F-6.  The same as F-1 over a socketpair whose peer closed, whatever errno
   the system gives: the owner is told once with CLOSED or REFUSED.  The TCP
   loopback is measured and reported, never asserted (XWPE_TEST_REPORT=1).
   Breaks when: the loop drops the writer's result, as for F-1. */
/* @scenario SC-session-end-09 */
static int test_a_failed_socket_queue_tells_its_owner_once(void)
{
 told_t t;

 if (run_kind("F-6", PAIR_SOCKETPAIR, &t) != 0)
 { printf("FAIL: F-6: no socketpair in this state\n"); return 1; }
 if (t.calls != 1)
 { printf("FAIL: F-6: the owner was told %d times, not once\n", t.calls); return 1; }
 if (t.how != WPE_FD_WRITE_CLOSED && t.how != WPE_FD_WRITE_REFUSED)
 { printf("FAIL: F-6: told %d, neither CLOSED nor REFUSED\n", t.how); return 1; }
 if (t.pending_at_call != 0)
 { printf("FAIL: F-6: %ld bytes still queued when it was told\n", t.pending_at_call); return 1; }
 report_kind_when_asked("F-6", PAIR_TCP);
 return 0;
}

/* -- the owed call (F-7a .. F-7e) -- */

#define OWED_WAIT_MS 1000

static int owed_calls = 0, owed_again = 0, owed_timer_fired = 0;

static void owed_count(void)
{
 owed_calls++;
 if (owed_again && owed_calls == 1)
  wpe_loop_owe();
}

static void owed_other(void) { }
static void owed_timer(void *data) { (void)data; owed_timer_fired++; }
static int owed_ran(void) { return owed_calls >= 1; }

/* The loop owes at most one call and nobody else has named it. */
static void owed_reset(int again)
{
 loop_owed_fn = NULL;
 loop_owed_flag = 0;
 owed_calls = 0;
 owed_again = again;
 owed_timer_fired = 0;
}

/* F-7a.  With no descriptor and no timer, an owed call makes poll(-1) run it
   and return at once; asked once, it runs once.
   Breaks when: the early return of wpe_fd_poll ignores the owed call. */
/* @scenario SC-session-end-10 */
static int test_an_owed_call_runs_with_nothing_else_to_wait_for(void)
{
 int first, second;

 owed_reset(0);
 if (wpe_loop_owed_set(owed_count) != 0) return 1;
 wpe_loop_owe();
 first = wpe_fd_poll(OWED_WAIT_MS);
 second = wpe_fd_poll(OWED_WAIT_MS);
 if (owed_calls != 1)
 { printf("FAIL: F-7a: the owed call ran %d times\n", owed_calls); return 1; }
 if (first <= 0)
 { printf("FAIL: F-7a: the pass that ran it counted no work (%d)\n", first); return 1; }
 if (second != 0)
 { printf("FAIL: F-7a: nothing is owed and the next pass said %d\n", second); return 1; }
 return 0;
}

/* F-7b.  A wait whose only source is the owed call runs it and asks its
   condition again.
   Breaks when: wpe_loop_has_sources ignores the owed call. */
/* @scenario SC-session-end-11 */
static int test_a_wait_counts_the_owed_call_as_a_source(void)
{
 int r;

 owed_reset(0);
 if (wpe_loop_owed_set(owed_count) != 0) return 1;
 wpe_loop_owe();
 r = wpe_fd_wait_for(owed_ran, OWED_WAIT_MS, "the owed call");
 if (r != WPE_WAIT_DONE)
 { printf("FAIL: F-7b: the wait said %d, not DONE\n", r); return 1; }
 return 0;
}

/* F-7c.  With a descriptor watched and a timer far off, an unbounded poll
   does not sleep while the call is owed: it returns before the timer.
   Breaks when: wpe_fd_poll_bound is not shortened while the call is owed. */
/* @scenario SC-session-end-12 */
static int test_the_loop_does_not_sleep_while_a_call_is_owed(void)
{
 int p[2], id;

 owed_reset(0);
 if (pipe(p) != 0) return 1;
 wpe_fd_add(p[0], POLLIN, on_socket_watched, NULL);
 id = wpe_timer_add(NEVER_MS, 0, owed_timer, NULL);
 if (wpe_loop_owed_set(owed_count) != 0) return 1;
 wpe_loop_owe();
 wpe_fd_poll(WPE_POLL_FOREVER);   /* only the owed call or the timer ends it */
 wpe_timer_del(id);
 wpe_fd_del(p[0]);
 close(p[0]);
 close(p[1]);
 if (owed_calls != 1)
 { printf("FAIL: F-7c: the owed call ran %d times\n", owed_calls); return 1; }
 if (owed_timer_fired != 0)
 { printf("FAIL: F-7c: the poll slept until the timer instead of running the owed call\n"); return 1; }
 return 0;
}

/* F-7d.  A wpe_loop_owe() made inside the call is served by the next pass,
   once.
   Breaks when: the flag is cleared after the call, so the inner request is
   lost. */
/* @scenario SC-session-end-13 */
static int test_an_owe_made_inside_the_call_runs_on_the_next_pass_once(void)
{
 int calls_after_first, calls_after_second;

 owed_reset(1);
 if (wpe_loop_owed_set(owed_count) != 0) return 1;
 wpe_loop_owe();
 wpe_fd_poll(OWED_WAIT_MS);
 calls_after_first = owed_calls;
 wpe_fd_poll(OWED_WAIT_MS);
 calls_after_second = owed_calls;
 wpe_fd_poll(OWED_WAIT_MS);
 if (calls_after_first != 1 || calls_after_second != 2 || owed_calls != 2)
 { printf("FAIL: F-7d: the call ran %d, %d and %d times over three passes, not 1, 2 and 2\n",
          calls_after_first, calls_after_second, owed_calls); return 1; }
 return 0;
}

/* F-7e.  Naming a second owed call answers -1; the same one again, 0; no
   call, -1.
   Breaks when: naming the owed call always answers 0. */
/* @scenario SC-session-end-14 */
static int test_the_owed_call_is_named_once(void)
{
 owed_reset(0);
 if (wpe_loop_owed_set(NULL) != -1)
 { printf("FAIL: F-7e: a NULL call was accepted\n"); return 1; }
 if (wpe_loop_owed_set(owed_count) != 0)
 { printf("FAIL: F-7e: the first call was refused\n"); return 1; }
 if (wpe_loop_owed_set(owed_count) != 0)
 { printf("FAIL: F-7e: the same call again was refused\n"); return 1; }
 if (wpe_loop_owed_set(owed_other) != -1)
 { printf("FAIL: F-7e: a second call was accepted\n"); return 1; }
 owed_reset(0);
 return 0;
}

/* -- a descriptor the loop closes for its owner (F-8, F-8b, F-8c) -- */

/* F-8.  A queue whose close was handed to the loop fails: the loop closes it
   through wpe_fd_close, so an owner entry that survived is forgotten with
   it and the number's next user is told to nobody.  The close request is set
   on the queue directly, past the two guards of the public call, so this
   row holds the loop's own close.
   Breaks when: wpe_fd_writer_finish closes with a plain close(fd).  Only
   the owner's forget at that close holds this state, which no public call
   reaches. */
/* @scenario SC-session-end-15 */
static int test_a_queue_the_loop_closes_forgets_its_owner(void)
{
 int p[2], number;
 told_t t;
 wpe_fd_writer *w;

 memset(&t, 0, sizeof t);
 if (open_owned_pipe(p, &t) != 0) return 1;
 number = p[1];
 w = wpe_fd_writer_find(number);
 if (!w) return 1;
 w->close_when_empty = 1;
 close(p[0]);
 test_loop_until(descriptor_closed, &number, FAIL_BOUND_MS);
 test_loop_passes(FAIL_PASSES);
 if (!descriptor_closed(&number))
 { printf("FAIL: F-8: the loop did not close the failed queue's descriptor\n"); wpe_fd_close(number); return 1; }
 if (t.calls != 0)
 { printf("FAIL: F-8: a descriptor the loop closed told its owner (%d)\n", t.calls); return 1; }
 return 0;
}

/* F-8b.  An owner is refused for a descriptor whose close was handed to the
   loop, for a negative number, for no function, and when every entry is
   taken.
   Breaks when: wpe_fd_write_failed_set no longer refuses a descriptor the
   loop closes. */
/* @scenario SC-session-end-16 */
static int test_the_failure_owner_is_refused_where_it_cannot_be_kept(void)
{
 int p[2], i, r;
 told_t t;

 memset(&t, 0, sizeof t);
 if (open_queued_pipe(p) <= 0) return 1;
 if (wpe_fd_close_when_written(p[1]) != 1)
 { printf("FAIL: F-8b: the close was not handed to the loop\n"); return 1; }
 if (wpe_fd_write_failed_set(p[1], on_failed, &t) != -1)
 { printf("FAIL: F-8b: an owner was kept for a descriptor the loop closes\n"); return 1; }
 close(p[0]);
 test_loop_until(descriptor_closed, &p[1], FAIL_BOUND_MS);
 if (wpe_fd_write_failed_set(-1, on_failed, &t) != -1 ||
     wpe_fd_write_failed_set(p[1], NULL, &t) != -1)
 { printf("FAIL: F-8b: a negative number or no function was accepted\n"); return 1; }
 for (i = 0; i < WPE_FD_WRITERS_MAX; i++)
  if (wpe_fd_write_failed_set(OWNER_FAKE_BASE + i, on_failed, &t) != 0)
  { printf("FAIL: F-8b: entry %d of %d was refused\n", i, WPE_FD_WRITERS_MAX); return 1; }
 r = wpe_fd_write_failed_set(OWNER_FAKE_BASE + WPE_FD_WRITERS_MAX, on_failed, &t);
 for (i = 0; i < WPE_FD_WRITERS_MAX; i++)
  wpe_fd_write_failed_forget(OWNER_FAKE_BASE + i);
 if (r != -1)
 { printf("FAIL: F-8b: an owner past the table's size was accepted\n"); return 1; }
 return 0;
}

/* F-8c.  A queue whose close was handed to the loop, and whose number is
   closed behind the loop, tells nobody: it has no owner.
   Breaks when: wpe_fd_close_when_written does not forget the owner. */
/* @scenario SC-session-end-17 */
static int test_a_queue_the_loop_closes_has_no_owner_when_its_number_is_lost(void)
{
 int p[2], number;
 told_t t;

 memset(&t, 0, sizeof t);
 if (open_owned_pipe(p, &t) != 0) return 1;
 number = p[1];
 if (wpe_fd_close_when_written(number) != 1) return 1;
 close(number);
 if (!test_loop_until(queue_gone, &number, FAIL_BOUND_MS))
 { printf("FAIL: F-8c: the queue of a lost number was never dropped\n"); close(p[0]); return 1; }
 test_loop_passes(FAIL_PASSES);
 close(p[0]);
 if (t.calls != 0)
 { printf("FAIL: F-8c: a queue handed to the loop told its owner (%d)\n", t.calls); return 1; }
 return 0;
}

int main(void)
{
 int rc = 0;
 /* a poll that never wakes fails the test, by name, not the build */
 test_watchdog_case("test_fdloop: a poll that never wakes", HANG_GUARD_S);
 rc |= test_callback_fd_reaped_on_hup();
 rc |= test_wakeonly_fd_dropped_on_hup();
 rc |= test_one_shot_fires_once();
 rc |= test_repeating_fires_until_deleted();
 rc |= test_poll_sleeps_until_the_timer();
 rc |= test_late_timer_does_not_burst();
 rc |= test_callback_deletes_itself_and_adds_another();
 rc |= test_timer_and_fd_in_one_poll();
 rc |= test_nested_poll_runs_each_due_timer_once();
 rc |= test_nested_poll_does_not_refire_the_running_timer();
 rc |= test_nested_poll_from_an_fd_callback();
 rc |= test_timer_table_full_refuses_one_more();
 rc |= test_fd_table_full_refuses_one_more();
 rc |= test_fd_slot_reused_after_a_delete_anywhere();
 rc |= test_fd_added_into_a_removed_slot_waits_for_next_poll();
 rc |= test_fd_reopened_on_a_removed_number_waits_for_next_poll();
 rc |= test_nested_poll_does_not_reenter_the_running_fd();
 rc |= test_outer_pass_skips_what_a_nested_poll_served();
 rc |= test_dispatch_does_not_reenter_the_running_fd();
 rc |= test_fd_refusal_says_why();
 rc |= test_timer_ids_stay_unique_across_the_wrap();
 rc |= test_readable_now_tracks_data();
 rc |= test_readable_now_hangup_and_closed();
 rc |= test_wait_for_true_predicate_returns_at_once();
 rc |= test_wait_for_deadline_on_a_silent_descriptor();
 rc |= test_wait_for_nothing_registered_ends_at_once();
 rc |= test_wait_for_zero_deadline_asks_once();
 rc |= test_wait_for_serves_timers_and_names_the_wait();
 rc |= test_wait_for_serves_fd_callbacks();
 rc |= test_wait_for_hands_back_unread_wake_only_input();
 rc |= test_wait_serving_does_not_wake_for_a_muted_keyboard();
 rc |= test_write_queues_when_the_pipe_is_full_and_flushes_on_pollout();
 rc |= test_two_writes_arrive_whole_and_in_order();
 rc |= test_a_write_past_the_queue_bound_is_refused_whole();
 rc |= test_the_stall_line_comes_once_after_the_bound_and_clears_on_progress();
 rc |= test_the_stall_line_comes_when_every_timer_is_taken();
 rc |= test_epipe_drops_the_queue_and_reports_closed();
 rc |= test_del_keeps_the_queue_and_close_forgets_it();
 rc |= test_close_when_written_sends_every_byte_then_end_of_file();
 test_watchdog_case("test_fdloop: a queue the loop drops is told", FAIL_ROWS_GUARD_S);
 rc |= test_failed_queue_is_told_to_its_owner_once();
 rc |= test_an_owner_told_once_is_not_told_for_its_number_again();
 rc |= test_a_write_refused_at_the_call_tells_nobody();
 rc |= test_a_failing_flush_returns_the_result_and_tells_nobody();
 rc |= test_close_forgets_the_owner_so_a_reused_number_reaches_nobody();
 rc |= test_an_owner_told_after_the_walk_may_close_another_writer();
 rc |= test_an_owner_is_told_before_any_reader_is_served();
 rc |= test_a_number_closed_behind_the_loop_tells_its_owner();
 rc |= test_a_failed_socket_queue_tells_its_owner_once();
 rc |= test_an_owed_call_runs_with_nothing_else_to_wait_for();
 rc |= test_a_wait_counts_the_owed_call_as_a_source();
 rc |= test_the_loop_does_not_sleep_while_a_call_is_owed();
 rc |= test_an_owe_made_inside_the_call_runs_on_the_next_pass_once();
 rc |= test_the_owed_call_is_named_once();
 rc |= test_a_queue_the_loop_closes_forgets_its_owner();
 rc |= test_the_failure_owner_is_refused_where_it_cannot_be_kept();
 rc |= test_a_queue_the_loop_closes_has_no_owner_when_its_number_is_lost();
 if (rc == 0) printf("test_fdloop: OK\n");
 return rc;
}
