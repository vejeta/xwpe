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
out:
 unlink(rs);
 unlink(bin);
 rmdir(dir);
 return rc;
}
