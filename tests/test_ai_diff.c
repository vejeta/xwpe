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

 /* @scenario SC-line-endings-28
  * A line of the window that ends in a carriage return (it is text there)
  * is the same line as the reply's, which was cleaned of CR LF: only the
  * line the reply changed is a hunk, and the unchanged part is taken from
  * the window's side, carriage return and all. */
 {
  wpe_ai_seg *segs;
  int n, i, changes = 0;
  n = wpe_ai_diff_reply_segments("a\r\nb\nc\r\n", "a\nB\nc\n", &segs);
  for (i = 0; i < n; i++) if (segs[i].is_change) changes++;
  check(changes == 1, "carriage return: exactly one change hunk");
  check(n >= 1 && !segs[0].is_change && !strcmp(segs[0].a[0], "a\r"),
        "carriage return: the unchanged line keeps the window's carriage return");
  check(n == 3 && !segs[2].is_change && !strcmp(segs[2].a[0], "c\r"),
        "carriage return: so does the one after the change");
  wpe_ai_segs_free(segs, n);
  n = wpe_ai_diff_reply_segments("a\nb\n", "a\r\nb\r\n", &segs);
  for (i = 0, changes = 0; i < n; i++) if (segs[i].is_change) changes++;
  check(changes == 0, "carriage return: a reply with extra carriage returns changes nothing");
  wpe_ai_segs_free(segs, n);
  n = wpe_ai_diff_reply_segments("a\rb\n", "ab\n", &segs);
  for (i = 0, changes = 0; i < n; i++) if (segs[i].is_change) changes++;
  check(changes == 1, "carriage return: one inside a line is a real difference");
  wpe_ai_segs_free(segs, n);
 }

 /* @scenario SC-line-endings-33
  * Two texts that will be written as they are (a file and what a tool writes
  * over it) are compared byte for byte: a tool that takes the carriage
  * returns away shows its change. */
 {
  wpe_ai_seg *segs;
  char *d;
  int n, i, changes = 0;
  n = wpe_ai_diff_segments("a\r\nb\r\n", "a\nb\n", &segs);
  for (i = 0; i < n; i++) if (segs[i].is_change) changes++;
  check(changes == 1, "strict: a CRLF to LF rewrite is a change hunk");
  wpe_ai_segs_free(segs, n);
  d = wpe_ai_diff("a\r\nb\r\n", "a\nb\n");
  check(d && strchr(d, '-') != NULL && strchr(d, '+') != NULL,
        "strict: and the line diff shows it");
  free(d);
  n = wpe_ai_diff_segments("a\r\n", "a\r\n", &segs);
  check(n == 1 && !segs[0].is_change, "strict: identical text is one unchanged hunk");
  wpe_ai_segs_free(segs, n);
 }

 /* @scenario SC-ai-edit-34
  * Either side past the size guard (AI_DIFF_MAX_LINES = 4000, we_ai_diff.c:17)
  * falls back to one all-encompassing hunk instead of running the line diff
  * (we_ai_diff.c:91-99, 151-164).  Built with IDENTICAL text on both sides so
  * the two code paths disagree on the answer: a real line diff would find no
  * change at all, while the fallback always proposes the whole text as one
  * hunk regardless of content.  Seeing exactly one change hunk here can only
  * come from the fallback firing. */
 {
  enum { BIG_LINES = 4001 };   /* one past the guard */
  char *big = NULL;
  size_t cap = 0, len = 0;
  int k;
  char *d;
  wpe_ai_seg *segs;
  int n, i, changes = 0;

  for (k = 0; k < BIG_LINES; k++) {
   char line[32];
   size_t ll;
   snprintf(line, sizeof line, "line%d\n", k);
   ll = strlen(line);
   if (len + ll + 1 > cap) { cap = cap ? cap * 2 : 4096; while (len + ll + 1 > cap) cap *= 2; big = realloc(big, cap); }
   memcpy(big + len, line, ll);
   len += ll;
  }
  big[len] = '\0';

  d = wpe_ai_diff(big, big);
  check(d != NULL, "huge identical text: diff non-null");
  check(d && strstr(d, "large change:") != NULL,
        "huge identical text: falls back to the compact summary, not a line diff");
  free(d);

  n = wpe_ai_diff_segments(big, big, &segs);
  for (i = 0; i < n; i++) if (segs[i].is_change) changes++;
  check(n == 1 && changes == 1,
        "huge identical text: exactly one change hunk (the fallback), not zero");
  if (n == 1) {
   check(segs[0].an == BIG_LINES && segs[0].bn == BIG_LINES,
         "huge identical text: the one hunk carries the whole text on both sides");
  }
  wpe_ai_segs_free(segs, n);
  free(big);
 }

 printf(fail ? "SOME TESTS FAILED\n" : "ALL OK\n");
 return fail;
}
