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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../we_bsp.h"
#include "../we_dap.h"

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

int main(void)
{
 char dir[] = "/tmp/xwpe-dapscala-XXXXXX";
 char scala[300], path[400], cmd[512];
 char host[64], mainclass[256], err[256];
 int port = 0, rc = 0;
 FILE *f;
 struct rec rec;
 e_dap_host hostcb;
 e_bsp_session *bsp;
 e_dap_session *s;
 char *val;

 if (!have_tools())
 {
  printf("SKIP: scala-cli is required for the Scala BSP/DAP test\n");
  return 77;
 }
 if (!mkdtemp(dir))
  return fail("mkdtemp");

 snprintf(scala, sizeof(scala), "%s/factorial.scala", dir);
 if (!(f = fopen(scala, "w"))) { rc = fail("write factorial.scala"); goto out; }
 fputs(SCALA_SRC, f); fclose(f);

 memset(&rec, 0, sizeof(rec));
 hostcb.on_stopped = on_stopped;
 hostcb.on_output = on_output;
 hostcb.on_terminated = on_terminated;
 hostcb.ud = &rec;

 /* BSP bootstrap -> DAP endpoint (this is the slow step on a cold cache). */
 bsp = e_bsp_start(dir, "factorial.scala", host, sizeof(host), &port,
                   mainclass, sizeof(mainclass), err, sizeof(err));
 if (!bsp) { printf("  (%s)\n", err); rc = fail("e_bsp_start (BSP bootstrap)"); goto out; }
 if (strcmp(mainclass, "Factorial") != 0)
 { printf("  (main class = %s)\n", mainclass); rc = fail("main class != Factorial"); goto closebsp; }

 s = e_dap_open_tcp(host, port, dir, NULL, &hostcb);
 if (!s) { rc = fail("e_dap_open_tcp (connect to Bloop DAP endpoint)"); goto closebsp; }

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
 snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
 if (system(cmd) != 0) { /* best-effort cleanup of our own mkdtemp dir */ }
 return rc;
}
