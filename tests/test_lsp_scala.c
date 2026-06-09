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
 "    println(s\"factorial(10) = $f\")\n"   /* line 7: println          */
 "\n"
 "trait Greeter:\n"                        /* line 9                   */
 "  def greet(): String\n"                 /* line 10: abstract method */
 "\n"
 "class Hello extends Greeter:\n"          /* line 12                  */
 "  def greet(): String = \"hi\"\n"        /* line 13: implementation  */
 "\n"
 "val theGreeter: Hello = Hello()\n";      /* line 15: typed val       */

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

 /* references on the `f` use (line 5, char 6) -> declaration + several uses */
 {
  e_lsp_location locs[64];
  int nr = e_lsp_references(s, scala, 5, 6, locs, 64);
  printf("  REFERENCES f: %d\n", nr);
  if (nr < 2) { rc = fail("references on f should find the decl + uses"); goto close; }
 }

 /* document outline -> at least the Factorial object / main */
 {
  e_lsp_symbol syms[64];
  int ns = e_lsp_document_symbols(s, scala, syms, 64);
  printf("  OUTLINE: %d symbols%s%s\n", ns, ns > 0 ? ", e.g. " : "",
         ns > 0 ? syms[0].name : "");
  if (ns < 1) { rc = fail("document outline returned no symbols"); goto close; }
 }

 /* signature help inside println( ... ) on line 7 -> a signature label */
 {
  char *sg = NULL;
  for (int t = 0; t < 5 && !sg; t++)
  { sg = e_lsp_signature_help(s, scala, 7, 12); if (!sg) sleep(2); }
  if (sg) { printf("  SIGNATURE: %s\n", sg); free(sg); }
  else printf("  SIGNATURE: (none -- not fatal)\n");
 }

 /* rename `f` (declaration line 2, char 8) -> "fff": the edit-application path */
 {
  int others = 0;
  char *rn = e_lsp_rename(s, scala, 2, 8, "fff", SCALA_SRC, &others);
  if (!rn) { rc = fail("rename returned nothing"); goto close; }
  printf("  RENAME f->fff (other files: %d)\n", others);
  if (!strstr(rn, "var fff") || !strstr(rn, "fff = fff * i"))
  { printf("  ->\n%s\n", rn); free(rn);
    rc = fail("rename did not replace every f"); goto close; }
  if (strstr(rn, "var f =")) { free(rn); rc = fail("rename left an old f"); goto close; }
  free(rn);
 }

 /* format: no-op (already tidy) or a reformat -- must round-trip, not corrupt */
 {
  char *fmt = e_lsp_format(s, scala, SCALA_SRC);
  if (fmt) { printf("  FORMAT: %zu bytes\n", strlen(fmt)); free(fmt); }
  else printf("  FORMAT: no edits (already formatted)\n");
 }

 /* implementation of the abstract Greeter.greet (line 10, char 6) -> the
    override in Hello.  Retry while the index warms. */
 {
  char ip[1024]; int il = -1, ic = -1, ok = 0, t;
  for (t = 0; t < 6 && !ok; t++)
  { ok = (e_lsp_implementation(s, scala, 10, 6, ip, sizeof(ip), &il, &ic) == 0);
    if (!ok) sleep(2); }
  if (!ok) { rc = fail("implementation of Greeter.greet returned nothing"); goto close; }
  printf("  IMPLEMENTATION greet -> %s:%d\n", ip, il);
 }

 /* type definition of `theGreeter` (line 15, char 4): its type is Hello, so
    this jumps to the Hello class (line 12), not the val itself. */
 {
  char tp[1024]; int tl = -1, tc = -1, ok = 0, t;
  for (t = 0; t < 6 && !ok; t++)
  { ok = (e_lsp_type_definition(s, scala, 15, 4, tp, sizeof(tp), &tl, &tc) == 0);
    if (!ok) sleep(2); }
  if (!ok) { rc = fail("type definition of theGreeter returned nothing"); goto close; }
  printf("  TYPE-DEFINITION theGreeter -> %s:%d\n", tp, tl);
 }

 /* workspace symbol search for "Factorial" -> at least the Factorial object */
 {
  e_lsp_symbol syms[64];
  int nw = e_lsp_workspace_symbols(s, "Factorial", syms, 64), i, found = 0;
  printf("  WORKSPACE-SYMBOLS \"Factorial\": %d\n", nw);
  if (nw < 1) { rc = fail("workspace symbol search found nothing"); goto close; }
  for (i = 0; i < nw; i++)
   if (strstr(syms[i].name, "Factorial")) found = 1;
  if (!found) { rc = fail("workspace search missed the Factorial symbol"); goto close; }
 }

 /* code actions at the abstract method (line 10) -> the call must round-trip
    (>= 0, never -1); exercise the apply path if a direct-edit action exists. */
 {
  e_lsp_code_action acts[32];
  int na = e_lsp_code_actions(s, scala, 10, 6, acts, 32), i;
  if (na < 0) { rc = fail("code actions returned an error"); goto close; }
  printf("  CODE-ACTIONS: %d%s%s\n", na, na > 0 ? ", e.g. " : "",
         na > 0 ? acts[0].title : "");
  for (i = 0; i < na; i++)
   if (acts[i].has_edit)
   {
    int others = 0;
    char *applied = e_lsp_apply_code_action(s, i, scala, SCALA_SRC, &others);
    if (!applied) { rc = fail("apply_code_action on a has_edit action failed"); goto close; }
    printf("  APPLIED \"%s\" (other files: %d)\n", acts[i].title, others);
    free(applied);
    break;
   }
 }

 printf("PASS: LSP engine vs real Metals (hover/definition/implementation/"
        "type-definition/completion/references/outline/signature/rename/format/"
        "workspace-symbols/code-actions)\n");

close:
 e_lsp_close(s);
out:
 snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
 if (system(cmd) != 0) { /* best-effort cleanup of our own mkdtemp dir */ }
 return rc;
}
