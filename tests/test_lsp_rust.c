/* test_lsp_rust.c -- integration test for the LSP engine (we_lsp.c) against a
 * REAL rust-analyzer language server, on a single Rust file in a tiny crate.
 *
 * Same engine as Metals/clangd/pyright/gopls -- only the spawned command
 * ("rust-analyzer") and the languageId ("rust") differ.  rust-analyzer works
 * best in a Cargo workspace, so the test writes a Cargo.toml + src/main.rs and
 * points the server at the crate root.
 *
 * rust-analyzer is heavy: it indexes the std library on first run, so this is
 * the slowest of the breadth tests.  It asserts the reliable local-symbol core
 * (definition, references) and reports completion/hover/rename/diagnostics
 * best-effort (their availability depends on how far the cold-start index and
 * the cargo-check flycheck have progressed).  SKIPs (exit 77) when
 * rust-analyzer or cargo is not on PATH. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../we_lsp.h"

/* add() is defined on line 0 and called on line 5. */
static const char *RUST_SRC =
 "fn add(a: i32, b: i32) -> i32 {\n"  /* line 0: definition of add (add at col 3) */
 "    a + b\n"                        /* line 1                                   */
 "}\n"                                /* line 2                                   */
 "\n"                                 /* line 3                                   */
 "fn main() {\n"                      /* line 4                                   */
 "    let total = add(2, 3);\n"       /* line 5: call add (col 16); total at 8    */
 "    println!(\"{}\", total);\n"     /* line 6                                   */
 "}\n";                               /* line 7                                   */

static const char *CARGO_TOML =
 "[package]\n"
 "name = \"demo\"\n"
 "version = \"0.1.0\"\n"
 "edition = \"2021\"\n";

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
 return system("sh -c 'command -v rust-analyzer >/dev/null 2>&1 && "
               "command -v cargo >/dev/null 2>&1'") == 0;
}

static int fail(const char *m) { printf("FAIL: %s\n", m); return 1; }

static int write_file(const char *path, const char *body)
{
 FILE *f = fopen(path, "w");
 if (!f) return -1;
 fputs(body, f);
 return fclose(f);
}

int main(void)
{
 char dir[] = "/tmp/xwpe-lsprust-XXXXXX";
 char toml[300], srcdir[300], rspath[320], cmd[400];
 char *const argv[] = { "rust-analyzer", NULL };
 e_lsp_host host = {0};
 e_lsp_session *s;
 char *hov;
 char defpath[1024];
 int defline = -1, defchar = -1, rc = 0, n, t;
 e_lsp_completion_item items[64];

 if (!have_tools())
 {  printf("SKIP: rust-analyzer and cargo are required "
           "(apt install rust-analyzer cargo)\n");
    return 77;  }
 if (!mkdtemp(dir))
  return fail("mkdtemp");
 snprintf(toml, sizeof(toml), "%s/Cargo.toml", dir);
 snprintf(srcdir, sizeof(srcdir), "%s/src", dir);
 snprintf(rspath, sizeof(rspath), "%s/src/main.rs", dir);
 snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", srcdir);
 if (system(cmd) != 0) { rc = fail("mkdir src"); goto out; }
 if (write_file(toml, CARGO_TOML) != 0) { rc = fail("write Cargo.toml"); goto out; }
 if (write_file(rspath, RUST_SRC) != 0) { rc = fail("write src/main.rs"); goto out; }

 host.on_diagnostic = on_diag;
 host.ud = NULL;
 s = e_lsp_open(argv, dir, "rust", &host);
 if (!s) { rc = fail("e_lsp_open (rust-analyzer initialize handshake)"); goto out; }

 if (e_lsp_did_open(s, rspath, RUST_SRC) != 0) { rc = fail("did_open"); goto close; }

 /* Let rust-analyzer load the crate and index std (slow cold start).  Pump for
    a while -- a diagnostics publish (even empty) signals the workspace loaded. */
 for (t = 0; t < 6; t++)
  if (e_lsp_wait_diagnostics(s, rspath, 15000) && g_diags)
   break;
 if (g_diags)
  printf("  analysed (%d diagnostic(s) delivered)\n", g_diags);
 else
  printf("  loaded (no diagnostics yet -- flycheck still running; not fatal)\n");

 /* definition of the add() call (line 5, col 16) -> its def on line 0.  Retry
    while the index settles. */
 {
  int ok = 0;
  for (t = 0; t < 8 && !ok; t++)
  {
   if (e_lsp_definition(s, rspath, 5, 16, defpath, sizeof(defpath),
                        &defline, &defchar) == 0 && defline == 0)
    ok = 1;
   else
    sleep(2);
  }
  if (!ok) { rc = fail("definition of add never resolved to line 0"); goto close; }
  printf("  DEFINITION add -> line %d\n", defline);
 }

 /* references on the add() call (line 5, col 16) -> definition + the call */
 {
  e_lsp_location locs[64];
  int nr = e_lsp_references(s, rspath, 5, 16, locs, 64);
  printf("  REFERENCES add: %d\n", nr);
  if (nr < 2) { rc = fail("references on add should find the def + call"); goto close; }
 }

 /* completion / hover / rename: report best-effort (availability depends on how
    far the cold-start index got). */
 n = e_lsp_completion(s, rspath, 5, 16, items, 64);
 printf("  COMPLETION: %d items%s%s\n", n, n > 0 ? ", e.g. " : "",
        n > 0 ? items[0].label : "");
 hov = e_lsp_hover(s, rspath, 5, 16);
 if (hov) { printf("  HOVER add: %.60s\n", hov); free(hov); }
 else printf("  HOVER add: (none -- not fatal)\n");
 {
  int others = 0;
  char *rn = e_lsp_rename(s, rspath, 0, 3, "plus", RUST_SRC, &others);
  if (rn) { printf("  RENAME add->plus ok (other files: %d)\n", others); free(rn); }
  else printf("  RENAME: (none -- not fatal)\n");
 }

 printf("PASS: rust-analyzer drives the same LSP engine as Metals/clangd/pyright/gopls\n");

close:
 e_lsp_close(s);
out:
 { char rm[400]; snprintf(rm, sizeof(rm), "rm -rf '%s'", dir); if (system(rm)) {} }
 return rc;
}
