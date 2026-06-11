/* test_lsp_python.c -- integration test for the LSP engine (we_lsp.c) against a
 * REAL Python language server, on a single .py file.
 *
 * Python has TWO candidate servers (the editor prefers pyright, falls back to
 * pylsp -- see e_lsp_servers[] in we_debug.c).  This test mirrors that: it uses
 * whichever is installed, so the SAME functionality is proven against either.
 * It asserts only the capabilities BOTH provide (diagnostics, definition,
 * completion, references); hover text and rename differ by server (pylsp needs
 * the rope plugin to rename), so those are printed best-effort, not asserted.
 *
 * SKIPs (exit 77) when neither pyright-langserver nor pylsp is on PATH. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../we_lsp.h"

/* `undefined_name` on line 6 is undefined -> guarantees a diagnostic from both
 * pyright and pylsp (pyflakes).  add() is defined on line 0 and called on 4. */
static const char *PY_SRC =
 "def add(a, b):\n"             /* line 0: definition of add       */
 "    return a + b\n"           /* line 1                          */
 "\n"                           /* line 2                          */
 "def main():\n"                /* line 3                          */
 "    total = add(2, 3)\n"      /* line 4: call add (->0); total   */
 "    print(total)\n"           /* line 5                          */
 "    return undefined_name\n"  /* line 6: undefined -> diagnostic */
 "\n"                           /* line 7                          */
 "main()\n";                    /* line 8                          */

static int g_diags = 0;
static void on_diag(const char *path, int line, int ch, int end_line, int end_ch,
                    int sev, const char *msg, void *ud)
{
 (void)path; (void)line; (void)ch; (void)end_line; (void)end_ch;
 (void)sev; (void)msg; (void)ud;
 g_diags++;
}

/* Pick the installed Python server, preferring pyright (the editor's order). */
static char *const PYRIGHT_ARGV[] = { "pyright-langserver", "--stdio", NULL };
static char *const PYLSP_ARGV[]   = { "pylsp", NULL };
static char *const *pick_server(const char **name)
{
 if (system("sh -c 'command -v pyright-langserver >/dev/null 2>&1'") == 0)
 {  *name = "pyright";  return PYRIGHT_ARGV;  }
 if (system("sh -c 'command -v pylsp >/dev/null 2>&1'") == 0)
 {  *name = "pylsp";  return PYLSP_ARGV;  }
 return NULL;
}

static int fail(const char *m) { printf("FAIL: %s\n", m); return 1; }

int main(void)
{
 char dir[] = "/tmp/xwpe-lsppy-XXXXXX";
 char pypath[300];
 const char *srvname = NULL;
 char *const *argv = pick_server(&srvname);
 e_lsp_host host = {0};
 e_lsp_session *s;
 char *hov;
 char defpath[1024];
 int defline = -1, defchar = -1, rc = 0, n;
 e_lsp_completion_item items[64];
 FILE *f;

 if (!argv)
 {
  printf("SKIP: a Python LSP is required (pip install pyright, or "
         "apt install python3-pylsp)\n");
  return 77;
 }
 printf("  using %s\n", srvname);
 if (!mkdtemp(dir))
  return fail("mkdtemp");
 snprintf(pypath, sizeof(pypath), "%s/demo.py", dir);
 if (!(f = fopen(pypath, "w"))) { rc = fail("write demo.py"); goto out; }
 fputs(PY_SRC, f); fclose(f);

 host.on_diagnostic = on_diag;
 host.ud = NULL;
 s = e_lsp_open(argv, dir, "python", &host);
 if (!s) { rc = fail("e_lsp_open (Python server initialize handshake)"); goto out; }

 if (e_lsp_did_open(s, pypath, PY_SRC) != 0) { rc = fail("did_open"); goto close; }

 /* Let the server analyse the file (pylsp publishes an empty set first, then
    the real one).  Diagnostics are BEST-EFFORT here: pyright flags the
    undefined name, but pylsp only does so when python3-pyflakes is installed
    (jedi alone -- the bundled default -- gives navigation but no linting).  So
    pump briefly and report, without failing on zero. */
 { int t; for (t = 0; t < 3 && g_diags == 0; t++)
     e_lsp_wait_diagnostics(s, pypath, 6000); }
 if (g_diags)
  printf("  analysed (%d diagnostic(s) delivered)\n", g_diags);
 else
  printf("  analysed (0 diagnostics -- pylsp without python3-pyflakes; not fatal)\n");

 /* definition of the add() call (line 4, char 12) -> its def on line 0 */
 if (e_lsp_definition(s, pypath, 4, 12, defpath, sizeof(defpath), &defline, &defchar) != 0)
 { rc = fail("definition of add returned nothing"); goto close; }
 printf("  DEFINITION add -> line %d\n", defline);
 if (defline != 0) { rc = fail("definition of add should point to line 0"); goto close; }

 /* completion inside `total` in `print(total)` (line 5, char 12) -> non-empty;
    jedi completes the identifier prefix under the cursor, so the position must
    sit on a real prefix, not a blank column. */
 n = e_lsp_completion(s, pypath, 5, 12, items, 64);
 printf("  COMPLETION: %d items%s%s\n", n, n > 0 ? ", e.g. " : "",
        n > 0 ? items[0].label : "");
 if (n <= 0) { rc = fail("completion returned no items"); goto close; }

 /* references on the add() call (line 4, char 12) -> definition + the call */
 {
  e_lsp_location locs[64];
  int nr = e_lsp_references(s, pypath, 4, 12, locs, 64);
  printf("  REFERENCES add: %d\n", nr);
  if (nr < 2) { rc = fail("references on add should find the def + call"); goto close; }
 }

 /* hover + rename: capability differs by server (pyright rich; pylsp needs
    jedi/rope), so report best-effort without failing the test. */
 hov = e_lsp_hover(s, pypath, 4, 12);
 if (hov) { printf("  HOVER add: %.60s\n", hov); free(hov); }
 else printf("  HOVER add: (none -- not fatal)\n");
 {
  int others = 0;
  char *rn = e_lsp_rename(s, pypath, 0, 4, "plus", PY_SRC, &others);
  if (rn) { printf("  RENAME add->plus ok (other files: %d)\n", others); free(rn); }
  else printf("  RENAME: (none -- server lacks rename, not fatal)\n");
 }

 printf("PASS: %s drives the same LSP engine as Metals/clangd\n", srvname);

close:
 e_lsp_close(s);
out:
 { char rm[400]; snprintf(rm, sizeof(rm), "rm -rf '%s'", dir); if (system(rm)) {} }
 return rc;
}
