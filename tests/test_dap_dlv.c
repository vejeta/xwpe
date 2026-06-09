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
out:
 unlink(gofile);
 unlink(gomod);
 rmdir(dir);
 return rc;
}
