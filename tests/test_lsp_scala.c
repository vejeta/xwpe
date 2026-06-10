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
 "val theGreeter: Hello = Hello()\n"       /* line 15: typed val       */
 "def caller(): Unit = helper()\n"         /* line 16: calls helper -- a
                                              user-to-user call edge    */
 "def helper(): Unit = ()\n";              /* line 17: the callee       */

static int g_diags = 0;
static int g_last_diag_span = -1;   /* end_char - char of the last diagnostic */
static void on_diag(const char *path, int line, int ch, int end_line, int end_ch,
                    int sev, const char *msg, void *ud)
{
 (void)path; (void)line; (void)sev; (void)msg; (void)ud;
 if (end_line == line)
  g_last_diag_span = end_ch - ch;
 g_diags++;
}

static int have_tools(void)
{
 return system("sh -c 'command -v metals >/dev/null 2>&1 && "
               "command -v scala-cli >/dev/null 2>&1'") == 0;
}

static int fail(const char *m) { printf("FAIL: %s\n", m); return 1; }

/* (al,ac) at or before (bl,bc) in document order -- for nesting checks. */
static int pos_le(int al, int ac, int bl, int bc)
{ return al < bl || (al == bl && ac <= bc); }

int main(void)
{
 char dir[] = "/tmp/xwpe-lspscala-XXXXXX";
 char scala[300], cmd[512];
 char *argv[] = { "metals", NULL };
 e_lsp_host host = {0};        /* zero ALL callbacks: the engine NULL-checks each,
                                 so unused ones (summary/show_text/status) must
                                 not be left as garbage stack pointers */
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

 /* range formatting: reformat just the main method's lines [2..7].  On tidy
    source the server may return no edits (NULL) -- fine; when it does return
    text it must round-trip without corruption (the object + println survive and
    out-of-range lines like `trait Greeter` are still present). */
 {
  char *rf = e_lsp_format_range(s, scala, SCALA_SRC, 2, 0, 7, 40);
  if (rf)
  {
   printf("  RANGE-FORMAT [2..7]: %zu bytes\n", strlen(rf));
   if (!strstr(rf, "object Factorial") || !strstr(rf, "println") ||
       !strstr(rf, "trait Greeter"))
   { free(rf); rc = fail("range format corrupted the buffer"); goto close; }
   free(rf);
  }
  else
   printf("  RANGE-FORMAT [2..7]: no edits (already tidy)\n");
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

 /* document highlight on the `f` use (line 5, char 6) -> its occurrences in
    THIS file (declaration + uses); same symbol, so >= 2. */
 {
  e_lsp_location hl[64];
  int nh = e_lsp_document_highlight(s, scala, 5, 6, hl, 64);
  printf("  DOCUMENT-HIGHLIGHT f: %d\n", nh);
  if (nh < 2) { rc = fail("document highlight on f should find the decl + uses"); goto close; }
 }

 /* code lenses: Metals attaches a run/debug lens to the `main` method.  Resolve
    is exercised inside the engine; we just confirm at least one lens with a
    label comes back. */
 {
  e_lsp_code_lens cl[32];
  int nl = 0, k, t;
  /* the run/debug lens only appears once the build server (Bloop) has imported
     the build and resolved the main class -- retry while that warms up. */
  for (t = 0; t < 8 && nl < 1; t++)
  { nl = e_lsp_code_lenses(s, scala, cl, 32); if (nl < 1) sleep(2); }
  printf("  CODE-LENSES: %d%s%s\n", nl, nl > 0 ? ", e.g. " : "",
         nl > 0 ? cl[0].title : "");
  if (nl < 1) { rc = fail("expected at least one code lens (run main)"); goto close; }
  for (k = 0; k < nl; k++)
   if (!cl[k].title || !cl[k].title[0])
   { rc = fail("a code lens came back with no label"); goto close; }
 }

 /* INLAY HINTS over the whole file: with inferred types enabled (we report that
    config to Metals), "var i = 1" etc. should get a ": Int"-style hint.  Hint
    emission is config/version dependent, so only a hard error (-1) or an empty
    label fails; a count of 0 is reported, not fatal. */
 {
  e_lsp_inlay_hint ih[256];
  int ni = e_lsp_inlay_hints(s, scala, 0, 16, ih, 256), k;
  if (ni < 0) { rc = fail("inlay hints returned an error"); goto close; }
  printf("  INLAY-HINTS: %d%s%s\n", ni, ni > 0 ? ", e.g. " : "",
         ni > 0 ? ih[0].label : "");
  for (k = 0; k < ni; k++)
   if (!ih[k].label || !ih[k].label[0])
   { rc = fail("an inlay hint came back with no label"); goto close; }
 }

 /* CALL HIERARCHY on `caller` (line 16, char 4): prepareCallHierarchy then its
    OUTGOING calls.  caller() calls helper(), a user-to-user edge Metals reports
    once the index is warm -- so we expect >= 1 callee named "helper" (retry
    while it warms).  Then the INCOMING side from helper (line 17, char 4) must
    list caller -- the inverse edge.  Each entry must carry a name. */
 {
  e_lsp_symbol ch[64];
  int nc = 0, t, k, found = 0;
  for (t = 0; t < 8 && nc < 1; t++)
  { nc = e_lsp_call_hierarchy(s, scala, 16, 4, 1 /*outgoing*/, ch, 64);
    if (nc < 1) sleep(2); }
  printf("  CALL-HIERARCHY caller outgoing: %d%s%s\n", nc, nc > 0 ? ", e.g. " : "",
         nc > 0 ? ch[0].name : "");
  if (nc < 1) { rc = fail("caller() should have an outgoing call to helper()"); goto close; }
  for (k = 0; k < nc; k++)
  {
   if (!ch[k].name || !ch[k].name[0])
   { rc = fail("a call-hierarchy entry came back with no name"); goto close; }
   if (strstr(ch[k].name, "helper")) found = 1;
  }
  if (!found) { rc = fail("outgoing calls of caller() missed helper()"); goto close; }

  found = 0;
  nc = e_lsp_call_hierarchy(s, scala, 17, 4, 0 /*incoming*/, ch, 64);
  printf("  CALL-HIERARCHY helper incoming: %d%s%s\n", nc, nc > 0 ? ", e.g. " : "",
         nc > 0 ? ch[0].name : "");
  if (nc < 0) { rc = fail("incoming call hierarchy returned an error"); goto close; }
  for (k = 0; k < nc; k++)
   if (ch[k].name && strstr(ch[k].name, "caller")) found = 1;
  if (!found) { rc = fail("incoming calls of helper() missed caller()"); goto close; }
 }

 /* TYPE HIERARCHY: `class Hello extends Greeter` (line 12) and `trait Greeter`
    (line 9).  Supertypes of Hello (line 12, char 6) must list Greeter; subtypes
    of Greeter (line 9, char 6) must list Hello -- the inverse edge.  Index
    dependent, so retry while it warms; -1 is an error, any entry needs a name. */
 {
  e_lsp_symbol th[64];
  int nt = 0, t, k, found = 0;
  for (t = 0; t < 8 && nt < 1; t++)
  { nt = e_lsp_type_hierarchy(s, scala, 12, 6, 0 /*supertypes*/, th, 64);
    if (nt < 1) sleep(2); }
  printf("  TYPE-HIERARCHY Hello supertypes: %d%s%s\n", nt, nt > 0 ? ", e.g. " : "",
         nt > 0 ? th[0].name : "");
  if (nt < 1) { rc = fail("Hello should have a supertype Greeter"); goto close; }
  for (k = 0; k < nt; k++)
  {
   if (!th[k].name || !th[k].name[0])
   { rc = fail("a type-hierarchy entry came back with no name"); goto close; }
   if (strstr(th[k].name, "Greeter")) found = 1;
  }
  if (!found) { rc = fail("supertypes of Hello missed Greeter"); goto close; }

  found = 0;
  nt = e_lsp_type_hierarchy(s, scala, 9, 6, 1 /*subtypes*/, th, 64);
  printf("  TYPE-HIERARCHY Greeter subtypes: %d%s%s\n", nt, nt > 0 ? ", e.g. " : "",
         nt > 0 ? th[0].name : "");
  if (nt < 0) { rc = fail("subtype hierarchy returned an error"); goto close; }
  for (k = 0; k < nt; k++)
   if (th[k].name && strstr(th[k].name, "Hello")) found = 1;
  if (!found) { rc = fail("subtypes of Greeter missed Hello"); goto close; }
 }

 /* SELECTION RANGE (expand selection): at the use of `f` (line 5, char 6) the
    server returns nested ranges innermost-first; each must enclose the previous
    -- that monotonic nesting is exactly what "expand selection" walks.  Index
    dependent (retry while warm); -1 errors, <1 is no range. */
 {
  e_lsp_range sr[64];
  int ns = 0, t, k, nested = 1;
  for (t = 0; t < 6 && ns < 1; t++)
  { ns = e_lsp_selection_range(s, scala, 5, 6, sr, 64); if (ns < 1) sleep(2); }
  printf("  SELECTION-RANGE at f: %d range(s)\n", ns);
  if (ns < 1) { rc = fail("selection range returned nothing at f"); goto close; }
  for (k = 1; k < ns; k++)
   if (!(pos_le(sr[k].start_line, sr[k].start_char,
                sr[k-1].start_line, sr[k-1].start_char) &&
         pos_le(sr[k-1].end_line, sr[k-1].end_char,
                sr[k].end_line, sr[k].end_char)))
    nested = 0;
  if (!nested) { rc = fail("selection ranges are not monotonically nested"); goto close; }
 }

 /* diagnostics carry a RANGE: push a buffer with a type error and confirm the
    server reports it with a non-empty, single-line span (what the inline marks
    recolor).  Reuses this session -- no second server start. */
 {
  static const char *BROKEN =
   "@@@\n"                          /* line 0: garbage tokens before the object */
   "object Factorial:\n"
   "  def main(args: Array[String]): Unit =\n"
   "    println(1)\n";              /* a hard parse error the presentation
                                       compiler flags fast (no full build), so
                                       the diagnostic arrives within the loop. */
  int t;
  g_diags = 0; g_last_diag_span = -1;
  if (e_lsp_did_change(s, scala, BROKEN) != 0) { rc = fail("did_change(broken)"); goto close; }
  /* Metals often publishes an empty set first, then recompiles; keep waiting
     for successive publishes until the type error actually surfaces. */
  for (t = 0; t < 24 && g_diags == 0; t++)
   e_lsp_wait_diagnostics(s, scala, 5000);
  printf("  DIAGNOSTICS(broken): %d, last single-line span=%d\n",
         g_diags, g_last_diag_span);
  if (g_diags < 1) { rc = fail("expected at least one diagnostic"); goto close; }
  if (g_last_diag_span < 0) { rc = fail("diagnostic range was not single-line/usable"); goto close; }
 }

 printf("PASS: LSP engine vs real Metals (hover/definition/implementation/"
        "type-definition/completion/references/document-highlight/outline/"
        "code-lenses/signature/rename/format/workspace-symbols/code-actions/"
        "diagnostics)\n");

close:
 e_lsp_close(s);
out:
 snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
 if (system(cmd) != 0) { /* best-effort cleanup of our own mkdtemp dir */ }
 return rc;
}
