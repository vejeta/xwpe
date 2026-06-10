/* test_lsp_doctor.c -- unit test for e_lsp_doc_is_new (we_lsp.c): the dedup gate
 * for a server-pushed display document (the Metals Doctor).  Metals re-PUSHES
 * the same Doctor on every build/index event, so the editor must render it only
 * when it actually changed -- otherwise the report flickers and the "Doctor
 * updated" note repeats in Messages.  The gate is also reset per language-server
 * session (the caller NULLs *last on each fresh start), so a re-started Metals
 * shows its Doctor again rather than being deduped away forever -- the
 * "only appears the first time LSP starts" bug.  Pure, no server, no skip. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../we_lsp.h"

static int fails;

static void check(const char *name, int got, int want)
{
 if (got == want)
  printf("ok   - %s\n", name);
 else
 {
  printf("FAIL - %s (want %d, got %d)\n", name, want, got);
  fails++;
 }
}

int main(void)
{
 char *last = NULL;                       /* the caller's per-session store */

 /* 1. first push of a session: new -> render, and *last adopts the body. */
 check("first push is new", e_lsp_doc_is_new(&last, "Doctor v1"), 1);
 check("first push adopts body", last && !strcmp(last, "Doctor v1"), 1);

 /* 2. THE DEDUP: Metals re-pushes the identical Doctor -> ignore. */
 check("identical re-push is deduped", e_lsp_doc_is_new(&last, "Doctor v1"), 0);
 check("identical re-push keeps body", last && !strcmp(last, "Doctor v1"), 1);

 /* 3. content actually changed (a new build problem) -> render again. */
 check("changed body is new", e_lsp_doc_is_new(&last, "Doctor v2"), 1);
 check("changed body adopts", last && !strcmp(last, "Doctor v2"), 1);

 /* 4. NULL body never renders and never clobbers the stored copy. */
 check("NULL body is not new", e_lsp_doc_is_new(&last, NULL), 0);
 check("NULL body keeps last", last && !strcmp(last, "Doctor v2"), 1);

 /* 5. THE "first time only" BUG: a fresh session resets *last to NULL; the same
       Doctor body that a previous session showed must render again. */
 free(last);
 last = NULL;                             /* what e_lsp_ensure does per session */
 check("re-started session shows same Doctor again",
       e_lsp_doc_is_new(&last, "Doctor v2"), 1);

 free(last);
 if (fails)
 {  printf("%d failure(s)\n", fails);  return(1);  }
 printf("all doctor-dedup checks passed\n");
 return(0);
}
