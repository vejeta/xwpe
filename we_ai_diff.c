/* we_ai_diff.c - minimal line diff (LCS) for the AI Edit preview: renders the
 * change from text a -> text b as a unified list of ' '/'-'/'+' lines.
 * Editor-free and self-contained (unit-testable).
 * Compiled only under --enable-ai (WPE_AI). */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef WPE_AI

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "we_ai.h"

#define AI_DIFF_MAX_LINES 4000   /* LCS is O(n*m); fall back beyond this        */

/* How two lines are compared: byte for byte, or as a window's line against a
 * model's reply. */
typedef int (*ai_line_eq)(const char *a, const char *b);

/* Are two lines the same bytes?  For two texts that will be written as they
 * are (a file on disk and what a tool writes over it), so a change of line
 * ending is a change the user sees. */
static int ai_lines_equal(const char *a, const char *b)
{
 return strcmp(a, b) == 0;
}

/* Are two lines the same line, a window's (a) and a model's reply (b)?  They
 * are when they differ at most by a carriage return at the end of either.
 * The text of a window keeps the carriage return of a line that had one (it
 * is text there), and a reply is cleaned of the CR of its CR LF breaks, so
 * the line the reply leaves alone must not show as a change; the unchanged
 * part of a segment is taken from the window's side, so it keeps its
 * carriage return. */
static int ai_lines_same(const char *a, const char *b)
{
 while (*a && *a == *b) { a++; b++; }
 if (*a == *b) return 1;
 return (*a == '\r' && a[1] == '\0' && *b == '\0') ||
        (*b == '\r' && b[1] == '\0' && *a == '\0');
}

/* Split text into a vector of line strings (without newline).  Returns count,
 * *lines = malloc'd array of malloc'd strings (caller frees).  Shared with the
 * editor side, which lays a completion out as one hunk from the text around
 * the cursor without running the diff. */
int wpe_ai_split_lines(const char *t, char ***lines_out)
{
 int cap = 64, n = 0;
 char **lines = malloc(cap * sizeof *lines);
 const char *p = t;
 if (!lines) { *lines_out = NULL; return 0; }
 if (!t) t = "", p = t;
 while (*p) {
  const char *nl = strchr(p, '\n');
  size_t len = nl ? (size_t)(nl - p) : strlen(p);
  char *ln;
  if (n == cap) {
   char **nb;
   cap *= 2;
   nb = realloc(lines, cap * sizeof *lines);
   if (!nb) break;
   lines = nb;
  }
  ln = malloc(len + 1);
  if (!ln) break;
  memcpy(ln, p, len);
  ln[len] = '\0';
  lines[n++] = ln;
  if (!nl) break;
  p = nl + 1;
 }
 *lines_out = lines;
 return n;
}

void wpe_ai_free_lines(char **lines, int n)
{
 int i;
 for (i = 0; i < n; i++) free(lines[i]);
 free(lines);
}

static int ai_out(char **buf, size_t *len, size_t *cap, char pfx, const char *s)
{
 size_t sl = strlen(s), need = *len + sl + 3;
 if (need > *cap) {
  size_t nc = *cap ? *cap : 512;
  char *nb;
  while (need > nc) nc *= 2;
  nb = realloc(*buf, nc);
  if (!nb) return -1;
  *buf = nb; *cap = nc;
 }
 (*buf)[(*len)++] = pfx;
 memcpy(*buf + *len, s, sl);
 *len += sl;
 (*buf)[(*len)++] = '\n';
 (*buf)[*len] = '\0';
 return 0;
}

char *wpe_ai_diff(const char *a, const char *b)
{
 char **la, **lb;
 int na, nb, i, j;
 int *dp = NULL;
 char *out = NULL;
 size_t olen = 0, ocap = 0;

 na = wpe_ai_split_lines(a, &la);
 nb = wpe_ai_split_lines(b, &lb);

 /* Guard against pathological sizes: emit a compact summary instead of LCS. */
 if (na > AI_DIFF_MAX_LINES || nb > AI_DIFF_MAX_LINES) {
  char summary[128];
  snprintf(summary, sizeof summary,
           "(large change: %d lines -> %d lines; accept to apply)", na, nb);
  ai_out(&out, &olen, &ocap, ' ', summary);
  wpe_ai_free_lines(la, na);
  wpe_ai_free_lines(lb, nb);
  return out ? out : NULL;
 }

 /* LCS length table: dp[i][j] over (na+1)*(nb+1). */
 dp = calloc((size_t)(na + 1) * (nb + 1), sizeof *dp);
 if (!dp) { wpe_ai_free_lines(la, na); wpe_ai_free_lines(lb, nb); return NULL; }
#define DP(I, J) dp[(size_t)(I) * (nb + 1) + (J)]
 for (i = na - 1; i >= 0; i--)
  for (j = nb - 1; j >= 0; j--) {
   if (ai_lines_equal(la[i], lb[j])) DP(i, j) = DP(i + 1, j + 1) + 1;
   else DP(i, j) = DP(i + 1, j) >= DP(i, j + 1) ? DP(i + 1, j) : DP(i, j + 1);
  }

 i = j = 0;
 while (i < na && j < nb) {
  if (ai_lines_equal(la[i], lb[j])) { ai_out(&out, &olen, &ocap, ' ', la[i]); i++; j++; }
  else if (DP(i + 1, j) >= DP(i, j + 1)) { ai_out(&out, &olen, &ocap, '-', la[i]); i++; }
  else { ai_out(&out, &olen, &ocap, '+', lb[j]); j++; }
 }
 while (i < na) { ai_out(&out, &olen, &ocap, '-', la[i]); i++; }
 while (j < nb) { ai_out(&out, &olen, &ocap, '+', lb[j]); j++; }
#undef DP

 free(dp);
 wpe_ai_free_lines(la, na);
 wpe_ai_free_lines(lb, nb);
 if (!out) out = strdup("");    /* identical texts => empty diff */
 return out;
}

/* ----- structured segments (per-hunk accept/reject) --------------------- */

static void ai_seg_push(char ***arr, int *n, int *cap, const char *s)
{
 if (*n == *cap) {
  *cap = *cap ? *cap * 2 : 8;
  *arr = realloc(*arr, (size_t)(*cap) * sizeof **arr);
 }
 (*arr)[(*n)++] = strdup(s);
}

/* The segments of two texts, lines compared by @same. */
static int ai_diff_segments_by(const char *a, const char *b, ai_line_eq same,
                               wpe_ai_seg **out)
{
 char **la, **lb;
 int na, nb, i, j;
 int *dp;
 wpe_ai_seg *segs = NULL;
 int ns = 0, scap = 0;

 na = wpe_ai_split_lines(a, &la);
 nb = wpe_ai_split_lines(b, &lb);
 *out = NULL;

 /* Pathological size: one all-encompassing change hunk (skip the O(n*m) LCS). */
 if (na > AI_DIFF_MAX_LINES || nb > AI_DIFF_MAX_LINES) {
  wpe_ai_seg seg;
  int acap = 0, bcap = 0;
  memset(&seg, 0, sizeof seg);
  seg.is_change = 1;
  for (i = 0; i < na; i++) ai_seg_push(&seg.a, &seg.an, &acap, la[i]);
  for (j = 0; j < nb; j++) ai_seg_push(&seg.b, &seg.bn, &bcap, lb[j]);
  segs = malloc(sizeof *segs);
  if (segs) segs[ns++] = seg;
  wpe_ai_free_lines(la, na); wpe_ai_free_lines(lb, nb);
  *out = segs;
  return ns;
 }

 dp = calloc((size_t)(na + 1) * (nb + 1), sizeof *dp);
 if (!dp) { wpe_ai_free_lines(la, na); wpe_ai_free_lines(lb, nb); return 0; }
#define DP(I, J) dp[(size_t)(I) * (nb + 1) + (J)]
 for (i = na - 1; i >= 0; i--)
  for (j = nb - 1; j >= 0; j--) {
   if (same(la[i], lb[j])) DP(i, j) = DP(i + 1, j + 1) + 1;
   else DP(i, j) = DP(i + 1, j) >= DP(i, j + 1) ? DP(i + 1, j) : DP(i, j + 1);
  }

 i = j = 0;
 while (i < na || j < nb) {
  wpe_ai_seg seg;
  int acap = 0, bcap = 0;
  memset(&seg, 0, sizeof seg);
  if (i < na && j < nb && same(la[i], lb[j])) {
   seg.is_change = 0;
   while (i < na && j < nb && same(la[i], lb[j])) {
    ai_seg_push(&seg.a, &seg.an, &acap, la[i]); i++; j++;
   }
  } else {
   seg.is_change = 1;
   while ((i < na || j < nb) && !(i < na && j < nb && same(la[i], lb[j]))) {
    if (j >= nb || (i < na && DP(i + 1, j) >= DP(i, j + 1))) {
     ai_seg_push(&seg.a, &seg.an, &acap, la[i]); i++;
    } else {
     ai_seg_push(&seg.b, &seg.bn, &bcap, lb[j]); j++;
    }
   }
  }
  if (ns == scap) { scap = scap ? scap * 2 : 8; segs = realloc(segs, (size_t)scap * sizeof *segs); }
  segs[ns++] = seg;
 }
#undef DP
 free(dp);
 wpe_ai_free_lines(la, na);
 wpe_ai_free_lines(lb, nb);
 *out = segs;
 return ns;
}

/**
 * wpe_ai_diff_segments - The hunks between two texts, line by line, byte for
 * byte.
 *
 * For a file on disk and what a tool writes over it: a line whose carriage
 * return the tool adds or takes away is a change, so the user sees it in the
 * box before allowing the write.
 */
int wpe_ai_diff_segments(const char *a, const char *b, wpe_ai_seg **out)
{
 return ai_diff_segments_by(a, b, ai_lines_equal, out);
}

/**
 * wpe_ai_diff_reply_segments - The hunks between a window's text and a
 * model's reply for it.
 *
 * A line that differs only by a carriage return at its end is the same line,
 * and the unchanged part is the window's, so accepting an edit does not
 * change a line the model left alone.  The cost: an Edit cannot remove such
 * a stray carriage return.
 */
int wpe_ai_diff_reply_segments(const char *window, const char *reply, wpe_ai_seg **out)
{
 return ai_diff_segments_by(window, reply, ai_lines_same, out);
}

void wpe_ai_segs_free(wpe_ai_seg *segs, int n)
{
 int i, k;
 if (!segs) return;
 for (i = 0; i < n; i++) {
  for (k = 0; k < segs[i].an; k++) free(segs[i].a[k]);
  free(segs[i].a);
  for (k = 0; k < segs[i].bn; k++) free(segs[i].b[k]);
  free(segs[i].b);
 }
 free(segs);
}

#endif /* WPE_AI */

typedef int wpe_ai_diff_translation_unit;
