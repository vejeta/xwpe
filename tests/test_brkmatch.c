/* test_brkmatch.c -- golden-text unit for the bracket matcher
 * (we_brkmatch.c).  Always runs; run it under ASan+UBSan too.
 *
 * What the user sees behind each group: "Matching BracKet" (Run menu) sends
 * the cursor from a brace, parenthesis or bracket to its partner, and from
 * any other character to the enclosing opener.  It must step over quoted
 * text, comments, a // tail and the unused side of an #else exactly as
 * before, or the cursor lands on the wrong bracket or reports none.  Each
 * case below freezes where the matcher stops for one kind of text.
 *
 * A case is the text (rows separated by '|'), the cursor, the byte that
 * ends each row in memory (the editor keeps the line break there; one case
 * puts a backslash there to reach the continued-quote rule that looks at
 * it), and the frozen answer: "ret=0 at=<row>,<column>" or "ret=-1".
 *
 * The rows are built the way the editor's row owner builds them: len+2
 * bytes after s[0] (the text, the line break, a NUL) and nothing before
 * s[0], in an array of exactly as many rows as the text has.  The matcher
 * may read s[0..len+1] and rows 0..n-1 and nothing else, so under ASan every
 * read outside that is a report.
 *
 * Every case runs twice.  The second run is a hostile frame that makes a
 * read outside the text change the answer in a plain build too: the bytes
 * before each s[0] alternate '*' and '/' (s[-1] is '*'), the bytes past
 * each NUL are '{', and the row array has stale slots at [-1] (a row of
 * openers) and [n] (a row of closers).  A matcher that reads there sees a
 * comment edge, an opener or a closer where the text has none.
 *
 * What only ASan sees: a matcher whose wpe_row_len does not test the row
 * still gives the right answers in the hostile frame, though it reads the
 * stale slot.  That read is red only under the sanitizer, which
 * tests/run-tests.sh --asan runs on this unit as a local gate.
 */
#include <stdio.h>
#include "linebuf.h"
#include <stdlib.h>
#include <string.h>

#include "../edit.h"

#define MAX_ROWS   16
#define FRAME_BYTES 16  /* hostile frame: bytes before s[0] and past the NUL */
#define STALE_BEFORE "{{{{(((([[[["   /* hostile frame: the stale slot at [-1] */
#define STALE_AFTER  "}}}}))))]]]]"   /* hostile frame: the stale slot at [n] */
#define NO_TERM    '\n' /* the byte the editor keeps after a row */
#define BACKSLASH_TERM '\\'

struct golden {
  const char *name;
  const char *text;     /* rows separated by '|' */
  int y, x;             /* the cursor */
  int term;             /* the byte after each row's text */
  const char *want;
};

static const struct golden cases[] = {
  /* forward: the three pairs and nesting */
  {"brace pair", "{ }", 0, 0, NO_TERM, "ret=0 at=0,2"},
  {"parenthesis pair", "( )", 0, 0, NO_TERM, "ret=0 at=0,2"},
  {"bracket pair", "[ ]", 0, 0, NO_TERM, "ret=0 at=0,2"},
  {"nested same pair", "{ { } }", 0, 0, NO_TERM, "ret=0 at=0,6"},
  {"other pairs not counted", "{ ( } )", 0, 0, NO_TERM, "ret=0 at=0,4"},
  {"partner on a later row", "{|  x|}", 0, 0, NO_TERM, "ret=0 at=2,0"},
  {"start mid-row", "a { b } c", 0, 2, NO_TERM, "ret=0 at=0,6"},
  {"no closer", "{ (", 0, 0, NO_TERM, "ret=-1"},
  {"no closer, several rows", "{|x|y", 0, 0, NO_TERM, "ret=-1"},
  {"lone slash", "{ a / b }", 0, 0, NO_TERM, "ret=0 at=0,8"},
  {"hash that is not else", "{ #if }", 0, 0, NO_TERM, "ret=0 at=0,6"},
  /* forward: quoted text */
  {"closer in double quotes", "{ \"}\" }", 0, 0, NO_TERM, "ret=0 at=0,6"},
  {"escaped quote in string", "{ \"\\\"}\" }", 0, 0, NO_TERM, "ret=0 at=0,8"},
  {"escaped backslash ends string", "{ \"\\\\\" }", 0, 0, NO_TERM, "ret=0 at=0,7"},
  {"string continued by backslash", "{ \"a\\|}\" }", 0, 0, NO_TERM, "ret=0 at=1,3"},
  {"string continued on last row", "{|\"a\\", 0, 0, NO_TERM, "ret=-1"},
  {"unterminated string", "{ \"} ", 0, 0, NO_TERM, "ret=-1"},
  {"closer in single quotes", "{ '}' }", 0, 0, NO_TERM, "ret=0 at=0,6"},
  {"escaped single quote", "{ '\\'' }", 0, 0, NO_TERM, "ret=0 at=0,7"},
  {"char continued by backslash", "{ 'a\\|}' }", 0, 0, NO_TERM, "ret=0 at=1,3"},
  {"unterminated char", "{ '} ", 0, 0, NO_TERM, "ret=-1"},
  /* forward: comments */
  {"closer in comment", "{ /* } */ }", 0, 0, NO_TERM, "ret=0 at=0,10"},
  {"comment over rows", "{ /* a|} b|c */ }", 0, 0, NO_TERM, "ret=0 at=2,5"},
  {"comment never closed", "{ /* }|}", 0, 0, NO_TERM, "ret=-1"},
  {"slash star at row end", "{ /*|*/ }", 0, 0, NO_TERM, "ret=0 at=1,3"},
  {"closer after // tail", "{ // }|}", 0, 0, NO_TERM, "ret=0 at=1,0"},
  {"closer before // tail", "{ } // {", 0, 0, NO_TERM, "ret=0 at=0,2"},
  /* forward: #else */
  {"else side skipped", "{|#else|}|#endif|}", 0, 0, NO_TERM, "ret=0 at=4,0"},
  {"elif side skipped", "{|#elif X|}|#endif|}", 0, 0, NO_TERM, "ret=0 at=4,0"},
  {"nested if in else side", "{|#else|#ifdef X|}|#endif|}|#endif|}", 0, 0, NO_TERM, "ret=0 at=7,0"},
  {"nested ifndef in else side", "{|#else|  #ifndef X|}|  #endif|}|#endif|}", 0, 0, NO_TERM, "ret=0 at=7,0"},
  {"else with no endif", "{|#else|}|x", 0, 0, NO_TERM, "ret=-1"},
  {"if side counted", "{|#if A|}|#else|x|#endif", 0, 0, NO_TERM, "ret=0 at=2,0"},
  /* backward: closers */
  {"back from brace", "{ }", 0, 2, NO_TERM, "ret=0 at=0,0"},
  {"back from parenthesis", "( )", 0, 2, NO_TERM, "ret=0 at=0,0"},
  {"back from bracket", "[ ]", 0, 2, NO_TERM, "ret=0 at=0,0"},
  {"back over nesting", "{ { } }", 0, 6, NO_TERM, "ret=0 at=0,0"},
  {"back over other pairs", "{ ( } )", 0, 6, NO_TERM, "ret=0 at=0,2"},
  {"back to an earlier row", "{|x|}", 2, 0, NO_TERM, "ret=0 at=0,0"},
  {"back, no opener", "x }", 0, 2, NO_TERM, "ret=-1"},
  {"back, opener of another pair", "( }", 0, 2, NO_TERM, "ret=-1"},
  /* backward: quoted text */
  {"back over double quotes", "{ \"{\" }", 0, 6, NO_TERM, "ret=0 at=0,0"},
  {"back over escaped quote", "{ \"\\\"{\" }", 0, 8, NO_TERM, "ret=0 at=0,0"},
  {"back over escaped backslash", "{ \"\\\\\" }", 0, 7, NO_TERM, "ret=0 at=0,0"},
  {"back over single quotes", "{ '{' }", 0, 6, NO_TERM, "ret=0 at=0,0"},
  {"back over escaped single quote", "{ '\\'{' }", 0, 8, NO_TERM, "ret=0 at=0,0"},
  {"back, string never opened", "x\" { }", 0, 5, NO_TERM, "ret=0 at=0,3"},
  {"back, char never opened", "x' { }", 0, 5, NO_TERM, "ret=0 at=0,3"},
  {"back, string joined to row above", "{ \"a\\|b\" }", 1, 3, BACKSLASH_TERM, "ret=0 at=0,0"},
  {"back, char joined to row above", "{ 'a\\|b' }", 1, 3, BACKSLASH_TERM, "ret=0 at=0,0"},
  {"back, string joined, row above ends otherwise", "{ \"a|b\" }", 1, 3, NO_TERM, "ret=-1"},
  /* backward: comments */
  {"back over comment", "{ /* { */ }", 0, 10, NO_TERM, "ret=0 at=0,0"},
  {"back over comment of rows", "{|/* {|{ */ }", 2, 6, NO_TERM, "ret=-1"},
  {"back, comment never opened", "x */ }", 0, 5, NO_TERM, "ret=-1"},
  {"back, comment never opened, rows", "x|y */ }", 1, 5, NO_TERM, "ret=-1"},
  {"back, slash after non-star", "{ a/ }", 0, 5, NO_TERM, "ret=0 at=0,0"},
  {"slash at column 0 behind the cursor", "/ }", 0, 2, NO_TERM, "ret=-1"},
  /* backward: // tails */
  {"back, bracket in tail above", "{ // {|}", 1, 0, NO_TERM, "ret=0 at=0,0"},
  {"back, tail starts the row", "{|// {|}", 2, 0, NO_TERM, "ret=0 at=0,0"},
  {"back, tail on the cursor row", "{ // {", 0, 5, NO_TERM, "ret=-1"},
  {"back, cursor left of the tail", "{ } // x", 0, 2, NO_TERM, "ret=0 at=0,0"},
  /* backward: #else */
  {"back over else side", "{|#if A|}|#else|x|#endif|}", 5, 0, NO_TERM, "ret=0 at=0,0"},
  {"back over else side, nested", "{|#if A|#ifdef B|#endif|#else|#endif|}", 5, 0, NO_TERM, "ret=0 at=0,0"},
  {"back over elif side", "{|#if A|}|#elif B|x|#endif|}", 5, 0, NO_TERM, "ret=0 at=0,0"},
  {"back from the else row", "{|#if A|#else|}", 2, 2, NO_TERM, "ret=0 at=0,0"},
  {"back, else with no if", "{|#else|x|}", 3, 0, NO_TERM, "ret=-1"},
  /* backward: from anything that is not a bracket */
  {"enclosing brace", "{ a b", 0, 4, NO_TERM, "ret=0 at=0,0"},
  {"enclosing paren after closed pair", "( { } a", 0, 6, NO_TERM, "ret=0 at=0,0"},
  {"enclosing bracket on row above", "[|a", 1, 0, NO_TERM, "ret=0 at=0,0"},
  {"no enclosing opener", "a b", 0, 2, NO_TERM, "ret=0 at=0,0"},
  {"no enclosing opener after pair", "{ } a", 0, 4, NO_TERM, "ret=-1"},
  {"cursor at the start", "a", 0, 0, NO_TERM, "ret=-1"},
  {"start of text is the answer", "x|y", 1, 0, NO_TERM, "ret=0 at=0,0"},
  {"enclosing skips string", "{ \"{\" a", 0, 7, NO_TERM, "ret=0 at=0,0"},
  {"enclosing skips comment", "{ /* } */ a", 0, 10, NO_TERM, "ret=0 at=0,0"},
  {"empty row", "{||x", 2, 0, NO_TERM, "ret=0 at=0,0"},
  /* edges the first set let a changed matcher through */
  {"slash-star-slash does not close", "{ /*/ } */ }", 0, 0, NO_TERM, "ret=0 at=0,11"},
  {"backslash at row end, no join byte", "{ \"a\\|b\" }", 1, 3, NO_TERM, "ret=-1"},
  {"back over slash-star-slash", "{ /*/ }", 0, 6, NO_TERM, "ret=-1"},
  {"#endif on the last row is not read", "[|#else|        x|#endif]", 0, 0, NO_TERM, "ret=-1"},
  {"back, row 0 not read by the #else skip", "#if {|#else#|}", 2, 0, NO_TERM, "ret=0 at=0,4"},
  {"indented #endif closes the else side", "{|#else|}|  #endif|}", 0, 0, NO_TERM, "ret=0 at=4,0"},
  {"back, indented #if", "{|  #if A|}|#else|x|#endif|}", 5, 0, NO_TERM, "ret=0 at=0,0"},
  {"enclosing, bracket pair before cursor", "[ ] x", 0, 4, NO_TERM, "ret=-1"},
  {"#else row with a later hash", "#else ( #|)|#endif|)", 0, 6, NO_TERM, "ret=0 at=3,0"},
  /* reads outside the text: an #else row at either end of the text */
  {"else row 0, cursor at its end", "#else # xyz", 0, 11, NO_TERM, "ret=-1"},
  {"else row 0, cursor on z", "#else # xyz", 0, 10, NO_TERM, "ret=-1"},
  {"else row 0 reached from the row below", "#else # x|}", 1, 0, NO_TERM, "ret=-1"},
  {"elif row 0 with a later hash", "#elif A # x|)", 1, 0, NO_TERM, "ret=-1"},
  {"opener, else on the last row", "{|#else", 0, 0, NO_TERM, "ret=-1"},
  {"elif on the last row", "(|x|#elif B", 0, 0, NO_TERM, "ret=-1"},
  {"else on the last row, closer beyond is not read", "{|x|#else", 0, 0, NO_TERM, "ret=-1"},
  /* reads outside the text: a comment edge at column 0 */
  {"comment at column 0 above a closer", "/* c */|}", 1, 0, NO_TERM, "ret=-1"},
  {"comment at column 0 inside a pair", "{|/* c */|}", 2, 0, NO_TERM, "ret=0 at=0,0"},
  {"star-slash at column 0", "*/|}", 1, 0, NO_TERM, "ret=-1"},
  {"star-slash at column 0 under an opener", "{|*/|}", 2, 0, NO_TERM, "ret=-1"},
  {"slash at column 0, enclosing walk", "/ a", 0, 2, NO_TERM, "ret=0 at=0,0"},
  {"comment of rows closed at column 0", "{ /*|*/ }", 0, 6, NO_TERM, "ret=0 at=0,0"},
  /* reads outside the text: a column carried onto a shorter row */
  {"skip column lands past row 0's end", "a|        x|#else|}", 3, 0, NO_TERM, "ret=-1"},
  {"same, any-opener walk", "{|        x|#else|y", 3, 0, NO_TERM, "ret=0 at=0,0"},
  {"same, pair walk", "{|        x|#else|}", 3, 0, NO_TERM, "ret=0 at=0,0"},
};

#define NCASES ((int)(sizeof cases / sizeof cases[0]))

static int fails;

/* How many bytes lie before s[0] in a frame. */
static int frame_before(int hostile)
{
  return hostile ? FRAME_BYTES : 0;
}

/* Allocate one row of @len text bytes the way the row owner does: len+2
 * bytes from s[0].  A hostile frame adds the bytes before and after that
 * a matcher must not read: '/' and '*' alternating before s[0] (s[-1] is
 * '*'), '{' past the NUL. */
static void make_row(STRING *row, const char *text, size_t len, int term,
                     int hostile)
{
  int before = frame_before(hostile);
  size_t total = (size_t)before + len + 2 + (hostile ? FRAME_BYTES : 0);
  unsigned char *area = calloc(1, total);
  size_t k;

  for (k = 0; k < (size_t)before; k++)
    area[k] = ((size_t)before - k) % 2 ? '*' : '/';
  memcpy(area + before, text, len);
  area[before + len] = (unsigned char)term;
  area[before + len + 1] = 0;
  for (k = before + len + 2; k < total; k++)
    area[k] = '{';
  row->s = area + before;
  row->len = (int)len;
}

static void free_row(STRING *row, int hostile)
{
  free(row->s - frame_before(hostile));
}

/* Build a buffer of the rows of @text in an array of exactly as many rows
 * as the text has.  In a hostile frame the array also has a stale slot at
 * [-1] and at [n], each a row of closers.  Return the number of rows. */
static int build(BUFFER *b, const char *text, int term, int hostile)
{
  int n = 0;
  int slack = hostile ? 1 : 0;
  const char *p = text;
  STRING *rows = calloc(MAX_ROWS + 2, sizeof *rows);
  STRING *exact;

  while (n < MAX_ROWS)
  {
    const char *e = strchr(p, '|');
    size_t len = e ? (size_t)(e - p) : strlen(p);

    make_row(&rows[n], p, len, term, hostile);
    n++;
    if (!e)
      break;
    p = e + 1;
  }
  exact = calloc(n + 2 * slack, sizeof *exact);
  memcpy(exact + slack, rows, n * sizeof *exact);
  free(rows);
  if (hostile)
  {
    make_row(&exact[0], STALE_BEFORE, strlen(STALE_BEFORE), term, hostile);
    make_row(&exact[n + 1], STALE_AFTER, strlen(STALE_AFTER), term, hostile);
  }
  memset(b, 0, sizeof *b);
  b->bf = exact + slack;
  b->mxlines = n;
  return n;
}

static void release(BUFFER *b, int n, int hostile)
{
  int i;

  for (i = 0; i < n; i++)
    free_row(&b->bf[i], hostile);
  if (hostile)
  {
    free_row(&b->bf[-1], hostile);
    free_row(&b->bf[n], hostile);
    free(b->bf - 1);
  }
  else
    free(b->bf);
}

/* Run one case in one frame and describe where the matcher stopped. */
static void run_case(const struct golden *g, int hostile, char *out,
                     size_t size)
{
  BUFFER b;
  int n = build(&b, g->text, g->term, hostile);
  int y = -1, x = -1, ret;

  b.b.y = g->y;
  b.b.x = g->x;
  ret = e_brk_match(&b, &y, &x);
  if (ret == 0)
    snprintf(out, size, "ret=0 at=%d,%d", y, x);
  else
    snprintf(out, size, "ret=%d", ret);
  release(&b, n, hostile);
}

int main(int argc, char **argv)
{
  char out[64];
  int i, print = argc > 1 && !strcmp(argv[1], "--print");

  for (i = 0; i < NCASES; i++)
  {
    int hostile;

    for (hostile = 1; hostile >= 0; hostile--)
    {
      run_case(&cases[i], hostile, out, sizeof out);
      if (print)
      {
        if (!hostile)
          printf("  {\"%s\", \"%s\", %d, %d, %s, \"%s\"},\n", cases[i].name,
                 cases[i].text, cases[i].y, cases[i].x,
                 cases[i].term == NO_TERM ? "NO_TERM" : "BACKSLASH_TERM", out);
      }
      else if (strcmp(out, cases[i].want) != 0)
      {
        printf("FAIL: %s (%s frame): got [%s] want [%s]\n", cases[i].name,
               hostile ? "hostile" : "exact", out, cases[i].want);
        fflush(stdout);   /* a later case may crash the process */
        fails++;
      }
    }
  }
  if (fails)
    return 1;
  if (!print)
    printf("test_brkmatch: %d cases OK\n", NCASES);
  return 0;
}
