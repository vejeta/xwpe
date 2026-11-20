/* test_bsp_close.c -- how e_bsp_close ends the build server's launcher.
 *
 * No scala-cli needed: this program stands in for the launcher.  Run with
 * FAKE_FLAG it is a minimal BSP server that answers the handshake e_bsp_start
 * drives, starts a long-lived child of its own (the stand-in for the shared
 * Bloop daemon, which the real launcher may start as its child), and then
 * either takes a moment to end after SIGTERM or ignores it.
 *
 * What is checked: e_bsp_close returns at once, whatever the launcher does,
 * and the editor's children's owner (we_child.c) ends it on the fd loop; a
 * launcher that needs a moment after SIGTERM gets it and ends by itself (it
 * is not SIGKILLed at a fixed instant); a launcher that ignores SIGTERM is
 * killed after the owner's grace; and the launcher's own child is never
 * signalled, because only the launcher's PID is, never its process group.
 * The test reaps as the editor's SIGCHLD handler does.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "../we_bsp.h"
#include "../we_child.h"
#include "../we_dap_proto.h"
#include "../we_fdloop.h"
#define TEST_WANT_CHILD_STATE   /* the process table, for "has it ended" and "is it reaped" */
#include "test_watchdog.h"

/* Every case runs under the watchdog with its own name: a hang fails naming
   the case within CASE_GUARD_S (scaled), not at a CI step limit. */
#define CASE_GUARD_S 60
#define RUN(c) do { test_watchdog_case(#c, CASE_GUARD_S); c(); } while (0)

#define FAKE_FLAG        "--fake-bsp"
#define FAKE_SLOW_TERM   "slow-term"  /* ends FAKE_TERM_CLEANUP_MS after TERM */
#define FAKE_DEAF        "deaf"       /* ignores SIGTERM                      */
#define FAKE_TERM_CLEANUP_MS 500      /* longer than the old fixed 200 ms     */
#define FAKE_LIFETIME_S  30           /* a stand-in left over ends by itself  */
#define FAKE_READ_CHUNK  4096
#define FAKE_FRAME_MAX   1024
#define CLOSE_SLACK_MS   400
#define CLOSE_RETURNS_MS 100          /* e_bsp_close must not wait            */
#define LOOP_SLICE_MS    20
#define GONE_WAIT_MS     (WPE_CHILD_END_GRACE_MS + WPE_CHILD_END_KILL_MS + \
                          2 * CLOSE_SLACK_MS)
#define PATH_TEXT_MAX    (PATH_MAX + 64)
#define FAKE_DAP_URI     "tcp://127.0.0.1:5005"
#define MS_PER_SEC       1000L
#define NS_PER_MS        1000000L
/* A launcher that ends at SIGTERM is reaped as it exits. */
#define SLOW_TERM_BOUND_MS (FAKE_TERM_CLEANUP_MS + CLOSE_SLACK_MS)

static char self_path[PATH_MAX];
static int failures = 0;

/* ---- the fake launcher ------------------------------------------------- */

static volatile sig_atomic_t fake_termed = 0;

/** fake_on_term - Note SIGTERM, so the fake can clean up before it ends. */
static void fake_on_term(int sig)
{
 (void)sig;
 fake_termed = 1;
}

/** fake_write_pid - Leave @pid in @dir/@name for the test to read. */
static void fake_write_pid(const char *dir, const char *name, pid_t pid)
{
 char path[PATH_TEXT_MAX];
 FILE *f;

 snprintf(path, sizeof path, "%s/%s", dir, name);
 f = fopen(path, "w");
 if (!f)
  return;
 fprintf(f, "%ld\n", (long)pid);
 fclose(f);
}

/**
 * fake_start_daemon - Start the stand-in for the shared build server: a
 * child that outlives the launcher, as Bloop does when the launcher started
 * it.  It ends by itself after FAKE_LIFETIME_S.
 */
static void fake_start_daemon(const char *dir)
{
 pid_t pid = fork();

 if (pid == 0)
 {
  close(STDIN_FILENO);
  close(STDOUT_FILENO);
  signal(SIGTERM, SIG_DFL);
  test_lifetime_arm(FAKE_LIFETIME_S);
  for (;;)
   pause();
 }
 fake_write_pid(dir, "daemon.pid", pid);
}

/** fake_result - The result the fake answers to a BSP request @method. */
static const char *fake_result(const char *method)
{
 if (!strcmp(method, "workspace/buildTargets"))
  return "{\"targets\":[{\"id\":{\"uri\":\"file:///fake/?id=fake\"}}]}";
 if (!strcmp(method, "buildTarget/scalaMainClasses"))
  return "{\"items\":[{\"classes\":[{\"class\":\"Main\"}]}]}";
 if (!strcmp(method, "debugSession/start"))
  return "{\"uri\":\"" FAKE_DAP_URI "\"}";
 return "{}";
}

/** fake_answer - Answer one request (a message with an id). */
static void fake_answer(struct json_object *m)
{
 struct json_object *jid, *jm;
 char body[FAKE_FRAME_MAX / 2], frame[FAKE_FRAME_MAX];
 int n;

 if (!json_object_object_get_ex(m, "id", &jid) ||
     !json_object_object_get_ex(m, "method", &jm))
  return;
 snprintf(body, sizeof body, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":%s}",
          json_object_get_int(jid), fake_result(json_object_get_string(jm)));
 n = snprintf(frame, sizeof frame, "Content-Length: %zu\r\n\r\n%s",
              strlen(body), body);
 if (write(STDOUT_FILENO, frame, (size_t)n) != n)
  _exit(2);
}

/** fake_serve - Answer requests until the editor closes the input. */
static void fake_serve(void)
{
 e_dap_reader rd;
 char buf[FAKE_READ_CHUNK];
 ssize_t n;

 e_dap_reader_init(&rd);
 while ((n = read(STDIN_FILENO, buf, sizeof buf)) != 0)
 {
  struct json_object *m;
  int err = 0;

  if (n < 0 && errno == EINTR)
   continue;
  if (n < 0)
   break;
  e_dap_reader_push(&rd, buf, (size_t)n);
  while ((m = e_dap_reader_pop(&rd, &err)) != NULL)
  {
   fake_answer(m);
   json_object_put(m);
  }
 }
 e_dap_reader_free(&rd);
}

/**
 * fake_wait_for_term - After the input ends, wait for SIGTERM; then clean
 * up for FAKE_TERM_CLEANUP_MS and leave the marker.  A launcher killed
 * before that leaves none.
 */
static int fake_wait_for_term(const char *dir)
{
 char marker[PATH_TEXT_MAX];
 sigset_t term, old;
 FILE *f;

 sigemptyset(&term);
 sigaddset(&term, SIGTERM);
 sigprocmask(SIG_BLOCK, &term, &old);
 while (!fake_termed)
  sigsuspend(&old);
 test_sleep_ms(FAKE_TERM_CLEANUP_MS);
 snprintf(marker, sizeof marker, "%s/ended-cleanly", dir);
 f = fopen(marker, "w");
 if (f)
  fclose(f);
 return 0;
}

/** fake_launcher - The stand-in for scala-cli's BSP launcher. */
static int fake_launcher(const char *mode, const char *dir)
{
 int deaf = !strcmp(mode, FAKE_DEAF);

 test_lifetime_arm(FAKE_LIFETIME_S);
 fake_write_pid(dir, "launcher.pid", getpid());
 fake_start_daemon(dir);
 if (deaf)
  signal(SIGTERM, SIG_IGN);
 else
  signal(SIGTERM, fake_on_term);
 fake_serve();
 if (!deaf)
  return fake_wait_for_term(dir);
 for (;;)
  pause();
}

/* ---- the test side ----------------------------------------------------- */

/**
 * now_ms - A monotonic instant in milliseconds, to time e_bsp_close the way
 * a user pressing Quit would feel it.
 */
static long now_ms(void)
{
 struct timespec ts;

 clock_gettime(CLOCK_MONOTONIC, &ts);
 return (long)ts.tv_sec * MS_PER_SEC + ts.tv_nsec / NS_PER_MS;
}

/**
 * check - Print one result and count a failure, so a broken close is named
 * by the behaviour the user would miss.
 */
static void check(int ok, const char *what)
{
 if (ok)
 {
  printf("ok: %s\n", what);
  return;
 }
 printf("FAIL: %s\n", what);
 failures++;
}

/** read_pid - The PID the fake left in @dir/@name, or -1. */
static pid_t read_pid(const char *dir, const char *name)
{
 char path[PATH_TEXT_MAX];
 long pid = -1;
 FILE *f;

 snprintf(path, sizeof path, "%s/%s", dir, name);
 f = fopen(path, "r");
 if (!f)
  return -1;
 if (fscanf(f, "%ld", &pid) != 1)
  pid = -1;
 fclose(f);
 return (pid_t)pid;
}

/** exists - True when the fake left @dir/@name (its clean-end marker). */
static int exists(const char *dir, const char *name)
{
 char path[PATH_TEXT_MAX];
 struct stat st;

 snprintf(path, sizeof path, "%s/%s", dir, name);
 return stat(path, &st) == 0;
}

/**
 * check_reaped - The watched process is gone from the process table, and the
 * line of a failure says what the table did say (pid, state, errno).
 * @w: the process as first seen.  @what: the check's words.
 *
 * "Reaped" and not only "ended": a launcher the editor never waited for is a
 * zombie, and a row called "gone and reaped" must fail on it.  Asked of the
 * process table (tests/child_state.h), which never waits itself, so the
 * editor's own reaper is the only one that can have taken the status.
 */
static void check_reaped(const ChildWatch *w, const char *what)
{
 char said[160];

 if (w->pid > 0 && child_reaped(w))
 {
  check(1, what);
  return;
 }
 child_describe(w, said, sizeof said);
 printf("  %s\n", said);
 check(0, what);
}

/** check_running - The watched process has not ended (a zombie has). */
static void check_running(const ChildWatch *w, const char *what)
{
 char said[160];

 if (w->pid > 0 && !child_ended(w))
 {
  check(1, what);
  return;
 }
 child_describe(w, said, sizeof said);
 printf("  %s\n", said);
 check(0, what);
}

/** write_connection_file - The .bsp file that makes e_bsp_start run us. */
static int write_connection_file(const char *dir, const char *mode)
{
 char path[PATH_TEXT_MAX];
 FILE *f;

 snprintf(path, sizeof path, "%s/.bsp", dir);
 if (mkdir(path, 0700) != 0)
  return -1;
 snprintf(path, sizeof path, "%s/.bsp/fake.json", dir);
 f = fopen(path, "w");
 if (!f)
  return -1;
 fprintf(f, "{\"name\":\"fake\",\"argv\":[\"%s\",\"%s\",\"%s\",\"%s\"]}\n",
         self_path, FAKE_FLAG, mode, dir);
 fclose(f);
 return 0;
}

/** remove_dir - Remove the files this test made in @dir, then @dir. */
static void remove_dir(const char *dir)
{
 static const char *names[] = { ".bsp/fake.json", ".bsp", "launcher.pid",
                                "daemon.pid", "ended-cleanly" };
 char path[PATH_TEXT_MAX];
 unsigned int i;

 for (i = 0; i < sizeof names / sizeof names[0]; i++)
 {
  snprintf(path, sizeof path, "%s/%s", dir, names[i]);
  remove(path);
 }
 rmdir(dir);
}

struct closed {
 long  ms;          /* how long e_bsp_close took                      */
 long  gone_ms;     /* from the close until the launcher was reaped   */
 pid_t launcher;
 pid_t daemon;
 ChildWatch launcher_w;   /* the launcher as first seen, before the close */
 ChildWatch daemon_w;     /* the stand-in daemon as first seen */
};

/* The handler the editor installs (WpeSignalChild), in its essentials. */
static void on_child(int sig)
{
 (void)sig;
 wpe_child_reap_now();
}

/** install_reaper - Reap children as the editor does, so the owner sees ends. */
static void install_reaper(void)
{
 wpe_child_sigchld_install(on_child);
}

/**
 * loop_until_gone - Run the editor's loop until the watched process is
 * reaped, or @ms.
 * Return: how long it took, from @t0 on.
 */
static long loop_until_gone(const ChildWatch *w, long t0, long ms)
{
 long deadline = now_ms() + ms;

 while (!child_reaped(w) && now_ms() < deadline)
  wpe_fd_poll(LOOP_SLICE_MS);
 return now_ms() - t0;
}

/**
 * start_and_close - Start a session on the fake in @mode, close it, and
 * report how long the close took, how long until the launcher was gone,
 * and which processes the fake had.
 * Return: 0, or -1 when the session did not start.
 */
static int start_and_close(const char *dir, const char *mode,
                           struct closed *out)
{
 char host[64], mainclass[64], err[256];
 int port = 0;
 e_bsp_session *b;
 long t0;

 if (write_connection_file(dir, mode) != 0)
  return -1;
 b = e_bsp_start(dir, "Main.scala", host, sizeof host, &port,
                 mainclass, sizeof mainclass, err, sizeof err);
 if (!b)
 {
  printf("  e_bsp_start: %s\n", err);
  return -1;
 }
 out->launcher = read_pid(dir, "launcher.pid");
 out->daemon = read_pid(dir, "daemon.pid");
 out->launcher_w = child_watch(out->launcher);
 out->daemon_w = child_watch(out->daemon);
 t0 = now_ms();
 e_bsp_close(b);
 out->ms = now_ms() - t0;
 out->gone_ms = loop_until_gone(&out->launcher_w, t0, GONE_WAIT_MS);
 return 0;
}

/** end_daemon - End the fake's stand-in daemon, which the test made. */
static void end_daemon(pid_t pid)
{
 if (pid > 0)
  kill(pid, SIGKILL);
}

/**
 * test_close_lets_the_launcher_end_after_sigterm - A launcher that needs a
 * moment after SIGTERM gets it, and Quit goes on as soon as it has ended.
 */
static void test_close_lets_the_launcher_end_after_sigterm(void)
{
 char dir[] = "/tmp/xwpe-bspclose-XXXXXX";
 struct closed c;

 if (!mkdtemp(dir))
 {
  check(0, "mkdtemp");
  return;
 }
 if (start_and_close(dir, FAKE_SLOW_TERM, &c) != 0)
 {
  check(0, "a session on the fake launcher starts");
  remove_dir(dir);
  return;
 }
 printf("  close took %ld ms, the launcher was gone after %ld ms\n",
        c.ms, c.gone_ms);
 check(c.ms < CLOSE_RETURNS_MS, "Quit goes on at once");
 check(exists(dir, "ended-cleanly"),
       "a launcher that needs a moment after SIGTERM ends by itself");
 check(c.gone_ms < SLOW_TERM_BOUND_MS,
       "it is reaped as soon as it has ended");
 check_reaped(&c.launcher_w, "the launcher is gone and reaped");
 check_running(&c.daemon_w,
               "the launcher's own child (the shared server) is not signalled");
 end_daemon(c.daemon);
 remove_dir(dir);
}

/**
 * test_close_ends_a_launcher_that_ignores_sigterm - A launcher deaf to
 * SIGTERM is still ended within the bound, so Quit never hangs.
 */
static void test_close_ends_a_launcher_that_ignores_sigterm(void)
{
 char dir[] = "/tmp/xwpe-bspclose-XXXXXX";
 struct closed c;

 if (!mkdtemp(dir))
 {
  check(0, "mkdtemp");
  return;
 }
 if (start_and_close(dir, FAKE_DEAF, &c) != 0)
 {
  check(0, "a session on the fake launcher starts");
  remove_dir(dir);
  return;
 }
 printf("  close took %ld ms, the launcher was gone after %ld ms\n",
        c.ms, c.gone_ms);
 check(c.ms < CLOSE_RETURNS_MS, "Quit goes on at once");
 check(c.gone_ms >= WPE_CHILD_END_GRACE_MS - CLOSE_SLACK_MS,
       "a launcher that ignores SIGTERM gets the whole grace first");
 check(c.gone_ms < WPE_CHILD_END_GRACE_MS + CLOSE_SLACK_MS,
       "and is then killed: its end stays bounded");
 check_reaped(&c.launcher_w, "the launcher is gone and reaped (launcher that ignores SIGTERM)");
 check_running(&c.daemon_w,
               "the launcher's own child (the shared server) is not signalled");
 end_daemon(c.daemon);
 remove_dir(dir);
}

/* @scenario SC-debug-75 */
int main(int argc, char **argv)
{
 if (argc >= 4 && !strcmp(argv[1], FAKE_FLAG))
  return fake_launcher(argv[2], argv[3]);
 if (!realpath(argv[0], self_path))
  snprintf(self_path, sizeof self_path, "%s", argv[0]);
 signal(SIGPIPE, SIG_IGN);
 install_reaper();
 RUN(test_close_lets_the_launcher_end_after_sigterm);
 RUN(test_close_ends_a_launcher_that_ignores_sigterm);
 test_watchdog_disarm();
 if (failures == 0)
  return 0;
 printf("%d check(s) failed\n", failures);
 return 1;
}
