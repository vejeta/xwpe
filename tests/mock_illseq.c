/* mock_illseq.c -- an engine that refuses invalid UTF-8 as FreeBSD's does.
 *
 * Under a multibyte ctype (MB_CUR_MAX > 1) a pattern, or the text a call may
 * read, that holds a byte which is not valid UTF-8 is answered with
 * REG_ILLSEQ (the build names it as an error code glibc knows) and counted in
 * mock_illseq_refusals: the owner is meant never to hand the engine such a
 * byte, so the test fails on a count above zero.  The text a call may read is
 * everything before the end of its range (the BSD engines read the byte
 * before the start of a REG_STARTEND range, lib/61017), or the whole
 * NUL-terminated string when the call has no range.  What counts as valid is
 * as strict as the BSD decoders: no overlong form, no surrogate, nothing
 * above U+10FFFF.  Under the C ctype everything goes to glibc, as a byte
 * engine would.  See mock_illseq.h. */
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <regex.h>

/* The build force-includes mock_illseq.h into every source of the program,
   this one too; the calls below are the C library's own. */
#undef regcomp
#undef regexec

#ifndef REG_ILLSEQ
#error "mock_illseq.h must be force-included (it names REG_ILLSEQ)"
#endif

int mock_illseq_refusals;

/* The second byte of a sequence is limited by its lead byte: E0 takes A0 to
   BF (80 to 9F is an overlong form), ED takes 80 to 9F (A0 to BF is a
   surrogate), F0 takes 90 to BF, F4 takes 80 to 8F (90 and above is past
   U+10FFFF). */
#define SECOND_E0_LOW  0xA0
#define SECOND_ED_HIGH 0x9F
#define SECOND_F0_LOW  0x90
#define SECOND_F4_HIGH 0x8F

/** second_byte_ok - May @b follow the lead byte @lead? */
static int second_byte_ok(unsigned char lead, unsigned char b)
{
 if (lead == 0xE0)
  return(b >= SECOND_E0_LOW);
 if (lead == 0xED)
  return(b <= SECOND_ED_HIGH);
 if (lead == 0xF0)
  return(b >= SECOND_F0_LOW);
 if (lead == 0xF4)
  return(b <= SECOND_F4_HIGH);
 return(1);
}

/** utf8_valid - Is the byte range [s, s + n) well-formed UTF-8? */
static int utf8_valid(const unsigned char *s, size_t n)
{
 size_t i = 0, k, extra;

 while (i < n)
 {
  if (s[i] < 0x80)
   extra = 0;
  else if (s[i] >= 0xC2 && s[i] <= 0xDF)
   extra = 1;
  else if (s[i] >= 0xE0 && s[i] <= 0xEF)
   extra = 2;
  else if (s[i] >= 0xF0 && s[i] <= 0xF4)
   extra = 3;
  else
   return(0);
  if (i + extra >= n && extra != 0)
   return(0);
  if (extra != 0 && !second_byte_ok(s[i], s[i + 1]))
   return(0);
  for (k = 1; k <= extra; k++)
   if ((s[i + k] & 0xC0) != 0x80)
    return(0);
  i += extra + 1;
 }
 return(1);
}

/** refuse - Count one refusal and answer it. */
static int refuse(void)
{
 mock_illseq_refusals++;
 return(REG_ILLSEQ);
}

int mock_regcomp(regex_t *re, const char *pattern, int cflags)
{
 if (MB_CUR_MAX > 1 && !utf8_valid((const unsigned char *)pattern, strlen(pattern)))
  return(refuse());
 return(regcomp(re, pattern, cflags));
}

/** readable_bytes - How many bytes of @text this call may read: up to the end
 * of its range with REG_STARTEND, the whole string without. */
static size_t readable_bytes(const char *text, size_t nmatch, const regmatch_t pmatch[], int eflags)
{
 if ((eflags & REG_STARTEND) && nmatch > 0)
  return((size_t)pmatch[0].rm_eo);
 return(strlen(text));
}

int mock_regexec(const regex_t *re, const char *text, size_t nmatch,
                 regmatch_t pmatch[], int eflags)
{
 if (MB_CUR_MAX > 1 &&
     !utf8_valid((const unsigned char *)text, readable_bytes(text, nmatch, pmatch, eflags)))
  return(refuse());
 return(regexec(re, text, nmatch, pmatch, eflags));
}
