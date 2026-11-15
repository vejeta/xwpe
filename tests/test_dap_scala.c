/* test_dap_scala.c -- integration test for the JVM/Scala debug path: the BSP
 * bootstrap (we_bsp.c, scala-cli/Bloop hosting scala-debug-adapter) plus the
 * DAP engine's TCP-client transport (we_dap.c, e_dap_open_tcp), against REAL
 * tooling, debugging a Scala 3 program.
 *
 * Sequence: e_bsp_start (setup-ide -> BSP initialize/buildTargets/compile/
 * scalaMainClasses/debugSession-start -> tcp:// endpoint) -> e_dap_open_tcp ->
 * line breakpoint -> run -> evaluate a local -> continue and watch it grow.
 *
 * SKIPs (exit 77) when `scala-cli` is missing.  Slow on first run (the build
 * server resolves the Scala toolchain via coursier), so it is not in the
 * default CI Test-Depends -- it self-skips there. */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../we_bsp.h"
#include "../we_dap.h"
#include "test_watchdog.h"

/* Line 6 is the loop body; `f` is in scope and grows 1,1,2,6,24,120,... */
static const char *SCALA_SRC =
 "object Factorial:\n"
 "  def main(args: Array[String]): Unit =\n"
 "    var f = 1L\n"
 "    var i = 1\n"
 "    while i <= 10 do\n"
 "      f = f * i\n"                       /* line 6: the breakpoint */
 "      i = i + 1\n"
 "    println(s\"factorial(10) = $f\")\n";

struct rec { int stops, last_line, ended; };

static void on_stopped(const char *file, int line, const char *reason, void *ud)
{
 struct rec *r = ud;
 (void)file; (void)reason;
 r->stops++;
 r->last_line = line;
}
static void on_output(const char *text, void *ud) { (void)text; (void)ud; }
static void on_terminated(void *ud) { ((struct rec *)ud)->ended = 1; }

static int have_tools(void)
{
 return system("sh -c 'command -v scala-cli >/dev/null 2>&1'") == 0;
}

static int fail(const char *msg) { printf("FAIL: %s\n", msg); return 1; }

/* The whole session gets this long (a cold coursier cache included) before
   the test gives up on it and fails, so a build server that stops answering
   fails `make check` with a line instead of holding it.  A slower machine
   sets TEST_DEADLINE_ENV. */
#define TEST_DEADLINE_S    600
#define MAIN_GUARD_SLACK_S 120  /* beyond the session's deadline: its end and the clean up */
#define TEST_DEADLINE_ENV  "XWPE_TEST_DAP_SCALA_DEADLINE_S"
/* After the session, its program gets this long to end by itself. */
#define PROGRAM_END_MS     5000
#define PROGRAM_COMM       "java"   /* the kernel's command name of a JVM */
#define PROC_ROOT          "/proc"
#define PROC_PATH_MAX      64
#define COMM_MAX           32

static int session(const char *dir);

/**
 * test_deadline_s - How long the session may take: TEST_DEADLINE_S, or
 * what TEST_DEADLINE_ENV says.
 */
static int test_deadline_s(void)
{
 const char *v = getenv(TEST_DEADLINE_ENV);
 int n = v ? atoi(v) : 0;

 return n > 0 ? n : TEST_DEADLINE_S;
}

/**
 * set_session_budgets - Give every wait of the sync layer the session's own
 * deadline, as long as the whole session may take.
 *
 * The layer's default budgets are the editor's (30 s for the first answer),
 * which a Scala build server on a cold cache can exceed; the session's
 * deadline above is what ends a server that stops answering, so no single
 * wait is cut earlier.  Both are scaled by XWPE_TEST_WAIT_SCALE.
 */
static void set_session_budgets(void)
{
 long ms = (long)test_deadline_s() * 1000L;

 e_dap_sync_budget_set(E_DAP_SYNC_START, ms);
 e_dap_sync_budget_set(E_DAP_SYNC_LAUNCH, ms);
 e_dap_sync_budget_set(E_DAP_SYNC_REQUEST, ms);
 e_dap_sync_budget_set(E_DAP_SYNC_RUN, ms);
}

/**
 * run_within_deadline - Run the session in a child of the test's, and give
 * up on it at the deadline.
 * @dir: the session's folder.
 *
 * Every wait of the session is bounded on its own, but a build server that
 * stops answering between them could still hold `make check` for many
 * minutes; past the deadline the child is killed and the test fails saying
 * so.  Only the child is signalled: the build server it may have started is
 * shared and is left alone.
 * Return: the session's exit status, or 1 when it was given up on.
 */
static int run_within_deadline(const char *dir)
{
 int st = 0, secs = test_deadline_s();
 pid_t pid;

 fflush(stdout);
 pid = fork();
 if (pid < 0)
  return fail("fork");
 if (pid == 0)
 {
  int rc = session(dir);

  fflush(stdout);
  _exit(rc);
 }
 /* test_wait_child ends the child by SIGKILL at the deadline (scaled by
    XWPE_TEST_WAIT_SCALE on top of TEST_DEADLINE_ENV) and returns -1. */
 st = test_wait_child(pid, (long)secs * 1000L);
 if (st == -1)
 {
  printf("FAIL: the Scala session did not finish within %d s "
         "(set %s for a slower machine); ended it\n", secs, TEST_DEADLINE_ENV);
  return 1;
 }
 return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

/**
 * is_program_in - Is @pid a live JVM whose working folder is @dir?
 * @pid: a process; @dir: the session's folder, resolved.
 *
 * The program runs as the shared build server's child, not the test's, so
 * it is told from every other JVM by the folder it was started in.
 */
static int is_program_in(int pid, const char *dir)
{
 char p[PROC_PATH_MAX], comm[COMM_MAX], cwd[PATH_MAX];
 FILE *f;
 ssize_t n;
 int ok;

 snprintf(p, sizeof p, PROC_ROOT "/%d/comm", pid);
 if (!(f = fopen(p, "r")))
  return 0;
 ok = fgets(comm, sizeof comm, f) != NULL;
 fclose(f);
 if (!ok || strncmp(comm, PROGRAM_COMM "\n", sizeof PROGRAM_COMM) != 0)
  return 0;
 snprintf(p, sizeof p, PROC_ROOT "/%d/cwd", pid);
 n = readlink(p, cwd, sizeof cwd - 1);
 if (n < 0)
  return 0;
 cwd[n] = '\0';
 return strcmp(cwd, dir) == 0;
}

/**
 * wait_program_end - Give a program that is still ending PROGRAM_END_MS.
 * @pid: the program.
 *
 * The process is not the test's child, so waitpid cannot wait for it:
 * test_wait_gone asks kill(pid, 0) between short sleeps, the same on every
 * system (no pidfd, so macOS waits too).
 * Return: 1 once it has ended.
 */
static int wait_program_end(int pid)
{
 return test_wait_gone(pid, PROGRAM_END_MS);
}

/**
 * end_programs_left - The session is over: no program of it may be left
 * running under the build server.
 * @dir: the session's folder.
 *
 * A program held at a breakpoint by a session that did not end it ignores
 * SIGTERM and stays for as long as the build server runs, so each one found
 * fails the test, named, and is ended (only it, never the server).
 * Return: 0 when none was left, 1 otherwise.
 */
static int end_programs_left(const char *dir)
{
 char real[PATH_MAX];
 struct dirent *e;
 DIR *d;
 int pid, rc = 0;

 if (!realpath(dir, real) || !(d = opendir(PROC_ROOT)))
  return 0;
 while ((e = readdir(d)) != NULL)
 {
  pid = atoi(e->d_name);
  if (pid <= 0 || !is_program_in(pid, real) || wait_program_end(pid))
   continue;
  printf("FAIL: the session left its program running (pid %d in %s); "
         "ended it\n", pid, real);
  kill(pid, SIGKILL);
  rc = 1;
 }
 closedir(d);
 return rc;
}

int main(void)
{
 char dir[] = "/tmp/xwpe-dapscala-XXXXXX";
 char cmd[512];
 int rc;

 if (!have_tools())
 {
  printf("SKIP: scala-cli is required for the Scala BSP/DAP test\n");
  return 77;
 }
 test_watchdog_arm("test_dap_scala (the session and its clean up)",
                   test_deadline_s() + MAIN_GUARD_SLACK_S);
 if (!mkdtemp(dir))
  return fail("mkdtemp");
 rc = run_within_deadline(dir);
 if (end_programs_left(dir))
  rc = 1;
 snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
 if (system(cmd) != 0) { /* best-effort cleanup of our own mkdtemp dir */ }
 return rc;
}

/**
 * session - The Scala session itself, in the folder @dir.
 * Return: 0 when it passed, 1 when it failed.
 */
static int session(const char *dir)
{
 char scala[300], path[400], cmd[512];
 char host[64], mainclass[256], err[256];
 int port = 0, rc = 0;
 FILE *f;
 struct rec rec;
 e_dap_host hostcb;
 e_bsp_session *bsp;
 e_dap_session *s;
 char *val;

 snprintf(scala, sizeof(scala), "%s/factorial.scala", dir);
 if (!(f = fopen(scala, "w"))) { rc = fail("write factorial.scala"); goto out; }
 fputs(SCALA_SRC, f); fclose(f);

 /* Pin the build JVM to an LTS (temurin:21).  The system default can be a
    too-new JDK (e.g. OpenJDK 26) that crashes the Scala 3 compiler, which would
    surface as a spurious "no main class found" bootstrap failure -- an
    environment problem, not an xwpe one.  A separate project.scala applies the
    directive to the whole build WITHOUT shifting factorial.scala's line numbers
    (the breakpoint is line 6).  Mirrors a real project (docs/examples/scala-lsp
    pins the same way). */
 snprintf(scala, sizeof(scala), "%s/project.scala", dir);
 if (!(f = fopen(scala, "w"))) { rc = fail("write project.scala"); goto out; }
 fputs("//> using jvm temurin:21\n", f); fclose(f);

 memset(&rec, 0, sizeof(rec));
 memset(&hostcb, 0, sizeof(hostcb));
 hostcb.on_stopped = on_stopped;
 hostcb.on_output = on_output;
 hostcb.on_terminated = on_terminated;
 hostcb.ud = &rec;

 /* The connection file first: the editor runs this as a child of its own on
    its loop; the test has no loop and simply waits for it. */
 if (e_bsp_needs_setup(dir))
 {
  snprintf(cmd, sizeof(cmd), "scala-cli setup-ide '%s' >/dev/null 2>&1", dir);
  if (system(cmd) != 0)
  { rc = fail("scala-cli setup-ide"); goto out; }
 }

 /* BSP bootstrap -> DAP endpoint (this is the slow step on a cold cache). */
 bsp = e_bsp_start(dir, "factorial.scala", host, sizeof(host), &port,
                   mainclass, sizeof(mainclass), err, sizeof(err));
 if (!bsp) { printf("  (%s)\n", err); rc = fail("e_bsp_start (BSP bootstrap)"); goto out; }
 if (strcmp(mainclass, "Factorial") != 0)
 { printf("  (main class = %s)\n", mainclass); rc = fail("main class != Factorial"); goto closebsp; }

 set_session_budgets();
 s = e_dap_open_tcp(host, port, dir, NULL, &hostcb);
 if (!s)
 {
  printf("  (%s)\n", e_dap_sync_last_wait());
  rc = fail("e_dap_open_tcp (connect to Bloop DAP endpoint)");
  goto closebsp;
 }

 snprintf(path, sizeof(path), "%s/factorial.scala", dir);
 e_dap_add_breakpoint(s, path, 6);

 if (e_dap_run(s) != 0) { rc = fail("e_dap_run"); goto close; }
 if (rec.last_line != 6)
 { printf("  (stopped at line %d)\n", rec.last_line);
   rc = fail("did not stop at the line-6 breakpoint"); goto close; }

 val = e_dap_evaluate(s, "f");
 if (!val) { rc = fail("evaluate(f) returned nothing"); goto close; }
 printf("  (f at first stop = %s)\n", val);
 free(val);

 /* continue around the loop until f grows past 1 (factorial: 1,1,2,6,24,...). */
 {
  int i, grew = 0;
  for (i = 0; i < 6 && !e_dap_ended(s); i++)
  {
   e_dap_step(s, "continue");
   val = e_dap_evaluate(s, "f");
   if (val && atol(val) > 1) grew = 1;
   free(val);
   if (grew) break;
  }
  if (!grew) { rc = fail("watch on f never grew past 1 while continuing"); goto close; }
 }

 printf("PASS: Scala BSP/DAP (Bloop) -- run -> line 6 -> watch f grows\n");

close:
 e_dap_close(s);
closebsp:
 e_bsp_close(bsp);
out:
 return rc;
}
