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
/* The loop's source is compiled into this test, not linked, so the id wrap
 * test can put the private id counter next to INT_MAX instead of making two
 * billion calls to reach it.  Its tables are static: so is its table size. */
#include "we_fdloop.c"

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
 r = wpe_fd_wait_serving(serving_pred, WAIT_LABEL, p[0], serving_serve);
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
 if (rc == 0) printf("test_fdloop: OK\n");
 return rc;
}
