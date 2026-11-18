/* mock_nocontext.c -- an engine that loses the text before a start.
 *
 * With REG_STARTEND, mock_nocontext_regexec gives the C library's engine the
 * text from rm_so on, as a string of its own, and adds rm_so back to every
 * group of the answer: so a \b, \< or \B at the start is judged as if the row
 * began there, the way macOS's regexec does (a \ba from 1 on "xab" matches at
 * 1 there, and a \Ba does not).  Without REG_STARTEND the call is the C
 * library's own.  See mock_nocontext.h. */
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <regex.h>

/* The build force-includes mock_nocontext.h into every source of the program,
   this one too; the calls below are the C library's own. */
#undef regexec

#define MOCK_LIE_FAR 100000   /* how far past the row MOCK_LIE_PAST_RANGE reports */

int mock_nocontext_lie;

int mock_real_regexec(const regex_t *re, const char *text, size_t nmatch,
                      regmatch_t pmatch[], int eflags)
{
 return(regexec(re, text, nmatch, pmatch, eflags));
}

/** shift_groups - Add @by to the columns of every group that took part. */
static void shift_groups(size_t nmatch, regmatch_t pmatch[], regoff_t by)
{
 size_t i;

 for (i = 0; i < nmatch; i++)
 {
  if (pmatch[i].rm_so < 0)
   continue;
  pmatch[i].rm_so += by;
  pmatch[i].rm_eo += by;
 }
}

/** misreport_group - Make group 1 wrong the way mock_nocontext_lie says. */
static void misreport_group(size_t nmatch, regmatch_t pmatch[])
{
 if (nmatch < 2 || pmatch[1].rm_so < 0)
  return;
 if (mock_nocontext_lie == MOCK_LIE_SHORT_END)
  pmatch[1].rm_eo -= 1;
 else if (mock_nocontext_lie == MOCK_LIE_STARTS_AT_0)
  pmatch[1].rm_so = pmatch[0].rm_so;
 else if (mock_nocontext_lie == MOCK_LIE_UNSET)
  pmatch[1].rm_so = pmatch[1].rm_eo = -1;
 else if (mock_nocontext_lie == MOCK_LIE_PAST_RANGE)
 {
  pmatch[0].rm_eo += MOCK_LIE_FAR;
  pmatch[1].rm_eo += MOCK_LIE_FAR;
 }
}

int mock_nocontext_regexec(const regex_t *re, const char *text, size_t nmatch,
                           regmatch_t pmatch[], int eflags)
{
 regoff_t start;
 int rc;

 if (!(eflags & REG_STARTEND) || nmatch == 0)
  return(regexec(re, text, nmatch, pmatch, eflags));
 start = pmatch[0].rm_so;
 pmatch[0].rm_so = 0;
 pmatch[0].rm_eo -= start;
 rc = regexec(re, text + start, nmatch, pmatch, eflags);
 if (rc == 0)
 {
  shift_groups(nmatch, pmatch, start);
  misreport_group(nmatch, pmatch);
 }
 return(rc);
}
