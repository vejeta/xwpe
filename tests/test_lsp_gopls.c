/* test_lsp_gopls.c -- integration test for the LSP engine (we_lsp.c) against a
 * REAL gopls language server, on a single Go file in a tiny module.
 *
 * Same engine as Metals/clangd/pyright -- only the spawned command ("gopls")
 * and the languageId ("go") differ.  gopls wants a module to give full results,
 * so the test runs `go mod init` in its tmp dir before opening the file.
 * Sequence: open -> diagnostics (gopls is a compiler; the undefined name is a
 * hard error) -> definition -> completion -> references; hover/rename best-effort.
 *
 * SKIPs (exit 77) when gopls or go is not on PATH. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../we_lsp.h"

/* Go indents with TABS (\t).  add() is defined on line 4 and called on line 9;
 * `undefinedName` on line 11 is undefined -> gopls reports a hard error. */
static const char *GO_SRC =
 "package main\n"               /* line 0  */
 "\n"                           /* line 1  */
 "import \"fmt\"\n"             /* line 2  */
 "\n"                           /* line 3  */
 "func add(a, b int) int {\n"   /* line 4: definition of add (add at col 5) */
 "\treturn a + b\n"             /* line 5  */
 "}\n"                          /* line 6  */
 "\n"                           /* line 7  */
 "func main() {\n"              /* line 8  */
 "\ttotal := add(2, 3)\n"       /* line 9: call add (col 10); total          */
 "\tfmt.Println(total)\n"       /* line 10: fmt. completion at col 5         */
 "\t_ = undefinedName\n"        /* line 11: undefined -> diagnostic          */
 "}\n";                         /* line 12 */

static int g_diags = 0;
static void on_diag(const char *path, int line, int ch, int end_line, int end_ch,
                    int sev, const char *msg, void *ud)
{
 (void)path; (void)line; (void)ch; (void)end_line; (void)end_ch;
 (void)sev; (void)msg; (void)ud;
 g_diags++;
}

static int have_tools(void)
{
 return system("sh -c 'command -v gopls >/dev/null 2>&1 && "
               "command -v go >/dev/null 2>&1'") == 0;
}

static int fail(const char *m) { printf("FAIL: %s\n", m); return 1; }

int main(void)
{
 char dir[] = "/tmp/xwpe-lspgo-XXXXXX";
 char gopath[300], cmd[512];
 char *const argv[] = { "gopls", NULL };
 e_lsp_host host = {0};
 e_lsp_session *s;
 char *hov;
 char defpath[1024];
 int defline = -1, defchar = -1, rc = 0, n;
 e_lsp_completion_item items[64];
 FILE *f;

 if (!have_tools())
 {  printf("SKIP: gopls and go are required (apt install gopls golang-go)\n");
    return 77;  }
 if (!mkdtemp(dir))
  return fail("mkdtemp");
 snprintf(gopath, sizeof(gopath), "%s/main.go", dir);
 if (!(f = fopen(gopath, "w"))) { rc = fail("write main.go"); goto out; }
 fputs(GO_SRC, f); fclose(f);

 /* gopls needs a module: without go.mod it runs in a degraded ad-hoc mode. */
 snprintf(cmd, sizeof(cmd),
          "cd '%s' && go mod init demo >/dev/null 2>&1", dir);
 if (system(cmd) != 0) { rc = fail("go mod init"); goto out; }

 host.on_diagnostic = on_diag;
 host.ud = NULL;
 s = e_lsp_open(argv, dir, "go", &host);
 if (!s) { rc = fail("e_lsp_open (gopls initialize handshake)"); goto out; }

 if (e_lsp_did_open(s, gopath, GO_SRC) != 0) { rc = fail("did_open"); goto close; }

 /* gopls loads the module then type-checks; the undefined name is a hard error.
    The first publish may be empty (async load), so pump until it arrives. */
 { int t; for (t = 0; t < 6 && g_diags == 0; t++)
     e_lsp_wait_diagnostics(s, gopath, 10000); }
 if (g_diags == 0)
 { rc = fail("no diagnostics (the undefined name was not flagged)"); goto close; }
 printf("  analysed (%d diagnostic(s) delivered)\n", g_diags);

 /* definition of the add() call (line 9, col 10) -> its def on line 4 */
 if (e_lsp_definition(s, gopath, 9, 10, defpath, sizeof(defpath), &defline, &defchar) != 0)
 { rc = fail("definition of add returned nothing"); goto close; }
 printf("  DEFINITION add -> line %d\n", defline);
 if (defline != 4) { rc = fail("definition of add should point to line 4"); goto close; }

 /* completion right after `fmt.` (line 10, col 5) -> package members */
 n = e_lsp_completion(s, gopath, 10, 5, items, 64);
 printf("  COMPLETION: %d items%s%s\n", n, n > 0 ? ", e.g. " : "",
        n > 0 ? items[0].label : "");
 if (n <= 0) { rc = fail("completion after fmt. returned no items"); goto close; }

 /* references on the add() call (line 9, col 10) -> definition + the call */
 {
  e_lsp_location locs[64];
  int nr = e_lsp_references(s, gopath, 9, 10, locs, 64);
  printf("  REFERENCES add: %d\n", nr);
  if (nr < 2) { rc = fail("references on add should find the def + call"); goto close; }
 }

 /* hover + rename: report best-effort (gopls provides both, but keep the hard
    assertions to the cross-server common core). */
 hov = e_lsp_hover(s, gopath, 9, 10);
 if (hov) { printf("  HOVER add: %.60s\n", hov); free(hov); }
 else printf("  HOVER add: (none -- not fatal)\n");
 {
  int others = 0;
  char *rn = e_lsp_rename(s, gopath, 4, 5, "plus", GO_SRC, &others);
  if (rn) { printf("  RENAME add->plus ok (other files: %d)\n", others); free(rn); }
  else printf("  RENAME: (none -- not fatal)\n");
 }

 printf("PASS: gopls drives the same LSP engine as Metals/clangd/pyright\n");

close:
 e_lsp_close(s);
out:
 { char rm[400]; snprintf(rm, sizeof(rm), "rm -rf '%s'", dir); if (system(rm)) {} }
 return rc;
}
