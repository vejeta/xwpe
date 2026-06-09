/* test_dap_gdb.c -- integration test for the DAP engine's STDIO transport
 * (we_dap.c, e_dap_open_stdio) against a REAL `gdb --interpreter=dap`, debugging
 * a rustc-built binary.  This is the Rust vertical slice: gdb is a first-class
 * Rust debugger (rust-gdb IS gdb) and ships a DAP interpreter over stdio.
 *
 * Sequence: compile (rustc -g) -> open stdio adapter -> line breakpoint -> run
 * to the breakpoint -> evaluate a variable -> continue and watch it change.
 *
 * SKIPs (exit 77) when `gdb` or `rustc` is missing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../we_dap.h"

/* Line 4 is the loop body; `f` is in scope and grows 1,1,2,6,24,120. */
static const char *RUST_SRC =
 "fn main() {\n"
 "    let n = 5;\n"
 "    let mut f = 1;\n"
 "    for i in 1..=n { f *= i; }\n"     /* line 4: the breakpoint */
 "    println!(\"result: {}\", f);\n"
 "}\n";

struct rec {
 int  stops;
 int  last_line;
 int  ended;
 char last_out[64];
};

static void on_stopped(const char *file, int line, const char *reason, void *ud)
{
 struct rec *r = ud;
 (void)file; (void)reason;
 r->stops++;
 r->last_line = line;
}

static void on_output(const char *text, void *ud)
{
 struct rec *r = ud;
 if (text)
  snprintf(r->last_out, sizeof(r->last_out), "%s", text);
}

static void on_terminated(void *ud) { ((struct rec *)ud)->ended = 1; }

static int have_tools(void)
{
 return system("sh -c 'command -v gdb >/dev/null 2>&1 && "
               "command -v rustc >/dev/null 2>&1'") == 0;
}

static int fail(const char *msg) { printf("FAIL: %s\n", msg); return 1; }

int main(void)
{
 char dir[] = "/tmp/xwpe-dapgdb-XXXXXX";
 char rs[300], bin[300], cmd[800], path[400];
 FILE *f;
 struct rec rec;
 e_dap_host host;
 e_dap_session *s;
 char *argv[] = { "gdb", "--interpreter=dap", NULL };
 char *val;
 int rc = 0;

 if (!have_tools())
 {
  printf("SKIP: gdb and rustc are required for the DAP stdio (Rust) test\n");
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
 host.on_stopped = on_stopped;
 host.on_output = on_output;
 host.on_terminated = on_terminated;
 host.ud = &rec;

 /* entry_func NULL: a function breakpoint on "main" stops at the C runtime stub
    (no source) for Rust; rely on the line breakpoint instead. */
 s = e_dap_open_stdio(argv, bin, NULL, dir, &host);
 if (!s) { rc = fail("e_dap_open_stdio (gdb --interpreter=dap handshake)"); goto out; }

 snprintf(path, sizeof(path), "%s/main.rs", dir);
 e_dap_add_breakpoint(s, path, 4);

 if (e_dap_run(s) != 0) { rc = fail("e_dap_run"); goto close; }
 if (rec.last_line != 4)
 { printf("  (stopped at line %d)\n", rec.last_line);
   rc = fail("did not stop at the line-4 breakpoint"); goto close; }

 val = e_dap_evaluate(s, "f");
 if (!val || strcmp(val, "1") != 0)
 { printf("  (f = %s)\n", val ? val : "(null)");
   rc = fail("evaluate(f) at first stop should be 1"); free(val); goto close; }
 free(val);

 /* continue around the loop until f exceeds 1 (gdb evaluates f BEFORE the
    iteration's multiply, so it lags by one: 1,1,2,6,...). */
 {
  int i, grew = 0;
  for (i = 0; i < 6 && !e_dap_ended(s); i++)
  {
   e_dap_step(s, "continue");
   val = e_dap_evaluate(s, "f");
   if (val && atoi(val) > 1) grew = 1;
   free(val);
   if (grew) break;
  }
  if (!grew) { rc = fail("watch on f never grew past 1 while continuing"); goto close; }
 }

 printf("PASS: DAP stdio engine vs real gdb (Rust: run -> line 4 -> watch f grows)\n");

close:
 e_dap_close(s);
out:
 unlink(rs);
 unlink(bin);
 rmdir(dir);
 return rc;
}
