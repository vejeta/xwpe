/* test_rowfind.c -- unit test for the owner of a search over a row
 * (we_rowfind.c), linked alone.  Always runs.
 *
 * What the user would see go wrong if a rule broke: Find stopping on half
 * an accented letter, a case insensitive search turning `\w` into `\W`, a
 * word boundary at the edge of a block judged on a cut row, `^` or `$`
 * found in the middle of a long line that the editor wrapped, Change All
 * with `^` or `x*` standing on one place for ever or writing inside its own
 * text, a whole word search that takes `a_foo` for `foo`, Replace All on a
 * row of twenty thousand bytes taking seconds.
 *
 * Built three times from this file:
 *   test_rowfind         the engine runs on the whole row (REG_STARTEND);
 *   test_rowfind_copy    WPE_ROWFIND_NO_STARTEND: the engine runs on a copy
 *                        from the start of each call, which cannot see the
 *                        byte before it; the word boundary patterns differ
 *                        from the reference there and only there;
 *   test_rowfind_reject  WPE_ROWFIND_NO_UTF8_CTYPE: no UTF-8 ctype to run the
 *                        engine under, so a process locale that is not UTF-8
 *                        leaves the engine reading bytes and the owner has
 *                        to reject a match that covers part of a character.
 * Each runs its passes under the C locale, C.UTF-8 and en_US.iso885915 (a
 * pass whose locale is missing says so and is skipped).
 *
 * The differential: for every character start p of a random row the engine
 * is run by hand with rm_so = p and the match taken only if it starts at p;
 * the filters (scope, whole characters, words, empty hits, the row's kind)
 * are applied in plain code below.  The owner, which walks and doubles a
 * window, must give the same first (forward) or last (backward) hit.
 *
 * ROWFIND_ITERS sets the random rows per pass (default 20000);
 * ROWFIND_NO_PERF=1 skips the speed bounds (a sanitizer build). */
/* The scenario tag goes here when the manifest rows are added, with the
   callers' pass. */
#include <ctype.h>
#include <langinfo.h>
#include <locale.h>
#include <regex.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "../we_rowfind.h"
#include "../we_utf8.h"

#ifdef WPE_ROWFIND_NO_UTF8_CTYPE
#define OWNER_HAS_UTF8_CTYPE 0
#else
#define OWNER_HAS_UTF8_CTYPE 1
#endif

#ifdef WPE_ROWFIND_NO_STARTEND
#define OWNER_SEES_BEFORE 0
#else
#define OWNER_SEES_BEFORE 1
#endif

#define SW_CASE   128
#define SW_WORD    64
#define SW_REGEX   32
#define BWD  WPE_ROWFIND_BACKWARD
#define AFT  WPE_ROWFIND_AFTER_HIT
#define KIND_BOTH (WPE_ROW_STARTS_LINE | WPE_ROW_ENDS_LINE)

static int fails, checks;
static int copy_differences;       /* owner != reference, on a word boundary pattern */
/* glibc judges \B after an atom that may match empty differently when a call
   starts before the place and when it starts there, on ASCII rows too
   (measured with a bare regexec: ".*\B" on " ..e" from 2 gives 3..3, from 3
   no match).  The engine disagrees with itself, so such a row cannot be a
   reference; engine_self_inconsistent finds those rows, which are counted
   and said, and every other row is compared. */
static int byte_engine_quirks;
static const char *pass_name = "";

static void fail(const char *what)
{
 printf("FAIL [%s]: %s\n", pass_name, what);
 fails++;
}

/* ---- a deterministic generator ---- */

static uint64_t rng_state = 88172645463325252ULL;

static unsigned rnd(unsigned n)
{
 rng_state ^= rng_state << 13;
 rng_state ^= rng_state >> 7;
 rng_state ^= rng_state << 17;
 return((unsigned)((rng_state >> 11) % n));
}

/* ---- the process's locale and what the engine will read ---- */

static int process_utf8(void)
{
 const char *cs = nl_langinfo(CODESET);

 return(cs != NULL && (strcasecmp(cs, "UTF-8") == 0 || strcasecmp(cs, "UTF8") == 0));
}

/* Does the engine read characters (the owner made a UTF-8 ctype, or the
   process already has one), or bytes? */
static int engine_reads_chars(void)
{
 return(OWNER_HAS_UTF8_CTYPE || process_utf8());
}

static locale_t ref_utf8_ctype(void)
{
 static locale_t loc;
 static int made;

 if (!made)
 {
  made = 1;
  loc = newlocale(LC_CTYPE_MASK, "C.UTF-8", (locale_t)0);
  if (loc == (locale_t)0)
   loc = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", (locale_t)0);
 }
 return(loc);
}

/* ---- the reference: the engine run by hand on every character start ---- */

static int ref_is_start(const unsigned char *row, int len, int p)
{
 return(e_utf8_glyph_floor(row, len, p) == p);
}

static int ref_word_byte(unsigned char c)
{
 return(isalnum(c) || c == '_' || c >= 0x80);
}

static void ref_escape(const char *text, char *out)
{
 for (; *text; text++)
 {
  if (strchr("\\^$.[]|()*+?{}", *text) != NULL)
   *out++ = '\\';
  *out++ = *text;
 }
 *out = '\0';
}

/* 1 a hit, 0 none, -1 the text does not compile. */
static int ref_find(const FIND *fd, const unsigned char *row, int len, unsigned kind,
                    int from, int to, unsigned how, WpeRowHit *hit)
{
 regex_t re;
 char pat[2 * E_FIND_TEXT_MAX + 4];
 int cflags = REG_EXTENDED | ((fd->sw & SW_CASE) ? 0 : REG_ICASE);
 int rc, p, found = 0, f, t;
 locale_t loc = (OWNER_HAS_UTF8_CTYPE && !process_utf8()) ? ref_utf8_ctype() : (locale_t)0;
 locale_t prev = (locale_t)0;

 if (fd->search[0] == '\0')
  return(0);
 if (fd->sw & SW_REGEX)
  strcpy(pat, fd->search);
 else
  ref_escape(fd->search, pat);
 if (loc != (locale_t)0)
  prev = uselocale(loc);
 rc = regcomp(&re, pat, cflags);
 if (rc != 0)
 {
  if (loc != (locale_t)0)
   uselocale(prev);
  return(-1);
 }
 if (from < 0)
  from = 0;
 if (to > len)
  to = len;
 f = from;
 while (f < len && !ref_is_start(row, len, f))
  f++;
 t = (to < 0) ? -1 : e_utf8_glyph_floor(row, len, to);
 for (p = f; p <= t; p++)
 {
  regmatch_t m;
  int flags = 0, so, eo;

  if (!ref_is_start(row, len, p))
   continue;
  if (p > 0 || !(kind & WPE_ROW_STARTS_LINE) || ((how & AFT) && !(how & BWD)))
   flags |= REG_NOTBOL;
  if (!(kind & WPE_ROW_ENDS_LINE))
   flags |= REG_NOTEOL;
  m.rm_so = p;
  m.rm_eo = len;
  if (regexec(&re, (const char *)row, 1, &m, flags | REG_STARTEND) != 0)
   continue;
  so = (int)m.rm_so;
  eo = (int)m.rm_eo;
  if (so != p || eo > t || !ref_is_start(row, len, eo))
   continue;
  if (so == eo)
  {
   if (so == 0 && !(kind & WPE_ROW_STARTS_LINE))
    continue;
   if ((how & AFT) && !(how & BWD) && so == f)
    continue;
   if ((how & AFT) && (how & BWD) && so == t)
    continue;
  }
  else if (fd->sw & SW_WORD)
  {
   if (so > 0 && ref_word_byte(row[so - 1]))
    continue;
   if (eo < len && ref_word_byte(row[eo]))
    continue;
  }
  hit->so = so;
  hit->eo = eo;
  found = 1;
  if (!(how & BWD))
   break;
 }
 regfree(&re);
 if (loc != (locale_t)0)
  uselocale(prev);
 return(found);
}

/* ---- helpers for the rows ---- */

static FIND the_fd;

static const FIND *fd_for(const char *text, unsigned sw)
{
 memset(&the_fd, 0, sizeof the_fd);
 strncpy(the_fd.search, text, E_FIND_TEXT_MAX);
 the_fd.sw = sw;
 return(&the_fd);
}

/* A row as the editor holds it: its bytes, then the line break and a NUL, in
   a heap block of exactly that size.  The owner must never take the line
   break for text (a sanitizer's regexec check also reads up to the NUL). */
static unsigned char *row_copy(const void *bytes, int len)
{
 unsigned char *r = malloc((size_t)(len > 0 ? len : 0) + 2);

 if (r == NULL)
 {
  printf("FAIL: out of memory\n");
  exit(1);
 }
 if (len > 0)
  memcpy(r, bytes, (size_t)len);
 r[len > 0 ? len : 0] = '\n';
 r[(len > 0 ? len : 0) + 1] = '\0';
 return(r);
}

/* One search; the result is rc, and the columns when rc == 1. */
static int run(const char *text, unsigned sw, const char *row, unsigned kind,
               int from, int to, unsigned how, int *so, int *eo)
{
 WpeRowHit h = { -7, -7 };
 int len = (int)strlen(row), rc;
 unsigned char *r = row_copy(row, len);

 rc = wpe_rowfind_row(fd_for(text, sw), r, len, kind, from, to < 0 ? len : to, how, &h);
 free(r);
 if (so != NULL)
  *so = h.so;
 if (eo != NULL)
  *eo = h.eo;
 return(rc);
}

static void expect(const char *name, const char *text, unsigned sw, const char *row,
                   unsigned kind, int from, int to, unsigned how, int want_rc,
                   int want_so, int want_eo)
{
 int so = -7, eo = -7, rc = run(text, sw, row, kind, from, to, how, &so, &eo);
 char msg[400];

 checks++;
 if (rc != want_rc || (rc == 1 && (so != want_so || eo != want_eo)))
 {
  snprintf(msg, sizeof msg, "%s: Find \"%s\" on \"%s\": got rc %d %d..%d, want rc %d %d..%d",
           name, text, row, rc, so, eo, want_rc, want_so, want_eo);
  fail(msg);
 }
}

/* ---- the named rows ---- */

static void named_rows(void)
{
 const char *e_acute = "\xC3\xA9", *E_acute = "\xC3\x89";
 int so, eo, rc, i;

 /* case: a regular expression keeps its meaning with Case sensitive off */
 expect("w ICASE", "\\w+", SW_REGEX, "ab cd", KIND_BOTH, 0, -1, 0, 1, 0, 2);
 /* look-behind at the start of the call: the owner sees the byte before */
 expect("ba from 1", "\\ba", SW_REGEX | SW_CASE, "xab", KIND_BOTH, 1, -1, 0,
        OWNER_SEES_BEFORE ? 0 : 1, 1, 2);
 /* the block's end is a range, not the end of the text */
 expect("b boundary at to", "b\\b", SW_REGEX | SW_CASE, "abc", KIND_BOTH, 0, 2, 0, 0, 0, 0);
 expect("b at to", "b", SW_CASE, "abc", KIND_BOTH, 0, 2, 0, 1, 1, 2);
 expect("c at to", "c", SW_CASE, "abc", KIND_BOTH, 0, 2, 0, 0, 0, 0);
 /* the row's kind */
 expect("c$", "c$", SW_REGEX | SW_CASE, "abc", KIND_BOTH, 0, -1, 0, 1, 2, 3);
 expect("c$ not the end of the line", "c$", SW_REGEX | SW_CASE, "abc", WPE_ROW_STARTS_LINE,
        0, -1, 0, 0, 0, 0);
 expect("c$ past to", "c$", SW_REGEX | SW_CASE, "abc", KIND_BOTH, 0, 2, 0, 0, 0, 0);
 expect("^a not the start of the line", "^a", SW_REGEX | SW_CASE, "abc", WPE_ROW_ENDS_LINE,
        0, -1, 0, 0, 0, 0);
 /* an empty match is a position, found once */
 expect("^ fresh", "^", SW_REGEX, "abc", KIND_BOTH, 0, -1, 0, 1, 0, 0);
 expect("^ after a hit", "^", SW_REGEX, "abc", KIND_BOTH, 0, -1, AFT, 0, 0, 0);
 expect("^ on a continuation row", "^", SW_REGEX, "abc", WPE_ROW_ENDS_LINE, 0, -1, 0, 0, 0, 0);
 expect("$ fresh", "$", SW_REGEX, "abc", KIND_BOTH, 0, 3, 0, 1, 3, 3);
 expect("$ after a hit, backward", "$", SW_REGEX, "abc", KIND_BOTH, 0, 3, AFT | BWD, 0, 0, 0);
 expect("x* fresh", "x*", SW_REGEX, "abc", KIND_BOTH, 0, -1, 0, 1, 0, 0);
 expect("x* after a hit from 1", "x*", SW_REGEX, "abc", KIND_BOTH, 1, -1, AFT, 1, 2, 2);
 expect("x* on a continuation row", "x*", SW_REGEX, "abc", WPE_ROW_ENDS_LINE, 0, -1, 0, 1, 1, 1);
 expect("x* backward fresh", "x*", SW_REGEX, "ab", KIND_BOTH, 0, -1, BWD, 1, 2, 2);
 expect("x* backward after a hit", "x*", SW_REGEX, "ab", KIND_BOTH, 0, -1, BWD | AFT, 1, 1, 1);
 expect("a* hit with text, from 0", "a*", SW_REGEX, "aab", KIND_BOTH, 0, -1, AFT, 1, 0, 2);
 /* going on from a hit at a line's start (Change All with nothing for New
    Text) has passed the ^: `^a` on "aab" is replaced once, `^.` once */
 expect("^a after a hit at column 0", "^a", SW_REGEX, "ab", KIND_BOTH, 0, -1, AFT, 0, 0, 0);
 expect("^. after a hit at column 0", "^.", SW_REGEX, "bc", KIND_BOTH, 0, -1, AFT, 0, 0, 0);
 expect("^a fresh", "^a", SW_REGEX, "ab", KIND_BOTH, 0, -1, 0, 1, 0, 1);
 expect("^a backward after a hit", "^a", SW_REGEX, "aab", KIND_BOTH, 0, 2, BWD | AFT, 1, 0, 1);
 /* whole characters */
 if (engine_reads_chars())
 {
  expect(". is a character", ".", SW_REGEX, "\xC3\xA9" "a", KIND_BOTH, 0, -1, 0, 1, 0, 2);
  expect("e acute ICASE finds E acute", e_acute, 0, E_acute, KIND_BOTH, 0, -1, 0, 1, 0, 2);
  expect("e acute ICASE finds E acute, regular", e_acute, SW_REGEX, E_acute, KIND_BOTH, 0, -1, 0, 1, 0, 2);
 }
 else
 {
  expect(". rejects half a character", ".", SW_REGEX, "\xC3\xA9" "a", KIND_BOTH, 0, -1, 0, 1, 2, 3);
  expect("e acute ICASE is bytes", e_acute, 0, E_acute, KIND_BOTH, 0, -1, 0, 0, 0, 0);
 }
 expect("[e acute] on A tilde", "[\xC3\xA9]", SW_REGEX, "\xC3\x83", KIND_BOTH, 0, -1, 0, 0, 0, 0);
 expect("[copyright] on e acute", "[\xC2\xA9]", SW_REGEX, "\xC3\xA9", KIND_BOTH, 0, -1, 0, 0, 0, 0);
 expect("e acute ICASE is no fold of U+3A40", e_acute, 0, "\xE3\xA9\x80", KIND_BOTH, 0, -1, 0, 0, 0, 0);
 /* plain text goes through the engine as text */
 expect("plain a.b", "a.b", SW_CASE, "axb a.b", KIND_BOTH, 0, -1, 0, 1, 4, 7);
 expect("plain a*b", "a*b", SW_CASE, "aab a*b", KIND_BOTH, 0, -1, 0, 1, 4, 7);
 expect("plain ICASE", "AbC", 0, "xaBc", KIND_BOTH, 0, -1, 0, 1, 1, 4);
 expect("plain ASCII on a lone 0xE9", "a", SW_CASE, "\xE9" "a", KIND_BOTH, 0, -1, 0, 1, 1, 2);
 expect("plain 0xE9", "\xE9", SW_CASE, "x\xE9y", KIND_BOTH, 0, -1, 0, 1, 1, 2);
 expect("plain 0xFF", "\xFF", SW_CASE, "x\xFFy", KIND_BOTH, 0, -1, 0, 1, 1, 2);
 /* an empty text finds nothing; a bad expression is an error */
 expect("empty text", "", 0, "abc", KIND_BOTH, 0, -1, 0, 0, 0, 0);
 expect("empty text, expression", "", SW_REGEX, "abc", KIND_BOTH, 0, -1, 0, 0, 0, 0);
 expect("bad expression", "(", SW_REGEX | SW_WORD, "abc", KIND_BOTH, 0, -1, 0, -1, 0, 0);
 checks++;
 if (wpe_rowfind_error() == NULL || wpe_rowfind_error()[0] == '\0')
  fail("bad expression: wpe_rowfind_error says nothing");
 expect("good text clears the error", "a", SW_CASE, "abc", KIND_BOTH, 0, -1, 0, 1, 0, 1);
 checks++;
 if (wpe_rowfind_error() != NULL)
  fail("a text that compiles leaves an error");
 /* whole words */
 expect("word: _ is a word byte", "foo", SW_CASE | SW_WORD, "a_foo foo", KIND_BOTH, 0, -1, 0, 1, 6, 9);
 expect("word: accented letter is a word letter", "caf", SW_CASE | SW_WORD, "caf\xC3\xA9", KIND_BOTH,
        0, -1, 0, 0, 0, 0);
 expect("word: space then e acute", "caf", SW_CASE | SW_WORD, "caf \xC3\xA9", KIND_BOTH, 0, -1, 0, 1, 0, 3);
 expect("word: a later one, not the first only", "foo", SW_CASE | SW_WORD, "foo_x foo", KIND_BOTH,
        0, -1, 0, 1, 6, 9);
 expect("word: regular expression", "fo+", SW_CASE | SW_WORD | SW_REGEX, "xfoo fo", KIND_BOTH,
        0, -1, 0, 1, 5, 7);
 /* backward */
 expect("backward b", "b", SW_CASE, "abab", KIND_BOTH, 0, 4, BWD, 1, 3, 4);
 expect("backward b to 3", "b", SW_CASE, "abab", KIND_BOTH, 0, 3, BWD, 1, 1, 2);
 expect("backward none", "b", SW_CASE, "abab", KIND_BOTH, 0, 1, BWD, 0, 0, 0);
 expect("backward ab overlaps", "aba", SW_CASE, "ababa", KIND_BOTH, 0, 5, BWD, 1, 2, 5);
 expect("backward longest at the last start", "b+", SW_CASE | SW_REGEX, "abbb", KIND_BOTH, 0, 4, BWD, 1, 3, 4);
 /* the range */
 expect("from > to", "a", SW_CASE, "aaa", KIND_BOTH, 2, 1, 0, 0, 0, 0);
 expect("from > to backward", "a", SW_CASE, "aaa", KIND_BOTH, 2, 1, BWD, 0, 0, 0);
 expect("from inside a character", "a", SW_CASE, "\xC3\xA9" "a", KIND_BOTH, 1, -1, 0, 1, 2, 3);
 expect("to inside a character", ".", SW_REGEX, "a\xC3\xA9", KIND_BOTH, 0, 2, 0, 1, 0, 1);
 /* the look-behind probe: reports, and asserts what each build must say */
 rc = run("\\<b", SW_REGEX | SW_CASE, "ab", KIND_BOTH, 1, -1, 0, &so, &eo);
 checks++;
 if (rc == 1 && OWNER_SEES_BEFORE)
  fail("look-behind: \\<b on \"ab\" from 1 hit although the engine sees the byte before");
 if (rc == 1)
  printf("note [%s]: engine without look-behind at the start of a call (copy path)\n", pass_name);
 /* a window that has to double: the hit is at the far start of a long row */
 {
  char row[2100];

  row[0] = 'a';
  for (i = 1; i < 2000; i++)
   row[i] = 'b';
  row[2000] = '\0';
  expect("backward doubles to the far start", "a", SW_CASE, row, KIND_BOTH, 0, -1, BWD, 1, 0, 1);
  expect("backward doubles, regular", "a", SW_CASE | SW_REGEX, row, KIND_BOTH, 0, -1, BWD, 1, 0, 1);
  expect("backward doubles, none", "x", SW_CASE | SW_REGEX, row, KIND_BOTH, 0, -1, BWD, 0, 0, 0);
 }
}

/* ---- the differential and the invariants ---- */

static const char *ALPHA = "abAB x_.\t-1";
static const char *UNITS[] = { "\xC3\xA9", "\xC3\x89", "\xE2\x86\x92", "\xFF", "\xE9", "\xC3\x83" };
#define UNIT_COUNT 6

static int make_row(unsigned char *out, int max_len, int long_row)
{
 int n = long_row ? 1000 + (int)rnd(1000) : (int)rnd(15);
 int len = 0, k;

 if (n > max_len - 8)
  n = max_len - 8;
 while (len < n)
 {
  if (rnd(6) == 0)
  {
   const char *u = UNITS[rnd(UNIT_COUNT)];

   k = (int)strlen(u);
   memcpy(out + len, u, (size_t)k);
   len += k;
  }
  else
   out[len++] = (unsigned char)ALPHA[rnd((unsigned)strlen(ALPHA))];
 }
 return(len);
}

static const char *TOKENS[] = {
 "a", "b", "A", "x", "\xC3\xA9", "\xC3\x89", ".", "*", "+", "?", "^", "$", "\\w", "\\b",
 "\\<", "\\>", "[ab]", "(a|b)", "a*", "[\xC3\xA9]", "\\s", "_", " ", ".*", "x*", "\\B"
};
#define TOKEN_COUNT 26

static const char *PLAIN[] = {
 "a", "b", "ab", "A", "ab ", " ", "\xC3\xA9", "\xC3\x89", "x_", ".", "a.b", "(", "*", "\xE9", "\xFF", "-1"
};
#define PLAIN_COUNT 16

static void make_text(char *out, unsigned *sw)
{
 *sw = rnd(2) ? SW_CASE : 0;
 if (rnd(2))
  *sw |= SW_WORD;
 out[0] = '\0';
 if (rnd(2))
 {
  int n = 1 + (int)rnd(4);

  *sw |= SW_REGEX;
  while (n-- > 0)
   strcat(out, TOKENS[rnd(TOKEN_COUNT)]);
 }
 else
  strcpy(out, PLAIN[rnd(PLAIN_COUNT)]);
}

static int has_boundary_escape(const char *text, unsigned sw)
{
 return((sw & SW_REGEX) &&
        (strstr(text, "\\b") || strstr(text, "\\B") || strstr(text, "\\<") || strstr(text, "\\>")));
}

/* One bare call of the engine from @p with the owner's flags for the row.
   1 a match (its columns in *so, *eo), 0 none. */
static int bare_match(regex_t *re, const unsigned char *row, int len, unsigned kind,
                      int p, int *so, int *eo)
{
 regmatch_t m;
 int flags = 0;

 if (p > 0 || !(kind & WPE_ROW_STARTS_LINE))
  flags |= REG_NOTBOL;
 if (!(kind & WPE_ROW_ENDS_LINE))
  flags |= REG_NOTEOL;
 m.rm_so = p;
 m.rm_eo = len;
 if (regexec(re, (const char *)row, 1, &m, flags | REG_STARTEND) != 0)
  return(0);
 *so = (int)m.rm_so;
 *eo = (int)m.rm_eo;
 return(1);
}

/* Does the engine disagree with itself on this row: a call from p gives a
   match at s, and a call from s gives another match (or none)?  On such a row
   the answer depends on where a call starts, so no walk can be compared with
   the reference. */
static int engine_self_inconsistent(const FIND *fd, const unsigned char *row, int len,
                                    unsigned kind)
{
 regex_t re;
 char pat[2 * E_FIND_TEXT_MAX + 4];
 int cflags = REG_EXTENDED | ((fd->sw & SW_CASE) ? 0 : REG_ICASE);
 int p, so, eo, so2, eo2, odd = 0;
 locale_t loc = (OWNER_HAS_UTF8_CTYPE && !process_utf8()) ? ref_utf8_ctype() : (locale_t)0;
 locale_t prev = (locale_t)0;

 if (!(fd->sw & SW_REGEX))
  return(0);
 strcpy(pat, fd->search);
 if (loc != (locale_t)0)
  prev = uselocale(loc);
 if (regcomp(&re, pat, cflags) == 0)
 {
  for (p = 0; p <= len && !odd; p++)
  {
   if (!bare_match(&re, row, len, kind, p, &so, &eo))
    continue;
   if (!bare_match(&re, row, len, kind, so, &so2, &eo2) || so2 != so || eo2 != eo)
    odd = 1;
  }
  regfree(&re);
 }
 if (loc != (locale_t)0)
  uselocale(prev);
 return(odd);
}

/* A row glibc's \B disagrees with itself on, in plain ASCII: the
   differential must skip it by that measure and not by its bytes. */
static void engine_quirk_rows(void)
{
 static const char row[] = " ..e";

 checks++;
 if (!engine_self_inconsistent(fd_for(".*\\B", SW_REGEX | SW_CASE), (const unsigned char *)row,
                               (int)strlen(row), KIND_BOTH))
  printf("note [%s]: the engine is consistent on \".*\\B\" over \" ..e\" (not glibc's)\n", pass_name);
}

static int word_bytes_ok(const unsigned char *row, int len, int so, int eo)
{
 if (so > 0 && ref_word_byte(row[so - 1]))
  return(0);
 if (eo < len && ref_word_byte(row[eo]))
  return(0);
 return(1);
}

/* Invariants of a hit that hold whatever the engine did. */
static void check_hit(const char *label, const FIND *fd, const unsigned char *row,
                      int len, int from, int to, unsigned how, const WpeRowHit *h)
{
 int f = from < 0 ? 0 : from, t = to > len ? len : to;

 while (f < len && !ref_is_start(row, len, f))
  f++;
 t = e_utf8_glyph_floor(row, len, t);
 if (h->so < f || h->so > h->eo || h->eo > t)
  fail(label), printf("   hit %d..%d outside [%d,%d]\n", h->so, h->eo, f, t);
 if (!ref_is_start(row, len, h->so) || !ref_is_start(row, len, h->eo))
  fail(label), printf("   hit %d..%d is not on character starts\n", h->so, h->eo);
 if (h->so < h->eo && (fd->sw & SW_WORD) && !word_bytes_ok(row, len, h->so, h->eo))
  fail(label), printf("   whole word hit %d..%d has a word byte beside it\n", h->so, h->eo);
 if (h->so == h->eo && (how & AFT) && !(how & BWD) && h->so == f)
  fail(label), printf("   empty hit stands where the search starts\n");
 if (h->so == h->eo && (how & AFT) && (how & BWD) && h->so == t)
  fail(label), printf("   empty hit stands where the backward search starts\n");
}

/* One case of the differential: the owner against the reference, and the
   invariants of the owner's hit. */
static void compare_case(const char *text, unsigned sw, const unsigned char *tmp, int len,
                         unsigned kind, int from, int to, unsigned how, int it)
{
 unsigned char *row, *before;
 int rc_own, rc_ref;
 WpeRowHit own = { -1, -1 }, ref = { -1, -1 };
 const FIND *fd;
 char label[300];

 row = row_copy(tmp, len);
 before = row_copy(tmp, len);
 fd = fd_for(text, sw);
 rc_own = wpe_rowfind_row(fd, row, len, kind, from, to, how, &own);
 rc_ref = ref_find(fd, row, len, kind, from, to, how, &ref);
 checks++;
 snprintf(label, sizeof label, "differential: Find \"%s\" sw %u kind %u [%d,%d] how %u on %d bytes (it %d)",
          text, sw, kind, from, to, how, len, it);
 if (memcmp(row, before, (size_t)(len > 0 ? len : 0)) != 0)
  fail("the owner changed the row");
 if (rc_own != rc_ref || (rc_own == 1 && (own.so != ref.so || own.eo != ref.eo)))
 {
  if (!OWNER_SEES_BEFORE && has_boundary_escape(text, sw))
   copy_differences++;
  else if (has_boundary_escape(text, sw) && engine_self_inconsistent(fd, row, len, kind))
   byte_engine_quirks++;
  else
  {
   int b;

   fail(label);
   printf("   owner rc %d %d..%d, reference rc %d %d..%d\n   row:", rc_own, own.so, own.eo,
          rc_ref, ref.so, ref.eo);
   for (b = 0; b < len && b < 40; b++)
    printf(" %02X", row[b]);
   printf("\n");
   if (fails > 20)
    exit(1);
  }
 }
 if (rc_own == 1)
  check_hit(label, fd, row, len, from, to, how, &own);
 free(row);
 free(before);
}

static void differential(int iterations, int long_rows)
{
 int it;

 for (it = 0; it < iterations; it++)
 {
  unsigned char tmp[2100];
  char text[200];
  unsigned sw, kind = rnd(4), how = 0;
  int len, from, to;

  len = make_row(tmp, (int)sizeof tmp, long_rows);
  make_text(text, &sw);
  if (rnd(2))
   how |= BWD;
  if (rnd(3) == 0)
   how |= AFT;
  from = (int)rnd((unsigned)len + 3) - 1;
  to = (rnd(4) == 0) ? (int)rnd((unsigned)len + 3) - 1 : len - (int)rnd((unsigned)len + 1);
  if (to < from && rnd(4) != 0)
  {
   int room = len + 1 - (from < 0 ? 0 : from);

   to = from + (int)rnd((unsigned)(room > 0 ? room : 1));
  }
  compare_case(text, sw, tmp, len, kind, from, to, how, it);
 }
}

/* Pinned rows of the differential: glibc's \B over plain ASCII disagrees
   with itself, so the skip must be by that measure and not by the row's
   bytes (a skip by bytes fails this row although the owner is right). */
static void pinned_rows(void)
{
 static const char row[] = " ..e";

 engine_quirk_rows();
 compare_case(".*\\B", SW_REGEX | SW_CASE, (const unsigned char *)row, (int)strlen(row),
              KIND_BOTH, 0, (int)strlen(row), BWD, -1);
}

/* ---- progress: Search Again and Change All on rows wrapped at a width ---- */

#define LINE_MAX_BYTES 600
#define LINES_MAX 4
#define ROWS_MAX 256

typedef struct {
 unsigned char t[LINE_MAX_BYTES];
 int orig[LINE_MAX_BYTES];       /* the old byte a byte came from, -1 for the New Text */
 int len, oldlen;
 unsigned char placed[LINE_MAX_BYTES + 2]; /* old places an empty hit was found at */
} Line;

typedef struct { int line, start, len; unsigned kind; } Row;

typedef struct {
 Line line[LINES_MAX];
 int nlines, width;
 Row row[ROWS_MAX];
 int nrows;
} Model;

static int model_cut(const Line *l, int start, int width)
{
 int n = l->len - start, cut;

 if (n <= width)
  return(n);
 cut = e_utf8_glyph_floor(l->t + start, n, width);
 return(cut > 0 ? cut : width);
}

static void model_wrap(Model *m)
{
 int i;

 m->nrows = 0;
 for (i = 0; i < m->nlines; i++)
 {
  const Line *l = &m->line[i];
  int start = 0;

  do {
   int n = model_cut(l, start, m->width);
   Row *r = &m->row[m->nrows++];

   r->line = i;
   r->start = start;
   r->len = n;
   r->kind = (start == 0 ? WPE_ROW_STARTS_LINE : 0u) | (start + n >= l->len ? WPE_ROW_ENDS_LINE : 0u);
   start += n;
  } while (start < l->len && m->nrows < ROWS_MAX);
 }
}

static void model_make(Model *m, int width)
{
 int i, k;

 memset(m, 0, sizeof *m);
 m->width = width;
 m->nlines = 1 + (int)rnd(LINES_MAX);
 for (i = 0; i < m->nlines; i++)
 {
  Line *l = &m->line[i];

  int target = (rnd(3) == 0) ? (int)rnd(8) : (int)rnd(3 * (unsigned)width);

  l->len = 0;
  while (l->len < target)
  {
   if (rnd(6) == 0)
   {
    const char *u = UNITS[rnd(UNIT_COUNT)];
    int n = (int)strlen(u);

    memcpy(l->t + l->len, u, (size_t)n);
    l->len += n;
   }
   else
    l->t[l->len++] = (unsigned char)ALPHA[rnd((unsigned)strlen(ALPHA))];
  }
  for (k = 0; k < l->len; k++)
   l->orig[k] = k;
  l->oldlen = l->len;
 }
 model_wrap(m);
}

/* Normal name of the place (row, col) in the model. */
static void model_place(const Model *m, int row, int col, int *py, int *px)
{
 const Row *r = &m->row[row];

 *py = row;
 *px = col;
 wpe_rowfind_text_place(py, px, r->len, r->kind);
}

/* The row and column a logical offset of a line is shown at; at a soft edge
   either name, by a coin. */
static void model_locate(const Model *m, int line, int off, int *row, int *col)
{
 int i;

 for (i = 0; i < m->nrows; i++)
 {
  const Row *r = &m->row[i];

  if (r->line != line)
   continue;
  if (off >= r->start && off < r->start + r->len)
  {
   *row = i;
   *col = off - r->start;
   return;
  }
  if (off == r->start + r->len && ((r->kind & WPE_ROW_ENDS_LINE) || rnd(2)))
  {
   *row = i;
   *col = off - r->start;
   return;
  }
 }
 for (i = 0; i < m->nrows; i++)       /* off at the end of a soft row, coin said the next */
  if (m->row[i].line == line && off == m->row[i].start)
  {
   *row = i;
   *col = 0;
   return;
  }
 *row = m->nrows - 1;
 *col = m->row[*row].len;
}

static int model_total_bytes(const Model *m)
{
 int i, n = 0;

 for (i = 0; i < m->nlines; i++)
  n += m->line[i].len;
 return(n);
}

/* One search from the cursor, as e_find_next goes over the rows.  The place
   a search starts at on each row is asked of the owner as the resume place. */
static int model_find(const Model *m, const FIND *fd, int backward, int y, int x, int *hrow, WpeRowHit *h)
{
 int j;

 if (!backward)
 {
  for (j = y; j < m->nrows; j++)
  {
   const Row *r = &m->row[j];
   int from = (j == y) ? x : 0, py, px, rc;
   unsigned how = 0;
   unsigned char *copy = row_copy(m->line[r->line].t + r->start, r->len);

   model_place(m, j, from, &py, &px);
   if (wpe_rowfind_resumes_at(py, px, 0))
    how |= AFT;
   rc = wpe_rowfind_row(fd, copy, r->len, r->kind, from, r->len, how, h);
   free(copy);
   if (rc == 1)
   {
    *hrow = j;
    return(1);
   }
  }
  return(0);
 }
 for (j = y; j >= 0; j--)
 {
  const Row *r = &m->row[j];
  int to = (j == y) ? x : r->len, py, px, rc;
  unsigned how = BWD;
  unsigned char *copy = row_copy(m->line[r->line].t + r->start, r->len);

  model_place(m, j, to, &py, &px);
  if (wpe_rowfind_resumes_at(py, px, 1))
   how |= AFT;
  rc = wpe_rowfind_row(fd, copy, r->len, r->kind, 0, to, how, h);
  free(copy);
  if (rc == 1)
  {
   *hrow = j;
   return(1);
  }
 }
 return(0);
}

static void model_note(const Model *m, int row, int col, int backward)
{
 int py, px;

 model_place(m, row, col, &py, &px);
 wpe_rowfind_note_resume(py, px, backward);
}

static void model_progress(int search_again, int backward, int width)
{
 Model m;
 char text[200], label[300];
 unsigned sw;
 const FIND *fd;
 int y, x, steps = 0, bound, prev_line = -1, prev_so = 0, prev_eo = 0, first = 1;
 int rn = (int)rnd(4);
 char news[64];
 int i;

 model_make(&m, width);
 make_text(text, &sw);
 if (rnd(3) != 0)
  sw &= ~(unsigned)SW_WORD;
 fd = fd_for(text, sw);
 snprintf(label, sizeof label, "progress: %s %s Find \"%s\" sw %u width %d",
          search_again ? "Search Again" : "Change All", backward ? "backward" : "forward",
          text, sw, width);
 if (rn == 3)
  rn = width;
 if (rn > 60)
  rn = 60;
 for (i = 0; i < rn; i++)
  news[i] = 'Z';
 news[rn] = '\0';
 for (i = 0; i < m.nlines; i++)
  memset(m.line[i].placed, 0, sizeof m.line[i].placed);
 wpe_rowfind_forget_resume();
 if (backward)
 {
  y = m.nrows - 1;
  x = m.row[y].len;
 }
 else
 {
  y = 0;
  x = 0;
 }
 bound = 2 * (model_total_bytes(&m) + m.nlines) + m.nrows + 8;
 for (;;)
 {
  WpeRowHit h;
  int hrow, line, so_off, eo_off, k, nrow, ncol;
  Line *l;

  if (!model_find(&m, fd, backward, y, x, &hrow, &h))
   break;
  if (++steps > bound)
  {
   fail(label);
   printf("   no end within %d steps\n", bound);
   return;
  }
  line = m.row[hrow].line;
  l = &m.line[line];
  so_off = m.row[hrow].start + h.so;
  eo_off = m.row[hrow].start + h.eo;
  if (search_again)
  {
   /* the hits strictly advance along the text */
   if (!first && prev_line == line &&
       (backward ? !(eo_off <= prev_so && (so_off < prev_so || eo_off < prev_eo))
                 : !(so_off >= prev_eo && (so_off > prev_so || eo_off > prev_eo))))
   {
    fail(label);
    printf("   hit %d..%d does not advance from %d..%d\n", so_off, eo_off, prev_so, prev_eo);
    return;
   }
   if (!first && (backward ? line > prev_line : line < prev_line))
   {
    fail(label);
    printf("   hit went to an earlier place\n");
    return;
   }
   first = 0;
   prev_line = line;
   prev_so = so_off;
   prev_eo = eo_off;
   if (backward)
   {
    y = hrow;
    x = h.so;
    model_note(&m, y, x, 1);
   }
   else
   {
    y = hrow;
    x = h.eo;
    model_note(&m, y, x, 0);
   }
   continue;
  }
  /* Change All: the hit lies in old text, an empty hit once per old place */
  for (k = so_off; k < eo_off; k++)
   if (l->orig[k] < 0)
   {
    fail(label);
    printf("   hit %d..%d covers text the replacement made\n", so_off, eo_off);
    return;
   }
  if (so_off == eo_off)
  {
   int place = l->oldlen;

   for (k = so_off; k < l->len; k++)
    if (l->orig[k] >= 0)
    {
     place = l->orig[k];
     break;
    }
   if (l->placed[place])
   {
    fail(label);
    printf("   an empty hit found twice at old place %d of line %d\n", place, line);
    return;
   }
   l->placed[place] = 1;
  }
  if (l->len - (eo_off - so_off) + rn >= LINE_MAX_BYTES)
   break;
  memmove(l->t + so_off + rn, l->t + eo_off, (size_t)(l->len - eo_off));
  memmove(l->orig + so_off + rn, l->orig + eo_off, (size_t)(l->len - eo_off) * sizeof(int));
  memcpy(l->t + so_off, news, (size_t)rn);
  for (k = 0; k < rn; k++)
   l->orig[so_off + k] = -1;
  l->len += rn - (eo_off - so_off);
  model_wrap(&m);
  model_locate(&m, line, backward ? so_off : so_off + rn, &nrow, &ncol);
  y = nrow;
  x = ncol;
  model_note(&m, y, x, backward);
 }
}

static void progress_models(int iterations)
{
 static const int widths[] = { 8, 9, 12, 20 };
 int it;

 for (it = 0; it < iterations; it++)
 {
  int width = widths[rnd(4)];

  checks++;
  model_progress(it % 2, (it / 2) % 2, width);
  if (fails > 20)
   exit(1);
 }
}

/* ---- speed ---- */

static double now_ms(void)
{
 struct timespec ts;

 clock_gettime(CLOCK_MONOTONIC, &ts);
 return(ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6);
}

static int perf_wanted(void)
{
 const char *e = getenv("ROWFIND_NO_PERF");

#if defined(__SANITIZE_ADDRESS__)
 return(0);
#endif
 return(!(e != NULL && e[0] == '1'));
}

/* Backward over a whole row to the last hit, one call per hit. */
static int exhaust_backward(const FIND *fd, const unsigned char *row, int len, int *calls)
{
 int to = len, hits = 0;
 unsigned how = BWD;

 for (;;)
 {
  WpeRowHit h;
  int rc = wpe_rowfind_row(fd, row, len, KIND_BOTH, 0, to, how, &h);

  (*calls)++;
  if (rc != 1)
   return(hits);
  hits++;
  to = h.so;
  how = (h.so == h.eo) ? (BWD | AFT) : BWD;
 }
}

static void perf_rows(void)
{
 unsigned char *row;
 int i, len, calls, hits;
 double t0, ms;
 char msg[200];

 /* the shape the review measured: 20000 bytes, 4000 hits, backward to the start */
 len = 20000;
 row = malloc((size_t)len + 2);
 for (i = 0; i < len; i++)
  row[i] = (i % 5 == 4) ? 'a' : 'b';
 row[len] = '\n';
 row[len + 1] = '\0';
 {
  static const unsigned sws[] = { SW_CASE, SW_CASE | SW_REGEX, 0 };
  static const char *names[] = { "plain", "regular", "plain ICASE" };

  for (i = 0; i < 3; i++)
  {
   calls = 0;
   t0 = now_ms();
   hits = exhaust_backward(fd_for("a", sws[i]), row, len, &calls);
   ms = now_ms() - t0;
   printf("perf [%s]: backward to exhaustion, 20000 bytes, %s: %d hits in %.1f ms\n",
          pass_name, names[i], hits, ms);
   checks++;
   if (hits != 4000)
    fail("perf: wrong number of hits");
   if (perf_wanted() && ms > 50.0)
   {
    snprintf(msg, sizeof msg, "perf: backward %s took %.1f ms, bound 50", names[i], ms);
    fail(msg);
   }
  }
 }
 free(row);
 /* the 2000-byte shape: every byte a hit of `.`, and `a*` on a row of a */
 len = 2000;
 row = malloc((size_t)len + 2);
 memset(row, 'a', (size_t)len);
 row[len] = '\n';
 row[len + 1] = '\0';
 {
  static const char *pats[] = { ".", "a*" };

  for (i = 0; i < 2; i++)
  {
   calls = 0;
   t0 = now_ms();
   hits = exhaust_backward(fd_for(pats[i], SW_CASE | SW_REGEX), row, len, &calls);
   ms = now_ms() - t0;
   printf("perf [%s]: backward, 2000 bytes, `%s`: %d hits in %.1f ms\n", pass_name, pats[i], hits, ms);
   checks++;
   if (perf_wanted() && ms > 500.0)
   {
    snprintf(msg, sizeof msg, "perf: backward `%s` took %.1f ms, bound 500", pats[i], ms);
    fail(msg);
   }
  }
 }
 free(row);
 /* forward: Change All over 100000 rows of 80 bytes */
 {
  unsigned char r80[82];
  int rows = 100000, n = 0;
  static const unsigned sws[] = { SW_CASE, 0 };

  memset(r80, 'x', 80);
  memcpy(r80 + 10, "foo", 3);
  memcpy(r80 + 40, "foo", 3);
  r80[80] = '\n';
  r80[81] = '\0';
  for (i = 0; i < 2; i++)
  {
   int j;

   n = 0;
   t0 = now_ms();
   for (j = 0; j < rows; j++)
   {
    WpeRowHit h;
    int from = 0;

    while (wpe_rowfind_row(fd_for("foo", sws[i]), r80, 80, KIND_BOTH, from, 80, 0, &h) == 1)
    {
     n++;
     from = h.eo;
    }
   }
   ms = now_ms() - t0;
   printf("perf [%s]: forward, 100000 rows of 80 bytes, plain%s: %d hits in %.1f ms\n",
          pass_name, i ? " ICASE" : "", n, ms);
   checks++;
   if (perf_wanted() && ms > 2000.0)
    fail("perf: forward over 100000 rows took more than 2 s");
  }
 }
}

/* ---- the passes ---- */

static void one_pass(const char *locale, int with_perf, int iterations)
{
 pass_name = locale;
 if (setlocale(LC_ALL, locale) == NULL)
 {
  printf("skip: locale %s is not installed\n", locale);
  return;
 }
 wpe_rowfind_release();
 rng_state = 88172645463325252ULL;
 named_rows();
 pinned_rows();
 differential(iterations, 0);
 differential(iterations / 200 + 5, 1);
 progress_models(iterations / 4);
 if (with_perf)
  perf_rows();
 wpe_rowfind_release();
}

int main(void)
{
 const char *e = getenv("ROWFIND_ITERS");
 int iterations = (e != NULL && atoi(e) > 0) ? atoi(e) : 20000;

 one_pass("C", 0, iterations);
 one_pass("C.UTF-8", 1, iterations);
 one_pass("en_US.iso885915", 0, iterations);
 if (!OWNER_SEES_BEFORE && copy_differences == 0)
 {
  printf("FAIL: the copy build shows no look-behind difference (the expected one is untested)\n");
  fails++;
 }
 if (!OWNER_SEES_BEFORE)
  printf("copy path: %d differential rows differ on a word boundary, as expected\n", copy_differences);
 if (byte_engine_quirks > 0)
  printf("engine quirk: %d differential rows skipped, the engine disagrees with itself on a word boundary\n",
         byte_engine_quirks);
 if (fails == 0)
  printf("test_rowfind: %d checks, all green\n", checks);
 else
  printf("test_rowfind: %d FAILED of %d checks\n", fails, checks);
 return(fails == 0 ? 0 : 1);
}
