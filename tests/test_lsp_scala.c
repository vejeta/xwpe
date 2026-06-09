/* test_lsp_scala.c -- integration test for the LSP engine (we_lsp.c) against a
 * REAL Metals language server, on a single Scala 3 file.
 *
 * Sequence: scala-cli setup-ide (so Metals finds the BSP) -> e_lsp_open (spawn
 * metals, initialize/initialized) -> didOpen -> wait diagnostics (== compiled)
 * -> hover (type), definition (declaration line), completion (items).
 *
 * SKIPs (exit 77) when metals or scala-cli is missing.  Slow on a cold cache
 * (Metals imports the build and warms the presentation compiler), so it is not
 * in the default CI Test-Depends -- it self-skips there. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../we_lsp.h"

static const char *SCALA_SRC =
 "object Factorial:\n"
 "  def main(args: Array[String]): Unit =\n"
 "    var f = 1L\n"                        /* line 2: declaration of f */
 "    var i = 1\n"
 "    while i <= 10 do\n"
 "      f = f * i\n"                       /* line 5: a use of f       */
 "      i = i + 1\n"
 "    println(s\"factorial(10) = $f\")\n";  /* line 7: println          */

static int g_diags = 0;
static void on_diag(const char *path, int line, int ch, int sev,
                    const char *msg, void *ud)
{
 (void)path; (void)line; (void)ch; (void)sev; (void)msg; (void)ud;
 g_diags++;
}

static int have_tools(void)
{
 return system("sh -c 'command -v metals >/dev/null 2>&1 && "
               "command -v scala-cli >/dev/null 2>&1'") == 0;
}

static int fail(const char *m) { printf("FAIL: %s\n", m); return 1; }

int main(void)
{
 char dir[] = "/tmp/xwpe-lspscala-XXXXXX";
 char scala[300], cmd[512];
 char *argv[] = { "metals", NULL };
 e_lsp_host host;
 e_lsp_session *s;
 char *hov;
 char defpath[1024];
 int defline = -1, defchar = -1, rc = 0, n;
 e_lsp_completion_item items[64];
 FILE *f;

 if (!have_tools())
 {
  printf("SKIP: metals and scala-cli are required for the LSP test\n");
  return 77;
 }
 if (!mkdtemp(dir))
  return fail("mkdtemp");
 snprintf(scala, sizeof(scala), "%s/Factorial.scala", dir);
 if (!(f = fopen(scala, "w"))) { rc = fail("write Factorial.scala"); goto out; }
 fputs(SCALA_SRC, f); fclose(f);

 /* create .bsp so Metals connects to scala-cli's build server */
 snprintf(cmd, sizeof(cmd), "cd '%s' && scala-cli setup-ide . >/dev/null 2>&1", dir);
 if (system(cmd) != 0) { rc = fail("scala-cli setup-ide"); goto out; }

 host.on_diagnostic = on_diag;
 host.ud = NULL;
 s = e_lsp_open(argv, dir, "scala", &host);
 if (!s) { rc = fail("e_lsp_open (metals initialize handshake)"); goto out; }

 if (e_lsp_did_open(s, scala, SCALA_SRC) != 0) { rc = fail("did_open"); goto close; }
 if (!e_lsp_wait_diagnostics(s, scala, 240000))
 { rc = fail("no diagnostics (Metals never compiled the file)"); goto close; }
 printf("  compiled (%d diagnostic(s) delivered)\n", g_diags);

 /* hover on `f` in `var f = 1L` (line 2, char 8) -> type Long.  Retry while
    the presentation compiler warms. */
 hov = NULL;
 for (int t = 0; t < 8 && !hov; t++)
 {
  hov = e_lsp_hover(s, scala, 2, 8);
  if (!hov) sleep(3);
 }
 if (!hov) { rc = fail("hover on f returned nothing"); goto close; }
 printf("  HOVER f: %s\n", hov);
 if (!strstr(hov, "Long")) { free(hov); rc = fail("hover did not mention Long"); goto close; }
 free(hov);

 /* definition of the `f` use (line 5, char 6) -> its declaration on line 2 */
 if (e_lsp_definition(s, scala, 5, 6, defpath, sizeof(defpath), &defline, &defchar) != 0)
 { rc = fail("definition of f returned nothing"); goto close; }
 printf("  DEFINITION f -> line %d\n", defline);
 if (defline != 2) { rc = fail("definition of f should point to line 2"); goto close; }

 /* completion at end of `println` (line 7, char 11) -> non-empty */
 n = e_lsp_completion(s, scala, 7, 11, items, 64);
 printf("  COMPLETION: %d items%s%s\n", n, n > 0 ? ", e.g. " : "",
        n > 0 ? items[0].label : "");
 if (n <= 0) { rc = fail("completion returned no items"); goto close; }

 printf("PASS: LSP engine vs real Metals (compile -> hover -> definition -> completion)\n");

close:
 e_lsp_close(s);
out:
 snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
 if (system(cmd) != 0) { /* best-effort cleanup of our own mkdtemp dir */ }
 return rc;
}
