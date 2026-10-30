/* test_dap_dlv.c -- integration test for the DAP engine (we_dap.c) against a
 * REAL `dlv dap` adapter.  Drives the validated Go vertical slice:
 * open -> breakpoint -> run -> continue to the breakpoint -> evaluate a watch.
 *
 * SKIPs (exit 77) when `dlv` or `go` is not installed, so CI without the Go
 * toolchain stays green. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>

#include "../we_dap.h"

static const char *GO_SRC =
 "package main\n"
 "\n"
 "import \"fmt\"\n"
 "\n"
 "func main() {\n"
 "\tx := 1\n"
 "\ty := x + 1\n"
 "\tz := y * 21\n"          /* line 8: the breakpoint; here x=1, y=2 */
 "\tfmt.Println(z)\n"
 "}\n";

struct rec {
 int   stops;
 int   last_line;
 char  last_reason[32];
 int   ended;
};

static void on_stopped(const char *file, int line, const char *reason, void *ud)
{
 struct rec *r = ud;
 (void)file;
 r->stops++;
 r->last_line = line;
 snprintf(r->last_reason, sizeof(r->last_reason), "%s", reason ? reason : "");
}

static void on_output(const char *text, void *ud) { (void)text; (void)ud; }
static void on_terminated(void *ud) { ((struct rec *)ud)->ended = 1; }

static int have_tools(void)
{
 return system("sh -c 'command -v dlv >/dev/null 2>&1 && "
               "command -v go >/dev/null 2>&1'") == 0;
}

static int fail(const char *msg)
{
 printf("FAIL: %s\n", msg);
 return 1;
}

/* ---- a breakpoint set while the program runs ---------------------------- */

#define LIVE_LINE      8       /* the loop body: set while the loop runs  */
#define LIVE_BUDGET_MS 60000   /* dlv builds first, then places and stops */
#define RUNNING_MS     1500    /* the program runs this long, unstopped   */

/* Loops for minutes unless stopped, so the breakpoint goes in mid-run. */
static const char *LOOP_SRC =
 "package main\n"
 "\n"
 "import \"time\"\n"
 "\n"
 "func main() {\n"
 "\tn := 0\n"
 "\tfor {\n"
 "\t\tn++\n"
 "\t\ttime.Sleep(5 * time.Millisecond)\n"
 "\t\tif n > 100000 {\n"
 "\t\t\tbreak\n"
 "\t\t}\n"
 "\t}\n"
 "\tprintln(n)\n"
 "}\n";

struct live {
 int stops;
 int line;
 int told;           /* on_breakpoints calls naming LIVE_LINE        */
 int first_verified; /* what the setBreakpoints answer itself said  */
 int verified;       /* the latest word: answer or breakpoint event */
};

static void live_stopped(const char *file, int line, const char *reason,
                         void *ud)
{
 struct live *l = ud;

 (void)file;
 (void)reason;
 l->stops++;
 l->line = line;
}

static void live_note(struct live *l, int verified)
{
 if (l->told == 0)
  l->first_verified = verified;
 l->told++;
 l->verified = verified;
}

static void live_breakpoints(const char *file, const int *line,
                             const int *verified, const char *const *message,
                             int n, void *ud)
{
 int i;

 (void)file;
 (void)message;
 for (i = 0; i < n; i++)
  if (line[i] == LIVE_LINE)
   live_note(ud, verified[i]);
}

static long now_ms(void)
{
 struct timespec ts;

 clock_gettime(CLOCK_MONOTONIC, &ts);
 return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Follow the run as the editor's loop does, until it stops or ends; dlv's
   pty is drained as Messages would be. */
static int pump_to_stop(e_dap_session *s, int budget_ms)
{
 long end = now_ms() + budget_ms;
 int st = E_DAP_RUN_GOING;

 while (st == E_DAP_RUN_GOING && now_ms() < end)
 {
  struct pollfd p[2];
  int np = 1;

  p[0].fd = e_dap_fd(s);
  p[0].events = POLLIN;
  p[0].revents = 0;
  if (p[0].fd < 0)
   return E_DAP_RUN_ENDED;
  p[1].fd = e_dap_output_fd(s);
  p[1].events = POLLIN;
  p[1].revents = 0;
  if (p[1].fd >= 0)
   np = 2;
  if (poll(p, np, (int)(end - now_ms())) <= 0)
   continue;
  if (np == 2 && p[1].revents)
   e_dap_drain_output(s);
  if (p[0].revents)
   st = e_dap_run_pump(s);
 }
 return st;
}

/* @scenario SC-debug-08 */
static int live_breakpoint_row(void)
{
 static const int lines[] = { LIVE_LINE };
 char dir[] = "/tmp/xwpe-daplive-XXXXXX";
 char gofile[300], gomod[300];
 char *argv[] = { "dlv", "dap", NULL };
 struct live l;
 e_dap_host host;
 e_dap_session *s;
 FILE *f;
 int rc = 0;

 if (!mkdtemp(dir))
  return fail("mkdtemp for the live row");
 snprintf(gofile, sizeof(gofile), "%s/main.go", dir);
 snprintf(gomod, sizeof(gomod), "%s/go.mod", dir);
 if (!(f = fopen(gofile, "w"))) { rc = fail("write main.go"); goto out; }
 fputs(LOOP_SRC, f); fclose(f);
 if (!(f = fopen(gomod, "w"))) { rc = fail("write go.mod"); goto out; }
 fputs("module daplive\n\ngo 1.21\n", f); fclose(f);

 memset(&l, 0, sizeof(l));
 memset(&host, 0, sizeof(host));
 host.on_stopped = live_stopped;
 host.on_breakpoints = live_breakpoints;
 host.ud = &l;
 s = e_dap_open(argv, dir, "main.main", dir, &host);
 if (!s) { rc = fail("e_dap_open for the live row"); goto out; }
 if (e_dap_run(s) != 0 || l.stops < 1)
 { rc = fail("the live row did not stop at main.main"); goto close; }
 if (e_dap_resume(s, "continue") != 0)
 { rc = fail("resume refused at a stop"); goto close; }
 if (pump_to_stop(s, RUNNING_MS) != E_DAP_RUN_GOING || !e_dap_running(s))
 { rc = fail("the program was not running when the breakpoint was set");
   goto close; }
 if (e_dap_set_file_breakpoints_async(s, gofile, lines, 1) != 0)
 { rc = fail("setBreakpoints refused while running"); goto close; }
 if (pump_to_stop(s, LIVE_BUDGET_MS) != E_DAP_RUN_STOPPED ||
     l.line != LIVE_LINE)
 { printf("  (stopped at %d)\n", l.line);
   rc = fail("the program did not stop on the breakpoint set while it ran");
   goto close; }
 if (!l.verified)
 { rc = fail("dlv never verified the breakpoint set while running");
   goto close; }
 printf("PASS: dlv setBreakpoints while running: answered verified=%d, "
        "told %d time(s), stopped at line %d\n",
        l.first_verified, l.told, l.line);
close:
 e_dap_close(s);
out:
 unlink(gofile);
 unlink(gomod);
 rmdir(dir);
 return rc;
}

int main(void)
{
 char dir[] = "/tmp/xwpe-dap-XXXXXX";
 char gofile[300], gomod[300], path[400];
 FILE *f;
 struct rec rec;
 e_dap_host host;
 e_dap_session *s;
 char *argv[] = { "dlv", "dap", NULL };
 char *val;
 int rc = 0;

 if (!have_tools())
 {
  printf("SKIP: dlv and go are required for the DAP integration test\n");
  return 77;
 }
 if (!mkdtemp(dir))
  return fail("mkdtemp");

 snprintf(gofile, sizeof(gofile), "%s/main.go", dir);
 snprintf(gomod, sizeof(gomod), "%s/go.mod", dir);
 if (!(f = fopen(gofile, "w"))) return fail("write main.go");
 fputs(GO_SRC, f); fclose(f);
 if (!(f = fopen(gomod, "w"))) return fail("write go.mod");
 fputs("module dapdemo\n\ngo 1.21\n", f); fclose(f);

 memset(&rec, 0, sizeof(rec));
 memset(&host, 0, sizeof(host));
 host.on_stopped = on_stopped;
 host.on_output = on_output;
 host.on_terminated = on_terminated;
 host.ud = &rec;

 s = e_dap_open(argv, dir, "main.main", dir, &host);
 if (!s) { rc = fail("e_dap_open (adapter launch / handshake)"); goto out; }

 snprintf(path, sizeof(path), "%s/main.go", dir);
 e_dap_add_breakpoint(s, path, 8);

 if (e_dap_run(s) != 0) { rc = fail("e_dap_run"); goto close; }
 if (rec.stops < 1 || rec.last_line <= 0)
 { rc = fail("did not stop at a source line on Run"); goto close; }

 /* continue from the entry stop to the line-8 breakpoint */
 e_dap_step(s, "continue");
 if (rec.last_line != 8)
 { printf("  (stopped at line %d, reason %s)\n", rec.last_line, rec.last_reason);
   rc = fail("continue did not reach the breakpoint at line 8"); goto close; }

 val = e_dap_evaluate(s, "y");
 if (!val || strcmp(val, "2") != 0)
 { printf("  (watch y = %s)\n", val ? val : "(null)");
   rc = fail("evaluate(y) should be 2"); free(val); goto close; }
 free(val);

 printf("PASS: DAP engine vs real dlv (run -> breakpoint line 8 -> watch y=2)\n");

close:
 e_dap_close(s);
 if (rc == 0)
  rc = live_breakpoint_row();
out:
 unlink(gofile);
 unlink(gomod);
 rmdir(dir);
 return rc;
}
