/* test_lsp_clangd.c -- integration test for the LSP engine (we_lsp.c) against a
 * REAL clangd language server, on a single C file.
 *
 * This is the breadth proof: the same engine that drives Metals for Scala must
 * drive clangd for C with NO server-specific plumbing -- only the language id
 * ("c") and the spawned command ("clangd") differ.  So it exercises the same
 * we_lsp.h calls as test_lsp_scala.c: open (spawn clangd, initialize/initialized)
 * -> didOpen -> diagnostics -> hover -> definition -> completion -> references
 * -> outline -> rename.
 *
 * clangd needs no build server and no JVM, so this is fast (seconds, not the
 * minute-plus Metals cold start) and CI-friendly.  SKIPs (exit 77) when clangd
 * is not installed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../we_lsp.h"

/* A tiny C translation unit.  Line numbers are 0-based (as LSP positions are):
 *   2  definition of add()
 *   7  a call to add(); `total` is an int
 *   9  an undeclared symbol -> guarantees clangd publishes a diagnostic */
static const char *C_SRC =
 "#include <stdio.h>\n"                 /* line 0 */
 "\n"                                   /* line 1 */
 "int add(int a, int b) {\n"            /* line 2: definition of add        */
 "    return a + b;\n"                  /* line 3                            */
 "}\n"                                  /* line 4                            */
 "\n"                                   /* line 5                            */
 "int main(void) {\n"                   /* line 6                            */
 "    int total = add(2, 3);\n"         /* line 7: call add (->2); total:int */
 "    printf(\"%d\\n\", total);\n"      /* line 8                            */
 "    return bad_symbol;\n"             /* line 9: undeclared -> diagnostic  */
 "}\n";                                 /* line 10                           */

static int g_diags = 0;
static void on_diag(const char *path, int line, int ch, int end_line, int end_ch,
                    int sev, const char *msg, void *ud)
{
 (void)path; (void)line; (void)ch; (void)end_line; (void)end_ch;
 (void)sev; (void)msg; (void)ud;
 g_diags++;
}

static int have_clangd(void)
{
 return system("sh -c 'command -v clangd >/dev/null 2>&1'") == 0;
}

static int fail(const char *m) { printf("FAIL: %s\n", m); return 1; }

int main(void)
{
 char dir[] = "/tmp/xwpe-lspclangd-XXXXXX";
 char cpath[300];
 char *argv[] = { "clangd", NULL };
 e_lsp_host host = {0};        /* zero ALL callbacks (the engine NULL-checks each) */
 e_lsp_session *s;
 char *hov;
 char defpath[1024];
 int defline = -1, defchar = -1, rc = 0, n;
 e_lsp_completion_item items[64];
 FILE *f;

 if (!have_clangd())
 {
  printf("SKIP: clangd is required for the LSP C test (apt install clangd)\n");
  return 77;
 }
 if (!mkdtemp(dir))
  return fail("mkdtemp");
 snprintf(cpath, sizeof(cpath), "%s/add.c", dir);
 if (!(f = fopen(cpath, "w"))) { rc = fail("write add.c"); goto out; }
 fputs(C_SRC, f); fclose(f);

 host.on_diagnostic = on_diag;
 host.ud = NULL;
 s = e_lsp_open(argv, dir, "c", &host);
 if (!s) { rc = fail("e_lsp_open (clangd initialize handshake)"); goto out; }

 if (e_lsp_did_open(s, cpath, C_SRC) != 0) { rc = fail("did_open"); goto close; }

 /* clangd compiles on didOpen and publishes diagnostics; the bad_symbol on
    line 9 guarantees a non-empty set. */
 if (!e_lsp_wait_diagnostics(s, cpath, 30000))
 { rc = fail("no diagnostics (clangd never analysed the file)"); goto close; }
 printf("  analysed (%d diagnostic(s) delivered)\n", g_diags);

 /* hover on `total` in `int total = add(2, 3)` (line 7, char 8) -> type int.
    Retry briefly while the AST settles. */
 hov = NULL;
 { int t; for (t = 0; t < 8 && !hov; t++)
   { hov = e_lsp_hover(s, cpath, 7, 8); if (!hov) sleep(1); } }
 if (!hov) { rc = fail("hover on total returned nothing"); goto close; }
 printf("  HOVER total: %s\n", hov);
 if (!strstr(hov, "int")) { free(hov); rc = fail("hover did not mention int"); goto close; }
 free(hov);

 /* definition of the add() call (line 7, char 16) -> its definition on line 2 */
 if (e_lsp_definition(s, cpath, 7, 16, defpath, sizeof(defpath), &defline, &defchar) != 0)
 { rc = fail("definition of add returned nothing"); goto close; }
 printf("  DEFINITION add -> line %d\n", defline);
 if (defline != 2) { rc = fail("definition of add should point to line 2"); goto close; }

 /* completion after `add` members context (line 8, char 4) -> non-empty */
 n = e_lsp_completion(s, cpath, 8, 4, items, 64);
 printf("  COMPLETION: %d items%s%s\n", n, n > 0 ? ", e.g. " : "",
        n > 0 ? items[0].label : "");
 if (n <= 0) { rc = fail("completion returned no items"); goto close; }

 /* references on the add() call (line 7, char 16) -> definition + the call */
 {
  e_lsp_location locs[64];
  int nr = e_lsp_references(s, cpath, 7, 16, locs, 64);
  printf("  REFERENCES add: %d\n", nr);
  if (nr < 2) { rc = fail("references on add should find the def + call"); goto close; }
 }

 /* document outline -> at least add and main */
 {
  e_lsp_symbol syms[64];
  int ns = e_lsp_document_symbols(s, cpath, syms, 64);
  printf("  OUTLINE: %d symbols%s%s\n", ns, ns > 0 ? ", e.g. " : "",
         ns > 0 ? syms[0].name : "");
  if (ns < 2) { rc = fail("document outline should list add + main"); goto close; }
 }

 /* rename `add` (definition line 2, char 4) -> "plus": the edit-application path */
 {
  int others = 0;
  char *rn = e_lsp_rename(s, cpath, 2, 4, "plus", C_SRC, &others);
  if (!rn) { rc = fail("rename returned nothing"); goto close; }
  printf("  RENAME add->plus (other files: %d)\n", others);
  if (!strstr(rn, "int plus(int a, int b)") || !strstr(rn, "plus(2, 3)"))
  { printf("  ->\n%s\n", rn); free(rn);
    rc = fail("rename did not replace the def + call"); goto close; }
  if (strstr(rn, "add(2, 3)")) { free(rn); rc = fail("rename left an old add"); goto close; }
  free(rn);
 }

 /* inlay hints over the whole file -> clangd annotates the add(2, 3) call on
    line 7 with parameter names ("a:", "b:").  This is the request/parse path the
    Alt-Q Y overlay drives; a non-empty result here is what makes the overlay
    fill (instantly when warm, or via the async re-pull once a cold server has
    indexed).  Retry briefly while the AST settles, exactly like hover. */
 {
  e_lsp_inlay_hint hints[64];
  int ni = 0, t, on7 = 0, i;
  for (t = 0; t < 8 && ni <= 0; t++)
  { ni = e_lsp_inlay_hints(s, cpath, 0, 10, hints, 64); if (ni <= 0) sleep(1); }
  printf("  INLAY: %d hint(s)%s%s\n", ni, ni > 0 ? ", e.g. " : "",
         ni > 0 ? hints[0].label : "");
  if (ni <= 0) { rc = fail("inlay hints returned nothing for add(2, 3)"); goto close; }
  for (i = 0; i < ni; i++) if (hints[i].line == 7) on7 = 1;
  if (!on7) { rc = fail("expected a parameter-name hint on the add() call (line 7)"); goto close; }
 }

 printf("PASS: clangd drives the same LSP engine as Metals\n");

close:
 e_lsp_close(s);
out:
 { char rm[400]; snprintf(rm, sizeof(rm), "rm -rf '%s'", dir); if (system(rm)) {} }
 return rc;
}
