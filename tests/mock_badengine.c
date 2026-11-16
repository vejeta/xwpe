/* mock_badengine.c -- an engine that crashes and answers outside the row.
 *
 * Crash: under a multibyte ctype (MB_CUR_MAX > 1), a pattern that holds a
 * word-boundary escape (\b \B \< \>) and an unescaped bar, and a range that
 * holds a byte of 0x80 or more, raise SIGSEGV, as NetBSD 10's does.
 *
 * Answers outside the range, for a row whose range holds a marker:
 *   <<FAR>>  a match 2114 bytes past the end (NetBSD's 2119..2121 on 5 bytes);
 *   <<PRE>>  the match [pos - 1, pos): before where the call starts;
 *   <<NEG>>  the match [pos + 2, pos + 1): its end before its start;
 *   <<END>>  the match [pos, end + 1): one byte past the range;
 *   <<MID>>  a match that starts one byte after the first F4 byte of the range
 *            (the lead of a stand-in for a byte that is no UTF-8, four bytes
 *            for one character), so inside that character.
 * Everything else goes to the C library.  See mock_badengine.h. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <regex.h>

/* The build force-includes mock_badengine.h into every source of the
   program, this one too; the calls below are the C library's own. */
#undef regcomp
#undef regexec
#undef regfree

#define MOCK_PATTERNS 64        /* compiled patterns alive at once, owner and reference */
#define MOCK_PATTERN_MAX 600
#define MOCK_FAR_OFFSET 2114
#define MOCK_STAND_IN_LEAD 0xF4   /* the first byte of U+10FE00 to U+10FFFF in UTF-8 */

static struct {
 const regex_t *re;
 char pattern[MOCK_PATTERN_MAX];
} g_table[MOCK_PATTERNS];

/** mock_fail - Say the mock is out of room and end the test: a silent miss
 * would let the crash shape through unnoticed. */
static void mock_fail(const char *what)
{
 printf("FAIL: mock_badengine: %s\n", what);
 fflush(stdout);
 _exit(2);
}

/** table_slot - The slot that keeps the pattern of @re (a free one if none). */
static int table_slot(const regex_t *re)
{
 int i, free_slot = -1;

 for (i = 0; i < MOCK_PATTERNS; i++)
 {
  if (g_table[i].re == re)
   return(i);
  if (g_table[i].re == NULL && free_slot < 0)
   free_slot = i;
 }
 if (free_slot < 0)
  mock_fail("more compiled patterns alive than the table keeps");
 return(free_slot);
}

/** pattern_of - The pattern @re was compiled from, or "" when unknown. */
static const char *pattern_of(const regex_t *re)
{
 int i;

 for (i = 0; i < MOCK_PATTERNS; i++)
  if (g_table[i].re == re)
   return(g_table[i].pattern);
 return("");
}

/** is_crash_pattern - A word-boundary escape together with an unescaped bar. */
static int is_crash_pattern(const char *pat)
{
 int escape = 0, bar = 0;
 const char *p;

 for (p = pat; *p != '\0'; p++)
 {
  if (*p == '\\' && p[1] != '\0')
  {
   if (strchr("bB<>", p[1]) != NULL)
    escape = 1;
   p++;
  }
  else if (*p == '|')
   bar = 1;
 }
 return(escape && bar);
}

/** range_has_high_byte - Is a byte of 0x80 or more in [@s, @s + @n)? */
static int range_has_high_byte(const unsigned char *s, size_t n)
{
 size_t i;

 for (i = 0; i < n; i++)
  if (s[i] >= 0x80)
   return(1);
 return(0);
}

/** range_has - Does [@s, @s + @n) hold the text @what? */
static int range_has(const char *s, size_t n, const char *what)
{
 size_t k = strlen(what), i;

 for (i = 0; i + k <= n; i++)
  if (memcmp(s + i, what, k) == 0)
   return(1);
 return(0);
}

int mock_regcomp(regex_t *re, const char *pattern, int cflags)
{
 int rc = regcomp(re, pattern, cflags);

 if (rc == 0)
 {
  int slot = table_slot(re);

  if (strlen(pattern) >= MOCK_PATTERN_MAX)
   mock_fail("a pattern longer than the table keeps");
  g_table[slot].re = re;
  strcpy(g_table[slot].pattern, pattern);
 }
 return(rc);
}

void mock_regfree(regex_t *re)
{
 int i;

 for (i = 0; i < MOCK_PATTERNS; i++)
  if (g_table[i].re == re)
   g_table[i].re = NULL;
 regfree(re);
}

/** stand_in_lead - Where the first F4 byte (the lead of a stand-in for a byte
 * that is no UTF-8) is in [@s, @s + @n), or -1. */
static long stand_in_lead(const unsigned char *s, size_t n)
{
 size_t i;

 for (i = 0; i < n; i++)
  if (s[i] == MOCK_STAND_IN_LEAD)
   return((long)i);
 return(-1);
}

/** marker_answer - Answer a call outside the range when the row asks for it.
 * @pos: where the call starts; @end: the range's end.
 * Return: 1 the answer was given in @m, 0 no marker. */
static int marker_answer(const char *text, regoff_t pos, regoff_t end, regmatch_t *m)
{
 if (range_has(text + pos, (size_t)(end - pos), "<<MID>>"))
 {
  long lead = stand_in_lead((const unsigned char *)text + pos, (size_t)(end - pos));

  if (lead >= 0)
  {
   m->rm_so = pos + lead + 1;
   m->rm_eo = pos + lead + 2;
   return(1);
  }
 }
 if (range_has(text + pos, (size_t)(end - pos), "<<FAR>>"))
 {
  m->rm_so = end + MOCK_FAR_OFFSET;
  m->rm_eo = end + MOCK_FAR_OFFSET + 2;
  return(1);
 }
 if (range_has(text + pos, (size_t)(end - pos), "<<PRE>>"))
 {
  m->rm_so = pos - 1;
  m->rm_eo = pos;
  return(1);
 }
 if (range_has(text + pos, (size_t)(end - pos), "<<END>>"))
 {
  m->rm_so = pos;
  m->rm_eo = end + 1;
  return(1);
 }
 if (range_has(text + pos, (size_t)(end - pos), "<<NEG>>"))
 {
  m->rm_so = pos + 2;
  m->rm_eo = pos + 1;
  return(1);
 }
 return(0);
}

int mock_regexec(const regex_t *re, const char *text, size_t nmatch,
                 regmatch_t pmatch[], int eflags)
{
 regoff_t pos = 0, end;

 if ((eflags & REG_STARTEND) && nmatch > 0)
 {
  pos = pmatch[0].rm_so;
  end = pmatch[0].rm_eo;
 }
 else
  end = (regoff_t)strlen(text);
 if (nmatch > 0 && marker_answer(text, pos, end, &pmatch[0]))
  return(0);
 if (MB_CUR_MAX > 1 && is_crash_pattern(pattern_of(re)) &&
     range_has_high_byte((const unsigned char *)text + pos, (size_t)(end - pos)))
  raise(SIGSEGV);
 return(regexec(re, text, nmatch, pmatch, eflags));
}
