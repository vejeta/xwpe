/* mock_nocontext.h -- force-included into the test_rowfind_nocontext build.
 *
 * Some regexec (macOS's) takes the start of a REG_STARTEND call for the
 * beginning of the string: a \b there is judged as if no text came before it.
 * glibc keeps the text, so the owner's answer for that engine would never run
 * on Linux.  This build puts mock_nocontext_regexec (mock_nocontext.c) between
 * the owner and the C library: it hands the real engine the text from the
 * start of the range and shifts every group of the answer back.  The build
 * also forces the owner's decision to "no start context"
 * (WPE_ROWFIND_NO_START_CONTEXT), so the owner must give the engine the
 * character before each start itself.
 *
 * The test's reference is a regexec too, and the macro below replaces it as
 * well; it calls mock_real_regexec, the C library's own, to get the answer the
 * engine gives reading the row from its first column.
 *
 * mock_nocontext_lie makes the engine report a group wrongly (as an engine
 * with a bug would), so a test can show that the owner takes such an answer
 * for no hit. */
#ifndef MOCK_NOCONTEXT_H
#define MOCK_NOCONTEXT_H

#include <sys/types.h>
#include <regex.h>

/* How the mock misreports group 1 (the owner's hit) after a real answer. */
#define MOCK_LIE_NONE        0
#define MOCK_LIE_SHORT_END   1   /* group 1 ends a byte before the whole match */
#define MOCK_LIE_STARTS_AT_0 2   /* group 1 starts where the whole match does */
#define MOCK_LIE_UNSET       3   /* group 1 is reported as not taking part */
#define MOCK_LIE_PAST_RANGE  4   /* both ends of the match, far past the row */

extern int mock_nocontext_lie;

int mock_nocontext_regexec(const regex_t *re, const char *text, size_t nmatch,
                           regmatch_t pmatch[], int eflags);
int mock_real_regexec(const regex_t *re, const char *text, size_t nmatch,
                      regmatch_t pmatch[], int eflags);

#define regexec mock_nocontext_regexec

#endif
