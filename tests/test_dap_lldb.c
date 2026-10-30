/* test_dap_lldb.c -- integration test for the DAP engine's STDIO transport
 * against `lldb-dap` (the LLVM Debug Adapter), debugging a rustc-built binary.
 *
 * Same stdio path as test_dap_gdb.c, different adapter.  lldb-dap is the native
 * Rust/C/C++ debugger on macOS and an alternative on Linux; this pins that the
 * engine drives it unchanged.  A multi-line loop (the multiply on its own line)
 * is used so the breakpoint binds to a single statement and the watch advances
 * deterministically.
 *
 * SKIPs (exit 77) when `lldb-dap` or `rustc` is missing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <signal.h>

#include "../we_dap.h"

static const char *RUST_SRC =
 "fn main() {\n"
 "    let n = 5;\n"
 "    let mut f = 1;\n"
 "    let mut i = 1;\n"
 "    while i <= n {\n"
 "        f = f * i;\n"            /* line 6: the breakpoint */
 "        i = i + 1;\n"
 "    }\n"
 "    println!(\"result: {}\", f);\n"
 "}\n";

struct rec { int stops; int last_line; int ended; };

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
 return system("sh -c 'command -v lldb-dap >/dev/null 2>&1 && "
               "command -v rustc >/dev/null 2>&1'") == 0;
}
static int fail(const char *msg) { printf("FAIL: %s\n", msg); return 1; }

/* ---- a breakpoint set while the program runs ---------------------------- */

#define START_LINE     2       /* the first stop                          */
#define LIVE_LINE      4       /* the loop body: set while the loop runs  */
#define LIVE_BUDGET_MS 30000   /* for the adapter to place it and stop    */
#define RUNNING_MS     1500    /* the program runs this long, unstopped   */

/* Loops for minutes unless stopped, so the breakpoint goes in mid-run. */
static const char *LOOP_SRC =
 "fn main() {\n"
 "    let mut n: u64 = 0;\n"
 "    loop {\n"
 "        n += 1;\n"
 "        std::thread::sleep(std::time::Duration::from_millis(5));\n"
 "        if n > 100000 { break; }\n"
 "    }\n"
 "    println!(\"{}\", n);\n"
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

/* Follow the run as the editor's loop does, until it stops or ends. */
static int pump_to_stop(e_dap_session *s, int budget_ms)
{
 long end = now_ms() + budget_ms;
 int st = E_DAP_RUN_GOING;

 while (st == E_DAP_RUN_GOING && now_ms() < end)
 {
  struct pollfd p;

  p.fd = e_dap_fd(s);
  p.events = POLLIN;
  p.revents = 0;
  if (p.fd < 0)
   return E_DAP_RUN_ENDED;
  if (poll(&p, 1, (int)(end - now_ms())) > 0)
   st = e_dap_run_pump(s);
 }
 return st;
}

/* ---- the editor ends while the program runs ------------------------------ */

#define PID_FILE        "prog.pid"
#define PROGRAM_GONE_MS 1500    /* the program ends this soon after Quit  */

/* Writes its PID, then loops for minutes unless stopped. */
static const char *PID_SRC =
 "fn main() {\n"
 "    std::fs::write(\"%s\", std::process::id().to_string()).unwrap();\n"
 "    let mut n: u64 = 0;\n"
 "    loop {\n"
 "        n += 1;\n"
 "        std::thread::sleep(std::time::Duration::from_millis(5));\n"
 "        if n > 100000 { break; }\n"
 "    }\n"
 "    println!(\"{}\", n);\n"
 "}\n";

static int end_requested(void) { return 1; }

/* The program's PID, from the file it wrote, or 0. */
static int read_program_pid(const char *path)
{
 FILE *f = fopen(path, "r");
 int pid = 0;

 if (!f)
  return 0;
 if (fscanf(f, "%d", &pid) != 1)
  pid = 0;
 fclose(f);
 return pid;
}

/* Is @pid gone within PROGRAM_GONE_MS?  A program left behind is ended
   here, by its own PID, so no test leaves one either. */
static int program_gone(int pid)
{
 long end = now_ms() + PROGRAM_GONE_MS;

 while (kill(pid, 0) == 0 && now_ms() < end)
  poll(NULL, 0, 50);
 if (kill(pid, 0) != 0)
  return 1;
 kill(pid, SIGKILL);
 return 0;
}

/* A logout or "kill PID" while the program runs: the editor's end request
   is already raised when it closes the session.  lldb-dap ends a running
   program only on `disconnect`; giving up on it then would leave the
   program running.
   @scenario SC-debug-18 */
static int ending_editor_row(const char *dir)
{
 char *argv[] = { "lldb-dap", NULL };
 char src[300], bin[300], pidf[300], cmd[800];
 int endp[2], pid, rc = 0;
 struct live l;
 e_dap_host host;
 e_dap_session *s;
 FILE *f;

 snprintf(src, sizeof(src), "%s/pidloop.rs", dir);
 snprintf(bin, sizeof(bin), "%s/pidloop", dir);
 snprintf(pidf, sizeof(pidf), "%s/" PID_FILE, dir);
 if (!(f = fopen(src, "w"))) return fail("write pidloop.rs");
 fprintf(f, PID_SRC, pidf);
 fclose(f);
 snprintf(cmd, sizeof(cmd), "rustc -g -o %s %s 2>/dev/null", bin, src);
 if (system(cmd) != 0) { rc = fail("rustc -g build of pidloop.rs"); goto out; }
 memset(&l, 0, sizeof(l));
 memset(&host, 0, sizeof(host));
 host.on_stopped = live_stopped;
 host.ud = &l;
 s = e_dap_open_stdio(argv, bin, NULL, dir, &host);
 if (!s) { rc = fail("e_dap_open_stdio for the ending row"); goto out; }
 e_dap_add_breakpoint(s, src, 3);
 if (e_dap_run(s) != 0 || l.line != 3)
 { e_dap_close(s); rc = fail("the ending row did not stop at line 3");
   goto out; }
 pid = read_program_pid(pidf);
 if (pid <= 1 || e_dap_resume(s, "continue") != 0 ||
     pump_to_stop(s, RUNNING_MS) != E_DAP_RUN_GOING)
 { e_dap_close(s); rc = fail("the program did not run"); goto out; }
 if (pipe(endp) != 0 || write(endp[1], "x", 1) != 1)
 { e_dap_close(s); rc = fail("pipe"); goto out; }
 e_dap_set_end_watch(endp[0], end_requested, NULL);
 e_dap_close(s);
 e_dap_set_end_watch(-1, NULL, NULL);
 close(endp[0]);
 close(endp[1]);
 if (!program_gone(pid))
 { rc = fail("the editor's end left lldb-dap's running program behind");
   goto out; }
 printf("PASS: lldb-dap, editor ending while the program runs: "
        "program %d gone\n", pid);
out:
 unlink(src);
 unlink(bin);
 unlink(pidf);
 return rc;
}

/* @scenario SC-debug-08 */
static int live_breakpoint_row(const char *dir)
{
 static const int lines[] = { LIVE_LINE };
 char *argv[] = { "lldb-dap", NULL };
 char src[300], bin[300], cmd[800];
 struct live l;
 e_dap_host host;
 e_dap_session *s;
 FILE *f;
 int rc = 0;

 snprintf(src, sizeof(src), "%s/loop.rs", dir);
 snprintf(bin, sizeof(bin), "%s/loop", dir);
 if (!(f = fopen(src, "w"))) return fail("write loop.rs");
 fputs(LOOP_SRC, f); fclose(f);
 snprintf(cmd, sizeof(cmd), "rustc -g -o %s %s 2>/dev/null", bin, src);
 if (system(cmd) != 0) { rc = fail("rustc -g build of loop.rs"); goto out; }

 memset(&l, 0, sizeof(l));
 memset(&host, 0, sizeof(host));
 host.on_stopped = live_stopped;
 host.on_breakpoints = live_breakpoints;
 host.ud = &l;
 s = e_dap_open_stdio(argv, bin, NULL, dir, &host);
 if (!s) { rc = fail("e_dap_open_stdio for the live row"); goto out; }
 e_dap_add_breakpoint(s, src, START_LINE);
 if (e_dap_run(s) != 0 || l.line != START_LINE)
 { rc = fail("the live row did not stop at its start line"); goto close; }
 if (e_dap_resume(s, "continue") != 0)
 { rc = fail("resume refused at a stop"); goto close; }
 if (pump_to_stop(s, RUNNING_MS) != E_DAP_RUN_GOING || !e_dap_running(s))
 { rc = fail("the program was not running when the breakpoint was set");
   goto close; }
 if (e_dap_set_file_breakpoints_async(s, src, lines, 1) != 0)
 { rc = fail("setBreakpoints refused while running"); goto close; }
 if (pump_to_stop(s, LIVE_BUDGET_MS) != E_DAP_RUN_STOPPED ||
     l.line != LIVE_LINE)
 { printf("  (stopped at %d)\n", l.line);
   rc = fail("the program did not stop on the breakpoint set while it ran");
   goto close; }
 if (!l.verified)
 { rc = fail("lldb-dap never verified the breakpoint set while running");
   goto close; }
 printf("PASS: lldb-dap setBreakpoints while running: answered verified=%d, "
        "told %d time(s), stopped at line %d\n",
        l.first_verified, l.told, l.line);
close:
 e_dap_close(s);
out:
 unlink(src);
 unlink(bin);
 return rc;
}

int main(void)
{
 char dir[] = "/tmp/xwpe-daplldb-XXXXXX";
 char rs[300], bin[300], cmd[800], path[400];
 FILE *f;
 struct rec rec;
 e_dap_host host;
 e_dap_session *s;
 char *argv[] = { "lldb-dap", NULL };
 char *val;
 int rc = 0, i, grew = 0;

 if (!have_tools())
 {
  printf("SKIP: lldb-dap and rustc are required for the DAP/lldb test\n");
  return 77;
 }
 if (!mkdtemp(dir))
  return fail("mkdtemp");

 snprintf(rs, sizeof(rs), "%s/main.rs", dir);
 snprintf(bin, sizeof(bin), "%s/main", dir);
 if (!(f = fopen(rs, "w"))) return fail("write main.rs");
 fputs(RUST_SRC, f); fclose(f);
 snprintf(cmd, sizeof(cmd), "rustc -g -o %s %s 2>/dev/null", bin, rs);
 if (system(cmd) != 0) { rc = fail("rustc -g build"); goto out; }

 memset(&rec, 0, sizeof(rec));
 memset(&host, 0, sizeof(host));
 host.on_stopped = on_stopped;
 host.on_output = on_output;
 host.on_terminated = on_terminated;
 host.ud = &rec;

 s = e_dap_open_stdio(argv, bin, NULL, dir, &host);
 if (!s) { rc = fail("e_dap_open_stdio (lldb-dap handshake)"); goto out; }

 snprintf(path, sizeof(path), "%s/main.rs", dir);
 e_dap_add_breakpoint(s, path, 6);

 if (e_dap_run(s) != 0) { rc = fail("e_dap_run"); goto close; }
 if (rec.last_line != 6)
 { printf("  (stopped at line %d)\n", rec.last_line);
   rc = fail("did not stop at the line-6 breakpoint"); goto close; }

 val = e_dap_evaluate(s, "f");
 if (!val || strcmp(val, "1") != 0)
 { printf("  (f = %s)\n", val ? val : "(null)");
   rc = fail("evaluate(f) at first stop should be 1"); free(val); goto close; }
 free(val);

 for (i = 0; i < 6 && !e_dap_ended(s); i++)
 {
  e_dap_step(s, "continue");
  val = e_dap_evaluate(s, "f");
  if (val && atoi(val) > 1) grew = 1;
  free(val);
  if (grew) break;
 }
 if (!grew) { rc = fail("watch on f never grew past 1 while continuing"); goto close; }

 printf("PASS: DAP stdio engine vs real lldb-dap (Rust: run -> line 6 -> watch f grows)\n");

close:
 e_dap_close(s);
 if (rc == 0)
  rc = live_breakpoint_row(dir);
 if (rc == 0)
  rc = ending_editor_row(dir);
out:
 unlink(rs);
 unlink(bin);
 rmdir(dir);
 return rc;
}
