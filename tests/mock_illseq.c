/* mock_illseq.c -- an engine that refuses invalid UTF-8 as FreeBSD's does.
 *
 * Under a multibyte ctype (MB_CUR_MAX > 1) a pattern, or the part of a row a
 * call looks at, that holds a byte which is not valid UTF-8 is answered with
 * REG_ILLSEQ (the build names it as an error code glibc knows).  Under the C
 * ctype everything goes to glibc, as a byte engine would.  See mock_illseq.h. */
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
  for (k = 1; k <= extra; k++)
   if ((s[i + k] & 0xC0) != 0x80)
    return(0);
  i += extra + 1;
 }
 return(1);
}

int mock_regcomp(regex_t *re, const char *pattern, int cflags)
{
 if (MB_CUR_MAX > 1 && !utf8_valid((const unsigned char *)pattern, strlen(pattern)))
  return(REG_ILLSEQ);
 return(regcomp(re, pattern, cflags));
}

int mock_regexec(const regex_t *re, const char *text, size_t nmatch,
                 regmatch_t pmatch[], int eflags)
{
 if (MB_CUR_MAX > 1 && (eflags & REG_STARTEND) && nmatch > 0 &&
     !utf8_valid((const unsigned char *)text + pmatch[0].rm_so,
                 (size_t)(pmatch[0].rm_eo - pmatch[0].rm_so)))
  return(REG_ILLSEQ);
 return(regexec(re, text, nmatch, pmatch, eflags));
}
