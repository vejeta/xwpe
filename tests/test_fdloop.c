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
 if (rc == 0) printf("test_fdloop: OK\n");
 return rc;
}
