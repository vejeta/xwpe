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
 * The editor's rows have room after the text (len+2 bytes: the line break
 * and a NUL) but no byte before s[0].  The matcher reads one character
 * beyond a row's end and, from column 0, one before it (a known read at
 * s[-1]).  The byte before each row exists only here, as a zeroed pad, so
 * the unit can step over that read; the rows also get zeroed bytes after.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../edit.h"

#define MAX_ROWS   16
#define ROW_PAD    16   /* zeroed bytes after each row */
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
};

#define NCASES ((int)(sizeof cases / sizeof cases[0]))

static int fails;

/* Build a buffer of the rows of @text.  Return the number of rows. */
static int build(BUFFER *b, STRING *rows, const char *text, int term)
{
  int n = 0;
  const char *p = text;

  while (n < MAX_ROWS)
  {
    const char *e = strchr(p, '|');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    unsigned char *area = calloc(1, 1 + len + 1 + ROW_PAD);

    memcpy(area + 1, p, len);
    area[1 + len] = (unsigned char)term;
    rows[n].s = area + 1;
    rows[n].len = (int)len;
    n++;
    if (!e)
      break;
    p = e + 1;
  }
  memset(b, 0, sizeof *b);
  b->bf = rows;
  b->mxlines = n;
  return n;
}

static void release(STRING *rows, int n)
{
  int i;

  for (i = 0; i < n; i++)
    free(rows[i].s - 1);
}

/* Run one case and describe where the matcher stopped. */
static void run_case(const struct golden *g, char *out, size_t size)
{
  BUFFER b;
  STRING rows[MAX_ROWS];
  int n = build(&b, rows, g->text, g->term);
  int y = -1, x = -1, ret;

  b.b.y = g->y;
  b.b.x = g->x;
  ret = e_brk_match(&b, &y, &x);
  if (ret == 0)
    snprintf(out, size, "ret=0 at=%d,%d", y, x);
  else
    snprintf(out, size, "ret=%d", ret);
  release(rows, n);
}

int main(int argc, char **argv)
{
  char out[64];
  int i, print = argc > 1 && !strcmp(argv[1], "--print");

  for (i = 0; i < NCASES; i++)
  {
    run_case(&cases[i], out, sizeof out);
    if (print)
      printf("  {\"%s\", \"%s\", %d, %d, %s, \"%s\"},\n", cases[i].name,
             cases[i].text, cases[i].y, cases[i].x,
             cases[i].term == NO_TERM ? "NO_TERM" : "BACKSLASH_TERM", out);
    else if (strcmp(out, cases[i].want) != 0)
    {
      printf("FAIL: %s: got [%s] want [%s]\n", cases[i].name, out,
             cases[i].want);
      fails++;
    }
  }
  if (fails)
    return 1;
  if (!print)
    printf("test_brkmatch: %d cases OK\n", NCASES);
  return 0;
}
