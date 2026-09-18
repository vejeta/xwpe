/* Unit test: the AI line diff (we_ai_diff.c).  No editor, no network. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "we_ai.h"

static int fail;
static void check(int cond, const char *msg)
{
 if (!cond) { printf("FAIL: %s\n", msg); fail = 1; }
 else printf("ok: %s\n", msg);
}

int main(void)
{
 char *d;

 /* identical texts -> only context (space) lines, no +/- */
 d = wpe_ai_diff("a\nb\nc\n", "a\nb\nc\n");
 check(d != NULL, "identical: non-null");
 check(d && !strchr(d, '+') && !strchr(d, '-'), "identical: no +/- lines");
 free(d);

 /* a changed middle line -> one '-' and one '+', context kept */
 d = wpe_ai_diff("a\nb\nc\n", "a\nB\nc\n");
 check(d && strstr(d, "-b") != NULL, "middle: removes old line");
 check(d && strstr(d, "+B") != NULL, "middle: adds new line");
 check(d && strstr(d, " a") != NULL && strstr(d, " c") != NULL, "middle: keeps context");
 free(d);

 /* pure addition */
 d = wpe_ai_diff("a\n", "a\nb\n");
 check(d && strstr(d, "+b") != NULL, "addition: adds line");
 free(d);

 /* pure deletion */
 d = wpe_ai_diff("a\nb\n", "a\n");
 check(d && strstr(d, "-b") != NULL, "deletion: removes line");
 free(d);

 /* --- structured segments (per-hunk accept) --- */
 {
  wpe_ai_seg *segs;
  int n, i, changes = 0;
  n = wpe_ai_diff_segments("a\nb\nc\n", "a\nB\nc\n", &segs);
  for (i = 0; i < n; i++) if (segs[i].is_change) changes++;
  check(changes == 1, "segments: exactly one change hunk");
  for (i = 0; i < n; i++)
   if (segs[i].is_change) {
    check(segs[i].an == 1 && !strcmp(segs[i].a[0], "b"), "segments: old line b");
    check(segs[i].bn == 1 && !strcmp(segs[i].b[0], "B"), "segments: new line B");
   }
  wpe_ai_segs_free(segs, n);
 }
 {
  wpe_ai_seg *segs;
  int n, i, changes = 0;
  n = wpe_ai_diff_segments("a\nb\n", "a\nb\n", &segs);
  for (i = 0; i < n; i++) if (segs[i].is_change) changes++;
  check(changes == 0, "segments: identical has no change hunk");
  wpe_ai_segs_free(segs, n);
 }

 printf(fail ? "SOME TESTS FAILED\n" : "ALL OK\n");
 return fail;
}
